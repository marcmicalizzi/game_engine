// The curated tools of engine-mcp (docs/subsystems/apps.md, "engine-mcp"; plan 06 §6.3 and §6.9).
//
// Each tool is a schema builder and a runner. The builder starts from the input schema the host's
// own `schema.describe` generates for the protocol method's params type and says only what the
// protocol cannot: which fields the bridge fills or hides, which are required, the fields it adds
// (`load`, `cursor`, `detail`, `name`), and prose. The runner is the compound behaviour: filling
// attribution, paging, loading a scene once, writing bulk results into the workspace and returning
// their file:// URIs with a short summary.
#include "bridge.h"

#include <core/ids/id128.h>
#include <core/json/json.h>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>

namespace engine::mcp {

namespace {

// ---- schema editing
// -------------------------------------------------------------------------------

JsonValue& props(JsonValue& schema) {
  JsonValue* p = schema.find("properties");
  if (p == nullptr || !p->is_object()) {
    schema.set("properties", JsonValue::object());
    p = schema.find("properties");
  }
  return *p;
}

JsonValue object_schema() {
  JsonValue s = JsonValue::object();
  s.set("type", JsonValue("object"));
  s.set("properties", JsonValue::object());
  s.set("additionalProperties", JsonValue(false));
  return s;
}

void drop(JsonValue& schema, std::string_view name) { props(schema).as_object().erase(name); }

// A required field has no default to state: the caller always sends it.
void require(JsonValue& schema, std::initializer_list<const char*> names) {
  JsonValue list = JsonValue::array();
  for (const char* n : names) {
    list.push_back(JsonValue(n));
    if (JsonValue* p = props(schema).find(n); p != nullptr && p->is_object()) {
      p->as_object().erase(std::string_view("default"));
    }
  }
  schema.set("required", std::move(list));
}

// Appends the bridge's sentence to what the schema already says about a field; the schema's own
// documentation comes first because it is the engine's.
void note(JsonValue& schema, std::string_view field, std::string_view text) {
  JsonValue* p = props(schema).find(field);
  if (p == nullptr) return;
  std::string d = text_of(*p, "description");
  if (!d.empty()) d.push_back(' ');
  d.append(text);
  p->set("description", JsonValue(d));
}

void set_default(JsonValue& schema, std::string_view field, JsonValue value) {
  if (JsonValue* p = props(schema).find(field); p != nullptr) p->set("default", std::move(value));
}

// Copies a generated property from one schema into another, optionally under another name.
bool copy_prop(const JsonValue& from, JsonValue& to, std::string_view name, std::string& error,
               std::string_view as = {}) {
  const JsonValue* p = from.find("properties");
  const JsonValue* field = p != nullptr ? p->find(name) : nullptr;
  if (field == nullptr) {
    error = "the host's schema has no field " + std::string(name);
    return false;
  }
  props(to).set(as.empty() ? name : as, *field);
  return true;
}

JsonValue prop(const char* type, std::string_view description) {
  JsonValue p = JsonValue::object();
  p.set("type", JsonValue(type));
  p.set("description", JsonValue(description));
  return p;
}

// `limit` and `cursor`, for a tool that pages a list.
void page_schema(JsonValue& schema, u32 default_limit, u32 max_limit, std::string_view what) {
  JsonValue limit =
      prop("integer", std::string("How many ") + std::string(what) + " to return at most (" +
                          std::to_string(max_limit) + " at most).");
  limit.set("minimum", JsonValue(u32{1}));
  limit.set("maximum", JsonValue(max_limit));
  limit.set("default", JsonValue(default_limit));
  props(schema).set("limit", std::move(limit));
  props(schema).set("cursor", prop("string",
                                   "Where to continue: the next_cursor a previous call "
                                   "of this tool returned. Omit it for the first page."));
}

// ---- argument reading ---------------------------------------------------------------------------

struct Page {
  u32 offset = 0;
  u32 limit = 0;
};

bool read_page(const JsonValue& args, u32 default_limit, u32 max_limit, Page& page,
               ToolOutcome& out) {
  u64 limit = default_limit;
  const JsonValue* given = args.find("limit");
  if ((given != nullptr && !given->get_u64(limit)) || limit == 0 || limit > max_limit) {
    fail(out, "limit must be an integer from 1 to " + std::to_string(max_limit),
         "Omit it for " + std::to_string(default_limit) + ".");
    return false;
  }
  page.limit = static_cast<u32>(limit);
  const std::string cursor = text_of(args, "cursor");
  page.offset = 0;
  if (!cursor.empty()) {
    const std::from_chars_result r =
        std::from_chars(cursor.data(), cursor.data() + cursor.size(), page.offset);
    if (r.ec != std::errc() || r.ptr != cursor.data() + cursor.size()) {
      fail(out, "cursor '" + cursor + "' is not one this tool returned",
           "Pass the next_cursor from the previous page, or omit cursor to start again.");
      return false;
    }
  }
  return true;
}

// Sets `next_cursor` when there is more after this page.
void finish_page(JsonValue& data, u32 offset, u64 returned, u64 total) {
  data.set("total", JsonValue(total));
  if (offset + returned < total) {
    data.set("next_cursor", JsonValue(std::to_string(offset + returned)));
  }
}

std::string range_text(u32 offset, u64 returned, u64 total) {
  if (returned == 0) return "none of " + std::to_string(total);
  return std::to_string(offset + 1) + "-" + std::to_string(offset + returned) + " of " +
         std::to_string(total);
}

std::string fixed(f64 value, int decimals) {
  char text[64];
  std::snprintf(text, sizeof(text), "%.*f", decimals, value);
  return text;
}

// A copy of the arguments without the fields the bridge consumes itself.
JsonValue without(const JsonValue& args, std::initializer_list<const char*> names) {
  JsonValue out = args;
  for (const char* n : names)
    out.as_object().erase(std::string_view(n));
  return out;
}

// ---- workspace
// ------------------------------------------------------------------------------------

std::filesystem::path native(std::string_view utf8) {
  return std::filesystem::path(
      std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

bool workspace_dir(Bridge& b, std::string_view sub, std::string& dir, ToolOutcome& out) {
  dir = b.options().workspace;
  if (!sub.empty()) {
    dir.push_back('/');
    dir.append(sub);
  }
  std::error_code ec;
  std::filesystem::create_directories(native(dir), ec);
  if (ec) {
    fail(out, "cannot create the workspace directory " + dir + ": " + ec.message(),
         "Start engine-mcp with --workspace naming a directory it may write.");
    return false;
  }
  return true;
}

bool plain_name(std::string_view name) {
  if (name.empty() || name.size() > 100 || name.front() == '.') return false;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

// The base name a render tool writes under. A name the caller gives is used as given, so a
// "baseline" can be written again on purpose; a generated one is never a name whose result file
// already exists, so two captures in a row never overwrite each other.
bool output_name(const JsonValue& args, const std::string& dir, std::string_view stem,
                 std::string& name, ToolOutcome& out) {
  name = text_of(args, "name");
  if (!name.empty()) {
    if (!plain_name(name)) {
      fail(out, "name '" + name + "' is not a plain file name",
           "Use letters, digits, '-', '_' and '.', not starting with '.', or omit name.");
      return false;
    }
    return true;
  }
  for (u32 n = 1; n < 100000; ++n) {
    char candidate[64];
    std::snprintf(candidate, sizeof(candidate), "%.*s-%03u", static_cast<int>(stem.size()),
                  stem.data(), n);
    std::error_code ec;
    if (!std::filesystem::exists(native(dir + "/" + candidate + ".json"), ec)) {
      name = candidate;
      return true;
    }
  }
  fail(out, "no free name under " + dir, "Pass name, or clear out the workspace.");
  return false;
}

bool write_file(const std::string& path, std::string_view text, ToolOutcome& out) {
  std::ofstream file(native(path), std::ios::binary | std::ios::trunc);
  if (file.good()) file.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file.good()) {
    fail(out, "cannot write " + path, "Check that the workspace directory is writable.");
    return false;
  }
  return true;
}

bool write_json_file(const std::string& path, const JsonValue& value, ToolOutcome& out) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = true});
  text.push_back('\n');
  return write_file(path, text, out);
}

void link(ToolOutcome& out, const std::string& uri, std::string name, std::string mime) {
  out.links.push_back(ToolOutcome::Link{uri, std::move(name), std::move(mime)});
}

std::string mime_of(std::string_view uri) {
  if (uri.ends_with(".png")) return "image/png";
  if (uri.ends_with(".json")) return "application/json";
  if (uri.ends_with(".jsonl")) return "application/jsonl";
  return "application/octet-stream";
}

// ---- attribution --------------------------------------------------------------------------------

// The attribution property of a mutating tool: engine.doc.Attribution as the host describes it,
// without the timestamp the host fills, with rationale required and the actor's default stated.
bool attribution_schema(Bridge& b, JsonValue& schema, std::string& error) {
  JsonValue apply;
  if (!b.schemas().params_schema("doc.apply", apply, error)) return false;
  if (!copy_prop(apply, schema, "attribution", error)) return false;
  JsonValue& a = *props(schema).find("attribution");
  drop(a, "timestamp_unix_ms");
  note(a, "actor",
       "Who is making the change. Defaults to '" + b.options().actor + "', the bridge's --actor.");
  note(a, "role", "The role the change is made in: environment, designer, qa, ...");
  note(a, "task", "The task id or short name the change belongs to.");
  note(a, "rationale",
       "Why the change is made, in a sentence. Required: a mutation without one "
       "is refused.");
  set_default(a, "actor", JsonValue(b.options().actor));
  JsonValue req = JsonValue::array();
  req.push_back(JsonValue("rationale"));
  a.set("required", std::move(req));
  a.set("description",
        JsonValue("Who made the change and why; recorded in the journal with the transaction "
                  "(plan 06 section 6.4)."));
  return true;
}

// The attribution a mutation is sent with: the caller's, with the actor filled from --actor, or
// a refusal that says what to add.
bool attribution(Bridge& b, const JsonValue& args, JsonValue& out, ToolOutcome& failure) {
  const JsonValue* given = args.find("attribution");
  if (given != nullptr && !given->is_object()) {
    fail(failure, "attribution must be an object",
         "Pass {\"rationale\": \"why\", \"task\": \"...\", \"role\": \"...\"}.");
    return false;
  }
  out = given != nullptr ? *given : JsonValue::object();
  std::string rationale = text_of(out, "rationale");
  while (!rationale.empty() && (rationale.back() == ' ' || rationale.back() == '\t'))
    rationale.pop_back();
  if (rationale.empty()) {
    fail(failure,
         "refused: this change has no rationale. Every transaction records who made it and why, "
         "and a change nobody can explain later cannot be reviewed or safely undone.",
         "Add attribution.rationale, one sentence saying why the change is made, and call "
         "again; attribution.task and attribution.role are worth filling too (actor defaults "
         "to '" +
             b.options().actor + "').");
    return false;
  }
  if (text_of(out, "actor").empty()) out.set("actor", JsonValue(b.options().actor));
  return true;
}

// Sends commands as one doc.apply transaction and turns the ApplyResult into an outcome: a
// transaction that committed nothing is an error, with every diagnostic.
void commit(Bridge& b, const JsonValue& args, JsonValue commands, bool atomic, std::string what,
            ToolOutcome& out) {
  JsonValue attr;
  if (!attribution(b, args, attr, out)) return;
  JsonValue params = JsonValue::object();
  params.set("session", JsonValue(text_of(args, "session")));
  params.set("commands", std::move(commands));
  params.set("attribution", std::move(attr));
  params.set("atomic", JsonValue(atomic));
  JsonValue r;
  if (!b.call("doc.apply", params, r, out)) return;
  std::string problems;
  const JsonValue* diagnostics = r.find("diagnostics");
  const usize count = diagnostics != nullptr ? diagnostics->size() : 0;
  for (usize i = 0; i < count; ++i) {
    problems +=
        "\n  " + text_of((*diagnostics)[i], "path") + ": " + text_of((*diagnostics)[i], "message");
  }
  if (!bool_of(r, "committed")) {
    fail(out, "Nothing was committed: " + std::to_string(count) + " problem(s)." + problems,
         atomic ? "Fix what the diagnostics name. The transaction was atomic, so one failing "
                  "command rolled back all of them."
                : "Fix what the diagnostics name.");
    out.data.set("result", r);
    return;
  }
  out.summary = what + ": committed as journal patch " + std::to_string(uint_of(r, "patch_index")) +
                " (" + std::to_string(uint_of(r, "applied")) + " command(s) applied).";
  if (count > 0) out.summary += " Not applied:" + problems;
  out.data = r;
}

JsonValue command(const char* kind, std::string_view id) {
  JsonValue c = JsonValue::object();
  c.set("kind", JsonValue(kind));
  c.set("id", JsonValue(id));
  return c;
}

// ---- summaries ----------------------------------------------------------------------------------

std::string layers_text(const JsonValue& layers) {
  if (!layers.is_array() || layers.size() == 0) return "No layers.";
  std::string s = "Layers, weakest first:";
  for (usize i = 0; i < layers.size(); ++i) {
    const JsonValue& l = layers[i];
    s += (i == 0 ? " " : ", ") + text_of(l, "name") + " (" + text_of(l, "role") + ", " +
         std::to_string(uint_of(l, "records")) + " records";
    if (uint_of(l, "tiles") > 0) s += ", " + std::to_string(uint_of(l, "tiles")) + " tiles";
    if (bool_of(l, "is_edit")) s += ", edit layer";
    s += ")";
  }
  return s + ".";
}

std::string session_text(const JsonValue& info) {
  return "Session " + text_of(info, "session") + ": \"" + text_of(info, "name") + "\" at " +
         text_of(info, "path") + ". " +
         layers_text(info.find("layers") != nullptr ? *info.find("layers") : JsonValue()) +
         " Journal: " + std::to_string(uint_of(info, "journal_length")) +
         " patch(es), undo position " + std::to_string(uint_of(info, "undo_position")) + ".";
}

// ---- session tools
// --------------------------------------------------------------------------------

bool schema_open_session(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("session.open", s, e)) return false;
  note(s, "path", "Relative paths are relative to the directory engine-mcp was started in.");
  require(s, {"path"});
  return true;
}

void run_open_session(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  JsonValue r;
  if (!b.call("session.open", args, r, out)) return;
  out.summary = session_text(r);
  out.data = r;
}

bool schema_session_ref(Bridge& b, const char* method, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema(method, s, e)) return false;
  note(s, "session", "The session id open_session returned (\"s1\").");
  require(s, {"session"});
  return true;
}

bool schema_close_session(Bridge& b, JsonValue& s, std::string& e) {
  return schema_session_ref(b, "session.close", s, e);
}

void run_close_session(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  JsonValue r;
  if (!b.call("session.close", args, r, out)) return;
  out.summary = "Closed session " + text_of(r, "session") +
                ". Everything it committed is on disk; open_session opens it again.";
  out.data = r;
}

bool schema_list_sessions(Bridge& b, JsonValue& s, std::string& e) {
  return b.schemas().params_schema("session.list", s, e);
}

void run_list_sessions(Bridge& b, const JsonValue&, ToolOutcome& out) {
  JsonValue r;
  if (!b.call("session.list", JsonValue(), r, out)) return;
  const JsonValue* list = r.find("sessions");
  const usize n = list != nullptr ? list->size() : 0;
  if (n == 0) {
    out.summary = "No open sessions; open_session opens one.";
  } else {
    out.summary = std::to_string(n) + " open session(s):";
    for (usize i = 0; i < n; ++i) {
      const JsonValue& s = (*list)[i];
      out.summary += "\n  " + text_of(s, "session") + " \"" + text_of(s, "name") + "\" at " +
                     text_of(s, "path");
    }
  }
  out.data = r;
}

// ---- layer tools
// ------------------------------------------------------------------------------------

bool schema_layers(Bridge& b, JsonValue& s, std::string& e) {
  return schema_session_ref(b, "doc.layers", s, e);
}

void run_layers_method(Bridge& b, const char* method, const JsonValue& args, ToolOutcome& out) {
  JsonValue r;
  if (!b.call(method, args, r, out)) return;
  out.summary = layers_text(r.find("layers") != nullptr ? *r.find("layers") : JsonValue());
  out.data = r;
}

void run_layers(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  run_layers_method(b, "doc.layers", args, out);
}

bool schema_add_layer(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("doc.add_layer", s, e)) return false;
  note(s, "session", "The session id open_session returned.");
  note(s, "name", "The new layer's name, unique in the document.");
  require(s, {"session", "name"});
  return true;
}

void run_add_layer(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  run_layers_method(b, "doc.add_layer", args, out);
}

bool schema_set_edit_layer(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("doc.set_edit_layer", s, e)) return false;
  note(s, "session", "The session id open_session returned.");
  note(s, "layer", "The layer's name; layers lists them.");
  require(s, {"session", "layer"});
  return true;
}

void run_set_edit_layer(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  run_layers_method(b, "doc.set_edit_layer", args, out);
}

// ---- reading objects
// ------------------------------------------------------------------------------

constexpr u32 k_objects_default = 50;
constexpr u32 k_objects_max = 500;

JsonValue detail_prop(const char* summary_means, const char* default_value) {
  JsonValue d = prop("string", std::string("\"full\" or \"summary\": ") + summary_means);
  JsonValue values = JsonValue::array();
  values.push_back(JsonValue("full"));
  values.push_back(JsonValue("summary"));
  d.set("enum", std::move(values));
  d.set("default", JsonValue(default_value));
  return d;
}

bool read_detail(const JsonValue& args, const char* fallback, bool& summary, ToolOutcome& out) {
  const std::string detail = text_of(args, "detail", fallback);
  if (detail != "full" && detail != "summary") {
    fail(out, "detail must be \"full\" or \"summary\"; got '" + detail + "'", "");
    return false;
  }
  summary = detail == "summary";
  return true;
}

bool schema_objects(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("doc.objects", s, e)) return false;
  drop(s, "offset");
  drop(s, "limit");
  note(s, "session", "The session id open_session returned.");
  note(s, "type", "list_schema lists the types.");
  page_schema(s, k_objects_default, k_objects_max, "objects");
  props(s).set(
      "detail",
      detail_prop("summary gives ids, types, parents and property names without values.", "full"));
  require(s, {"session"});
  return true;
}

void run_objects(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  Page page;
  bool summary = false;
  if (!read_page(args, k_objects_default, k_objects_max, page, out)) return;
  if (!read_detail(args, "full", summary, out)) return;
  // The protocol pages this one itself; the bridge's cursor is its offset.
  JsonValue params = without(args, {"cursor", "detail", "limit"});
  params.set("offset", JsonValue(page.offset));
  params.set("limit", JsonValue(page.limit));
  JsonValue r;
  if (!b.call("doc.objects", params, r, out)) return;
  JsonValue objects = r.find("objects") != nullptr ? *r.find("objects") : JsonValue::array();
  if (summary) {
    for (usize i = 0; i < objects.size(); ++i) {
      JsonValue& o = objects[i];
      JsonValue names = JsonValue::array();
      if (const JsonValue* p = o.find("properties"); p != nullptr && p->is_object()) {
        for (u32 k = 0; k < p->as_object().size(); ++k)
          names.push_back(JsonValue(p->as_object().key_at(k)));
      }
      o.as_object().erase(std::string_view("properties"));
      o.set("property_names", std::move(names));
    }
  }
  const u64 total = uint_of(r, "total");
  out.data = JsonValue::object();
  out.data.set("objects", objects);
  finish_page(out.data, page.offset, objects.size(), total);
  out.summary = "Objects " + range_text(page.offset, objects.size(), total) + ".";
  if (out.data.contains("next_cursor")) {
    out.summary += " More: pass cursor \"" + text_of(out.data, "next_cursor") + "\".";
  }
}

bool schema_get(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("doc.get", s, e)) return false;
  note(s, "session", "The session id open_session returned.");
  note(s, "id", "The object's id; objects lists them.");
  require(s, {"session", "id"});
  return true;
}

void run_get(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  JsonValue r;
  if (!b.call("doc.get", args, r, out)) return;
  const JsonValue* p = r.find("properties");
  const std::string parent = text_of(r, "parent");
  const bool root = parent.find_first_not_of('0') == std::string::npos;
  out.summary = text_of(r, "type") + " " + text_of(r, "id") +
                (root ? std::string(" at the root") : ", parent " + parent) +
                ", defined in layer " + text_of(r, "defining_layer") + ", " +
                std::to_string(p != nullptr ? p->size() : 0) + " propert" +
                ((p != nullptr && p->size() == 1) ? "y." : "ies.");
  out.data = r;
}

// ---- mutations ----------------------------------------------------------------------------------

// The session and attribution every single-command mutation takes, and the Command fields it
// exposes, all as the host describes them.
bool mutation_schema(Bridge& b, JsonValue& s, std::initializer_list<const char*> command_fields,
                     std::string& e) {
  JsonValue apply;
  JsonValue cmd;
  if (!b.schemas().params_schema("doc.apply", apply, e)) return false;
  if (!b.schemas().type_schema("engine.doc.Command", cmd, e)) return false;
  s = object_schema();
  if (!copy_prop(apply, s, "session", e)) return false;
  note(s, "session", "The session id open_session returned.");
  for (const char* f : command_fields) {
    if (!copy_prop(cmd, s, f, e)) return false;
  }
  return attribution_schema(b, s, e);
}

bool schema_create_object(Bridge& b, JsonValue& s, std::string& e) {
  if (!mutation_schema(b, s, {"id", "type", "parent"}, e)) return false;
  JsonValue cmd;
  if (!b.schemas().type_schema("engine.doc.Command", cmd, e)) return false;
  if (!copy_prop(cmd, s, "value", e, "properties")) return false;
  note(s, "id",
       "The new object's id, 32 hex digits. Omit it and the bridge generates one and "
       "returns it.");
  note(s, "type",
       "The object's qualified schema type, e.g. engine.content.AssetProvenance; "
       "list_schema lists them and describe explains one.");
  note(s, "parent", "The parent object's id. Omit it for a root object.");
  props(s).find("properties")->set("type", JsonValue("object"));
  note(s, "properties", "Initial property values, keyed by field name of the type.");
  require(s, {"session", "type", "attribution"});
  return true;
}

void run_create_object(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  std::string id = text_of(args, "id");
  if (id.empty()) {
    char hex[Id128::k_hex_length + 1];
    Id128::generate().to_hex(hex);
    id = hex;
  }
  JsonValue c = command("CreateObject", id);
  c.set("type", JsonValue(text_of(args, "type")));
  if (const JsonValue* parent = args.find("parent"); parent != nullptr) c.set("parent", *parent);
  if (const JsonValue* value = args.find("properties"); value != nullptr) c.set("value", *value);
  JsonValue commands = JsonValue::array();
  commands.push_back(std::move(c));
  commit(b, args, std::move(commands), true, "Created " + text_of(args, "type") + " " + id, out);
  if (!out.error) out.data.set("id", JsonValue(id));
}

bool schema_set_property(Bridge& b, JsonValue& s, std::string& e) {
  if (!mutation_schema(b, s, {"id", "name", "value"}, e)) return false;
  note(s, "id", "The object's id.");
  note(s, "name", "The property's name: a field of the object's type (describe lists them).");
  note(s, "value", "The new value, as JSON of the field's type.");
  require(s, {"session", "id", "name", "value", "attribution"});
  return true;
}

void run_set_property(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  const std::string id = text_of(args, "id");
  JsonValue c = command("SetProperty", id);
  c.set("name", JsonValue(text_of(args, "name")));
  c.set("value", args.find("value") != nullptr ? *args.find("value") : JsonValue());
  JsonValue commands = JsonValue::array();
  commands.push_back(std::move(c));
  commit(b, args, std::move(commands), true, "Set " + text_of(args, "name") + " of " + id, out);
}

bool schema_reparent(Bridge& b, JsonValue& s, std::string& e) {
  if (!mutation_schema(b, s, {"id", "parent"}, e)) return false;
  note(s, "id", "The object to move.");
  note(s, "parent", "The new parent's id. Omit it to make the object a root.");
  require(s, {"session", "id", "attribution"});
  return true;
}

void run_reparent(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  const std::string id = text_of(args, "id");
  JsonValue c = command("SetParent", id);
  if (const JsonValue* parent = args.find("parent"); parent != nullptr) c.set("parent", *parent);
  JsonValue commands = JsonValue::array();
  commands.push_back(std::move(c));
  const std::string parent = text_of(args, "parent");
  commit(b, args, std::move(commands), true,
         "Moved " + id + (parent.empty() ? std::string(" to the root") : " under " + parent), out);
}

bool schema_delete_object(Bridge& b, JsonValue& s, std::string& e) {
  if (!mutation_schema(b, s, {"id"}, e)) return false;
  note(s, "id", "The object to delete.");
  require(s, {"session", "id", "attribution"});
  return true;
}

void run_delete_object(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  const std::string id = text_of(args, "id");
  JsonValue commands = JsonValue::array();
  commands.push_back(command("DeleteObject", id));
  commit(b, args, std::move(commands), true, "Deleted " + id, out);
}

bool schema_apply(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("doc.apply", s, e)) return false;
  note(s, "session", "The session id open_session returned.");
  note(s, "commands", "Applied in order to the edit layer, as one transaction.");
  drop(s, "attribution");
  if (!attribution_schema(b, s, e)) return false;
  require(s, {"session", "commands", "attribution"});
  return true;
}

void run_apply(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  const JsonValue* commands = args.find("commands");
  if (commands == nullptr || !commands->is_array() || commands->size() == 0) {
    fail(out, "commands must be a non-empty array",
         "Each command is {\"kind\": \"SetProperty\", \"id\": \"...\", \"name\": \"...\", "
         "\"value\": ...}; the kind enum in the input schema says which fields each kind reads.");
    return;
  }
  const u64 n = commands->size();
  commit(b, args, *commands, bool_of(args, "atomic", true),
         "Applied a transaction of " + std::to_string(n) + " command(s)", out);
}

// ---- undo, redo, journal
// ----------------------------------------------------------------------------

bool schema_step(Bridge& b, const char* method, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema(method, s, e)) return false;
  note(s, "session", "The session id open_session returned.");
  require(s, {"session"});
  return true;
}

bool schema_undo(Bridge& b, JsonValue& s, std::string& e) {
  return schema_step(b, "doc.undo", s, e);
}
bool schema_redo(Bridge& b, JsonValue& s, std::string& e) {
  return schema_step(b, "doc.redo", s, e);
}

void run_step(Bridge& b, const char* method, const char* verb, const JsonValue& args,
              ToolOutcome& out) {
  JsonValue r;
  if (!b.call(method, args, r, out)) return;
  const u64 stepped = uint_of(r, "stepped");
  out.summary = stepped == 0 ? std::string("Nothing to ") + (verb[0] == 'U' ? "undo" : "redo") + "."
                             : std::string(verb) + " " + std::to_string(stepped) + " patch(es).";
  out.summary += " Journal position " + std::to_string(uint_of(r, "position")) + ".";
  out.data = r;
}

void run_undo(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  run_step(b, "doc.undo", "Undid", args, out);
}
void run_redo(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  run_step(b, "doc.redo", "Redid", args, out);
}

constexpr u32 k_journal_default = 20;
constexpr u32 k_journal_max = 200;

bool schema_journal(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("doc.journal", s, e)) return false;
  drop(s, "offset");
  drop(s, "limit");
  note(s, "session", "The session id open_session returned.");
  page_schema(s, k_journal_default, k_journal_max, "patches");
  props(s).set("detail", detail_prop("summary gives each patch's layer, attribution and command "
                                     "counts; full adds the forward and inverse commands.",
                                     "summary"));
  require(s, {"session"});
  return true;
}

void run_journal(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  Page page;
  bool summary = true;
  if (!read_page(args, k_journal_default, k_journal_max, page, out)) return;
  if (!read_detail(args, "summary", summary, out)) return;
  JsonValue params = without(args, {"cursor", "detail", "limit"});
  params.set("offset", JsonValue(page.offset));
  params.set("limit", JsonValue(page.limit));
  JsonValue r;
  if (!b.call("doc.journal", params, r, out)) return;
  JsonValue patches = JsonValue::array();
  if (const JsonValue* list = r.find("patches"); list != nullptr) {
    for (usize i = 0; i < list->size(); ++i) {
      JsonValue p = (*list)[i];
      p.set("index", JsonValue(static_cast<u64>(page.offset + i)));
      if (summary) {
        const JsonValue* forward = p.find("forward");
        const JsonValue* inverse = p.find("inverse");
        p.set("forward_commands", JsonValue(static_cast<u64>(forward ? forward->size() : 0)));
        p.set("inverse_commands", JsonValue(static_cast<u64>(inverse ? inverse->size() : 0)));
        p.as_object().erase(std::string_view("forward"));
        p.as_object().erase(std::string_view("inverse"));
      }
      patches.push_back(std::move(p));
    }
  }
  const u64 total = uint_of(r, "total");
  out.data = JsonValue::object();
  out.data.set("patches", patches);
  out.data.set("position", JsonValue(uint_of(r, "position")));
  finish_page(out.data, page.offset, patches.size(), total);
  out.summary = "Journal patches " + range_text(page.offset, patches.size(), total) +
                "; the first " + std::to_string(uint_of(r, "position")) +
                " are applied and the rest can be redone.";
}

// ---- diff, merge, validate
// ------------------------------------------------------------------------

constexpr u32 k_list_default = 100;
constexpr u32 k_list_max = 1000;

// Pages an array the protocol returns whole: the bridge's cursor over what one call answered.
void page_array(const JsonValue& all, const Page& page, const char* key, JsonValue& data) {
  JsonValue items = JsonValue::array();
  const u64 total = all.is_array() ? all.size() : 0;
  for (u64 i = page.offset; i < total && items.size() < page.limit; ++i)
    items.push_back(all[static_cast<usize>(i)]);
  const u64 returned = items.size();
  data.set(key, std::move(items));
  finish_page(data, page.offset, returned, total);
}

bool schema_diff(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("doc.diff", s, e)) return false;
  note(s, "session", "The session id open_session returned.");
  note(s, "from_layer", "The layer the commands start from.");
  note(s, "to_layer", "The layer the commands would produce.");
  page_schema(s, k_list_default, k_list_max, "commands");
  require(s, {"session", "from_layer", "to_layer"});
  return true;
}

void run_diff(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  Page page;
  if (!read_page(args, k_list_default, k_list_max, page, out)) return;
  JsonValue r;
  if (!b.call("doc.diff", without(args, {"cursor", "limit"}), r, out)) return;
  const JsonValue all = r.find("commands") != nullptr ? *r.find("commands") : JsonValue::array();
  out.data = JsonValue::object();
  page_array(all, page, "commands", out.data);
  const u64 total = all.size();
  out.summary = total == 0
                    ? "The two layers say the same thing: no commands."
                    : std::to_string(total) + " command(s) turn " + text_of(args, "from_layer") +
                          " into " + text_of(args, "to_layer") + "; showing " +
                          range_text(page.offset, out.data.find("commands")->size(), total) + ".";
}

bool schema_merge(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("doc.merge", s, e)) return false;
  note(s, "session", "The session id open_session returned.");
  require(s, {"session", "base_layer", "ours_layer", "theirs_layer", "output_layer"});
  return true;
}

void run_merge(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  JsonValue r;
  if (!b.call("doc.merge", args, r, out)) return;
  const JsonValue* conflicts = r.find("conflicts");
  const u64 n = conflicts != nullptr ? conflicts->size() : 0;
  out.summary = "Merged " + text_of(args, "ours_layer") + " and " + text_of(args, "theirs_layer") +
                " over " + text_of(args, "base_layer") + " into " + text_of(args, "output_layer") +
                ": " + std::to_string(uint_of(r, "applied_ours")) + " change(s) from ours, " +
                std::to_string(uint_of(r, "applied_theirs")) + " from theirs, " +
                std::to_string(n) + " conflict(s).";
  out.summary += bool_of(r, "committed")
                     ? " Committed as journal patch " + std::to_string(uint_of(r, "patch_index")) +
                           "; undo reverts it."
                     : " Nothing changed, so nothing was committed.";
  out.data = r;
}

bool schema_validate(Bridge& b, JsonValue& s, std::string& e) {
  if (!schema_session_ref(b, "doc.validate", s, e)) return false;
  page_schema(s, k_list_default, k_list_max, "diagnostics");
  return true;
}

void run_validate(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  Page page;
  if (!read_page(args, k_list_default, k_list_max, page, out)) return;
  JsonValue r;
  if (!b.call("doc.validate", without(args, {"cursor", "limit"}), r, out)) return;
  const JsonValue all =
      r.find("diagnostics") != nullptr ? *r.find("diagnostics") : JsonValue::array();
  out.data = JsonValue::object();
  out.data.set("ok", JsonValue(bool_of(r, "ok")));
  page_array(all, page, "diagnostics", out.data);
  out.summary =
      bool_of(r, "ok")
          ? "The document is valid."
          : std::to_string(all.size()) + " problem(s); showing " +
                range_text(page.offset, out.data.find("diagnostics")->size(), all.size()) + ".";
}

// ---- schema discovery
// ----------------------------------------------------------------------------

// Which tools call which protocol methods, for list_schema's catalogue.
JsonValue tools_calling(std::string_view method) {
  JsonValue names = JsonValue::array();
  for (const ToolDef& t : tool_table()) {
    std::string_view m = t.methods;
    while (!m.empty()) {
      const usize space = m.find(' ');
      if (m.substr(0, space) == method) {
        names.push_back(JsonValue(t.name));
        break;
      }
      if (space == std::string_view::npos) break;
      m.remove_prefix(space + 1);
    }
  }
  return names;
}

bool schema_describe(Bridge& b, JsonValue& s, std::string& e) {
  JsonValue described;
  if (!b.schemas().params_schema("schema.describe", described, e)) return false;
  s = object_schema();
  if (!copy_prop(described, s, "type", e)) return false;
  note(s, "type", "A qualified schema type, e.g. engine.doc.Command; list_schema lists them.");
  props(s).set("method", prop("string",
                              "A protocol method, e.g. doc.apply: its documentation and "
                              "its parameter and result types, described."));
  return true;
}

void run_describe(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  const std::string type = text_of(args, "type");
  const std::string method = text_of(args, "method");
  if (type.empty() == method.empty()) {
    fail(out, "describe takes one of `type` and `method`",
         "type is a schema type such as engine.doc.Command; method is a protocol method such as "
         "doc.apply.");
    return;
  }
  const auto describe_one = [&](const std::string& name, JsonValue& d) {
    JsonValue params = JsonValue::object();
    params.set("type", JsonValue(name));
    JsonValue r;
    if (!b.call("schema.describe", params, r, out)) return false;
    d = r.find("description") != nullptr ? *r.find("description") : JsonValue();
    return true;
  };
  if (!type.empty()) {
    JsonValue d;
    if (!describe_one(type, d)) return;
    const JsonValue* fields = d.find("fields");
    const JsonValue* values = d.find("values");
    out.summary =
        type + ": " + text_of(d, "kind") + " version " + std::to_string(uint_of(d, "version")) +
        ", " +
        (fields != nullptr ? std::to_string(fields->size()) + " field(s)."
                           : std::to_string(values != nullptr ? values->size() : 0) + " value(s).");
    out.data = JsonValue::object();
    out.data.set("type", std::move(d));
    return;
  }
  const SchemaGen::Method* m = b.schemas().method(method);
  if (m == nullptr) {
    fail(out, "the host has no method " + method,
         "list_schema lists every method with the tools that call it.");
    return;
  }
  out.data = JsonValue::object();
  out.data.set("method", JsonValue(m->name));
  out.data.set("doc", JsonValue(m->doc));
  out.data.set("params_type", JsonValue(m->params_type));
  out.data.set("result_type", JsonValue(m->result_type));
  out.data.set("tools", tools_calling(m->name));
  JsonValue d;
  if (!m->params_type.empty()) {
    if (!describe_one(m->params_type, d)) return;
    out.data.set("params", std::move(d));
  }
  if (!m->result_type.empty()) {
    if (!describe_one(m->result_type, d)) return;
    out.data.set("result", std::move(d));
  }
  out.summary = m->name + ": " + m->doc + " Params " +
                (m->params_type.empty() ? std::string("none") : m->params_type) + ", result " +
                m->result_type + ".";
}

constexpr u32 k_types_default = 200;
constexpr u32 k_types_max = 1000;

bool schema_list_schema(Bridge&, JsonValue& s, std::string&) {
  s = object_schema();
  props(s).set("namespace",
               prop("string",
                    "Only types whose qualified name starts with this, e.g. engine.doc "
                    "or engine.content. Omit it for every type."));
  JsonValue methods = prop("boolean",
                           "Also list the protocol's methods, with the tools that call "
                           "each one.");
  methods.set("default", JsonValue(true));
  props(s).set("methods", std::move(methods));
  page_schema(s, k_types_default, k_types_max, "types");
  return true;
}

void run_list_schema(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  Page page;
  if (!read_page(args, k_types_default, k_types_max, page, out)) return;
  JsonValue r;
  if (!b.call("schema.types", JsonValue(), r, out)) return;
  const std::string ns = text_of(args, "namespace");
  JsonValue matching = JsonValue::array();
  if (const JsonValue* types = r.find("types"); types != nullptr) {
    for (usize i = 0; i < types->size(); ++i) {
      std::string_view name;
      if ((*types)[i].get_string(name) && name.starts_with(ns)) matching.push_back((*types)[i]);
    }
  }
  out.data = JsonValue::object();
  page_array(matching, page, "types", out.data);
  const u64 total = matching.size();
  out.summary = std::to_string(total) + " schema type(s)" +
                (ns.empty() ? std::string() : " under " + ns) + "; showing " +
                range_text(page.offset, out.data.find("types")->size(), total) + ".";
  if (bool_of(args, "methods", true)) {
    JsonValue methods = JsonValue::array();
    for (const SchemaGen::Method& m : b.schemas().methods()) {
      JsonValue o = JsonValue::object();
      o.set("name", JsonValue(m.name));
      o.set("doc", JsonValue(m.doc));
      o.set("params_type", JsonValue(m.params_type));
      o.set("result_type", JsonValue(m.result_type));
      o.set("tools", tools_calling(m.name));
      methods.push_back(std::move(o));
    }
    out.summary += " " + std::to_string(methods.size()) + " protocol method(s).";
    out.data.set("methods", std::move(methods));
  }
}

// ---- rendering
// -------------------------------------------------------------------------------------

// `scene` and `load`, which every tool that renders a loaded scene takes.
bool scene_schema(Bridge& b, JsonValue& s, std::string& e) {
  note(s, "scene",
       "A scene id an earlier capture, benchmark or evaluate returned (\"scene1\"). Omit it and "
       "give load instead. Scene ids live as long as the host.");
  JsonValue load;
  if (!b.schemas().params_schema("render.load", load, e)) return false;
  load.set("description",
           JsonValue("What to render, loaded now and kept by the host: exactly one of mesh (a "
                     "glTF, GLB or .clusters file) and scene (a scene file), or neither for a "
                     "procedural scene. The same load twice is loaded once. settings are the "
                     "renderer's knobs, spelled as engine-view's flags."));
  props(s).set("load", std::move(load));
  return true;
}

void scene_data(ToolOutcome& out, const std::string& scene, const JsonValue& info, bool reused) {
  out.data.set("scene", JsonValue(scene));
  if (!info.is_null()) {
    JsonValue loaded = info;
    loaded.set("reused", JsonValue(reused));
    out.data.set("loaded", std::move(loaded));
  }
}

std::string scene_text(const std::string& scene, const JsonValue& info, bool reused) {
  if (info.is_null()) return "Scene " + scene + ".";
  return std::string(reused ? "Reused" : "Loaded") + " scene " + scene + " (" +
         std::to_string(uint_of(info, "instances")) + " instance(s), " +
         std::to_string(uint_of(info, "triangles")) + " triangles, " +
         std::to_string(uint_of(info, "clusters")) + " clusters; raster " +
         text_of(info, "raster") + ", shadows " + text_of(info, "shadows") + ", on " +
         text_of(info, "adapter") + ").";
}

// Files a result names by channel, as links and as a URI map.
void add_files(ToolOutcome& out, const JsonValue& files, std::string_view name) {
  if (!files.is_object()) return;
  for (u32 i = 0; i < files.as_object().size(); ++i) {
    std::string_view uri;
    if (!files.as_object().value_at(i).get_string(uri)) continue;
    link(out, std::string(uri), std::string(name) + " " + files.as_object().key_at(i),
         mime_of(uri));
  }
}

std::string files_text(const JsonValue& files) {
  std::string s;
  if (!files.is_object()) return s;
  for (u32 i = 0; i < files.as_object().size(); ++i) {
    std::string_view uri;
    if (!files.as_object().value_at(i).get_string(uri)) continue;
    s += "\n  " + files.as_object().key_at(i) + ": " + std::string(uri);
  }
  return s;
}

bool schema_capture(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("render.capture", s, e)) return false;
  drop(s, "out_dir");
  if (!scene_schema(b, s, e)) return false;
  note(s, "name",
       "Files land in <workspace>/captures. Omitted, the bridge picks capture-NNN, the "
       "first number not yet used there.");
  return true;
}

void run_capture(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  std::string scene;
  JsonValue info;
  bool reused = false;
  if (!b.scene_for(args, scene, info, reused, out)) return;
  std::string dir;
  std::string name;
  if (!workspace_dir(b, "captures", dir, out)) return;
  if (!output_name(args, dir, "capture", name, out)) return;
  JsonValue params = without(args, {"load", "name"});
  params.set("scene", JsonValue(scene));
  params.set("out_dir", JsonValue(dir));
  params.set("name", JsonValue(name));
  JsonValue r;
  if (!b.call("render.capture", params, r, out)) return;
  const std::string result_path = dir + "/" + name + ".json";
  if (!write_json_file(result_path, r, out)) return;

  JsonValue files = r.find("files") != nullptr ? *r.find("files") : JsonValue::object();
  files.set("result", JsonValue(file_uri(result_path)));
  const JsonValue* stats = r.find("stats");
  const u64 width = uint_of(r, "width");
  const u64 height = uint_of(r, "height");
  const u64 covered = uint_of(r, "covered");
  out.data = JsonValue::object();
  scene_data(out, scene, info, reused);
  out.data.set("name", JsonValue(name));
  out.data.set("width", JsonValue(width));
  out.data.set("height", JsonValue(height));
  out.data.set("covered", JsonValue(covered));
  out.data.set("integrator", JsonValue(text_of(r, "integrator", "realtime")));
  if (stats != nullptr) {
    out.data.set("visible_pairs", JsonValue(uint_of(*stats, "visible_pairs")));
    out.data.set("shadow_casters", JsonValue(uint_of(*stats, "shadow_casters")));
  }
  if (text_of(r, "integrator") == "reference") {
    out.data.set("samples", JsonValue(uint_of(r, "samples")));
    out.data.set("seconds", JsonValue(real_of(r, "seconds")));
  }
  out.data.set("files", files);
  const f64 pixels = static_cast<f64>(width * height);
  out.summary = scene_text(scene, info, reused) + " Captured " + std::to_string(width) + "x" +
                std::to_string(height) + " as " + name + ": " +
                (stats != nullptr ? std::to_string(uint_of(*stats, "visible_pairs")) : "?") +
                " visible cluster pairs, " +
                fixed(pixels > 0 ? 100.0 * static_cast<f64>(covered) / pixels : 0.0, 1) +
                "% of pixels covered. Files (read them with your own tools):" + files_text(files);
  add_files(out, files, name);
}

// "Measured beside other work" when the harness's thresholds say so (docs/subsystems/bench.md):
// others above 10% of the CPU or the GPU above 20% busy at either end of the run.
std::string machine_text(const JsonValue& result) {
  const JsonValue* span = result.find("machine_state");
  if (span == nullptr || !span->is_object()) return {};
  bool busy = false;
  bool measured = false;
  for (const char* end : {"start", "end"}) {
    const JsonValue* m = span->find(end);
    if (m == nullptr) continue;
    const JsonValue* cpu = m->find("cpu_others_pct");
    const JsonValue* gpu = m->find("gpu_util_pct");
    f64 v = 0.0;
    if (cpu != nullptr && cpu->get_f64(v)) {
      measured = true;
      busy = busy || v > 10.0;
    }
    if (gpu != nullptr && gpu->get_f64(v)) {
      measured = true;
      busy = busy || v > 20.0;
    }
  }
  if (!measured) return " The machine's load could not be measured.";
  return busy ? " Other work was running on the machine (see machine_state): these numbers are "
                "upper bounds, not costs."
              : " The machine was quiet.";
}

bool schema_benchmark(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("render.benchmark", s, e)) return false;
  if (!scene_schema(b, s, e)) return false;
  props(s).set("name", prop("string",
                            "Base name of the files in <workspace>/benchmarks. "
                            "Omitted, the bridge picks benchmark-NNN."));
  note(s, "camera_path",
       "A path file relative to the directory engine-mcp was started in; with "
       "one, the flythrough summary is also written as <name>.jsonl.");
  return true;
}

void run_benchmark(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  std::string scene;
  JsonValue info;
  bool reused = false;
  if (!b.scene_for(args, scene, info, reused, out)) return;
  std::string dir;
  std::string name;
  if (!workspace_dir(b, "benchmarks", dir, out)) return;
  if (!output_name(args, dir, "benchmark", name, out)) return;
  JsonValue params = without(args, {"load", "name"});
  params.set("scene", JsonValue(scene));
  JsonValue r;
  if (!b.call("render.benchmark", params, r, out)) return;
  const std::string result_path = dir + "/" + name + ".json";
  if (!write_json_file(result_path, r, out)) return;
  JsonValue files = JsonValue::object();
  files.set("result", JsonValue(file_uri(result_path)));
  const JsonValue* flythrough = r.find("flythrough");
  if (flythrough != nullptr && flythrough->is_object()) {
    // The same last line engine-view's --benchmark file ends with, so one reader serves both.
    const std::string jsonl_path = dir + "/" + name + ".jsonl";
    std::string line = write_json(*flythrough, JsonWriteOptions{.pretty = false});
    line.push_back('\n');
    if (!write_file(jsonl_path, line, out)) return;
    files.set("flythrough", JsonValue(file_uri(jsonl_path)));
  }

  const JsonValue* stats = r.find("stats");
  const JsonValue* gpu = stats != nullptr ? stats->find("gpu_ms") : nullptr;
  out.data = JsonValue::object();
  scene_data(out, scene, info, reused);
  out.data.set("name", JsonValue(name));
  out.data.set("width", JsonValue(uint_of(r, "width")));
  out.data.set("height", JsonValue(uint_of(r, "height")));
  out.data.set("raster", JsonValue(text_of(r, "raster")));
  out.data.set("shadows", JsonValue(text_of(r, "shadows")));
  out.data.set("seconds", JsonValue(real_of(r, "seconds")));
  if (stats != nullptr) {
    out.data.set("frames", JsonValue(uint_of(*stats, "frames")));
    out.data.set("cpu_ms_per_frame", JsonValue(real_of(*stats, "cpu_ms_per_frame")));
    out.data.set("visible_pairs", JsonValue(uint_of(*stats, "visible_pairs")));
    if (gpu != nullptr) out.data.set("gpu_ms", *gpu);
  }
  if (const JsonValue* m = r.find("machine_state"); m != nullptr) out.data.set("machine_state", *m);
  if (flythrough != nullptr && flythrough->is_object()) {
    JsonValue f = JsonValue::object();
    for (const char* key : {"frames", "repeats", "deterministic", "gpu_ms", "visible_pairs"}) {
      if (const JsonValue* v = flythrough->find(key); v != nullptr) f.set(key, *v);
    }
    out.data.set("flythrough", std::move(f));
  }
  out.data.set("files", files);

  std::string passes;
  if (gpu != nullptr && gpu->is_object()) {
    for (const char* pass : {"cull", "hw", "sw", "hiz", "resolve", "rt", "deform", "trace"}) {
      const f64 ms = real_of(*gpu, pass);
      if (ms > 0.0) passes += std::string(passes.empty() ? "" : ", ") + pass + " " + fixed(ms, 3);
    }
  }
  out.summary = scene_text(scene, info, reused) + " Benchmarked " +
                std::to_string(uint_of(r, "width")) + "x" + std::to_string(uint_of(r, "height")) +
                ", " + (stats != nullptr ? std::to_string(uint_of(*stats, "frames")) : "?") +
                " frames: GPU " + (gpu != nullptr ? fixed(real_of(*gpu, "total"), 3) : "?") +
                " ms a frame" + (passes.empty() ? "" : " (" + passes + ")") + ", CPU " +
                (stats != nullptr ? fixed(real_of(*stats, "cpu_ms_per_frame"), 3) : "?") +
                " ms a frame, " +
                (stats != nullptr ? std::to_string(uint_of(*stats, "visible_pairs")) : "?") +
                " visible cluster pairs." + machine_text(r) + " Files:" + files_text(files);
  add_files(out, files, name);
}

bool schema_compare(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("render.compare", s, e)) return false;
  drop(s, "flip_out");
  note(s, "a", "An image path, or a file:// URI another tool returned.");
  note(s, "b", "The image to compare against, the same way.");
  JsonValue heatmap = prop("boolean",
                           "Write the per-pixel FLIP error as a heat map PNG into "
                           "<workspace>/compares.");
  heatmap.set("default", JsonValue(true));
  props(s).set("heatmap", std::move(heatmap));
  props(s).set("name", prop("string",
                            "Base name of the files in <workspace>/compares. Omitted, "
                            "the bridge picks compare-NNN."));
  require(s, {"a", "b"});
  return true;
}

std::string metrics_text(const JsonValue& m) {
  if (bool_of(m, "identical")) return "identical images (FLIP 0, SSIM 1)";
  const JsonValue* psnr = m.find("psnr");
  f64 p = 0.0;
  return "FLIP mean " + fixed(real_of(m, "flip_mean"), 4) + " (p95 " +
         fixed(real_of(m, "flip_p95"), 4) + ", max " + fixed(real_of(m, "flip_max"), 4) +
         "), PSNR " + (psnr != nullptr && psnr->get_f64(p) ? fixed(p, 2) + " dB" : "n/a") +
         ", SSIM " + fixed(real_of(m, "ssim"), 5);
}

void run_compare(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  std::string dir;
  std::string name;
  if (!workspace_dir(b, "compares", dir, out)) return;
  if (!output_name(args, dir, "compare", name, out)) return;
  JsonValue params = without(args, {"heatmap", "name"});
  params.set("a", JsonValue(path_of_uri(text_of(args, "a"))));
  params.set("b", JsonValue(path_of_uri(text_of(args, "b"))));
  if (bool_of(args, "heatmap", true)) {
    params.set("flip_out", JsonValue(dir + "/" + name + ".flip.png"));
  }
  JsonValue r;
  if (!b.call("render.compare", params, r, out)) return;
  const std::string result_path = dir + "/" + name + ".json";
  if (!write_json_file(result_path, r, out)) return;
  JsonValue files = JsonValue::object();
  if (!text_of(r, "flip_image").empty()) files.set("flip", JsonValue(text_of(r, "flip_image")));
  files.set("result", JsonValue(file_uri(result_path)));
  out.data = r;
  out.data.set("name", JsonValue(name));
  out.data.set("files", files);
  out.summary = std::to_string(uint_of(r, "width")) + "x" + std::to_string(uint_of(r, "height")) +
                ": " + metrics_text(r) + ". Files:" + files_text(files);
  add_files(out, files, name);
}

bool schema_evaluate(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("render.evaluate", s, e)) return false;
  drop(s, "out_dir");
  if (!scene_schema(b, s, e)) return false;
  note(s, "name",
       "Files land in <workspace>/evaluations. Omitted, the bridge picks "
       "evaluate-NNN.");
  return true;
}

void run_evaluate(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  std::string scene;
  JsonValue info;
  bool reused = false;
  if (!b.scene_for(args, scene, info, reused, out)) return;
  std::string dir;
  std::string name;
  if (!workspace_dir(b, "evaluations", dir, out)) return;
  if (!output_name(args, dir, "evaluate", name, out)) return;
  JsonValue params = without(args, {"load", "name"});
  params.set("scene", JsonValue(scene));
  params.set("out_dir", JsonValue(dir));
  params.set("name", JsonValue(name));
  JsonValue r;
  if (!b.call("render.evaluate", params, r, out)) return;
  const std::string result_path = dir + "/" + name + ".json";
  if (!write_json_file(result_path, r, out)) return;
  JsonValue files = r.find("files") != nullptr ? *r.find("files") : JsonValue::object();
  files.set("result", JsonValue(file_uri(result_path)));
  out.data = JsonValue::object();
  scene_data(out, scene, info, reused);
  out.data.set("name", JsonValue(name));
  for (const char* key : {"width", "height", "full", "direct", "samples", "reference_seconds",
                          "realtime_seconds", "machine_state"}) {
    if (const JsonValue* v = r.find(key); v != nullptr) out.data.set(key, *v);
  }
  out.data.set("files", files);
  const JsonValue* full = r.find("full");
  const JsonValue* direct = r.find("direct");
  out.summary = scene_text(scene, info, reused) + " Real-time against the reference (" +
                std::to_string(uint_of(r, "samples")) +
                " samples a pixel): " + (full != nullptr ? metrics_text(*full) : std::string("?")) +
                ".";
  if (direct != nullptr && direct->is_object()) {
    out.summary += " Against the direct-light reference: " + metrics_text(*direct) +
                   "; the gap between the two is the indirect light the real-time path lacks.";
  }
  out.summary += machine_text(r) + " Files:" + files_text(files);
  add_files(out, files, name);
}

// ---- machine and host
// -------------------------------------------------------------------------------

bool schema_adapters(Bridge& b, JsonValue& s, std::string& e) {
  return b.schemas().params_schema("gpu.adapters", s, e);
}

void run_adapters(Bridge& b, const JsonValue&, ToolOutcome& out) {
  JsonValue r;
  if (!b.call("gpu.adapters", JsonValue(), r, out)) return;
  std::string dir;
  if (!workspace_dir(b, "", dir, out)) return;
  const std::string report = dir + "/adapters.json";
  if (!write_json_file(report, r, out)) return;
  JsonValue adapters = JsonValue::array();
  const JsonValue* list = r.find("adapters");
  std::string lines;
  for (usize i = 0; list != nullptr && i < list->size(); ++i) {
    const JsonValue& a = (*list)[i];
    const JsonValue verdict = a.find("verdict") != nullptr ? *a.find("verdict") : JsonValue();
    JsonValue o = JsonValue::object();
    for (const char* key :
         {"name", "vendor", "type", "api_version", "driver_name", "driver_info", "tier"}) {
      if (const JsonValue* v = a.find(key); v != nullptr) o.set(key, *v);
    }
    o.set("device_local_mib", JsonValue(uint_of(a, "device_local_bytes") / (1024u * 1024u)));
    JsonValue v = JsonValue::object();
    for (const char* key : {"tier", "usable", "blocking", "degraded"}) {
      if (const JsonValue* x = verdict.find(key); x != nullptr) v.set(key, *x);
    }
    o.set("verdict", std::move(v));
    adapters.push_back(std::move(o));
    lines +=
        "\n  " + text_of(a, "name") + " (" + text_of(a, "type") + ", Vulkan " +
        text_of(a, "api_version") + "): " +
        (bool_of(verdict, "usable") ? "the renderer runs here at tier " + text_of(verdict, "tier")
                                    : std::string("the renderer cannot run here"));
    for (const char* key : {"blocking", "degraded"}) {
      const JsonValue* reasons = verdict.find(key);
      for (usize k = 0; reasons != nullptr && k < reasons->size(); ++k) {
        std::string_view reason;
        if ((*reasons)[k].get_string(reason)) lines += "\n    " + std::string(reason);
      }
    }
  }
  out.data = JsonValue::object();
  out.data.set("available", JsonValue(bool_of(r, "available")));
  if (!text_of(r, "error").empty()) out.data.set("error", JsonValue(text_of(r, "error")));
  out.data.set("adapters", adapters);
  out.data.set("report", JsonValue(file_uri(report)));
  if (!bool_of(r, "available") || adapters.size() == 0) {
    out.summary = "No GPU on this machine: " + text_of(r, "error", "no Vulkan device") +
                  ". Render tools will answer the same; document tools work.";
  } else {
    out.summary = std::to_string(adapters.size()) + " Vulkan device(s):" + lines;
  }
  out.summary += "\nFull report: " + file_uri(report);
  link(out, file_uri(report), "adapters report", "application/json");
}

constexpr u32 k_logs_default = 100;
constexpr u32 k_logs_max = 2000;

i32 level_rank(std::string_view level) {
  constexpr const char* k_levels[] = {"trace", "debug", "info", "warn", "error", "fatal"};
  for (i32 i = 0; i < 6; ++i) {
    if (level == k_levels[i]) return i;
  }
  return -1;
}

bool schema_get_logs(Bridge& b, JsonValue& s, std::string& e) {
  if (!b.schemas().params_schema("log.tail", s, e)) return false;
  set_default(s, "limit", JsonValue(k_logs_default));
  if (JsonValue* limit = props(s).find("limit"); limit != nullptr) {
    limit->set("maximum", JsonValue(k_logs_max));
  }
  note(s, "since", "Pass the `next` a previous call returned to read only what is new.");
  JsonValue level = prop("string",
                         "Drop records below this level. The host's ring keeps every "
                         "level from debug up.");
  JsonValue levels = JsonValue::array();
  for (const char* l : {"trace", "debug", "info", "warn", "error"})
    levels.push_back(JsonValue(l));
  level.set("enum", std::move(levels));
  level.set("default", JsonValue("trace"));
  props(s).set("min_level", std::move(level));
  props(s).set("category", prop("string",
                                "Keep only records whose category starts with this, "
                                "e.g. host.render."));
  return true;
}

void run_get_logs(Bridge& b, const JsonValue& args, ToolOutcome& out) {
  const u64 limit = uint_of(args, "limit", k_logs_default);
  if (limit == 0 || limit > k_logs_max) {
    fail(out, "limit must be between 1 and " + std::to_string(k_logs_max), "");
    return;
  }
  const std::string min_level = text_of(args, "min_level", "trace");
  const i32 min_rank = level_rank(min_level);
  if (min_rank < 0) {
    fail(out, "min_level must be trace, debug, info, warn or error; got '" + min_level + "'", "");
    return;
  }
  JsonValue params = without(args, {"min_level", "category"});
  params.set("limit", JsonValue(limit));
  JsonValue r;
  if (!b.call("log.tail", params, r, out)) return;
  const std::string category = text_of(args, "category");
  JsonValue records = JsonValue::array();
  u64 read = 0;
  if (const JsonValue* all = r.find("records"); all != nullptr) {
    read = all->size();
    for (usize i = 0; i < all->size(); ++i) {
      const JsonValue& rec = (*all)[i];
      if (level_rank(text_of(rec, "level")) < min_rank) continue;
      if (!std::string_view(text_of(rec, "cat")).starts_with(category)) continue;
      records.push_back(rec);
    }
  }
  const u64 next = uint_of(r, "next");
  out.data = JsonValue::object();
  out.data.set("records", records);
  out.data.set("next", JsonValue(next));
  out.summary = std::to_string(records.size()) + " record(s) of " + std::to_string(read) +
                " read; pass since " + std::to_string(next) + " to continue.";
}

bool schema_host_info(Bridge& b, JsonValue& s, std::string& e) {
  return b.schemas().params_schema("engine.info", s, e);
}

void run_host_info(Bridge& b, const JsonValue&, ToolOutcome& out) {
  JsonValue r;
  if (!b.call("engine.info", JsonValue(), r, out)) return;
  JsonValue bridge = JsonValue::object();
  bridge.set("workspace", JsonValue(b.options().workspace));
  bridge.set("workspace_uri", JsonValue(file_uri(b.options().workspace)));
  bridge.set("actor", JsonValue(b.options().actor));
  bridge.set("host_path", JsonValue(b.options().host_path));
  bridge.set("host_generation", JsonValue(b.host().generation()));
  bridge.set("protocol_version", JsonValue(b.protocol_version()));
  out.data = JsonValue::object();
  out.data.set("host", r);
  out.data.set("bridge", std::move(bridge));
  out.summary = "engine-host " + text_of(r, "version") + " (" + text_of(r, "build") + "), pid " +
                std::to_string(uint_of(r, "pid")) + ", " + std::to_string(uint_of(r, "methods")) +
                " methods. Workspace " + b.options().workspace + "; default actor '" +
                b.options().actor + "'; MCP " + std::string(b.protocol_version()) + ".";
}

// ---- the table ----------------------------------------------------------------------------------

constexpr ToolDef k_tools[] = {
    {"open_session", "Open a document",
     "Open a document directory and get the session id the document tools take. Pass create: "
     "true to make a new document (one base layer) where there is none. Opening a document this "
     "host already has open returns the same session.",
     "session.open", false, false, true, &schema_open_session, &run_open_session},
    {"close_session", "Close a document",
     "Close a session. Everything it committed is already on disk; this only releases the "
     "host's copy.",
     "session.close", false, false, true, &schema_close_session, &run_close_session},
    {"list_sessions", "List open documents",
     "The sessions this host has open: each document's path, name, layers, journal length and "
     "undo position.",
     "session.list", true, false, true, &schema_list_sessions, &run_list_sessions},
    {"layers", "List layers",
     "A document's layer stack, weakest first: each layer's role, record count and storage form, "
     "and which one edits go to.",
     "doc.layers", true, false, true, &schema_layers, &run_layers},
    {"add_layer", "Add a layer",
     "Append a layer on top of the stack, where it overrides everything below. It becomes the "
     "edit layer unless edit is false. A partition stores it as tile files.",
     "doc.add_layer", false, false, false, &schema_add_layer, &run_add_layer},
    {"set_edit_layer", "Choose the edit layer",
     "Choose the layer that create_object, set_property, reparent, delete_object and apply "
     "write to.",
     "doc.set_edit_layer", false, false, true, &schema_set_edit_layer, &run_set_edit_layer},
    {"objects", "List objects",
     "Live objects with their composed properties (every layer applied), filtered by type or "
     "parent, a page at a time: pass the next_cursor a call returned to get the next page. "
     "detail \"summary\" leaves the values out.",
     "doc.objects", true, false, true, &schema_objects, &run_objects},
    {"get", "Get an object",
     "One live object composed across the layers: its type, parent, every property, and the "
     "layer that defines it.",
     "doc.get", true, false, true, &schema_get, &run_get},
    {"create_object", "Create an object",
     "Create one object in the edit layer as one undoable transaction, with initial properties. "
     "The id is generated when omitted and returned. Needs attribution.rationale.",
     "doc.apply", false, false, false, &schema_create_object, &run_create_object},
    {"set_property", "Set a property",
     "Set one property of an object in the edit layer (an override, when a weaker layer defines "
     "the object) as one undoable transaction. Needs attribution.rationale.",
     "doc.apply", false, false, true, &schema_set_property, &run_set_property},
    {"reparent", "Move an object",
     "Put an object under another parent, or at the root when parent is omitted, as one undoable "
     "transaction. A move that would make a cycle is refused. Needs attribution.rationale.",
     "doc.apply", false, false, true, &schema_reparent, &run_reparent},
    {"delete_object", "Delete an object",
     "Delete an object as one undoable transaction: removed when the edit layer defines it, "
     "hidden by a tombstone when a weaker layer does. Needs attribution.rationale.",
     "doc.apply", false, true, true, &schema_delete_object, &run_delete_object},
    {"apply", "Apply commands",
     "Apply a batch of commands as one transaction: CreateObject, DeleteObject, SetProperty, "
     "ClearProperty, SetParent, RemoveRecord or RestoreRecord (the kind enum says what each "
     "reads). With atomic (the default) one failing command rolls back the batch and every "
     "diagnostic is reported. Needs attribution.rationale.",
     "doc.apply", false, true, false, &schema_apply, &run_apply},
    {"undo", "Undo",
     "Undo the last committed transactions (steps, default 1). Saved to disk at once; redo "
     "brings them back until the next commit.",
     "doc.undo", false, false, false, &schema_undo, &run_undo},
    {"redo", "Redo", "Redo transactions undone since the last commit.", "doc.redo", false, false,
     false, &schema_redo, &run_redo},
    {"journal", "Read the journal",
     "Committed transactions, oldest first, with who made each and why (actor, role, task, "
     "rationale), a page at a time. detail \"full\" adds each patch's forward and inverse "
     "commands.",
     "doc.journal", true, false, true, &schema_journal, &run_journal},
    {"diff", "Diff two layers",
     "The commands that would turn one layer into another: what the second says that the first "
     "does not. A page at a time.",
     "doc.diff", true, false, true, &schema_diff, &run_diff},
    {"merge_layers", "Merge layers",
     "Three-way structural merge, per object and per property: the changes ours and theirs each "
     "made from base, written into output_layer as one undoable transaction. Conflicts are "
     "reported rather than refused, and prefer says which side wins them. The host records its "
     "own attribution for a merge.",
     "doc.merge", false, true, false, &schema_merge, &run_merge},
    {"validate", "Validate the document",
     "Check every record against the schema registry — unknown types and properties, values of "
     "the wrong type, missing parents, cycles — and list the problems a page at a time.",
     "doc.validate", true, false, true, &schema_validate, &run_validate},
    {"describe", "Describe a type or method",
     "Explain a schema type (its fields with their types, defaults and documentation, or an "
     "enum's values) or a protocol method (its documentation and its parameter and result types, "
     "described).",
     "schema.describe", true, false, true, &schema_describe, &run_describe},
    {"list_schema", "List types and methods",
     "The schema types the engine knows — the object types create_object takes among them — "
     "filtered by namespace and a page at a time, and the protocol's method catalogue with the "
     "tools that call each method.",
     "schema.types engine.methods", true, false, true, &schema_list_schema, &run_list_schema},
    {"capture", "Capture a frame",
     "Render one frame offscreen into the workspace: color as PNG, and any of ids (the entity-ID "
     "buffer: instance, cluster and triangle per pixel), depth and normals. Give load (a mesh, a "
     "scene file or a procedural scene, with render settings) or the scene id an earlier render "
     "call returned; the same load twice is loaded once. Returns file:// URIs to read with your "
     "own tools, never pixels.",
     "render.load render.capture", false, false, true, &schema_capture, &run_capture},
    {"benchmark", "Benchmark a scene",
     "Render a scene for a number of frames with the camera still, or fly a camera path, and "
     "report GPU milliseconds per pass, CPU milliseconds per frame, visible cluster counts and "
     "what else the machine was doing. The whole result goes to <workspace>/benchmarks/"
     "<name>.json. Numbers taken beside other GPU work are upper bounds.",
     "render.load render.benchmark", false, false, false, &schema_benchmark, &run_benchmark},
    {"compare", "Compare two images",
     "The perceptual difference between two images — FLIP, the primary number, with PSNR and "
     "SSIM — and a FLIP heat map in the workspace. Takes paths or the file:// URIs other tools "
     "returned. Needs no GPU.",
     "render.compare", false, false, true, &schema_compare, &run_compare},
    {"evaluate", "Evaluate against the reference",
     "Render the same frame through the real-time path and the reference path tracer, compare "
     "them, and write both pictures and the heat maps into the workspace: the accept-or-reject "
     "measurement of plan 04 section 4.8 in one call. Needs a GPU with cluster acceleration "
     "structures (NVIDIA RTX).",
     "render.load render.evaluate", false, false, false, &schema_evaluate, &run_evaluate},
    {"get_logs", "Read the host's log",
     "Log records from the host's ring, oldest first, from a sequence number on; pass the "
     "returned next as since to continue. Filter by minimum level and category.",
     "log.tail", true, false, true, &schema_get_logs, &run_get_logs},
    {"adapters", "List GPUs",
     "The machine's Vulkan devices and whether the renderer can run on each: its tier, and what "
     "blocks or degrades it. The full requirements report goes to <workspace>/adapters.json.",
     "gpu.adapters", true, false, true, &schema_adapters, &run_adapters},
    {"host_info", "About the host",
     "What the bridge is talking to: the engine-host's version, build, process id and method "
     "count, the workspace directory, the default actor, and the MCP version agreed.",
     "engine.info", true, false, true, &schema_host_info, &run_host_info},
};

}  // namespace

std::span<const ToolDef> tool_table() noexcept { return {k_tools, std::size(k_tools)}; }

}  // namespace engine::mcp
