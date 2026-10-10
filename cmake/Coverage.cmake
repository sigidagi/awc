# Code coverage support (GCC gcov / Clang gcov-compatible) with an lcov based
# `coverage` target producing an HTML report in <build>/coverage/index.html.
function(awc_enable_coverage target)
  if(NOT AWC_ENABLE_COVERAGE)
    return()
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(WARNING "Coverage is only supported with GCC or Clang")
    return()
  endif()
  target_compile_options(${target} PRIVATE --coverage -O0 -g -fno-inline)
  target_link_options(${target} PRIVATE --coverage)
endfunction()

if(AWC_ENABLE_COVERAGE)
  find_program(AWC_LCOV_EXE NAMES lcov)
  find_program(AWC_GENHTML_EXE NAMES genhtml)
  if(AWC_LCOV_EXE AND AWC_GENHTML_EXE)
    set(_cov_dir "${CMAKE_BINARY_DIR}/coverage")
    add_custom_target(coverage
      COMMAND ${AWC_LCOV_EXE} --directory ${CMAKE_BINARY_DIR} --zerocounters -q
      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
      COMMAND ${AWC_LCOV_EXE} --directory ${CMAKE_BINARY_DIR} --capture
              --output-file ${_cov_dir}.info --ignore-errors mismatch,inconsistent,unused -q
      COMMAND ${AWC_LCOV_EXE} --extract ${_cov_dir}.info
              "${CMAKE_SOURCE_DIR}/src/*" "${CMAKE_SOURCE_DIR}/include/*"
              --output-file ${_cov_dir}.filtered.info --ignore-errors unused -q
      COMMAND ${AWC_GENHTML_EXE} ${_cov_dir}.filtered.info --output-directory ${_cov_dir} -q
      COMMAND ${AWC_LCOV_EXE} --summary ${_cov_dir}.filtered.info
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Running tests and generating coverage report in ${_cov_dir}/index.html"
      VERBATIM)

  else()
    message(WARNING "AWC_ENABLE_COVERAGE=ON but lcov/genhtml not found; the `coverage` target is unavailable")
  endif()
endif()
