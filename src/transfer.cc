// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "src/transfer.h"

#include <fcntl.h>
#include <openssl/evp.h>
#include <unistd.h>

#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace vpp_json {
namespace {
Status Invalid() { return {"invalid_params", "Invalid transfer parameters"}; }
Json ReadManifest(const std::string& path) {
  std::ifstream stream(path);
  if (!stream) {
    return Json();
  }
  return Json::parse(stream, nullptr, false);
}
std::string FileHash(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    std::abort();
  }
  char buffer[65536];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount()) {
    if (EVP_DigestUpdate(context, buffer, stream.gcount()) != 1) {
      std::abort();
    }
  }
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(context, hash, &length) != 1) {
    std::abort();
  }
  EVP_MD_CTX_free(context);
  if (!stream.eof()) {
    return "";
  }
  const char kDigits[] = "0123456789abcdef";
  std::string result;
  for (unsigned int i = 0; i < length; ++i) {
    result += kDigits[hash[i] >> 4];
    result += kDigits[hash[i] & 15];
  }
  return result;
}
}  // namespace
TransferManager::TransferManager(std::string directory)
    : directory_(std::move(directory)) {
  if (!MakeDirectories(directory_)) {
    std::abort();
  }
}
std::string TransferManager::Path(const std::string& session,
                                  const std::string& id) const {
  return directory_ + "/" + Sha256(session + ":" + id);
}
Status TransferManager::Load(const std::string& session, const std::string& id,
                             std::string* path, Json* manifest) {
  *path = Path(session, id);
  *manifest = ReadManifest(*path + ".json");
  if (!manifest->is_object()) {
    return {"resource_not_found", "Transfer not found"};
  }
  if ((*manifest)["expires"].get<int64_t>() <= Now()) {
    return {"transfer_expired", "Start a new transfer"};
  }
  return {};
}
Status TransferManager::Handle(const std::string& session,
                               const std::string& method, const Json& params,
                               Json* result) {
  if (!params.contains("transfer") || !IsId(params["transfer"])) {
    return Invalid();
  }
  std::string id = params["transfer"];
  std::string path = Path(session, id);
  Json manifest;
  if (method == "transfer.begin") {
    if (params.size() != 3 || !params.contains("bytes") ||
        !IsUnsigned(params["bytes"], Limits::kUploadBytes) ||
        !params.contains("sha256") || !params["sha256"].is_string()) {
      return Invalid();
    }
    const std::string digest = params["sha256"];
    if (digest.size() != 64 ||
        digest.find_first_not_of("0123456789abcdef") != std::string::npos) {
      return Invalid();
    }
    manifest = ReadManifest(path + ".json");
    if (manifest.is_object()) {
      if (manifest["total"] != params["bytes"] ||
          manifest["sha256"] != params["sha256"]) {
        return {"id_conflict", "Transfer ID already has a different manifest"};
      }
      *result = {{"transfer", id},
                 {"next_seq", manifest["next_seq"]},
                 {"committed", manifest["committed"]}};
      return {};
    }
    uint64_t reserved = 0;
    int count = 0;
    std::vector<std::string> files;
    Status listed = ListFiles(directory_, &files);
    if (!listed.ok()) {
      return listed;
    }
    for (const auto& entry : files) {
      if (!entry.ends_with(".json")) {
        continue;
      }
      Json other = ReadManifest(entry);
      if (!other.is_object()) {
        continue;
      }
      reserved += other["total"].get<uint64_t>();
      if (other["session"] == session) {
        ++count;
      }
    }
    if (count >= 4 ||
        reserved + params["bytes"].get<uint64_t>() > 256 * 1024 * 1024) {
      return {"quota_exceeded", "Transfer reservation quota exceeded"};
    }
    Status status = WriteAtomic(path + ".data", "");
    if (!status.ok()) {
      return status;
    }
    manifest = {{"session", session},     {"total", params["bytes"]},
                {"sha256", digest},       {"bytes", 0},
                {"next_seq", 0},          {"last_hash", ""},
                {"expires", Now() + 600}, {"committed", false}};
  } else {
    Status status = Load(session, id, &path, &manifest);
    if (!status.ok()) {
      return status;
    }
    if (method == "transfer.chunk") {
      if (params.size() != 3 || !params.contains("seq") ||
          !IsUnsigned(params["seq"], UINT32_MAX) || !params.contains("data") ||
          !params["data"].is_string()) {
        return Invalid();
      }
      uint64_t seq = params["seq"];
      uint64_t next = manifest["next_seq"];
      const std::string data = params["data"];
      if (data.empty()) {
        return Invalid();
      }
      if (next > 0 && seq == next - 1 &&
          Sha256(data) == manifest["last_hash"].get<std::string>()) {
        *result = {{"transfer", id}, {"next_seq", next}};
        return {};
      }
      if (manifest["committed"] == true) {
        return {"transfer_committed", "Committed transfers are immutable"};
      }
      if (seq != next) {
        return {"sequence_mismatch", "Resume from transfer.status next_seq"};
      }
      uint64_t offset = manifest["bytes"];
      if (offset + data.size() > manifest["total"].get<uint64_t>()) {
        return {"payload_too_large", "Chunk exceeds declared transfer length"};
      }
      int fd = open((path + ".data").c_str(), O_WRONLY);
      if (fd < 0) {
        return {"storage_error", "Cannot open transfer"};
      }
      std::size_t written = 0;
      while (written < data.size()) {
        ssize_t count = pwrite(fd, data.data() + written, data.size() - written,
                               offset + written);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          break;
        }
        written += count;
      }
      bool ok = written == data.size() &&
                ftruncate(fd, offset + data.size()) == 0 && fsync(fd) == 0;
      close(fd);
      if (!ok) {
        return {"storage_error", "Cannot persist transfer chunk"};
      }
      manifest["bytes"] = offset + data.size();
      manifest["next_seq"] = next + 1;
      manifest["last_hash"] = Sha256(data);
    } else if (method == "transfer.commit") {
      if (params.size() != 1) {
        return Invalid();
      }
      if (manifest["bytes"] != manifest["total"]) {
        return {"transfer_incomplete", "Declared bytes have not arrived"};
      }
      if (FileHash(path + ".data") != manifest["sha256"].get<std::string>()) {
        return {"checksum_mismatch", "SHA-256 does not match"};
      }
      manifest["committed"] = true;
    } else if (method == "transfer.status") {
      if (params.size() != 1) {
        return Invalid();
      }
    } else {
      return {"method_not_found", "Unknown transfer method"};
    }
  }
  Status status = WriteAtomic(path + ".json", manifest.dump());
  if (!status.ok()) {
    return status;
  }
  *result = {{"transfer", id},
             {"next_seq", manifest["next_seq"]},
             {"bytes", manifest["bytes"]},
             {"committed", manifest["committed"]}};
  return {};
}
Status TransferManager::Resolve(const std::string& session,
                                const std::string& id, Json* params) {
  std::string path;
  Json manifest;
  Status status = Load(session, id, &path, &manifest);
  if (!status.ok()) {
    return status;
  }
  if (manifest["committed"] != true) {
    return {"transfer_incomplete", "Commit before using params_ref"};
  }
  // The adapter's JSON parameter limit remains independent of transport size.
  // Large opaque transfers can be staged, but this initial API has small
  // params.
  if (manifest["total"].get<uint64_t>() > Limits::kFrameBytes) {
    return {"payload_too_large",
            "This adapter accepts at most 256 KiB of JSON parameters"};
  }
  std::ifstream file(path + ".data", std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  Json envelope;
  status = ParseRequest(
      "{\"id\":\"transfer\",\"method\":\"resolve\",\"params\":" + text + "}",
      &envelope);
  if (!status.ok()) {
    return {"invalid_params",
            "Transfer must contain a JSON parameter object within the adapter "
            "limit"};
  }
  *params = envelope["params"];
  return {};
}
void TransferManager::Expire() {
  std::vector<std::string> files;
  if (!ListFiles(directory_, &files).ok()) {
    return;
  }
  for (const auto& entry : files) {
    if (!entry.ends_with(".json")) {
      continue;
    }
    Json manifest = ReadManifest(entry);
    if (!manifest.is_object() || manifest["expires"].get<int64_t>() > Now()) {
      continue;
    }
    unlink((entry.substr(0, entry.size() - 5) + ".data").c_str());
    unlink(entry.c_str());
  }
}
}  // namespace vpp_json
