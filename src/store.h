#ifndef VPP_JSON_GATEWAY_SRC_STORE_H_
#define VPP_JSON_GATEWAY_SRC_STORE_H_

#include <sqlite3.h>

#include <string>
#include <vector>

namespace vpp_json {
// Main-thread-only journal. Any storage error terminates the process before
// another operation can execute; recovery converts running jobs to uncertain.
class Store {
 public:
  explicit Store(const std::string& path);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  using Rows = std::vector<std::vector<std::string>>;
  Rows Query(const std::string& sql, const std::vector<std::string>& args = {});
  int64_t Number(const std::string& sql,
                 const std::vector<std::string>& args = {});

 private:
  sqlite3* db_ = nullptr;
};
}  // namespace vpp_json
#endif  // VPP_JSON_GATEWAY_SRC_STORE_H_
