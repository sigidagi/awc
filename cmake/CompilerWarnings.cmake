# Applies a strict warning set to a target.
function(awc_set_warnings target)
  set(gcc_clang_warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough)

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE ${gcc_clang_warnings})
    # GCC's -Wnull-dereference reports false positives inside inlined Boost.Asio code
    # at -O2/-O3 (it ignores the system-header suppression there); Clang's is reliable.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target} PRIVATE -Wnull-dereference)
    endif()
    if(AWC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  elseif(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(AWC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  endif()
endfunction()
