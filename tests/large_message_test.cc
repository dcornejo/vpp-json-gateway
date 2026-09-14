// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "src/common.h"
#include "src/schema.h"
#include "src/transfer.h"

namespace {
using vpp_json::Json;
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
std::vector<Json> Read(const std::string& path) {
  std::ifstream file(path);
  std::vector<Json> frames;
  std::string line;
  while (std::getline(file, line)) {
    Check(line.size() + 1 <= vpp_json::Limits::kFrameBytes,
          "Bounded wire frame");
    Json frame = Json::parse(line);
    Check(frame["seq"] == frames.size(), "Contiguous wire sequence");
    frames.push_back(frame);
  }
  return frames;
}
}  // namespace
int main() {
  const std::string directory = "/tmp/large-message-" + vpp_json::RandomId();
  Check(vpp_json::MakeDirectories(directory), "Create directory");
  std::string text;
  for (int i = 0; i < 300000; ++i) {
    text += "\"\\\n\xE2\x98\x83";
  }
  Json item = {{"text", text}};
  {
    vpp_json::Spool spool(directory + "/single", "large");
    Check(spool.Single(vpp_json::Result("large", item)).ok(),
          "Spool large result");
  }
  auto frames = Read(directory + "/single");
  Check(frames.size() > 32, "Large result exceeds delivery window");
  vpp_json::ResponseAssembler assembler;
  Json logical;
  bool ready = false;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    Check(assembler.Accept(frames[i], &logical, &ready).ok(),
          "Assemble fragments");
    Check(ready == (i + 1 == frames.size()), "No premature result");
  }
  Check(logical["result"] == item, "Exact Unicode and escaped text round trip");
  {
    vpp_json::ResponseAssembler corrupt;
    Json bad = frames[0];
    bad["data"].get_ref<std::string&>()[0] = '!';
    Check(!corrupt.Accept(bad, &logical, &ready).ok(), "Reject invalid base64");
  }
  {
    vpp_json::ResponseAssembler missing;
    Check(!missing.Accept(frames[1], &logical, &ready).ok(),
          "Reject missing first fragment");
  }
  {
    vpp_json::ResponseAssembler corrupt;
    for (auto frame : frames) {
      frame["sha256"] = std::string(64, '0');
      auto status = corrupt.Accept(frame, &logical, &ready);
      if (frame["seq"] == frames.back()["seq"]) {
        Check(!status.ok(), "Reject wrong checksum");
      } else {
        Check(status.ok() && !ready, "Await checksum verification");
      }
    }
  }
  {
    vpp_json::Spool spool(directory + "/quota", "large", 100000);
    Check(!spool.Add(item), "Stop partial record at quota");
    Check(spool.Finish({}).ok(), "Persist terminal quota error");
  }
  vpp_json::ResponseAssembler partial;
  for (const auto& frame : Read(directory + "/quota")) {
    Check(partial.Accept(frame, &logical, &ready).ok(),
          "Discard partial record on error");
  }
  Check(ready && logical["type"] == "error",
        "No false complete after partial record");
  Check(vpp_json::FileSize(directory + "/quota") <= 100000,
        "Spool budget respected");
  {
    vpp_json::TransferManager transfers(directory + "/uploads");
    // Large valid params, even though the friendly operation itself is small.
    std::string body(1024 * 1024, ' ');
    body += R"({"interface":"loop1","state":"up"})";
    Json result;
    Check(transfers
              .Handle("s", "transfer.begin",
                      {{"transfer", "large"},
                       {"bytes", body.size()},
                       {"sha256", vpp_json::Sha256(body)}},
                      &result)
              .ok(),
          "Begin large params");
    uint64_t seq = 0;
    for (std::size_t offset = 0; offset < body.size(); offset += 100000) {
      Check(transfers
                .Handle("s", "transfer.chunk",
                        {{"transfer", "large"},
                         {"seq", seq++},
                         {"data", body.substr(offset, 100000)}},
                        &result)
                .ok(),
            "Upload params");
    }
    Check(transfers
              .Handle("s", "transfer.commit", {{"transfer", "large"}}, &result)
              .ok(),
          "Commit large params");
    Check(transfers.Resolve("s", "large", &result).ok() &&
              result["interface"] == "loop1",
          "Resolve params beyond one frame");
  }
  vpp_json::Schema schema;
  std::vector<uint8_t> encoded;
  Check(schema.Encode("string", std::string(300000, 'x'), {}, &encoded).ok(),
        "Encode native payload above frame limit");
  Check(!schema
             .Encode("string",
                     std::string(vpp_json::Limits::kNativePayloadBytes, 'x'),
                     {}, &encoded)
             .ok(),
        "Enforce native payload budget including length");
  Check(vpp_json::RemoveTree(directory), "Clean test files");
  std::cout << "Large message checks passed\n";
}
