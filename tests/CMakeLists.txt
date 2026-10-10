find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(awc_tests
  test_server.cpp
  error_test.cpp
  url_test.cpp
  config_test.cpp
  logger_test.cpp
  client_test.cpp)

target_link_libraries(awc_tests PRIVATE awc::async_websocket_client GTest::gtest GTest::gmock GTest::gtest_main)
awc_set_warnings(awc_tests)
awc_enable_coverage(awc_tests)

gtest_discover_tests(awc_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
