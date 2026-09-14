// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <vapi/interface.api.vapi.h>
#include <vapi/vapi.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "src/backend.h"
#include "src/generated_api.h"

namespace vpp_json {
namespace {
// VPP 26.06 generated C bindings handle IDs, endian conversion and dump pings.
class VapiBackend final : public Backend {
 public:
  explicit VapiBackend(std::string socket) : socket_(std::move(socket)) {}
  ~VapiBackend() override { Reset(); }
  void Poll() override {
    if (!context_) {
      return;
    }
    vapi_error_e rc = vapi_dispatch_one(context_);
    if (rc != VAPI_OK && rc != VAPI_EAGAIN) {
      Reset();
    }
  }
  Status List(const ItemSink& sink) override {
    Status status = Connect();
    if (!status.ok()) {
      return status;
    }
    if (!vapi_is_msg_available(context_, vapi_msg_id_sw_interface_dump)) {
      return {"unsupported_operation",
              "VPP interface dump schema is unavailable"};
    }
    DumpState state;
    state.sink = &sink;
    return Dump(&state);
  }
  Status SetState(const std::string& name, bool up) override {
    Status status = Connect();
    if (!status.ok()) {
      return status;
    }
    if (!vapi_is_msg_available(context_, vapi_msg_id_sw_interface_dump) ||
        !vapi_is_msg_available(context_, vapi_msg_id_sw_interface_set_flags)) {
      return {"unsupported_operation", "VPP interface schemas are unavailable"};
    }
    // Resolve afresh; never reuse an index across operations or reconnections.
    DumpState state;
    state.name = name;
    status = Dump(&state);
    if (!status.ok()) {
      return status;
    }
    if (state.matches == 0) {
      return {"resource_not_found", "Interface name did not resolve"};
    }
    if (state.matches != 1) {
      return {"ambiguous_resource", "Interface name is not unique"};
    }
    auto* message = vapi_alloc_sw_interface_set_flags(context_);
    if (!message) {
      return {"backend_unavailable", "Cannot allocate VAPI request"};
    }
    message->payload.sw_if_index = state.index;
    message->payload.flags = static_cast<vapi_enum_if_status_flags>(
        up ? IF_STATUS_API_FLAG_ADMIN_UP : 0);
    ReplyState reply;
    vapi_error_e rc =
        vapi_sw_interface_set_flags(context_, message, OnSet, &reply);
    if (rc != VAPI_OK) {
      vapi_msg_free(context_, message);
      Reset();
      return {"outcome_unknown", "VAPI send failed; reconcile interface state"};
    }
    status = Dispatch(&reply.done, true);
    if (!status.ok()) {
      return status;
    }
    if (reply.error != VAPI_OK) {
      return {"outcome_unknown",
              "VAPI reply failed; reconcile interface state"};
    }
    if (reply.retval != 0) {
      return {"vpp_rejected", "VPP rejected the interface state change"};
    }
    return {};
  }

  Status Generic(const Json& request, Spool* spool) override {
    const std::string id = request["id"], method = request["method"];
    const Json params = request.value("params", Json::object());
    auto fail = [&](const Status& status) {
      return spool->Single(Error(id, status));
    };
    const auto& methods = schema_.catalog()["methods"];
    if (method == "api.describe") {
      if (params.size() != 1 || !params.contains("method") ||
          !params["method"].is_string()) {
        return fail({"invalid_params", "Expected a method name"});
      }
      std::string name = params["method"];
      if (!methods.contains(name)) {
        return fail({"method_not_found", "Unknown generated method"});
      }
      Json description = methods[name];
      description["params"] = schema_.Describe(description["request"]);
      description["result"] = schema_.Describe(description.value("reply", ""));
      return spool->Single(Result(id, description));
    }
    if (method == "api.methods") {
      if (!params.empty()) {
        return fail({"invalid_params", "api.methods takes no parameters"});
      }
      Status connection = Connect();
      for (auto it = methods.begin(); it != methods.end(); ++it) {
        Json item = it.value();
        item["method"] = it.key();
        item["available"] = false;
        if (connection.ok()) {
          for (const auto& binding : GeneratedBindings()) {
            if (it.key() == binding.name) {
              item["available"] = binding.available(context_);
            }
          }
        }
        if (!spool->Add(item)) {
          break;
        }
      }
      return spool->Finish({});
    }
    if (!methods.contains(method)) {
      return Backend::Generic(request, spool);
    }
    const Json& description = methods[method];
    if (!description.value("supported", false)) {
      return fail({"unsupported_operation",
                   description.value("reason", "Unsupported layout")});
    }
    Status status = Connect();
    if (!status.ok()) {
      return fail(status);
    }
    const GeneratedBinding* selected = nullptr;
    for (const auto& binding : GeneratedBindings()) {
      if (method == binding.name) {
        selected = &binding;
      }
    }
    if (!selected || !selected->available(context_)) {
      return fail({"unsupported_operation",
                   "VPP message schema is unavailable or incompatible"});
    }
    std::string input = description["request"], output = description["reply"];
    bool stream = description.value("stream", false);
    if (schema_.HasInterface(input) || schema_.HasInterface(output)) {
      status = RefreshNames();
      if (!status.ok()) {
        return fail(status);
      }
    }
    Symbols symbols;
    symbols.interface_index = [this](const std::string& name,
                                     uint32_t* index) -> Status {
      int matches = 0;
      for (const auto& [key, value] : names_) {
        if (value == name) {
          *index = key;
          ++matches;
        }
      }
      if (matches != 1) {
        return {matches ? "ambiguous_resource" : "resource_not_found",
                "Interface name did not resolve uniquely"};
      }
      return {};
    };
    symbols.interface_name = [this](uint32_t index,
                                    std::string* name) -> Status {
      auto found = names_.find(index);
      if (found == names_.end()) {
        return {"resource_not_found",
                "VPP returned an unresolved interface index"};
      }
      *name = found->second;
      return {};
    };
    std::vector<uint8_t> bytes;
    status = schema_.Encode(input, params, symbols, &bytes);
    if (!status.ok()) {
      return fail(status);
    }
    GeneratedCall call;
    Status reply_status;
    call.item = [&](std::span<const uint8_t> payload) {
      if (!reply_status.ok() || !spool->status().ok()) {
        return;
      }
      Json item;
      reply_status = schema_.Decode(output, payload, symbols, &item);
      if (reply_status.ok()) {
        spool->Add(item);
      }
    };
    auto rc = selected->send(context_, bytes, &call);
    if (rc != VAPI_OK) {
      Reset();
      return fail({"outcome_unknown",
                   "VAPI submission failed; reconcile before retrying"});
    }
    status = Dispatch(&call.done, true);
    if (status.ok() && call.error != VAPI_OK) {
      Reset();
      status = {"outcome_unknown", "VAPI reply failed"};
    }
    if (stream) {
      return spool->Finish(status.ok() ? reply_status : status);
    }
    if (!status.ok()) {
      return fail(status);
    }
    if (call.retval != 0) {
      return fail({"vpp_rejected", "VPP rejected the operation"});
    }
    // A successful create operation can return an interface absent before send.
    if (schema_.HasInterface(output)) {
      status = RefreshNames();
      if (!status.ok()) {
        return fail(
            {"outcome_unknown",
             "Reply received but interface names could not be refreshed"});
      }
    }
    Json result;
    status = schema_.Decode(output, call.reply, symbols, &result);
    if (!status.ok()) {
      return fail(status);
    }
    if (result.contains("retval")) {
      if (result["retval"] != 0) {
        return fail({"vpp_rejected", "VPP rejected the operation (retval " +
                                         result["retval"].dump() + ")"});
      }
      result.erase("retval");
    }
    return spool->Single(Result(id, result));
  }

 private:
  struct DumpState {
    const ItemSink* sink = nullptr;
    std::map<uint32_t, std::string>* names = nullptr;
    std::string name;
    uint32_t index = 0;
    int matches = 0;
    bool done = false;
    bool discard = false;
    vapi_error_e error = VAPI_OK;
  };
  struct ReplyState {
    bool done = false;
    int32_t retval = 0;
    vapi_error_e error = VAPI_OK;
  };
  static vapi_error_e OnDump(vapi_ctx_t, void* opaque, vapi_error_e rc,
                             bool last,
                             vapi_payload_sw_interface_details* reply) {
    auto* state = static_cast<DumpState*>(opaque);
    if (rc != VAPI_OK) {
      state->error = rc;
      state->done = true;
      return VAPI_OK;
    }
    if (reply) {
      const char* raw = reinterpret_cast<const char*>(reply->interface_name);
      std::string name(raw, strnlen(raw, sizeof(reply->interface_name)));
      if (state->names) {
        (*state->names)[reply->sw_if_index] = name;
      }
      if (!state->name.empty() && state->name == name) {
        state->index = reply->sw_if_index;
        ++state->matches;
      }
      if (state->sink && !state->discard) {
        bool up = (reply->flags & IF_STATUS_API_FLAG_ADMIN_UP) != 0;
        state->discard = !(*state->sink)(
            {{"interface", name}, {"state", up ? "up" : "down"}});
      }
    }
    // Even if the spool quota is exhausted, drain until the generated ping
    // completes the dump. A stopped sink must not block VPP's response queue.
    if (last) {
      state->done = true;
    }
    return VAPI_OK;
  }
  static vapi_error_e OnSet(vapi_ctx_t, void* opaque, vapi_error_e rc, bool,
                            vapi_payload_sw_interface_set_flags_reply* reply) {
    auto* state = static_cast<ReplyState*>(opaque);
    state->error = reply ? rc : VAPI_ENORESP;
    if (reply) {
      state->retval = reply->retval;
    }
    state->done = true;
    return VAPI_OK;
  }
  Status RefreshNames() {
    names_.clear();
    if (!vapi_is_msg_available(context_, vapi_msg_id_sw_interface_dump)) {
      return {"unsupported_operation", "Interface resolver unavailable"};
    }
    DumpState state;
    state.names = &names_;
    return Dump(&state);
  }
  Status Dump(DumpState* state) {
    auto* message = vapi_alloc_sw_interface_dump(context_, 0);
    if (!message) {
      return {"backend_unavailable", "Cannot allocate VAPI dump"};
    }
    message->payload.sw_if_index = std::numeric_limits<uint32_t>::max();
    message->payload.name_filter_valid = false;
    vapi_error_e rc = vapi_sw_interface_dump(context_, message, OnDump, state);
    if (rc != VAPI_OK) {
      vapi_msg_free(context_, message);
      Reset();
      return {"backend_unavailable", "Cannot submit VAPI dump"};
    }
    Status status = Dispatch(&state->done, false);
    if (!status.ok()) {
      return status;
    }
    if (state->error != VAPI_OK) {
      Reset();
      return {"backend_unavailable", "VAPI dump failed"};
    }
    return {};
  }
  Status Dispatch(const bool* done, bool mutation) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!*done) {
      vapi_error_e rc = vapi_dispatch_one(context_);
      if (rc != VAPI_OK && rc != VAPI_EAGAIN) {
        Reset();
        return {mutation ? "outcome_unknown" : "backend_unavailable",
                "VAPI connection failed"};
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        Reset();
        return {mutation ? "outcome_unknown" : "deadline_exceeded",
                "VAPI request exceeded 30 seconds"};
      }
      if (rc == VAPI_EAGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    return {};
  }
  Status Connect() {
    if (context_) {
      return {};
    }
    if (vapi_ctx_alloc(&context_) != VAPI_OK) {
      return {"backend_unavailable", "Cannot allocate VAPI context"};
    }
    // VAPI 26.06 exposes requests_size - 1 usable entries. Reserve its
    // sentinel slot in addition to our single in-flight operation.
    constexpr int kRequestSlots = 2;
    vapi_error_e rc =
        vapi_connect_ex(context_, "json-gateway", socket_.c_str(),
                        kRequestSlots, 1024, VAPI_MODE_NONBLOCKING, true, true);
    if (rc != VAPI_OK) {
      vapi_ctx_free(context_);
      context_ = nullptr;
      return {"backend_unavailable",
              "Cannot connect to compatible VPP 26.06 API"};
    }
    return {};
  }
  void Reset() {
    if (!context_) {
      return;
    }
    vapi_disconnect(context_);
    vapi_ctx_free(context_);
    context_ = nullptr;
  }
  Schema schema_ = GeneratedSchema();
  std::map<uint32_t, std::string> names_;
  std::string socket_;
  vapi_ctx_t context_ = nullptr;
};
}  // namespace
std::unique_ptr<Backend> MakeVapiBackend(const std::string& socket) {
  return std::make_unique<VapiBackend>(socket);
}
}  // namespace vpp_json
