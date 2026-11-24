#pragma once

#include <set>
#include <vector>

#include "common/serde/SerdeHelper.h"
#include "common/utils/Address.h"
#include "common/utils/String.h"

// Copied from 3FS common/app/AppInfo.h (only this struct), layout and namespace unchanged for wire compatibility.
namespace hf3fs::flat {

struct ServiceGroupInfo : public serde::SerdeHelper<ServiceGroupInfo> {
  ServiceGroupInfo() = default;
  ServiceGroupInfo(std::set<String> ss, std::vector<net::Address> es);

  bool operator==(const ServiceGroupInfo &other) const;

  SERDE_STRUCT_FIELD(services, std::set<String>{});
  SERDE_STRUCT_FIELD(endpoints, std::vector<net::Address>{});
};

}  // namespace hf3fs::flat
