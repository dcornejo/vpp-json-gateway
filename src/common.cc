// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "src/common.h"

#include <dirent.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace vpp_json {
namespace {
std::string Hex(const unsigned char* data, std::size_t size) {
  const char kHex[] = "0123456789abcdef";
  std::string output;
  for (std::size_t i = 0; i < size; ++i) {
    output += kHex[data[i] >> 4];
    output += kHex[data[i] & 15];
  }
  return output;
}
bool WriteAll(int fd, const std::string& data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    ssize_t count = write(fd, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    offset += count;
  }
  return true;
}
Status CheckNesting(const std::string& text) {
  // Reject excessive nesting before the recursive JSON parser runs.
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (char c : text) {
    if (quoted) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        quoted = false;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == '{' || c == '[') {
      if (++depth > 32) {
        return {"invalid_request", "Nesting limit exceeded"};
      }
    } else if (c == '}' || c == ']') {
      --depth;
    }
  }
  return {};
}
}  // namespace
std::string ParentDirectory(const std::string& path) {
  auto slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  return slash == 0 ? "/" : path.substr(0, slash);
}
bool MakeDirectories(const std::string& path) {
  if (mkdir(path.c_str(), 0700) == 0) {
    return true;
  }
  if (errno == EEXIST) {
    struct stat info;
    return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
  }
  if (errno != ENOENT || path.empty()) {
    return false;
  }
  return MakeDirectories(ParentDirectory(path)) &&
         mkdir(path.c_str(), 0700) == 0;
}
int64_t FileSize(const std::string& path) {
  struct stat info;
  if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return -1;
  }
  return info.st_size;
}
Status ListFiles(const std::string& directory,
                 std::vector<std::string>* files) {
  DIR* handle = opendir(directory.c_str());
  if (!handle) {
    return {"storage_error", "Cannot list directory"};
  }
  errno = 0;
  while (dirent* entry = readdir(handle)) {
    std::string name = entry->d_name;
    if (name != "." && name != "..") {
      files->push_back(directory + "/" + name);
    }
  }
  int error = errno;
  closedir(handle);
  return error ? Status{"storage_error", "Cannot read directory"} : Status{};
}
bool RemoveTree(const std::string& path) {
  struct stat info;
  if (lstat(path.c_str(), &info) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISDIR(info.st_mode)) {
    return unlink(path.c_str()) == 0;
  }
  std::vector<std::string> files;
  if (!ListFiles(path, &files).ok()) {
    return false;
  }
  for (const auto& file : files) {
    if (!RemoveTree(file)) {
      return false;
    }
  }
  return rmdir(path.c_str()) == 0;
}
int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
std::string RandomId() {
  unsigned char bytes[16];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
    std::abort();
  }
  return Hex(bytes, sizeof(bytes));
}
std::string Sha256(const std::string& data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  if (EVP_Digest(data.data(), data.size(), digest, &size, EVP_sha256(),
                 nullptr) != 1) {
    std::abort();
  }
  return Hex(digest, size);
}
bool EqualSecret(const std::string& a, const std::string& b) {
  return a.size() == b.size() &&
         CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
bool IsId(const Json& value) {
  if (!value.is_string()) {
    return false;
  }
  const auto& text = value.get_ref<const std::string&>();
  if (text.empty() || text.size() > 96) {
    return false;
  }
  for (unsigned char c : text) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}
bool IsUnsigned(const Json& value, uint64_t maximum) {
  return value.is_number_unsigned() && value.get<uint64_t>() <= maximum;
}
Status ParseRequest(const std::string& text, Json* request, std::size_t limit) {
  if (text.size() > limit) {
    return {"frame_too_large", "Use transfer.begin/chunk/commit"};
  }
  Status nesting = CheckNesting(text);
  if (!nesting.ok()) {
    return nesting;
  }
  *request = Json::parse(text, nullptr, false);
  if (!request->is_object() || !request->contains("id") ||
      !IsId((*request)["id"]) || !request->contains("method") ||
      !(*request)["method"].is_string() ||
      (request->contains("params") && !(*request)["params"].is_object()) ||
      (request->contains("params_ref") && !IsId((*request)["params_ref"]))) {
    return {"invalid_request",
            "Expected id, symbolic method, and object params"};
  }
  if (request->contains("params") && request->contains("params_ref")) {
    return {"invalid_request", "Use params or params_ref, not both"};
  }
  for (auto it = request->begin(); it != request->end(); ++it) {
    if (it.key() != "id" && it.key() != "method" && it.key() != "params" &&
        it.key() != "params_ref") {
      return {"invalid_request", "Unknown envelope field"};
    }
  }
  return {};
}
Json Error(const std::string& id, const Status& status) {
  return {{"id", id},
          {"type", "error"},
          {"error", {{"code", status.code}, {"message", status.message}}}};
}
Json Result(const std::string& id, const Json& value) {
  return {{"id", id}, {"type", "result"}, {"result", value}};
}
Status WriteAtomic(const std::string& path, const std::string& data) {
  const std::string temporary = path + ".tmp";
  int fd = open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) {
    return {"storage_error", "Cannot open journal file"};
  }
  bool ok = WriteAll(fd, data) && fsync(fd) == 0;
  if (close(fd) != 0) {
    ok = false;
  }
  if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
    return {"storage_error", "Cannot commit journal file"};
  }
  int directory = open(ParentDirectory(path).c_str(), O_RDONLY);
  ok = directory >= 0 && fsync(directory) == 0;
  if (directory >= 0) {
    close(directory);
  }
  return ok ? Status{}
            : Status{"storage_error", "Cannot sync journal directory"};
}
Spool::Spool(std::string path, std::string request_id, std::size_t limit)
    : path_(std::move(path)), id_(std::move(request_id)), limit_(limit) {
  fd_ = open(path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd_ < 0) {
    status_ = {"storage_error", "Cannot create response spool"};
  }
}
Spool::~Spool() {
  if (fd_ >= 0) {
    close(fd_);
  }
}
bool Spool::Write(Json frame, bool terminal) {
  frame["seq"] = seq_;
  std::string text =
      frame.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
  if (text.size() > Limits::kFrameBytes ||
      (!terminal && bytes_ + text.size() + 1024 > limit_)) {
    status_ = {"quota_exceeded", "Response spool or record limit exceeded"};
    return false;
  }
  if (fd_ < 0 || !WriteAll(fd_, text)) {
    status_ = {"storage_error", "Response spool write failed"};
    return false;
  }
  bytes_ += text.size();
  ++seq_;
  return true;
}
bool Spool::WriteRecord(Json frame) {
  std::string text = frame.dump(-1, ' ', false, Json::error_handler_t::replace);
  if (text.size() + 64 <= Limits::kFrameBytes) {
    return Write(frame, false);
  }
  if (text.size() > Limits::kRecordBytes) {
    status_ = {"payload_too_large", "Logical response exceeds 16 MiB"};
    return false;
  }
  const std::string digest = Sha256(text);
  const uint64_t record = seq_;
  constexpr std::size_t kBlock = 48 * 1024;
  for (std::size_t offset = 0; offset < text.size(); offset += kBlock) {
    std::size_t size = std::min(kBlock, text.size() - offset);
    std::string encoded(4 * ((size + 2) / 3) + 1, '\0');
    int length = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(text.data() + offset), size);
    encoded.resize(length);
    if (!Write({{"id", id_},
                {"type", "fragment"},
                {"record", record},
                {"offset", offset},
                {"total", text.size()},
                {"sha256", digest},
                {"encoding", "base64-json"},
                {"data", encoded}},
               false)) {
      return false;
    }
  }
  return true;
}
Status ResponseAssembler::Accept(const Json& frame, Json* logical,
                                 bool* ready) {
  *ready = false;
  auto invalid = [] {
    return Status{"invalid_reply", "Invalid response fragment"};
  };
  if (!frame.is_object() || !frame.contains("type") ||
      !frame["type"].is_string() || !frame.contains("id") ||
      !frame["id"].is_string()) {
    return invalid();
  }
  if (frame["type"] != "fragment") {
    if (total_ != 0 && frame["type"] != "error") {
      return invalid();
    }
    data_.clear();
    total_ = 0;
    *logical = frame;
    *ready = true;
    return {};
  }
  for (const char* key : {"record", "offset", "total", "seq"}) {
    if (!frame.contains(key) || !IsUnsigned(frame[key], UINT64_MAX)) {
      return invalid();
    }
  }
  if (!frame.contains("sha256") || !frame["sha256"].is_string() ||
      !frame.contains("data") || !frame["data"].is_string() ||
      !frame.contains("encoding") || frame["encoding"] != "base64-json") {
    return invalid();
  }
  uint64_t total = frame["total"], offset = frame["offset"];
  const auto& encoded = frame["data"].get_ref<const std::string&>();
  if (!total || total > Limits::kRecordBytes || encoded.empty() ||
      encoded.size() > 64 * 1024 || encoded.size() % 4 != 0) {
    return invalid();
  }
  if (total_ == 0) {
    if (offset != 0) {
      return invalid();
    }
    total_ = total;
    record_ = frame["record"];
    id_ = frame["id"];
    digest_ = frame["sha256"];
  }
  if (offset != data_.size() || total != total_ || frame["record"] != record_ ||
      frame["id"] != id_ || frame["sha256"] != digest_) {
    return invalid();
  }
  std::string decoded(encoded.size(), '\0');
  int count = EVP_DecodeBlock(
      reinterpret_cast<unsigned char*>(decoded.data()),
      reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
  if (count < 0) {
    return invalid();
  }
  if (encoded.back() == '=') {
    --count;
  }
  if (encoded[encoded.size() - 2] == '=') {
    --count;
  }
  if (count <= 0 || static_cast<uint64_t>(count) > total_ - data_.size()) {
    return invalid();
  }
  decoded.resize(count);
  std::string canonical(4 * ((count + 2) / 3) + 1, '\0');
  int length = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(canonical.data()),
      reinterpret_cast<const unsigned char*>(decoded.data()), count);
  canonical.resize(length);
  if (canonical != encoded) {
    return invalid();
  }
  data_ += decoded;
  if (data_.size() != total_) {
    return {};
  }
  if (Sha256(data_) != digest_) {
    return invalid();
  }
  if (!CheckNesting(data_).ok()) {
    return invalid();
  }
  *logical = Json::parse(data_, nullptr, false);
  if (!logical->is_object() || !logical->contains("id") ||
      (*logical)["id"] != id_ || !logical->contains("type") ||
      ((*logical)["type"] != "chunk" && (*logical)["type"] != "result" &&
       (*logical)["type"] != "error")) {
    return invalid();
  }
  (*logical)["seq"] = frame["seq"];
  data_.clear();
  total_ = 0;
  *ready = true;
  return {};
}
bool Spool::Add(const Json& item) {
  if (!status_.ok()) {
    return false;
  }
  if (!WriteRecord(
          {{"id", id_}, {"type", "chunk"}, {"items", Json::array({item})}})) {
    return false;
  }
  ++items_;
  return true;
}
Status Spool::Finish(const Status& status) {
  if (status_.code == "storage_error") {
    return status_;
  }
  Status terminal = status_.ok() ? status : status_;
  Json frame = terminal.ok() ? Json{{"id", id_},
                                    {"type", "complete"},
                                    {"chunks", seq_},
                                    {"items", items_}}
                             : Error(id_, terminal);
  if (!Write(frame, true) || fsync(fd_) != 0) {
    return {"storage_error", "Cannot persist response"};
  }
  return {};
}
Status Spool::Single(const Json& result) {
  if (!WriteRecord(result)) {
    return Finish(status_);
  }
  if (fsync(fd_) != 0) {
    return {"storage_error", "Cannot persist response"};
  }
  return {};
}
}  // namespace vpp_json
