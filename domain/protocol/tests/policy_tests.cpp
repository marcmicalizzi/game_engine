// Plan 06 §6.5 over the dispatcher (docs/subsystems/protocol.md, "Roles, leases and proposals"):
// roles as configuration, leases refused at acquisition and checked at the edit, and proposal
// layers promoted by a structural merge that validates first.
#include <core/json/json.h>
#include <core/time/time.h>
#include <domain/protocol/leases.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/proposals.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <fstream>
#include <iterator>
#include <string>

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
  Dispatcher dispatcher{Context{&sessions, nullptr, nullptr, nullptr, &policy}};
  std::string session;

  Host() { add_builtin_methods(dispatcher); }

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
  for (const char* name : {"director", "designer", "environment", "qa", "performance"})
    CHECK_MESSAGE(shipped.find_role(name) != nullptr, name);
  CHECK(shipped.roles().default_role == "qa");
  CHECK(shipped.find_role("qa")->methods.empty());
  CHECK(shipped.find_role("designer")->review_required);
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
  CHECK(at(la, "id") == JsonValue(u64{1}));
  // b overlapping a: refused, naming a's lease. a overlapping itself: fine.
  JsonValue e;
  CHECK(host.fails("lease.acquire",
                   obj({{"layer", JsonValue("world")},
                        {"tiles", ranges({range(1, 1, 2, 2)})},
                        {"attribution", b}}),
                   &e) == codes::k_lease_conflict);
  CHECK(at(at(at(e, "data"), "lease"), "actor") == JsonValue("a"));
  host.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                                {"tiles", ranges({range(1, 1, 1, 1)})},
                                {"attribution", a}}));
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
  CHECK(host.fails("lease.release", obj({{"lease", JsonValue(u64{1})}, {"attribution", b}})) ==
        codes::k_forbidden);
  CHECK(host.fails("lease.renew", obj({{"lease", JsonValue(u64{1})}, {"attribution", b}})) ==
        codes::k_forbidden);
  JsonValue renewed = host.ok(
      "lease.renew",
      obj({{"lease", JsonValue(u64{1})}, {"ttl_seconds", JsonValue(3600.0)}, {"attribution", a}}));
  CHECK(num(at(renewed, "expires_unix_ms")) > num(at(la, "expires_unix_ms")));
  host.ok("lease.release", obj({{"lease", JsonValue(u64{1})}, {"attribution", a}}));
  host.ok("lease.release", obj({{"lease", JsonValue(u64{2})}, {"attribution", a}}));
  CHECK(host.fails("lease.release", obj({{"lease", JsonValue(u64{1})}, {"attribution", a}})) ==
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
  Session* s = sessions.open(open, error);
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
  Session* s2 = sessions2.open(open, error);
  REQUIRE(s2 != nullptr);
  CHECK(s2->leases().required());
  REQUIRE(s2->leases().leases().size() == 1);
  CHECK(s2->leases().leases()[0].id == held.id);
  CHECK(s2->leases().file().next_id == held.id + 1);
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
