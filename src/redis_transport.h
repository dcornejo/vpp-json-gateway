// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef VPP_JSON_GATEWAY_SRC_REDIS_TRANSPORT_H_
#define VPP_JSON_GATEWAY_SRC_REDIS_TRANSPORT_H_

#include <hiredis/hiredis.h>

#include <memory>
#include <string>
#include <vector>

namespace vpp_json {
struct ReplyDeleter {
  void operator()(redisReply* reply) const;
};
using Reply = std::unique_ptr<redisReply, ReplyDeleter>;
class RedisTransport {
 public:
  RedisTransport(std::string host, int port);
  ~RedisTransport();
  RedisTransport(const RedisTransport&) = delete;
  RedisTransport& operator=(const RedisTransport&) = delete;
  bool Connect();
  Reply Command(const std::vector<std::string>& args);
  bool Publish(const std::string& stream, const std::string& json);

 private:
  std::string host_;
  int port_;
  redisContext* context_ = nullptr;
};
}  // namespace vpp_json
#endif  // VPP_JSON_GATEWAY_SRC_REDIS_TRANSPORT_H_
