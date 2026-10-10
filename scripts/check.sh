#!/usr/bin/env bash
# Runs a project check and prints a short, structured summary instead of raw tool output.
# Full logs are kept in build/logs/ for when the summary is not enough.
#
# Usage: scripts/check.sh <command> [options]
#   build              configure + build the dev preset; on failure show only the errors
#   test [FILTER]      build + run the gtest suite (optional --gtest_filter pattern)
#   asan [FILTER]      the gtest suite built with AddressSanitizer + UndefinedBehaviorSanitizer
#   tidy               clang-tidy build (incremental); warnings grouped by check
#   coverage           coverage build + lcov; totals and every uncovered line
#   format [--fix]     check formatting (or reformat in place with --fix)
#   release            Release build with warnings as errors, then the tests
#   all                format, build, test, asan, tidy, coverage, release (stops at the first failure)
#
# Exit status is non-zero when the check fails.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="build/logs"  # relative to ROOT, the script always runs from there
MAX_LINES="${MAX_LINES:-25}"  # cap on listed errors / warnings / uncovered lines
mkdir -p "$LOG_DIR"
cd "$ROOT" || exit 2

if [[ -t 1 ]]; then
    RED=$'\e[31m' GREEN=$'\e[32m' YELLOW=$'\e[33m' BOLD=$'\e[1m' RESET=$'\e[0m'
else
    RED='' GREEN='' YELLOW='' BOLD='' RESET=''
fi

ok() { echo "${GREEN}PASS${RESET} $*"; }
bad() { echo "${RED}FAIL${RESET} $*"; }
note() { echo "${YELLOW}WARN${RESET} $*"; }
section() { echo "${BOLD}== $* ==${RESET}"; }

# Lists at most MAX_LINES lines of stdin, then says how many were left out.
capped() {
    awk -v max="$MAX_LINES" 'NR <= max { print "  " $0 } END { if (NR > max) printf "  ... and %d more\n", NR - max }'
}

# configure <preset> <log>: configures the preset only when it has not been configured yet.
configure() {
    local preset="$1" log="$2"
    if [[ -f "build/$preset/build.ninja" ]]; then
        return 0
    fi
    if ! cmake --preset "$preset" >"$log" 2>&1; then
        bad "configure ($preset), see $log"
        grep -E "CMake Error|Could not find|error" "$log" | capped
        return 1
    fi
}

# Prints compiler errors (or warnings) from a build log, project files first, deduplicated.
compiler_diagnostics() {
    local kind="$1" log="$2"
    grep -E "^[^ ]+:[0-9]+:[0-9]+: (fatal )?$kind:" "$log" | grep -v "^/usr/" | sed "s|$ROOT/||" | sort -u
}

cmd_build() {
    section "build (dev)"
    local log="$LOG_DIR/build.log"
    configure dev "$log" || return 1
    if ! cmake --build --preset dev >"$log" 2>&1; then
        bad "build, $(grep -c '^FAILED:' "$log") target(s) failed, see $log"
        local errors
        errors="$(compiler_diagnostics error "$log")"
        if [[ -n "$errors" ]]; then
            capped <<<"$errors"
        else
            grep -E "error|undefined reference" "$log" | capped
        fi
        return 1
    fi
    local warnings
    warnings="$(compiler_diagnostics warning "$log")"
    if [[ -n "$warnings" ]]; then
        note "build succeeded with $(wc -l <<<"$warnings") warning(s)"
        capped <<<"$warnings"
    else
        ok "build"
    fi
}

# run_gtest <binary> <log> <filter>: runs a gtest binary and summarises the result.
run_gtest() {
    local binary="$1" log="$2" filter="$3"
    "$binary" --gtest_brief=1 --gtest_color=no --gtest_filter="$filter" >"$log" 2>&1
    local status=$?
    local total passed
    total="$(sed -nE 's/^\[==========\] ([0-9]+) tests? from.*ran.*/\1/p' "$log")"
    passed="$(sed -nE 's/^\[  PASSED  \] ([0-9]+) tests?\./\1/p' "$log")"
    if [[ $status -eq 0 && -n "$total" ]]; then
        ok "$passed/$total tests"
        return 0
    fi
    if [[ -z "$total" ]]; then
        bad "test binary crashed or did not finish (exit $status), see $log"
        # A sanitizer report names the culprit better than the last lines of output.
        if grep -qE "^SUMMARY: (Address|UndefinedBehavior)Sanitizer" "$log"; then
            grep -E "^\[ RUN      \]" "$log" | tail -n 1 | sed 's/^\[ RUN      \] /in test /' | capped
            grep -E "^SUMMARY: |ERROR: AddressSanitizer|runtime error" "$log" | sed "s|$ROOT/||" | capped
        else
            tail -n 15 "$log" | capped
        fi
        return 1
    fi
    local failed
    failed="$(grep -cE '^\[  FAILED  \] [A-Za-z_]' "$log")"
    bad "${passed:-0}/$total passed, $failed failed, see $log"
    # For every failed test: its name, then up to 3 failed assertions with their messages
    # (gtest prints "file:line: Failure" followed by message lines up to a blank line).
    awk -v root="$ROOT/" '
        /: Failure$/ {
            loc = $0; sub(root, "", loc); sub(/: Failure$/, "", loc)
            text = ""; lines = 0
            while ((getline line) > 0 && line != "") {
                if (++lines <= 6) text = text "\n        " line
            }
            if (lines > 6) text = text "\n        ..."
            failure[++n] = loc text
            next
        }
        /^\[  FAILED  \] [A-Za-z_].*\(/ {
            name = $0; sub(/^\[  FAILED  \] /, "", name); sub(/ \(.*/, "", name)
            printf "  %s\n", name
            for (i = 1; i <= n && i <= 3; i++) printf "      %s\n", failure[i]
            if (n > 3) printf "      ... and %d more failed assertions\n", n - 3
            n = 0
        }' "$log"
    return 1
}

cmd_test() {
    local filter="${1:-*}"
    cmd_build || return 1
    section "test"
    run_gtest ./build/dev/tests/awc_tests "$LOG_DIR/test.log" "$filter"
}

cmd_asan() {
    local filter="${1:-*}"
    section "asan (AddressSanitizer + UndefinedBehaviorSanitizer)"
    local log="$LOG_DIR/asan-build.log"
    configure asan "$log" || return 1
    if ! cmake --build --preset asan >"$log" 2>&1; then
        bad "asan build, see $log"
        compiler_diagnostics error "$log" | capped
        return 1
    fi
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" UBSAN_OPTIONS="print_stacktrace=1" \
        run_gtest ./build/asan/tests/awc_tests "$LOG_DIR/asan.log" "$filter"
}

cmd_tidy() {
    section "clang-tidy"
    local log="$LOG_DIR/tidy.log"
    configure tidy "$log" || return 1
    echo "  running (only changed translation units are re-checked)..."
    if ! cmake --build --preset tidy >"$log" 2>&1; then
        bad "tidy build failed, see $log"
        compiler_diagnostics error "$log" | capped
        return 1
    fi
    # "file:line:col: warning: text [check-name]" from project files only.
    local warnings
    warnings="$(grep -E '^[^ ]+:[0-9]+:[0-9]+: warning: .*\[[a-z0-9.,-]+\]$' "$log" | grep -v '^/usr/' |
        sed "s|$ROOT/||" | sort -u)"
    local ignored
    ignored="$(grep -cE '^/usr/[^ ]+:[0-9]+:[0-9]+: warning:' "$log")"
    if [[ -z "$warnings" ]]; then
        local suffix=""
        if [[ "$ignored" -gt 0 ]]; then
            suffix=" ($ignored in system headers ignored)"
        fi
        ok "no clang-tidy warnings in project code$suffix"
        return 0
    fi
    note "$(wc -l <<<"$warnings") clang-tidy warning(s), see $log"
    echo "  by check:"
    sed -E 's/.*\[([a-z0-9.,-]+)\]$/\1/' <<<"$warnings" | sort | uniq -c | sort -rn | capped
    echo "  locations:"
    sed -E 's/^([^ ]+:[0-9]+):[0-9]+: warning: (.*) \[([a-z0-9.,-]+)\]$/\1  \3: \2/' <<<"$warnings" | capped
    return 1
}

cmd_coverage() {
    section "coverage"
    local log="$LOG_DIR/coverage.log"
    configure coverage "$log" || return 1
    if ! cmake --build --preset coverage >"$log" 2>&1; then
        bad "coverage run failed (build or tests), see $log"
        compiler_diagnostics error "$log" | capped
        grep -E "tests failed|\(Failed\)" "$log" | capped
        return 1
    fi
    local info="build/coverage/coverage.filtered.info"
    local lines functions
    lines="$(grep -m1 -E '^ +lines' "$log" | sed -E 's/^ +lines\.+: //')"
    functions="$(grep -m1 -E '^ +functions' "$log" | sed -E 's/^ +functions\.+: //')"
    local uncovered
    uncovered="$(awk -v root="$ROOT/" '
        /^SF:/ { file = substr($0, 4); sub(root, "", file); next }
        /^DA:/ { split(substr($0, 4), f, ","); if (f[2] == "0") print file ":" f[1] }' "$info")"
    if [[ -z "$uncovered" ]]; then
        ok "lines $lines, functions $functions"
        echo "  report: build/coverage/coverage/index.html"
        return 0
    fi
    note "lines $lines, functions $functions"
    echo "  uncovered lines:"
    while IFS=: read -r file line; do
        printf '%s:%s  %s\n' "$file" "$line" "$(sed -n "${line}p" "$file" | sed -E 's/^ +//')"
    done <<<"$uncovered" | capped
    echo "  report: build/coverage/coverage/index.html"
    return 1
}

cmd_format() {
    section "format"
    local files
    mapfile -t files < <(find include src tests examples -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)
    if [[ "${1:-}" == "--fix" ]]; then
        clang-format -i "${files[@]}"
        ok "reformatted ${#files[@]} files"
        return 0
    fi
    local offenders=()
    for file in "${files[@]}"; do
        if ! clang-format --dry-run --Werror "$file" >/dev/null 2>&1; then
            offenders+=("$file")
        fi
    done
    if [[ ${#offenders[@]} -eq 0 ]]; then
        ok "${#files[@]} files formatted"
        return 0
    fi
    bad "${#offenders[@]} file(s) need formatting (run: scripts/check.sh format --fix)"
    printf '%s\n' "${offenders[@]}" | capped
    return 1
}

cmd_release() {
    section "release (warnings as errors)"
    local log="$LOG_DIR/release.log"
    configure release "$log" || return 1
    if ! cmake --build --preset release >"$log" 2>&1; then
        bad "release build, $(grep -c '^FAILED:' "$log") target(s) failed, see $log"
        # With -Werror the culprit may sit in a system header (inlined code), so fall
        # back to all diagnostics when no project file is named.
        local errors
        errors="$(compiler_diagnostics error "$log")"
        if [[ -z "$errors" ]]; then
            errors="$(grep -E '^[^ ]+:[0-9]+:[0-9]+: (fatal )?error:' "$log" | sort -u)"
        fi
        capped <<<"${errors:-$(tail -n 15 "$log")}"
        return 1
    fi
    if ! ctest --preset release >>"$log" 2>&1; then
        bad "release tests, see $log"
        grep -E '\((Failed|Timeout|SEGFAULT|Subprocess aborted|Exception)\)|tests passed' "$log" | capped
        return 1
    fi
    ok "release build and $(grep -oE 'out of [0-9]+' "$log" | tail -n 1 | sed 's/out of //') tests"
}

cmd_all() {
    cmd_format && cmd_test && cmd_asan && cmd_tidy && cmd_coverage && cmd_release
}

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-}" in
    build) cmd_build ;;
    test) cmd_test "${2:-*}" ;;
    asan) cmd_asan "${2:-*}" ;;
    tidy) cmd_tidy ;;
    coverage) cmd_coverage ;;
    format) cmd_format "${2:-}" ;;
    release) cmd_release ;;
    all) cmd_all ;;
    -h | --help | help | "") usage ;;
    *)
        usage
        exit 2
        ;;
esac
