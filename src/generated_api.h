// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef VPP_JSON_GATEWAY_SRC_GENERATED_API_H_
#define VPP_JSON_GATEWAY_SRC_GENERATED_API_H_

#include <vapi/vapi.h>

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "src/schema.h"

namespace vpp_json {
struct GeneratedCall {
  bool done = false;
  int32_t retval = 0;
  vapi_error_e error = VAPI_OK;
  std::vector<uint8_t> reply;
  std::function<void(std::span<const uint8_t>)> item;
};
struct GeneratedBinding {
  const char* name;
  bool (*available)(vapi_ctx_t);
  vapi_error_e (*send)(vapi_ctx_t, std::span<const uint8_t>, GeneratedCall*);
};
const std::vector<GeneratedBinding>& GeneratedBindings();
Schema GeneratedSchema();
}  // namespace vpp_json
#endif  // VPP_JSON_GATEWAY_SRC_GENERATED_API_H_
