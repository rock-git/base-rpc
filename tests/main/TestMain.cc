// Simplified from 3FS tests/main/TestMain.cc: no 3FS log handlers / monitor.
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>

#include "common/net/ib/IBDevice.h"
#include "common/utils/SysResource.h"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  hf3fs::SysResource::increaseProcessFDLimit(524288);
  int exitCode = RUN_ALL_TESTS();
  hf3fs::net::IBManager::stop();
  return exitCode;
}
