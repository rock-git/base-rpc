#pragma once
// base-rpc shim: every recorder is a no-op. Same names/signatures as 3FS's common/monitor/Recorder.h so the
// copied net/serde/utils sources compile unchanged.
// shortcut: metrics are dropped; put a real implementation behind these classes if you need metrics.
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/monitor/Sample.h"
#include "common/utils/StatusCode.h"

namespace hf3fs::monitor {

class Recorder {
 public:
  explicit Recorder(std::string_view name = {}, std::optional<TagSet> = std::nullopt) : name_(name) {}
  const std::string &name() { return name_; }

  // Stands in for the per-tag recorder handle; every tag maps to one shared dummy instance.
  template <typename T>
  class TagRef {
   public:
    T *operator->() const {
      static T dummy{std::string_view{}};
      return &dummy;
    }
  };

 private:
  std::string name_;
};

class CountRecorder : public Recorder {
 public:
  explicit CountRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt, bool = true)
      : Recorder(name, std::move(tag)) {}
  void addSample(int64_t) {}
  void addSample(int64_t, const TagSet &) {}
  TagRef<CountRecorder> getRecorderWithTag(const TagSet &) { return {}; }
};

class DistributionRecorder : public Recorder {
 public:
  explicit DistributionRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt)
      : Recorder(name, std::move(tag)) {}
  void addSample(double) {}
  void addSample(double, const TagSet &) {}
  TagRef<DistributionRecorder> getRecorderWithTag(const TagSet &) { return {}; }
};

class LatencyRecorder : public Recorder {
 public:
  explicit LatencyRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt)
      : Recorder(name, std::move(tag)) {}
  void addSample(std::chrono::nanoseconds) {}
  void addSample(std::chrono::nanoseconds, const TagSet &) {}
  TagRef<LatencyRecorder> getRecorderWithTag(const TagSet &) { return {}; }
};

class ValueRecorder : public Recorder {
 public:
  explicit ValueRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt, bool = true)
      : Recorder(name, std::move(tag)) {}
  void set(int64_t) {}
  void set(int64_t, const TagSet &) {}
  int64_t value() { return 0; }
  TagRef<ValueRecorder> getRecorderWithTag(const TagSet &) { return {}; }
};

class OperationRecorder : public Recorder {
 public:
  explicit OperationRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt, bool = false)
      : Recorder(name, std::move(tag)) {}
  struct Guard {
    void reportWithCode(status_code_t) {}
    void report(bool) {}
    void succ() {}
    void dismiss() {}
  };
  Guard record() { return {}; }
  Guard record(const TagSet &) { return {}; }
};

}  // namespace hf3fs::monitor
