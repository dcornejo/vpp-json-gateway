// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "src/common.h"
#include "src/redis_transport.h"

namespace vpp_json {
namespace {
[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(1);
}
void Add(RedisTransport* redis, const std::string& stream, const Json& body) {
  std::string text = body.dump();
  if (text.size() > Limits::kFrameBytes) {
    Fail("Frame exceeds 256 KiB; use the transfer protocol");
  }
  auto reply = redis->Command({"XADD", stream, "*", "json", text});
  if (!reply || reply->type != REDIS_REPLY_STRING) {
    Fail("Redis write failed; retry with the same session and request ID");
  }
}
Json Wait(RedisTransport* redis, const std::string& stream,
          const std::string& id, bool print) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
  uint64_t expected = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    auto reply = redis->Command({"XRANGE", stream, "-", "+", "COUNT", "32"});
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
      Fail("Redis read failed; retry with the same session and request ID");
    }
    for (std::size_t i = 0; i < reply->elements; ++i) {
      auto* row = reply->element[i];
      if (row->type != REDIS_REPLY_ARRAY || row->elements != 2) {
        Fail("Invalid Redis stream entry");
      }
      auto* fields = row->element[1];
      Json frame;
      for (std::size_t j = 0; j + 1 < fields->elements; j += 2) {
        if (std::string(fields->element[j]->str, fields->element[j]->len) ==
            "json") {
          frame = Json::parse(std::string(fields->element[j + 1]->str,
                                          fields->element[j + 1]->len),
                              nullptr, false);
        }
      }
      bool terminal = false;
      if (frame.is_object() && frame.value("id", "") == id) {
        if (!frame.contains("seq") || !IsUnsigned(frame["seq"], UINT64_MAX)) {
          Fail("Missing frame sequence");
        }
        uint64_t seq = frame["seq"];
        if (seq > expected && expected != 0) {
          Fail("Response gap detected; replay the request");
        }
        if (seq == expected) {
          ++expected;
          if (print) {
            std::cout << frame.dump() << '\n';
            std::cout.flush();
          }
          terminal = frame["type"] == "complete" || frame["type"] == "result" ||
                     frame["type"] == "error";
        }
      }
      auto ack = redis->Command(
          {"XDEL", stream,
           std::string(row->element[0]->str, row->element[0]->len)});
      if (!ack || ack->type != REDIS_REPLY_INTEGER) {
        Fail("Response acknowledgment failed; replay is safe");
      }
      if (terminal) {
        return frame;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Fail("Response timeout; retain the session and request ID for replay");
}
}  // namespace
int ClientMain(int argc, char** argv) {
  std::string host = "127.0.0.1", prefix = "vpp", client = "demo";
  std::string session_path = "./session.json", method = "interface.list";
  std::string request_id = RandomId();
  int port = 6379;
  Json params = Json::object();
  for (int i = 1; i < argc; ++i) {
    std::string option = argv[i];
    if (option == "--help") {
      std::cout
          << "gateway-client [--client name] [--session file] [--method name] "
             "[--params JSON | --params-file file] [--request-id id] "
             "[--redis-host host] [--redis-port port] [--namespace name]\n"
             "Set GATEWAY_TOKEN for first registration. One consumer per "
             "session.\n";
      return 0;
    }
    if (++i == argc) {
      Fail("Missing option value");
    }
    std::string value = argv[i];
    if (option == "--client") {
      client = value;
    } else if (option == "--session") {
      session_path = value;
    } else if (option == "--method") {
      method = value;
    } else if (option == "--request-id") {
      request_id = value;
    } else if (option == "--redis-host") {
      host = value;
    } else if (option == "--redis-port") {
      char* end = nullptr;
      int64_t number = std::strtoll(value.c_str(), &end, 10);
      if (value.empty() || *end || number < 1 || number > 65535) {
        Fail("Invalid port");
      }
      port = number;
    } else if (option == "--namespace") {
      prefix = value;
    } else if (option == "--params") {
      params = Json::parse(value, nullptr, false);
    } else if (option == "--params-file") {
      std::ifstream file(value);
      params = Json::parse(file, nullptr, false);
    } else {
      Fail("Unknown option");
    }
  }
  if (!IsId(client) || !IsId(prefix) || !IsId(request_id) ||
      !params.is_object()) {
    Fail("Invalid client, namespace, ID or parameters");
  }
  RedisTransport redis(host, port);
  if (!redis.Connect()) {
    Fail("Cannot connect to Redis");
  }
  std::ifstream saved(session_path);
  Json session = Json::parse(saved, nullptr, false);
  if (!session.is_object() || !session.contains("expires_at") ||
      !session["expires_at"].is_number_integer() ||
      session["expires_at"].get<int64_t>() <= Now()) {
    const char* token = std::getenv("GATEWAY_TOKEN");
    if (!token) {
      Fail("Set GATEWAY_TOKEN to register a new session");
    }
    const std::string registration = RandomId();
    Add(&redis, prefix + ":register",
        {{"id", registration},
         {"method", "session.register"},
         {"params", {{"client", client}, {"token", token}, {"protocol", 1}}}});
    Json reply =
        Wait(&redis, prefix + ":bootstrap:" + client, registration, false);
    if (reply["type"] == "error") {
      Fail(reply.dump());
    }
    session = reply["result"];
    Status status = WriteAtomic(session_path, session.dump(2));
    if (!status.ok()) {
      Fail(status.message);
    }
  }
  if (!session.contains("requests") || !session["requests"].is_string() ||
      !session.contains("responses") || !session["responses"].is_string()) {
    Fail("Invalid session file");
  }
  std::cerr << "request_id=" << request_id << '\n';
  Add(&redis, session["requests"],
      {{"id", request_id}, {"method", method}, {"params", params}});
  // A repeated request is deduplicated. Ask separately for retained output,
  // which also makes rerunning this command with the same ID useful.
  if (method != "transfer.chunk") {
    Add(&redis, session["requests"],
        {{"id", RandomId()},
         {"method", "api.replay"},
         {"params", {{"request", request_id}}}});
  }
  Json result = Wait(&redis, session["responses"], request_id, true);
  return result["type"] == "error" ? 1 : 0;
}
}  // namespace vpp_json
int main(int argc, char** argv) { return vpp_json::ClientMain(argc, argv); }
