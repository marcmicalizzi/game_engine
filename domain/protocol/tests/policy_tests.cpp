// Plan 06 §6.5 over the dispatcher (docs/subsystems/protocol.md, "Roles, leases and proposals"):
// roles as configuration, leases refused at acquisition and checked at the edit, and proposal
// layers promoted by a structural merge that validates first.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/protocol/leases.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/proposals.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <domain/protocol/side_file.h>
#include <foundation/io/roots.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::protocol;
using TempDir = test::TempDir;

namespace {

const char* k_prop = "engine.protocol.test.Prop";
const char* k_note = "engine.protocol.test.Note";

JsonValue obj(std::initializer_list<std::pair<const char*, JsonValue>> fields) {
  JsonValue o = JsonValue::object();
  for (auto& [k, v] : fields)
    o.set(k, v);
  return o;
}

const JsonValue& at(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  REQUIRE_MESSAGE(v != nullptr, "no '" << key << "' in " << write_json(o));
  return *v;
}

std::string hex(Id128 id) {
  char buf[Id128::k_hex_length + 1];
  id.to_hex(buf);
  return buf;
}

Id128 oid(u64 n) { return Id128::from_parts(0x10, n); }

// Any JSON integer, signed or not.
i64 num(const JsonValue& v) {
  i64 n = -1;
  REQUIRE(v.get_i64(n));
  return n;
}

JsonValue position(f64 x, f64 z) {
  JsonValue p = JsonValue::array();
  p.push_back(JsonValue(x));
  p.push_back(JsonValue(0.0));
  p.push_back(JsonValue(z));
  return p;
}

JsonValue who(const char* actor, const char* role = "", const char* rationale = "because") {
  return obj({{"actor", JsonValue(actor)},
              {"role", JsonValue(role)},
              {"task", JsonValue("t")},
              {"rationale", JsonValue(rationale)}});
}

JsonValue create_prop(Id128 id, f64 x, f64 z, Id128 parent = {}) {
  JsonValue props = obj({{"name", JsonValue("p")}, {"position", position(x, z)}});
  JsonValue c = obj({{"kind", JsonValue("CreateObject")},
                     {"id", JsonValue(hex(id))},
                     {"type", JsonValue(k_prop)},
                     {"value", props}});
  if (!parent.is_null()) c.set("parent", JsonValue(hex(parent)));
  return c;
}

JsonValue create_note(Id128 id) {
  return obj({{"kind", JsonValue("CreateObject")},
              {"id", JsonValue(hex(id))},
              {"type", JsonValue(k_note)},
              {"value", obj({{"text", JsonValue("n")}})}});
}

JsonValue set(Id128 id, const char* name, JsonValue value) {
  return obj({{"kind", JsonValue("SetProperty")},
              {"id", JsonValue(hex(id))},
              {"name", JsonValue(name)},
              {"value", std::move(value)}});
}

JsonValue del(Id128 id) {
  return obj({{"kind", JsonValue("DeleteObject")}, {"id", JsonValue(hex(id))}});
}

JsonValue one(JsonValue command) {
  JsonValue list = JsonValue::array();
  list.push_back(std::move(command));
  return list;
}

JsonValue range(i32 x0, i32 y0, i32 x1, i32 y1) {
  return obj(
      {{"x0", JsonValue(x0)}, {"y0", JsonValue(y0)}, {"x1", JsonValue(x1)}, {"y1", JsonValue(y1)}});
}

JsonValue ranges(std::initializer_list<JsonValue> list) {
  JsonValue a = JsonValue::array();
  for (const JsonValue& r : list)
    a.push_back(r);
  return a;
}

JsonValue tile(i32 x, i32 y) {
  JsonValue t = JsonValue::array();
  t.push_back(JsonValue(x));
  t.push_back(JsonValue(y));
  return t;
}

JsonValue strings(std::initializer_list<const char*> list) {
  JsonValue a = JsonValue::array();
  for (const char* s : list)
    a.push_back(JsonValue(s));
  return a;
}

// One process: a session manager and a dispatcher with a policy (empty unless a case loads roles),
// over a document in the case's own scratch directory with a `world` layer tiled 10 m a side and
// four props in four tiles: 1 at (0,0), 2 at (1,0), 3 at (0,1), 4 at (5,5).
struct Host {
  io::Vfs vfs;
  SessionManager sessions{vfs};
  Policy policy;
  // Every path a case names is under a TempDir, and every TempDir under one parent: that parent is
  // the host's one writable root, as an operator's --root would be (paths.h).
  test::TempDir roots_anchor{"roots"};
  io::Roots roots;
  Dispatcher dispatcher{Context{&sessions, nullptr, nullptr, nullptr, &policy}};
  std::string session;

  Host() {
    (void)roots.add("tests", std::string(io::parent_path(roots_anchor.path())), true);
    dispatcher.context().roots = &roots;
    add_builtin_methods(dispatcher);
  }

  JsonValue call(std::string_view method, JsonValue params) {
    JsonValue req = obj({{"jsonrpc", JsonValue("2.0")}, {"id", JsonValue(1)}});
    req.set("method", JsonValue(method));
    if (params.is_object() && !session.empty() && params.find("session") == nullptr)
      params.set("session", JsonValue(session));
    req.set("params", std::move(params));
    return dispatcher.dispatch(req);
  }
  JsonValue ok(std::string_view method, JsonValue params = JsonValue::object()) {
    JsonValue resp = call(method, std::move(params));
    if (const JsonValue* e = resp.find("error")) FAIL(method << " failed: " << write_json(*e));
    return *resp.find("result");
  }
  // The error's code, with the whole error in `out`.
  i32 fails(std::string_view method, JsonValue params, JsonValue* out = nullptr) {
    JsonValue resp = call(method, std::move(params));
    const JsonValue* e = resp.find("error");
    REQUIRE_MESSAGE(e != nullptr, method << " succeeded: " << write_json(resp));
    if (out != nullptr) *out = *e;
    return static_cast<i32>(e->find("code")->as_int());
  }
  JsonValue apply(JsonValue commands, JsonValue attribution, const char* layer = "") {
    return ok("doc.apply", obj({{"commands", std::move(commands)},
                                {"attribution", std::move(attribution)},
                                {"layer", JsonValue(layer)}}));
  }
  i32 apply_fails(JsonValue commands, JsonValue attribution, const char* layer = "",
                  JsonValue* error = nullptr) {
    return fails("doc.apply",
                 obj({{"commands", std::move(commands)},
                      {"attribution", std::move(attribution)},
                      {"layer", JsonValue(layer)}}),
                 error);
  }

  void open(const std::string& dir, bool create) {
    JsonValue info =
        ok("session.open", obj({{"path", JsonValue(dir)}, {"create", JsonValue(create)}}));
    session = std::string(at(info, "session").as_string());
  }

  void make_world(const std::string& dir) {
    open(dir, true);
    ok("doc.add_layer",
       obj({{"name", JsonValue("world")}, {"partition", obj({{"tile_size", JsonValue(10.0)}})}}));
    JsonValue commands = JsonValue::array();
    commands.push_back(create_prop(oid(1), 5, 5));
    commands.push_back(create_prop(oid(2), 15, 5));
    commands.push_back(create_prop(oid(3), 5, 15));
    commands.push_back(create_prop(oid(4), 55, 55));
    CHECK(at(apply(commands, who("setup")), "committed") == JsonValue(true));
  }

  // The composed value: what everyone reads, open proposals included.
  JsonValue property(Id128 id, const char* name) {
    JsonValue view = ok("doc.get", obj({{"id", JsonValue(hex(id))}}));
    const JsonValue* v = at(view, "properties").find(name);
    return v != nullptr ? *v : JsonValue();
  }
  // One layer's own record, open proposals left out: what a promotion changed.
  JsonValue layer_property(const char* layer, Id128 id, const char* name) {
    const doc::Document& d = sessions.find(session)->document();
    const i32 index = d.find_layer(layer);
    REQUIRE(index >= 0);
    const doc::ObjectRecord* r = d.layer(static_cast<u32>(index)).find(id);
    if (r == nullptr) return JsonValue();
    const JsonValue* v = r->properties.find_value(std::string(name));
    return v != nullptr ? *v : JsonValue();
  }
};

const char* k_roles = R"({
  "default_role": "qa",
  "roles": [
    {"name": "director", "methods": ["*"], "layers": ["*"], "object_types": ["*"]},
    {"name": "mapper", "methods": ["doc.apply", "lease.*"], "layers": ["world"],
     "tiles": [{"x0": 0, "y0": 0, "x1": 1, "y1": 1}], "untiled": false,
     "object_types": ["engine.protocol.test.Prop"]},
    {"name": "writer", "methods": ["doc.*"], "layers": ["*"], "object_types": ["*"],
     "review_required": true},
    {"name": "qa", "methods": []}
  ]
})";

}  // namespace

TEST_CASE("policy: patterns and tile ranges") {
  CHECK(glob_match("*", ""));
  CHECK(glob_match("*", "doc.apply"));
  CHECK(glob_match("doc.*", "doc.apply"));
  CHECK_FALSE(glob_match("doc.*", "lease.list"));
  CHECK(glob_match("doc.apply", "doc.apply"));
  CHECK_FALSE(glob_match("doc.apply", "doc.apply2"));
  CHECK(glob_match("*.test.*", "engine.protocol.test.Prop"));
  CHECK(glob_match("a*b*c", "aXXbYYc"));
  CHECK_FALSE(glob_match("a*b*c", "aXXbYY"));
  CHECK(glob_match("**", "x"));
  const TileRange r{0, 0, 1, 1};
  CHECK(tile_in(r, {1, 1}));
  CHECK_FALSE(tile_in(r, {2, 1}));
  CHECK(ranges_overlap(r, TileRange{1, 1, 3, 3}));
  CHECK_FALSE(ranges_overlap(r, TileRange{2, 0, 3, 3}));
  Vector<TileRange> outer;
  outer.push_back(TileRange{-5, -5, 5, 5});
  CHECK(range_within(r, outer));
  CHECK_FALSE(range_within(TileRange{0, 0, 6, 0}, outer));
}

TEST_CASE("policy: a roles file is checked when it is read") {
  Policy p;
  std::string error;
  CHECK_FALSE(p.load_roles("r.json", "{not json", error));
  CHECK_FALSE(p.load_roles("r.json", R"({"roles":[{"name":""}]})", error));
  CHECK(error.find("has no name") != std::string::npos);
  CHECK_FALSE(p.load_roles("r.json", R"({"roles":[{"name":"a"},{"name":"a"}]})", error));
  CHECK(error.find("defined twice") != std::string::npos);
  CHECK_FALSE(p.load_roles("r.json", R"({"roles":[{"name":"a","methods":[""]}]})", error));
  CHECK(error.find("empty entry") != std::string::npos);
  CHECK_FALSE(p.load_roles(
      "r.json", R"({"roles":[{"name":"a","tiles":[{"x0":2,"y0":0,"x1":1,"y1":0}]}]})", error));
  CHECK(error.find("tile range") != std::string::npos);
  CHECK_FALSE(p.load_roles("r.json", R"({"default_role":"b","roles":[{"name":"a"}]})", error));
  CHECK(error.find("default_role") != std::string::npos);
  CHECK_FALSE(p.load_roles("r.json", R"({"roles":[{"name":"a","bogus":1}]})", error));
  CHECK_FALSE(p.roles_loaded());
  CHECK(p.load_roles("r.json", k_roles, error));
  CHECK(p.roles_loaded());
  CHECK(p.find_role("mapper") != nullptr);

  // The shipped file reads, and has the five roles plan 06 names.
  const std::string path = test::data_path(
      std::string(ENGINE_SOURCE_DIR "/content/roles/roles.json"), "content/roles/roles.json");
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot read " << path);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Policy shipped;
  REQUIRE_MESSAGE(shipped.load_roles(path, text, error), error);
  for (const char* name : {"director", "designer", "environment", "qa", "performance", "reviewer"})
    CHECK_MESSAGE(shipped.find_role(name) != nullptr, name);
  // A person's role is the reviewer alone: no role an agent runs in closes a human review item.
  for (const Role& role : shipped.roles().roles) {
    const bool human = role.human;  // engine-lint: allow-human-flag the shipped file's own flags
    CHECK_MESSAGE(human == (role.name == "reviewer"), role.name);
  }
  CHECK(shipped.roles().default_role == "qa");
  // QA writes nothing of a document: what it may call is the host's own state that looking needs
  // (protocol.md, "What a read may do"), and no layer, tile or object type.
  {
    const Role& qa = *shipped.find_role("qa");
    const char* const expect[] = {"profile.start", "profile.stop", "render.unload",
                                  "session.materialize"};
    REQUIRE(qa.methods.size() == 4);
    for (u32 i = 0; i < 4; ++i)
      CHECK(qa.methods[i] == expect[i]);
    CHECK(qa.layers.empty());
    CHECK(qa.object_types.empty());
  }
  CHECK(shipped.find_role("designer")->review_required);
}

// Every method's class (protocol.md, "What a read may do"): a read, a read that leaves a file, or a
// write. The builtin table's three that the sort moved are pinned here so a later registration
// cannot quietly put them back; engine-host's and the editor's are pinned by their own tests.
TEST_CASE("roles: a read that leaves a file says so, and doc.save is a write") {
  TempDir tmp("protocol_roles_sort");
  Host host;
  host.make_world(tmp.file("doc"));
  const JsonValue methods = host.ok("engine.methods", JsonValue());
  int seen = 0;
  for (usize i = 0; i < at(methods, "methods").size(); ++i) {
    const JsonValue& m = at(methods, "methods")[i];
    const JsonValue& name = at(m, "name");
    const bool read = at(m, "read_only") == JsonValue(true);
    const bool files = at(m, "leaves_files") == JsonValue(true);
    // No write claims to leave a file: the flag is a read's.
    CHECK_MESSAGE(!(files && !read), at(m, "name").as_string());
    if (name == JsonValue("doc.save")) {
      CHECK_FALSE(read);
      ++seen;
    }
    if (name == JsonValue("session.open")) {
      CHECK(read);
      CHECK(files);
      ++seen;
    }
    if (name == JsonValue("doc.get") || name == JsonValue("lease.list")) {
      CHECK(read);
      CHECK_FALSE(files);
      ++seen;
    }
  }
  CHECK(seen == 4);
}

TEST_CASE("roles: no roles file restricts nothing, and the gate reads only writes") {
  TempDir tmp("protocol_roles");
  Host host;
  host.make_world(tmp.file("doc"));
  // No roles: a call naming any role, or none, writes.
  host.apply(one(set(oid(1), "color", JsonValue(3))), who("anyone", "whatever"));

  std::string error;
  REQUIRE(host.policy.load_roles("roles.json", k_roles, error));
  // The catalogue says which methods are reads.
  bool apply_write = false, objects_read = false, list_read = false;
  const JsonValue methods = host.ok("engine.methods", JsonValue());
  for (usize i = 0; i < at(methods, "methods").size(); ++i) {
    const JsonValue& m = at(methods, "methods")[i];
    if (at(m, "name") == JsonValue("doc.apply"))
      apply_write = at(m, "read_only") == JsonValue(false);
    if (at(m, "name") == JsonValue("doc.objects"))
      objects_read = at(m, "read_only") == JsonValue(true);
    if (at(m, "name") == JsonValue("lease.list")) list_read = at(m, "read_only") == JsonValue(true);
  }
  CHECK(apply_write);
  CHECK(objects_read);
  CHECK(list_read);

  // QA (named, or the default for a call that names none) reads and writes nothing.
  JsonValue e;
  CHECK(host.apply_fails(one(set(oid(1), "color", JsonValue(4))), who("q", "qa"), "", &e) ==
        codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("method"));
  CHECK(at(at(e, "data"), "method") == JsonValue("doc.apply"));
  CHECK(host.apply_fails(one(set(oid(1), "color", JsonValue(4))), who("q")) == codes::k_forbidden);
  CHECK(host.fails("doc.add_layer", obj({{"name", JsonValue("x")}})) == codes::k_forbidden);
  host.ok("doc.objects", obj({}));
  host.ok("doc.validate", obj({}));
  host.ok("lease.list", obj({}));
  // A role nobody configured is refused, not waved through.
  CHECK(host.apply_fails(one(set(oid(1), "color", JsonValue(4))), who("x", "hacker"), "", &e) ==
        codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("unknown role"));
  // The director writes; the host's identity is what a call naming nobody runs as.
  host.apply(one(set(oid(1), "color", JsonValue(5))), who("d", "director"));
  host.policy.identity.role = "director";
  host.ok("doc.add_layer", obj({{"name", JsonValue("x")}, {"edit", JsonValue(false)}}));

  // engine.roles: every role with the methods it may call, reads included.
  const JsonValue roles = host.ok("engine.roles", JsonValue());
  CHECK(at(roles, "loaded") == JsonValue(true));
  CHECK(at(roles, "default_role") == JsonValue("qa"));
  CHECK(at(roles, "role") == JsonValue("director"));
  bool qa_reads = false, qa_writes = false;
  for (usize i = 0; i < at(roles, "roles").size(); ++i) {
    const JsonValue& r = at(roles, "roles")[i];
    if (at(at(r, "role"), "name") != JsonValue("qa")) continue;
    for (usize k = 0; k < at(r, "allowed_methods").size(); ++k) {
      const std::string_view m = at(r, "allowed_methods")[k].as_string();
      qa_reads = qa_reads || m == "doc.objects";
      qa_writes = qa_writes || m == "doc.apply";
    }
  }
  CHECK(qa_reads);
  CHECK_FALSE(qa_writes);
}

TEST_CASE("roles: layers, tiles and object types, judged where the object stands") {
  TempDir tmp("protocol_roles");
  Host host;
  host.make_world(tmp.file("doc"));
  std::string error;
  REQUIRE(host.policy.load_roles("roles.json", k_roles, error));
  const JsonValue mapper = who("m", "mapper");

  // Inside the role's tiles: fine. Outside them, before or after: refused, and nothing changes.
  host.apply(one(set(oid(1), "color", JsonValue(7))), mapper, "world");
  JsonValue e;
  CHECK(host.apply_fails(one(set(oid(4), "color", JsonValue(7))), mapper, "world", &e) ==
        codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("tile"));
  CHECK(at(at(e, "data"), "tile") == tile(5, 5));
  // A move out of the ranges is judged by where the object ends up; the whole transaction goes.
  JsonValue move = JsonValue::array();
  move.push_back(set(oid(2), "color", JsonValue(9)));
  move.push_back(set(oid(2), "position", position(35, 5)));
  CHECK(host.apply_fails(move, mapper, "world") == codes::k_forbidden);
  CHECK(host.property(oid(2), "color").is_null());
  // An object with no tile, and a type the role does not name.
  CHECK(host.apply_fails(one(create_note(oid(20))), mapper, "world", &e) == codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("untiled"));
  // Another layer.
  CHECK(host.apply_fails(one(set(oid(1), "color", JsonValue(1))), mapper, "base", &e) ==
        codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("layer"));
}

TEST_CASE("leases: acquisition refuses overlap with another holder") {
  TempDir tmp("protocol_leases");
  Host host;
  host.make_world(tmp.file("doc"));
  const JsonValue a = who("a"), b = who("b");
  JsonValue la = host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                               {"tiles", ranges({range(0, 0, 1, 1)})},
                                               {"attribution", a}}));
  CHECK(at(la, "actor") == JsonValue("a"));
  const JsonValue first = at(la, "id");  // drawn, not counted (protocol.md, "Leases")
  CHECK(first.as_uint() > 0);
  // b overlapping a: refused, naming a's lease. a overlapping itself: fine.
  JsonValue e;
  CHECK(host.fails("lease.acquire",
                   obj({{"layer", JsonValue("world")},
                        {"tiles", ranges({range(1, 1, 2, 2)})},
                        {"attribution", b}}),
                   &e) == codes::k_lease_conflict);
  CHECK(at(at(at(e, "data"), "lease"), "actor") == JsonValue("a"));
  const JsonValue second = at(host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                                            {"tiles", ranges({range(1, 1, 1, 1)})},
                                                            {"attribution", a}})),
                              "id");
  CHECK_FALSE(second == first);
  // Next door, and another layer: fine.
  host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                {"tiles", ranges({range(2, 0, 3, 3)})},
                                {"attribution", b}}));
  host.ok("lease.acquire", obj({{"layer", JsonValue("base")},
                                {"tiles", ranges({range(0, 0, 1, 1)})},
                                {"attribution", b}}));
  // Types overlap by name.
  host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                {"object_types", strings({k_note})},
                                {"attribution", a}}));
  CHECK(host.fails("lease.acquire", obj({{"layer", JsonValue("world")},
                                         {"object_types", strings({k_note})},
                                         {"attribution", b}})) == codes::k_lease_conflict);
  // Malformed asks.
  CHECK(host.fails("lease.acquire", obj({{"layer", JsonValue("world")}, {"attribution", a}})) ==
        codes::k_invalid_argument);
  CHECK(host.fails("lease.acquire", obj({{"layer", JsonValue("world")},
                                         {"tiles", ranges({range(0, 0, 0, 0)})},
                                         {"ttl_seconds", JsonValue(0.0)},
                                         {"attribution", a}})) == codes::k_invalid_argument);
  CHECK(host.fails("lease.acquire", obj({{"layer", JsonValue("nope")},
                                         {"tiles", ranges({range(0, 0, 0, 0)})},
                                         {"attribution", a}})) == codes::k_not_found);
  CHECK(host.fails("lease.acquire", obj({{"layer", JsonValue("world")},
                                         {"object_types", strings({"engine.*"})},
                                         {"attribution", a}})) == codes::k_invalid_argument);
  CHECK(host.fails("lease.acquire", obj({{"layer", JsonValue("world")},
                                         {"tiles", ranges({range(9, 9, 9, 9)})},
                                         {"attribution", who("")}})) == codes::k_invalid_argument);

  // Visible to everyone; only the holder renews or releases.
  JsonValue list = host.ok("lease.list", obj({}));
  CHECK(at(list, "leases").size() == 5);
  CHECK(at(host.ok("lease.list", obj({{"actor", JsonValue("b")}})), "leases").size() == 2);
  CHECK(host.fails("lease.release", obj({{"lease", first}, {"attribution", b}})) ==
        codes::k_forbidden);
  CHECK(host.fails("lease.renew", obj({{"lease", first}, {"attribution", b}})) ==
        codes::k_forbidden);
  JsonValue renewed =
      host.ok("lease.renew",
              obj({{"lease", first}, {"ttl_seconds", JsonValue(3600.0)}, {"attribution", a}}));
  CHECK(num(at(renewed, "expires_unix_ms")) > num(at(la, "expires_unix_ms")));
  host.ok("lease.release", obj({{"lease", first}, {"attribution", a}}));
  host.ok("lease.release", obj({{"lease", second}, {"attribution", a}}));
  CHECK(host.fails("lease.release", obj({{"lease", first}, {"attribution", a}})) ==
        codes::k_not_found);
  // Released, the tiles are b's to take.
  host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                {"tiles", ranges({range(1, 1, 2, 2)})},
                                {"attribution", b}}));
}

TEST_CASE("leases: expiry is by the host's clock, and the table is kept beside the document") {
  TempDir tmp("protocol_leases");
  const std::string dir = tmp.file("doc");
  {
    Host host;
    host.make_world(dir);
  }
  io::Vfs vfs;
  SessionManager sessions(vfs);
  RpcError error;
  SessionOpenParams open;
  open.path = dir;
  Session* s = sessions.open(open, 0, error);
  REQUIRE(s != nullptr);
  Caller a, b;
  a.who.actor = "a";
  b.who.actor = "b";
  LeaseAcquireParams p;
  p.layer = "world";
  p.tiles.push_back(TileRange{0, 0, 0, 0});
  p.ttl_seconds = 10;
  LeaseInfo held;
  REQUIRE(s->acquire_lease(p, a, 1'000'000, held, error));
  CHECK(held.expires_unix_ms == 1'010'000);
  CHECK_FALSE(s->acquire_lease(p, b, 1'009'999, held, error));
  CHECK(error.code == codes::k_lease_conflict);
  // At its expiry it covers nothing, and the next acquisition drops it.
  REQUIRE(s->acquire_lease(p, b, 1'010'000, held, error));
  CHECK(s->leases().leases().size() == 1);
  CHECK(s->leases().leases()[0].actor == "b");
  LeasesResult listed;
  REQUIRE(s->list_leases("", "", 2'000'000, listed, error));
  CHECK(listed.expired == 1);
  CHECK(listed.leases.empty());
  REQUIRE(s->acquire_lease(p, a, 3'000'000, held, error));
  CHECK(s->renew_lease(held.id, 10, a, 4'000'000, held, error) == false);
  CHECK(error.code == codes::k_not_found);

  // A second process sees what the first granted.
  REQUIRE(s->acquire_lease(p, a, time::wall_unix_ms(), held, error));
  REQUIRE(s->require_leases(true, time::wall_unix_ms(), listed, error));
  io::Vfs vfs2;
  SessionManager sessions2(vfs2);
  Session* s2 = sessions2.open(open, 0, error);
  REQUIRE(s2 != nullptr);
  CHECK(s2->leases().required());
  REQUIRE(s2->leases().leases().size() == 1);
  CHECK(s2->leases().leases()[0].id == held.id);
  // Two hosts on the document at once (protocol.md, "Two hosts, one side file"): each grants a
  // lease from the table the file holds at the time, not the one it read at its open, and the ids
  // are drawn rather than counted from a number both read.
  LeaseInfo second;
  REQUIRE(s2->acquire_lease(p, a, time::wall_unix_ms(), second, error));
  CHECK(second.id != held.id);
  LeaseInfo third;
  REQUIRE(s->acquire_lease(p, a, time::wall_unix_ms(), third, error));
  CHECK(third.id != second.id);
  LeasesResult all;
  REQUIRE(s2->list_leases("", "", time::wall_unix_ms(), all, error));
  REQUIRE(all.leases.size() == 3);
  CHECK(all.leases[0].id == held.id);
  CHECK(all.leases[1].id == second.id);
  CHECK(all.leases[2].id == third.id);
}

TEST_CASE("leases: when the document requires them, every write is covered") {
  TempDir tmp("protocol_leases");
  Host host;
  host.make_world(tmp.file("doc"));
  const JsonValue a = who("a"), b = who("b");
  // Not required: anyone writes, as before leases existed.
  host.apply(one(set(oid(1), "color", JsonValue(1))), b, "world");
  host.ok("lease.require", obj({{"required", JsonValue(true)}, {"attribution", a}}));

  JsonValue e;
  CHECK(host.apply_fails(one(set(oid(1), "color", JsonValue(2))), a, "world", &e) ==
        codes::k_lease_required);
  CHECK(at(at(e, "data"), "tile") == tile(0, 0));
  host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                {"tiles", ranges({range(0, 0, 1, 0)})},
                                {"attribution", a}}));
  host.apply(one(set(oid(1), "color", JsonValue(2))), a, "world");
  host.apply(one(create_prop(oid(10), 12, 3)), a, "world");  // tile (1, 0)
  // b holds nothing: a's tiles are a's.
  CHECK(host.apply_fails(one(set(oid(2), "color", JsonValue(3))), b, "world", &e) ==
        codes::k_lease_conflict);
  CHECK(at(at(at(e, "data"), "lease"), "actor") == JsonValue("a"));
  // A move out of a's tiles into tiles a does not hold.
  CHECK(host.apply_fails(one(set(oid(1), "position", position(5, 25))), a, "world", &e) ==
        codes::k_lease_required);
  // A type lease covers a record with no tile; a tile lease never does.
  CHECK(host.apply_fails(one(create_note(oid(30))), a, "world") == codes::k_lease_required);
  host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                {"object_types", strings({k_note})},
                                {"attribution", a}}));
  host.apply(one(create_note(oid(30))), a, "world");
  // A tile lease and a type lease can both cover a record; that is refused at the edit.
  host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                {"object_types", strings({k_prop})},
                                {"attribution", b}}));
  CHECK(host.apply_fails(one(set(oid(1), "color", JsonValue(4))), a, "world", &e) ==
        codes::k_lease_conflict);
  CHECK(at(at(at(e, "data"), "lease"), "actor") == JsonValue("b"));
  // Undo is a write like any other: b's undo of a's patch is refused and put back.
  const u32 journal_before =
      static_cast<u32>(num(at(host.ok("session.info", obj({})), "journal_length")));
  CHECK(host.fails("doc.undo", obj({{"attribution", b}})) == codes::k_lease_conflict);
  const JsonValue info = host.ok("session.info", obj({}));
  CHECK(num(at(info, "undo_position")) == journal_before);
  CHECK(host.property(oid(30), "text") == JsonValue("n"));
}

TEST_CASE("proposals: an owner's layer, promoted by merge, taken back by two undos") {
  TempDir tmp("protocol_proposals");
  Host host;
  host.make_world(tmp.file("doc"));
  const JsonValue a = who("a", "", "recolour the square");
  JsonValue layers = host.ok(
      "doc.propose_layer",
      obj({{"name", JsonValue("p.a")}, {"target", JsonValue("world")}, {"attribution", a}}));
  const JsonValue& stack = at(layers, "layers");
  const JsonValue& p = stack[stack.size() - 1];
  CHECK(at(p, "name") == JsonValue("p.a"));
  CHECK(at(p, "role") == JsonValue("Proposal"));
  CHECK(at(p, "is_edit") == JsonValue(true));
  CHECK(at(at(p, "partition"), "tile_size") == JsonValue(10.0));  // the target's form
  CHECK(at(at(p, "proposal"), "owner") == JsonValue("a"));
  CHECK(at(at(p, "proposal"), "target") == JsonValue("world"));
  CHECK(at(at(p, "proposal"), "state") == JsonValue("Open"));

  // Only its owner writes it.
  JsonValue e;
  CHECK(host.apply_fails(one(set(oid(1), "color", JsonValue(1))), who("b"), "p.a", &e) ==
        codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("proposal owner"));

  JsonValue edits = JsonValue::array();
  edits.push_back(set(oid(1), "color", JsonValue(11)));
  edits.push_back(create_prop(oid(10), 7, 7, oid(1)));
  edits.push_back(del(oid(3)));
  host.apply(edits, a, "p.a");
  // The proposal composes over the world for everyone to read.
  CHECK(host.property(oid(1), "color") == JsonValue(11));
  const JsonValue after_edits = host.ok("doc.layers", obj({}));
  CHECK(at(at(at(after_edits, "layers")[2], "proposal"), "touched") == JsonValue(u32{3}));

  // A dry run commits nothing.
  const JsonValue promoter = who("d", "", "reviewed");
  JsonValue dry = host.ok("doc.promote", obj({{"proposal", JsonValue("p.a")},
                                              {"dry_run", JsonValue(true)},
                                              {"attribution", promoter}}));
  CHECK(at(dry, "ok") == JsonValue(true));
  CHECK(at(dry, "promoted") == JsonValue(false));
  CHECK(at(dry, "records") == JsonValue(u32{3}));
  const i64 journal = num(at(host.ok("session.info", obj({})), "journal_length"));

  JsonValue done =
      host.ok("doc.promote", obj({{"proposal", JsonValue("p.a")}, {"attribution", promoter}}));
  CHECK(at(done, "promoted") == JsonValue(true));
  CHECK(at(done, "conflicts").size() == 0);
  CHECK(at(done, "validation").size() == 0);
  CHECK(at(done, "applied_proposal") == JsonValue(u32{3}));
  CHECK(num(at(done, "patch_index")) == journal);
  CHECK(num(at(done, "cleared_patch_index")) == journal + 1);
  // The world holds it; the proposal is empty and closed.
  const JsonValue world_layers = host.ok("doc.layers", obj({}));
  CHECK(at(at(world_layers, "layers")[1], "records") == JsonValue(u32{4}));  // 1, 2, 4, 10
  CHECK(at(at(world_layers, "layers")[2], "records") == JsonValue(u32{0}));
  CHECK(at(at(at(world_layers, "layers")[2], "proposal"), "state") == JsonValue("Promoted"));
  CHECK(host.property(oid(1), "color") == JsonValue(11));
  CHECK(host.fails("doc.get", obj({{"id", JsonValue(hex(oid(3)))}})) == codes::k_not_found);
  CHECK(at(host.ok("doc.validate", obj({})), "ok") == JsonValue(true));
  // The journal says who promoted it and why the proposal existed.
  JsonValue j = host.ok("doc.journal", obj({{"offset", JsonValue(static_cast<u32>(journal))}}));
  const JsonValue& attribution = at(at(j, "patches")[0], "attribution");
  CHECK(at(attribution, "actor") == JsonValue("d"));
  CHECK(at(attribution, "rationale").as_string().find("recolour the square") != std::string::npos);
  CHECK(at(at(j, "patches")[0], "layer") == JsonValue("world"));
  // A closed proposal refuses writes until reopened.
  CHECK(host.apply_fails(one(set(oid(2), "color", JsonValue(1))), a, "p.a", &e) ==
        codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("proposal closed"));

  // Two undos take it back whole: the world as it was, the proposal holding its records again.
  host.ok("doc.undo", obj({{"steps", JsonValue(u32{2})}, {"attribution", promoter}}));
  CHECK(host.layer_property("world", oid(1), "color").is_null());
  CHECK(host.layer_property("world", oid(3), "name") == JsonValue("p"));
  CHECK(host.layer_property("p.a", oid(1), "color") == JsonValue(11));
  host.ok("doc.redo", obj({{"steps", JsonValue(u32{2})}, {"attribution", promoter}}));
  CHECK(host.layer_property("world", oid(1), "color") == JsonValue(11));
  CHECK(host.layer_property("p.a", oid(1), "color").is_null());

  // Reopened by its owner, over the same target, empty.
  CHECK(host.fails("doc.propose_layer", obj({{"name", JsonValue("p.a")},
                                             {"target", JsonValue("world")},
                                             {"attribution", who("b")}})) ==
        codes::k_invalid_argument);
  host.ok("doc.propose_layer",
          obj({{"name", JsonValue("p.a")}, {"target", JsonValue("world")}, {"attribution", a}}));
  host.apply(one(set(oid(2), "color", JsonValue(22))), a, "p.a");
  // Rejected: emptied, never merged.
  host.ok("doc.reject", obj({{"proposal", JsonValue("p.a")}, {"attribution", a}}));
  CHECK(host.property(oid(2), "color").is_null());
}

// A proposal's opening, promotion, reopening and rejection reach a subscriber as `event.session`
// (protocol.md, "Proposal layers and promotion"), each after `proposals.json` is written, naming
// the layer and who did it; a dry run publishes nothing.
TEST_CASE("proposals: opening, promoting, reopening and rejecting are pushed as session events") {
  struct Collect final : EventTransport {
    std::vector<std::string> sent;
    Post post(u64, std::string text, u32) override {
      sent.push_back(std::move(text));
      return Post::Queued;
    }
  } pushed;
  Subscriptions subscriptions;
  subscriptions.attach(&pushed);
  TempDir tmp("protocol_proposals");
  Host host;
  host.sessions.set_events(&subscriptions);
  (void)subscriptions.subscribe(7, "", event_bit(EventKind::Session));
  host.make_world(tmp.file("doc"));
  const auto proposal_changes = [&] {
    std::vector<std::string> out;
    for (const std::string& text : pushed.sent) {
      JsonValue note;
      REQUIRE(parse_json(text, note).ok);
      const JsonValue& p = at(note, "params");
      const std::string change(at(p, "change").as_string());
      if (change.rfind("proposal_", 0) != 0) continue;
      out.push_back(change + " " + std::string(at(p, "layer").as_string()) + " " +
                    std::string(at(p, "actor").as_string()));
    }
    return out;
  };
  const JsonValue a = who("a"), d = who("d");
  const JsonValue open =
      obj({{"name", JsonValue("p.a")}, {"target", JsonValue("world")}, {"attribution", a}});
  host.ok("doc.propose_layer", open);
  host.apply(one(set(oid(1), "color", JsonValue(5))), a, "p.a");
  host.ok("doc.promote",
          obj({{"proposal", JsonValue("p.a")}, {"dry_run", JsonValue(true)}, {"attribution", d}}));
  CHECK(proposal_changes() == std::vector<std::string>{"proposal_opened p.a a"});
  REQUIRE(at(host.ok("doc.promote", obj({{"proposal", JsonValue("p.a")}, {"attribution", d}})),
             "promoted") == JsonValue(true));
  host.ok("doc.propose_layer", open);
  host.apply(one(set(oid(2), "color", JsonValue(6))), a, "p.a");
  host.ok("doc.reject", obj({{"proposal", JsonValue("p.a")}, {"attribution", a}}));
  CHECK(proposal_changes() ==
        std::vector<std::string>{"proposal_opened p.a a", "proposal_promoted p.a d",
                                 "proposal_reopened p.a a", "proposal_rejected p.a a"});
}

TEST_CASE("proposals: a conflict refuses the promotion unless prefer settles it") {
  TempDir tmp("protocol_proposals");
  Host host;
  host.make_world(tmp.file("doc"));
  const JsonValue a = who("a"), b = who("b"), d = who("d");
  host.ok("doc.propose_layer", obj({{"name", JsonValue("p.a")},
                                    {"target", JsonValue("world")},
                                    {"edit", JsonValue(false)},
                                    {"attribution", a}}));
  host.ok("doc.propose_layer", obj({{"name", JsonValue("p.b")},
                                    {"target", JsonValue("world")},
                                    {"edit", JsonValue(false)},
                                    {"attribution", b}}));
  host.apply(one(set(oid(1), "color", JsonValue(1))), a, "p.a");
  host.apply(one(set(oid(1), "color", JsonValue(2))), b, "p.b");
  host.apply(one(set(oid(2), "color", JsonValue(2))), b, "p.b");
  // Deleted on one side, modified on the other.
  host.apply(one(set(oid(4), "scale", JsonValue(2.0))), a, "p.a");
  host.apply(one(del(oid(4))), b, "p.b");
  REQUIRE(at(host.ok("doc.promote", obj({{"proposal", JsonValue("p.a")}, {"attribution", d}})),
             "promoted") == JsonValue(true));

  JsonValue refused =
      host.ok("doc.promote", obj({{"proposal", JsonValue("p.b")}, {"attribution", d}}));
  CHECK(at(refused, "promoted") == JsonValue(false));
  CHECK(at(refused, "ok") == JsonValue(false));
  CHECK(at(refused, "refused").as_string().find("conflict") != std::string::npos);
  REQUIRE(at(refused, "conflicts").size() == 2);
  bool both_changed = false, deleted_modified = false;
  for (usize i = 0; i < at(refused, "conflicts").size(); ++i) {
    const JsonValue& c = at(refused, "conflicts")[i];
    both_changed = both_changed || at(c, "kind") == JsonValue("PropertyBothChanged");
    deleted_modified = deleted_modified || at(c, "kind") == JsonValue("DeletedAndModified");
  }
  CHECK(both_changed);
  CHECK(deleted_modified);
  CHECK(host.layer_property("world", oid(1), "color") == JsonValue(1));  // nothing moved
  CHECK(host.layer_property("world", oid(2), "color").is_null());

  JsonValue taken = host.ok(
      "doc.promote",
      obj({{"proposal", JsonValue("p.b")}, {"prefer", JsonValue("Proposal")}, {"attribution", d}}));
  CHECK(at(taken, "promoted") == JsonValue(true));
  CHECK(host.property(oid(1), "color") == JsonValue(2));
  CHECK(host.property(oid(2), "color") == JsonValue(2));
  // A deletion never silently destroys the other side's edit: the object stays, modified.
  CHECK(host.property(oid(4), "scale") == JsonValue(2.0));
}

// "Take the proposal's" applies to the values the person saw (protocol.md, "What a promotion is
// tied to"): a promotion carrying its dry run's digest is refused when the proposal's owner added a
// command since, or the target's value at a conflict moved, and taken when neither did.
TEST_CASE("proposals: a promotion is refused when what its dry run showed has moved") {
  TempDir tmp("protocol_proposals_seen");
  Host host;
  host.make_world(tmp.file("doc"));
  const JsonValue a = who("a"), b = who("b"), d = who("d");
  for (const char* name : {"p.a", "p.b"}) {
    host.ok("doc.propose_layer", obj({{"name", JsonValue(name)},
                                      {"target", JsonValue("world")},
                                      {"edit", JsonValue(false)},
                                      {"attribution", name[2] == 'a' ? a : b}}));
  }
  host.apply(one(set(oid(1), "color", JsonValue(1))), a, "p.a");
  host.apply(one(set(oid(1), "color", JsonValue(2))), b, "p.b");
  REQUIRE(at(host.ok("doc.promote", obj({{"proposal", JsonValue("p.a")}, {"attribution", d}})),
             "promoted") == JsonValue(true));
  const auto dry_run = [&] {
    const JsonValue dry = host.ok(
        "doc.promote",
        obj({{"proposal", JsonValue("p.b")}, {"dry_run", JsonValue(true)}, {"attribution", d}}));
    REQUIRE(at(dry, "conflicts").size() == 1);
    return at(dry, "digest");
  };
  const auto take = [&](const JsonValue& seen, JsonValue* error) {
    JsonValue params = obj({{"proposal", JsonValue("p.b")},
                            {"prefer", JsonValue("Proposal")},
                            {"digest", seen},
                            {"attribution", d}});
    if (error == nullptr) return host.ok("doc.promote", std::move(params));
    CHECK(host.fails("doc.promote", std::move(params), error) == codes::k_invalid_argument);
    CHECK(at(at(*error, "data"), "reason").as_string() == "proposal moved");
    return JsonValue();
  };
  JsonValue error;

  const JsonValue seen = dry_run();
  CHECK(seen.as_string().size() == 64);
  CHECK(dry_run() == seen);  // nothing moved, nothing differs

  // The owner adds a command between the person's dry run and the promotion.
  host.apply(one(set(oid(2), "color", JsonValue(7))), b, "p.b");
  (void)take(seen, &error);
  CHECK(at(at(error, "data"), "digest") != seen);

  // The target's value at the conflict moves.
  const JsonValue again = dry_run();
  host.apply(one(set(oid(1), "color", JsonValue(5))), d, "world");
  (void)take(again, &error);
  CHECK(host.layer_property("world", oid(1), "color") == JsonValue(5));  // nothing promoted

  // Seen as it is, taken.
  const JsonValue taken = take(dry_run(), nullptr);
  CHECK(at(taken, "promoted") == JsonValue(true));
  CHECK(host.property(oid(1), "color") == JsonValue(2));
}

TEST_CASE("proposals: the base is the record as the proposal first found it") {
  TempDir tmp("protocol_proposals");
  const std::string dir = tmp.file("doc");
  const JsonValue a = who("a"), b = who("b"), d = who("d");
  {
    Host host;
    host.make_world(dir);
    host.ok("doc.propose_layer", obj({{"name", JsonValue("p.a")},
                                      {"target", JsonValue("world")},
                                      {"edit", JsonValue(false)},
                                      {"attribution", a}}));
    host.ok("doc.propose_layer", obj({{"name", JsonValue("p.b")},
                                      {"target", JsonValue("world")},
                                      {"edit", JsonValue(false)},
                                      {"attribution", b}}));
    host.apply(one(set(oid(1), "color", JsonValue(1))), a, "p.a");
    host.apply(one(set(oid(2), "color", JsonValue(5))), b, "p.b");  // touches 2 first
    REQUIRE(at(host.ok("doc.promote", obj({{"proposal", JsonValue("p.a")}, {"attribution", d}})),
               "promoted") == JsonValue(true));
  }
  // A second process: the bases come back from disk.
  Host host;
  host.open(dir, false);
  // b touches 1 only now, after a's colour was promoted: it built on it, so no conflict.
  host.apply(one(set(oid(1), "color", JsonValue(3))), b, "p.b");
  JsonValue done =
      host.ok("doc.promote", obj({{"proposal", JsonValue("p.b")}, {"attribution", d}}));
  CHECK(at(done, "promoted") == JsonValue(true));
  CHECK(at(done, "conflicts").size() == 0);
  CHECK(host.property(oid(1), "color") == JsonValue(3));
  CHECK(host.property(oid(2), "color") == JsonValue(5));
}

TEST_CASE("proposals: validation blocks what the promotion introduces, not what was there") {
  TempDir tmp("protocol_proposals");
  Host host;
  host.make_world(tmp.file("doc"));
  const JsonValue a = who("a"), b = who("b"), d = who("d");
  // Already wrong in the world: a property Prop does not have.
  host.apply(one(set(oid(4), "bogus", JsonValue(1))), d, "world");
  REQUIRE(at(host.ok("doc.validate", obj({})), "ok") == JsonValue(false));
  host.ok("doc.propose_layer", obj({{"name", JsonValue("p.a")},
                                    {"target", JsonValue("world")},
                                    {"edit", JsonValue(false)},
                                    {"attribution", a}}));
  host.ok("doc.propose_layer", obj({{"name", JsonValue("p.b")},
                                    {"target", JsonValue("world")},
                                    {"edit", JsonValue(false)},
                                    {"attribution", b}}));
  // b places a child of 2 while 2 stands; a deletes 2. Each is sound on its own, and a strict
  // apply would have refused b's create had a's deletion come first (proposals compose).
  host.apply(one(create_prop(oid(10), 15, 6, oid(2))), b, "p.b");
  host.apply(one(del(oid(2))), a, "p.a");
  JsonValue first =
      host.ok("doc.promote", obj({{"proposal", JsonValue("p.a")}, {"attribution", d}}));
  CHECK(at(first, "promoted") == JsonValue(true));  // the old problem does not block it
  JsonValue second =
      host.ok("doc.promote", obj({{"proposal", JsonValue("p.b")}, {"attribution", d}}));
  CHECK(at(second, "promoted") == JsonValue(false));
  CHECK(at(second, "conflicts").size() == 0);  // no structural conflict: a new child
  REQUIRE(at(second, "validation").size() == 1);
  CHECK(at(at(second, "validation")[0], "message") == JsonValue("parent does not exist"));
  // prefer settles conflicts, never validation.
  CHECK(at(host.ok("doc.promote", obj({{"proposal", JsonValue("p.b")},
                                       {"prefer", JsonValue("Proposal")},
                                       {"attribution", d}})),
           "promoted") == JsonValue(false));
}

TEST_CASE("roles: a role that needs review proposes and does not promote") {
  TempDir tmp("protocol_proposals");
  Host host;
  host.make_world(tmp.file("doc"));
  std::string error;
  REQUIRE(host.policy.load_roles("roles.json", k_roles, error));
  const JsonValue w = who("w", "writer");
  JsonValue e;
  CHECK(host.apply_fails(one(set(oid(1), "color", JsonValue(1))), w, "world", &e) ==
        codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("review required"));
  host.ok("doc.propose_layer",
          obj({{"name", JsonValue("p.w")}, {"target", JsonValue("world")}, {"attribution", w}}));
  host.apply(one(set(oid(1), "color", JsonValue(1))), w, "p.w");
  CHECK(host.fails("doc.promote", obj({{"proposal", JsonValue("p.w")}, {"attribution", w}}), &e) ==
        codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("review required"));
  CHECK(at(host.ok("doc.promote",
                   obj({{"proposal", JsonValue("p.w")}, {"attribution", who("boss", "director")}})),
           "promoted") == JsonValue(true));
  // A lease is on the layer a proposal promotes into, never on the proposal.
  CHECK(host.fails("lease.acquire", obj({{"layer", JsonValue("p.w")},
                                         {"tiles", ranges({range(0, 0, 0, 0)})},
                                         {"attribution", who("m", "mapper")}})) ==
        codes::k_invalid_argument);
  // The mapper may lease only inside its own tiles.
  CHECK(host.fails("lease.acquire", obj({{"layer", JsonValue("world")},
                                         {"tiles", ranges({range(0, 0, 4, 4)})},
                                         {"attribution", who("m", "mapper")}})) ==
        codes::k_forbidden);
  host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                {"tiles", ranges({range(0, 0, 1, 1)})},
                                {"attribution", who("m", "mapper")}}));
}

TEST_CASE("proposals: compose_onto writes a proposal's opinions into a layer") {
  doc::Layer over("world", doc::LayerRole::Feature);
  doc::ObjectRecord a;
  a.id = oid(1);
  a.type = k_prop;
  a.properties.insert_or_assign(std::string("color"), JsonValue(1));
  over.set(a);
  doc::ObjectRecord b;
  b.id = oid(2);
  b.properties.insert_or_assign(std::string("color"), JsonValue(1));  // an override
  over.set(b);
  doc::Layer proposal("p", doc::LayerRole::Proposal);
  doc::ObjectRecord pa;
  pa.id = oid(1);
  pa.properties.insert_or_assign(std::string("scale"), JsonValue(2.0));
  proposal.set(pa);
  doc::ObjectRecord pb;
  pb.id = oid(2);
  pb.deleted = true;
  proposal.set(pb);
  doc::ObjectRecord pc;
  pc.id = oid(3);
  pc.type = k_prop;
  proposal.set(pc);
  doc::ObjectRecord pd;  // tombstones an object `over` defines
  pd.id = oid(1);
  const doc::Layer out = compose_onto(over, proposal);
  CHECK(out.name() == "world");
  REQUIRE(out.find(oid(1)) != nullptr);
  CHECK(*out.find(oid(1))->properties.find_value("color") == JsonValue(1));
  CHECK(*out.find(oid(1))->properties.find_value("scale") == JsonValue(2.0));
  REQUIRE(out.find(oid(2)) != nullptr);
  CHECK(out.find(oid(2))->deleted);  // defined elsewhere: tombstoned
  CHECK(out.find(oid(3)) != nullptr);
  pd.deleted = true;
  proposal.set(pd);
  CHECK(compose_onto(over, proposal).find(oid(1)) == nullptr);  // defined here: dropped
}

// ---- a client's transactions (plan 06 §6.4; protocol.md, "Transactions a client opens") --------

namespace {

JsonValue staged(Host& host, u64 transaction, JsonValue commands, JsonValue attribution,
                 bool atomic = true) {
  return host.ok("doc.apply", obj({{"commands", std::move(commands)},
                                   {"attribution", std::move(attribution)},
                                   {"atomic", JsonValue(atomic)},
                                   {"transaction", JsonValue(transaction)}}));
}

i32 staged_fails(Host& host, u64 transaction, JsonValue commands, JsonValue attribution,
                 JsonValue* error = nullptr) {
  return host.fails("doc.apply",
                    obj({{"commands", std::move(commands)},
                         {"attribution", std::move(attribution)},
                         {"transaction", JsonValue(transaction)}}),
                    error);
}

u64 begin_on(Host& host, const char* layer, JsonValue attribution, f64 ttl = 3600) {
  const JsonValue info = host.ok("doc.begin", obj({{"layer", JsonValue(layer)},
                                                   {"ttl_seconds", JsonValue(ttl)},
                                                   {"attribution", std::move(attribution)}}));
  return static_cast<u64>(num(at(info, "id")));
}

JsonValue ended(Host& host, const char* method, u64 transaction, JsonValue attribution) {
  return host.ok(method, obj({{"transaction", JsonValue(transaction)},
                              {"attribution", std::move(attribution)}}));
}

i64 journal_length(Host& host) {
  return num(at(host.ok("session.info", obj({})), "journal_length"));
}

}  // namespace

TEST_CASE("transactions: several calls land as one patch, the patch one doc.apply journals") {
  TempDir tmp("protocol_transactions");
  JsonValue commands = JsonValue::array();
  commands.push_back(set(oid(1), "color", JsonValue(7)));
  commands.push_back(create_prop(oid(10), 7, 7, oid(1)));
  commands.push_back(set(oid(10), "color", JsonValue(8)));
  JsonValue stamped = who("a", "", "a lamp beside the first prop");
  stamped.set("timestamp_unix_ms", JsonValue(i64{1'700'000'000'000}));

  // The same three commands as one plain doc.apply, in a second document, for the patch to match.
  Host plain;
  plain.make_world(tmp.file("plain"));
  REQUIRE(at(plain.apply(commands, stamped, "world"), "committed") == JsonValue(true));
  const std::string expected =
      write_json(schema::to_json(plain.sessions.find(plain.session)->document().journal().back()));

  Host host;
  host.make_world(tmp.file("doc"));
  const i64 before = journal_length(host);
  const u64 t = begin_on(host, "world", who("a", "", "a lamp beside the first prop"));
  CHECK(t > 0);
  JsonValue r = staged(host, t, one(commands[0]), who("a"));
  CHECK(at(r, "committed") == JsonValue(false));
  CHECK(num(at(r, "applied")) == 1);
  CHECK(num(at(r, "staged")) == 1);
  CHECK(num(at(r, "transaction")) == static_cast<i64>(t));
  // Staged is not landed: nobody reads it, and nothing is journaled.
  CHECK(host.property(oid(1), "color").is_null());
  CHECK(journal_length(host) == before);
  // A later call is checked against the earlier ones: oid 10 exists only in the transaction.
  staged(host, t, one(commands[1]), who("a"));
  CHECK(host.fails("doc.get", obj({{"id", JsonValue(hex(oid(10)))}})) == codes::k_not_found);
  r = staged(host, t, one(commands[2]), who("a"));
  CHECK(num(at(r, "staged")) == 3);
  // An atomic call that fails stages nothing; a non-atomic one stages what applied.
  r = staged(host, t, one(set(oid(99), "color", JsonValue(1))), who("a"));
  CHECK(num(at(r, "applied")) == 0);
  CHECK(at(r, "diagnostics").size() == 1);
  CHECK(num(at(r, "staged")) == 3);
  const JsonValue listed = host.ok("doc.transactions", obj({}));
  REQUIRE(at(listed, "transactions").size() == 1);
  const JsonValue& info = at(listed, "transactions")[0];
  CHECK(num(at(info, "commands")) == 3);
  CHECK(num(at(info, "applies")) == 3);
  CHECK(at(info, "layer") == JsonValue("world"));
  CHECK(at(at(info, "attribution"), "actor") == JsonValue("a"));

  r = ended(host, "doc.commit", t, stamped);
  CHECK(at(r, "committed") == JsonValue(true));
  CHECK(num(at(r, "applied")) == 3);
  CHECK(num(at(r, "staged")) == 0);
  CHECK(journal_length(host) == before + 1);
  CHECK(host.property(oid(10), "color") == JsonValue(8));
  // The patch one doc.apply journals, and it names the transaction it is the commit of.
  doc::Patch patch = host.sessions.find(host.session)->document().journal().back();
  CHECK(patch.transaction == t);
  patch.transaction = 0;
  CHECK(write_json(schema::to_json(patch)) == expected);
  CHECK(at(host.ok("doc.transactions", obj({})), "transactions").size() == 0);
  // Committed: committing it again (an answer the client never saw) is answered with the patch
  // that landed, and nothing is applied twice; rolling it back is 1003 naming that patch.
  r = ended(host, "doc.commit", t, who("a"));
  CHECK(at(r, "committed") == JsonValue(true));
  CHECK(num(at(r, "patch_index")) == before);
  CHECK(at(r, "diagnostics").size() == 1);
  CHECK(journal_length(host) == before + 1);
  CHECK(host.fails("doc.commit", obj({{"transaction", JsonValue(t)}, {"attribution", who("b")}})) ==
        codes::k_forbidden);
  JsonValue gone;
  CHECK(host.fails("doc.rollback", obj({{"transaction", JsonValue(t)}, {"attribution", who("a")}}),
                   &gone) == codes::k_not_found);
  CHECK(std::string(at(gone, "message").as_string()).find("journal patch") != std::string::npos);
  // Landed together, so one undo takes them back together.
  host.ok("doc.undo", obj({}));
  CHECK(host.property(oid(1), "color").is_null());
  CHECK(host.fails("doc.get", obj({{"id", JsonValue(hex(oid(10)))}})) == codes::k_not_found);
}

TEST_CASE("transactions: rollback and lapse leave nothing, and a transaction is its opener's") {
  TempDir tmp("protocol_transactions");
  const std::string dir = tmp.file("doc");
  u64 t = 0;
  {
    Host host;
    host.make_world(dir);
    const i64 before = journal_length(host);
    JsonValue e;
    CHECK(host.fails("doc.begin", obj({{"layer", JsonValue("world")},
                                       {"ttl_seconds", JsonValue(0.0)},
                                       {"attribution", who("a")}})) == codes::k_invalid_argument);
    CHECK(host.fails("doc.begin", obj({{"layer", JsonValue("nowhere")},
                                       {"attribution", who("a")}})) == codes::k_not_found);
    CHECK(host.fails("doc.begin", obj({{"layer", JsonValue("world")}, {"attribution", who("")}})) ==
          codes::k_invalid_argument);

    const u64 r = begin_on(host, "world", who("a"));
    staged(host, r, one(set(oid(2), "color", JsonValue(5))), who("a"));
    // Not b's to add to, commit or roll back.
    CHECK(staged_fails(host, r, one(set(oid(2), "color", JsonValue(6))), who("b"), &e) ==
          codes::k_forbidden);
    CHECK(at(at(e, "data"), "reason") == JsonValue("transaction owner"));
    CHECK(host.fails("doc.commit", obj({{"transaction", JsonValue(r)},
                                        {"attribution", who("b")}})) == codes::k_forbidden);
    CHECK(host.fails("doc.rollback", obj({{"transaction", JsonValue(r)},
                                          {"attribution", who("b")}})) == codes::k_forbidden);
    // A transaction is one layer's.
    CHECK(host.fails("doc.apply", obj({{"commands", one(set(oid(2), "color", JsonValue(6)))},
                                       {"attribution", who("a")},
                                       {"layer", JsonValue("base")},
                                       {"transaction", JsonValue(r)}})) ==
          codes::k_invalid_argument);
    const JsonValue gone = ended(host, "doc.rollback", r, who("a"));
    CHECK(num(at(gone, "commands")) == 1);
    CHECK(host.property(oid(2), "color").is_null());
    CHECK(journal_length(host) == before);
    CHECK(staged_fails(host, r, one(set(oid(2), "color", JsonValue(6))), who("a")) ==
          codes::k_not_found);

    // Another client commits under a staged command: the commit lands nothing and stays open.
    const u64 c = begin_on(host, "world", who("a"));
    staged(host, c, one(set(oid(4), "color", JsonValue(1))), who("a"));
    host.apply(one(del(oid(4))), who("b"), "world");
    const JsonValue refused = ended(host, "doc.commit", c, who("a"));
    CHECK(at(refused, "committed") == JsonValue(false));
    CHECK(at(refused, "diagnostics").size() >= 1);
    CHECK(num(at(refused, "staged")) == 1);
    CHECK(journal_length(host) == before + 1);  // b's delete alone
    ended(host, "doc.rollback", c, who("a"));

    // Lapsing is a rollback: judged by the host's clock, here a day ahead of it.
    const u64 l = begin_on(host, "world", who("a"), 60);
    staged(host, l, one(set(oid(3), "color", JsonValue(2))), who("a"));
    Session* s = host.sessions.find(host.session);
    Caller a;
    a.who.actor = "a";
    ApplyResult result;
    RpcError error;
    CHECK_FALSE(s->commit_transaction(l, a, time::wall_unix_ms() + 86'400'000, result, error));
    CHECK(error.code == codes::k_not_found);
    CHECK(error.message.find("lapsed") != std::string::npos);
    CHECK(host.property(oid(3), "color").is_null());

    // One left open for the next process.
    t = begin_on(host, "world", who("a"));
    staged(host, t, one(set(oid(3), "color", JsonValue(9))), who("a"));
    host.ok("session.close", obj({}));
  }
  // A host started for one call commits what an earlier one staged: the table is on disk.
  Host next;
  next.open(dir, false);
  const JsonValue listed = next.ok("doc.transactions", obj({{"actor", JsonValue("a")}}));
  REQUIRE(at(listed, "transactions").size() == 1);
  CHECK(num(at(at(listed, "transactions")[0], "id")) == static_cast<i64>(t));
  CHECK(at(ended(next, "doc.commit", t, who("a")), "committed") == JsonValue(true));
  CHECK(next.property(oid(3), "color") == JsonValue(9));
}

// Batch 68's review (2026-11-18; protocol.md, "Two hosts, one side file"): two host processes on
// one document — two engine-cli calls an agent made at once — each read `transactions.json` at
// their open. The second writer is a second session manager in this process on the same directory,
// interleaved the way two parallel calls are.
TEST_CASE("transactions: two hosts on one document lose no staged command and no transaction") {
  TempDir tmp("protocol_two_hosts");
  const std::string dir = tmp.file("doc");
  Host left;
  left.make_world(dir);
  const u64 t = begin_on(left, "world", who("a"));
  Host right;
  right.open(dir, false);
  staged(left, t, one(set(oid(1), "color", JsonValue(1))), who("a"));
  staged(right, t, one(set(oid(2), "color", JsonValue(2))), who("a"));
  const JsonValue r = staged(left, t, one(set(oid(3), "color", JsonValue(3))), who("a"));
  CHECK(num(at(r, "staged")) == 3);
  // A begin in each: two transactions with two ids, though each host read the same table.
  const u64 u = begin_on(right, "world", who("b"));
  const u64 v = begin_on(left, "world", who("c"));
  CHECK(u != v);
  CHECK(u != t);
  CHECK(v != t);
  CHECK(at(right.ok("doc.transactions", obj({})), "transactions").size() == 3);
  const JsonValue committed = ended(right, "doc.commit", t, who("a"));
  CHECK(at(committed, "committed") == JsonValue(true));
  CHECK(num(at(committed, "applied")) == 3);
  CHECK(right.property(oid(1), "color") == JsonValue(1));
  CHECK(right.property(oid(2), "color") == JsonValue(2));
  CHECK(right.property(oid(3), "color") == JsonValue(3));
}

// The same with real concurrency: eight writers, each its own session manager on its own thread,
// staging four calls each into one transaction at once. The lock is taken by handle, so two in one
// process exclude each other as two processes do.
TEST_CASE("transactions: eight writers at once stage every command") {
  TempDir tmp("protocol_two_hosts");
  const std::string dir = tmp.file("doc");
  Host host;
  host.make_world(dir);
  const u64 t = begin_on(host, "world", who("a"));
  constexpr u32 k_writers = 8;
  constexpr u32 k_calls = 4;
  Vector<doc::Command> commands;
  for (u32 i = 0; i < k_writers * k_calls; ++i) {
    doc::Command c;
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(c, set(oid(1 + i % 4), "color", JsonValue(i)), ctx));
    commands.push_back(std::move(c));
  }
  u32 failures[k_writers] = {};
  std::string first_error[k_writers];
  {
    std::vector<std::thread> writers;
    for (u32 w = 0; w < k_writers; ++w) {
      writers.emplace_back([&, w] {
        io::Vfs vfs;
        SessionManager sessions(vfs);
        SessionOpenParams open;
        open.path = dir;
        RpcError error;
        Session* s = sessions.open(open, 0, error);
        if (s == nullptr) {
          failures[w] = k_calls;
          first_error[w] = error.message;
          return;
        }
        Caller a;
        a.who.actor = "a";
        for (u32 c = 0; c < k_calls; ++c) {
          ApplyResult result;
          const doc::Command& command = commands[w * k_calls + c];
          if (!s->stage(t, {&command, 1}, a, true, "", time::wall_unix_ms(), result, error) ||
              result.applied != 1) {
            if (failures[w]++ == 0) first_error[w] = error.message;
          }
        }
      });
    }
    for (std::thread& w : writers)
      w.join();
  }
  for (u32 w = 0; w < k_writers; ++w)
    CHECK_MESSAGE(failures[w] == 0, "writer " << w << ": " << first_error[w]);
  const JsonValue listed = host.ok("doc.transactions", obj({}));
  REQUIRE(at(listed, "transactions").size() == 1);
  CHECK(num(at(at(listed, "transactions")[0], "commands")) == k_writers * k_calls);
  CHECK(num(at(at(listed, "transactions")[0], "applies")) == k_writers * k_calls);
}

// A commit that landed and was reported failed: a commit whose patch is journaled and saved, and
// whose write of `transactions.json` then fails (a scanner holding the file past the rename's
// retries, a host killed there), left the transaction open, and a retry applied its commands again.
// The patch names its transaction, so a retry — in this host, or in the next process, which reads
// the table that still lists it — is answered with the patch that landed.
TEST_CASE("transactions: a commit whose table write failed lands once, here and in the next host") {
  TempDir tmp("protocol_transactions");
  const std::string dir = tmp.file("doc");
  Host host;
  host.make_world(dir);
  const i64 before = journal_length(host);
  Session* s = host.sessions.find(host.session);
  const u64 t = begin_on(host, "world", who("a"));
  staged(host, t, one(set(oid(1), "color", JsonValue(4))), who("a"));
  s->fail_transactions_write_for_test();
  CHECK(host.fails("doc.commit", obj({{"transaction", JsonValue(t)}, {"attribution", who("a")}})) ==
        codes::k_io_error);
  CHECK(journal_length(host) == before + 1);
  CHECK(s->landed_patch(t) == before);
  JsonValue r = ended(host, "doc.commit", t, who("a"));
  CHECK(at(r, "committed") == JsonValue(true));
  CHECK(num(at(r, "patch_index")) == before);
  CHECK(at(r, "diagnostics").size() == 1);
  CHECK(journal_length(host) == before + 1);
  CHECK(host.property(oid(1), "color") == JsonValue(4));

  const u64 u = begin_on(host, "world", who("a"));
  staged(host, u, one(set(oid(2), "color", JsonValue(5))), who("a"));
  s->fail_transactions_write_for_test();
  CHECK(host.fails("doc.commit", obj({{"transaction", JsonValue(u)}, {"attribution", who("a")}})) ==
        codes::k_io_error);
  std::string table;
  REQUIRE(io::read_file(dir + "/transactions.json", table) == io::Status::Ok);
  CHECK(table.find(std::to_string(u)) != std::string::npos);  // still listed on disk
  Host next;
  next.open(dir, false);
  CHECK(at(next.ok("doc.transactions", obj({})), "transactions").size() == 0);
  r = ended(next, "doc.commit", u, who("a"));
  CHECK(at(r, "committed") == JsonValue(true));
  CHECK(num(at(r, "patch_index")) == before + 1);
  CHECK(journal_length(next) == before + 2);
  CHECK(next.property(oid(2), "color") == JsonValue(5));
  // One patch, so one undo takes it back.
  next.ok("doc.undo", obj({}));
  CHECK(next.property(oid(2), "color").is_null());
  CHECK(next.property(oid(1), "color") == JsonValue(4));
  // Undone, it is not answered committed: its work is not in the document.
  CHECK(next.fails("doc.commit", obj({{"transaction", JsonValue(u)}, {"attribution", who("a")}})) ==
        codes::k_not_found);
  CHECK(next.property(oid(2), "color").is_null());
}

// A commit whose save failed after the journal took its patch: the files keep the undo position
// they were saved at, so the next host reads the patch as the redo tail's first, not as work in the
// document. Its table entry stays, the retry commits the staged commands afresh (cutting that
// tail, so nothing is applied twice), and another actor's commit after it keeps them. Before the
// mend the next host's first read of the table dropped the entry, the retry answered "committed"
// for work the document did not hold, and the other actor's commit cut it off for good.
TEST_CASE(
    "transactions: a commit whose save failed after the journal lands once in the next host") {
  TempDir tmp("protocol_transactions");
  const std::string dir = tmp.file("doc");
  Host host;
  host.make_world(dir);
  const i64 before = journal_length(host);
  Session* s = host.sessions.find(host.session);
  const u64 t = begin_on(host, "world", who("a"));
  staged(host, t, one(set(oid(1), "color", JsonValue(4))), who("a"));
  s->fail_save_for_test();
  CHECK(host.fails("doc.commit", obj({{"transaction", JsonValue(t)}, {"attribution", who("a")}})) ==
        codes::k_io_error);
  // This host's memory holds the patch, but nothing saved it: the table still lists it.
  CHECK(at(host.ok("doc.transactions", obj({})), "transactions").size() == 1);

  Host next;
  next.open(dir, false);
  CHECK(next.property(oid(1), "color").is_null());
  CHECK(at(next.ok("doc.transactions", obj({})), "transactions").size() == 1);
  const JsonValue r = ended(next, "doc.commit", t, who("a"));
  CHECK(at(r, "committed") == JsonValue(true));
  CHECK(num(at(r, "patch_index")) == before);
  CHECK(journal_length(next) == before + 1);
  CHECK(next.property(oid(1), "color") == JsonValue(4));
  CHECK(at(next.ok("doc.transactions", obj({})), "transactions").size() == 0);

  Host third;
  third.open(dir, false);
  CHECK(at(third.apply(one(set(oid(2), "color", JsonValue(5))), who("b"), "world"), "committed") ==
        JsonValue(true));
  CHECK(third.property(oid(1), "color") == JsonValue(4));
  CHECK(third.property(oid(2), "color") == JsonValue(5));
  Host last;
  last.open(dir, false);
  CHECK(last.property(oid(1), "color") == JsonValue(4));
  CHECK(journal_length(last) == before + 2);
}

// The bounds: a transaction holds at most `k_max_staged_commands`, and an actor opens at most
// `k_max_open_transactions` at once; past either, 1004 and nothing changes.
TEST_CASE("transactions: bounded in commands and in open transactions per actor") {
  TempDir tmp("protocol_transactions");
  Host host;
  host.make_world(tmp.file("doc"));
  const u64 t = begin_on(host, "world", who("a"));
  JsonValue many = JsonValue::array();
  for (u32 i = 0; i <= Session::k_max_staged_commands; ++i)
    many.push_back(set(oid(1), "color", JsonValue(i)));
  CHECK(staged_fails(host, t, many, who("a")) == codes::k_invalid_argument);
  CHECK(num(at(at(host.ok("doc.transactions", obj({})), "transactions")[0], "commands")) == 0);
  for (u32 i = 1; i < Session::k_max_open_transactions; ++i)
    (void)begin_on(host, "world", who("a"));
  CHECK(host.fails("doc.begin", obj({{"layer", JsonValue("world")}, {"attribution", who("a")}})) ==
        codes::k_invalid_argument);
  // Another actor's count is its own.
  (void)begin_on(host, "world", who("b"));
}

TEST_CASE("side files: the lock excludes a second holder, and a version-1 table still reads") {
  TempDir tmp("protocol_side_file");
  const std::string path = tmp.file("table.json");
  SideFileLock first;
  SideFileLock second;
  std::string message;
  REQUIRE(first.acquire(path, 0, message));
  CHECK(first.held());
  const i64 start = time::monotonic_ns();
  CHECK_FALSE(second.acquire(path, 50, message));
  CHECK(time::monotonic_ns() - start >= 50'000'000);
  CHECK(message.find("another host process") != std::string::npos);
  first.release();
  CHECK(second.acquire(path, 0, message));
  second.release();

  // Ids: never zero, under 2^53, and never one the caller calls taken.
  u32 refusals = 0;
  const u64 id = fresh_side_file_id(
      [](const void* context, u64) {
        u32& n = *static_cast<u32*>(const_cast<void*>(context));
        return n++ < 3;
      },
      &refusals);
  CHECK(refusals == 4);
  CHECK(id > 0);
  CHECK(id < (u64{1} << 53));

  // A table written before ids were drawn names its counter; it reads through the version step.
  io::Vfs vfs;
  {
    std::ofstream f(tmp.file("leases.json"), std::ios::binary);
    f << R"({"required":true,"next_id":3,"leases":[{"id":2,"actor":"a","layer":"world"}]})";
  }
  Session warmup(vfs, "s", tmp.path());  // registers the version steps
  LeaseFile leases;
  bool present = false;
  RpcError error;
  REQUIRE_MESSAGE(read_side_file(vfs, tmp.file("leases.json"), leases, present, error),
                  error.message);
  CHECK(present);
  CHECK(leases.required);
  REQUIRE(leases.leases.size() == 1);
  CHECK(leases.leases[0].id == 2);
}

TEST_CASE("transactions: roles, proposals and leases check every staging call and the commit") {
  TempDir tmp("protocol_transactions");
  Host host;
  host.make_world(tmp.file("doc"));
  std::string message;
  REQUIRE(host.policy.load_roles("roles.json", k_roles, message));
  JsonValue e;
  // The dispatcher's gate: doc.begin is a write, so QA (the default role) and a role whose
  // methods do not name it are refused; doc.transactions is a read.
  CHECK(host.fails("doc.begin", obj({{"layer", JsonValue("world")}, {"attribution", who("q")}}),
                   &e) == codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("method"));
  CHECK(host.fails("doc.begin", obj({{"layer", JsonValue("world")},
                                     {"attribution", who("m", "mapper")}})) == codes::k_forbidden);
  host.ok("doc.transactions", obj({}));
  // Fail fast: a role that needs review may write only its own proposals, so a transaction on
  // the world is refused when it is opened, not when it is committed.
  CHECK(host.fails("doc.begin",
                   obj({{"layer", JsonValue("world")}, {"attribution", who("w", "writer")}}),
                   &e) == codes::k_forbidden);

  const JsonValue d = who("d", "director", "recolour in a proposal");
  host.ok("doc.propose_layer",
          obj({{"name", JsonValue("p.d")}, {"target", JsonValue("world")}, {"attribution", d}}));
  // Someone else's proposal is refused at doc.begin.
  CHECK(host.fails("doc.begin",
                   obj({{"layer", JsonValue("p.d")}, {"attribution", who("w", "writer")}}),
                   &e) == codes::k_forbidden);
  CHECK(at(at(e, "data"), "reason") == JsonValue("proposal owner"));
  const u64 p = begin_on(host, "p.d", d);
  staged(host, p, one(set(oid(1), "color", JsonValue(3))), d);
  CHECK(at(ended(host, "doc.commit", p, d), "committed") == JsonValue(true));
  // The commit wrote the proposal as a doc.apply does: its first touch is its merge base.
  const JsonValue promoted =
      host.ok("doc.promote", obj({{"proposal", JsonValue("p.d")}, {"attribution", d}}));
  CHECK(at(promoted, "promoted") == JsonValue(true));
  CHECK(host.layer_property("world", oid(1), "color") == JsonValue(3));

  // Leases: required, so a staging call with none is refused at once, and the commit asks again.
  host.ok("lease.require", obj({{"required", JsonValue(true)}, {"attribution", d}}));
  const u64 l = begin_on(host, "world", d);
  CHECK(staged_fails(host, l, one(set(oid(1), "color", JsonValue(4))), d) ==
        codes::k_lease_required);
  const JsonValue lease = host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                                        {"tiles", ranges({range(0, 0, 0, 0)})},
                                                        {"attribution", d}}));
  staged(host, l, one(set(oid(1), "color", JsonValue(4))), d);
  host.ok("lease.release", obj({{"lease", at(lease, "id")}, {"attribution", d}}));
  const i64 before = journal_length(host);
  CHECK(host.fails("doc.commit", obj({{"transaction", JsonValue(l)}, {"attribution", d}})) ==
        codes::k_lease_required);
  CHECK(journal_length(host) == before);
  CHECK(at(host.ok("doc.transactions", obj({})), "transactions").size() == 1);  // still open
  host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                {"tiles", ranges({range(0, 0, 0, 0)})},
                                {"attribution", d}}));
  CHECK(at(ended(host, "doc.commit", l, d), "committed") == JsonValue(true));
  CHECK(host.layer_property("world", oid(1), "color") == JsonValue(4));
}

// ---- place_asset and instantiate_template (plan 06 §6.9; protocol.md, "Placing an asset,
// instantiating a template") --------------------------------------------------------------------

namespace {

void write_bytes(const std::string& path, std::string_view bytes) {
  std::ofstream f(path, std::ios::binary);
  f << bytes;
}

JsonValue node(Id128 id, f64 x, f64 z, Id128 parent = {}) {
  JsonValue c = obj({{"kind", JsonValue("CreateObject")},
                     {"id", JsonValue(hex(id))},
                     {"type", JsonValue("engine.world.Node")},
                     {"value", obj({{"name", JsonValue("n")}, {"position", position(x, z)}})}});
  if (!parent.is_null()) c.set("parent", JsonValue(hex(parent)));
  return c;
}

JsonValue placement(f64 x, f64 z, const char* name) {
  return obj({{"name", JsonValue(name)}, {"translation", position(x, z)}});
}

f64 coord(const JsonValue& point, u32 i) {
  f64 v = 0;
  REQUIRE(point[i].get_f64(v));
  return v;
}

}  // namespace

TEST_CASE("place_asset: a checked mesh placed as MeshPlacement records, ids from the seed") {
  TempDir tmp("protocol_place");
  const std::string dir = tmp.file("doc");
  Host host;
  host.make_world(dir);
  write_bytes(dir + "/crate.glb", std::string("glTF\x02\0\0\0", 8));
  write_bytes(dir + "/tree.gltf", R"({"asset":{"version":"2.0"},"meshes":[]})");
  write_bytes(dir + "/fake.glb", "not a glb");
  write_bytes(dir + "/notes.txt", "x");
  const JsonValue a = who("a", "", "two crates by the gate");
  JsonValue two = JsonValue::array();
  two.push_back(placement(1, 2, "crate a"));
  two.push_back(placement(3, 4, "crate b"));
  const auto params = [&](const char* asset, JsonValue list, u64 seed = 7) {
    return obj({{"asset", JsonValue(asset)},
                {"placements", std::move(list)},
                {"seed", JsonValue(seed)},
                {"attribution", a}});
  };
  const i64 before = journal_length(host);
  const JsonValue r = host.ok("doc.place_asset", params("crate.glb", two));
  CHECK(at(r, "kind") == JsonValue("glb"));
  CHECK(num(at(r, "bytes")) == 8);
  CHECK(at(at(r, "apply"), "committed") == JsonValue(true));
  REQUIRE(at(r, "ids").size() == 2);
  CHECK(journal_length(host) == before + 1);  // one transaction
  const JsonValue view = host.ok("doc.get", obj({{"id", at(r, "ids")[1]}}));
  CHECK(at(view, "type") == JsonValue("engine.scene.MeshPlacement"));
  CHECK(at(at(view, "properties"), "mesh") == JsonValue("crate.glb"));
  CHECK(at(at(view, "properties"), "name") == JsonValue("crate b"));
  CHECK(coord(at(at(view, "properties"), "translation"), 2) == 4.0);
  CHECK(at(host.ok("doc.validate", obj({})), "ok") == JsonValue(true));

  // The same call in another document makes the same ids; another seed, others.
  Host other;
  other.make_world(tmp.file("other"));
  write_bytes(tmp.file("other") + "/crate.glb", std::string("glTF\x02\0\0\0", 8));
  CHECK(at(other.ok("doc.place_asset", params("crate.glb", two)), "ids") == at(r, "ids"));
  CHECK(at(other.ok("doc.place_asset", params("crate.glb", two, 8)), "ids")[0] != at(r, "ids")[0]);

  // A .gltf reads as JSON naming its asset version; staged into a transaction it is not placed yet.
  const u64 t = begin_on(host, "", a);
  JsonValue staged_params = params("tree.gltf", one(placement(9, 9, "tree")));
  staged_params.set("transaction", JsonValue(t));
  const JsonValue s = host.ok("doc.place_asset", staged_params);
  CHECK(at(s, "kind") == JsonValue("gltf"));
  CHECK(num(at(at(s, "apply"), "staged")) == 1);
  CHECK(host.fails("doc.get", obj({{"id", at(s, "ids")[0]}})) == codes::k_not_found);
  ended(host, "doc.commit", t, a);
  host.ok("doc.get", obj({{"id", at(s, "ids")[0]}}));

  // Refused: a file that is not a mesh kind, one that is not there, one that does not read as its
  // kind, and nothing to place.
  CHECK(host.fails("doc.place_asset", params("notes.txt", two)) == codes::k_invalid_argument);
  CHECK(host.fails("doc.place_asset", params("missing.glb", two)) == codes::k_not_found);
  CHECK(host.fails("doc.place_asset", params("fake.glb", two)) == codes::k_invalid_argument);
  CHECK(host.fails("doc.place_asset", params("crate.glb", JsonValue::array())) ==
        codes::k_invalid_argument);
  // The one check (paths.h, admit_document_file): the roots are asked as well as the lexical rule,
  // so a host whose roots do not hold the document reads none of its assets; and a link inside the
  // document that leads out of it is refused, which the lexical rule alone let through.
  {
    TempDir elsewhere("protocol_place_elsewhere");
    io::Roots narrow;
    REQUIRE(narrow.add("elsewhere", elsewhere.path(), false) == io::Status::Ok);
    host.dispatcher.context().roots = &narrow;
    CHECK(host.fails("doc.place_asset", params("crate.glb", two)) == codes::k_not_found);
    host.dispatcher.context().roots = &host.roots;
    std::error_code link_error;
    std::filesystem::create_directories(tmp.file("outside"), link_error);
    write_bytes(tmp.file("outside") + "/crate.glb", std::string("glTF\x02\0\0\0", 8));
    std::filesystem::create_directory_symlink(tmp.file("outside"), dir + "/linked", link_error);
    if (!link_error)
      CHECK(host.fails("doc.place_asset", params("linked/crate.glb", two)) == codes::k_not_found);
    else
      MESSAGE("no symbolic link here (" << link_error.message() << "), so that form is not tried");
  }
  // A role that may not write is refused at the gate, as for doc.apply.
  std::string message;
  REQUIRE(host.policy.load_roles("roles.json", k_roles, message));
  JsonValue e;
  CHECK(host.fails("doc.place_asset",
                   obj({{"asset", JsonValue("crate.glb")},
                        {"placements", two},
                        {"attribution", who("q", "qa")}}),
                   &e) == codes::k_forbidden);
  CHECK(at(at(e, "data"), "method") == JsonValue("doc.place_asset"));
}

TEST_CASE("place_asset: reads only under the document's directory, and says nothing of the rest") {
  TempDir tmp("protocol_place_root");
  const std::string dir = tmp.file("doc");
  Host host;
  host.make_world(dir);
  // A real .glb beside the document and one inside it, under a directory of its own.
  const std::string glb("glTF\x02\0\0\0", 8);
  write_bytes(tmp.file("outside.glb"), glb);
  std::filesystem::create_directories(dir + "/props");
  write_bytes(dir + "/props/crate.glb", glb);
  const JsonValue a = who("a", "", "a crate");
  const auto params = [&](const std::string& asset) {
    return obj({{"asset", JsonValue(asset)},
                {"placements", one(placement(1, 2, "crate"))},
                {"attribution", a}});
  };
  host.ok("doc.place_asset", params("props/crate.glb"));
  host.ok("doc.place_asset", params("props\\crate.glb"));
  // The file outside exists and reads as a .glb, and every way of naming it is refused with the
  // sentence a missing file gets: absolute, climbing out, a drive, a stream, an empty name.
  JsonValue missing;
  REQUIRE(host.fails("doc.place_asset", params("props/missing.glb"), &missing) ==
          codes::k_not_found);
  const std::string climbing[] = {
      tmp.file("outside.glb"), "../outside.glb", "props/../../outside.glb", "..\\outside.glb",
      "/outside.glb", "\\\\host\\share\\outside.glb", "C:outside.glb", "props/crate.glb:x.glb",
      "props//crate.glb",
      // Either separator on every platform: the mixed forms are refused on Linux as on Windows,
      // never read as one file name holding a backslash (protocol.md, "Blobs and checkpoints"). A
      // name that is only a backslash is an empty component between two separators. (A whole asset
      // of `\` names no .glb, and is refused as one before any path is read.)
      "props/..\\props\\crate.glb", "..\\outside.glb", "props\\..\\..\\outside.glb",
      "props/\\/crate.glb"};
  const i64 before = journal_length(host);
  for (const std::string& asset : climbing) {
    JsonValue e;
    CHECK_MESSAGE(host.fails("doc.place_asset", params(asset), &e) == codes::k_not_found, asset);
    CHECK_MESSAGE(at(e, "message") == at(missing, "message"), asset);
  }
  CHECK(journal_length(host) == before);
}

TEST_CASE("instantiate_template: a subtree copied under fresh ids, offset, with its origin") {
  TempDir tmp("protocol_template");
  Host host;
  host.make_world(tmp.file("doc"));
  const JsonValue a = who("a", "", "a second yard");
  // The template: a yard node with two children, one with a child of its own.
  JsonValue yard = JsonValue::array();
  yard.push_back(node(oid(20), 5, 5));
  yard.push_back(node(oid(21), 6, 5, oid(20)));
  yard.push_back(node(oid(22), 7, 5, oid(20)));
  yard.push_back(node(oid(23), 7, 6, oid(22)));
  host.apply(yard, a, "world");
  const auto params = [&](Id128 root, u32 max_records = 10000) {
    JsonValue offset = JsonValue::array();
    offset.push_back(JsonValue(100.0));
    offset.push_back(JsonValue(0.0));
    offset.push_back(JsonValue(-10.0));
    return obj({{"root", JsonValue(hex(root))},
                {"offset", offset},
                {"seed", JsonValue(u64{3})},
                {"max_records", JsonValue(max_records)},
                {"layer", JsonValue("world")},
                {"attribution", a}});
  };
  const i64 before = journal_length(host);
  const JsonValue r = host.ok("doc.instantiate_template", params(oid(20)));
  CHECK(journal_length(host) == before + 1);
  const JsonValue& ids = at(r, "ids");
  REQUIRE(ids.size() == 4);
  // Depth first, children by id: the copy's order is the template's.
  const JsonValue root = host.ok("doc.get", obj({{"id", ids[0]}}));
  CHECK(at(root, "parent") == JsonValue(hex(Id128{})));
  CHECK(coord(at(at(root, "properties"), "position"), 0) == 105.0);
  CHECK(coord(at(at(root, "properties"), "position"), 2) == -5.0);
  const JsonValue deepest = host.ok("doc.get", obj({{"id", ids[3]}}));
  CHECK(at(deepest, "parent") == ids[2]);
  CHECK(coord(at(at(deepest, "properties"), "position"), 0) == 107.0);
  // The template is untouched, and the copy validates.
  CHECK(coord(host.property(oid(23), "position"), 0) == 7.0);
  CHECK(at(host.ok("doc.validate", obj({})), "ok") == JsonValue(true));

  // The origin: a child of the copy's root, standing where it stands.
  const JsonValue kids =
      host.ok("doc.objects",
              obj({{"parent", ids[0]}, {"type", JsonValue("engine.protocol.TemplateOrigin")}}));
  REQUIRE(at(kids, "objects").size() == 1);
  const JsonValue& origin = at(at(kids, "objects")[0], "properties");
  CHECK(at(origin, "source_root") == JsonValue(hex(oid(20))));
  CHECK(at(origin, "sources").size() == 4);
  CHECK(at(origin, "copies") == ids);
  CHECK(coord(at(origin, "position"), 0) == 105.0);

  // A copy of the copy leaves its origin behind and records its own.
  Id128 copy;
  REQUIRE(Id128::from_hex(ids[0].as_string(), copy));
  const JsonValue again = host.ok("doc.instantiate_template", params(copy));
  CHECK(at(again, "ids").size() == 4);
  // One undo takes a copy back whole.
  host.ok("doc.undo", obj({}));
  CHECK(host.fails("doc.get", obj({{"id", at(again, "ids")[0]}})) == codes::k_not_found);

  // Refused: an unknown template, one over max_records.
  CHECK(host.fails("doc.instantiate_template", params(oid(99))) == codes::k_not_found);
  JsonValue e;
  CHECK(host.fails("doc.instantiate_template", params(oid(20), 3), &e) ==
        codes::k_invalid_argument);
  CHECK(std::string(at(e, "message").as_string()).find("max_records") != std::string::npos);
  // Staged into a transaction, nothing is copied until the commit.
  const u64 t = begin_on(host, "world", a);
  JsonValue staged_params = params(oid(22));
  staged_params.set("transaction", JsonValue(t));
  staged_params.set("seed", JsonValue(u64{4}));
  const JsonValue s = host.ok("doc.instantiate_template", staged_params);
  CHECK(num(at(at(s, "apply"), "staged")) == 3);  // two nodes and the origin
  CHECK(at(ended(host, "doc.commit", t, a), "committed") == JsonValue(true));
  host.ok("doc.get", obj({{"id", at(s, "ids")[1]}}));
}

// ---- describe (plan 06 §6.6; protocol.md, "Describing an object, a tile or a region") ---------

namespace {

// The world's four props, and in tile (0, 0) a yard: a node with two child nodes, the first with a
// child of its own.
void make_yard(Host& host) {
  JsonValue yard = JsonValue::array();
  yard.push_back(node(oid(20), 5, 5));
  yard.push_back(node(oid(21), 6, 6, oid(20)));
  yard.push_back(node(oid(22), 7, 7, oid(20)));
  yard.push_back(node(oid(23), 8, 8, oid(21)));
  host.apply(yard, who("setup"));
}

JsonValue describe(Host& host, JsonValue params) { return host.ok("doc.describe", params); }

Vector<std::string> listed_ids(const JsonValue& r) {
  Vector<std::string> ids;
  const JsonValue& records = at(r, "records");
  for (u32 i = 0; i < records.size(); ++i)
    ids.push_back(std::string(at(records[i], "id").as_string()));
  return ids;
}

}  // namespace

TEST_CASE("describe: an object at three details, its sentence, and the bounds") {
  TempDir tmp("protocol_describe_object");
  Host host;
  host.make_world(tmp.file("doc"));
  make_yard(host);
  const auto params = [](const char* detail, u32 max_records = 256, u32 max_bytes = 65536) {
    return obj({{"id", JsonValue(hex(oid(20)))},
                {"detail", JsonValue(detail)},
                {"max_records", JsonValue(max_records)},
                {"max_bytes", JsonValue(max_bytes)}});
  };
  // Summary: the object alone, its children counted; the sentence names it, where it is, and what
  // it holds, the same words from a second call.
  const JsonValue s = describe(host, params("Summary"));
  CHECK(at(s, "scope") == JsonValue("object"));
  CHECK(num(at(s, "total")) == 3);
  CHECK(num(at(s, "omitted")) == 2);
  CHECK(listed_ids(s) == Vector<std::string>{hex(oid(20))});
  const std::string sentence(at(s, "summary").as_string());
  CHECK(sentence.find("engine.world.Node 'n' " + hex(oid(20)) + " at (5.00, 0.00, 5.00)") == 0);
  CHECK(sentence.find(", a root, with 2 live records as children: 2 engine.world.Node.") !=
        std::string::npos);
  CHECK(at(describe(host, params("Summary")), "summary") == at(s, "summary"));
  const JsonValue& self = at(s, "records")[0];
  CHECK(num(at(self, "children")) == 2);
  CHECK(coord(at(self, "position"), 2) == 5.0);
  CHECK(at(self, "properties").size() == 0);

  // Standard: the object and its children by id, no properties; Full: the subtree depth first,
  // with properties.
  const JsonValue st = describe(host, params("Standard"));
  CHECK(listed_ids(st) == Vector<std::string>{hex(oid(20)), hex(oid(21)), hex(oid(22))});
  CHECK(num(at(at(st, "records")[1], "depth")) == 1);
  CHECK(at(at(st, "records")[1], "parent") == JsonValue(hex(oid(20))));
  CHECK(at(at(st, "records")[2], "properties").size() == 0);
  const JsonValue full = describe(host, params("Full"));
  CHECK(num(at(full, "total")) == 4);
  CHECK(listed_ids(full) ==
        Vector<std::string>{hex(oid(20)), hex(oid(21)), hex(oid(23)), hex(oid(22))});
  CHECK(num(at(at(full, "records")[2], "depth")) == 2);
  CHECK(at(at(at(full, "records")[2], "properties"), "name") == JsonValue("n"));
  CHECK(std::string(at(full, "summary").as_string()).find("3 live records in its subtree") !=
        std::string::npos);
  CHECK(at(full, "bound") == JsonValue(""));

  // Bounds: the rest is counted and the sentence says by which bound; the object itself is always
  // listed, however small the bound.
  const JsonValue two = describe(host, params("Full", 2));
  CHECK(listed_ids(two).size() == 2);
  CHECK(num(at(two, "omitted")) == 2);
  CHECK(at(two, "bound") == JsonValue("max_records"));
  CHECK(std::string(at(two, "summary").as_string())
            .find("Listed 2 of 4; 2 left out by max_records.") != std::string::npos);
  const JsonValue small = describe(host, params("Full", 256, 1024));
  CHECK(at(small, "bound") == JsonValue("max_bytes"));
  CHECK(num(at(small, "bytes")) <= 1024);
  CHECK(listed_ids(small).size() + num(at(small, "omitted")) == 4);
  CHECK(listed_ids(describe(host, params("Full", 1))) == Vector<std::string>{hex(oid(20))});

  // Refused: an unknown or deleted object, no target or two, a bound out of range.
  CHECK(host.fails("doc.describe", obj({{"id", JsonValue(hex(oid(99)))}})) == codes::k_not_found);
  host.apply(one(del(oid(22))), who("setup"));
  CHECK(host.fails("doc.describe", obj({{"id", JsonValue(hex(oid(22)))}})) == codes::k_not_found);
  CHECK(listed_ids(describe(host, params("Standard"))).size() == 2);
  CHECK(host.fails("doc.describe", obj({})) == codes::k_invalid_argument);
  CHECK(host.fails("doc.describe",
                   obj({{"id", JsonValue(hex(oid(20)))},
                        {"tile", obj({{"x", JsonValue(0)}, {"y", JsonValue(0)}})}})) ==
        codes::k_invalid_argument);
  CHECK(host.fails("doc.describe", params("Full", 0)) == codes::k_invalid_argument);
  CHECK(host.fails("doc.describe", params("Full", 10001)) == codes::k_invalid_argument);
  CHECK(host.fails("doc.describe", params("Full", 256, 1023)) == codes::k_invalid_argument);
}

TEST_CASE("describe: a tile and a region from the tile index, in one order, under any role") {
  TempDir tmp("protocol_describe_tiles");
  Host host;
  host.make_world(tmp.file("doc"));
  make_yard(host);
  const auto at_tile = [](i32 x, i32 y) {
    return obj({{"tile", obj({{"x", JsonValue(x)}, {"y", JsonValue(y)}})}});
  };
  const auto region = [](i32 x0, i32 y0, i32 x1, i32 y1, const char* detail = "Standard") {
    return obj({{"region", range(x0, y0, x1, y1)}, {"detail", JsonValue(detail)}});
  };
  // Tile (0, 0): prop 1 and the yard's four nodes, by id.
  const JsonValue t = describe(host, at_tile(0, 0));
  CHECK(at(t, "scope") == JsonValue("tile"));
  CHECK(at(t, "summary") == JsonValue("Tile (0, 0) holds 5 live records: 4 engine.world.Node and 1 "
                                      "engine.protocol.test.Prop."));
  CHECK(listed_ids(t) ==
        Vector<std::string>{hex(oid(1)), hex(oid(20)), hex(oid(21)), hex(oid(22)), hex(oid(23))});
  CHECK(num(at(t, "occupied_tiles")) == 1);
  CHECK(at(at(t, "records")[0], "tiled") == JsonValue(true));
  CHECK(at(at(t, "types")[0], "type") == JsonValue("engine.world.Node"));
  CHECK(at(describe(host, at_tile(9, 9)), "summary") ==
        JsonValue("Tile (9, 9) holds no live records."));

  // A region walked tile by tile (4 tiles, 4 occupied) and one answered from the live set (9
  // tiles): the same records in the same order, row by row.
  const JsonValue by_tiles = describe(host, region(0, 0, 1, 1));
  const JsonValue by_objects = describe(host, region(-1, -1, 1, 1));
  CHECK(num(at(by_tiles, "total")) == 7);
  CHECK(listed_ids(by_tiles) == listed_ids(by_objects));
  CHECK(listed_ids(by_tiles) == Vector<std::string>{hex(oid(1)), hex(oid(20)), hex(oid(21)),
                                                    hex(oid(22)), hex(oid(23)), hex(oid(2)),
                                                    hex(oid(3))});
  CHECK(std::string(at(by_objects, "summary").as_string())
            .find("Tiles (-1, -1) to (1, 1), 3 of 9 occupied, hold 7 live records") == 0);
  CHECK(listed_ids(describe(host, region(0, 0, 1, 1, "Summary"))).size() == 0);

  // Refused: a region too large or upside down.
  CHECK(host.fails("doc.describe", region(0, 0, 256, 255)) == codes::k_invalid_argument);
  CHECK(host.ok("doc.describe", region(0, 0, 255, 255)).find("total") != nullptr);
  CHECK(host.fails("doc.describe", region(1, 0, 0, 0)) == codes::k_invalid_argument);

  // A read: a role that may write nothing describes, and the catalogue says so.
  std::string message;
  REQUIRE(host.policy.load_roles("roles.json", k_roles, message));
  host.policy.identity.role = "qa";
  CHECK(host.apply_fails(one(set(oid(1), "color", JsonValue(4))), who("q")) == codes::k_forbidden);
  CHECK(num(at(describe(host, at_tile(0, 0)), "total")) == 5);
  const JsonValue methods = at(host.ok("engine.methods", JsonValue()), "methods");
  bool found = false;
  for (u32 i = 0; i < methods.size(); ++i)
    if (at(methods[i], "name") == JsonValue("doc.describe")) {
      found = true;
      CHECK(at(methods[i], "read_only") == JsonValue(true));
    }
  CHECK(found);
}

// ---- query (plan 06 §6.6; protocol.md, "Querying the document") ------------------------------

namespace {

JsonValue query(Host& host, JsonValue filter,
                std::initializer_list<std::pair<const char*, JsonValue>> extra = {}) {
  JsonValue params = obj({{"filter", std::move(filter)}});
  for (auto& [k, v] : extra)
    params.set(k, v);
  return host.ok("doc.query", params);
}

JsonValue term(const char* property, const char* test, JsonValue value) {
  return obj({{"property", JsonValue(property)}, {test, std::move(value)}});
}

JsonValue terms(const char* op, std::initializer_list<JsonValue> list) {
  JsonValue a = JsonValue::array();
  for (const JsonValue& t : list)
    a.push_back(t);
  return obj({{op, std::move(a)}});
}

}  // namespace

TEST_CASE("query: a structured filter over the document, in id order, a page at a time") {
  TempDir tmp("protocol_query");
  Host host;
  host.make_world(tmp.file("doc"));
  make_yard(host);
  host.apply(one(set(oid(2), "color", JsonValue(7))), who("setup"));
  // Every live record, then by type pattern, by a property test, and composed.
  CHECK(at(query(host, JsonValue()), "rows").size() == 8);
  CHECK(listed_ids(JsonValue(
            obj({{"records", at(query(host, obj({{"type", JsonValue("*.Node")}})), "rows")}}))) ==
        Vector<std::string>{hex(oid(20)), hex(oid(21)), hex(oid(22)), hex(oid(23))});
  const JsonValue east = query(host, term("position[0]", "at_least", JsonValue(10)),
                               {{"fields", strings({"position", "color"})}});
  REQUIRE(at(east, "rows").size() == 2);  // props 2 (x 15) and 4 (x 55)
  CHECK(at(at(east, "rows")[0], "id") == JsonValue(hex(oid(2))));
  CHECK(at(at(at(east, "rows")[0], "fields"), "color") == JsonValue(7));
  CHECK(at(at(east, "rows")[1], "fields").find("color") == nullptr);
  CHECK(at(east, "complete") == JsonValue(true));
  const JsonValue mixed = query(
      host,
      terms("any", {term("color", "equals", JsonValue(7.0)),
                    terms("all", {obj({{"type", JsonValue("engine.world.Node")}}),
                                  obj({{"not", obj({{"parent", JsonValue(hex(oid(20)))}})}})})}));
  // Prop 2 (its colour, 7, compared by value) and the yard's two nodes not directly under it.
  CHECK(listed_ids(JsonValue(obj({{"records", at(mixed, "rows")}}))) ==
        Vector<std::string>{hex(oid(2)), hex(oid(20)), hex(oid(23))});
  CHECK(at(query(host, term("name", "prefix", JsonValue("p"))), "rows").size() == 4);
  CHECK(at(query(host, term("color", "exists", JsonValue(false))), "rows").size() == 7);

  // Narrowed through the indices: a parent's children, a region's tiles.
  CHECK(at(query(host, JsonValue(), {{"parent", JsonValue(hex(oid(20)))}}), "rows").size() == 2);
  CHECK(at(query(host, JsonValue(), {{"region", range(0, 0, 0, 1)}}), "candidates") ==
        JsonValue(u32{6}));
  CHECK(at(query(host, JsonValue(), {{"region", range(-50, -50, 50, 50)}}), "rows").size() == 8);

  // Pages: three rows at a time, resumed after `next`, the same rows as one call, none twice.
  Vector<std::string> paged;
  JsonValue after = JsonValue(hex(Id128{}));
  for (u32 page = 0; page < 4; ++page) {
    const JsonValue r =
        query(host, JsonValue(), {{"max_rows", JsonValue(u32{3})}, {"after", after}});
    for (u32 i = 0; i < at(r, "rows").size(); ++i)
      paged.push_back(std::string(at(at(r, "rows")[i], "id").as_string()));
    if (at(r, "complete") == JsonValue(true)) break;
    CHECK(at(r, "bound") == JsonValue("max_rows"));
    after = at(r, "next");
  }
  CHECK(paged == listed_ids(JsonValue(obj({{"records", at(query(host, JsonValue()), "rows")}}))));
  // The examination bound stops early and says where to go on from.
  const JsonValue few =
      query(host, obj({{"type", JsonValue("*.Node")}}), {{"max_examined", JsonValue(u32{2})}});
  CHECK(at(few, "rows").size() == 0);  // props 1 and 2 examined, neither a node
  CHECK(at(few, "bound") == JsonValue("max_examined"));
  CHECK(at(few, "next") == JsonValue(hex(oid(2))));
  CHECK(at(query(host, JsonValue(),
                 {{"max_bytes", JsonValue(u32{1024})}, {"fields", strings({"name", "position"})}}),
           "bound") == JsonValue("max_bytes"));

  // Refused, naming the path: malformed terms, too deep, too many, bounds out of range.
  JsonValue e;
  CHECK(host.fails("doc.query", obj({{"filter", obj({{"colour", JsonValue(1)}})}}), &e) ==
        codes::k_invalid_argument);
  CHECK(host.fails("doc.query",
                   obj({{"filter", terms("all", {term("x", "between", JsonValue(1))})}}),
                   &e) == codes::k_invalid_argument);
  CHECK(std::string(at(e, "message").as_string()).find("filter.all[0]") == 0);
  CHECK(host.fails("doc.query", obj({{"filter", term("x", "at_least", JsonValue("a"))}})) ==
        codes::k_invalid_argument);
  CHECK(host.fails("doc.query", obj({{"filter", term("x[]", "exists", JsonValue(true))}})) ==
        codes::k_invalid_argument);
  JsonValue deep = obj({{"type", JsonValue("*")}});
  for (u32 i = 0; i < 40; ++i)
    deep = obj({{"not", deep}});
  CHECK(host.fails("doc.query", obj({{"filter", deep}})) == codes::k_invalid_argument);
  JsonValue wide = JsonValue::array();
  for (u32 i = 0; i < 1100; ++i)
    wide.push_back(obj({{"type", JsonValue("*")}}));
  CHECK(host.fails("doc.query", obj({{"filter", obj({{"any", wide}})}})) ==
        codes::k_invalid_argument);
  CHECK(host.fails("doc.query", obj({{"max_rows", JsonValue(u32{0})}})) ==
        codes::k_invalid_argument);
  CHECK(host.fails("doc.query", obj({{"max_ms", JsonValue(u32{10001})}})) ==
        codes::k_invalid_argument);
  CHECK(host.fails("doc.query", obj({{"parent", JsonValue(hex(oid(20)))},
                                     {"region", range(0, 0, 0, 0)}})) == codes::k_invalid_argument);
  // A read: QA queries.
  std::string message;
  REQUIRE(host.policy.load_roles("roles.json", k_roles, message));
  host.policy.identity.role = "qa";
  CHECK(at(query(host, JsonValue()), "rows").size() == 8);
}

// `proposals.json` goes through the side files' one helper (protocol.md, "Two hosts, one side
// file"): each change reads the table afresh under its lock and replaces only its own proposal.
// Before, a host wrote its whole table from memory, so the second host to open a proposal wrote a
// table without the first host's.
TEST_CASE("side files: two hosts that each open a proposal keep both in proposals.json") {
  test::TempDir tmp("protocol_proposals_two_hosts");
  const std::string dir = tmp.file("doc");
  Host a;
  a.open(dir, true);
  Host b;
  b.open(dir, false);  // read the table while it was empty
  (void)a.ok("doc.propose_layer", obj({{"name", JsonValue("roads")},
                                       {"target", JsonValue("base")},
                                       {"edit", JsonValue(false)},
                                       {"attribution", who("a")}}));
  (void)b.ok("doc.propose_layer", obj({{"name", JsonValue("dunes")},
                                       {"target", JsonValue("base")},
                                       {"edit", JsonValue(false)},
                                       {"attribution", who("b")}}));
  ProposalsFile file;
  bool present = false;
  RpcError error;
  REQUIRE_MESSAGE(read_side_file(a.vfs, dir + "/proposals.json", file, present, error),
                  error.message);
  REQUIRE(present);
  std::vector<std::string> layers;
  for (const ProposalInfo& p : file.proposals) {
    layers.push_back(p.layer);
    CHECK(p.state == ProposalState::Open);
  }
  CHECK(layers.size() == 2);
  CHECK(std::find(layers.begin(), layers.end(), "roads") != layers.end());
  CHECK(std::find(layers.begin(), layers.end(), "dunes") != layers.end());
  CHECK(a.vfs.exists(dir + "/proposals.json.lock"));
}
// A host holds a mark on a document while its socket connections may hold leases there (the lock
// of `leases.<host>`, session.cpp): a second host's open drops a connection's lease as a gone
// host's only when nobody holds that host's mark. Before the mark, `host` alone could not tell the
// two apart, so the open, a read any credential may make, ended a live host's clients' leases.
TEST_CASE("side files: a second host's open keeps a live host's connection leases") {
  test::TempDir tmp("protocol_live_host_leases");
  const std::string dir = tmp.file("doc");
  {
    Host first;
    first.open(dir, true);
  }
  const i64 now = time::wall_unix_ms();
  const auto lease = [&](u64 id, const char* host) {
    return std::string(R"({"id":)") + std::to_string(id) + R"(,"connection":3,"host":")" + host +
           R"(","actor":"c","layer":"base","acquired_unix_ms":)" + std::to_string(now) +
           R"(,"expires_unix_ms":)" + std::to_string(now + 3'600'000) + "}";
  };
  {
    std::ofstream out(dir + "/leases.json", std::ios::binary);
    out << R"({"leases":[)" << lease(77, "feedfacefeedface") << "," << lease(78, "0000000000000001")
        << "]}";
  }
  // The live host's mark, held as that host holds it; the other host left none.
  SideFileLock mark;
  std::string message;
  REQUIRE_MESSAGE(mark.acquire(dir + "/leases.feedfacefeedface", 10'000, message), message);
  const auto listed = [&](Host& h) {
    std::vector<i64> ids;
    const JsonValue result = h.ok("lease.list");
    const JsonValue& leases = at(result, "leases");
    for (u32 i = 0; i < leases.size(); ++i) {
      i64 id = 0;
      REQUIRE(at(leases[i], "id").get_i64(id));
      ids.push_back(id);
    }
    return ids;
  };
  Host second;
  second.open(dir, false);
  CHECK(listed(second) == std::vector<i64>{77});
  // Once the host is gone its mark is free: the next open drops its lease and the mark with it.
  mark.release();
  Host third;
  third.open(dir, false);
  CHECK(listed(third).empty());
  CHECK_FALSE(third.vfs.exists(dir + "/leases.feedfacefeedface.lock"));
}
