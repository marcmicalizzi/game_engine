#include <core/json/json.h>
#include <domain/doc/merge.h>

#include <doctest/doctest.h>

#include <schemas/provenance.h>
#include <string>
#include <utility>

using namespace engine;
using namespace engine::doc;

namespace {

const char* const k_prov = "engine.content.AssetProvenance";

ObjectId id_of(u64 n) { return Id128::from_seed(1, n); }

ObjectRecord make(ObjectId id, const char* type, std::optional<ObjectId> parent,
                  std::initializer_list<std::pair<const char*, JsonValue>> properties) {
  ObjectRecord r;
  r.id = id;
  r.type = type;
  r.parent = parent;
  for (const auto& [name, value] : properties)
    r.properties.insert_or_assign(std::string(name), value);
  return r;
}

// Three objects in a chain: 1 at the root, 2 under 1, 3 under 2.
Layer base_layer() {
  Layer l("world", LayerRole::Base);
  l.set(
      make(id_of(1), k_prov, ObjectId{}, {{"name", JsonValue("a")}, {"seed", JsonValue(u64{1})}}));
  l.set(make(id_of(2), k_prov, id_of(1),
             {{"name", JsonValue("b")}, {"position", JsonValue("0 0 0")}}));
  l.set(make(id_of(3), k_prov, id_of(2), {{"name", JsonValue("c")}}));
  return l;
}

void set_property(Layer& layer, ObjectId id, const char* name, JsonValue value) {
  ObjectRecord* r = layer.find(id);
  REQUIRE(r != nullptr);
  r->properties.insert_or_assign(std::string(name), std::move(value));
}

const JsonValue* property_of(const Layer& layer, ObjectId id, const char* name) {
  const ObjectRecord* r = layer.find(id);
  return r != nullptr ? r->properties.find_value(name) : nullptr;
}

MergeResult merged_with(const Layer& base, const Layer& ours, const Layer& theirs,
                        MergeOptions::Prefer prefer) {
  MergeOptions options;
  options.prefer_on_conflict = prefer;
  MergeResult out;
  std::string error;
  REQUIRE(merge_layers(base, ours, theirs, options, out, &error));
  CHECK(error.empty());
  return out;
}

std::string hex(ObjectId id) {
  char buf[Id128::k_hex_length + 1];
  id.to_hex(buf);
  return buf;
}

}  // namespace

TEST_CASE("doc merge: a change made on one side only is taken, with the counts") {
  const Layer base = base_layer();
  Layer ours = base;
  set_property(ours, id_of(1), "name", JsonValue("ours-a"));
  set_property(ours, id_of(2), "position", JsonValue("1 0 0"));
  const Layer theirs = base;

  MergeResult r;
  std::string error;
  REQUIRE(merge_layers(base, ours, theirs, r, &error));
  CHECK(r.conflicts.empty());
  CHECK(r.applied_ours == 2);
  CHECK(r.applied_theirs == 0);
  CHECK(r.merged.name() == "world");
  CHECK(r.merged.role() == LayerRole::Base);
  CHECK(property_of(r.merged, id_of(1), "name")->as_string() == "ours-a");
  CHECK(property_of(r.merged, id_of(2), "position")->as_string() == "1 0 0");
  CHECK(property_of(r.merged, id_of(3), "name")->as_string() == "c");

  // The same, from the other side.
  MergeResult back;
  REQUIRE(merge_layers(base, theirs, ours, back, &error));
  CHECK(back.applied_ours == 0);
  CHECK(back.applied_theirs == 2);
  CHECK(back.merged.to_json_text() == r.merged.to_json_text());
}

TEST_CASE("doc merge: the same change on both sides is silent and counts for both") {
  const Layer base = base_layer();
  Layer ours = base;
  Layer theirs = base;
  set_property(ours, id_of(1), "name", JsonValue("same"));
  set_property(theirs, id_of(1), "name", JsonValue("same"));

  const MergeResult r = merged_with(base, ours, theirs, MergeOptions::Ours);
  CHECK(r.conflicts.empty());
  CHECK(r.applied_ours == 1);
  CHECK(r.applied_theirs == 1);
  CHECK(property_of(r.merged, id_of(1), "name")->as_string() == "same");
}

TEST_CASE("doc merge: different properties of one object merge into one object") {
  const Layer base = base_layer();
  Layer ours = base;
  Layer theirs = base;
  set_property(ours, id_of(2), "position", JsonValue("7 8 9"));
  set_property(theirs, id_of(2), "name", JsonValue("renamed"));

  const MergeResult r = merged_with(base, ours, theirs, MergeOptions::Ours);
  CHECK(r.conflicts.empty());
  CHECK(r.applied_ours == 1);
  CHECK(r.applied_theirs == 1);
  const ObjectRecord* m = r.merged.find(id_of(2));
  REQUIRE(m != nullptr);
  CHECK(m->properties.find_value("position")->as_string() == "7 8 9");
  CHECK(m->properties.find_value("name")->as_string() == "renamed");
  CHECK(m->properties.size() == 2);
  // A rename is not a conflict: the object is the same object either way.
  CHECK(m->id == id_of(2));
}

TEST_CASE("doc merge: one property changed to two values conflicts, and prefer decides") {
  const Layer base = base_layer();
  Layer ours = base;
  Layer theirs = base;
  set_property(ours, id_of(1), "name", JsonValue("ours-a"));
  set_property(theirs, id_of(1), "name", JsonValue("theirs-a"));

  const MergeResult kept_ours = merged_with(base, ours, theirs, MergeOptions::Ours);
  REQUIRE(kept_ours.conflicts.size() == 1);
  const MergeConflict& c = kept_ours.conflicts[0];
  CHECK(c.kind == MergeConflict::PropertyBothChanged);
  CHECK(std::string(conflict_kind_name(c.kind)) == "PropertyBothChanged");
  CHECK(c.object == id_of(1));
  CHECK(c.property == "name");
  CHECK(c.base.as_string() == "a");
  CHECK(c.ours.as_string() == "ours-a");
  CHECK(c.theirs.as_string() == "theirs-a");
  CHECK(property_of(kept_ours.merged, id_of(1), "name")->as_string() == "ours-a");
  CHECK(kept_ours.applied_ours == 1);
  CHECK(kept_ours.applied_theirs == 0);

  const MergeResult kept_theirs = merged_with(base, ours, theirs, MergeOptions::Theirs);
  CHECK(kept_theirs.conflicts.size() == 1);
  CHECK(property_of(kept_theirs.merged, id_of(1), "name")->as_string() == "theirs-a");
  CHECK(kept_theirs.applied_ours == 0);
  CHECK(kept_theirs.applied_theirs == 1);

  const MergeResult kept_base = merged_with(base, ours, theirs, MergeOptions::Neither);
  CHECK(kept_base.conflicts.size() == 1);
  CHECK(property_of(kept_base.merged, id_of(1), "name")->as_string() == "a");
  CHECK(kept_base.applied_ours == 0);
  CHECK(kept_base.applied_theirs == 0);
  CHECK(kept_base.merged.to_json_text() == base.to_json_text());

  // A property one side clears and the other sets is the same kind of conflict; the cleared
  // side is reported as JSON null.
  Layer cleared = base;
  cleared.find(id_of(1))->properties.erase("name");
  const MergeResult r = merged_with(base, cleared, theirs, MergeOptions::Ours);
  REQUIRE(r.conflicts.size() == 1);
  CHECK(r.conflicts[0].ours.is_null());
  CHECK(r.conflicts[0].theirs.as_string() == "theirs-a");
  CHECK(property_of(r.merged, id_of(1), "name") == nullptr);
}

TEST_CASE("doc merge: deletion against nothing, against a modification, and on both sides") {
  const Layer base = base_layer();

  // Deleted on one side, untouched on the other: deleted.
  {
    Layer ours = base;
    ours.remove(id_of(3));
    const MergeResult r = merged_with(base, ours, base, MergeOptions::Ours);
    CHECK(r.conflicts.empty());
    CHECK(r.merged.find(id_of(3)) == nullptr);
    CHECK(r.applied_ours == 1);
    CHECK(r.applied_theirs == 0);
  }
  // A tombstone is a deletion too, and survives the same way.
  {
    Layer ours = base;
    ours.find(id_of(3))->deleted = true;
    const MergeResult r = merged_with(base, ours, base, MergeOptions::Ours);
    CHECK(r.conflicts.empty());
    REQUIRE(r.merged.find(id_of(3)) != nullptr);
    CHECK(r.merged.find(id_of(3))->deleted);
  }
  // Deleted on one side, modified on the other: kept, with the modification.
  {
    Layer ours = base;
    Layer theirs = base;
    ours.remove(id_of(3));
    set_property(theirs, id_of(3), "name", JsonValue("still here"));
    const MergeResult r = merged_with(base, ours, theirs, MergeOptions::Ours);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].kind == MergeConflict::DeletedAndModified);
    CHECK(r.conflicts[0].property.empty());
    CHECK(r.conflicts[0].ours.is_null());  // the record is gone on our side
    CHECK(r.conflicts[0].base.is_object());
    CHECK(property_of(r.merged, id_of(3), "name")->as_string() == "still here");
    CHECK(r.applied_ours == 0);
    CHECK(r.applied_theirs == 1);

    // Preferring theirs keeps the modification just the same; only Neither leaves base alone.
    const MergeResult toward_theirs = merged_with(base, ours, theirs, MergeOptions::Theirs);
    CHECK(property_of(toward_theirs.merged, id_of(3), "name")->as_string() == "still here");
    const MergeResult neither = merged_with(base, ours, theirs, MergeOptions::Neither);
    CHECK(neither.conflicts.size() == 1);
    CHECK(property_of(neither.merged, id_of(3), "name")->as_string() == "c");
    CHECK(neither.merged.to_json_text() == base.to_json_text());
  }
  // Deleted on both sides, by different means: removed, silently.
  {
    Layer ours = base;
    Layer theirs = base;
    ours.remove(id_of(3));
    theirs.find(id_of(3))->deleted = true;
    const MergeResult r = merged_with(base, ours, theirs, MergeOptions::Ours);
    CHECK(r.conflicts.empty());
    CHECK(r.merged.find(id_of(3)) == nullptr);
    CHECK(r.applied_ours == 1);
    CHECK(r.applied_theirs == 1);
    MergeResult swapped;
    std::string error;
    REQUIRE(merge_layers(base, theirs, ours, swapped, &error));
    CHECK(swapped.merged.to_json_text() == r.merged.to_json_text());
  }
}

TEST_CASE("doc merge: an object created on both sides") {
  const Layer base = base_layer();
  const ObjectRecord same =
      make(id_of(9), k_prov, id_of(1), {{"name", JsonValue("new")}, {"seed", JsonValue(u64{9})}});

  {  // Identical content: no conflict.
    Layer ours = base;
    Layer theirs = base;
    ours.set(same);
    theirs.set(same);
    const MergeResult r = merged_with(base, ours, theirs, MergeOptions::Ours);
    CHECK(r.conflicts.empty());
    CHECK(r.applied_ours == 1);
    CHECK(r.applied_theirs == 1);
    REQUIRE(r.merged.find(id_of(9)) != nullptr);
    CHECK(*r.merged.find(id_of(9)) == same);
  }
  {  // Different content: a whole-object conflict, resolved by prefer.
    Layer ours = base;
    Layer theirs = base;
    ours.set(same);
    ObjectRecord other = same;
    other.properties.insert_or_assign("name", JsonValue("theirs"));
    theirs.set(other);

    const MergeResult r = merged_with(base, ours, theirs, MergeOptions::Ours);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].kind == MergeConflict::CreatedBothDifferent);
    CHECK(r.conflicts[0].property.empty());
    CHECK(r.conflicts[0].base.is_null());
    CHECK(r.conflicts[0].ours.is_object());
    CHECK(property_of(r.merged, id_of(9), "name")->as_string() == "new");
    CHECK(r.applied_ours == 1);
    CHECK(r.applied_theirs == 0);

    CHECK(
        property_of(merged_with(base, ours, theirs, MergeOptions::Theirs).merged, id_of(9), "name")
            ->as_string() == "theirs");
    const MergeResult neither = merged_with(base, ours, theirs, MergeOptions::Neither);
    CHECK(neither.merged.find(id_of(9)) == nullptr);  // base has no record to keep
  }
}

TEST_CASE("doc merge: reparenting, both at once and into a cycle") {
  const Layer base = base_layer();

  {  // One side reparents: taken.
    Layer ours = base;
    ours.find(id_of(3))->parent = id_of(1);
    const MergeResult r = merged_with(base, ours, base, MergeOptions::Ours);
    CHECK(r.conflicts.empty());
    CHECK(r.merged.find(id_of(3))->parent == std::optional<ObjectId>(id_of(1)));
    CHECK(r.applied_ours == 1);
  }
  {  // Both reparent, differently: a conflict about "$parent".
    Layer ours = base;
    Layer theirs = base;
    ours.find(id_of(3))->parent = id_of(1);
    theirs.find(id_of(3))->parent = ObjectId{};
    const MergeResult r = merged_with(base, ours, theirs, MergeOptions::Ours);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].kind == MergeConflict::PropertyBothChanged);
    CHECK(r.conflicts[0].property == "$parent");
    CHECK(r.conflicts[0].base.as_string() == hex(id_of(2)));
    CHECK(r.conflicts[0].ours.as_string() == hex(id_of(1)));
    CHECK(r.merged.find(id_of(3))->parent == std::optional<ObjectId>(id_of(1)));
    CHECK(r.applied_ours == 1);
    CHECK(r.applied_theirs == 0);
  }
  {  // A reparent that closes a cycle: base's parent stands.
    Layer ours = base;
    ours.find(id_of(1))->parent = id_of(3);  // 1 -> 3 -> 2 -> 1
    const MergeResult r = merged_with(base, ours, base, MergeOptions::Ours);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].kind == MergeConflict::ParentCycle);
    CHECK(r.conflicts[0].property == "$parent");
    CHECK(r.conflicts[0].ours.as_string() == hex(id_of(3)));
    CHECK(r.merged.find(id_of(1))->parent == std::optional<ObjectId>(ObjectId{}));
    CHECK(r.applied_ours == 0);
    CHECK(r.merged.to_json_text() == base.to_json_text());
  }
  {  // One reparent from each side, which only together close the cycle.
    Layer ours = base;
    Layer theirs = base;
    ours.find(id_of(1))->parent = id_of(2);  // 1 under 2
    theirs.find(id_of(2))->parent = ObjectId{};
    const MergeResult r = merged_with(base, ours, theirs, MergeOptions::Ours);
    // 2 moves to the root, 1 moves under 2: no cycle, both survive.
    CHECK(r.conflicts.empty());
    CHECK(r.merged.find(id_of(1))->parent == std::optional<ObjectId>(id_of(2)));
    CHECK(r.merged.find(id_of(2))->parent == std::optional<ObjectId>(ObjectId{}));
    CHECK(r.applied_ours == 1);
    CHECK(r.applied_theirs == 1);
  }
  {  // A base that already has a cycle is not an ancestor anything can be merged over.
    Layer cyclic = base;
    cyclic.find(id_of(1))->parent = id_of(3);
    MergeResult r;
    std::string error;
    CHECK_FALSE(merge_layers(cyclic, cyclic, cyclic, r, &error));
    CHECK(error.find("parent cycle") != std::string::npos);
  }
}

TEST_CASE("doc merge: a type changed on one side, and on both") {
  const Layer base = base_layer();
  Layer ours = base;
  Layer theirs = base;
  ours.find(id_of(3))->type = "engine.doc.Attribution";
  const MergeResult one = merged_with(base, ours, base, MergeOptions::Ours);
  CHECK(one.conflicts.empty());
  CHECK(one.merged.find(id_of(3))->type == "engine.doc.Attribution");

  theirs.find(id_of(3))->type = "engine.doc.Patch";
  const MergeResult both = merged_with(base, ours, theirs, MergeOptions::Ours);
  REQUIRE(both.conflicts.size() == 1);
  CHECK(both.conflicts[0].property == "$type");
  CHECK(both.conflicts[0].base.as_string() == k_prov);
  CHECK(both.merged.find(id_of(3))->type == "engine.doc.Attribution");
  CHECK(merged_with(base, ours, theirs, MergeOptions::Neither).merged.find(id_of(3))->type ==
        k_prov);
}

TEST_CASE("doc merge: conflict-free merges are commutative and idempotent") {
  const Layer base = base_layer();
  Layer ours = base;
  Layer theirs = base;
  set_property(ours, id_of(1), "name", JsonValue("ours-a"));
  set_property(ours, id_of(2), "position", JsonValue("1 2 3"));
  ours.set(make(id_of(7), k_prov, id_of(1), {{"name", JsonValue("ours-new")}}));
  set_property(theirs, id_of(2), "name", JsonValue("theirs-b"));
  theirs.remove(id_of(3));
  theirs.set(make(id_of(8), k_prov, id_of(2), {{"name", JsonValue("theirs-new")}}));

  MergeResult forward;
  MergeResult backward;
  std::string error;
  REQUIRE(merge_layers(base, ours, theirs, forward, &error));
  REQUIRE(merge_layers(base, theirs, ours, backward, &error));
  CHECK(forward.conflicts.empty());
  CHECK(backward.conflicts.empty());
  CHECK(forward.merged.to_json_text() == backward.merged.to_json_text());
  CHECK(forward.applied_ours == 3);    // two properties and one new record
  CHECK(forward.applied_theirs == 3);  // one property, one removed record, one new record
  CHECK(forward.applied_ours == backward.applied_theirs);
  CHECK(forward.applied_theirs == backward.applied_ours);
  CHECK(forward.merged.find(id_of(3)) == nullptr);
  CHECK(property_of(forward.merged, id_of(2), "name")->as_string() == "theirs-b");
  CHECK(property_of(forward.merged, id_of(2), "position")->as_string() == "1 2 3");

  // Merging the result with itself over itself changes nothing.
  MergeResult again;
  REQUIRE(merge_layers(forward.merged, forward.merged, forward.merged, again, &error));
  CHECK(again.conflicts.empty());
  CHECK(again.applied_ours == 0);
  CHECK(again.applied_theirs == 0);
  CHECK(again.merged.to_json_text() == forward.merged.to_json_text());

  // And re-merging the result against one of its parents adds nothing either.
  MergeResult settled;
  REQUIRE(merge_layers(base, forward.merged, theirs, settled, &error));
  CHECK(settled.conflicts.empty());
  CHECK(settled.merged.to_json_text() == forward.merged.to_json_text());
}

// ---- randomized ------------------------------------------------------------------------------

namespace {

constexpr u32 k_objects = 12;

struct Rng {
  u64 state;
  u32 next() noexcept {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<u32>(state >> 32);
  }
  u32 below(u32 n) noexcept { return next() % n; }
};

ObjectId object_id(u32 k) { return Id128::from_seed(7, k); }

// A heap-shaped tree: object k hangs under (k - 1) / 2, so it is acyclic by construction and
// the objects below k_objects / 2 are the only ones with children.
Layer random_base() {
  Layer l("world", LayerRole::Base);
  for (u32 k = 0; k < k_objects; ++k) {
    ObjectRecord r;
    r.id = object_id(k);
    r.type = k_prov;
    r.parent = k == 0 ? ObjectId{} : object_id((k - 1) / 2);
    r.properties.insert_or_assign(std::string("generator"), JsonValue("gen-" + std::to_string(k)));
    r.properties.insert_or_assign(std::string("seed"), JsonValue(static_cast<u64>(k)));
    l.set(std::move(r));
  }
  return l;
}

// Random edits that keep the layer valid: only leaves are removed or moved, and they only move
// under interior objects, so no edit and no merge of two of them can close a cycle.
Layer random_edits(const Layer& base, Rng& rng, u32 count, const char* tag) {
  Layer out = base;
  for (u32 e = 0; e < count; ++e) {
    const u32 kind = rng.below(6);
    const u32 k = rng.below(k_objects);
    ObjectRecord* r = out.find(object_id(k));
    switch (kind) {
      case 0:
        if (r != nullptr)
          r->properties.insert_or_assign(std::string("generator"),
                                         JsonValue(std::string(tag) + "-" + std::to_string(e)));
        break;
      case 1:
        if (r != nullptr)
          r->properties.insert_or_assign(std::string("license"),
                                         JsonValue(std::string("MIT-") + tag));
        break;
      case 2:
        if (r != nullptr) r->properties.erase("seed");
        break;
      case 3:
        if (r != nullptr && k >= k_objects / 2) r->parent = object_id(rng.below(k_objects / 2));
        break;
      case 4:
        if (k >= k_objects / 2) out.remove(object_id(k));
        break;
      default: {
        ObjectRecord fresh;
        fresh.id = Id128::from_seed(8, rng.below(4));
        fresh.type = k_prov;
        fresh.parent = object_id(rng.below(k_objects / 2));
        fresh.properties.insert_or_assign(std::string("generator"),
                                          JsonValue(std::string("new-") + tag));
        out.set(std::move(fresh));
        break;
      }
    }
  }
  return out;
}

const RecordDiff* find_diff(const Vector<RecordDiff>& diffs, ObjectId id) {
  for (const RecordDiff& d : diffs) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

const PropertyChange* find_change(const RecordDiff& d, const std::string& name) {
  for (const PropertyChange& p : d.properties) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

// Everything `side` changed that `other` left alone must be in the merged layer verbatim.
void check_uncontested_edits(const Layer& base, const Layer& side, const Layer& other,
                             const Layer& merged) {
  const Vector<RecordDiff> mine = diff_records(base, side);
  const Vector<RecordDiff> yours = diff_records(base, other);
  for (const RecordDiff& d : mine) {
    const RecordDiff* o = find_diff(yours, d.id);
    if (d.removed) {
      if (o == nullptr) CHECK(merged.find(d.id) == nullptr);
      continue;
    }
    if (d.added) {
      if (o == nullptr) {
        const ObjectRecord* added = merged.find(d.id);
        REQUIRE(added != nullptr);
        CHECK(*added == *side.find(d.id));
      }
      continue;
    }
    if (o != nullptr && (o->added || o->removed)) continue;  // a whole-record conflict
    const ObjectRecord* m = merged.find(d.id);
    REQUIRE(m != nullptr);
    if (d.parent_changed && (o == nullptr || !o->parent_changed)) CHECK(m->parent == d.parent);
    for (const PropertyChange& p : d.properties) {
      const PropertyChange* theirs = o != nullptr ? find_change(*o, p.name) : nullptr;
      const bool contested = theirs != nullptr && (theirs->removed != p.removed ||
                                                   (!p.removed && !(theirs->value == p.value)));
      if (contested) continue;  // decided by MergeOptions, not by this rule
      const JsonValue* value = m->properties.find_value(p.name);
      if (p.removed) {
        CHECK(value == nullptr);
      } else {
        REQUIRE(value != nullptr);
        CHECK(*value == p.value);
      }
    }
  }
}

}  // namespace

TEST_CASE("doc merge: random edit sequences merge into a valid layer keeping every edit") {
  const Layer base = random_base();
  Rng rng{0x9e3779b97f4a7c15ull};
  u32 total_conflicts = 0;
  for (u32 round = 0; round < 40; ++round) {
    const Layer ours = random_edits(base, rng, 10, "ours");
    const Layer theirs = random_edits(base, rng, 10, "theirs");

    MergeResult r;
    std::string error;
    REQUIRE(merge_layers(base, ours, theirs, r, &error));
    total_conflicts += r.conflicts.size();
    for (const MergeConflict& c : r.conflicts)
      CHECK(c.kind != MergeConflict::ParentCycle);  // these edits cannot close one

    check_uncontested_edits(base, ours, theirs, r.merged);
    check_uncontested_edits(base, theirs, ours, r.merged);

    // The merged layer is a document a validator accepts: registered types, parents that exist,
    // no cycles, property values of the right type.
    Document d;
    d.add_layer(Layer(r.merged));
    Vector<Diagnostic> diagnostics;
    if (!d.validate(diagnostics)) {
      std::string joined;
      for (const Diagnostic& x : diagnostics)
        joined += x.path + ": " + x.message + "\n";
      FAIL("round " << round << " produced an invalid layer:\n" << joined);
    }

    // With no conflicts the order of the two sides cannot matter.
    if (r.conflicts.empty()) {
      MergeResult swapped;
      REQUIRE(merge_layers(base, theirs, ours, swapped, &error));
      CHECK(swapped.merged.to_json_text() == r.merged.to_json_text());
    }
  }
  CHECK(total_conflicts > 0);  // the generator does produce disagreements
  MESSAGE("conflicts over 40 rounds: " << total_conflicts);
}
