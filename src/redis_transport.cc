// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "src/redis_transport.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "src/common.h"

namespace vpp_json {
void ReplyDeleter::operator()(redisReply* reply) const {
  if (reply) {
    freeReplyObject(reply);
  }
}
RedisTransport::RedisTransport(std::string host, int port)
    : host_(std::move(host)), port_(port) {}
RedisTransport::~RedisTransport() {
  if (context_) {
    redisFree(context_);
  }
}
bool RedisTransport::Connect() {
  if (context_ && !context_->err) {
    return true;
  }
  if (context_) {
    redisFree(context_);
  }
  timeval timeout{0, 200000};
  context_ = redisConnectWithTimeout(host_.c_str(), port_, timeout);
  if (!context_ || context_->err) {
    return false;
  }
  if (redisSetTimeout(context_, timeout) != REDIS_OK) {
    return false;
  }
  const char* password = std::getenv("REDIS_PASSWORD");
  const char* username = std::getenv("REDIS_USERNAME");
  if (password) {
    Reply reply = username ? Command({"AUTH", username, password})
                           : Command({"AUTH", password});
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
      redisFree(context_);
      context_ = nullptr;
      return false;
    }
  }
  return true;
}
Reply RedisTransport::Command(const std::vector<std::string>& args) {
  if (!context_ || context_->err) {
    return {};
  }
  std::vector<const char*> values;
  std::vector<std::size_t> sizes;
  for (const auto& arg : args) {
    values.push_back(arg.data());
    sizes.push_back(arg.size());
  }
  return Reply(static_cast<redisReply*>(
      redisCommandArgv(context_, values.size(), values.data(), sizes.data())));
}
bool RedisTransport::Publish(const std::string& stream,
                             const std::string& json) {
  // A single atomic operation enforces the window even if other writers exist.
  // All keys of this script belong to one stream and work in a cluster slot;
  // the transport itself targets a standalone Redis endpoint.
  static const std::string kScript =
      "if redis.call('XLEN',KEYS[1]) >= tonumber(ARGV[1]) then return false "
      "end "
      "local id=redis.call('XADD',KEYS[1],'*','json',ARGV[2]); "
      "redis.call('EXPIRE',KEYS[1],ARGV[3]); return id";
  Reply reply =
      Command({"EVAL", kScript, "1", stream, std::to_string(Limits::kWindow),
               json, std::to_string(Limits::kLeaseSeconds)});
  return reply && reply->type == REDIS_REPLY_STRING;
}
}  // namespace vpp_json
