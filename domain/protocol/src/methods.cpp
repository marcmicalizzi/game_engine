// The day-one method set (docs/plan/06-agent-tooling.md §6.9): sessions, document editing and
// introspection, tunables, the log ring, and schema discovery.
#include <core/log/log.h>
#include <core/schema/type_info.h>
#include <domain/gfx/adapter.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <cstring>

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
    out.methods.push_back(std::move(info));
  }
  return true;
}

// ---- session.* -------------------------------------------------------------------------------

bool session_open(Context& ctx, const SessionOpenParams& params, SessionInfo& out,
                  RpcError& error) {
  Session* s = ctx.sessions->open(params, error);
  if (s == nullptr) return false;
  out = s->info();
  return true;
}

bool session_close(Context& ctx, const SessionRef& params, SessionRef& out, RpcError& error) {
  if (ctx.sessions->require(params.session, error) == nullptr) return false;
  ctx.sessions->close(params.session);
  out.session = params.session;
  return true;
}

bool session_list(Context& ctx, SessionsResult& out, RpcError&) {
  for (const auto& s : ctx.sessions->sessions())
    out.sessions.push_back(s->info());
  return true;
}

bool session_info(Context& ctx, const SessionRef& params, SessionInfo& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  out = s->info();
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
  if (!s->add_layer(params.name, params.role, params.edit, error)) return false;
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

bool doc_apply(Context& ctx, const ApplyParams& params, ApplyResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  return s->apply(params.commands, params.attribution, params.atomic, out, error);
}

bool doc_undo(Context& ctx, const StepParams& params, StepResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  return s != nullptr && s->undo(params.steps, out, error);
}

bool doc_redo(Context& ctx, const StepParams& params, StepResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  return s != nullptr && s->redo(params.steps, out, error);
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
  return s != nullptr && s->merge(params, out, error);
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
  out = s->info();
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
    JsonValue fields = JsonValue::array();
    for (const schema::FieldInfo& f : t->fields) {
      JsonValue fo = JsonValue::object();
      fo.set("name", JsonValue(f.name));
      std::string type_text;
      render_type(f.type, type_text);
      fo.set("type", JsonValue(type_text));
      fo.set("since", JsonValue(static_cast<u32>(f.since_version)));
      fo.set("transient", JsonValue((f.flags & schema::FieldFlag::transient) != 0));
      fo.set("deprecated", JsonValue((f.flags & schema::FieldFlag::deprecated) != 0));
      fo.set("doc", JsonValue(f.doc != nullptr ? f.doc : ""));
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

void add_builtin_methods(Dispatcher& d) {
  d.add(method_no_params<EngineInfo, &engine_info>(
      "engine.info", "Engine name, version, build configuration, process id, and counts."));
  d.add(method_no_params<MethodsResult, &engine_methods>(
      "engine.methods", "The method catalogue with parameter and result schema types."));

  d.add(method<SessionOpenParams, SessionInfo, &session_open>(
      "session.open", "Open a document directory (create it with `create`); returns the session."));
  d.add(method<SessionRef, SessionRef, &session_close>("session.close", "Close a session."));
  d.add(method_no_params<SessionsResult, &session_list>("session.list", "Open sessions."));
  d.add(method<SessionRef, SessionInfo, &session_info>("session.info",
                                                       "Layers, journal length, undo position."));

  d.add(method<SessionRef, LayersResult, &doc_layers>("doc.layers",
                                                      "The layer stack, weakest first."));
  d.add(method<AddLayerParams, LayersResult, &doc_add_layer>(
      "doc.add_layer", "Append a layer (strongest) and optionally make it the edit layer."));
  d.add(method<SetEditLayerParams, LayersResult, &doc_set_edit_layer>(
      "doc.set_edit_layer", "Choose the layer that commands edit."));
  d.add(method<ObjectsParams, ObjectsResult, &doc_objects>(
      "doc.objects", "Live objects with composed properties, filtered by type or parent."));
  d.add(method<GetParams, ObjectView, &doc_get>("doc.get", "One live object, composed."));
  d.add(method<ApplyParams, ApplyResult, &doc_apply>(
      "doc.apply",
      "Apply commands as one transaction with attribution; commits to the journal and saves."));
  d.add(method<StepParams, StepResult, &doc_undo>("doc.undo", "Undo committed patches."));
  d.add(method<StepParams, StepResult, &doc_redo>("doc.redo", "Redo undone patches."));
  d.add(method<DiffParams, DiffResult, &doc_diff>("doc.diff",
                                                  "Commands that turn one layer into another."));
  d.add(method<MergeParams, MergeResult, &doc_merge>(
      "doc.merge",
      "Three-way merge of two layers over their common base into a layer, as one transaction."));
  d.add(method<SessionRef, ValidateResult, &doc_validate>(
      "doc.validate", "Validate every record against the schema registry."));
  d.add(method<JournalParams, JournalResult, &doc_journal>("doc.journal",
                                                           "Committed patches with attribution."));
  d.add(method<SessionRef, SessionInfo, &doc_save>("doc.save",
                                                   "Write layer files and the manifest."));

  d.add(method_no_params<TunablesResult, &tunables_list>("tunables.list",
                                                         "Every tunable with value and range."));
  d.add(method<TunableSetParams, TunablesResult, &tunables_set>("tunables.set",
                                                                "Set one tunable by name."));

  d.add(method<LogTailParams, LogTailResult, &log_tail>(
      "log.tail", "Log records from the host's ring since a sequence number."));

  d.add(method_no_params<AdaptersResult, &gpu_adapters>(
      "gpu.adapters",
      "Vulkan physical devices with driver, memory, queues, extensions, and tier."));

  d.add(method_no_params<SchemaTypesResult, &schema_types>("schema.types",
                                                           "Every registered schema type."));
  d.add(method<SchemaDescribeParams, SchemaDescribeResult, &schema_describe>(
      "schema.describe", "Fields or values of one schema type."));
}

}  // namespace engine::protocol
