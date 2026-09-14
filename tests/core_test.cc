#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "src/backend.h"
#include "src/common.h"
#include "src/store.h"
#include "src/transfer.h"

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
}  // namespace
int main() {
  using vpp_json::Json;
  using vpp_json::Status;
  const char* temporary = std::getenv("TMPDIR");
  std::string directory = std::string(temporary ? temporary : "/tmp") +
                          "/gateway-test-" + vpp_json::RandomId();
  Check(vpp_json::MakeDirectories(directory), "Create test directory");
  Json request;
  Check(!vpp_json::ParseRequest("{bad", &request).ok(),
        "Reject malformed JSON");
  Check(!vpp_json::ParseRequest(R"({"id":"a","method":12})", &request).ok(),
        "Reject numeric method");
  Check(!vpp_json::ParseRequest(R"({"id":"a","method":"x","params":[]})",
                                &request)
             .ok(),
        "Reject nonobject params");
  Check(!vpp_json::ParseRequest(R"({"id":"../x","method":"x"})", &request).ok(),
        "Reject unsafe IDs");
  Check(
      !vpp_json::ParseRequest(
           R"({"id":"a","method":"x","params":{},"params_ref":"r"})", &request)
           .ok(),
      "Reject ambiguous params");
  Check(!vpp_json::ParseRequest(std::string(33, '[') + std::string(33, ']'),
                                &request)
             .ok(),
        "Reject deep JSON");
  Check(vpp_json::Sha256("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 vector");
  Check(vpp_json::EqualSecret("abc", "abc") &&
            !vpp_json::EqualSecret("abc", "abd"),
        "Secret comparison");
  {
    vpp_json::Spool spool(directory + "/bounded", "r", 1400);
    for (int i = 0; i < 1000; ++i) {
      spool.Add({{"value", std::string(100, 'a')}});
    }
    Check(spool.Finish({}).ok(), "Quota terminal persisted");
  }
  std::ifstream bounded(directory + "/bounded");
  std::string line;
  Json last;
  int seq = 0;
  while (std::getline(bounded, line)) {
    last = Json::parse(line);
    Check(last["seq"] == seq++, "Contiguous sequence numbers");
  }
  Check(last["type"] == "error" && last["error"]["code"] == "quota_exceeded",
        "No false complete on truncation");
  Check(vpp_json::FileSize(directory + "/bounded") <= 1400,
        "Bounded spool bytes");
  {
    auto backend = vpp_json::MakeMockBackend(3);
    Check(backend->SetState("loop1", true).ok(), "Symbolic state change");
    Check(!backend->SetState("missing", true).ok(),
          "Missing symbolic resource");
    int count = 0;
    Check(backend->List([&count](const Json& item) {
                   if (item["interface"] == "loop1") {
                     Check(item["state"] == "up", "State persisted");
                   }
                   ++count;
                   return true;
                 })
                  .ok() &&
              count == 3,
          "Complete mock enumeration");
  }
  {
    vpp_json::TransferManager transfers(directory + "/uploads");
    std::string body = R"({"interface":"loop1","state":"up"})";
    Json result;
    Json begin = {{"transfer", "t"},
                  {"bytes", body.size()},
                  {"sha256", vpp_json::Sha256(body)}};
    Check(transfers.Handle("s", "transfer.begin", begin, &result).ok(),
          "Begin transfer");
    Check(!transfers
               .Handle("other", "transfer.status", {{"transfer", "t"}}, &result)
               .ok(),
          "Session isolation");
    Check(!transfers.Resolve("s", "t", &result).ok(),
          "Uncommitted transfer inaccessible");
    Check(
        !transfers
             .Handle("s", "transfer.chunk",
                     {{"transfer", "t"}, {"seq", 1u}, {"data", body}}, &result)
             .ok(),
        "Out of order chunk rejected");
    Json chunk = {{"transfer", "t"}, {"seq", 0u}, {"data", body}};
    Check(transfers.Handle("s", "transfer.chunk", chunk, &result).ok(),
          "Write chunk");
    Check(transfers.Handle("s", "transfer.chunk", chunk, &result).ok(),
          "Last chunk retry idempotent");
    Check(result["next_seq"] == 1, "Retry does not advance");
    Check(transfers.Handle("s", "transfer.commit", {{"transfer", "t"}}, &result)
              .ok(),
          "Commit checksum");
    Check(transfers.Resolve("s", "t", &result).ok() && result["state"] == "up",
          "Resolve committed params");
    vpp_json::TransferManager restarted(directory + "/uploads");
    Check(restarted.Resolve("s", "t", &result).ok(),
          "Transfer survives restart");
    begin["transfer"] = "bad";
    begin["sha256"] = std::string(64, '0');
    chunk["transfer"] = "bad";
    Check(transfers.Handle("s", "transfer.begin", begin, &result).ok(),
          "Begin bad checksum transfer");
    Check(transfers.Handle("s", "transfer.chunk", chunk, &result).ok(),
          "Stage bad checksum transfer");
    Check(
        transfers.Handle("s", "transfer.commit", {{"transfer", "bad"}}, &result)
                .code == "checksum_mismatch",
        "Reject checksum mismatch");
  }
  {
    vpp_json::Store store(directory + "/journal");
    store.Query("INSERT INTO sessions VALUES(?,?,?,?)",
                {"s", "client", "registration", "9999999999"});
  }
  {
    vpp_json::Store store(directory + "/journal");
    Check(store.Number("SELECT COUNT(*) FROM sessions WHERE sid=?", {"s"}) == 1,
          "Durable journal");
  }
  vpp_json::RemoveTree(directory);
  std::cout << "Core checks passed\n";
}
