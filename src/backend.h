// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef VPP_JSON_GATEWAY_SRC_BACKEND_H_
#define VPP_JSON_GATEWAY_SRC_BACKEND_H_

#include <functional>
#include <memory>
#include <string>

#include "src/common.h"

namespace vpp_json {
using ItemSink = std::function<bool(const Json&)>;
// Created, called and destroyed exclusively by the backend worker thread.
class Backend {
 public:
  virtual ~Backend() = default;
  virtual void Poll() {}
  virtual Status Generic(const Json& request, Spool* spool);
  virtual Status List(const ItemSink& sink) = 0;
  virtual Status SetState(const std::string& name, bool up) = 0;
};
std::unique_ptr<Backend> MakeMockBackend(int count);
#ifdef WITH_VAPI
std::unique_ptr<Backend> MakeVapiBackend(const std::string& socket);
#endif
Status RunBackend(Backend* backend, const Json& request, Spool* spool);
}  // namespace vpp_json
#endif  // VPP_JSON_GATEWAY_SRC_BACKEND_H_
