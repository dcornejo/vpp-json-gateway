#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "src/common.h"
#include "src/redis_transport.h"

namespace {
using vpp_json::Json;
using vpp_json::RedisTransport;
[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(1);
}
std::string Text(const redisReply* reply) {
  if (!reply || reply->type != REDIS_REPLY_STRING) {
    Fail("Expected a Redis string");
  }
  return std::string(reply->str, reply->len);
}
void Send(RedisTransport* redis, const std::string& stream,
          const Json& request) {
  auto reply = redis->Command({"XADD", stream, "*", "json", request.dump()});
  if (!reply || reply->type != REDIS_REPLY_STRING) {
    Fail("Cannot send request; check Redis credentials and ACLs");
  }
}
// Consume one request at a time and acknowledge only after processing each
// frame. XDEL releases the server's bounded response window for the next frame.
Json Receive(RedisTransport* redis, const std::string& stream,
             const std::string& id) {
  uint64_t expected = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    auto reply = redis->Command({"XRANGE", stream, "-", "+", "COUNT", "32"});
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
      Fail("Cannot read response stream");
    }
    for (std::size_t i = 0; i < reply->elements; ++i) {
      const auto* row = reply->element[i];
      if (row->type != REDIS_REPLY_ARRAY || row->elements != 2 ||
          row->element[1]->type != REDIS_REPLY_ARRAY) {
        Fail("Malformed stream entry");
      }
      const auto* fields = row->element[1];
      Json frame;
      for (std::size_t j = 0; j + 1 < fields->elements; j += 2) {
        if (Text(fields->element[j]) == "json") {
          frame = Json::parse(Text(fields->element[j + 1]), nullptr, false);
        }
      }
      if (!frame.is_object() || frame.value("id", "") != id) {
        // Another request's response belongs to its consumer; do not delete it.
        continue;
      }
      if (!frame.contains("seq") ||
          !vpp_json::IsUnsigned(frame["seq"], UINT64_MAX) ||
          frame["seq"].get<uint64_t>() != expected++) {
        Fail(
            "Response sequence gap; retain the request ID for explicit replay");
      }
      std::cout << frame.dump() << '\n';
      std::cout.flush();
      auto ack = redis->Command({"XDEL", stream, Text(row->element[0])});
      if (!ack || ack->type != REDIS_REPLY_INTEGER) {
        Fail("Acknowledgment failed");
      }
      if (frame["type"] == "error") {
        Fail("API returned an error; see the JSON frame above");
      }
      if (frame["type"] == "result" || frame["type"] == "complete") {
        return frame;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Fail("Response timeout; do not blindly retry mutations with a new ID");
}
}  // namespace
int main() {
  const char* token = std::getenv("GATEWAY_TOKEN");
  if (!token) {
    Fail("Source example/client.env first");
  }
  RedisTransport redis("127.0.0.1", 6389);
  if (!redis.Connect()) {
    Fail("Cannot authenticate to the example Redis server");
  }
  std::string registration = vpp_json::RandomId();
  Send(&redis, "vpp:register",
       {{"id", registration},
        {"method", "session.register"},
        {"params", {{"client", "demo"}, {"token", token}, {"protocol", 1}}}});
  Json session = Receive(&redis, "vpp:bootstrap:demo", registration)["result"];
  // Use the assigned keys verbatim. Never derive them from a guessed session
  // ID.
  const std::string requests = session["requests"];
  const std::string responses = session["responses"];
  auto call = [&](const std::string& method, const Json& params) {
    std::string id = vpp_json::RandomId();
    std::cerr << "Calling " << method << " request_id=" << id << '\n';
    Send(&redis, requests,
         {{"id", id}, {"method", method}, {"params", params}});
    Receive(&redis, responses, id);
  };
  call("api.describe", Json::object());
  call("interface.list", Json::object());
  call("interface.set_state", {{"interface", "loop1"}, {"state", "up"}});
  call("interface.list", Json::object());
  return 0;
}
