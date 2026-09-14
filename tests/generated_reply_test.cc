// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>
#include <iostream>
#include <vector>

// Include generated callbacks in this test translation unit to exercise native
// variable payload lengths without depending on hardware queue availability.
#include "generated_api.cc"  // NOLINT(build/include)

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
  auto schema = vpp_json::GeneratedSchema();
  std::vector<uint8_t> bytes;
  Json threads = {{"retval", 0}, {"thread_data", Json::array()}};
  Check(schema.Encode("show_threads_reply", threads, {}, &bytes).ok(),
        "Encode empty variable reply");
  vpp_json::GeneratedCall ordinary;
  vpp_json::Reply_show_threads(
      nullptr, &ordinary, VAPI_OK, true,
      reinterpret_cast<vapi_payload_show_threads_reply*>(bytes.data()));
  Check(ordinary.done && ordinary.reply == bytes, "Zero-length callback reply");

  Json item = {{"sw_if_index", "@all"},
               {"queue_id", 7},
               {"shared", 1},
               {"threads", {0, 1, 2}}};
  Check(
      schema.Encode("sw_interface_tx_placement_details", item, {}, &bytes).ok(),
      "Encode variable TX detail");
  vpp_json::GeneratedCall stream;
  int count = 0;
  stream.item = [&](std::span<const uint8_t> payload) {
    Json decoded;
    Check(
        schema
            .Decode("sw_interface_tx_placement_details", payload, {}, &decoded)
            .ok(),
        "Decode variable callback payload");
    Check(decoded == item && !decoded.contains("array_size"),
          "Array count stays internal");
    ++count;
  };
  vpp_json::Details_sw_interface_tx_placement_get(
      nullptr, &stream, VAPI_OK, false,
      reinterpret_cast<vapi_payload_sw_interface_tx_placement_details*>(
          bytes.data()));
  Check(count == 1 && !stream.done,
        "Detail does not terminate explicit stream");
  Check(schema
            .Encode("sw_interface_tx_placement_get_reply",
                    {{"retval", -165}, {"cursor", 8}}, {}, &bytes)
            .ok(),
        "Encode continuation reply");
  vpp_json::Reply_sw_interface_tx_placement_get(
      nullptr, &stream, VAPI_OK, true,
      reinterpret_cast<vapi_payload_sw_interface_tx_placement_get_reply*>(
          bytes.data()));
  Check(stream.done && stream.retval == -165 && stream.reply == bytes,
        "Continuation completion is retained for pagination");

  // Nested route prefixes contribute fixed bytes; the tail paths array adds
  // variable bytes even though it is inside the route member.
  Json route = {{"table_id", 0},
                {"stats_index", 0},
                {"prefix", "192.0.2.0/24"},
                {"paths", Json::array()}};
  Check(schema
            .Encode("ip_route_lookup_reply", {{"retval", 0}, {"route", route}},
                    {}, &bytes)
            .ok(),
        "Encode nested empty route reply");
  vpp_json::GeneratedCall nested;
  vpp_json::Reply_ip_route_lookup(
      nullptr, &nested, VAPI_OK, true,
      reinterpret_cast<vapi_payload_ip_route_lookup_reply*>(bytes.data()));
  Check(nested.done && nested.reply == bytes,
        "Nested empty array callback length");
  std::vector<uint8_t> one_path(
      sizeof(vapi_payload_ip_route_lookup_reply) +
      sizeof((static_cast<vapi_payload_ip_route_lookup_reply*>(nullptr))
                 ->route.paths[0]));
  auto* payload =
      reinterpret_cast<vapi_payload_ip_route_lookup_reply*>(one_path.data());
  payload->route.n_paths = 1;
  vpp_json::GeneratedCall nonempty;
  vpp_json::Reply_ip_route_lookup(nullptr, &nonempty, VAPI_OK, true, payload);
  Check(nonempty.done && nonempty.reply == one_path,
        "Nested path bytes are included in callback length");

  // The generated callback must reject an over-budget count before copying.
  vapi_payload_show_threads_reply oversized{};
  oversized.count = UINT32_MAX;
  vpp_json::GeneratedCall limited;
  vpp_json::Reply_show_threads(nullptr, &limited, VAPI_OK, true, &oversized);
  Check(limited.done && limited.status.code == "payload_too_large" &&
            limited.reply.empty(),
        "Oversized callback is rejected before copy");
  std::cout << "Generated reply checks passed\n";
}
