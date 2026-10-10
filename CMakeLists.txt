cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW) # use BoostConfig.cmake shipped with Boost
endif()

project(
  async_websocket_client
  VERSION 1.0.0
  DESCRIPTION "Coroutine-based (C++20) non-secure WebSocket client on top of Boost.Asio / Boost.Beast"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(AWC_BUILD_TESTS "Build unit tests" ON)
option(AWC_BUILD_EXAMPLES "Build examples" ON)
option(AWC_ENABLE_COVERAGE "Instrument the library and tests for code coverage (GCC/Clang)" OFF)
option(AWC_ENABLE_CLANG_TIDY "Run clang-tidy on every compiled translation unit" OFF)
option(AWC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
# The library uses C++20 coroutines and std::expected (C++23 library feature).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # consumed by clang-tidy / IDEs

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(StaticAnalysis)
include(Coverage)
include(Formatting)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(Boost 1.84 REQUIRED COMPONENTS system url)
find_package(spdlog 1.9 REQUIRED)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(async_websocket_client
  src/error.cpp
  src/url.cpp
  src/config.cpp
  src/logger.cpp
  src/client.cpp)
add_library(awc::async_websocket_client ALIAS async_websocket_client)

target_include_directories(async_websocket_client
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(async_websocket_client
  PUBLIC Boost::system Boost::url spdlog::spdlog Threads::Threads)

target_compile_definitions(async_websocket_client
  PUBLIC BOOST_ASIO_NO_DEPRECATED BOOST_ASIO_DISABLE_BOOST_ARRAY)

awc_set_warnings(async_websocket_client)
awc_enable_coverage(async_websocket_client)

# ---------------------------------------------------------------------------
# Tests / examples
# ---------------------------------------------------------------------------
if(AWC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  if(TARGET coverage)
    add_dependencies(coverage awc_tests) # always measure an up-to-date binary
  endif()
endif()

if(AWC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS async_websocket_client EXPORT async_websocket_clientTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT async_websocket_clientTargets
  NAMESPACE awc::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/async_websocket_client)
