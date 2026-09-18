#pragma once

// The engine protocol (docs/plan/06-agent-tooling.md §6.2, ADR-0001): JSON-RPC 2.0 over JSON
// values, transport-agnostic. A host feeds one request (or a batch array) to a Dispatcher and
// writes the response it gets back; stdio, sockets, and in-process callers share the code.
//
// Methods are registered with their parameter and result schema types, so the catalogue that
// `engine.methods` returns, the JSON Schema schemac emits, and the C++ handlers agree by
// construction. Handlers are plain functions taking typed params and filling typed results;
// the `method<P, R, Fn>()` adapter does the JSON work and turns deserialization problems into
// an InvalidParams error carrying {path, message} diagnostics.
//
// Error codes: the JSON-RPC standard ones are negative; engine codes are positive and listed
// in `codes`.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>

#include <schemas/protocol.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::log {
class RingSink;
}

namespace engine::protocol {

class SessionManager;
class Dispatcher;

namespace codes {
inline constexpr i32 k_parse_error = -32700;
inline constexpr i32 k_invalid_request = -32600;
inline constexpr i32 k_method_not_found = -32601;
inline constexpr i32 k_invalid_params = -32602;
inline constexpr i32 k_internal_error = -32603;
inline constexpr i32 k_session_not_found = 1000;
inline constexpr i32 k_document_error = 1001;
inline constexpr i32 k_validation_failed = 1002;
inline constexpr i32 k_not_found = 1003;
inline constexpr i32 k_invalid_argument = 1004;
inline constexpr i32 k_io_error = 1005;
inline constexpr i32 k_unavailable = 1006;
// A `render.*` method on a process that has no usable GPU: no Vulkan loader or driver, no
// 64-bit buffer atomics, or no cluster acceleration structures for the ray path. It is a code
// of its own rather than k_unavailable so a client can tell "this machine cannot render" from
// "this host has no log ring" and from "you asked for something wrong" — which is what lets one
// end-to-end suite run on a hosted CI runner and on a GPU machine, skipping on the first.
inline constexpr i32 k_render_unavailable = 1007;
}  // namespace codes

RpcError make_error(i32 code, std::string message, JsonValue data = {});

// What every handler sees.
struct Context {
  SessionManager* sessions = nullptr;
  log::RingSink* log_ring = nullptr;  // for log.tail; may be null
  Dispatcher* dispatcher = nullptr;   // set by the Dispatcher for engine.methods
  // The registration point an app adds its own methods' state through (ADR-0027: attachment
  // only through registration points, and the protocol's method table is one of them). A
  // handler is a plain function pointer and cannot capture, and the protocol layer must not
  // learn about anything above it — engine-host's renderer sessions live in `systems`, two
  // layers up — so the app owns the object, registers the methods that know its type, and
  // casts it back. The protocol never touches it, and nothing in domain/ may read it.
  void* app = nullptr;
};

using Handler = bool (*)(Context& ctx, const JsonValue& params, JsonValue& result, RpcError& error);

struct MethodDesc {
  const char* name;
  const char* doc;
  const char* params_type;  // qualified schema type, or "" for none
  const char* result_type;
  Handler handler;
};

class Dispatcher {
 public:
  explicit Dispatcher(Context context);

  // Names must be unique; methods are kept sorted by name.
  void add(const MethodDesc& method);
  const MethodDesc* find(std::string_view name) const noexcept;
  std::span<const MethodDesc> methods() const noexcept {
    return {methods_.data(), methods_.size()};
  }
  Context& context() noexcept { return context_; }

  // One request object or a batch array. Returns the response value, or a null JsonValue when
  // nothing is to be sent (a notification, or a batch of notifications).
  JsonValue dispatch(const JsonValue& request);
  // One line of JSON in, one line out (empty when nothing is to be sent). Parse errors become
  // JSON-RPC ParseError responses.
  std::string dispatch_text(std::string_view request_text);

 private:
  JsonValue dispatch_one(const JsonValue& request);

  Context context_;
  Vector<MethodDesc> methods_;
};

// Registers engine.*, session.*, doc.*, tunables.*, log.*, and schema.* methods.
void add_builtin_methods(Dispatcher& dispatcher);

// ---- typed handler adapters ----------------------------------------------------------------

namespace detail {
// Null or absent params leave `object` at its defaults; an object is deserialized; anything
// else, or a deserialization problem, is an InvalidParams error with diagnostics in `data`.
bool read_params(const schema::TypeInfo& type, void* object, const JsonValue& params,
                 RpcError& error);
}  // namespace detail

template <class P, class R, bool (*Fn)(Context&, const P&, R&, RpcError&)>
bool typed_handler(Context& ctx, const JsonValue& params, JsonValue& result, RpcError& error) {
  P p{};
  if (!detail::read_params(schema::type_of<P>(), &p, params, error)) return false;
  R r{};
  if (!Fn(ctx, p, r, error)) return false;
  result = schema::to_json(r);
  return true;
}

template <class R, bool (*Fn)(Context&, R&, RpcError&)>
bool typed_result_handler(Context& ctx, const JsonValue&, JsonValue& result, RpcError& error) {
  R r{};
  if (!Fn(ctx, r, error)) return false;
  result = schema::to_json(r);
  return true;
}

template <class P, class R, bool (*Fn)(Context&, const P&, R&, RpcError&)>
MethodDesc method(const char* name, const char* doc) {
  return MethodDesc{name, doc, schema::type_of<P>().qualified_name,
                    schema::type_of<R>().qualified_name, &typed_handler<P, R, Fn>};
}

template <class R, bool (*Fn)(Context&, R&, RpcError&)>
MethodDesc method_no_params(const char* name, const char* doc) {
  return MethodDesc{name, doc, "", schema::type_of<R>().qualified_name,
                    &typed_result_handler<R, Fn>};
}

}  // namespace engine::protocol
