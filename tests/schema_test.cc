// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "src/schema.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void Check(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
}  // namespace
int main() {
  using vpp_json::Json;
  vpp_json::Schema schema;
  Json document = Json::parse(R"({
    "messages":[["example",["u16","_vl_msg_id"],["u32","context"],
      ["u8","count"],["u16","values",0,"count"],
      ["vl_api_flags_t","flags"],["u64","large"],{}]],
    "services":{"example":{"reply":"example"}},
    "enumflags":[["flags",["ONE",1],["TWO",2],{"enumtype":"u32"}]]
  })");
  Check(schema.AddModule(document, "test").ok(), "Load schema");
  Json input = {{"values", {10, 65535}},
                {"flags", {"ONE", "TWO"}},
                {"large", "18446744073709551615"}};
  std::vector<uint8_t> bytes;
  Check(schema.Encode("example", input, {}, &bytes).ok(),
        "Encode arrays and symbols");
  Check(bytes.size() == 17 && bytes[0] == 2, "Derived count and packed layout");
  Json output;
  Check(schema.Decode("example", bytes, {}, &output).ok() && output == input,
        "Round trip preserves exact large integer and hides count");
  bytes.pop_back();
  Check(!schema.Decode("example", bytes, {}, &output).ok(),
        "Reject truncated reply");
  input["count"] = 2;
  Check(!schema.Encode("example", input, {}, &bytes).ok(),
        "Reject caller-supplied count");
  input.erase("count");
  input["flags"] = 3;
  Check(!schema.Encode("example", input, {}, &bytes).ok(),
        "Reject numeric flags");
  input["flags"] = {"ONE", "ONE"};
  Check(!schema.Encode("example", input, {}, &bytes).ok(),
        "Reject duplicate flags");
  input["flags"] = {"ONE"};
  input["values"] = {65536};
  Check(!schema.Encode("example", input, {}, &bytes).ok(),
        "Reject integer overflow");
  input["values"] = {1};
  input["large"] = "18446744073709551616";
  Check(!schema.Encode("example", input, {}, &bytes).ok(),
        "Reject 64-bit overflow");
  Check(!schema.Describe("example")["properties"].contains("count"),
        "Discovery hides counts");
  Check(!schema.Encode("vl_api_interface_index_t", 5, {}, &bytes).ok(),
        "Reject numeric interface index");
  Check(schema.Encode("vl_api_interface_index_t", "@all", {}, &bytes).ok(),
        "Encode symbolic sentinel");
  Check(schema.Decode("vl_api_interface_index_t", bytes, {}, &output).ok() &&
            output == "@all",
        "Decode symbolic sentinel");
  Check(schema.Encode("vl_api_mac_address_t", "02:00:ab:cd:ef:01", {}, &bytes)
            .ok(),
        "Encode MAC");
  Check(schema.Decode("vl_api_mac_address_t", bytes, {}, &output).ok() &&
            output == "02:00:ab:cd:ef:01",
        "Decode MAC");
  Check(!schema.Encode("vl_api_mac_address_t", "02:00:ag:cd:ef:01", {}, &bytes)
             .ok(),
        "Reject malformed MAC");
  std::cout << "Schema checks passed\n";
}
