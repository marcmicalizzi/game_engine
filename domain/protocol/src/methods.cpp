// The day-one method set (docs/plan/06-agent-tooling.md §6.9): sessions, document editing and
// introspection, tunables, the log ring, and schema discovery.
#include <core/base/macros.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <core/time/time.h>
#include <domain/gfx/adapter.h>
#include <domain/gfx/display_probe.h>
#include <domain/protocol/clients.h>
#include <domain/protocol/events.h>
#include <domain/protocol/paths.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>

#if ENGINE_PLATFORM_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#endif

#ifndef ENGINE_VERSION
#define ENGINE_VERSION "0.0.0"
#endif

namespace engine::protocol {

namespace {

// ---- engine.* --------------------------------------------------------------------------------

bool engine_info(Context& ctx, EngineInfo& out, RpcError&) {
  out.name = "game_engine";
  out.version = ENGINE_VERSION;
  out.build = ENGINE_DEBUG ? "debug" : "release";
#if ENGINE_PLATFORM_WINDOWS
  out.pid = static_cast<u32>(::_getpid());
#else
  out.pid = static_cast<u32>(::getpid());
#endif
  out.schema_types = static_cast<u32>(schema::Registry::global().all().size());
  out.methods = ctx.dispatcher != nullptr ? static_cast<u32>(ctx.dispatcher->methods().size()) : 0;
  return true;
}

bool engine_methods(Context& ctx, MethodsResult& out, RpcError&) {
  if (ctx.dispatcher == nullptr) return true;
  for (const MethodDesc& m : ctx.dispatcher->methods()) {
    MethodInfo info;
    info.name = m.name;
    info.doc = m.doc != nullptr ? m.doc : "";
    info.params_type = m.params_type != nullptr ? m.params_type : "";
    info.result_type = m.result_type != nullptr ? m.result_type : "";
    info.read_only = m.read_only;
    info.leaves_files = m.read_only && m.leaves_files != nullptr;
    out.methods.push_back(std::move(info));
  }
  return true;
}

// ---- session.* -------------------------------------------------------------------------------

// What a client is told of a session: its directory as `<root>://<relative>`, never the host's
// absolute path (protocol.md, "The host's file roots").
SessionInfo client_info(const Context& ctx, const Session& s) {
  SessionInfo info = s.info();
  info.path = portable_path(ctx, s.dir());
  return info;
}

bool session_open(Context& ctx, const SessionOpenParams& params, SessionInfo& out,
                  RpcError& error) {
  if (params.path.empty()) {
    error = make_error(codes::k_invalid_argument, "path is required");
    return false;
  }
  // The directory is a client's path (roots.h): read when a document is there, made and written
  // when `create` makes one. A relative path starts at the host's own document.
  io::Admitted dir;
  if (!admit_client_path(ctx, "path", params.path, client_base(ctx, {}), io::Access::read, dir,
                         error)) {
    return false;
  }
  if (params.create && !doc::DocumentStore::exists(ctx.sessions->vfs(), dir.native)) {
    if (!admit_client_path(ctx, "path", params.path, client_base(ctx, {}),
                           io::Access::make_directory, dir, error)) {
      return false;
    }
    // Not where the host keeps something else: a document made at the host's out/, a root's own
    // directory or around an open document would be protected, and close it to every client write
    // (roots.h, `may_make_document`). Refused as any place a client may not write is.
    if (!ctx.roots->may_make_document(dir.native)) {
      error = path_not_found("path", params.path, io::Access::make_directory);
      return false;
    }
  }
  SessionOpenParams opened = params;
  opened.path = dir.native;
  // Read is not write: a document opened from a read-only root is never saved in place
  // (Session::writable). Asked of the roots before the session protects the directory, and
  // without a log line, since nothing is refused yet; a document already open is shared as it is.
  io::Admitted write_probe;
  io::Refusal write_why = io::Refusal::outside;
  const bool may_write = ctx.roots->admit(params.path, client_base(ctx, {}),
                                          io::Access::make_directory, write_probe, write_why);
  // Held by the connection that asked (0: stdio and in-process callers), so a client that
  // vanishes lets go of what it opened and a close lets go of the caller's hold alone. An open
  // document's own files are written through doc.* alone, never by a client's path: the guard
  // protects the directory while the session is open and frees it when the session closes, by a
  // close or a dropped connection alike.
  const DocumentGuard guard = document_guard(*ctx.roots);
  Session* s = ctx.sessions->open(opened, ctx.connection, error,
                                  may_write ? std::string_view{} : std::string_view(params.path),
                                  portable_path(ctx, dir.native), &guard);
  if (s == nullptr) {
    // "no document at <path>" named the host's path; say only what the client sent.
    if (error.code == codes::k_not_found)
      error = path_not_found("path", params.path, io::Access::read);
    return false;
  }
  out = client_info(ctx, *s);
  return true;
}

// One client closing a session must not close it under the others (protocol.md, "Several clients,
// one host"): a close lets go of the caller's hold, and the session closes with its last holder.
bool session_close(Context& ctx, const SessionRef& params, SessionCloseResult& out,
                   RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  bool closed = false;
  if (!ctx.sessions->release(params.session, ctx.connection, closed)) {
    std::string holders;
    JsonValue list = JsonValue::array();
    for (const u64 h : s->holders()) {
      holders += (holders.empty() ? "" : ", ") + std::to_string(h);
      list.push_back(JsonValue(h));
    }
    JsonValue data = JsonValue::object();
    data.set("holders", std::move(list));
    error = make_error(codes::k_invalid_argument,
                       "session " + params.session + " is not held by " +
                           (ctx.connection == 0 ? std::string("stdio")
                                                : "connection " + std::to_string(ctx.connection)) +
                           ", which did not open it; it is held by " + holders +
                           " (0 is the host's own stdio), and closes when they let go",
                       std::move(data));
    return false;
  }
  out.session = params.session;
  out.closed = closed;  // and its directory freed by its guard (SessionManager::close)
  if (!closed) {
    for (const u64 h : s->holders())
      out.holders.push_back(h);
  }
  return true;
}

bool session_list(Context& ctx, SessionsResult& out, RpcError&) {
  for (const auto& s : ctx.sessions->sessions())
    out.sessions.push_back(client_info(ctx, *s));
  return true;
}

bool session_info(Context& ctx, const SessionRef& params, SessionInfo& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  out = client_info(ctx, *s);
  return true;
}

// session.subscribe / session.unsubscribe (events.h): the kinds named, checked, as a mask. Empty
// is every kind.
bool subscription_of(Context& ctx, const SubscribeParams& params, std::string_view method,
                     u32& mask, RpcError& error) {
  if (ctx.events == nullptr || ctx.connection == 0) {
    error =
        make_error(codes::k_unavailable,
                   std::string(method) +
                       " needs a socket connection (engine-host --listen, engine-editor): "
                       "events are pushed on the subscriber's own connection, and stdio has "
                       "no channel to push them on. Poll session.info's journal_length instead");
    return false;
  }
  if (!params.session.empty() && ctx.sessions->require(params.session, error) == nullptr) {
    return false;
  }
  mask = 0;
  for (const std::string& name : params.kinds) {
    EventKind kind = EventKind::Journal;
    if (!event_kind_from(name, kind)) {
      error = make_error(
          codes::k_invalid_argument,
          "no event kind '" + name + "': the kinds are journal, selection, scene and session");
      return false;
    }
    mask |= event_bit(kind);
  }
  if (params.kinds.empty()) mask = k_all_event_kinds;
  return true;
}

void kind_names(u32 mask, Vector<std::string>& out) {
  for (u32 i = 0; i < k_event_kind_count; ++i) {
    if ((mask & (1u << i)) != 0) out.push_back(event_kind_name(static_cast<EventKind>(i)));
  }
}

void fill_subscription(const Context& ctx, const SubscribeParams& params, u32 mask,
                       SubscribeResult& out) {
  out.session = params.session;
  kind_names(mask, out.kinds);
  out.queue = Subscriptions::queue_bound();
  out.connection = ctx.connection;
}

bool session_subscriptions(Context& ctx, SubscriptionsResult& out, RpcError&) {
  out.queue = Subscriptions::queue_bound();
  if (ctx.events == nullptr) return true;
  Vector<Subscriptions::Snapshot> snapshot;
  ctx.events->snapshot(snapshot);
  for (const Subscriptions::Snapshot& s : snapshot) {
    SubscriberInfo info;
    info.connection = s.connection;
    info.session = s.session;
    kind_names(s.kinds, info.kinds);
    info.dropped = s.dropped;
    out.subscribers.push_back(std::move(info));
  }
  return true;
}

bool session_subscribe(Context& ctx, const SubscribeParams& params, SubscribeResult& out,
                       RpcError& error) {
  u32 mask = 0;
  if (!subscription_of(ctx, params, "session.subscribe", mask, error)) return false;
  fill_subscription(ctx, params, ctx.events->subscribe(ctx.connection, params.session, mask), out);
  return true;
}

bool session_unsubscribe(Context& ctx, const SubscribeParams& params, SubscribeResult& out,
                         RpcError& error) {
  u32 mask = 0;
  if (!subscription_of(ctx, params, "session.unsubscribe", mask, error)) return false;
  fill_subscription(ctx, params, ctx.events->unsubscribe(ctx.connection, params.session, mask),
                    out);
  return true;
}

// ---- client.* (clients.h) ----------------------------------------------------------------------

void fill_client(const Context& ctx, const Client& c, ClientInfo& out) {
  out.connection = c.connection;
  out.name = c.name;
  out.actor = c.who.actor;
  out.role = c.who.role;
  out.task = c.who.task;
  out.identified = c.identified;
  out.connected_unix_ms = c.connected_unix_ms;
  out.requests = c.requests;
  out.credential = c.credential;
  out.bound = c.bound;
  if (ctx.sessions == nullptr) return;
  for (const auto& s : ctx.sessions->sessions()) {
    if (s->held_by(c.connection)) out.sessions.push_back(s->id());
    for (const LeaseInfo& l : s->leases().leases()) {
      if (l.connection == c.connection) out.leases.push_back(s->id() + ":" + std::to_string(l.id));
    }
  }
}

bool client_hello(Context& ctx, const ClientHelloParams& params, ClientInfo& out, RpcError& error) {
  if (ctx.clients == nullptr || ctx.connection == 0) {
    error = make_error(codes::k_unavailable,
                       "client.hello names a socket connection (engine-host --listen, "
                       "engine-editor); on stdio the caller is the host's own --actor, --role and "
                       "--task, or what each call's attribution says");
    return false;
  }
  if (params.name.size() > k_client_name_max) {
    error = make_error(codes::k_invalid_argument,
                       "a client's name is at most " + std::to_string(k_client_name_max) +
                           " characters: it is a label for client.list, not a description");
    return false;
  }
  // A role the roles file does not have is refused once, here, rather than at every call that
  // would inherit it.
  if (ctx.policy != nullptr && !params.attribution.role.empty()) {
    const Role* role = nullptr;
    if (!ctx.policy->resolve_role(params.attribution.role, role, error)) return false;
  }
  Client& c = ctx.clients->touch(ctx.connection, time::wall_unix_ms());
  c.name = params.name;
  // A connection bound to a credential keeps its credential's identity (ADR-0069): the dispatcher
  // already refused a hello naming another, and one naming the same, or none, changes nothing.
  if (!c.bound) {
    c.who = doc::Attribution{};
    c.who.actor = params.attribution.actor;
    c.who.role = params.attribution.role;
    c.who.task = params.attribution.task;
  }
  c.identified = true;
  fill_client(ctx, c, out);
  return true;
}

bool client_list(Context& ctx, ClientsResult& out, RpcError&) {
  out.you = ctx.connection;
  if (ctx.clients == nullptr) return true;
  for (const Client& c : ctx.clients->clients()) {
    ClientInfo info;
    fill_client(ctx, c, info);
    out.clients.push_back(std::move(info));
  }
  return true;
}

// ---- doc.* -----------------------------------------------------------------------------------

bool doc_layers(Context& ctx, const SessionRef& params, LayersResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  s->layers(out.layers);
  return true;
}

bool doc_add_layer(Context& ctx, const AddLayerParams& params, LayersResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  if (!s->add_layer(params.name, params.role, params.edit, params.partition, error)) return false;
  s->layers(out.layers);
  return true;
}

bool doc_set_partition(Context& ctx, const SetPartitionParams& params, LayersResult& out,
                       RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  if (!s->set_partition(params.layer, params.partition, error)) return false;
  s->layers(out.layers);
  return true;
}

bool doc_set_edit_layer(Context& ctx, const SetEditLayerParams& params, LayersResult& out,
                        RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  if (!s->set_edit_layer(params.layer, error)) return false;
  s->layers(out.layers);
  return true;
}

void fill_view(const doc::Document& d, const doc::ResolvedObject& r, ObjectView& out) {
  out.id = r.id;
  out.type = std::string(r.type);
  out.parent = r.parent;
  out.defining_layer = d.layer(r.defining_layer).name();
  for (auto [name, value] : r.properties)
    out.properties.insert_or_assign(std::string(name), *value);
}

bool doc_objects(Context& ctx, const ObjectsParams& params, ObjectsResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  const doc::Document& d = s->document();
  const Vector<doc::ObjectId> ids =
      params.parent.has_value() ? d.children(*params.parent) : d.objects();
  u32 matched = 0;
  for (const doc::ObjectId id : ids) {
    doc::ResolvedObject r;
    if (!d.resolve(id, r) || r.deleted) continue;
    if (!params.type.empty() && r.type != params.type) continue;
    ++matched;
    if (matched <= params.offset || out.objects.size() >= params.limit) continue;
    ObjectView view;
    fill_view(d, r, view);
    out.objects.push_back(std::move(view));
  }
  out.total = matched;
  return true;
}

bool doc_get(Context& ctx, const GetParams& params, ObjectView& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  doc::ResolvedObject r;
  if (!s->document().resolve(params.id, r) || r.deleted) {
    char hex[Id128::k_hex_length + 1];
    params.id.to_hex(hex);
    error = make_error(codes::k_not_found, std::string("no object ") + hex);
    return false;
  }
  fill_view(s->document(), r, out);
  return true;
}

// Every client write goes through a Caller (policy.h): the call's attribution with the host's
// defaults filled in and the role it runs in, which the session checks the write against.
bool doc_apply(Context& ctx, const ApplyParams& params, ApplyResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Caller caller;
  if (!resolve_caller(ctx, &params.attribution, caller, error)) return false;
  if (params.transaction != 0) {
    return s->stage(params.transaction, params.commands, caller, params.atomic, params.layer,
                    time::wall_unix_ms(), out, error);
  }
  return s->apply(params.commands, caller, params.atomic, params.layer, out, error);
}

// ---- a client's transactions (plan 06 §6.4; protocol.md, "Transactions a client opens") --------

bool doc_begin(Context& ctx, const BeginParams& params, TransactionInfo& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Caller caller;
  if (!resolve_caller(ctx, &params.attribution, caller, error)) return false;
  return s->begin_transaction(params, caller, time::wall_unix_ms(), out, error);
}

bool doc_commit(Context& ctx, const TransactionParams& params, ApplyResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Caller caller;
  if (!resolve_caller(ctx, &params.attribution, caller, error)) return false;
  return s->commit_transaction(params.transaction, caller, time::wall_unix_ms(), out, error);
}

bool doc_rollback(Context& ctx, const TransactionParams& params, TransactionInfo& out,
                  RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Caller caller;
  if (!resolve_caller(ctx, &params.attribution, caller, error)) return false;
  return s->rollback_transaction(params.transaction, caller, time::wall_unix_ms(), out, error);
}

bool doc_transactions(Context& ctx, const TransactionsParams& params, TransactionsResult& out,
                      RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  return s != nullptr && s->list_transactions(params.actor, time::wall_unix_ms(), out, error);
}

const doc::Attribution* attribution_of(const std::optional<doc::Attribution>& a) {
  return a.has_value() ? &*a : nullptr;
}

bool doc_undo(Context& ctx, const StepParams& params, StepResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Caller caller;
  if (!resolve_caller(ctx, attribution_of(params.attribution), caller, error)) return false;
  return s->undo(params.steps, &caller, out, error);
}

bool doc_redo(Context& ctx, const StepParams& params, StepResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Caller caller;
  if (!resolve_caller(ctx, attribution_of(params.attribution), caller, error)) return false;
  return s->redo(params.steps, &caller, out, error);
}

bool doc_diff(Context& ctx, const DiffParams& params, DiffResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  const doc::Document& d = s->document();
  const i32 from = d.find_layer(params.from_layer);
  const i32 to = d.find_layer(params.to_layer);
  if (from < 0 || to < 0) {
    error = make_error(codes::k_not_found,
                       "no layer named " + (from < 0 ? params.from_layer : params.to_layer));
    return false;
  }
  out.commands = doc::diff_layers(d.layer(static_cast<u32>(from)), d.layer(static_cast<u32>(to)));
  return true;
}

bool doc_merge(Context& ctx, const MergeParams& params, MergeResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Caller caller;
  if (!resolve_caller(ctx, attribution_of(params.attribution), caller, error)) return false;
  return s->merge(params, &caller, out, error);
}

bool doc_validate(Context& ctx, const SessionRef& params, ValidateResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  Vector<doc::Diagnostic> diagnostics;
  out.ok = s->document().validate(diagnostics);
  for (const auto& d : diagnostics) {
    Diagnostic item;
    item.path = d.path;
    item.message = d.message;
    out.diagnostics.push_back(std::move(item));
  }
  return true;
}

bool doc_journal(Context& ctx, const JournalParams& params, JournalResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  const Vector<doc::Patch>& journal = s->document().journal();
  out.total = journal.size();
  out.position = s->undo_position();
  for (u32 i = params.offset; i < journal.size() && out.patches.size() < params.limit; ++i) {
    out.patches.push_back(journal[i]);
  }
  return true;
}

bool doc_save(Context& ctx, const SessionRef& params, SessionInfo& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  if (!s->save(error)) return false;
  out = client_info(ctx, *s);
  return true;
}

// ---- tunables.* ------------------------------------------------------------------------------

bool tunables_list(Context&, TunablesResult& out, RpcError&) {
  out.tunables = tunables::describe_all();
  return true;
}

bool tunables_set(Context&, const TunableSetParams& params, TunablesResult& out, RpcError& error) {
  tunables::Tunable* t = tunables::find(params.name);
  if (t == nullptr) {
    error = make_error(codes::k_not_found, "no tunable " + params.name);
    return false;
  }
  std::string message;
  if (!t->set_from_json(params.value, &message)) {
    error = make_error(codes::k_invalid_argument, std::move(message));
    return false;
  }
  out.tunables = t->describe();
  return true;
}

// ---- log.* -----------------------------------------------------------------------------------

JsonValue field_json(const log::Field& f) {
  switch (f.kind) {
    case log::Field::Kind::Bool: return JsonValue(f.value.b);
    case log::Field::Kind::Int: return JsonValue(f.value.i);
    case log::Field::Kind::Uint: return JsonValue(f.value.u);
    case log::Field::Kind::Float: return JsonValue(f.value.f);
    case log::Field::Kind::String: return JsonValue(f.string());
    case log::Field::Kind::Pointer:
    case log::Field::Kind::Hex128: {
      // Render through the text formatter for a stable form.
      log::Record r{};
      r.fields = std::span<const log::Field>{&f, 1};
      std::string text;
      log::format_text(r, text);
      const usize eq = text.find_last_of('=');
      return JsonValue(eq == std::string::npos ? text : text.substr(eq + 1));
    }
  }
  return JsonValue();
}

bool log_tail(Context& ctx, const LogTailParams& params, LogTailResult& out, RpcError& error) {
  if (ctx.log_ring == nullptr) {
    error = make_error(codes::k_unavailable, "no log ring is attached to this host");
    return false;
  }
  out.records = JsonValue::array();
  out.next = params.since;
  u32 count = 0;
  ctx.log_ring->for_each(params.since, [&](const log::RingSink::Entry& e) {
    if (count >= params.limit) return;
    ++count;
    JsonValue o = JsonValue::object();
    o.set("seq", JsonValue(e.sequence));
    o.set("t", JsonValue(e.monotonic_ns));
    o.set("wall", JsonValue(e.wall_unix_us));
    o.set("level", JsonValue(log::level_name(e.level)));
    o.set("cat", JsonValue(e.category != nullptr ? e.category->name() : ""));
    o.set("thread", JsonValue(e.thread_index));
    o.set("msg", JsonValue(e.message));
    if (!e.fields.empty()) {
      JsonValue fields = JsonValue::object();
      for (const log::Field& f : e.fields)
        fields.set(f.key, field_json(f));
      o.set("fields", std::move(fields));
    }
    out.records.push_back(std::move(o));
    out.next = e.sequence + 1;
  });
  return true;
}

// ---- gpu.* ------------------------------------------------------------------------------------

bool gpu_adapters(Context&, AdaptersResult& out, RpcError&) {
  out.available = gfx::enumerate_adapters(out.adapters, &out.error);
  return true;
}

// E39's first measurement (gfx/display_probe.h): the outputs, their luminances, and what a hidden
// window on each is offered. Reads; shows no window, opens no device, changes no display setting.
bool gpu_displays(Context&, DisplaysResult& out, RpcError&) {
  out.available = gfx::probe_displays(out.outputs, out.swapchain_colorspace, &out.error);
  return true;
}

// ---- schema.* --------------------------------------------------------------------------------

void render_type(const schema::TypeRef& t, std::string& out) {
  using schema::Kind;
  switch (t.kind) {
    case Kind::Enum:
    case Kind::Struct: out.append(t.type != nullptr ? t.type->qualified_name : "?"); return;
    case Kind::Optional:
      render_type(*t.element, out);
      out.push_back('?');
      return;
    case Kind::Array:
      render_type(*t.element, out);
      out.append("[]");
      return;
    case Kind::FixedArray:
      render_type(*t.element, out);
      out.push_back('[');
      out.append(std::to_string(t.fixed_count));
      out.push_back(']');
      return;
    case Kind::Map:
      out.append("map<");
      render_type(*t.key, out);
      out.append(", ");
      render_type(*t.element, out);
      out.push_back('>');
      return;
    default: out.append(schema::kind_name(t.kind)); return;
  }
}

// A default-constructed instance of a schema struct, which is where a field's default lives:
// schemac writes the declared defaults into the C++ initializers and nowhere else, and the
// protocol's reader leaves an absent field at exactly this value. Reading it back through the
// type's own JSON writer means `schema.describe` states a default in the same form a caller
// would send it, and a client that generates forms or JSON Schema from the description (the MCP
// bridge does, docs/subsystems/apps.md "engine-mcp") needs no second copy of any default.
class DefaultObject {
 public:
  explicit DefaultObject(const schema::TypeInfo& type) : type_(type) {
    if (type.ops == nullptr || type.ops->construct == nullptr || type.size == 0) return;
    align_ = std::max<usize>(type.align, alignof(std::max_align_t));
    storage_ = ::operator new(type.size, std::align_val_t{align_});
    type.ops->construct(storage_);
  }
  ~DefaultObject() {
    if (storage_ == nullptr) return;
    if (type_.ops->destroy != nullptr) type_.ops->destroy(storage_);
    ::operator delete(storage_, std::align_val_t{align_});
  }
  ENGINE_NON_COPYABLE(DefaultObject);

  // The field's default as JSON, or false when there is no object to read it from.
  bool field(const schema::FieldInfo& f, JsonValue& out) const {
    if (storage_ == nullptr) return false;
    return schema::to_json(f.type, static_cast<const std::byte*>(storage_) + f.offset, out);
  }

 private:
  const schema::TypeInfo& type_;
  void* storage_ = nullptr;
  usize align_ = 0;
};

bool schema_types(Context&, SchemaTypesResult& out, RpcError&) {
  for (const schema::TypeInfo* t : schema::Registry::global().all()) {
    out.types.push_back(t->qualified_name);
  }
  std::sort(out.types.begin(), out.types.end());
  return true;
}

bool schema_describe(Context&, const SchemaDescribeParams& params, SchemaDescribeResult& out,
                     RpcError& error) {
  const schema::TypeInfo* t = schema::Registry::global().find(params.type);
  if (t == nullptr) {
    error = make_error(codes::k_not_found, "no schema type " + params.type);
    return false;
  }
  JsonValue d = JsonValue::object();
  d.set("name", JsonValue(t->qualified_name));
  d.set("ns", JsonValue(t->ns));
  d.set("kind", JsonValue(t->kind == schema::Kind::Enum ? "enum" : "struct"));
  d.set("version", JsonValue(static_cast<u32>(t->version)));
  d.set("tag", JsonValue(t->tag != nullptr ? t->tag : ""));
  d.set("doc", JsonValue(t->doc != nullptr ? t->doc : ""));
  if (t->kind == schema::Kind::Struct) {
    const DefaultObject defaults(*t);
    JsonValue fields = JsonValue::array();
    for (const schema::FieldInfo& f : t->fields) {
      JsonValue fo = JsonValue::object();
      fo.set("name", JsonValue(f.name));
      std::string type_text;
      render_type(f.type, type_text);
      fo.set("type", JsonValue(type_text));
      fo.set("since", JsonValue(static_cast<u32>(f.since_version)));
      const bool transient = (f.flags & schema::FieldFlag::transient) != 0;
      fo.set("transient", JsonValue(transient));
      fo.set("deprecated", JsonValue((f.flags & schema::FieldFlag::deprecated) != 0));
      fo.set("doc", JsonValue(f.doc != nullptr ? f.doc : ""));
      // What an absent field becomes. A transient field is never read, so it has none to state.
      JsonValue default_value;
      if (!transient && defaults.field(f, default_value)) {
        fo.set("default", std::move(default_value));
      }
      fields.push_back(std::move(fo));
    }
    d.set("fields", std::move(fields));
  } else {
    JsonValue values = JsonValue::array();
    for (const schema::EnumValueInfo& v : t->values) {
      JsonValue vo = JsonValue::object();
      vo.set("name", JsonValue(v.name));
      vo.set("value", JsonValue(v.value));
      vo.set("doc", JsonValue(v.doc != nullptr ? v.doc : ""));
      values.push_back(std::move(vo));
    }
    d.set("values", std::move(values));
  }
  out.description = std::move(d);
  return true;
}

}  // namespace

// policy_methods.cpp.
void add_policy_methods(Dispatcher& d);
void add_placement_methods(Dispatcher& d);
void add_introspection_methods(Dispatcher& d);  // describe.cpp
// history_methods.cpp.
void add_history_methods(Dispatcher& d);
// task.* and review.* (ledger.cpp).
void add_ledger_methods(Dispatcher& d);

// Read-only methods are marked `read_only` and no role restricts them (plan 06 §6.5: read access
// is unrestricted); every other one is a write a role must name. A method is read-only when it
// changes no document, no lease, no host setting, not the running world and not what is on a
// person's screen; a read that leaves a file the caller asked for says so (`read_leaving_files`),
// and protocol.md's "What a read may do" sorts every registration with its reason. `session.open`
// and `session.close` are the housekeeping of a caller's own handles: a role that may only look
// must still be able to open what it looks at and let it go, so only an open that creates the
// document leaves a file. `doc.save` is a write: it rewrites the document's files from this host's
// memory, over whatever another host saved since.
namespace {
bool open_creates(const JsonValue& params) { return param_is_true(params, "create"); }
}  // namespace

void add_builtin_methods(Dispatcher& d) {
  d.add(read_only(method_no_params<EngineInfo, &engine_info>(
      "engine.info", "Engine name, version, build configuration, process id, and counts.")));
  d.add(read_only(method_no_params<MethodsResult, &engine_methods>(
      "engine.methods",
      "The method catalogue with parameter and result schema types, and which methods are "
      "read-only.")));

  d.add(read_leaving_files(
      method<SessionOpenParams, SessionInfo, &session_open>(
          "session.open",
          "Open a document directory (create it with `create`); returns the session."),
      &open_creates));
  d.add(read_only(method<SessionRef, SessionCloseResult, &session_close>(
      "session.close",
      "Let go of a session this client opened; it closes when no client holds it.")));
  d.add(
      read_only(method_no_params<SessionsResult, &session_list>("session.list", "Open sessions.")));
  d.add(read_only(method<SessionRef, SessionInfo, &session_info>(
      "session.info", "Layers, journal length, undo position.")));
  // Subscriptions change no document, lease or host setting — only what this connection is sent —
  // so they are read-only like the reads they replace (a poll of session.info).
  d.add(read_only(method<SubscribeParams, SubscribeResult, &session_subscribe>(
      "session.subscribe",
      "Socket connections only: push this connection the session's events as JSON-RPC "
      "notifications `event.<kind>` — journal (a commit, undo or redo), selection (engine-editor), "
      "scene (a render.load {session} scene followed the document) and session (opened, closed, a "
      "lease). Empty session is every session; empty kinds is all four. Events carry ids, indices, "
      "revisions and attribution, never content.")));
  d.add(read_only(method<SubscribeParams, SubscribeResult, &session_unsubscribe>(
      "session.unsubscribe",
      "Socket connections only: stop pushing the named kinds (empty: all) of the session's "
      "events to this connection.")));
  d.add(read_only(method_no_params<SubscriptionsResult, &session_subscriptions>(
      "session.subscriptions",
      "Who is subscribed to which events, and how many each has had dropped since it was last "
      "sent one (non-zero: that connection is not reading).")));
  d.add(read_only(method<ClientHelloParams, ClientInfo, &client_hello>(
      "client.hello",
      "Socket connections only: say who this client is (its name, and the actor, role and task "
      "its calls are attributed to and checked as when they name none).")));
  d.add(read_only(method_no_params<ClientsResult, &client_list>(
      "client.list",
      "Every client connected to this host: who each said it is, how many requests it made, and "
      "the sessions and leases it holds (given back when its connection ends).")));

  d.add(read_only(method<SessionRef, LayersResult, &doc_layers>(
      "doc.layers",
      "The layer stack, weakest first, with each layer's storage form and tiles, and a proposal "
      "layer's target, owner and state.")));
  d.add(method<AddLayerParams, LayersResult, &doc_add_layer>(
      "doc.add_layer",
      "Append a layer (strongest), optionally partitioned into tile files and made the edit "
      "layer."));
  d.add(method<SetPartitionParams, LayersResult, &doc_set_partition>(
      "doc.set_partition", "Store a layer as tile files, or as one file again, and rewrite it."));
  d.add(method<SetEditLayerParams, LayersResult, &doc_set_edit_layer>(
      "doc.set_edit_layer", "Choose the layer that commands edit."));
  d.add(read_only(method<ObjectsParams, ObjectsResult, &doc_objects>(
      "doc.objects", "Live objects with composed properties, filtered by type or parent.")));
  d.add(
      read_only(method<GetParams, ObjectView, &doc_get>("doc.get", "One live object, composed.")));
  d.add(method<ApplyParams, ApplyResult, &doc_apply>(
      "doc.apply",
      "Apply commands as one transaction with attribution; commits to the journal and saves. "
      "Checked against the caller's role, a proposal's owner, and the leases when the document "
      "requires them. With `transaction`, stages them in that open transaction instead."));
  d.add(method<BeginParams, TransactionInfo, &doc_begin>(
      "doc.begin",
      "Open a transaction on a layer: the doc.apply calls that name it are checked and staged, "
      "and land together at doc.commit as one journal patch or vanish together at doc.rollback."));
  d.add(method<TransactionParams, ApplyResult, &doc_commit>(
      "doc.commit",
      "Commit an open transaction's staged commands as one checked transaction and one journal "
      "patch; when any no longer applies nothing lands and the transaction stays open."));
  d.add(method<TransactionParams, TransactionInfo, &doc_rollback>(
      "doc.rollback", "Discard an open transaction and everything it staged; nothing changes."));
  d.add(read_only(method<TransactionsParams, TransactionsResult, &doc_transactions>(
      "doc.transactions", "The document's open transactions, visible to everyone.")));
  d.add(method<StepParams, StepResult, &doc_undo>("doc.undo", "Undo committed patches."));
  d.add(method<StepParams, StepResult, &doc_redo>("doc.redo", "Redo undone patches."));
  d.add(read_only(method<DiffParams, DiffResult, &doc_diff>(
      "doc.diff", "Commands that turn one layer into another.")));
  d.add(method<MergeParams, MergeResult, &doc_merge>(
      "doc.merge",
      "Three-way merge of two layers over their common base into a layer, as one transaction."));
  d.add(read_only(method<SessionRef, ValidateResult, &doc_validate>(
      "doc.validate", "Validate every record against the schema registry.")));
  d.add(read_only(method<JournalParams, JournalResult, &doc_journal>(
      "doc.journal", "Committed patches with attribution.")));
  d.add(method<SessionRef, SessionInfo, &doc_save>(
      "doc.save", "Write layer files and the manifest from what this host holds."));

  d.add(read_only(method_no_params<TunablesResult, &tunables_list>(
      "tunables.list", "Every tunable with value and range.")));
  d.add(method<TunableSetParams, TunablesResult, &tunables_set>("tunables.set",
                                                                "Set one tunable by name."));

  d.add(read_only(method<LogTailParams, LogTailResult, &log_tail>(
      "log.tail", "Log records from the host's ring since a sequence number.")));

  d.add(read_only(method_no_params<AdaptersResult, &gpu_adapters>(
      "gpu.adapters",
      "Vulkan physical devices with driver, memory, queues, extensions, and tier.")));
  d.add(read_only(method_no_params<DisplaysResult, &gpu_displays>(
      "gpu.displays",
      "Display outputs with the luminances Windows reports and the surface formats and colour "
      "spaces a window on each is offered (E39). Shows no window, changes no setting.")));

  d.add(read_only(method_no_params<SchemaTypesResult, &schema_types>(
      "schema.types", "Every registered schema type.")));
  d.add(read_only(method<SchemaDescribeParams, SchemaDescribeResult, &schema_describe>(
      "schema.describe", "Fields or values of one schema type.")));

  // lease.*, doc.propose_layer, doc.promote, doc.reject and engine.roles (policy_methods.cpp).
  add_policy_methods(d);
  add_placement_methods(d);
  add_introspection_methods(d);
  // Blobs and checkpoints: doc.put_blob, doc.blob, doc.collect_blobs, doc.checkpoint,
  // doc.checkpoints and doc.rollback_to (history_methods.cpp).
  add_history_methods(d);
  // The task ledger and review items (plan 06 §6.8): task.* and review.* (ledger.cpp).
  add_ledger_methods(d);
}

}  // namespace engine::protocol
