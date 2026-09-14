#ifndef VPP_JSON_GATEWAY_SRC_TRANSFER_H_
#define VPP_JSON_GATEWAY_SRC_TRANSFER_H_

#include <string>

#include "src/common.h"

namespace vpp_json {
// Transfers are private to a session. IDs never become caller-controlled paths.
class TransferManager {
 public:
  explicit TransferManager(std::string directory);
  Status Handle(const std::string& session, const std::string& method,
                const Json& params, Json* result);
  Status Resolve(const std::string& session, const std::string& transfer,
                 Json* params);
  void Expire();

 private:
  std::string Path(const std::string& session, const std::string& id) const;
  Status Load(const std::string& session, const std::string& id,
              std::string* path, Json* manifest);
  std::string directory_;
};
}  // namespace vpp_json
#endif  // VPP_JSON_GATEWAY_SRC_TRANSFER_H_
