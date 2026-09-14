#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "src/schema.h"

namespace {
using vpp_json::Json;
using vpp_json::Schema;
// Untagged unions and variable replies need separate response-length handling.
bool Supported(const Schema& schema, const std::string& name,
               bool tagged = false, int depth = 0) {
  if (depth > 32 || schema.FixedSize(name) < 0) {
    return false;
  }
  const Json* type = schema.Type(name);
  if (!type) {
    return true;
  }
  if ((*type)["kind"] == "alias") {
    return Supported(schema, (*type)["type"], false, depth + 1);
  }
  if ((*type)["kind"] == "union" && !tagged) {
    return false;
  }
  for (const auto& field : type->value("fields", Json::array())) {
    if ((*type)["kind"] == "union" && field.contains("length")) {
      return false;
    }
    bool discriminator = field.contains("discriminator") ||
                         field["type"] == "vl_api_address_union_t";
    if (!Supported(schema, field["type"], discriminator, depth + 1)) {
      return false;
    }
  }
  return true;
}
}  // namespace
int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: vpp-schema-generator OUTPUT INPUT.api.json ...\n";
    return 2;
  }
  Schema schema;
  std::set<std::string> modules;
  for (int i = 2; i < argc; ++i) {
    std::string path = argv[i];
    auto slash = path.find_last_of('/');
    std::string module =
        path.substr(slash == std::string::npos ? 0 : slash + 1);
    auto suffix = module.find(".api.json");
    if (suffix == std::string::npos) {
      return 2;
    }
    module.resize(suffix);
    std::ifstream input(path);
    Json document = Json::parse(input, nullptr, false);
    auto status = schema.AddModule(document, module);
    if (!status.ok()) {
      std::cerr << status.message << '\n';
      return 1;
    }
    modules.insert(module);
  }
  std::ofstream out(argv[1]);
  out << "// Generated from VPP API schemas. Do not edit.\n#include "
         "<cstring>\n#include <cstddef>\n#include \"src/generated_api.h\"\n";
  for (const auto& module : modules) {
    out << "#include <vapi/" << module << ".api.vapi.h>\n";
  }
  for (auto module : modules) {
    for (char& c : module) {
      if (c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
      }
    }
    out << "DEFINE_VAPI_MSG_IDS_" << module << "_API_JSON;\n";
  }
  out << "namespace vpp_json {\nnamespace {\n";
  Json catalog = schema.catalog();
  std::vector<std::string> methods;
  for (auto it = catalog["methods"].begin(); it != catalog["methods"].end();
       ++it) {
    Json& method = it.value();
    std::string request = method["request"], reply = method.value("reply", "");
    std::string reason;
    if (method.contains("events")) {
      reason = "Event subscriptions require a subscription adapter";
    } else if (method.contains("stream_msg")) {
      reason = "Explicit streaming completion requires a dedicated adapter";
    } else if (!schema.Type(reply) || !Supported(schema, request) ||
               !Supported(schema, reply)) {
      reason = "Unsupported or ambiguous payload layout";
    } else if (schema.Variable(reply)) {
      reason = "Variable reply length requires a dedicated adapter";
    }
    method["supported"] = reason.empty();
    if (!reason.empty()) {
      method["reason"] = reason;
      continue;
    }
    methods.push_back(request);
    bool stream = method.value("stream", false);
    bool payload = !(*schema.Type(request))["fields"].empty();
    out << "bool Available_" << request
        << "(vapi_ctx_t ctx) { return vapi_is_msg_available(ctx, vapi_msg_id_"
        << request << ") && vapi_is_msg_available(ctx, vapi_msg_id_" << reply
        << "); }\n";
    out << "vapi_error_e Reply_" << request
        << "(vapi_ctx_t, void* opaque, vapi_error_e rc, bool last, "
           "vapi_payload_"
        << reply
        << "* reply) {\n auto* state = static_cast<GeneratedCall*>(opaque);\n "
           "if (rc != VAPI_OK) { state->error = rc; state->done = true; return "
           "VAPI_OK; }\n if (reply) {\n static_assert(sizeof(*reply) == "
        << schema.FixedSize(reply)
        << ", \"Schema/native reply layout mismatch\");\n const auto* data = "
           "reinterpret_cast<const uint8_t*>(reply);\n";
    for (const auto& field : (*schema.Type(reply))["fields"]) {
      if (field["name"] == "retval" && field["type"] == "i32") {
        out << " state->retval = reply->retval;\n";
      }
    }
    if (stream) {
      out << " state->item({data, sizeof(*reply)});\n";
    } else {
      out << " state->reply.assign(data, data + sizeof(*reply));\n";
    }
    out << " }\n if (last) state->done = true;\n return VAPI_OK;\n}\n";
    out << "vapi_error_e Send_" << request
        << "(vapi_ctx_t ctx, std::span<const uint8_t> bytes, GeneratedCall* "
           "state) {\n using Message = vapi_msg_"
        << request << ";\n";
    if (payload) {
      out << " constexpr size_t header = offsetof(Message, payload);\n "
             "static_assert(sizeof(((Message*)nullptr)->payload) == "
          << schema.FixedSize(request)
          << ", \"Schema/native request layout mismatch\");\n";
    } else {
      out << " constexpr size_t header = sizeof(Message);\n";
    }
    out << " auto* msg = static_cast<Message*>(vapi_msg_alloc(ctx, header + "
           "bytes.size()));\n if (!msg) return VAPI_ENOMEM;\n "
           "msg->header.client_index = vapi_get_client_index(ctx);\n "
           "msg->header.context = 0;\n msg->header._vl_msg_id = "
           "vapi_lookup_vl_msg_id(ctx, vapi_msg_id_"
        << request
        << ");\n if (!bytes.empty()) "
           "std::memcpy(reinterpret_cast<uint8_t*>(msg) + header, "
           "bytes.data(), bytes.size());\n auto rc = vapi_"
        << request << "(ctx, msg, Reply_" << request
        << ", state);\n if (rc != VAPI_OK) vapi_msg_free(ctx, msg);\n return "
           "rc;\n}\n";
  }
  out << "}  // namespace\nconst std::vector<GeneratedBinding>& "
         "GeneratedBindings() {\n static const std::vector<GeneratedBinding> "
         "bindings = {\n";
  for (const auto& name : methods) {
    out << "{\"vpp." << name << "\", Available_" << name << ", Send_" << name
        << "},\n";
  }
  out << "};\n return bindings;\n}\nSchema GeneratedSchema() {\n Schema "
         "schema;\n schema.Load(Json::parse("
      << Json(catalog.dump()).dump()
      << "));\n return schema;\n}\n}  // namespace vpp_json\n";
  return out.good() ? 0 : 1;
}
