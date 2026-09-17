// The composed index (docs/subsystems/doc.md, docs/plan/03-data-model.md §3.2): every query is
// answered from it, and every mutation keeps it equal to what a linear composition would say.
#include <core/json/json.h>
#include <domain/doc/document.h>

#include <doctest/doctest.h>

#include <algorithm>
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
