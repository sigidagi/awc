# `format` reformats all project sources in place with clang-format,
# `format-check` fails if any file is not formatted (useful for CI).
find_program(AWC_CLANG_FORMAT_EXE NAMES clang-format)
if(AWC_CLANG_FORMAT_EXE)
  file(GLOB_RECURSE AWC_FORMAT_SOURCES
       CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/include/*.hpp"
       "${CMAKE_SOURCE_DIR}/src/*.cpp"
       "${CMAKE_SOURCE_DIR}/src/*.hpp"
       "${CMAKE_SOURCE_DIR}/tests/*.cpp"
       "${CMAKE_SOURCE_DIR}/tests/*.hpp"
       "${CMAKE_SOURCE_DIR}/examples/*.cpp")
  add_custom_target(format
    COMMAND ${AWC_CLANG_FORMAT_EXE} -i ${AWC_FORMAT_SOURCES}
    COMMENT "Formatting sources with clang-format"
    VERBATIM)
  add_custom_target(format-check
    COMMAND ${AWC_CLANG_FORMAT_EXE} --dry-run --Werror ${AWC_FORMAT_SOURCES}
    COMMENT "Checking source formatting with clang-format"
    VERBATIM)
endif()
