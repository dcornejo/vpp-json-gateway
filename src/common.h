// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef VPP_JSON_GATEWAY_SRC_COMMON_H_
#define VPP_JSON_GATEWAY_SRC_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace vpp_json {
using Json = nlohmann::json;
struct Status {
  std::string code;
  std::string message;
  bool ok() const { return code.empty(); }
};
struct Limits {
  static constexpr std::size_t kFrameBytes = 256 * 1024;
  static constexpr std::size_t kSpoolBytes = 64 * 1024 * 1024;
  static constexpr std::size_t kUploadBytes = 64 * 1024 * 1024;
  static constexpr int kWindow = 32;
  static constexpr int kSessions = 32;
  static constexpr int kJobsPerSession = 128;
  static constexpr int kLeaseSeconds = 3600;
};
bool MakeDirectories(const std::string& path);
int64_t FileSize(const std::string& path);
Status ListFiles(const std::string& directory, std::vector<std::string>* files);
bool RemoveTree(const std::string& path);
std::string ParentDirectory(const std::string& path);
int64_t Now();
std::string RandomId();
std::string Sha256(const std::string& data);
bool EqualSecret(const std::string& a, const std::string& b);
bool IsId(const Json& value);
bool IsUnsigned(const Json& value, uint64_t maximum);
Status ParseRequest(const std::string& text, Json* request);
Json Error(const std::string& id, const Status& status);
Json Result(const std::string& id, const Json& value);
Status WriteAtomic(const std::string& path, const std::string& data);

// Disk-backed, bounded result stream. Always reserve space for a terminal
// error.
class Spool {
 public:
  Spool(std::string path, std::string request_id,
        std::size_t limit = Limits::kSpoolBytes);
  ~Spool();
  Spool(const Spool&) = delete;
  Spool& operator=(const Spool&) = delete;
  bool Add(const Json& item);
  Status Finish(const Status& status);
  Status Single(const Json& result);
  const Status& status() const { return status_; }

 private:
  bool Write(Json frame, bool terminal);
  std::string path_;
  std::string id_;
  int fd_ = -1;
  uint64_t seq_ = 0;
  uint64_t items_ = 0;
  std::size_t bytes_ = 0;
  std::size_t limit_;
  Status status_;
};
}  // namespace vpp_json
#endif  // VPP_JSON_GATEWAY_SRC_COMMON_H_
