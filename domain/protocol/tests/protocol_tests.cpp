#include <core/json/json.h>
#include <core/log/log.h>
#include <domain/protocol/paths.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/io/roots.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::protocol;

// One unguessable scratch directory per object, so a second copy of this binary cannot delete
// this one's session documents (tests/support/test_temp_dir.h).
using TempDir = test::TempDir;

namespace {

struct Host {
  io::Vfs vfs;
  SessionManager sessions{vfs};
  log::RingSink ring{64};
  // Every path a case names is under a TempDir, and every TempDir under one parent: that parent is
  // the host's one writable root, as an operator's --root would be (paths.h).
  test::TempDir roots_anchor{"roots"};
  io::Roots roots;
  Dispatcher dispatcher{Context{&sessions, &ring, nullptr}};
  Host() {
    (void)roots.add("tests", std::string(io::parent_path(roots_anchor.path())), true);
    dispatcher.context().roots = &roots;
    add_builtin_methods(dispatcher);
  }

  // Sends one request and returns the parsed response.
  JsonValue call(std::string_view method, JsonValue params = {}, JsonValue id = JsonValue(1)) {
    JsonValue req = JsonValue::object();
    req.set("jsonrpc", JsonValue("2.0"));
    req.set("id", std::move(id));
    req.set("method", JsonValue(method));
    if (!params.is_null()) req.set("params", std::move(params));
    return dispatcher.dispatch(req);
  }
  // Same, requiring success; returns the result.
  JsonValue ok(std::string_view method, JsonValue params = {}) {
    JsonValue resp = call(method, std::move(params));
    const JsonValue* error = resp.find("error");
    if (error != nullptr) {
      FAIL("unexpected error from " << method << ": " << write_json(*error));
    }
    const JsonValue* result = resp.find("result");
    REQUIRE(result != nullptr);
    return *result;
  }
  i32 error_code(std::string_view method, JsonValue params = {}) {
    JsonValue resp = call(method, std::move(params));
    const JsonValue* error = resp.find("error");
    REQUIRE(error != nullptr);
    return static_cast<i32>(error->find("code")->as_int());
  }
};

JsonValue obj(std::initializer_list<std::pair<const char*, JsonValue>> fields) {
  JsonValue o = JsonValue::object();
  for (auto& [k, v] : fields)
    o.set(k, v);
  return o;
}

const JsonValue& at(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  REQUIRE(v != nullptr);
  return *v;
}

std::string hex(Id128 id) {
  char buf[Id128::k_hex_length + 1];
  id.to_hex(buf);
  return buf;
}

JsonValue attribution() {
  return obj({{"actor", JsonValue("test-agent")},
              {"role", JsonValue("environment")},
              {"task", JsonValue("t1")},
              {"rationale", JsonValue("because")}});
}

JsonValue create_command(Id128 id, const char* type, Id128 parent = {}) {
  JsonValue c = obj(
      {{"kind", JsonValue("CreateObject")}, {"id", JsonValue(hex(id))}, {"type", JsonValue(type)}});
  if (!parent.is_null()) c.set("parent", JsonValue(hex(parent)));
  return c;
}

JsonValue set_command(Id128 id, const char* name, JsonValue value) {
  return obj({{"kind", JsonValue("SetProperty")},
              {"id", JsonValue(hex(id))},
              {"name", JsonValue(name)},
              {"value", std::move(value)}});
}

// Writes a whole override record (no type) into the edit layer: how a test builds a layer that
// is a copy of another one with edits on top, which is what a three-way merge takes.
JsonValue restore_command(Id128 id,
                          std::initializer_list<std::pair<const char*, JsonValue>> properties) {
  JsonValue bag = JsonValue::object();
  for (auto& [name, value] : properties)
    bag.set(name, value);
  JsonValue record = obj({{"id", JsonValue(hex(id))}});
  record.set("properties", std::move(bag));
  return obj({{"kind", JsonValue("RestoreRecord")},
              {"id", JsonValue(hex(id))},
              {"record", std::move(record)}});
}

}  // namespace

TEST_CASE("protocol: JSON-RPC framing") {
  Host host;
  // Parse error.
  std::string text = host.dispatcher.dispatch_text("{not json");
  JsonValue resp;
  REQUIRE(parse_json(text, resp).ok);
  CHECK(at(at(resp, "error"), "code") == JsonValue(i32{-32700}));
  CHECK(at(resp, "id").is_null());
  CHECK(at(resp, "jsonrpc") == JsonValue("2.0"));
  CHECK(text.find('\n') == std::string::npos);
  // Invalid request shapes.
  CHECK(at(at(host.dispatcher.dispatch(JsonValue(3)), "error"), "code") == JsonValue(i32{-32600}));
  CHECK(at(at(host.dispatcher.dispatch(obj({{"id", JsonValue(1)}, {"method", JsonValue("x")}})),
              "error"),
           "code") == JsonValue(i32{-32600}));
  CHECK(at(at(host.dispatcher.dispatch(obj(
                  {{"jsonrpc", JsonValue("2.0")}, {"id", JsonValue(1)}, {"method", JsonValue(2)}})),
              "error"),
           "code") == JsonValue(i32{-32600}));
  // Method not found, id echoed (string ids too).
  JsonValue nf = host.call("nope.method", {}, JsonValue("abc"));
  CHECK(at(at(nf, "error"), "code") == JsonValue(i32{-32601}));
  CHECK(at(nf, "id") == JsonValue("abc"));
  CHECK_FALSE(nf.contains("result"));
  // Notifications get nothing back, even when they fail.
  CHECK(host.dispatcher.dispatch_text(R"({"jsonrpc":"2.0","method":"nope.method"})").empty());
  CHECK(host.dispatcher.dispatch_text(R"({"jsonrpc":"2.0","method":"engine.info"})").empty());
  // Params must be an object.
  CHECK(host.error_code("engine.methods", JsonValue(5)) == -32602);
  // Batch: responses only for requests with ids, in order; a batch of notifications is silent.
  JsonValue batch = JsonValue::array();
  batch.push_back(obj(
      {{"jsonrpc", JsonValue("2.0")}, {"id", JsonValue(1)}, {"method", JsonValue("engine.info")}}));
  batch.push_back(obj({{"jsonrpc", JsonValue("2.0")}, {"method", JsonValue("engine.info")}}));
  batch.push_back(
      obj({{"jsonrpc", JsonValue("2.0")}, {"id", JsonValue(2)}, {"method", JsonValue("missing")}}));
  JsonValue out = host.dispatcher.dispatch(batch);
  REQUIRE(out.is_array());
  REQUIRE(out.size() == 2);
  CHECK(at(out[0], "id") == JsonValue(1));
  CHECK(out[0].contains("result"));
  CHECK(at(out[1], "id") == JsonValue(2));
  CHECK(out[1].contains("error"));
  CHECK(host.dispatcher.dispatch(JsonValue::array()).contains("error"));
  JsonValue silent = JsonValue::array();
  silent.push_back(obj({{"jsonrpc", JsonValue("2.0")}, {"method", JsonValue("engine.info")}}));
  CHECK(host.dispatcher.dispatch(silent).is_null());
}

TEST_CASE("protocol: engine.info and the method catalogue") {
  Host host;
  JsonValue info = host.ok("engine.info");
  CHECK(at(info, "name") == JsonValue("game_engine"));
  CHECK(at(info, "version").as_string().size() >= 5);
  CHECK(at(info, "build") == JsonValue(ENGINE_DEBUG ? "debug" : "release"));
  u64 methods = 0;
  CHECK(at(info, "methods").get_u64(methods));
  CHECK(methods == host.dispatcher.methods().size());
  JsonValue catalogue = host.ok("engine.methods");
  const JsonValue& list = at(catalogue, "methods");
  REQUIRE(list.size() == methods);
  bool found = false;
  for (usize i = 0; i < list.size(); ++i) {
    if (at(list[i], "name") == JsonValue("session.open")) {
      found = true;
      CHECK(at(list[i], "params_type") == JsonValue("engine.protocol.SessionOpenParams"));
      CHECK(at(list[i], "result_type") == JsonValue("engine.protocol.SessionInfo"));
      CHECK(at(list[i], "doc").as_string().size() > 10);
    }
    if (i > 0) CHECK(at(list[i - 1], "name").as_string() < at(list[i], "name").as_string());
  }
  CHECK(found);
}

TEST_CASE("protocol: typed params report diagnostics") {
  Host host;
  JsonValue resp = host.call("session.open", obj({{"path", JsonValue(42)}}));
  const JsonValue& error = at(resp, "error");
  CHECK(at(error, "code") == JsonValue(i32{-32602}));
  CHECK(at(error, "message").as_string().find("SessionOpenParams") != std::string::npos);
  REQUIRE(at(error, "data").is_array());
  CHECK(at(at(error, "data")[0], "path") == JsonValue("path"));
  CHECK(host.error_code("session.open", obj({{"bogus", JsonValue(1)}})) == -32602);
  CHECK(host.error_code("doc.objects", obj({{"session", JsonValue("s9")}})) ==
        codes::k_session_not_found);
  CHECK(host.error_code("doc.objects") == codes::k_session_not_found);
  CHECK(host.error_code("session.open", obj({{"path", JsonValue("")}})) ==
        codes::k_invalid_argument);
}

TEST_CASE("protocol: the Phase 0 exit criterion over the dispatcher") {
  TempDir tmp("engine_protocol");
  const std::string dir = tmp.file("world");
  const Id128 a = Id128::from_parts(0x10, 1);
  const Id128 b = Id128::from_parts(0x10, 2);
  const char* k_type = "engine.content.AssetProvenance";
  std::string session;

  {
    Host host;
    CHECK(host.error_code("session.open", obj({{"path", JsonValue(dir)}})) == codes::k_not_found);
    JsonValue info = host.ok(
        "session.open",
        obj({{"path", JsonValue(dir)}, {"create", JsonValue(true)}, {"name", JsonValue("World")}}));
    session = std::string(at(info, "session").as_string());
    CHECK(session == "s1");
    CHECK(at(info, "name") == JsonValue("World"));
    REQUIRE(at(info, "layers").size() == 1);
    CHECK(at(at(info, "layers")[0], "name") == JsonValue("base"));
    CHECK(at(at(info, "layers")[0], "is_edit") == JsonValue(true));
    // Opening the same path again returns the same session.
    CHECK(at(host.ok("session.open", obj({{"path", JsonValue(dir)}})), "session") ==
          JsonValue(session));
    CHECK(at(host.ok("session.list"), "sessions").size() == 1);

    // Create an object with a property.
    JsonValue commands = JsonValue::array();
    commands.push_back(create_command(a, k_type));
    commands.push_back(set_command(a, "generator", JsonValue("dune-gen")));
    JsonValue applied = host.ok("doc.apply", obj({{"session", JsonValue(session)},
                                                  {"commands", commands},
                                                  {"attribution", attribution()}}));
    CHECK(at(applied, "committed") == JsonValue(true));
    CHECK(at(applied, "applied") == JsonValue(u32{2}));
    CHECK(at(applied, "patch_index") == JsonValue(u32{0}));
    CHECK(at(applied, "diagnostics").size() == 0);

    JsonValue objects = host.ok("doc.objects", obj({{"session", JsonValue(session)}}));
    REQUIRE(at(objects, "objects").size() == 1);
    CHECK(at(objects, "total") == JsonValue(u32{1}));
    const JsonValue& view = at(objects, "objects")[0];
    CHECK(at(view, "id") == JsonValue(hex(a)));
    CHECK(at(view, "type") == JsonValue(k_type));
    CHECK(at(view, "defining_layer") == JsonValue("base"));
    CHECK(at(at(view, "properties"), "generator") == JsonValue("dune-gen"));
    JsonValue got =
        host.ok("doc.get", obj({{"session", JsonValue(session)}, {"id", JsonValue(hex(a))}}));
    CHECK(at(got, "type") == JsonValue(k_type));
    CHECK(host.error_code("doc.get", obj({{"session", JsonValue(session)},
                                          {"id", JsonValue(hex(b))}})) == codes::k_not_found);

    // A failing command in an atomic transaction commits nothing.
    JsonValue bad = JsonValue::array();
    bad.push_back(set_command(b, "generator", JsonValue("x")));  // b does not exist
    bad.push_back(set_command(a, "generator", JsonValue("changed")));
    JsonValue rejected = host.ok(
        "doc.apply",
        obj({{"session", JsonValue(session)}, {"commands", bad}, {"attribution", attribution()}}));
    CHECK(at(rejected, "committed") == JsonValue(false));
    CHECK(at(rejected, "applied") == JsonValue(u32{1}));
    REQUIRE(at(rejected, "diagnostics").size() == 1);
    CHECK(at(host.ok("doc.get", obj({{"session", JsonValue(session)}, {"id", JsonValue(hex(a))}})),
             "properties")
              .find("generator")
              ->as_string() == "dune-gen");
    // Non-atomic commits what succeeded.
    JsonValue partial = host.ok("doc.apply", obj({{"session", JsonValue(session)},
                                                  {"commands", bad},
                                                  {"attribution", attribution()},
                                                  {"atomic", JsonValue(false)}}));
    CHECK(at(partial, "committed") == JsonValue(true));
    CHECK(at(partial, "patch_index") == JsonValue(u32{1}));

    // Undo both patches, redo one, then a new commit drops the redo tail.
    JsonValue undone =
        host.ok("doc.undo", obj({{"session", JsonValue(session)}, {"steps", JsonValue(5)}}));
    CHECK(at(undone, "stepped") == JsonValue(u32{2}));
    CHECK(at(undone, "position") == JsonValue(u32{0}));
    CHECK(at(host.ok("doc.objects", obj({{"session", JsonValue(session)}})), "total") ==
          JsonValue(u32{0}));
    JsonValue redone = host.ok("doc.redo", obj({{"session", JsonValue(session)}}));
    CHECK(at(redone, "stepped") == JsonValue(u32{1}));
    CHECK(at(redone, "position") == JsonValue(u32{1}));
    CHECK(at(host.ok("doc.objects", obj({{"session", JsonValue(session)}})), "total") ==
          JsonValue(u32{1}));
    JsonValue more = JsonValue::array();
    more.push_back(create_command(b, k_type, a));
    JsonValue third = host.ok(
        "doc.apply",
        obj({{"session", JsonValue(session)}, {"commands", more}, {"attribution", attribution()}}));
    CHECK(at(third, "patch_index") == JsonValue(u32{1}));  // redo tail dropped
    JsonValue journal = host.ok("doc.journal", obj({{"session", JsonValue(session)}}));
    CHECK(at(journal, "total") == JsonValue(u32{2}));
    CHECK(at(journal, "position") == JsonValue(u32{2}));
    CHECK(at(at(at(journal, "patches")[1], "attribution"), "actor") == JsonValue("test-agent"));
    CHECK(at(host.ok("doc.objects",
                     obj({{"session", JsonValue(session)}, {"parent", JsonValue(hex(a))}})),
             "total") == JsonValue(u32{1}));
    CHECK(at(host.ok("doc.objects",
                     obj({{"session", JsonValue(session)}, {"type", JsonValue("other.Type")}})),
             "total") == JsonValue(u32{0}));

    // Layers and diff.
    JsonValue layers = host.ok(
        "doc.add_layer", obj({{"session", JsonValue(session)}, {"name", JsonValue("quest")}}));
    REQUIRE(at(layers, "layers").size() == 2);
    CHECK(at(at(layers, "layers")[1], "is_edit") == JsonValue(true));
    CHECK(at(at(layers, "layers")[1], "role") == JsonValue("Feature"));
    CHECK(host.error_code("doc.add_layer",
                          obj({{"session", JsonValue(session)}, {"name", JsonValue("quest")}})) ==
          codes::k_invalid_argument);
    JsonValue in_quest = JsonValue::array();
    in_quest.push_back(set_command(a, "generator", JsonValue("quest-override")));
    host.ok("doc.apply", obj({{"session", JsonValue(session)},
                              {"commands", in_quest},
                              {"attribution", attribution()}}));
    CHECK(at(host.ok("doc.get", obj({{"session", JsonValue(session)}, {"id", JsonValue(hex(a))}})),
             "properties")
              .find("generator")
              ->as_string() == "quest-override");
    JsonValue diff = host.ok("doc.diff", obj({{"session", JsonValue(session)},
                                              {"from_layer", JsonValue("base")},
                                              {"to_layer", JsonValue("quest")}}));
    CHECK(at(diff, "commands").size() >= 1);
    CHECK(host.error_code("doc.diff", obj({{"session", JsonValue(session)},
                                           {"from_layer", JsonValue("base")},
                                           {"to_layer", JsonValue("nope")}})) ==
          codes::k_not_found);
    CHECK(at(host.ok("doc.set_edit_layer",
                     obj({{"session", JsonValue(session)}, {"layer", JsonValue("base")}})),
             "layers")[0]
              .find("is_edit")
              ->as_bool());
    JsonValue validated = host.ok("doc.validate", obj({{"session", JsonValue(session)}}));
    CHECK(at(validated, "ok") == JsonValue(true));
    CHECK(at(host.ok("session.close", obj({{"session", JsonValue(session)}})), "session") ==
          JsonValue(session));
    CHECK(host.error_code("session.info", obj({{"session", JsonValue(session)}})) ==
          codes::k_session_not_found);
  }

  // A new host (a new process, in effect) sees everything, including the undo position.
  {
    Host host;
    JsonValue info = host.ok("session.open", obj({{"path", JsonValue(dir)}}));
    CHECK(at(info, "name") == JsonValue("World"));
    CHECK(at(info, "journal_length") == JsonValue(u32{3}));
    CHECK(at(info, "undo_position") == JsonValue(u32{3}));
    REQUIRE(at(info, "layers").size() == 2);
    CHECK(at(at(info, "layers")[0], "is_edit") == JsonValue(true));
    const std::string s = std::string(at(info, "session").as_string());
    CHECK(at(host.ok("doc.objects", obj({{"session", JsonValue(s)}})), "total") ==
          JsonValue(u32{2}));
    JsonValue undone =
        host.ok("doc.undo", obj({{"session", JsonValue(s)}, {"steps", JsonValue(3)}}));
    CHECK(at(undone, "position") == JsonValue(u32{0}));
    CHECK(at(host.ok("doc.objects", obj({{"session", JsonValue(s)}})), "total") ==
          JsonValue(u32{0}));
  }
  {
    Host host;
    JsonValue info = host.ok("session.open", obj({{"path", JsonValue(dir)}}));
    CHECK(at(info, "undo_position") == JsonValue(u32{0}));
    CHECK(at(info, "journal_length") == JsonValue(u32{3}));
    const std::string s = std::string(at(info, "session").as_string());
    CHECK(at(host.ok("doc.redo", obj({{"session", JsonValue(s)}, {"steps", JsonValue(2)}})),
             "position") == JsonValue(u32{2}));
    CHECK(at(host.ok("doc.objects", obj({{"session", JsonValue(s)}})), "total") ==
          JsonValue(u32{2}));
  }
}

TEST_CASE("protocol: doc.add_layer with edit=false leaves the edit layer alone, on disk too") {
  TempDir tmp("engine_protocol");
  const std::string dir = tmp.file("layered-world");
  const Id128 a = Id128::from_parts(0x30, 1);
  const Id128 b = Id128::from_parts(0x30, 2);
  const char* k_type = "engine.content.AssetProvenance";
  {
    Host host;
    JsonValue info = host.ok(
        "session.open",
        obj({{"path", JsonValue(dir)}, {"create", JsonValue(true)}, {"name", JsonValue("World")}}));
    const std::string session(at(info, "session").as_string());
    const JsonValue s(session);
    JsonValue first = JsonValue::array();
    first.push_back(create_command(a, k_type));
    host.ok("doc.apply",
            obj({{"session", s}, {"commands", first}, {"attribution", attribution()}}));

    // The new layer is appended, but the session keeps editing base.
    JsonValue layers =
        at(host.ok("doc.add_layer",
                   obj({{"session", s}, {"name", JsonValue("notes")}, {"edit", JsonValue(false)}})),
           "layers");
    REQUIRE(layers.size() == 2);
    CHECK(at(layers[0], "is_edit") == JsonValue(true));
    CHECK(at(layers[1], "is_edit") == JsonValue(false));
    layers = at(host.ok("doc.layers", obj({{"session", s}})), "layers");
    CHECK(at(layers[0], "is_edit") == JsonValue(true));
    CHECK(at(layers[1], "is_edit") == JsonValue(false));

    // So the next commit lands in base, not in the new layer.
    JsonValue second = JsonValue::array();
    second.push_back(create_command(b, k_type, a));
    host.ok("doc.apply",
            obj({{"session", s}, {"commands", second}, {"attribution", attribution()}}));
    layers = at(host.ok("doc.layers", obj({{"session", s}})), "layers");
    CHECK(at(layers[0], "records") == JsonValue(u32{2}));
    CHECK(at(layers[1], "records") == JsonValue(u32{0}));
    host.ok("session.close", obj({{"session", s}}));
  }

  // The manifest recorded the same choice: a fresh process reopens on base.
  {
    Host host;
    JsonValue info = host.ok("session.open", obj({{"path", JsonValue(dir)}}));
    const JsonValue& layers = at(info, "layers");
    REQUIRE(layers.size() == 2);
    CHECK(at(layers[0], "name") == JsonValue("base"));
    CHECK(at(layers[0], "is_edit") == JsonValue(true));
    CHECK(at(layers[0], "records") == JsonValue(u32{2}));
    CHECK(at(layers[1], "name") == JsonValue("notes"));
    CHECK(at(layers[1], "is_edit") == JsonValue(false));
    CHECK(at(layers[1], "records") == JsonValue(u32{0}));
  }
}

TEST_CASE("protocol: doc.merge writes a three-way merge into a layer, undoably") {
  TempDir tmp("engine_protocol");
  const std::string dir = tmp.file("merged-world");
  const Id128 a = Id128::from_parts(0x20, 1);
  const Id128 b = Id128::from_parts(0x20, 2);
  const char* k_type = "engine.content.AssetProvenance";
  Host host;
  JsonValue info = host.ok(
      "session.open",
      obj({{"path", JsonValue(dir)}, {"create", JsonValue(true)}, {"name", JsonValue("World")}}));
  const std::string session(at(info, "session").as_string());
  const JsonValue s(session);

  // Two objects in the base layer.
  JsonValue world = JsonValue::array();
  world.push_back(create_command(a, k_type));
  world.push_back(create_command(b, k_type, a));
  host.ok("doc.apply", obj({{"session", s}, {"commands", world}, {"attribution", attribution()}}));

  // Three layers of overrides: the common ancestor, and two copies of it that diverged.
  auto layer_of = [&](const char* name, JsonValue commands) {
    host.ok("doc.add_layer", obj({{"session", s}, {"name", JsonValue(name)}}));
    host.ok(
        "doc.apply",
        obj({{"session", s}, {"commands", std::move(commands)}, {"attribution", attribution()}}));
  };
  JsonValue shared = JsonValue::array();
  shared.push_back(
      restore_command(a, {{"generator", JsonValue("shared")}, {"license", JsonValue("MIT")}}));
  shared.push_back(restore_command(b, {{"generator", JsonValue("b-shared")}}));
  layer_of("shared", shared);
  JsonValue ours = JsonValue::array();
  ours.push_back(
      restore_command(a, {{"generator", JsonValue("ours")}, {"license", JsonValue("MIT")}}));
  ours.push_back(
      restore_command(b, {{"generator", JsonValue("b-shared")}, {"model_id", JsonValue("m1")}}));
  layer_of("ours", ours);
  JsonValue theirs = JsonValue::array();
  theirs.push_back(
      restore_command(a, {{"generator", JsonValue("theirs")}, {"license", JsonValue("MIT")}}));
  theirs.push_back(restore_command(b, {{"generator", JsonValue("b-theirs")}}));
  layer_of("theirs", theirs);

  JsonValue params = obj({{"session", s},
                          {"base_layer", JsonValue("shared")},
                          {"ours_layer", JsonValue("ours")},
                          {"theirs_layer", JsonValue("theirs")},
                          {"output_layer", JsonValue("merged")}});
  JsonValue merged = host.ok("doc.merge", params);
  CHECK(at(merged, "applied_ours") == JsonValue(u32{2}));    // our generator, our model_id
  CHECK(at(merged, "applied_theirs") == JsonValue(u32{1}));  // their generator on b
  CHECK(at(merged, "committed") == JsonValue(true));
  CHECK(at(merged, "patch_index") == JsonValue(u32{4}));
  REQUIRE(at(merged, "conflicts").size() == 1);
  const JsonValue& conflict = at(merged, "conflicts")[0];
  CHECK(at(conflict, "kind") == JsonValue("PropertyBothChanged"));
  CHECK(at(conflict, "object") == JsonValue(hex(a)));
  CHECK(at(conflict, "property") == JsonValue("generator"));
  CHECK(at(conflict, "base") == JsonValue("shared"));
  CHECK(at(conflict, "ours") == JsonValue("ours"));
  CHECK(at(conflict, "theirs") == JsonValue("theirs"));

  // The merged layer is the strongest, so the composed document reads it.
  auto generator_of = [&](Id128 id) {
    return std::string(
        at(host.ok("doc.get", obj({{"session", s}, {"id", JsonValue(hex(id))}})), "properties")
            .find("generator")
            ->as_string());
  };
  CHECK(generator_of(a) == "ours");
  CHECK(generator_of(b) == "b-theirs");
  CHECK(at(host.ok("doc.validate", obj({{"session", s}})), "ok") == JsonValue(true));
  REQUIRE(at(host.ok("doc.layers", obj({{"session", s}})), "layers").size() == 5);
  // The merge did not move the session's edit layer.
  CHECK(at(at(host.ok("doc.layers", obj({{"session", s}})), "layers")[3], "is_edit") ==
        JsonValue(true));

  // It is a transaction like any other: undo empties the layer again, redo fills it.
  CHECK(at(host.ok("doc.undo", obj({{"session", s}})), "position") == JsonValue(u32{4}));
  CHECK(generator_of(a) == "theirs");
  CHECK(at(host.ok("doc.redo", obj({{"session", s}})), "position") == JsonValue(u32{5}));
  CHECK(generator_of(a) == "ours");

  // Resolving toward theirs, into a second layer.
  JsonValue toward_theirs = params;
  toward_theirs.set("output_layer", JsonValue("merged-theirs"));
  toward_theirs.set("prefer", JsonValue("Theirs"));
  JsonValue second = host.ok("doc.merge", toward_theirs);
  CHECK(at(second, "applied_ours") == JsonValue(u32{1}));
  CHECK(at(second, "applied_theirs") == JsonValue(u32{2}));
  CHECK(at(second, "conflicts").size() == 1);
  CHECK(generator_of(a) == "theirs");

  // Merging a layer with itself over itself changes nothing and journals nothing.
  JsonValue idempotent = obj({{"session", s},
                              {"base_layer", JsonValue("merged")},
                              {"ours_layer", JsonValue("merged")},
                              {"theirs_layer", JsonValue("merged")},
                              {"output_layer", JsonValue("merged")}});
  JsonValue nothing = host.ok("doc.merge", idempotent);
  CHECK(at(nothing, "committed") == JsonValue(false));
  CHECK(at(nothing, "conflicts").size() == 0);

  // Bad arguments.
  JsonValue missing = params;
  missing.set("theirs_layer", JsonValue("nope"));
  CHECK(host.error_code("doc.merge", missing) == codes::k_not_found);
  JsonValue no_output = params;
  no_output.set("output_layer", JsonValue(""));
  CHECK(host.error_code("doc.merge", no_output) == codes::k_invalid_argument);
  JsonValue bad_prefer = params;
  bad_prefer.set("prefer", JsonValue("Whatever"));
  CHECK(host.error_code("doc.merge", bad_prefer) == codes::k_invalid_params);
}

TEST_CASE("protocol: tunables, log, and schema methods") {
  Host host;
  tunables::Int knob{"test.protocol.knob", 4, 1, 16, "A knob"};
  JsonValue list = host.ok("tunables.list");
  bool found = false;
  const JsonValue& all = at(list, "tunables");
  for (usize i = 0; i < all.size(); ++i) {
    if (at(all[i], "name") == JsonValue("test.protocol.knob")) found = true;
  }
  CHECK(found);
  JsonValue set = host.ok(
      "tunables.set", obj({{"name", JsonValue("test.protocol.knob")}, {"value", JsonValue(9)}}));
  CHECK(knob.get() == 9);
  CHECK(at(at(set, "tunables"), "value") == JsonValue(i64{9}));
  CHECK(host.error_code("tunables.set", obj({{"name", JsonValue("test.protocol.knob")},
                                             {"value", JsonValue(99)}})) ==
        codes::k_invalid_argument);
  CHECK(host.error_code("tunables.set", obj({{"name", JsonValue("nope")},
                                             {"value", JsonValue(1)}})) == codes::k_not_found);

  log::Category category{"test.protocol"};
  log::add_sink(&host.ring);
  log::set_global_min_level(log::Level::Trace);
  ENGINE_LOG_INFO(category, "hello", log::field("n", 3), log::field("who", "agent"));
  ENGINE_LOG_WARN(category, "careful");
  log::remove_sink(&host.ring);
  log::reset_levels();
  JsonValue tail = host.ok("log.tail", obj({{"since", JsonValue(0)}}));
  REQUIRE(at(tail, "records").size() == 2);
  const JsonValue& first = at(tail, "records")[0];
  CHECK(at(first, "msg") == JsonValue("hello"));
  CHECK(at(first, "level") == JsonValue("info"));
  CHECK(at(first, "cat") == JsonValue("test.protocol"));
  CHECK(at(at(first, "fields"), "who") == JsonValue("agent"));
  CHECK(at(tail, "next") == JsonValue(u64{2}));
  CHECK(at(host.ok("log.tail", obj({{"since", JsonValue(2)}})), "records").size() == 0);
  Host without_ring;
  without_ring.dispatcher.context().log_ring = nullptr;
  CHECK(without_ring.error_code("log.tail") == codes::k_unavailable);

  JsonValue types = host.ok("schema.types");
  const JsonValue& names = at(types, "types");
  bool has_command = false;
  for (usize i = 0; i < names.size(); ++i) {
    if (names[i] == JsonValue("engine.doc.Command")) has_command = true;
    if (i > 0) CHECK(names[i - 1].as_string() < names[i].as_string());
  }
  CHECK(has_command);
  JsonValue described =
      host.ok("schema.describe", obj({{"type", JsonValue("engine.doc.Command")}}));
  const JsonValue& d = at(described, "description");
  CHECK(at(d, "kind") == JsonValue("struct"));
  CHECK(at(d, "fields").size() >= 6);
  bool has_record = false;
  for (usize i = 0; i < at(d, "fields").size(); ++i) {
    const JsonValue& f = at(d, "fields")[i];
    if (at(f, "name") == JsonValue("record")) {
      has_record = true;
      CHECK(at(f, "type") == JsonValue("engine.doc.ObjectRecord?"));
    }
  }
  CHECK(has_record);
  // Every field states the value an absent one becomes, in the form a caller would send it: the
  // schema's declared default, a value-initialized one where it declares none, null for an empty
  // optional, and an enumerator by name. A client generating JSON Schema from this needs no
  // second copy of any default (the MCP bridge, docs/subsystems/apps.md).
  const auto default_of = [&](const char* type, const char* field) {
    JsonValue d2 = host.ok("schema.describe", obj({{"type", JsonValue(type)}}));
    const JsonValue& fields = at(at(d2, "description"), "fields");
    for (usize i = 0; i < fields.size(); ++i) {
      if (at(fields[i], "name") == JsonValue(field)) return at(fields[i], "default");
    }
    FAIL("no field " << field << " in " << type);
    return JsonValue();
  };
  CHECK(default_of("engine.protocol.SessionOpenParams", "create") == JsonValue(false));
  CHECK(default_of("engine.protocol.SessionOpenParams", "path") == JsonValue(""));
  CHECK(default_of("engine.protocol.ObjectsParams", "limit") == JsonValue(u32{1000}));
  CHECK(default_of("engine.protocol.ObjectsParams", "parent").is_null());
  CHECK(default_of("engine.protocol.AddLayerParams", "role") == JsonValue("Feature"));
  CHECK(default_of("engine.protocol.AddLayerParams", "edit") == JsonValue(true));
  CHECK(default_of("engine.doc.Command", "kind") == JsonValue("SetProperty"));
  CHECK(default_of("engine.protocol.ApplyParams", "commands") == JsonValue::array());
  JsonValue role = host.ok("schema.describe", obj({{"type", JsonValue("engine.doc.LayerRole")}}));
  CHECK(at(at(role, "description"), "kind") == JsonValue("enum"));
  CHECK(at(at(role, "description"), "values").size() == 4);
  CHECK(host.error_code("schema.describe", obj({{"type", JsonValue("nope.Type")}})) ==
        codes::k_not_found);
}

// The day-one operations' types (content.build, session.events, engine.budgets,
// session.run_headless, engine.run_tests). The methods are engine-host's (apps/engine_host) and
// their end-to-end cases are apps/engine_cli/tests/ops_tests.cpp; what the protocol owns is the
// types, so these pin what a client relies on without a host: the schema's defaults, the optional
// fields a result leaves null, and a round trip through the reader the dispatcher uses.
TEST_CASE("protocol: the day-one operations' schema types, their defaults and their round trips") {
  Host host;
  const auto default_of = [&](const char* type, const char* field) {
    JsonValue d = host.ok("schema.describe", obj({{"type", JsonValue(type)}}));
    const JsonValue& fields = at(at(d, "description"), "fields");
    for (usize i = 0; i < fields.size(); ++i) {
      if (at(fields[i], "name") == JsonValue(field)) return at(fields[i], "default");
    }
    FAIL("no field " << field << " in " << type);
    return JsonValue();
  };
  CHECK(default_of("engine.protocol.ContentBuildParams", "stats") == JsonValue(true));
  CHECK(default_of("engine.protocol.ContentBuildParams", "force") == JsonValue(false));
  CHECK(default_of("engine.protocol.ContentBuildOptions", "max_triangles") == JsonValue(u32{124}));
  CHECK(default_of("engine.protocol.ContentBuildOptions", "page_bytes") == JsonValue(u32{131072}));
  CHECK(default_of("engine.protocol.ContentBuildOptions", "uv_seams") == JsonValue("protect"));
  CHECK(default_of("engine.protocol.ContentBuildOptions", "atlas") == JsonValue("keep"));
  CHECK(default_of("engine.protocol.SessionEventsParams", "limit") == JsonValue(u32{100}));
  CHECK(default_of("engine.protocol.SessionEventsParams", "source") == JsonValue(""));
  CHECK(default_of("engine.protocol.RunHeadlessParams", "seconds") == JsonValue(0.0));
  CHECK(default_of("engine.protocol.RunHeadlessParams", "until").is_null());
  CHECK(default_of("engine.protocol.RunTestsParams", "tissue_modes") == JsonValue(true));
  CHECK(default_of("engine.protocol.RunTestsParams", "containers") == JsonValue::array());
  CHECK(default_of("engine.protocol.Budget", "limit").is_null());
  CHECK(default_of("engine.protocol.SessionEvent", "attribution").is_null());

  // ADR-0053: a world position is described as the IDL spells it, version 2, origin by default.
  {
    JsonValue d = host.ok("schema.describe", obj({{"type", JsonValue("engine.world.Transform")}}));
    const JsonValue& description = at(d, "description");
    CHECK(at(description, "version") == JsonValue(u32{2}));
    const JsonValue& position = at(description, "fields")[0];
    CHECK(at(position, "name") == JsonValue("position"));
    CHECK(at(position, "type") == JsonValue("worldpos"));
    JsonValue origin = JsonValue::array();
    for (int i = 0; i < 3; ++i)
      origin.push_back(JsonValue(0.0));
    CHECK(at(position, "default") == origin);
  }

  // A budget with no limit and no use says null for both, not 0: "counted, not bounded" and "not
  // in use yet" are different answers from "a limit of nothing".
  Budget budget;
  budget.name = "memory.allocations";
  budget.used = 12.0;
  JsonValue written = schema::to_json(budget);
  CHECK(at(written, "limit").is_null());
  CHECK(at(written, "used") == JsonValue(12.0));
  CHECK(at(written, "peak").is_null());
  Budget read_back;
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(read_back, written, ctx));
  CHECK(read_back == budget);

  // A journal event carries its attribution; a store event carries none, and says null.
  SessionEvent journal;
  journal.source = "journal";
  journal.kind = "commit";
  journal.attribution = doc::Attribution{"agent", "environment", "t1", "because", 5};
  SessionEvent stored;
  stored.source = "store";
  stored.tick = 42;
  stored.subject = Id128::from_seed(3, 4);
  SessionEventsResult events;
  events.events.push_back(journal);
  events.events.push_back(stored);
  events.next = "j=1;s=42:0:1";
  JsonValue events_json = schema::to_json(events);
  CHECK(at(at(at(events_json, "events")[0], "attribution"), "rationale") == JsonValue("because"));
  CHECK(at(at(events_json, "events")[1], "attribution").is_null());
  SessionEventsResult events_back;
  schema::ReadContext events_ctx;
  REQUIRE(schema::from_json(events_back, events_json, events_ctx));
  CHECK(events_back == events);

  // The run's predicate is JSON the reader passes through untouched, and the result's own
  // predicate is null when none was asked for.
  JsonValue run_params = obj({{"session", JsonValue("s1")},
                              {"seconds", JsonValue(0.5)},
                              {"until", obj({{"ticks_at_least", JsonValue(15)}})}});
  RunHeadlessParams run;
  schema::ReadContext run_ctx;
  REQUIRE(schema::from_json(run, run_params, run_ctx));
  CHECK(run.seconds == 0.5);
  CHECK(at(run.until, "ticks_at_least") == JsonValue(15));
  RunHeadlessResult ran;
  CHECK(at(schema::to_json(ran), "predicate").is_null());
  ran.predicate = true;
  CHECK(at(schema::to_json(ran), "predicate") == JsonValue(true));

  // content.build's params refuse a field they do not have, like every params struct.
  ContentBuildParams build;
  schema::ReadContext build_ctx;
  CHECK_FALSE(schema::from_json(build, obj({{"sorce", JsonValue("a.gltf")}}), build_ctx));
  CHECK_FALSE(build_ctx.diagnostics.empty());
}

// session.open is registered read-only, so every role reaches it: before the roots it opened any
// engine document on the disk, said whether one was there, and made one wherever it was told (the
// survey's rows 1-3). Now it reads under a root, makes a document only under a writable one, and
// answers a refusal exactly as it answers a directory that holds nothing.
TEST_CASE("roots: session.open reaches only the host's roots, and a refusal reads as missing") {
  TempDir tmp("protocol_roots_open");
  const std::string other = tmp.file("other");
  Host h;
  // A document outside the root the case narrows the host to, made while the host could reach it.
  JsonValue made =
      h.ok("session.open", obj({{"path", JsonValue(other)}, {"create", JsonValue(true)}}));
  h.ok("session.close", obj({{"session", *made.find("session")}}));
  io::Roots narrow;
  REQUIRE(narrow.add("work", tmp.file("work"), true) == io::Status::Ok);
  h.dispatcher.context().roots = &narrow;

  auto refused = [&](JsonValue params) {
    const JsonValue resp = h.call("session.open", std::move(params));
    const JsonValue* error = resp.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("code")->as_int() == codes::k_not_found);
    return std::string(error->find("message")->as_string());
  };
  // An existing document outside, and nothing at all inside: one sentence, the client's own path.
  const std::string outside_message = refused(obj({{"path", JsonValue(other)}}));
  CHECK(outside_message == "path: no file this host may read at '" + other + "'");
  const std::string missing = tmp.file("work") + "/nothing";
  CHECK(refused(obj({{"path", JsonValue(missing)}})) ==
        "path: no file this host may read at '" + missing + "'");
  // Spellings that reach the same document from inside the root.
  CHECK(!refused(obj({{"path", JsonValue("work://../other")}})).empty());
  CHECK(!refused(obj({{"path", JsonValue(tmp.file("work") + "/../other")}})).empty());
  // A document is made only under a writable root.
  const std::string elsewhere = tmp.file("elsewhere");
  (void)refused(obj({{"path", JsonValue(elsewhere)}, {"create", JsonValue(true)}}));
  CHECK_FALSE(io::exists(elsewhere));
  // A root this host does not have says so, and says nothing else.
  {
    const JsonValue resp = h.call("session.open", obj({{"path", JsonValue("nope://world")}}));
    REQUIRE(resp.find("error") != nullptr);
    CHECK(resp.find("error")->find("message")->as_string() ==
          "path: root nope is not present on this host");
  }
  // Inside, it works, and the session is answered as root and relative, never the host's path.
  const JsonValue opened =
      h.ok("session.open", obj({{"path", JsonValue("work://world")}, {"create", JsonValue(true)}}));
  CHECK(opened.find("path")->as_string() == "work://world");
  CHECK(io::exists(tmp.file("work") + "/world/manifest.json"));
  h.dispatcher.context().roots = &h.roots;
}

// With a root on a network share, a client's //server/share had to be resolved to be compared,
// which made the host connect to the client's server with the operator's credentials; so the
// start refuses one, says why, and the host exits 2.
TEST_CASE("roots: a root on a network share is refused at start with a message") {
  io::Roots roots;
  HostRootsConfig config;
  config.root_specs.push_back("art=//engine-tests.invalid/art");
  std::string error;
  CHECK_FALSE(configure_host_roots(roots, config, error));
  CHECK(error.find("network share") != std::string::npos);
  CHECK(roots.find("art") == nullptr);
  io::Roots rw;
  HostRootsConfig writable;
  writable.root_specs.push_back("work=\\\\engine-tests.invalid\\work:rw");
  error.clear();
  CHECK_FALSE(configure_host_roots(rw, writable, error));
  CHECK(error.find("network share") != std::string::npos);
}

// A document's out/ was added wherever it resolved: a file named out became a writable "root",
// and a link at out made its target the host's writable root (io's roots_tests has the link).
// Now it is the document's own plain directory or no root at all, and the host still starts.
TEST_CASE("roots: a document whose out is not its own directory opens with no out root") {
  TempDir tmp("protocol_roots_out_file");
  const std::string doc = tmp.file("doc");
  REQUIRE(io::make_directories(doc) == io::Status::Ok);
  REQUIRE(io::write_file(doc + "/out", "a file, not a directory") == io::Status::Ok);
  io::Roots roots;
  HostRootsConfig config;
  config.doc_dir = doc;
  std::string error;
  REQUIRE(configure_host_roots(roots, config, error));
  CHECK(roots.find("doc") != nullptr);
  CHECK(roots.find("out") == nullptr);
  // A plain one is the root, as before.
  TempDir plain("protocol_roots_out_plain");
  io::Roots ok;
  HostRootsConfig plain_config;
  plain_config.doc_dir = plain.file("doc");
  REQUIRE(io::make_directories(plain_config.doc_dir) == io::Status::Ok);
  REQUIRE(configure_host_roots(ok, plain_config, error));
  CHECK(ok.find("out") != nullptr);
}

// A session's derived data went to the repository's ddc/ found by walking up from the document,
// else to <doc>/ddc: inside an open document outside its out/, or under no root at all, where no
// client write could go. Now it is the host's own "ddc" root, else the document's out/ddc.
TEST_CASE("roots: a session's derived data goes to the host's ddc root, else its out/ddc") {
  TempDir tmp("protocol_roots_ddc");
  Host h;
  io::Roots without;
  HostRootsConfig plain;
  plain.root_specs.push_back("work=" + tmp.file("work") + ":rw");
  std::string error;
  REQUIRE(configure_host_roots(without, plain, error));
  h.dispatcher.context().roots = &without;
  const JsonValue opened =
      h.ok("session.open", obj({{"path", JsonValue("work://world")}, {"create", JsonValue(true)}}));
  const Session* session =
      h.dispatcher.context().sessions->find(std::string(at(opened, "session").as_string()));
  REQUIRE(session != nullptr);
  const std::string out_ddc = derived_data_root(h.dispatcher.context(), session);
  CHECK(out_ddc == io::join_path(session->dir(), "out/ddc"));
  // Under out/, so the roots' own rule for a document's files holds for it.
  CHECK(io::path_is_under(out_ddc, session->dir() + "/out"));
  h.ok("session.close", obj({{"session", at(opened, "session")}}));
  // With a "ddc" root, that, made at start and read-only to clients.
  io::Roots with;
  HostRootsConfig config = plain;
  config.ddc_dir = tmp.file("derived");
  REQUIRE(configure_host_roots(with, config, error));
  REQUIRE(with.find("ddc") != nullptr);
  CHECK_FALSE(with.find("ddc")->writable);
  CHECK(io::exists(tmp.file("derived")));
  h.dispatcher.context().roots = &with;
  CHECK(derived_data_root(h.dispatcher.context(), nullptr) == with.find("ddc")->canonical);
  h.dispatcher.context().roots = &h.roots;
}

// The session's events went to every subscriber with the host's own directory in `path`, and a
// file admitted under a root but missing was answered later, and differently, than a refused one.
TEST_CASE("roots: a session's events show its root form, and a missing file reads as refused") {
  TempDir tmp("protocol_roots_shown");
  Host h;
  const JsonValue opened = h.ok(
      "session.open", obj({{"path", JsonValue(tmp.file("world"))}, {"create", JsonValue(true)}}));
  const Session* session =
      h.dispatcher.context().sessions->find(std::string(at(opened, "session").as_string()));
  REQUIRE(session != nullptr);
  CHECK(session->shown_path().starts_with("tests://"));
  CHECK(session->shown_path().ends_with("/world"));
  CHECK(session->shown_path() == std::string(at(opened, "path").as_string()));
  h.ok("session.close", obj({{"session", at(opened, "session")}}));

  // Missing under a root and outside every root: one error, one sentence, at admission.
  Context& ctx = h.dispatcher.context();
  io::Admitted a;
  RpcError missing;
  CHECK_FALSE(admit_existing_path(ctx, "mesh", tmp.file("nothing.glb"), {}, a, missing));
  CHECK(a.native.empty());
  io::Roots narrow;
  REQUIRE(narrow.add("work", tmp.file("work"), true) == io::Status::Ok);
  ctx.roots = &narrow;
  RpcError refused;
  CHECK_FALSE(admit_existing_path(ctx, "mesh", tmp.file("nothing.glb"), {}, a, refused));
  ctx.roots = &h.roots;
  CHECK(missing.code == refused.code);
  CHECK(missing.message == refused.message);
  // And a present one is admitted.
  REQUIRE(io::write_file(tmp.file("there.glb"), "x") == io::Status::Ok);
  RpcError none;
  CHECK(admit_existing_path(ctx, "mesh", tmp.file("there.glb"), {}, a, none));
  // A reader's message is told the client's spelling, never the native path.
  CHECK(shown_message("'D/x/save' is not a save; D/x/save has no manifest", "D/x/save",
                      "out://save") == "'out://save' is not a save; out://save has no manifest");
}

// `<dir>:rw` was split on its suffix alone, so a directory whose name ends in ":rw" (Linux allows
// it) could not be named read-only and was silently made writable; `:ro` says read-only outright.
TEST_CASE("roots: --root takes :ro, so a directory named with :rw can be read-only") {
  TempDir tmp("protocol_roots_ro");
  const std::string dir = tmp.file("lib");
  REQUIRE(io::make_directories(dir) == io::Status::Ok);
  io::Roots roots;
  HostRootsConfig config;
  config.root_specs.push_back("lib=" + dir + ":ro");
  config.root_specs.push_back("work=" + tmp.file("work") + ":rw");
  std::string error;
  REQUIRE(configure_host_roots(roots, config, error));
  REQUIRE(roots.find("lib") != nullptr);
  CHECK_FALSE(roots.find("lib")->writable);
  CHECK(roots.find("lib")->spelled == dir);
  CHECK(roots.find("work")->writable);
}

// No roots, no paths: a dispatcher made without them refuses every client path.
TEST_CASE("roots: a context with no roots refuses every path") {
  TempDir tmp("protocol_roots_none");
  Host h;
  h.dispatcher.context().roots = nullptr;
  const JsonValue resp = h.call(
      "session.open", obj({{"path", JsonValue(tmp.file("w"))}, {"create", JsonValue(true)}}));
  REQUIRE(resp.find("error") != nullptr);
  CHECK_FALSE(io::exists(tmp.file("w")));
}

namespace {
// A text in one spelling for a search: forward slashes (JSON writes a backslash as two), lower
// case.
std::string folded(std::string_view text) {
  std::string out;
  for (usize i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\\') {
      if (i + 1 < text.size() && text[i + 1] == '\\') ++i;
      c = '/';
    }
    out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}
}  // namespace

// log.tail is a read every role may make, and the ring holds every record the host logs, so the
// check's log is the answer's twin: before, a refusal logged its reason and the canonical form it
// resolved (the long, on-disk name of whatever existed, link targets included) and every root
// logged its directory, while a missing file logged nothing, which told the two apart.
TEST_CASE("roots: a refused path leaves nothing of the disk in log.tail") {
  TempDir tmp("protocol_roots_log");
  const std::string other = tmp.file("other");
  Host h;
  JsonValue made =
      h.ok("session.open", obj({{"path", JsonValue(other)}, {"create", JsonValue(true)}}));
  h.ok("session.close", obj({{"session", *made.find("session")}}));

  log::add_sink(&h.ring);
  log::set_global_min_level(log::Level::Trace);
  io::Roots narrow;
  HostRootsConfig config;
  config.root_specs.push_back("work=" + tmp.file("work") + ":rw");
  std::string error;
  const bool configured = configure_host_roots(narrow, config, error);
  h.dispatcher.context().roots = &narrow;
  // Refused: a document that exists, outside the root it is spelled under. Missing: nothing inside.
  const JsonValue refused = h.call("session.open", obj({{"path", JsonValue("work://../other")}}));
  const JsonValue missing = h.call("session.open", obj({{"path", JsonValue("work://nothing")}}));
  // An absolute spelling is the client's own, but it names the host's directories: not logged.
  const JsonValue absolute = h.call("session.open", obj({{"path", JsonValue(other)}}));
  log::remove_sink(&h.ring);
  log::reset_levels();
  h.dispatcher.context().roots = &h.roots;
  REQUIRE(configured);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(missing.find("error") != nullptr);
  REQUIRE(absolute.find("error") != nullptr);

  const JsonValue tail = h.ok("log.tail", obj({{"since", JsonValue(0)}}));
  const std::string text = folded(write_json(tail));
  CHECK(text.find(folded(tmp.path())) == std::string::npos);
  CHECK(text.find("\"reason\"") == std::string::npos);
  CHECK(text.find("\"resolved\"") == std::string::npos);
  CHECK(text.find("\"canonical\"") == std::string::npos);
  // The refused and the missing path leave one record each, alike but for the client's spelling.
  const JsonValue& records = at(tail, "records");
  Vector<const JsonValue*> paths;
  for (usize i = 0; i < records.size(); ++i) {
    if (at(records[i], "cat") == JsonValue("protocol.paths") &&
        at(records[i], "level") == JsonValue("warn"))
      paths.push_back(&records[i]);
  }
  REQUIRE(paths.size() == 3);
  CHECK(at(*paths[0], "msg") == at(*paths[1], "msg"));
  CHECK(at(at(*paths[0], "fields"), "access") == at(at(*paths[1], "fields"), "access"));
  CHECK(at(at(*paths[0], "fields"), "field") == at(at(*paths[1], "fields"), "field"));
  CHECK(at(at(*paths[0], "fields"), "path") == JsonValue("work://../other"));
  CHECK(at(at(*paths[1], "fields"), "path") == JsonValue("work://nothing"));
  CHECK(at(at(*paths[2], "fields"), "path") == JsonValue("(absolute)"));
  // The root's directory is the operator's, on stderr at start.
  CHECK(folded(describe_host_roots(narrow, "engine-host")).find(folded(tmp.file("work"))) !=
        std::string::npos);
}

// session.open needs only read, so before this a document under a root the operator named
// read-only was opened and then saved in place by doc.save (every role), and rewritten with new
// content by a writing role's doc.apply: the host wrote inside a root it was told not to.
TEST_CASE("roots: a document under a read-only root opens and does not save") {
  TempDir tmp("protocol_roots_readonly");
  const Id128 a = Id128::from_parts(0x71, 1);
  Host h;
  const std::string world = tmp.file("lib") + "/world";
  JsonValue made =
      h.ok("session.open", obj({{"path", JsonValue(world)}, {"create", JsonValue(true)}}));
  h.ok("session.close", obj({{"session", *made.find("session")}}));
  std::string manifest_before;
  REQUIRE(io::read_file(world + "/manifest.json", manifest_before) == io::Status::Ok);

  io::Roots read_only;
  REQUIRE(read_only.add("lib", tmp.file("lib"), false) == io::Status::Ok);
  h.dispatcher.context().roots = &read_only;
  const JsonValue opened = h.ok("session.open", obj({{"path", JsonValue("lib://world")}}));
  const JsonValue session = at(opened, "session");
  // Read as before.
  CHECK(at(h.ok("session.info", obj({{"session", session}})), "path") == JsonValue("lib://world"));
  // Every write to its directory is the refusal a path outside the roots gets.
  const auto refused = [&](std::string_view method, JsonValue params) {
    const JsonValue resp = h.call(method, std::move(params));
    const JsonValue* error = resp.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("code")->as_int() == codes::k_not_found);
    CHECK(error->find("message")->as_string() ==
          "path: no place this host may write at 'lib://world'");
  };
  refused("doc.save", obj({{"session", session}}));
  JsonValue commands = JsonValue::array();
  commands.push_back(create_command(a, "engine.content.AssetProvenance"));
  refused("doc.apply",
          obj({{"session", session}, {"commands", commands}, {"attribution", attribution()}}));
  refused("doc.undo", obj({{"session", session}}));
  std::string manifest_after;
  REQUIRE(io::read_file(world + "/manifest.json", manifest_after) == io::Status::Ok);
  CHECK(manifest_after == manifest_before);
  h.ok("session.close", obj({{"session", session}}));

  // The same document under a root the operator made writable saves.
  io::Roots writable;
  REQUIRE(writable.add("lib", tmp.file("lib"), true) == io::Status::Ok);
  h.dispatcher.context().roots = &writable;
  const JsonValue again = h.ok("session.open", obj({{"path", JsonValue("lib://world")}}));
  h.ok("doc.save", obj({{"session", at(again, "session")}}));
  h.ok("session.close", obj({{"session", at(again, "session")}}));
  h.dispatcher.context().roots = &h.roots;
}
