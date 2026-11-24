#pragma once
// base-rpc shim: metrics are not part of base-rpc, TagSet only keeps the call sites compiling.
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace hf3fs::monitor {

class TagSet {
 public:
  using Map = std::map<std::string, std::string>;
  TagSet() = default;
  TagSet(std::initializer_list<Map::value_type> init) : tags_(init) {}

  static TagSet create(std::string name, std::string value) {
    TagSet ts;
    ts.addTag(std::move(name), std::move(value));
    return ts;
  }
  TagSet newTagSet(std::string name, std::string value) const {
    TagSet ts(*this);
    ts.addTag(std::move(name), std::move(value));
    return ts;
  }
  void addTag(std::string name, std::string value) { tags_[std::move(name)] = std::move(value); }
  const Map &asMap() const { return tags_; }

 private:
  Map tags_;
};

inline TagSet instanceTagSet(std::string instance) { return TagSet::create("instance", std::move(instance)); }
inline TagSet threadTagSet(const std::string_view &threadName) {
  return TagSet::create("thread", std::string{threadName});
}

}  // namespace hf3fs::monitor
