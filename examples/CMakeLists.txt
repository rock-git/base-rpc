add_executable(echo echo.cc)
target_link_libraries(echo PRIVATE base-rpc)
set_target_properties(echo PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
