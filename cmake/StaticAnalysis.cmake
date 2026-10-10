# Enables clang-tidy for every target built in this project when AWC_ENABLE_CLANG_TIDY=ON.
# The checks are configured in the top-level .clang-tidy file.
if(AWC_ENABLE_CLANG_TIDY)
  find_program(AWC_CLANG_TIDY_EXE NAMES clang-tidy)
  if(AWC_CLANG_TIDY_EXE)
    message(STATUS "clang-tidy enabled: ${AWC_CLANG_TIDY_EXE}")
    set(CMAKE_CXX_CLANG_TIDY "${AWC_CLANG_TIDY_EXE};--use-color")
  else()
    message(WARNING "AWC_ENABLE_CLANG_TIDY=ON but clang-tidy was not found")
  endif()
endif()
