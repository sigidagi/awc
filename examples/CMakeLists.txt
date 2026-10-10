# Small, self-contained examples. Each one is a single translation unit.
function(awc_add_example name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE awc::async_websocket_client)
  awc_set_warnings(${name})
endfunction()

awc_add_example(echo_client)
awc_add_example(reconnecting_client)
awc_add_example(echo_server)
