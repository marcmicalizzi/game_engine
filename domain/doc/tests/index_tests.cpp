// The composed index (docs/subsystems/doc.md, docs/plan/03-data-model.md §3.2): every query is
// answered from it, and every mutation keeps it equal to what a linear composition would say —
// the tile index among them.
#include <core/containers/flat_map.h>
#include <core/json/json.h>
#include <domain/doc/document.h>
#include <domain/doc/partition.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <schemas/doc_test_types.h>
#include <schemas/provenance.h>
#include <string>

using namespace engine;
using namespace engine::doc;

namespace {

const char* const k_prov = "engine.content.AssetProvenance";

ObjectId id_of(u32 n) { return Id128::from_parts(1, n + 1); }

Attribution who() {
  Attribution a;
  a.actor = "index-test";
  a.role = "test";
  a.task = "unit";
  a.timestamp_unix_ms = 1;
  return a;
}

// The composition this module used to do, kept out of the API: the reference the index is
// checked against, here and in domain/doc/bench.
bool linear_resolve(const Document& d, ObjectId id, ResolvedObject& out) {
  out = ResolvedObject{};
  out.id = id;
  bool defined = false;
  u32 defining = 0;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    const ObjectRecord* r = d.layer(i).find(id);
    if (r != nullptr && !r->type.empty()) {
      defined = true;
      defining = i;
      out.type = r->type;
    }
  }
  if (!defined) return false;
  out.defining_layer = defining;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    const ObjectRecord* r = d.layer(i).find(id);
    if (r == nullptr) continue;
    if (r->parent.has_value()) out.parent = *r->parent;
    if (r->deleted && i >= defining) out.deleted = true;
    for (auto [name, value] : r->properties)
      out.properties.insert_or_assign(std::string_view(name), &value);
  }
  return true;
}

Vector<ObjectId> linear_objects(const Document& d) {
  Vector<ObjectId> ids;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    for (auto [id, record] : d.layer(i).records()) {
      if (!record.type.empty()) ids.push_back(id);
    }
  }
  std::sort(ids.begin(), ids.end());
  Vector<ObjectId> out;
  for (u32 i = 0; i < ids.size(); ++i) {
    if (i > 0 && ids[i] == ids[i - 1]) continue;
    ResolvedObject r;
    if (linear_resolve(d, ids[i], r) && !r.deleted) out.push_back(ids[i]);
  }
  return out;
}

Vector<ObjectId> linear_children(const Document& d, ObjectId parent) {
  Vector<ObjectId> out;
  for (const ObjectId id : linear_objects(d)) {
    ResolvedObject r;
    if (linear_resolve(d, id, r) && r.parent == parent) out.push_back(id);
  }
  return out;
}

// A deterministic stream, so a failure is reproducible from the seed printed below.
struct Rng {
  u64 state;
  u32 next(u32 bound) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>((state >> 33) % bound);
  }
};

// The tile a reader used to find for itself, before the document kept a tile index: the defining
// record — found by walking the layers — under its own layer's partition. The reference the tile
// index is checked against; it is the classification sim::Materializer did for every live record
// of a document on every tile pass.
bool linear_tile(const Document& d, ObjectId id, TileCoord& out) {
  ResolvedObject r;
  if (!linear_resolve(d, id, r) || r.deleted) return false;
  const Layer& layer = d.layer(r.defining_layer);
  if (!layer.partitioned()) return false;
  const ObjectRecord* defining = layer.find(id);
  return defining != nullptr && tile_of(*defining, layer.partition(), out);
}

// Every tile's live objects and the untiled ones, by the linear rule, against the index's answers.
// Returns the first disagreement, or an empty string.
std::string compare_tiles(const Document& d) {
  FlatMap<TileCoord, Vector<ObjectId>> want;
  Vector<ObjectId> want_untiled;
  for (const ObjectId id : linear_objects(d)) {
    TileCoord tile;
    if (linear_tile(d, id, tile)) {
      want[tile].push_back(id);
    } else {
      want_untiled.push_back(id);
    }
    TileCoord have;
    const bool have_tiled = d.object_tile(id, have);
    if (have_tiled != linear_tile(d, id, tile) || (have_tiled && !(have == tile)))
      return "object_tile disagrees for an object";
  }
  if (d.occupied_tiles() != want.size()) return "occupied_tiles disagrees";
  for (auto [tile, ids] : want) {
    if (d.ids_in_tile(tile) != ids)
      return "ids_in_tile(" + std::to_string(tile.x) + ", " + std::to_string(tile.y) +
             ") disagrees";
  }
  if (d.untiled() != want_untiled) return "untiled() disagrees";
  return {};
}

const char* const k_placement = "engine.doc.test.Placement";

LayerPartition partition_of(std::string property, f64 tile_size) {
  LayerPartition p;
  p.property = std::move(property);
  p.tile_size = tile_size;
  return p;
}

JsonValue point(f64 x, f64 z) {
  JsonValue::Array a;
  a.push_back(JsonValue(x));
  a.push_back(JsonValue(1.5));
  a.push_back(JsonValue(z));
  return JsonValue(std::move(a));
}

}  // namespace

TEST_CASE("doc index: queries agree with a linear composition across a layer stack") {
  Document d;
  const u32 base = d.add_layer("base", LayerRole::Base);
  const u32 feature = d.add_layer("feature", LayerRole::Feature);
  const ObjectId a = id_of(0), b = id_of(1), c = id_of(2);

  d.set_edit_layer(base);
  REQUIRE(d.apply(cmd_create(a, k_prov), nullptr, nullptr));
  REQUIRE(d.apply(cmd_create(b, k_prov, a), nullptr, nullptr));
  REQUIRE(d.apply(cmd_create(c, k_prov, a), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set(b, "generator", JsonValue("base")), nullptr, nullptr));
  CHECK(d.validate_index());

  // The stronger layer reparents and overrides; the index moves the child with it.
  d.set_edit_layer(feature);
  REQUIRE(d.apply(cmd_set_parent(c, b), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set(b, "generator", JsonValue("feature")), nullptr, nullptr));
  CHECK(d.validate_index());
  CHECK(d.children(a) == Vector<ObjectId>{b});
  CHECK(d.children(b) == Vector<ObjectId>{c});
  CHECK(d.children(a) == linear_children(d, a));
  CHECK(d.property(b, "generator")->as_string() == "feature");
  CHECK(d.object_count() == 3);

  // A tombstone in the stronger layer takes the object out of objects() and out of its parent's
  // children, and restoring it puts it back where it was.
  REQUIRE(d.apply(cmd_delete(c), nullptr, nullptr));
  CHECK(d.validate_index());
  CHECK(d.children(b).empty());
  CHECK(d.objects() == linear_objects(d));
  REQUIRE(d.apply(cmd_remove_record(c), nullptr, nullptr));
  CHECK(d.validate_index());
  CHECK(d.children(a) == Vector<ObjectId>{b, c});  // the feature layer's reparent went with it

  // The mutable layer accessor is the one way to go behind the index's back, and the next query
  // rebuilds rather than answering from a stale entry.
  d.layer(base).ensure(id_of(9)).type = k_prov;
  CHECK(d.exists(id_of(9)));
  CHECK(d.validate_index());
}

TEST_CASE("doc index: add_layer, remove_layer, and the edit layer that follows") {
  Document d;
  d.add_layer("base", LayerRole::Base);
  const ObjectId a = id_of(0), b = id_of(1);
  REQUIRE(d.apply(cmd_create(a, k_prov), nullptr, nullptr));
  REQUIRE(d.apply(cmd_create(b, k_prov, a), nullptr, nullptr));

  // A layer arriving with records is composed on arrival, not on the next query.
  Layer feature("feature", LayerRole::Feature);
  ObjectRecord tombstone;
  tombstone.id = b;
  tombstone.deleted = true;
  feature.set(std::move(tombstone));
  ObjectRecord defined;
  defined.id = id_of(2);
  defined.type = k_prov;
  defined.parent = a;
  feature.set(std::move(defined));
  const u32 index = d.add_layer(std::move(feature));
  CHECK(d.validate_index());
  CHECK(d.children(a) == Vector<ObjectId>{id_of(2)});
  CHECK_FALSE(d.exists(b));

  d.set_edit_layer(index);
  CHECK_FALSE(d.remove_layer(7));
  CHECK(d.remove_layer(index));
  CHECK(d.edit_layer() == 0);  // the edit layer came down with it
  CHECK(d.validate_index());
  CHECK(d.exists(b));
  CHECK(d.children(a) == Vector<ObjectId>{b});
  CHECK(d.objects() == linear_objects(d));
  CHECK_FALSE(d.remove_layer(0));  // a document keeps its last layer
}

TEST_CASE("doc index: five thousand random commands, undo, redo, and layers") {
  Document d;
  d.add_layer("base", LayerRole::Base);
  d.add_layer("feature", LayerRole::Feature);
  Rng rng{0x5eed'1234'abcd'0001ull};
  constexpr u32 k_objects = 48;
  constexpr u32 k_commands = 5000;

  Vector<Patch> undone;  // the redo tail this test keeps by hand
  u32 mismatch_at = k_commands;
  std::string first_failure;
  for (u32 step = 0; step < k_commands && mismatch_at == k_commands; ++step) {
    const u32 pick = rng.next(100);
    const ObjectId id = id_of(rng.next(k_objects));
    const ObjectId other = id_of(rng.next(k_objects));
    if (d.layer_count() > 0) d.set_edit_layer(rng.next(d.layer_count()));

    if (pick < 26) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_create(id, k_prov, rng.next(4) == 0 ? other : ObjectId{}));
      tx.commit();
    } else if (pick < 40) {
      Transaction tx = d.begin(who());
      tx.apply(
          cmd_set(id, rng.next(2) == 0 ? "generator" : "license", JsonValue(std::to_string(step))));
      tx.commit();
    } else if (pick < 48) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_clear(id, rng.next(2) == 0 ? "generator" : "license"));
      tx.commit();
    } else if (pick < 58) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_set_parent(id, other));
      tx.commit();
    } else if (pick < 68) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_delete(id));
      tx.commit();
    } else if (pick < 74) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_remove_record(id));
      tx.commit();
    } else if (pick < 80) {
      // A transaction that rolls back must leave the index where it found it.
      Transaction tx = d.begin(who());
      tx.apply(cmd_create(id, k_prov));
      tx.apply(cmd_set(id, "generator", JsonValue("rolled back")));
      tx.rollback();
    } else if (pick < 90) {
      // A patch whose layer has been removed cannot be undone, and says so without touching
      // anything; everything else must come back exactly.
      if (d.journal().size() > undone.size()) {
        const Patch patch = d.journal()[d.journal().size() - undone.size() - 1];
        if (d.undo(patch)) undone.push_back(patch);
      }
    } else if (pick < 96) {
      if (!undone.empty()) {
        d.redo(undone.back());
        undone.pop_back();
      }
    } else if (pick < 98) {
      undone.clear();
      d.add_layer("layer" + std::to_string(step), LayerRole::Session);
    } else {
      undone.clear();
      if (d.layer_count() > 2) d.remove_layer(1 + rng.next(d.layer_count() - 1));
    }

    Vector<Diagnostic> diagnostics;
    if (!d.validate_index(&diagnostics)) {
      mismatch_at = step;
      for (const Diagnostic& x : diagnostics)
        first_failure += x.path + ": " + x.message + "\n";
    }
  }
  INFO("first mismatch at command " << mismatch_at << "\n" << first_failure);
  CHECK(mismatch_at == k_commands);

  // And the answers themselves, not just the bookkeeping.
  CHECK(d.objects() == linear_objects(d));
  for (u32 i = 0; i < k_objects; ++i) {
    const ObjectId id = id_of(i);
    ResolvedObject want;
    ResolvedObject have;
    const bool want_ok = linear_resolve(d, id, want);
    CHECK(d.resolve(id, have) == want_ok);
    if (!want_ok) continue;
    CHECK(have.type == want.type);
    CHECK(have.parent == want.parent);
    CHECK(have.deleted == want.deleted);
    CHECK(have.defining_layer == want.defining_layer);
    CHECK(have.properties.size() == want.properties.size());
    for (auto [name, value] : want.properties) {
      const JsonValue* const* mine = have.properties.find_value(name);
      REQUIRE(mine != nullptr);
      CHECK(**mine == *value);
    }
    CHECK(d.children(id) == linear_children(d, id));
  }
  CHECK(d.children(ObjectId{}) == linear_children(d, ObjectId{}));
}

TEST_CASE("doc index: the tile index follows positions, definitions and grids") {
  Document d;
  const u32 base = d.add_layer("base", LayerRole::Base);
  const u32 feature = d.add_layer("feature", LayerRole::Feature);
  d.set_layer_partition(base, partition_of("position", 10));
  const ObjectId a = id_of(0), b = id_of(1), c = id_of(2);

  d.set_edit_layer(base);
  JsonValue at = JsonValue::object();
  at.set("position", point(5, 5));
  REQUIRE(d.apply(cmd_create(a, k_placement, ObjectId{}, at), nullptr, nullptr));
  REQUIRE(d.apply(cmd_create(b, k_placement), nullptr, nullptr));  // no position: untiled
  REQUIRE(d.apply(cmd_create(c, k_prov), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set(c, "position", point(-3, 12)), nullptr, nullptr));
  CHECK(d.validate_index());
  CHECK(d.ids_in_tile(TileCoord{0, 0}) == Vector<ObjectId>{a});
  CHECK(d.ids_in_tile(TileCoord{-1, 1}) == Vector<ObjectId>{c});
  CHECK(d.untiled() == Vector<ObjectId>{b});
  CHECK(d.occupied_tiles() == 2);

  // A position change moves an object between tiles, and one that loses its position leaves them.
  REQUIRE(d.apply(cmd_set(a, "position", point(-3, 15)), nullptr, nullptr));
  CHECK(d.ids_in_tile(TileCoord{0, 0}).empty());
  CHECK(d.ids_in_tile(TileCoord{-1, 1}) == (Vector<ObjectId>{a, c}));
  REQUIRE(d.apply(cmd_clear(c, "position"), nullptr, nullptr));
  CHECK(d.untiled() == (Vector<ObjectId>{b, c}));
  CHECK(d.validate_index());

  // An override in a stronger layer moves nothing, even of the position: the object is filed by
  // its defining record, as the defining layer's tile files are. A tombstone takes it out.
  d.set_edit_layer(feature);
  REQUIRE(d.apply(cmd_set(a, "position", point(95, 95)), nullptr, nullptr));
  CHECK(d.ids_in_tile(TileCoord{-1, 1}) == (Vector<ObjectId>{a}));
  REQUIRE(d.apply(cmd_delete(a), nullptr, nullptr));
  CHECK(d.ids_in_tile(TileCoord{-1, 1}).empty());
  CHECK(d.occupied_tiles() == 0);
  TileCoord tile;
  CHECK(d.object_tile(a, tile));  // a deleted object's record is still filed where it was
  CHECK(tile == TileCoord{-1, 1});
  REQUIRE(d.apply(cmd_remove_record(a), nullptr, nullptr));
  CHECK(d.ids_in_tile(TileCoord{-1, 1}) == (Vector<ObjectId>{a}));
  CHECK_FALSE(d.object_tile(id_of(30), tile));  // an id no layer defines
  CHECK(d.validate_index());

  // A definition in the stronger, unpartitioned layer takes the object off the grid; removing it
  // puts it back where the weaker one files it.
  ObjectRecord redefined;
  redefined.id = a;
  redefined.type = k_placement;
  redefined.properties.insert_or_assign("position", point(5, 5));
  REQUIRE(d.apply(cmd_restore(a, redefined), nullptr, nullptr));
  CHECK_FALSE(d.object_tile(a, tile));
  CHECK(d.untiled() == (Vector<ObjectId>{a, b, c}));
  CHECK(d.validate_index());

  // A new grid re-files what the layer defines, and nothing else, without a stamp in the feed.
  const u64 revision = d.revision();
  d.set_layer_partition(feature, partition_of("", 4));  // Placement opts in by its type
  CHECK(d.revision() == revision);
  CHECK(d.object_tile(a, tile));
  CHECK(tile == TileCoord{1, 1});
  CHECK(d.untiled() == (Vector<ObjectId>{b, c}));
  d.set_layer_partition(base, LayerPartition{});
  CHECK(d.untiled() == (Vector<ObjectId>{b, c}));
  CHECK(d.validate_index());
  CHECK(compare_tiles(d).empty());
}

TEST_CASE("doc index: the tile index through five thousand random commands") {
  // Three layers on two grids and one with none; objects of a type that opts in and one that does
  // not; positions set, moved off the grid, replaced by something that is not a position, and
  // cleared; definitions, overrides, tombstones and removed records; rolled-back transactions,
  // undo and redo; layers added with a grid and removed; grids changed under the objects.
  Document d;
  d.add_layer("base", LayerRole::Base);
  d.add_layer("feature", LayerRole::Feature);
  d.add_layer("overrides", LayerRole::Session);
  d.set_layer_partition(0, partition_of("position", 16));
  d.set_layer_partition(1, partition_of("", 7));
  Rng rng{0x7115'0000'1dea'5eedull};
  constexpr u32 k_objects = 40;
  constexpr u32 k_commands = 5000;

  auto random_position = [&]() -> JsonValue {
    switch (rng.next(8)) {
      case 0: return JsonValue("not a position");
      case 1: {  // the object form
        JsonValue o = JsonValue::object();
        o.set("x", JsonValue(static_cast<f64>(rng.next(80)) - 40.0));
        o.set("z", JsonValue(static_cast<f64>(rng.next(80)) - 40.0));
        return o;
      }
      default:
        return point(static_cast<f64>(rng.next(800)) / 10.0 - 40.0,
                     static_cast<f64>(rng.next(800)) / 10.0 - 40.0);
    }
  };

  Vector<Patch> undone;
  u32 mismatch_at = k_commands;
  std::string first_failure;
  for (u32 step = 0; step < k_commands && mismatch_at == k_commands; ++step) {
    const u32 pick = rng.next(100);
    const ObjectId id = id_of(rng.next(k_objects));
    const ObjectId other = id_of(rng.next(k_objects));
    d.set_edit_layer(rng.next(d.layer_count()));

    if (pick < 22) {
      Transaction tx = d.begin(who());
      JsonValue props = JsonValue::object();
      if (rng.next(3) != 0) props.set("position", random_position());
      tx.apply(cmd_create(id, rng.next(3) == 0 ? k_prov : k_placement,
                          rng.next(4) == 0 ? other : ObjectId{}, std::move(props)));
      tx.commit();
    } else if (pick < 46) {
      // Strict half the time; otherwise an override of an object no layer may define yet.
      Transaction tx = d.begin(who());
      tx.apply(cmd_set(id, "position", random_position()), rng.next(2) == 0);
      tx.commit();
    } else if (pick < 54) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_clear(id, "position"));
      tx.commit();
    } else if (pick < 60) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_set_parent(id, other));
      tx.commit();
    } else if (pick < 68) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_delete(id));
      tx.commit();
    } else if (pick < 73) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_remove_record(id));
      tx.commit();
    } else if (pick < 78) {
      Transaction tx = d.begin(who());
      tx.apply(cmd_create(id, k_placement));
      tx.apply(cmd_set(id, "position", random_position()), false);
      tx.rollback();
    } else if (pick < 87) {
      if (d.journal().size() > undone.size()) {
        const Patch patch = d.journal()[d.journal().size() - undone.size() - 1];
        if (d.undo(patch)) undone.push_back(patch);
      }
    } else if (pick < 93) {
      if (!undone.empty()) {
        d.redo(undone.back());
        undone.pop_back();
      }
    } else if (pick < 96) {
      const u32 layer = rng.next(d.layer_count());
      switch (rng.next(3)) {
        case 0: d.set_layer_partition(layer, LayerPartition{}); break;
        case 1: d.set_layer_partition(layer, partition_of("position", 5 + rng.next(20))); break;
        default: d.set_layer_partition(layer, partition_of("", 5 + rng.next(20))); break;
      }
    } else if (pick < 98) {
      undone.clear();
      Layer arriving("layer" + std::to_string(step), LayerRole::Session);
      arriving.set_partition(partition_of("position", 16));
      for (u32 k = 0; k < 3; ++k) {
        ObjectRecord r;
        r.id = id_of(rng.next(k_objects));
        if (rng.next(2) == 0) r.type = k_placement;
        r.properties.insert_or_assign("position", random_position());
        arriving.set(std::move(r));
      }
      d.add_layer(std::move(arriving));
    } else {
      undone.clear();
      if (d.layer_count() > 3) d.remove_layer(1 + rng.next(d.layer_count() - 1));
    }

    Vector<Diagnostic> diagnostics;
    std::string tiles;
    if (!d.validate_index(&diagnostics) || !(tiles = compare_tiles(d)).empty()) {
      mismatch_at = step;
      for (const Diagnostic& x : diagnostics)
        first_failure += x.path + ": " + x.message + "\n";
      first_failure += tiles;
    }
  }
  INFO("first mismatch at command " << mismatch_at << "\n" << first_failure);
  CHECK(mismatch_at == k_commands);
  CHECK(d.occupied_tiles() > 0);
  CHECK_FALSE(d.untiled().empty());
}
