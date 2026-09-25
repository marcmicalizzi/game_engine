// Save and load (systems/world/save_game.h, state_hash.h, input_observer.h; docs/subsystems/
// world.md, "Save and load"): the persistent-state hash is a function of the persistent state and
// of nothing about how it was written, a player's input state is a fold of its log however the log
// is fed, and a save is a set of files a manifest names and hashes — read back as it was written,
// written again byte for byte from what it restored, and refused, naming the file or the format,
// when it is not what it says. The whole loop through a running world — run, save, load, run on —
// is engine-host's, and `apps/engine_cli/tests/save_tests.cpp` drives it end to end.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>
#include <foundation/store/event_log.h>
#include <systems/world/input_observer.h>
#include <systems/world/save_game.h>
#include <systems/world/state_hash.h>
#include <systems/world/tile_store.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

const char* const k_node = "engine.world.Node";

Id128 id_of(u64 n) { return Id128::from_parts(0x5A7E, n); }

JsonValue vec3(f64 x, f64 y, f64 z) {
  JsonValue v = JsonValue::array();
  v.push_back(JsonValue(x));
  v.push_back(JsonValue(y));
  v.push_back(JsonValue(z));
  return v;
}

void node(doc::Document& d, u64 n, f64 x, f64 z) {
  JsonValue props = JsonValue::object();
  props.set("name", JsonValue("n" + std::to_string(n)));
  props.set("position", vec3(x, 0.0, z));
  Vector<doc::Diagnostic> diagnostics;
  const bool ok =
      d.apply(doc::cmd_create(id_of(n), k_node, Id128{}, std::move(props)), nullptr, &diagnostics);
  REQUIRE_MESSAGE(ok, (diagnostics.empty() ? std::string() : diagnostics[0].message));
}

std::string file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

StateHash hash_of(u64 tick, const doc::Document& d, store::EventLog* log) {
  StateHash h;
  REQUIRE(state_hash(tick, static_cast<i64>(tick) * 16667, d, log, h) == store::Status::Ok);
  return h;
}

// The action map a player's log is recorded against: WASD on one Axis2.
input::ActionMap wasd() {
  input::ActionMap map;
  const input::ActionId move = map.add_action("move", input::ActionKind::Axis2);
  map.bind(move, input::Binding{input::Source::Key, 7, 1.0f, 0.0f}, 0);    // D: +x
  map.bind(move, input::Binding{input::Source::Key, 4, -1.0f, 0.0f}, 0);   // A: -x
  map.bind(move, input::Binding{input::Source::Key, 26, 1.0f, 0.0f}, 1);   // W: forward
  map.bind(move, input::Binding{input::Source::Key, 22, -1.0f, 0.0f}, 1);  // S: back
  map.add_action("jump", input::ActionKind::Button);
  return map;
}

input::InputLog walk(const input::ActionMap& map) {
  input::InputLog log;
  log.set_map(map);
  auto key = [&](u64 tick, u32 code, f32 value) {
    input::RawEvent e;
    e.tick = SimTick{tick};
    e.source = input::Source::Key;
    e.code = code;
    e.value = value;
    log.record(e);
  };
  key(3, 26, 1.0f);   // W down
  key(10, 7, 1.0f);   // D down: diagonal
  key(20, 26, 0.0f);  // W up
  key(31, 7, 0.0f);   // D up
  key(31, 4, 1.0f);   // A down
  key(44, 4, 0.0f);
  return log;
}

// A document directory as a session's store writes one, with records in two tiles.
void make_document(const std::string& dir, doc::Document& d) {
  io::Vfs vfs;
  doc::DocumentManifest manifest;
  std::string error;
  REQUIRE_MESSAGE(doc::DocumentStore::create(vfs, dir, "Saved", d, manifest, &error), error);
  node(d, 1, 5.0, 5.0);
  node(d, 2, 40.0, 5.0);
  node(d, 3, -20.0, 70.0);
  REQUIRE_MESSAGE(doc::DocumentStore::save(vfs, dir, d, manifest, &error), error);
}

SaveManifest manifest_at(u64 tick, const StateHash& hash) {
  SaveManifest m;
  m.tick = tick;
  m.game_time_us = static_cast<i64>(tick) * 16667;
  m.world_seed = 1;
  m.state_hash = hash_hex(hash.value);
  SaveRing ring;
  ring.rings.push_back(1.5f);
  ring.rings.push_back(3.0f);
  ring.simulated = 1;
  SaveObserver o;
  o.position = Vec3{0.1f, 1.7f, -2.3f};
  o.velocity = Vec3{0.3f, 0.0f, 0.0f};
  ring.observers.push_back(o);
  SaveTile t;
  t.x = -1;
  t.z = 0;
  ring.tiles.push_back(t);
  t.x = 0;
  ring.tiles.push_back(t);
  SavePlayer player;
  player.observer = 0;
  player.action = "move";
  player.speed = 4.0f;
  ring.player = player;
  m.ring = ring;
  return m;
}

}  // namespace

TEST_CASE("world save: the state hash is the persistent state's, however it was written") {
  // The same records created in two orders, one of them in a transaction the journal keeps with a
  // wall-clock time in it: one document hash.
  doc::Document a;
  a.add_layer("base", doc::LayerRole::Base);
  node(a, 1, 1.0, 2.0);
  node(a, 2, 3.0, 4.0);
  doc::Document b;
  b.add_layer("base", doc::LayerRole::Base);
  {
    doc::Attribution who;
    who.actor = "someone";
    who.rationale = "a journal is history, not state";
    doc::Transaction tx = b.begin(who);
    JsonValue props = JsonValue::object();
    props.set("name", JsonValue("n2"));
    props.set("position", vec3(3.0, 0.0, 4.0));
    REQUIRE(tx.apply(doc::cmd_create(id_of(2), k_node, Id128{}, std::move(props))));
    REQUIRE(tx.commit());
  }
  node(b, 1, 1.0, 2.0);
  REQUIRE(b.journal().size() == 1);
  CHECK(hash_of(7, a, nullptr).document == hash_of(7, b, nullptr).document);
  CHECK(hash_of(7, a, nullptr).value == hash_of(7, b, nullptr).value);
  CHECK(hash_of(7, a, nullptr).records == 2);

  // A property the world wrote back is state: another position is another hash. So is the clock.
  REQUIRE(b.apply(doc::cmd_set(id_of(1), "position", vec3(1.0, 0.0, 2.5)), nullptr, nullptr));
  CHECK(hash_of(7, a, nullptr).document != hash_of(7, b, nullptr).document);
  CHECK(hash_of(7, a, nullptr).value != hash_of(8, a, nullptr).value);
  CHECK(hash_of(7, a, nullptr).document == hash_of(8, a, nullptr).document);

  // Two stores holding the same rows, written in two orders through the world's own writer.
  WorldStore first;
  WorldStore second;
  REQUIRE(first.open_memory() == store::Status::Ok);
  REQUIRE(second.open_memory() == store::Status::Ok);
  Vector<TileRow> rows;
  for (u64 n = 1; n <= 3; ++n) {
    TileRow row;
    row.record = id_of(n);
    row.type = k_node;
    row.position = Vec3{static_cast<f32>(n), 0.0f, 0.0f};
    rows.push_back(row);
  }
  REQUIRE(first.write_tile(TileCoord{0, 0}, {rows.data(), 2}, 5, 5) == store::Status::Ok);
  REQUIRE(first.write_tile(TileCoord{-3, 1}, {rows.data() + 2, 1}, 6, 6) == store::Status::Ok);
  REQUIRE(second.write_tile(TileCoord{-3, 1}, {rows.data() + 2, 1}, 6, 6) == store::Status::Ok);
  REQUIRE(second.write_tile(TileCoord{0, 0}, {rows.data() + 1, 1}, 5, 5) == store::Status::Ok);
  // The first row of the tile goes in last: an upsert, then the snapshot taken again.
  REQUIRE(second.write_tile(TileCoord{0, 0}, {rows.data(), 2}, 5, 5) == store::Status::Ok);
  const StateHash h1 = hash_of(9, a, &first.log());
  const StateHash h2 = hash_of(9, a, &second.log());
  CHECK(h1.store);
  CHECK(h1.projection_count == 3);
  CHECK(h1.snapshot_count == 2);
  CHECK(h1.projections == h2.projections);
  CHECK(h1.snapshots == h2.snapshots);
  CHECK(h1.value == h2.value);
  // An event is state.
  store::EventRecord event;
  event.tile = 7;
  event.type = 1;
  REQUIRE(second.log().append(event) == store::Status::Ok);
  const StateHash h3 = hash_of(9, a, &second.log());
  CHECK(h3.event_count == 1);
  CHECK(h3.events != h1.events);
  CHECK(h3.value != h1.value);
  // No store is not an empty store's hash under another name: it says so.
  CHECK_FALSE(hash_of(9, a, nullptr).store);

  // The written form.
  u64 back = 0;
  CHECK(hash_hex(0x0123456789abcdefull) == "0123456789abcdef");
  CHECK(parse_hash_hex("0123456789ABCDEF", back));
  CHECK(back == 0x0123456789abcdefull);
  CHECK_FALSE(parse_hash_hex("0123", back));
  CHECK_FALSE(parse_hash_hex("0123456789abcdeg", back));
}

TEST_CASE("world save: a ring restored from the tiles it held goes on as the ring never stopped") {
  // A walk that turns and speeds up under a budget small enough to defer changes, so the ring at
  // the cut is history — tiles held by hysteresis, changes the budget has not reached yet.
  RingParams params;
  params.ring_count = 3;
  params.radius[0] = 1.5f;
  params.radius[1] = 4.0f;
  params.radius[2] = 7.0f;
  params.max_activations = 3;
  params.max_deactivations = 2;
  auto observers_at = [](u32 step) {
    sim::ObserverSet set;
    const f32 t = static_cast<f32>(step);
    set.add(Vec3{t * 9.0f, 0.0f, step < 40 ? t * 2.0f : 80.0f - (t - 40.0f) * 6.0f}, 1.0f);
    if (step >= 20 && step < 70) set.add(Vec3{-100.0f + t, 0.0f, 30.0f}, 1.5f);
    return set;
  };
  World through(params);
  u32 deferred = 0;
  for (u32 step = 0; step < 50; ++step) {
    const UpdateStats& s = through.update(observers_at(step), step);
    deferred += s.deferred_promotions + s.deferred_demotions;
  }
  // The cut: what a save keeps.
  Vector<TileCoord> tiles;
  Vector<u8> rings;
  for (u32 i = 0; i < through.ring().active_count(); ++i) {
    tiles.push_back(tile_of_key(through.ring().active_keys()[i]));
    rings.push_back(through.ring().active_rings()[i]);
  }
  REQUIRE(tiles.size() > 20);
  CHECK(deferred > 0);

  World restored(params);
  const char* why = nullptr;
  REQUIRE_MESSAGE(restored.restore({tiles.data(), tiles.size()}, {rings.data(), rings.size()},
                                   observers_at(49), 49, &why),
                  (why != nullptr ? why : ""));
  CHECK(restored.last().activated == tiles.size());
  CHECK(restored.last_events().size() == tiles.size());
  for (u32 step = 50; step < 90; ++step) {
    through.update(observers_at(step), step);
    restored.update(observers_at(step), step);
    const std::span<const TileEvent> a = through.last_events();
    const std::span<const TileEvent> b = restored.last_events();
    CAPTURE(step);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(TileEvent)) == 0);
  }

  // Refusals, each leaving the ring as it was.
  World busy(params);
  busy.update(observers_at(0), 0);
  CHECK_FALSE(busy.restore({tiles.data(), tiles.size()}, {rings.data(), rings.size()},
                           observers_at(0), 1, &why));
  CHECK(std::string_view(why) == "the ring already holds tiles");
  World fresh(params);
  const TileCoord backwards[2] = {TileCoord{3, 0}, TileCoord{1, 0}};
  const u8 inner[2] = {0, 0};
  CHECK_FALSE(fresh.restore(backwards, inner, observers_at(0), 1, &why));
  const TileCoord one[1] = {TileCoord{0, 0}};
  const u8 outside[1] = {5};
  CHECK_FALSE(fresh.restore(one, outside, observers_at(0), 1, &why));
  CHECK(fresh.ring().active_count() == 0);
}

TEST_CASE("world save: a player's input state is a fold of its log, however it is fed") {
  const input::ActionMap map = wasd();
  const input::InputLog log = walk(map);

  // One world binds the log at tick 0 and steps every tick; another binds it at tick 25, the way a
  // world loaded from a save does, and steps on from there. From tick 26 they move alike.
  InputObserver from_start;
  std::string error;
  REQUIRE_MESSAGE(from_start.bind(map, log, "move", 4.0f, 0, error), error);
  from_start.catch_up(0);
  Vector<Vec3> velocities;
  for (u64 t = 1; t <= 50; ++t)
    velocities.push_back(from_start.step(t));
  // W alone is forward, -z; W and D is the diagonal, clamped by the Axis2 into the unit disc or
  // not, as the input module decides — the same either way; A alone is -x.
  CHECK(velocities[4].z < 0.0f);
  CHECK(velocities[4].x == 0.0f);
  CHECK(velocities[14].x > 0.0f);
  CHECK(velocities[35].x < 0.0f);
  CHECK(velocities[48].x == 0.0f);

  InputObserver loaded;
  REQUIRE(loaded.bind(map, log, "move", 4.0f, 0, error));
  loaded.catch_up(25);
  CHECK(loaded.fed_through() == 25);
  for (u64 t = 26; t <= 50; ++t) {
    const Vec3 v = loaded.step(t);
    CAPTURE(t);
    CHECK(v.x == velocities[static_cast<u32>(t - 1)].x);
    CHECK(v.z == velocities[static_cast<u32>(t - 1)].z);
  }
  CHECK(loaded.state_hash() == from_start.state_hash());

  // What a save keeps is the log up to its tick.
  input::InputLog cut;
  from_start.recorded(20, cut);
  CHECK(cut.size() == 3);
  CHECK(cut.map_hash() == map.hash());
  from_start.recorded(31, cut);
  CHECK(cut.size() == 5);

  // Refusals: a log recorded against another map, an action that is not an Axis2, a bad speed.
  input::ActionMap other = wasd();
  other.add_action("crouch", input::ActionKind::Button);
  InputObserver refused;
  CHECK_FALSE(refused.bind(other, log, "move", 4.0f, 0, error));
  CHECK(error.find("another action map") != std::string::npos);
  CHECK_FALSE(refused.bind(map, log, "jump", 4.0f, 0, error));
  CHECK(error.find("Axis2") != std::string::npos);
  CHECK_FALSE(refused.bind(map, log, "move", -1.0f, 0, error));
  CHECK_FALSE(refused.bound());
}

TEST_CASE("world save: a save reads back as written, and restores to the same bytes") {
  const test::TempDir tmp("engine_world_save");
  REQUIRE(tmp.ok());
  const std::string doc_dir = tmp.file("session");
  doc::Document document;
  make_document(doc_dir, document);
  WorldStore store;
  REQUIRE(store.open(doc_dir + "/world.db") == store::Status::Ok);
  Vector<TileRow> rows;
  TileRow row;
  row.record = id_of(1);
  row.type = k_node;
  row.position = Vec3{5.0f, 0.0f, 5.0f};
  rows.push_back(row);
  REQUIRE(store.write_tile(TileCoord{0, 0}, {rows.data(), rows.size()}, 30, 30 * 16667) ==
          store::Status::Ok);
  store::EventRecord event;
  event.tile = store_tile(TileCoord{0, 0});
  event.sim_tick = 30;
  event.type = 5;
  const std::string payload = "{\"record\":\"x\"}";
  event.payload = {reinterpret_cast<const u8*>(payload.data()), payload.size()};
  REQUIRE(store.log().append(event) == store::Status::Ok);

  const input::ActionMap map = wasd();
  InputObserver player;
  std::string error;
  REQUIRE(player.bind(map, walk(map), "move", 4.0f, 0, error));
  player.catch_up(30);

  const StateHash at_save = hash_of(30, document, &store.log());
  SaveManifest manifest = manifest_at(30, at_save);
  SaveInputs inputs;
  inputs.document_dir = doc_dir;
  inputs.document = &document;
  inputs.store = &store.database();
  inputs.player = &player;
  const std::string first = tmp.file("save-1");
  REQUIRE_MESSAGE(write_save(first, inputs, manifest, error), error);

  // What it wrote: the document without the store's files, the store's backup, the player's log
  // cut at the save's tick and its map, and a manifest naming and hashing each one.
  CHECK(std::filesystem::exists(first + "/save.json"));
  CHECK(std::filesystem::exists(first + "/document/manifest.json"));
  CHECK_FALSE(std::filesystem::exists(first + "/document/world.db"));
  CHECK_FALSE(std::filesystem::exists(first + "/document/world.db-wal"));
  CHECK(std::filesystem::exists(first + "/world.db"));
  CHECK(manifest.format == k_save_format);
  CHECK(manifest.store == "world.db");
  REQUIRE(manifest.ring.has_value());
  REQUIRE(manifest.ring->player.has_value());
  CHECK(manifest.ring->player->log == "input.jsonl");
  bool names_store = false;
  bool names_node = false;
  for (const SaveVersion& v : manifest.versions) {
    names_store = names_store || (v.name == "store.tables" && v.version >= 1);
    names_node = names_node || v.name == k_node;
  }
  CHECK(names_store);
  CHECK(names_node);
  for (const SaveFile& f : manifest.files) {
    CAPTURE(f.path);
    CHECK(f.path != "save.json");
    CHECK(f.hash.size() == 16);
  }
  input::InputLog saved_log;
  REQUIRE(saved_log.load(first + "/input.jsonl") == io::Status::Ok);
  CHECK(saved_log.size() == 3);  // the events at ticks 3, 10 and 20; 31's are after the save

  // Read back: the manifest as written, every file as hashed.
  SaveManifest read;
  SaveCheck check;
  REQUIRE_MESSAGE(read_save(first, read, check, error), error);
  CHECK(schema::to_json(read) == schema::to_json(manifest));
  CHECK(check.migrations.empty());
  CHECK(check.bytes > 0);

  // Restored into a fresh directory and saved again from there: every file the same bytes, the
  // manifest among them — a save is a function of the state it holds.
  const std::string restored = tmp.file("restored");
  REQUIRE(std::filesystem::create_directories(restored));
  REQUIRE_MESSAGE(restore_save_files(first, read, restored, error), error);
  {
    io::Vfs vfs;
    doc::Document loaded;
    doc::DocumentManifest doc_manifest;
    REQUIRE_MESSAGE(doc::DocumentStore::load(vfs, restored, loaded, doc_manifest, &error), error);
    WorldStore again;
    REQUIRE(again.open(restored + "/world.db", false) == store::Status::Ok);
    CHECK(hash_of(30, loaded, &again.log()).value == at_save.value);
    input::ActionMap saved_map;
    input::InputLog log;
    REQUIRE_MESSAGE(read_save_player(first, *read.ring->player, saved_map, log, error), error);
    InputObserver back;
    REQUIRE(back.bind(saved_map, log, read.ring->player->action, read.ring->player->speed,
                      read.ring->player->observer, error));
    back.catch_up(30);
    CHECK(back.state_hash() == player.state_hash());

    SaveManifest again_manifest = manifest_at(30, at_save);
    SaveInputs from_restored;
    from_restored.document_dir = restored;
    from_restored.document = &loaded;
    from_restored.store = &again.database();
    from_restored.player = &back;
    const std::string second = tmp.file("save-2");
    REQUIRE_MESSAGE(write_save(second, from_restored, again_manifest, error), error);
    for (const SaveFile& f : manifest.files) {
      CAPTURE(f.path);
      CHECK(file_bytes(first + "/" + f.path) == file_bytes(second + "/" + f.path));
    }
    CHECK(file_bytes(first + "/save.json") == file_bytes(second + "/save.json"));
  }

  // Refusals, each before anything is loaded and naming what is wrong.
  CHECK_FALSE(write_save(first, inputs, manifest, error));  // not an empty directory
  CHECK(error.find("not empty") != std::string::npos);
  CHECK_FALSE(write_save(doc_dir + "/saves/one", inputs, manifest, error));
  CHECK(error.find("inside the document") != std::string::npos);

  auto copy_of = [&](const char* name) {
    const std::string to = tmp.file(name);
    std::filesystem::copy(first, to, std::filesystem::copy_options::recursive);
    return to;
  };
  {
    const std::string changed = copy_of("changed");
    std::string bytes = file_bytes(changed + "/document/manifest.json");
    bytes.push_back(' ');
    write_text(changed + "/document/manifest.json", bytes);
    CHECK_FALSE(read_save(changed, read, check, error));
    CHECK(error.find("document/manifest.json") != std::string::npos);
  }
  {
    const std::string missing = copy_of("missing");
    std::filesystem::remove(missing + "/world.db");
    CHECK_FALSE(read_save(missing, read, check, error));
    CHECK(error.find("world.db is missing") != std::string::npos);
  }
  auto edited = [&](const char* name, auto&& edit) {
    const std::string dir = copy_of(name);
    JsonValue json;
    REQUIRE(parse_json(file_bytes(dir + "/save.json"), json).ok);
    edit(json);
    write_text(dir + "/save.json", write_json(json));
    return dir;
  };
  {
    // Written by a newer engine: its store's tables are a version this build has never heard of.
    const std::string newer = edited("newer", [](JsonValue& json) {
      JsonValue& versions = json["versions"];
      for (usize i = 0; i < versions.size(); ++i) {
        if (versions[i]["name"] == JsonValue("store.tables"))
          versions[i].set("version", JsonValue(99u));
      }
    });
    CHECK_FALSE(read_save(newer, read, check, error));
    CHECK(error.find("store.tables is version 99, newer than this build's") != std::string::npos);
  }
  {
    const std::string unknown = edited("unknown", [](JsonValue& json) {
      JsonValue v = JsonValue::object();
      v.set("name", JsonValue("engine.cloth.Drape"));
      v.set("version", JsonValue(1u));
      json["versions"].push_back(std::move(v));
    });
    CHECK_FALSE(read_save(unknown, read, check, error));
    CHECK(error.find("engine.cloth.Drape, which this build does not have") != std::string::npos);
  }
  {
    const std::string extra =
        edited("extra", [](JsonValue& json) { json.set("autosave_slot", JsonValue(3u)); });
    CHECK_FALSE(read_save(extra, read, check, error));
    CHECK(error.find("autosave_slot") != std::string::npos);
  }
  {
    const std::string not_a_save = edited(
        "not_a_save", [](JsonValue& json) { json.set("format", JsonValue("engine.scene")); });
    CHECK_FALSE(read_save(not_a_save, read, check, error));
    CHECK(error.find("not a save") != std::string::npos);
  }
  CHECK_FALSE(read_save(doc_dir, read, check, error));
  CHECK(error.find("has no save.json") != std::string::npos);
}
