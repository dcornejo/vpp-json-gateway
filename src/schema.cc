// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "src/schema.h"

#include <arpa/inet.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace vpp_json {
namespace {
constexpr std::size_t kPayloadLimit = Limits::kNativePayloadBytes;
Status Invalid(const std::string& message) {
  return {"invalid_params", message};
}
bool Identifier(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return !(text[0] >= '0' && text[0] <= '9');
}
Json Fields(const Json& definition, bool message) {
  Json fields = Json::array();
  for (std::size_t i = 1; i < definition.size(); ++i) {
    const auto& raw = definition[i];
    if (!raw.is_array() || raw.size() < 2) {
      continue;
    }
    std::string name = raw[1];
    if (message &&
        (name == "_vl_msg_id" || name == "context" || name == "client_index")) {
      continue;
    }
    Json field = {{"type", raw[0]}, {"name", name}};
    if (raw.size() > 2 &&
        (raw[2].is_number_integer() && raw[2].get<int64_t>() >= 0)) {
      field["length"] = raw[2];
    }
    if (raw.size() > 3 && raw[3].is_string()) {
      field["count"] = raw[3];
    }
    for (const auto& part : raw) {
      if (!part.is_object()) {
        continue;
      }
      for (const std::string key : {"default", "discriminator", "case"}) {
        if (part.contains(key)) {
          field[key] = part[key];
        }
      }
    }
    fields.push_back(field);
  }
  return fields;
}
std::set<std::string> Counts(const Json& fields) {
  std::set<std::string> result;
  for (const auto& field : fields) {
    if (field.contains("count")) {
      result.insert(field["count"].get<std::string>());
    }
  }
  return result;
}
Status Bytes(void* data, std::size_t count, std::vector<uint8_t>* encoded,
             std::span<const uint8_t>* decoded) {
  if (encoded) {
    if (count > kPayloadLimit || encoded->size() > kPayloadLimit - count) {
      return {"payload_too_large", "Native payload exceeds adapter limit"};
    }
    const auto* source = static_cast<uint8_t*>(data);
    encoded->insert(encoded->end(), source, source + count);
  } else {
    if (decoded->size() < count) {
      return {"invalid_reply", "Truncated native payload"};
    }
    std::memcpy(data, decoded->data(), count);
    *decoded = decoded->subspan(count);
  }
  return {};
}
template <typename T>
Status Number(Json* value, std::vector<uint8_t>* encoded,
              std::span<const uint8_t>* decoded) {
  T native = 0;
  if (encoded) {
    if constexpr (std::is_floating_point_v<T>) {
      if (!value->is_number()) {
        return Invalid("Expected a finite number");
      }
      double input = value->get<double>();
      if (!std::isfinite(input) ||
          std::abs(input) > std::numeric_limits<T>::max()) {
        return Invalid("Number is out of range");
      }
      native = static_cast<T>(input);
    } else if constexpr (sizeof(T) == 8) {
      if (!value->is_string()) {
        return Invalid("64-bit integers must be decimal strings");
      }
      const auto& text = value->get_ref<const std::string&>();
      auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), native);
      if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
        return Invalid("Invalid decimal integer");
      }
    } else {
      if (!value->is_number_integer()) {
        return Invalid("Expected an integer");
      }
      if (value->is_number_unsigned()) {
        uint64_t input = value->get<uint64_t>();
        if (input > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
          return Invalid("Integer is out of range");
        }
        native = static_cast<T>(input);
      } else {
        int64_t input = value->get<int64_t>();
        if (input < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
            input > static_cast<int64_t>(std::numeric_limits<T>::max())) {
          return Invalid("Integer is out of range");
        }
        native = static_cast<T>(input);
      }
    }
  }
  Status status = Bytes(&native, sizeof(native), encoded, decoded);
  if (!status.ok()) {
    return status;
  }
  if (!encoded) {
    if constexpr (std::is_integral_v<T> && sizeof(T) == 8) {
      *value = std::to_string(native);
    } else {
      *value = native;
    }
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(native)) {
        return {"invalid_reply", "VPP returned a nonfinite number"};
      }
    }
  }
  return {};
}
Json PublicDefault(const Json& field, const Schema& schema) {
  Json value = field["default"];
  std::string type = field["type"];
  if (type == "bool" && value == "true") {
    return true;
  }
  if (type == "bool" && value == "false") {
    return false;
  }
  if (type == "vl_api_interface_index_t" && value == UINT32_MAX) {
    return "@all";
  }
  const Json* definition = schema.Type(type);
  if (definition && (*definition)["kind"] == "enum") {
    if (definition->value("flags", false) && value == 0) {
      return Json::array();
    }
    for (auto it = (*definition)["values"].begin();
         it != (*definition)["values"].end(); ++it) {
      if (it.value() == value) {
        return it.key();
      }
    }
  }
  if ((type == "u64" || type == "i64") && value.is_number_integer()) {
    return value.dump();
  }
  return value;
}
}  // namespace
Status Schema::Load(const Json& catalog) {
  if (!catalog.is_object() || !catalog.contains("types") ||
      !catalog["types"].is_object() || !catalog.contains("methods") ||
      !catalog["methods"].is_object()) {
    return Invalid("Invalid catalog");
  }
  catalog_ = catalog;
  return {};
}
Status Schema::AddModule(const Json& document, const std::string& module) {
  if (!Identifier(module) || !document.is_object() ||
      !document.contains("messages") || !document["messages"].is_array() ||
      !document.contains("services")) {
    return Invalid("Invalid VPP API module");
  }
  auto add = [this](const std::string& name, const Json& definition) -> Status {
    if (!Identifier(name)) {
      return Invalid("Unsafe type name in VPP schema");
    }
    if (catalog_["types"].contains(name) &&
        catalog_["types"][name] != definition) {
      return Invalid("Conflicting VPP type: " + name);
    }
    catalog_["types"][name] = definition;
    return {};
  };
  for (const std::string category : {"types", "unions", "messages"}) {
    for (const auto& raw : document.value(category, Json::array())) {
      if (!raw.is_array() || raw.empty() || !raw[0].is_string()) {
        return Invalid("Malformed VPP type definition");
      }
      std::string name = raw[0];
      bool message = category == "messages";
      Json definition = {{"kind", category == "unions" ? "union" : "struct"},
                         {"fields", Fields(raw, message)}};
      for (const auto& field : definition["fields"]) {
        if (!Identifier(field["name"]) || !Identifier(field["type"]) ||
            (field.contains("count") && !Identifier(field["count"]))) {
          return Invalid("Unsafe field name in VPP schema");
        }
      }
      Status status = add(message ? name : "vl_api_" + name + "_t", definition);
      if (!status.ok()) {
        return status;
      }
    }
  }
  for (const std::string category : {"enums", "enumflags"}) {
    for (const auto& raw : document.value(category, Json::array())) {
      std::string name = raw[0];
      Json values = Json::object();
      for (std::size_t i = 1; i < raw.size(); ++i) {
        if (raw[i].is_array()) {
          values[raw[i][0].get<std::string>()] = raw[i][1];
        }
      }
      // These legacy bitsets predate VPP's enumflag syntax.
      bool flags = category == "enumflags" || name == "if_status_flags" ||
                   name == "sub_if_flags";
      Status status = add("vl_api_" + name + "_t",
                          {{"kind", "enum"},
                           {"type", raw.back().value("enumtype", "u32")},
                           {"values", values},
                           {"flags", flags}});
      if (!status.ok()) {
        return status;
      }
    }
  }
  const Json aliases = document.value("aliases", Json::object());
  for (auto it = aliases.begin(); it != aliases.end(); ++it) {
    Json alias = it.value();
    alias["kind"] = "alias";
    Status status = add("vl_api_" + it.key() + "_t", alias);
    if (!status.ok()) {
      return status;
    }
  }
  for (auto it = document["services"].begin(); it != document["services"].end();
       ++it) {
    if (!Identifier(it.key())) {
      return Invalid("Unsafe message name");
    }
    Json method = it.value();
    method["request"] = it.key();
    method["module"] = module;
    std::string name = "vpp." + it.key();
    if (catalog_["methods"].contains(name)) {
      return Invalid("Duplicate VPP method: " + name);
    }
    for (const auto& raw : document["messages"]) {
      if (raw[0] == it.key()) {
        method["crc"] = raw.back().value("crc", "");
      }
    }
    catalog_["methods"][name] = method;
  }
  return {};
}
const Json* Schema::Type(const std::string& name) const {
  auto it = catalog_["types"].find(name);
  return it == catalog_["types"].end() ? nullptr : &*it;
}
int64_t Schema::FixedSize(const std::string& name, int depth) const {
  if (depth > 32) {
    return -1;
  }
  if (name == "bool" || name == "u8" || name == "i8") {
    return 1;
  }
  if (name == "u16" || name == "i16") {
    return 2;
  }
  if (name == "u32" || name == "i32" || name == "f32" || name == "string") {
    return 4;
  }
  if (name == "u64" || name == "i64" || name == "f64") {
    return 8;
  }
  const Json* type = Type(name);
  if (!type) {
    return -1;
  }
  std::string kind = (*type)["kind"];
  if (kind == "alias" || kind == "enum") {
    int64_t size = FixedSize((*type)["type"], depth + 1);
    return size < 0 ? -1 : size * type->value("length", int64_t{1});
  }
  int64_t size = 0;
  for (const auto& field : (*type)["fields"]) {
    int64_t part = field["type"] == "string" && field.contains("length") &&
                           field["length"] != 0
                       ? field["length"].get<int64_t>()
                       : FixedSize(field["type"], depth + 1) *
                             field.value("length", int64_t{1});
    if (field["type"] == "string" && field.value("length", 0) == 0) {
      part = 4;
    }
    if (part < 0 || part > static_cast<int64_t>(kPayloadLimit)) {
      return -1;
    }
    size = kind == "union" ? std::max(size, part) : size + part;
  }
  return size;
}
bool Schema::Variable(const std::string& name, int depth) const {
  if (depth > 32 || name == "string") {
    return true;
  }
  const Json* type = Type(name);
  if (!type) {
    return false;
  }
  if ((*type)["kind"] == "alias") {
    return Variable((*type)["type"], depth + 1);
  }
  if (!type->contains("fields")) {
    return false;
  }
  for (const auto& field : (*type)["fields"]) {
    if (field["type"] == "string" && field.value("length", 0) > 0) {
      continue;
    }
    if (field.value("length", -1) == 0 || Variable(field["type"], depth + 1)) {
      return true;
    }
  }
  return false;
}
bool Schema::HasInterface(const std::string& name, int depth) const {
  if (name == "vl_api_interface_index_t") {
    return true;
  }
  if (depth > 32) {
    return false;
  }
  const Json* type = Type(name);
  if (!type) {
    return false;
  }
  if ((*type)["kind"] == "alias") {
    return HasInterface((*type)["type"], depth + 1);
  }
  for (const auto& field : type->value("fields", Json::array())) {
    if (HasInterface(field["type"], depth + 1)) {
      return true;
    }
  }
  return false;
}
Status Schema::Encode(const std::string& type, const Json& value,
                      const Symbols& symbols,
                      std::vector<uint8_t>* output) const {
  output->clear();
  Json copy = value;
  return Convert(type, &copy, symbols, output, nullptr, nullptr, 0);
}
Status Schema::Decode(const std::string& type, std::span<const uint8_t> bytes,
                      const Symbols& symbols, Json* output) const {
  Status status = Convert(type, output, symbols, nullptr, &bytes, nullptr, 0);
  if (!status.ok()) {
    return status;
  }
  return bytes.empty()
             ? Status{}
             : Status{"invalid_reply", "Unexpected trailing payload bytes"};
}
Status Schema::Convert(const std::string& name, Json* value,
                       const Symbols& symbols, std::vector<uint8_t>* encoded,
                       std::span<const uint8_t>* decoded, const Json* tag,
                       int depth) const {
  if (depth > 32) {
    return Invalid("Type nesting exceeds 32 levels");
  }
  if (name == "u8") {
    return Number<uint8_t>(value, encoded, decoded);
  }
  if (name == "i8") {
    return Number<int8_t>(value, encoded, decoded);
  }
  if (name == "u16") {
    return Number<uint16_t>(value, encoded, decoded);
  }
  if (name == "i16") {
    return Number<int16_t>(value, encoded, decoded);
  }
  if (name == "u32") {
    return Number<uint32_t>(value, encoded, decoded);
  }
  if (name == "i32") {
    return Number<int32_t>(value, encoded, decoded);
  }
  if (name == "u64") {
    return Number<uint64_t>(value, encoded, decoded);
  }
  if (name == "i64") {
    return Number<int64_t>(value, encoded, decoded);
  }
  if (name == "f32") {
    return Number<float>(value, encoded, decoded);
  }
  if (name == "f64") {
    return Number<double>(value, encoded, decoded);
  }
  if (name == "bool") {
    if (encoded && !value->is_boolean()) {
      return Invalid("Expected a boolean");
    }
    uint8_t byte = encoded && value->get<bool>() ? 1 : 0;
    Status status = Bytes(&byte, 1, encoded, decoded);
    if (!status.ok()) {
      return status;
    }
    if (byte > 1) {
      return {"invalid_reply", "Noncanonical boolean"};
    }
    *value = byte != 0;
    return {};
  }
  if (name == "string") {
    if (encoded && !value->is_string()) {
      return Invalid("Expected a string");
    }
    uint32_t size = encoded ? value->get_ref<const std::string&>().size() : 0;
    Status status = Bytes(&size, sizeof(size), encoded, decoded);
    if (!status.ok()) {
      return status;
    }
    if (size > kPayloadLimit) {
      return {"payload_too_large", "String exceeds adapter limit"};
    }
    std::string text =
        encoded ? value->get<std::string>() : std::string(size, '\0');
    status = Bytes(text.data(), size, encoded, decoded);
    if (!encoded) {
      *value = text;
    }
    return status;
  }
  if (name == "vl_api_interface_index_t") {
    uint32_t index = 0;
    if (encoded) {
      if (!value->is_string()) {
        return Invalid("Use an interface name or @all, not a numeric index");
      }
      if (*value == "@all") {
        index = UINT32_MAX;
      } else {
        if (!symbols.interface_index) {
          return {"resource_not_found", "Interface resolver unavailable"};
        }
        Status status = symbols.interface_index(*value, &index);
        if (!status.ok()) {
          return status;
        }
      }
    }
    Status status = Bytes(&index, 4, encoded, decoded);
    if (!status.ok()) {
      return status;
    }
    if (!encoded) {
      if (index == UINT32_MAX) {
        *value = "@all";
      } else {
        std::string symbol;
        if (!symbols.interface_name) {
          return {"resource_not_found", "Interface resolver unavailable"};
        }
        status = symbols.interface_name(index, &symbol);
        if (!status.ok()) {
          return status;
        }
        *value = symbol;
      }
    }
    return {};
  }
  if (name == "vl_api_ip4_address_t" || name == "vl_api_ip6_address_t" ||
      name == "vl_api_mac_address_t") {
    std::size_t size = name == "vl_api_ip4_address_t"   ? 4
                       : name == "vl_api_ip6_address_t" ? 16
                                                        : 6;
    uint8_t bytes[16]{};
    if (encoded) {
      if (!value->is_string()) {
        return Invalid("Expected a textual address");
      }
      const std::string text = *value;
      if (size == 6) {
        if (text.size() != 17) {
          return Invalid("Invalid MAC address");
        }
        for (int i = 0; i < 6; ++i) {
          unsigned int byte = 0;
          auto parsed = std::from_chars(text.data() + i * 3,
                                        text.data() + i * 3 + 2, byte, 16);
          if (parsed.ec != std::errc() ||
              parsed.ptr != text.data() + i * 3 + 2 ||
              (i < 5 && text[i * 3 + 2] != ':')) {
            return Invalid("Invalid MAC address");
          }
          bytes[i] = byte;
        }
      } else if (inet_pton(size == 4 ? AF_INET : AF_INET6, text.c_str(),
                           bytes) != 1) {
        return Invalid("Invalid IP address");
      }
    }
    Status status = Bytes(bytes, size, encoded, decoded);
    if (!status.ok()) {
      return status;
    }
    if (!encoded) {
      if (size == 6) {
        const char kHex[] = "0123456789abcdef";
        std::string text;
        for (std::size_t i = 0; i < size; ++i) {
          if (i) {
            text += ':';
          }
          text += kHex[bytes[i] >> 4];
          text += kHex[bytes[i] & 15];
        }
        *value = text;
      } else {
        char text[INET6_ADDRSTRLEN];
        if (!inet_ntop(size == 4 ? AF_INET : AF_INET6, bytes, text,
                       sizeof(text))) {
          return {"invalid_reply", "Invalid IP address"};
        }
        *value = text;
      }
    }
    return {};
  }
  const Json* type = Type(name);
  if (!type) {
    return {"unsupported_type", "Unknown schema type: " + name};
  }
  std::string kind = (*type)["kind"];
  if (kind == "alias") {
    if (!type->contains("length")) {
      return Convert((*type)["type"], value, symbols, encoded, decoded, tag,
                     depth + 1);
    }
    std::size_t count = (*type)["length"];
    if (encoded && (!value->is_array() || value->size() != count)) {
      return Invalid("Wrong fixed array length");
    }
    if (!encoded) {
      *value = Json::array();
    }
    for (std::size_t i = 0; i < count; ++i) {
      Json item = encoded ? (*value)[i] : Json();
      Status status = Convert((*type)["type"], &item, symbols, encoded, decoded,
                              nullptr, depth + 1);
      if (!status.ok()) {
        return status;
      }
      if (!encoded) {
        value->push_back(item);
      }
    }
    return {};
  }
  if (kind == "enum") {
    const Json& values = (*type)["values"];
    bool flags = type->value("flags", false);
    uint64_t bits = 0;
    Json numeric;
    if (encoded) {
      if (flags) {
        if (!value->is_array()) {
          return Invalid("Flags must be an array of symbolic names");
        }
        std::set<std::string> seen;
        for (const auto& symbol : *value) {
          if (!symbol.is_string() ||
              !values.contains(symbol.get<std::string>()) ||
              !seen.insert(symbol.get<std::string>()).second) {
            return Invalid("Unknown or duplicate flag");
          }
          bits |= values[symbol.get<std::string>()].get<uint64_t>();
        }
        numeric = bits;
      } else {
        if (!value->is_string() ||
            !values.contains(value->get<std::string>())) {
          return Invalid("Unknown enum symbol");
        }
        numeric = values[value->get<std::string>()];
      }
      if ((*type)["type"] == "u64" || (*type)["type"] == "i64") {
        numeric = numeric.dump();
      }
    }
    Status status = Convert((*type)["type"], &numeric, symbols, encoded,
                            decoded, nullptr, depth + 1);
    if (!status.ok() || encoded) {
      return status;
    }
    if (numeric.is_string()) {
      const std::string text = numeric;
      auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), bits);
      if (parsed.ec != std::errc()) {
        return {"invalid_reply", "Invalid enum representation"};
      }
    } else {
      bits = numeric.get<uint64_t>();
    }
    if (flags) {
      *value = Json::array();
      uint64_t remaining = bits;
      for (auto it = values.begin(); it != values.end(); ++it) {
        uint64_t bit = it.value().get<uint64_t>();
        if (bit && (bit & (bit - 1)) == 0 && (bits & bit)) {
          value->push_back(it.key());
          remaining &= ~bit;
        }
      }
      if (remaining) {
        return {"unknown_enum", "VPP returned unknown flag bits"};
      }
    } else {
      for (auto it = values.begin(); it != values.end(); ++it) {
        if (it.value() == numeric || (it.value().is_number_unsigned() &&
                                      it.value().get<uint64_t>() == bits)) {
          *value = it.key();
          return {};
        }
      }
      return {"unknown_enum", "VPP returned an unknown enum value"};
    }
    return {};
  }
  bool address = name == "vl_api_address_t";
  bool prefix = name == "vl_api_prefix_t" || name == "vl_api_ip4_prefix_t" ||
                name == "vl_api_ip6_prefix_t";
  if (encoded && (address || prefix)) {
    if (!value->is_string()) {
      return Invalid("Expected textual IP address or prefix");
    }
    std::string text = *value;
    if (prefix) {
      auto slash = text.find_last_of('/');
      unsigned int length = 0;
      if (slash == std::string::npos) {
        return Invalid("Expected CIDR prefix");
      }
      auto parsed = std::from_chars(text.data() + slash + 1,
                                    text.data() + text.size(), length);
      std::string ip = text.substr(0, slash);
      if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
          length > (ip.find(':') == std::string::npos ? 32u : 128u)) {
        return Invalid("Invalid prefix length");
      }
      *value = {{"address", ip}, {"len", length}};
    } else {
      bool ipv6 = text.find(':') != std::string::npos;
      *value = {{"af", ipv6 ? "ADDRESS_IP6" : "ADDRESS_IP4"},
                {"un", {{ipv6 ? "ip6" : "ip4", text}}}};
    }
  }
  if (kind == "union") {
    int64_t size = FixedSize(name);
    if (size < 0 || Variable(name)) {
      return {"unsupported_type", "Variable union unsupported"};
    }
    const Json* member = nullptr;
    if (encoded) {
      if (!value->is_object() || value->size() != 1) {
        return Invalid("Union must select exactly one named member");
      }
      for (const auto& field : (*type)["fields"]) {
        if (value->contains(field["name"].get<std::string>())) {
          member = &field;
        }
      }
      if (!member) {
        return Invalid("Unknown union member");
      }
      if (tag && member->contains("case") && (*member)["case"] != *tag) {
        return Invalid("Union member disagrees with discriminator");
      }
      std::vector<uint8_t> bytes;
      Json item = (*value)[(*member)["name"].get<std::string>()];
      Status status = Convert((*member)["type"], &item, symbols, &bytes,
                              nullptr, nullptr, depth + 1);
      if (!status.ok()) {
        return status;
      }
      bytes.resize(size, 0);
      return Bytes(bytes.data(), bytes.size(), encoded, nullptr);
    }
    for (const auto& field : (*type)["fields"]) {
      if (tag && field.contains("case") && field["case"] == *tag) {
        member = &field;
      }
    }
    if (!member) {
      return {"unsupported_type",
              "Reply union needs an explicit discriminator"};
    }
    if (decoded->size() < static_cast<std::size_t>(size)) {
      return {"invalid_reply", "Truncated union"};
    }
    auto bytes = decoded->first(size);
    Json item;
    Status status = Convert((*member)["type"], &item, symbols, nullptr, &bytes,
                            nullptr, depth + 1);
    if (!status.ok()) {
      return status;
    }
    *decoded = decoded->subspan(size);
    *value = {{(*member)["name"].get<std::string>(), item}};
    return {};
  }
  if (encoded && !value->is_object()) {
    return Invalid("Expected an object for " + name);
  }
  Json object = encoded ? *value : Json::object();
  const Json& fields = (*type)["fields"];
  auto counts = Counts(fields);
  if (encoded) {
    for (auto it = object.begin(); it != object.end(); ++it) {
      bool known = false;
      for (const auto& field : fields) {
        if (field["name"] == it.key()) {
          known = true;
        }
      }
      if (!known || counts.contains(it.key())) {
        return Invalid("Unknown or derived field: " + it.key());
      }
    }
    for (const auto& field : fields) {
      if (field.contains("count")) {
        std::string field_name = field["name"];
        if (!object.contains(field_name) || !object[field_name].is_array()) {
          return Invalid("Expected array: " + field_name);
        }
        object[field["count"].get<std::string>()] = object[field_name].size();
      }
    }
  }
  for (const auto& field : fields) {
    std::string field_name = field["name"], field_type = field["type"];
    if (encoded && !object.contains(field_name)) {
      if (field.contains("default")) {
        object[field_name] = PublicDefault(field, *this);
      } else {
        return Invalid("Missing field: " + field_name);
      }
    }
    Json item = encoded ? object[field_name] : Json();
    Status status;
    if (field_type == "string" && field.value("length", 0) > 0) {
      std::size_t size = field["length"];
      if (size > kPayloadLimit) {
        return Invalid("String size exceeds limit");
      }
      if (encoded && (!item.is_string() ||
                      item.get_ref<const std::string&>().size() >= size ||
                      item.get_ref<const std::string&>().find('\0') !=
                          std::string::npos)) {
        return Invalid("Fixed string is too long or contains NUL");
      }
      std::string text(size, '\0');
      if (encoded) {
        text.replace(0, item.get_ref<const std::string&>().size(),
                     item.get<std::string>());
      }
      status = Bytes(text.data(), size, encoded, decoded);
      if (!encoded) {
        item = text.substr(0, text.find('\0'));
      }
    } else if (field_type != "string" && field.contains("length")) {
      uint64_t count = field["length"];
      if (field.contains("count")) {
        std::string counter = field["count"];
        if (!object.contains(counter) || !object[counter].is_number_integer()) {
          return {"unsupported_type", "Array count must precede its array"};
        }
        count = object[counter].get<uint64_t>();
      }
      if (count > kPayloadLimit ||
          (encoded && (!item.is_array() || item.size() != count))) {
        return Invalid("Invalid array length: " + field_name);
      }
      if (!encoded) {
        item = Json::array();
      }
      for (uint64_t i = 0; i < count; ++i) {
        Json element = encoded ? item[i] : Json();
        status = Convert(field_type, &element, symbols, encoded, decoded,
                         nullptr, depth + 1);
        if (!status.ok()) {
          break;
        }
        if (!encoded) {
          item.push_back(element);
        }
      }
    } else {
      std::string discriminator = field.value(
          "discriminator", field_type == "vl_api_address_union_t" ? "af" : "");
      const Json* selected =
          !discriminator.empty() && object.contains(discriminator)
              ? &object[discriminator]
              : nullptr;
      status = Convert(field_type, &item, symbols, encoded, decoded, selected,
                       depth + 1);
    }
    if (!status.ok()) {
      status.message = field_name + ": " + status.message;
      return status;
    }
    object[field_name] = item;
  }
  if (!encoded) {
    for (const auto& counter : counts) {
      object.erase(counter);
    }
    if (address) {
      *value = object["un"].begin().value();
    } else if (prefix) {
      *value =
          object["address"].get<std::string>() + "/" + object["len"].dump();
    } else {
      *value = object;
    }
  }
  return {};
}
Json Schema::Describe(const std::string& name, int depth) const {
  if (depth > 32) {
    return {{"unsupported", "recursive type"}};
  }
  if (name == "vl_api_interface_index_t") {
    return {{"type", "string"},
            {"format", "vpp-interface-name"},
            {"sentinels", {"@all"}}};
  }
  if (name == "u64" || name == "i64") {
    return {{"type", "string"},
            {"format", name},
            {"pattern", name == "u64" ? "^[0-9]+$" : "^-?[0-9]+$"}};
  }
  if (name == "bool") {
    return {{"type", "boolean"}};
  }
  if (name == "string") {
    return {{"type", "string"}};
  }
  if (name == "f32" || name == "f64") {
    return {{"type", "number"}};
  }
  if (name.size() >= 2 && (name[0] == 'u' || name[0] == 'i') &&
      FixedSize(name) > 0 && !Type(name)) {
    int bits = FixedSize(name) * 8;
    return {{"type", "integer"},
            {"minimum", name[0] == 'u' ? 0 : -(int64_t{1} << (bits - 1))},
            {"maximum", name[0] == 'u' ? (int64_t{1} << bits) - 1
                                       : (int64_t{1} << (bits - 1)) - 1}};
  }
  if (name == "vl_api_mac_address_t" || name == "vl_api_address_t" ||
      name == "vl_api_ip4_address_t" || name == "vl_api_ip6_address_t" ||
      name == "vl_api_prefix_t" || name == "vl_api_ip4_prefix_t" ||
      name == "vl_api_ip6_prefix_t") {
    return {{"type", "string"}, {"format", name}};
  }
  const Json* type = Type(name);
  if (!type) {
    return {{"unsupported", name}};
  }
  if ((*type)["kind"] == "alias") {
    Json result = Describe((*type)["type"], depth + 1);
    return type->contains("length") ? Json{{"type", "array"},
                                           {"items", result},
                                           {"minItems", (*type)["length"]},
                                           {"maxItems", (*type)["length"]}}
                                    : result;
  }
  if ((*type)["kind"] == "enum") {
    Json names = Json::array();
    for (auto it = (*type)["values"].begin(); it != (*type)["values"].end();
         ++it) {
      names.push_back(it.key());
    }
    Json symbols = {{"type", "string"}, {"enum", names}};
    return type->value("flags", false) ? Json{{"type", "array"},
                                              {"uniqueItems", true},
                                              {"items", symbols}}
                                       : symbols;
  }
  Json properties = Json::object(), required = Json::array();
  auto counts = Counts((*type)["fields"]);
  for (const auto& field : (*type)["fields"]) {
    std::string field_name = field["name"];
    if (counts.contains(field_name)) {
      continue;
    }
    Json item = Describe(field["type"], depth + 1);
    if (field["type"] == "string" && field.value("length", 0) > 0) {
      item["maxLength"] = field["length"].get<int>() - 1;
    } else if (field["type"] != "string" && field.contains("length")) {
      item = {{"type", "array"}, {"items", item}};
      if (!field.contains("count")) {
        item["minItems"] = field["length"];
        item["maxItems"] = field["length"];
      }
    }
    if (field.contains("default")) {
      item["default"] = PublicDefault(field, *this);
    } else {
      required.push_back(field_name);
    }
    properties[field_name] = item;
  }
  Json result = {{"type", "object"},
                 {"additionalProperties", false},
                 {"properties", properties}};
  if ((*type)["kind"] == "union") {
    result["minProperties"] = 1;
    result["maxProperties"] = 1;
  } else {
    result["required"] = required;
  }
  return result;
}
}  // namespace vpp_json
