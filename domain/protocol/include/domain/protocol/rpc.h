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

#include <mutex>
#include <schemas/protocol.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::log {
class RingSink;
}
namespace engine::io {
class Roots;
}

namespace engine::protocol {

class SessionManager;
class Dispatcher;
class Policy;
class Subscriptions;
class Clients;

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
// Plan 06 §6.5 (policy.h, leases.h). The call's role forbids the method, the layer, a tile or an
// object type it touches; or it writes a proposal layer it does not own, or one that is closed; or
// a role that needs review promotes. `data.reason` says which, with what was refused.
inline constexpr i32 k_forbidden = 1008;
// A lease another actor holds stands in the way: `lease.acquire` overlaps it, or an edit touches a
// record it covers. `data.lease` is the holder's lease, so the caller knows whom to wait for.
inline constexpr i32 k_lease_conflict = 1009;
// The document requires leases and the caller holds none covering a record the call touches:
// acquire one (`lease.acquire`) and call again. `data` names the layer, the object and its tile.
inline constexpr i32 k_lease_required = 1010;
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
  // The host's roles and default identity (policy.h, plan 06 §6.5). Null restricts nothing and
  // attributes nothing, which is every host before roles existed and every in-process caller that
  // does not ask for them.
  const Policy* policy = nullptr;
  // The host's subscriptions (events.h), when it serves a socket; null otherwise. A handler that
  // knows a change happened publishes through it (engine-host's `render.load {session}`).
  Subscriptions* events = nullptr;
  // The socket connection the request being dispatched came on, which `session.subscribe` names
  // its subscriber by; 0 for stdio and every in-process caller, which have no channel to push on.
  // The listener sets it under the dispatch lock for each request and puts 0 back after.
  u64 connection = 0;
  // The host's client connections (clients.h), when it serves a socket; null otherwise. Who each
  // connection said it is (`client.hello`) fills the blank attribution of its calls before the
  // host's identity does (policy.h, `resolve_caller`), and the dispatcher's role gate reads it too.
  Clients* clients = nullptr;
  // The host's file roots (paths.h, foundation/io/roots.h; protocol.md, "The host's file roots"):
  // every path a client names is admitted against them. Null admits nothing, so a context made
  // without roots fails closed.
  io::Roots* roots = nullptr;
  // The dispatch lock the caller holds for the request being dispatched, when it holds one: the
  // listener and engine-host's stdio loop set it under that lock for each request and put null back
  // after, as they do `connection`. Null for every caller that holds none (an in-process
  // dispatcher, a test, the console), which is why a handler never reads it itself and lets the
  // lock go only through `without_dispatch_lock` below (protocol.md, "Work outside the dispatch
  // lock").
  std::mutex* dispatch_lock = nullptr;
};

// Runs `work()` with the request's dispatch lock let go, for a handler whose work reads nothing of
// the host's state while it runs: everything it needs is copied out of the host first and
// everything it makes is its own, so other clients' requests and the world's ticks go on beside it
// (store.query's statement is the one user). The context's per-request fields are cleared before
// the lock is let go, so the context is as it stands between requests — the stdio loop does not set
// `connection` and reads 0 there, and would otherwise run as this request's connection — and are
// put back once it is taken again, before this returns: a later call of the same batch reads
// `connection` for its caller. With no lock held,
// `work()` simply runs. Nothing a handler looked up under the lock (a session, a document, a
// world) may be touched after this returns without looking it up again: it may be gone.
template <class Work>
void without_dispatch_lock(Context& ctx, Work&& work) {
  std::mutex* const lock = ctx.dispatch_lock;
  if (lock == nullptr) {
    work();
    return;
  }
  const u64 connection = ctx.connection;
  ctx.connection = 0;
  ctx.dispatch_lock = nullptr;
  lock->unlock();
  work();  // exceptions are disabled in engine targets, so the lock is always taken back
  lock->lock();
  ctx.connection = connection;
  ctx.dispatch_lock = lock;
}

using Handler = bool (*)(Context& ctx, const JsonValue& params, JsonValue& result, RpcError& error);

// Whether one call of a read leaves a file on the host's disk, from its params alone and before its
// handler runs (protocol.md, "What a read may do").
using LeavesFiles = bool (*)(const JsonValue& params);

struct MethodDesc {
  const char* name;
  const char* doc;
  const char* params_type;  // qualified schema type, or "" for none
  const char* result_type;
  Handler handler;
  // Changes no document, no lease, no host setting, not the running world and not what is on a
  // person's screen, so no role restricts it (plan 06 §6.5: read access is unrestricted). The
  // default is a write on purpose: a method nobody classified is refused to a role that did not
  // name it — loud — rather than open to every role — silent.
  bool read_only = false;
  // A read that leaves a file the caller asked for (a picture, a trace, a save, a created document,
  // derived data): null when it never does, else whether this call does. Such a call is refused to
  // a credential that may only read — one with no role, or a role the host cannot enforce — since
  // that state means nothing is written on its behalf (ADR-0069); an enforced role keeps it as a
  // read, confined by the host's roots. Housekeeping a call did not choose (a lapsed lease or claim
  // dropped, a lock file made) does not count: any caller's next call writes the same bytes.
  LeavesFiles leaves_files = nullptr;
};

// `method` marked read-only: `d.add(read_only(method<P, R, &fn>(...)))`.
inline MethodDesc read_only(MethodDesc method) noexcept {
  method.read_only = true;
  return method;
}

// Every call leaves a file.
inline bool always_leaves_files(const JsonValue&) noexcept { return true; }

// For a `LeavesFiles` that depends on one field: whether `params` sets `field` true, or names
// something in it (a string that is not empty). Anything else is a call that names no file; its
// params are the handler's to refuse.
inline bool param_is_true(const JsonValue& params, std::string_view field) noexcept {
  const JsonValue* v = params.is_object() ? params.find(field) : nullptr;
  bool on = false;
  return v != nullptr && v->get_bool(on) && on;
}
inline bool param_is_named(const JsonValue& params, std::string_view field) noexcept {
  const JsonValue* v = params.is_object() ? params.find(field) : nullptr;
  std::string_view s;
  return v != nullptr && v->get_string(s) && !s.empty();
}

// `method` marked as a read that leaves a file: every call, or the calls `when` says.
inline MethodDesc read_leaving_files(MethodDesc method,
                                     LeavesFiles when = &always_leaves_files) noexcept {
  method.read_only = true;
  method.leaves_files = when;
  return method;
}

// Whether a call of `method` with `params` would leave a file (false for a write: a write is gated
// by its role's list instead).
inline bool call_leaves_files(const MethodDesc& method, const JsonValue& params) noexcept {
  return method.read_only && method.leaves_files != nullptr && method.leaves_files(params);
}

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
  return MethodDesc{name,
                    doc,
                    schema::type_of<P>().qualified_name,
                    schema::type_of<R>().qualified_name,
                    &typed_handler<P, R, Fn>,
                    false,
                    nullptr};
}

template <class R, bool (*Fn)(Context&, R&, RpcError&)>
MethodDesc method_no_params(const char* name, const char* doc) {
  return MethodDesc{
      name,  doc,    "", schema::type_of<R>().qualified_name, &typed_result_handler<R, Fn>,
      false, nullptr};
}

}  // namespace engine::protocol
