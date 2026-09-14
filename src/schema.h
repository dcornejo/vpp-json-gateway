// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef VPP_JSON_GATEWAY_SRC_SCHEMA_H_
#define VPP_JSON_GATEWAY_SRC_SCHEMA_H_

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "src/common.h"

namespace vpp_json {
struct Symbols {
  std::function<Status(const std::string&, uint32_t*)> interface_index;
  std::function<Status(uint32_t, std::string*)> interface_name;
};
// A normalized catalog compiled from VPP's .api.json descriptions. Native
// payloads are passed to generated VAPI bindings, which own endian conversion.
class Schema {
 public:
  Status AddModule(const Json& document, const std::string& module);
  Status Load(const Json& catalog);
  const Json& catalog() const { return catalog_; }
  const Json* Type(const std::string& name) const;
  int64_t FixedSize(const std::string& name, int depth = 0) const;
  bool Variable(const std::string& name, int depth = 0) const;
  bool HasInterface(const std::string& name, int depth = 0) const;
  Json Describe(const std::string& name, int depth = 0) const;
  Status Encode(const std::string& type, const Json& value,
                const Symbols& symbols, std::vector<uint8_t>* output) const;
  Status Decode(const std::string& type, std::span<const uint8_t> bytes,
                const Symbols& symbols, Json* output) const;

 private:
  Status Convert(const std::string& type, Json* value, const Symbols& symbols,
                 std::vector<uint8_t>* encoded,
                 std::span<const uint8_t>* decoded, const Json* tag,
                 int depth) const;
  Json catalog_ = {{"types", Json::object()}, {"methods", Json::object()}};
};
}  // namespace vpp_json
#endif  // VPP_JSON_GATEWAY_SRC_SCHEMA_H_
