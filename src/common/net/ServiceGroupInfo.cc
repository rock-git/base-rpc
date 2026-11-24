#include "common/net/ServiceGroupInfo.h"

#include "common/serde/SerdeComparisons.h"

namespace hf3fs::flat {
ServiceGroupInfo::ServiceGroupInfo(std::set<String> ss, std::vector<net::Address> es)
    : services(std::move(ss)),
      endpoints(std::move(es)) {}

bool ServiceGroupInfo::operator==(const ServiceGroupInfo &other) const { return serde::equals(*this, other); }
}  // namespace hf3fs::flat
