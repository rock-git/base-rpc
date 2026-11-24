#pragma once

#include <folly/logging/xlog.h>
#include <gtest/gtest.h>

#include "common/net/ib/IBDevice.h"
#include "common/utils/ConfigBase.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::net::test {

class SetupIB : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    static IBConfig config;
    auto ib = IBManager::start(config);
    // base-rpc: not every machine has RDMA; tests that need it call `requireIB()`.
    if (ib.hasError()) {
      XLOGF(WARN, "IBManager start failed, RDMA tests will be skipped: {}", ib.error());
    }
  }

  static bool ibAvailable() { return !IBDevice::all().empty(); }
#define REQUIRE_IB() \
  if (!::hf3fs::net::test::SetupIB::ibAvailable()) GTEST_SKIP() << "no RDMA device"
};

}  // namespace hf3fs::net::test