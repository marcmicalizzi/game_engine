// Blobs and checkpoints over the dispatcher (roadmap D13, D14; docs/subsystems/protocol.md, "Blobs
// and checkpoints"): a blob put and read back checked, a command naming a blob the store lacks
// refused, a collection that keeps what the journal can bring back; checkpoints taken, listed and
// rolled back across a tile-partitioned layer to the files' bytes, as journaled patches the change
// feed reports and undo takes back, a blob the store had collected restored from the checkpoint,
// refused whole by a role or another's lease, a proposal left alone, and 1006 without git; and a
// seeded run of edits, undos, checkpoints and rollbacks in which every rollback leaves each accepted
// layer equal to what that checkpoint held.
#include <core/hash/base64.h>
#include <core/json/json.h>
#include <domain/doc/blob_store.h>
#include <domain/doc/checkpoint.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>

#include <doctest/doctest.h>
#include <test_environment.h>
#include <test_temp_dir.h>

#include <random>
#include <string>

using namespace engine;
using namespace engine::protocol;
using TempDir = test::TempDir;

namespace {

const char* k_prop = "engine.protocol.test.Prop";

JsonValue obj(std::initializer_list<std::pair<const char*, JsonValue>> fields) {
  JsonValue o = JsonValue::object();
  for (auto& [k, v] : fields)
    o.set(k, v);
  return o;
}

std::string hex(Id128 id) {
  char buf[Id128::k_hex_length + 1];
  id.to_hex(buf);
  return buf;
}

Id128 oid(u64 n) { return Id128::from_parts(0x20, n); }

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

JsonValue create_prop(Id128 id, f64 x, f64 z) {
  return obj({{"kind", JsonValue("CreateObject")},
              {"id", JsonValue(hex(id))},
              {"type", JsonValue(k_prop)},
              {"value", obj({{"name", JsonValue("p")}, {"position", position(x, z)}})}});
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

bool have_git() {
  std::string version, error;
  if (doc::Checkpoints::available(&version, &error)) return true;
  MESSAGE("no git on this machine, so the checkpoint cases do not run: " << error);
  return false;
}

struct Host {
  io::Vfs vfs;
  SessionManager sessions{vfs};
  Policy policy;
  Dispatcher dispatcher{Context{&sessions, nullptr, nullptr, nullptr, &policy}};
  std::string session;
  std::string dir;

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
  i32 fails(std::string_view method, JsonValue params, JsonValue* out = nullptr) {
    JsonValue resp = call(method, std::move(params));
    const JsonValue* e = resp.find("error");
    REQUIRE_MESSAGE(e != nullptr, method << " succeeded: " << write_json(resp));
    if (out != nullptr) *out = *e;
    return static_cast<i32>(e->find("code")->as_int());
  }
  JsonValue apply(JsonValue commands, JsonValue attribution = who("director"),
                  const char* layer = "") {
    return ok("doc.apply", obj({{"commands", std::move(commands)},
                                {"attribution", std::move(attribution)},
                                {"layer", JsonValue(layer)}}));
  }
  // A world: `base`, and `world` tiled 10 m a side holding four props in four tiles.
  void make_world(const std::string& path) {
    dir = path;
    JsonValue info = ok("session.open", obj({{"path", JsonValue(path)}, {"create", JsonValue(true)}}));
    session = std::string(info.find("session")->as_string());
    ok("doc.add_layer",
       obj({{"name", JsonValue("world")}, {"partition", obj({{"tile_size", JsonValue(10.0)}})}}));
    JsonValue commands = JsonValue::array();
    commands.push_back(create_prop(oid(1), 5, 5));
    commands.push_back(create_prop(oid(2), 15, 5));
    commands.push_back(create_prop(oid(3), 5, 15));
    commands.push_back(create_prop(oid(4), 55, 55));
    CHECK(*apply(commands).find("committed") == JsonValue(true));
  }
  Session& s() { return *sessions.find(session); }
  const doc::Document& document() { return s().document(); }
  JsonValue put(std::string_view bytes) {
    return ok("doc.put_blob", obj({{"data_base64", JsonValue(base64_encode(bytes))},
                                   {"media", JsonValue("test")}}));
  }
  // Every file of a layer's directory and its bytes, in name order.
  Vector<std::pair<std::string, std::string>> files_of(const std::string& sub) {
    Vector<std::pair<std::string, std::string>> out;
    collect(dir + "/" + sub, sub, out);
    return out;
  }
  void collect(const std::string& path, const std::string& rel,
               Vector<std::pair<std::string, std::string>>& out) {
    Vector<io::DirEntry> entries;
    if (vfs.list(path, entries) != io::Status::Ok) return;
    for (const io::DirEntry& e : entries) {
      if (e.is_directory) {
        collect(path + "/" + e.name, rel + "/" + e.name, out);
      } else {
        std::string bytes;
        REQUIRE(vfs.read(path + "/" + e.name, bytes) == io::Status::Ok);
        out.push_back({rel + "/" + e.name, std::move(bytes)});
      }
    }
  }
};

const char* k_roles = R"({
  "default_role": "qa",
  "roles": [
    {"name": "director", "methods": ["*"], "layers": ["*"], "object_types": ["*"], "untiled": true},
    {"name": "mapper", "methods": ["doc.apply", "lease.*", "doc.put_blob"], "layers": ["world"],
     "tiles": [{"x0": 0, "y0": 0, "x1": 1, "y1": 1}], "untiled": false,
     "object_types": ["engine.protocol.test.Prop"]},
    {"name": "writer", "methods": ["doc.*"], "layers": ["*"], "object_types": ["*"],
     "review_required": true},
    {"name": "qa", "methods": []}
  ]
})";

}  // namespace

TEST_CASE("blobs: put, read back checked, referenced, refused when unheld, collected") {
  TempDir tmp("engine_protocol_blobs");
  Host h;
  h.make_world(tmp.path() + "/doc");

  const std::string grid(70000, 'g');
  const JsonValue a = h.ok("doc.put_blob", obj({{"data_base64", JsonValue(base64_encode(grid))},
                                                {"media", JsonValue("engine.terrain.grid/1")}}));
  CHECK(*a.find("created") == JsonValue(true));
  const JsonValue& ref = *a.find("ref");
  const std::string name(ref.find("blob")->as_string());
  CHECK(name == doc::BlobStore::name_of(grid));
  CHECK(ref.find("size")->as_uint() == grid.size());
  CHECK(ref.find("media")->as_string() == "engine.terrain.grid/1");
  CHECK(*h.put(grid).find("created") == JsonValue(false));

  // From a file on the host's machine.
  const std::string file = tmp.path() + "/mask.bin";
  REQUIRE(io::write_file(file, "mask bytes") == io::Status::Ok);
  const JsonValue m = h.ok("doc.put_blob", obj({{"path", JsonValue(file)}}));
  CHECK(m.find("ref")->find("blob")->as_string() == doc::BlobStore::name_of("mask bytes"));
  CHECK(h.fails("doc.put_blob", obj({{"path", JsonValue(file)},
                                     {"data_base64", JsonValue("AA==")}})) ==
        codes::k_invalid_argument);
  CHECK(h.fails("doc.put_blob", JsonValue::object()) == codes::k_invalid_argument);

  const JsonValue back = h.ok("doc.blob", obj({{"blob", JsonValue(name)}, {"data", JsonValue(true)}}));
  std::string bytes;
  REQUIRE(base64_decode(back.find("data_base64")->as_string(), bytes));
  CHECK(bytes == grid);
  CHECK(h.fails("doc.blob", obj({{"blob", JsonValue("xyz")}})) == codes::k_invalid_argument);
  CHECK(h.fails("doc.blob", obj({{"blob", JsonValue(std::string(64, '0'))}})) ==
        codes::k_not_found);

  // A property holds the reference; a command naming a blob the store lacks is refused, atomically.
  CHECK(*h.apply(one(set(oid(1), "grid", ref))).find("committed") == JsonValue(true));
  JsonValue absent = ref;
  absent.set("blob", JsonValue(doc::BlobStore::name_of("never put")));
  JsonValue two = JsonValue::array();
  two.push_back(set(oid(2), "color", JsonValue(7)));
  two.push_back(set(oid(2), "grid", absent));
  const JsonValue refused = h.apply(two);
  CHECK(*refused.find("committed") == JsonValue(false));
  CHECK(write_json(*refused.find("diagnostics")).find("does not hold") != std::string::npos);

  // The catalogue: the two readers are read-only, the rest are writes.
  const JsonValue methods = h.ok("engine.methods");
  u32 seen = 0;
  for (const JsonValue& mi : methods.find("methods")->as_array()) {
    const std::string_view n = mi.find("name")->as_string();
    const bool ro = *mi.find("read_only") == JsonValue(true);
    if (n == "doc.blob" || n == "doc.checkpoints") {
      CHECK(ro);
      ++seen;
    }
    if (n == "doc.put_blob" || n == "doc.collect_blobs" || n == "doc.checkpoint" ||
        n == "doc.rollback_to") {
      CHECK_FALSE(ro);
      ++seen;
    }
  }
  CHECK(seen == 6);

  // Collection: the referenced grid stays, the file's mask (named by nothing) goes, and an undo
  // brings back nothing that was collected.
  const JsonValue kept = h.ok("doc.collect_blobs", obj({{"min_age_s", JsonValue(0.0)}}));
  CHECK(kept.find("removed")->size() == 1);
  CHECK(kept.find("removed")->as_array()[0] == *m.find("ref")->find("blob"));
  CHECK(kept.find("missing")->size() == 0);
  h.ok("doc.undo", obj({{"steps", JsonValue(1)}}));
  CHECK(h.ok("doc.collect_blobs", obj({{"min_age_s", JsonValue(0.0)}})).find("removed")->size() ==
        0);  // the undone set is the redo tail's: it can come back
  CHECK(doc::BlobStore::has(h.vfs, h.dir, name));
  // A new commit drops the redo tail; then nothing reaches the grid and it goes.
  h.apply(one(set(oid(3), "color", JsonValue(3))));
  const JsonValue gone = h.ok("doc.collect_blobs", obj({{"min_age_s", JsonValue(0.0)}}));
  CHECK(gone.find("removed")->size() == 1);
  CHECK_FALSE(doc::BlobStore::has(h.vfs, h.dir, name));
}

TEST_CASE("checkpoints: taken, rolled back across a tiled layer, undone, and refused whole") {
  if (!have_git()) return;
  TempDir tmp("engine_protocol_checkpoints");
  Host h;
  h.make_world(tmp.path() + "/doc");
  CHECK(h.ok("doc.checkpoints").find("checkpoints")->size() == 0);

  const JsonValue a = h.put("grid a");
  const JsonValue ref_a = *a.find("ref");
  h.apply(one(set(oid(1), "grid", ref_a)));
  const JsonValue c1 = h.ok("doc.checkpoint", obj({{"label", JsonValue("four props")},
                                                   {"attribution", who("ann", "director")}}));
  const std::string id1(c1.find("id")->as_string());
  CHECK(id1.size() == 40);
  const auto world_then = h.files_of("layers/world");
  REQUIRE(world_then.size() >= 4);  // index, three or more tiles

  // Move one, delete one, repaint, add a layer with a record: then roll back.
  const u64 before = h.document().revision();
  h.apply(one(set(oid(1), "position", position(95, 95))));
  h.apply(one(del(oid(2))));
  const JsonValue b = h.put("grid b");
  h.apply(one(set(oid(3), "grid", *b.find("ref"))));
  h.ok("doc.add_layer", obj({{"name", JsonValue("extra")}}));
  h.apply(one(create_prop(oid(9), 1, 1)), who("director"), "extra");
  const u32 journal_before = h.document().journal().size();

  const JsonValue dry = h.ok("doc.rollback_to", obj({{"checkpoint", JsonValue(id1.substr(0, 8))},
                                                     {"dry_run", JsonValue(true)}}));
  CHECK(*dry.find("committed") == JsonValue(false));
  CHECK(h.document().journal().size() == journal_before);
  CHECK(h.files_of("layers/world") != world_then);

  const u64 changed_at = h.document().revision();
  const JsonValue rolled = h.ok("doc.rollback_to", obj({{"checkpoint", JsonValue(id1)},
                                                        {"attribution", who("ann", "director")}}));
  CHECK(*rolled.find("committed") == JsonValue(true));
  CHECK(rolled.find("patches")->as_uint() == 2);  // world restored, extra emptied
  CHECK(h.document().journal().size() == journal_before + 2);
  // The tiled layer's files are the checkpoint's, byte for byte.
  CHECK(h.files_of("layers/world") == world_then);
  CHECK(h.document().exists(oid(2)));
  CHECK_FALSE(h.document().exists(oid(9)));
  CHECK(h.document().find_layer("extra") >= 0);  // emptied, not removed
  // The change feed names what the rollback changed, so a materialized world follows.
  Vector<doc::ObjectId> changed;
  REQUIRE(h.document().changed_since(changed_at, changed));
  bool saw1 = false, saw2 = false, saw9 = false, saw4 = false;
  for (const doc::ObjectId& id : changed) {
    saw1 = saw1 || id == oid(1);
    saw2 = saw2 || id == oid(2);
    saw9 = saw9 || id == oid(9);
    saw4 = saw4 || id == oid(4);
  }
  CHECK(saw1);
  CHECK(saw2);
  CHECK(saw9);
  CHECK_FALSE(saw4);  // untouched since the checkpoint
  CHECK(before < changed_at);
  // The journal says who and why.
  const doc::Patch& last = h.document().journal().back();
  CHECK(last.attribution.actor == "ann");
  CHECK(last.attribution.rationale.find("rollback to checkpoint") != std::string::npos);

  // Undo takes it back; redo brings it again.
  h.ok("doc.undo", obj({{"steps", JsonValue(2)}}));
  CHECK_FALSE(h.document().exists(oid(2)));
  CHECK(h.document().exists(oid(9)));
  h.ok("doc.redo", obj({{"steps", JsonValue(2)}}));
  CHECK(h.files_of("layers/world") == world_then);

  CHECK(h.fails("doc.rollback_to", obj({{"checkpoint", JsonValue("0123456789abcdef")}})) ==
        codes::k_not_found);
  CHECK(h.fails("doc.rollback_to", obj({{"checkpoint", JsonValue("not hex")}})) ==
        codes::k_not_found);

  // A blob the store collected since comes back from the checkpoint. Paint a, checkpoint, undo the
  // paint and drop it from the journal: nothing reaches a, and a collection takes it.
  const JsonValue c = h.put("grid c");
  const std::string name_c(c.find("ref")->find("blob")->as_string());
  h.apply(one(set(oid(4), "grid", *c.find("ref"))));
  const JsonValue c2 = h.ok("doc.checkpoint", obj({{"label", JsonValue("painted")}}));
  h.ok("doc.undo", obj({{"steps", JsonValue(1)}}));
  h.apply(one(set(oid(4), "color", JsonValue(1))));
  h.ok("doc.collect_blobs", obj({{"min_age_s", JsonValue(0.0)}}));
  REQUIRE_FALSE(doc::BlobStore::has(h.vfs, h.dir, name_c));
  const JsonValue back = h.ok("doc.rollback_to", obj({{"checkpoint", *c2.find("id")}}));
  CHECK(back.find("blobs_restored")->as_uint() == 1);
  CHECK(h.ok("doc.blob", obj({{"blob", JsonValue(name_c)}})).find("ref")->find("size")->as_uint() ==
        6);

  // Listed newest first.
  const JsonValue listed = h.ok("doc.checkpoints");
  REQUIRE(listed.find("checkpoints")->size() == 2);
  CHECK(listed.find("checkpoints")->as_array()[1].find("id")->as_string() == id1);
  CHECK(listed.find("checkpoints")->as_array()[1].find("label")->as_string() == "four props");
}

TEST_CASE("checkpoints: roles, leases and proposals") {
  if (!have_git()) return;
  TempDir tmp("engine_protocol_checkpoint_roles");
  Host h;
  h.make_world(tmp.path() + "/doc");  // before the roles: their default, QA, may add no layer
  std::string error;
  REQUIRE(h.policy.load_roles("roles.json", k_roles, error));
  const JsonValue c1 = h.ok("doc.checkpoint", obj({{"attribution", who("dee", "director")}}));
  const std::string id(c1.find("id")->as_string());
  h.apply(one(set(oid(1), "color", JsonValue(5))), who("dee", "director"));

  JsonValue e;
  // QA may read the list, and may neither checkpoint nor roll back.
  CHECK(h.ok("doc.checkpoints", JsonValue::object()).find("available") != nullptr);
  CHECK(h.fails("doc.checkpoint", obj({{"attribution", who("q", "qa")}}), &e) == codes::k_forbidden);
  CHECK(h.fails("doc.rollback_to",
                obj({{"checkpoint", JsonValue(id)}, {"attribution", who("q", "qa")}}), &e) ==
        codes::k_forbidden);
  // A role whose work needs review writes only its own proposals: a rollback is refused whole.
  CHECK(h.fails("doc.rollback_to",
                obj({{"checkpoint", JsonValue(id)}, {"attribution", who("w", "writer")}}), &e) ==
        codes::k_forbidden);

  // Another actor's live lease on a tile the rollback would change: 1009, and nothing changed.
  h.ok("lease.require", obj({{"required", JsonValue(true)}, {"attribution", who("dee", "director")}}));
  JsonValue range = obj({{"x0", JsonValue(0)}, {"y0", JsonValue(0)}, {"x1", JsonValue(0)},
                         {"y1", JsonValue(0)}});
  JsonValue ranges = JsonValue::array();
  ranges.push_back(range);
  h.ok("lease.acquire", obj({{"layer", JsonValue("world")},
                             {"tiles", ranges},
                             {"ttl_seconds", JsonValue(600.0)},
                             {"attribution", who("mo", "mapper")}}));
  const u32 journal = h.document().journal().size();
  CHECK(h.fails("doc.rollback_to",
                obj({{"checkpoint", JsonValue(id)}, {"attribution", who("dee", "director")}}), &e) ==
        codes::k_lease_conflict);
  CHECK(h.document().journal().size() == journal);
  h.ok("lease.require", obj({{"required", JsonValue(false)}, {"attribution", who("dee", "director")}}));

  // An open proposal is its owner's work in progress: a rollback leaves it as it is.
  h.ok("doc.propose_layer", obj({{"name", JsonValue("p.w")},
                                 {"target", JsonValue("world")},
                                 {"attribution", who("w", "writer")}}));
  h.apply(one(set(oid(3), "color", JsonValue(9))), who("w", "writer"), "p.w");
  const JsonValue rolled = h.ok("doc.rollback_to", obj({{"checkpoint", JsonValue(id)},
                                                        {"attribution", who("dee", "director")}}));
  bool kept = false;
  for (const JsonValue& l : rolled.find("layers")->as_array())
    if (l.find("layer")->as_string() == "p.w") kept = l.find("action")->as_string() == "kept";
  CHECK(kept);
  const i32 p = h.document().find_layer("p.w");
  REQUIRE(p >= 0);
  CHECK(h.document().layer(static_cast<u32>(p)).find(oid(3)) != nullptr);
  CHECK(h.document().property(oid(1), "color") == nullptr);  // the director's change rolled back
}

TEST_CASE("checkpoints: without git the methods say so") {
  TempDir tmp("engine_protocol_no_git");
  Host h;
  h.make_world(tmp.path() + "/doc");
  test::ScopedEnv no_git("ENGINE_GIT", tmp.path() + "/no-such-git.exe");
  CHECK(h.fails("doc.checkpoint", obj({})) == codes::k_unavailable);
  const JsonValue listed = h.ok("doc.checkpoints");
  CHECK(*listed.find("available") == JsonValue(false));
  CHECK(listed.find("git")->as_string().find("ENGINE_GIT") != std::string::npos);
  CHECK(h.fails("doc.rollback_to", obj({{"checkpoint", JsonValue("abcd1234")}})) ==
        codes::k_unavailable);
}

TEST_CASE("checkpoints: seeded edits, undos, checkpoints and rollbacks restore what was held") {
  if (!have_git()) return;
  TempDir tmp("engine_protocol_rollback_property");
  Host h;
  h.make_world(tmp.path() + "/doc");
  h.ok("doc.add_layer", obj({{"name", JsonValue("notes")}, {"edit", JsonValue(false)}}));

  struct Taken {
    std::string id;
    Vector<doc::Layer> layers;  // base, world, notes as the checkpoint held them
  };
  Vector<Taken> taken;
  std::mt19937 rng(0xC4EC4u);
  u32 rollbacks = 0;
  const auto snapshot = [&]() {
    Vector<doc::Layer> layers;
    for (u32 i = 0; i < h.document().layer_count(); ++i)
      layers.push_back(h.document().layer(i));
    return layers;
  };
  for (u32 step = 0; step < 70; ++step) {
    const u32 op = rng() % 100;
    const Id128 target = oid(1 + rng() % 6);
    const char* layer = rng() % 3 == 0 ? "notes" : "world";
    if (op < 45) {
      JsonValue c;
      switch (rng() % 4) {
        case 0: c = create_prop(target, static_cast<f64>(rng() % 60), static_cast<f64>(rng() % 60)); break;
        case 1: c = set(target, "position", position(rng() % 80, rng() % 80)); break;
        case 2: c = set(target, "color", JsonValue(static_cast<u32>(rng() % 9))); break;
        default: c = del(target); break;
      }
      h.ok("doc.apply", obj({{"commands", one(std::move(c))},
                             {"attribution", who("r")},
                             {"layer", JsonValue(layer)}}));
    } else if (op < 60) {
      h.ok("doc.undo", obj({{"steps", JsonValue(1 + rng() % 2)}}));
    } else if (op < 68) {
      h.ok("doc.redo", obj({{"steps", JsonValue(1)}}));
    } else if (op < 82) {
      const JsonValue c = h.ok("doc.checkpoint", obj({{"label", JsonValue("s" + std::to_string(step))}}));
      taken.push_back(Taken{std::string(c.find("id")->as_string()), snapshot()});
    } else if (!taken.empty()) {
      const Taken& t = taken[rng() % taken.size()];
      h.ok("doc.rollback_to", obj({{"checkpoint", JsonValue(t.id)}}));
      ++rollbacks;
      REQUIRE(h.document().layer_count() == t.layers.size());
      for (u32 i = 0; i < t.layers.size(); ++i)
        CHECK(h.document().layer(i) == t.layers[i]);
      Vector<doc::Diagnostic> problems;
      CHECK(h.document().validate_index(&problems));
    }
  }
  MESSAGE(taken.size() << " checkpoints, " << rollbacks << " rollbacks");
  CHECK(rollbacks >= 3);
}
