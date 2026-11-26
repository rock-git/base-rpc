add_executable(echo echo.cc)
target_link_libraries(echo PRIVATE base-rpc)
set_target_properties(echo PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")

add_executable(bench bench.cc)
target_link_libraries(bench PRIVATE base-rpc)
set_target_properties(bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
