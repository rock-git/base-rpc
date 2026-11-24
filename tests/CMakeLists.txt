add_library(test_main STATIC main/TestMain.cc)
target_link_libraries(test_main PUBLIC base-rpc gtest gmock)

# `tests/GtestHelpers.h` is included as "tests/GtestHelpers.h"
function(base_rpc_add_test NAME)
  add_executable(${NAME} ${ARGN})
  target_link_libraries(${NAME} PRIVATE test_main)
  target_include_directories(${NAME} PRIVATE ${PROJECT_SOURCE_DIR})
  set_target_properties(${NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endfunction()

file(GLOB NET_TESTS CONFIGURE_DEPENDS common/net/*.cc)
file(GLOB SERDE_TESTS CONFIGURE_DEPENDS common/serde/*.cc)
file(GLOB IB_TESTS CONFIGURE_DEPENDS common/net/ib/*.cc)

base_rpc_add_test(test_base_rpc ${NET_TESTS} ${SERDE_TESTS})
add_test(NAME test_base_rpc COMMAND test_base_rpc)

# Needs an RDMA NIC; only registered with -DBASE_RPC_RUN_IB_TESTS=ON.
base_rpc_add_test(test_base_rpc_ib ${IB_TESTS})
option(BASE_RPC_RUN_IB_TESTS "Register the RDMA tests with ctest" OFF)
if(BASE_RPC_RUN_IB_TESTS)
  add_test(NAME test_base_rpc_ib COMMAND test_base_rpc_ib)
endif()
