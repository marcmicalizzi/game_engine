#include <core/base/assert.h>
#include <core/json/json.h>
#include <core/profiling/profile.h>
#include <domain/protocol/rpc.h>

#include <algorithm>
#include <cstring>

namespace engine::protocol {

namespace {

JsonValue error_object(const RpcError& error) {
  JsonValue e = JsonValue::object();
  e.set("code", JsonValue(error.code));
  e.set("message", JsonValue(error.message));
  if (!error.data.is_null()) e.set("data", error.data);
  return e;
}

JsonValue error_response(const JsonValue& id, const RpcError& error) {
  JsonValue r = JsonValue::object();
  r.set("jsonrpc", JsonValue("2.0"));
  r.set("id", id);
  r.set("error", error_object(error));
  return r;
}

JsonValue result_response(const JsonValue& id, JsonValue result) {
  JsonValue r = JsonValue::object();
  r.set("jsonrpc", JsonValue("2.0"));
  r.set("id", id);
  r.set("result", std::move(result));
  return r;
}

}  // namespace

RpcError make_error(i32 code, std::string message, JsonValue data) {
  RpcError e;
  e.code = code;
  e.message = std::move(message);
  e.data = std::move(data);
  return e;
}

// ---- Dispatcher ------------------------------------------------------------------------------

Dispatcher::Dispatcher(Context context) : context_(context) {
  context_.dispatcher = this;
}

void Dispatcher::add(const MethodDesc& method) {
  ENGINE_VERIFY(method.name != nullptr && method.handler != nullptr,
                "Dispatcher::add: method needs a name and a handler");
  ENGINE_VERIFY(find(method.name) == nullptr, "Dispatcher::add: duplicate method name");
  auto pos = std::lower_bound(
      methods_.begin(), methods_.end(), method,
      [](const MethodDesc& a, const MethodDesc& b) { return std::strcmp(a.name, b.name) < 0; });
  methods_.insert(pos, method);
}

const MethodDesc* Dispatcher::find(std::string_view name) const noexcept {
  for (const MethodDesc& m : methods_) {
    if (name == m.name) return &m;
  }
  return nullptr;
}

JsonValue Dispatcher::dispatch(const JsonValue& request) {
  if (request.is_array()) {
    if (request.size() == 0) {
      return error_response(JsonValue(), make_error(codes::k_invalid_request, "empty batch"));
    }
    JsonValue out = JsonValue::array();
    for (usize i = 0; i < request.size(); ++i) {
      JsonValue r = dispatch_one(request[i]);
      if (!r.is_null()) out.push_back(std::move(r));
    }
    return out.size() == 0 ? JsonValue() : out;
  }
  return dispatch_one(request);
}

JsonValue Dispatcher::dispatch_one(const JsonValue& request) {
  ENGINE_PROFILE_ZONE_NAMED("rpc.dispatch");
  if (!request.is_object()) {
    return error_response(JsonValue(),
                          make_error(codes::k_invalid_request, "request must be an object"));
  }
  const JsonValue* id_value = request.find("id");
  const bool is_notification = id_value == nullptr;
  const JsonValue id = id_value != nullptr ? *id_value : JsonValue();
  if (!id.is_null() && !id.is_string() && !id.is_number()) {
    return error_response(JsonValue(),
                          make_error(codes::k_invalid_request, "id must be a string or number"));
  }

  const JsonValue* version = request.find("jsonrpc");
  if (version == nullptr || !version->is_string() || version->as_string() != "2.0") {
    return is_notification ? JsonValue()
                           : error_response(id, make_error(codes::k_invalid_request,
                                                           "jsonrpc must be \"2.0\""));
  }
  const JsonValue* method_name = request.find("method");
  if (method_name == nullptr || !method_name->is_string()) {
    return is_notification ? JsonValue()
                           : error_response(id, make_error(codes::k_invalid_request,
                                                           "method must be a string"));
  }
  const MethodDesc* method = find(method_name->as_string());
  if (method == nullptr) {
    if (is_notification) return JsonValue();
    std::string message = "unknown method '";
    message.append(method_name->as_string());
    message.push_back('\'');
    return error_response(id, make_error(codes::k_method_not_found, std::move(message)));
  }

  const JsonValue* params = request.find("params");
  static const JsonValue k_no_params;
  const JsonValue& p = params != nullptr ? *params : k_no_params;
  if (!p.is_null() && !p.is_object() && !p.is_array()) {
    return is_notification ? JsonValue()
                           : error_response(id, make_error(codes::k_invalid_params,
                                                           "params must be an object"));
  }

  JsonValue result;
  RpcError error;
  const bool ok = method->handler(context_, p, result, error);
  if (is_notification) return JsonValue();
  return ok ? result_response(id, std::move(result)) : error_response(id, error);
}

std::string Dispatcher::dispatch_text(std::string_view request_text) {
  JsonValue request;
  const JsonParseResult parsed = parse_json(request_text, request);
  JsonValue response;
  if (!parsed.ok) {
    std::string message = "parse error: ";
    message.append(parsed.message);
    response = error_response(JsonValue(), make_error(codes::k_parse_error, std::move(message)));
  } else {
    response = dispatch(request);
  }
  if (response.is_null()) return {};
  return write_json(response, JsonWriteOptions{.pretty = false});
}

// ---- typed adapters --------------------------------------------------------------------------

namespace detail {

bool read_params(const schema::TypeInfo& type, void* object, const JsonValue& params,
                 RpcError& error) {
  if (params.is_null()) return true;
  if (!params.is_object()) {
    error = make_error(codes::k_invalid_params, "params must be an object");
    return false;
  }
  schema::ReadContext ctx;
  if (schema::from_json(type, object, params, ctx)) return true;
  JsonValue data = JsonValue::array();
  for (const auto& d : ctx.diagnostics) {
    JsonValue item = JsonValue::object();
    item.set("path", JsonValue(d.path));
    item.set("message", JsonValue(d.message));
    data.push_back(std::move(item));
  }
  std::string message = "invalid params for ";
  message.append(type.qualified_name);
  if (!ctx.diagnostics.empty()) {
    message.append(": ");
    if (!ctx.diagnostics[0].path.empty()) {
      message.append(ctx.diagnostics[0].path);
      message.append(": ");
    }
    message.append(ctx.diagnostics[0].message);
  }
  error = make_error(codes::k_invalid_params, std::move(message), std::move(data));
  return false;
}

}  // namespace detail

}  // namespace engine::protocol
