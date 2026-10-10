# CLAUDE.md

Non-secure (`ws://` only) WebSocket client library: C++20 coroutines on
`boost::asio::io_context`, Boost.Beast for the protocol, spdlog for logging,
`std::expected` for errors, GoogleTest for tests. See README.md for the user-facing docs.

## Commands

Prefer `scripts/check.sh` over raw tool invocations: it prints compact summaries and
keeps full logs in `build/logs/`.

```sh
scripts/check.sh build            # dev preset build; errors only on failure
scripts/check.sh test [FILTER]    # gtest suite, optional --gtest_filter pattern
scripts/check.sh tidy             # clang-tidy (incremental, slow on first run: Beast headers)
scripts/check.sh coverage         # must stay at 100% lines and functions
scripts/check.sh format [--fix]   # clang-format check / apply
scripts/check.sh release          # Release + -Werror build and tests
scripts/check.sh all              # everything above; run before declaring work done
cmake --workflow --preset clang   # Clang build with -Werror and tests (CI runs this too)
```

Build trees live in `build/<preset>` (`dev`, `release`, `clang`, `coverage`, `tidy`).

CI (`.github/workflows/ci.yml`) runs format, GCC test + release, the Clang workflow,
tidy and coverage in `archlinux:latest` containers. Everything CI runs can be reproduced
locally with the commands above. The local `clang-format` on PATH may be an older
version than CI's, but both currently produce identical output.
The test binary is `build/dev/tests/awc_tests`.

## Definition of done

- `scripts/check.sh all` passes: formatted, 0 project clang-tidy warnings, all tests
  green, 100% line and function coverage of `src/` and `include/`, and the Release
  build with warnings as errors compiles and passes the tests.
- New behaviour gets a test in `tests/`; new failure modes get a test that drives them
  through `TestServer`, not a mock.

## Layout

- `include/async_websocket_client/`: public API. `client.hpp` is the facade,
  `error.hpp` holds `ErrorCode`/`Error`/`Result<T>`.
- `src/client.cpp`: all connection logic lives in `detail::ClientImpl`.
- `tests/test_server.{hpp,cpp}`: in-process Beast server whose `Behaviour` enum
  scripts misbehaviour (silent TCP, rejected handshake, drop, close, never read, ignore frames).
- `tests/test_utils.hpp`: `run_coroutine_test`, `wait_until`, `sleep_for`, `CO_ASSERT_*`.

## Conventions

- The standard is set to C++23 only because `std::expected` needs it. Do not use other
  C++23 features casually.
- No exceptions cross the public API. Everything returns `awc::Result<T>`; build
  failures with `fail(ErrorCode, message, boost_error_code)`.
- Inside the library, await with `use_nothrow` (`as_tuple(use_awaitable)`) and turn the
  error code into an `Error`. Do not use the throwing `use_awaitable` form.
- Adding an `ErrorCode` or `ConnectionState` value requires updating its `to_string`
  switch and the test that enumerates all names.
- Naming is enforced by `.clang-tidy`: CamelCase types and enum values, lower_case
  functions and variables, a trailing `_` on private members.
- Format with `scripts/check.sh format --fix`. The style is in `.clang-format`
  (4 spaces, 110 columns).

## Design invariants

- **Single-threaded.** The client is not thread safe. Everything runs on the
  `io_context` thread, so plain `bool`/`int` members are shared between coroutines
  without locks. Never add a second thread calling `io_context::run()` without
  introducing a strand.
- **Lifetime.** `WebSocketClient` is a thin facade over `shared_ptr<ClientImpl>`.
  Spawned coroutines capture `shared_from_this()`, and `~WebSocketClient` calls
  `abort()`, which drops the user callbacks. Keep both properties when adding coroutines.
- **One write at a time.** Beast allows a single outstanding write, so `send()`
  serialises through the `write_lock_` channel. Pings go through `async_ping`, which
  Beast already coordinates with writes.
- **Session teardown.** The read loop and ping loop of a session each report an
  `Error`. The first one reported is the disconnect cause, and teardown closes the
  TCP socket to abort the other loop.
- **Back-pressure.** When the receive queue is full the reader blocks, so pongs are
  not processed. The ping loop skips the pong timeout while `reader_blocked_` is set.
  Do not "fix" this into a false PongTimeout.
- `send()` never queues while disconnected; it fails with `NotConnected` by design.

## Testing gotchas

- gtest `ASSERT_*` contains a plain `return`, which does not compile inside a
  coroutine. Use `CO_ASSERT_TRUE` / `CO_ASSERT_FALSE` from `test_utils.hpp` in
  coroutine bodies. `EXPECT_*` works everywhere.
- Never assert timing with fixed sleeps alone. Poll with `co_await wait_until(...)`.
  Tests must stay stable under `--gtest_repeat=15 --gtest_shuffle`.
- Each `ClientTest` sets the library log level to `off`. To debug a test, call
  `awc::log::set_level(spdlog::level::trace)` temporarily.
- A server that must ignore pings and close frames *and* notice the client leaving
  should use `Behaviour::IgnoreFrames`. `NeverRead` never reads at all, so the server
  never sees a disconnect.
- The state handler only fires on real transitions. Staying in `Reconnecting`
  across attempts is reported once.

## Tooling gotchas

- Coverage exclusions (`LCOV_EXCL_*`) are used only for gcov artefacts on closing
  braces and for defensive catch blocks that cannot be triggered. Each one carries a
  comment saying why. Do not use them to hide untested logic.
- The clang static analyzer reports a few `core.NullDereference` /
  `core.CallAndMessage` diagnostics inside `boost/asio/impl/awaitable.hpp`. They are
  known false positives, and `check.sh tidy` filters out `/usr/` paths.
- GCC's `-Wnull-dereference` gives false positives inside inlined Boost.Asio code in
  Release builds, so it is enabled for Clang only (`cmake/CompilerWarnings.cmake`).
- `tests/.clang-tidy` and `examples/.clang-tidy` relax a few guideline checks that
  conflict with gtest fixtures and coroutine lambdas. Keep the library itself under
  the full root config.
