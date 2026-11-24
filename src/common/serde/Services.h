#pragma once

#ifndef __clang__
#include <vector>
#endif

#include "common/serde/CallContext.h"
#include "common/serde/Echo.h"
#include "common/serde/Service.h"
#include "common/utils/ConstructLog.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"

namespace hf3fs::serde {

class Services {
 public:
  Services() {
#ifndef __clang__
    for (auto &services : services_) {
      services.resize(65536);
    }
#endif
    addService(std::make_unique<echo::ServiceImpl>(), true);
  }

  template <class Service>
  Result<Void> addService(std::unique_ptr<Service> &&obj, bool isRDMA) {
    std::shared_ptr<Service> shared = std::move(obj);
    for (auto i = 0u; i <= uint32_t(isRDMA); ++i) {
      auto &service = services_[i][Service::kServiceID];
      if (UNLIKELY(service.object != nullptr)) {
        return makeError(StatusCode::kInvalidArg, fmt::format("redundant service id: {}", Service::kServiceID));
      }
      service.getter = &MethodExtractor<Service, CallContext, &CallContext::invalidId>::get;
      service.object = shared.get();
      service.alive = std::shared_ptr<void *>(shared, nullptr);
      if constexpr (requires { Service{}.onError(Status::OK); }) {
        service.onError = &CallContext::customOnError<Service, &Service::onError>;
      }
    }
    return Void{};
  }

  CallContext::ServiceWrapper &getServiceById(uint16_t idx, bool isRDMA) { return services_[isRDMA].at(idx); }

 private:
  ConstructLog<"serde::Services"> constructLog_;
#ifdef __clang__
  std::array<CallContext::ServiceWrapper, 65536> services_[2];  // 0 for TCP, 1 for RDMA.
#else
  // 0 for TCP, 1 for RDMA. A vector (65536 each) instead of std::array: gcc fully unrolls the array's
  // default construction into a ~5MB constructor and takes minutes to compile it.
  std::vector<CallContext::ServiceWrapper> services_[2];
#endif
};

}  // namespace hf3fs::serde
