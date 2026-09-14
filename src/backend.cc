#include "src/backend.h"

#include <map>
#include <memory>
#include <string>
#include <utility>

namespace vpp_json {
namespace {
class MockBackend final : public Backend {
 public:
  explicit MockBackend(int count) : count_(count) {}
  Status List(const ItemSink& sink) override {
    for (int i = 0; i < count_; ++i) {
      const std::string name = "loop" + std::to_string(i);
      auto it = states_.find(name);
      bool up = it != states_.end() && it->second;
      if (!sink({{"interface", name}, {"state", up ? "up" : "down"}})) {
        break;
      }
    }
    return {};
  }
  Status SetState(const std::string& name, bool up) override {
    for (int i = 0; i < count_; ++i) {
      if (name == "loop" + std::to_string(i)) {
        states_[name] = up;
        return {};
      }
    }
    return {"resource_not_found", "Interface name did not resolve"};
  }

 private:
  int count_;
  std::map<std::string, bool> states_;
};
}  // namespace
std::unique_ptr<Backend> MakeMockBackend(int count) {
  return std::make_unique<MockBackend>(count);
}
Status Backend::Generic(const Json& request, Spool* spool) {
  return spool->Single(
      Error(request["id"], {"method_not_found", "Method is not supported"}));
}
Status RunBackend(Backend* backend, const Json& request, Spool* spool) {
  const std::string method = request["method"];
  const Json params = request.value("params", Json::object());
  const std::string id = request["id"];
  if (method == "interface.list") {
    if (!params.empty()) {
      return spool->Single(
          Error(id, {"invalid_params", "interface.list takes no parameters"}));
    }
    return spool->Finish(
        backend->List([spool](const Json& item) { return spool->Add(item); }));
  }
  if (method == "interface.set_state") {
    if (params.size() != 2 || !params.contains("interface") ||
        !params["interface"].is_string() ||
        params["interface"].get_ref<const std::string&>().empty() ||
        params["interface"].get_ref<const std::string&>().size() > 63 ||
        !params.contains("state") ||
        (params["state"] != "up" && params["state"] != "down")) {
      return spool->Single(Error(
          id,
          {"invalid_params", "Expected interface name and state up or down"}));
    }
    Status status =
        backend->SetState(params["interface"], params["state"] == "up");
    return spool->Single(status.ok() ? Result(id, Json::object())
                                     : Error(id, status));
  }
  return backend->Generic(request, spool);
}
}  // namespace vpp_json
