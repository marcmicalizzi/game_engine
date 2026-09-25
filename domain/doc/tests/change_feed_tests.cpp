// The change feed (docs/subsystems/doc.md, "Change feed"): what a reader that mirrors the document
// — the runtime world's materializer — asks instead of rescanning it.
#include <core/json/json.h>
#include <domain/doc/document.h>

#include <doctest/doctest.h>

#include <schemas/provenance.h>
#include <string>

using namespace engine;
using namespace engine::doc;

namespace {

const char* const k_prov = "engine.content.AssetProvenance";

ObjectId id_of(u32 n) { return Id128::from_parts(7, n + 1); }

Attribution who() {
  Attribution a;
  a.actor = "feed-test";
  a.role = "test";
  a.timestamp_unix_ms = 1;
  return a;
}

Document document_of(u32 count) {
  Document d;
  d.add_layer("base", LayerRole::Base);
  for (u32 i = 0; i < count; ++i)
    REQUIRE(d.apply(cmd_create(id_of(i), k_prov), nullptr, nullptr));
  return d;
}

Vector<ObjectId> changed(const Document& d, u64 since) {
  Vector<ObjectId> out;
  REQUIRE(d.changed_since(since, out));
  return out;
}

}  // namespace

TEST_CASE("doc: every change stamps the id it touched, and the feed says which since when") {
  Document d = document_of(4);
  const u64 after_create = d.revision();
  CHECK(after_create >= 4);
  CHECK(d.revision_of(id_of(0)) < d.revision_of(id_of(3)));
  CHECK(d.revision_of(id_of(99)) == 0);
  CHECK(changed(d, after_create).empty());

  // Edited twice, deleted, and a fresh one: each id once, in id order, the deleted one included,
  // because a reader has to hear that it went.
  REQUIRE(d.apply(cmd_set(id_of(2), "generator", JsonValue("a")), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set(id_of(2), "generator", JsonValue("b")), nullptr, nullptr));
  REQUIRE(d.apply(cmd_delete(id_of(1)), nullptr, nullptr));
  REQUIRE(d.apply(cmd_create(id_of(9), k_prov), nullptr, nullptr));
  const Vector<ObjectId> since = changed(d, after_create);
  REQUIRE(since.size() == 3);
  CHECK(since[0] == id_of(1));
  CHECK(since[1] == id_of(2));
  CHECK(since[2] == id_of(9));
  CHECK(d.revision_of(id_of(2)) > after_create);
  CHECK(d.revision_of(id_of(0)) <= after_create);
  // A reader that has seen everything sees nothing.
  CHECK(changed(d, d.revision()).empty());
}

TEST_CASE("doc: a transaction, its undo and redo move the feed without resetting it") {
  Document d = document_of(3);
  const u64 floor = d.feed_floor();
  const u64 before = d.revision();
  {
    Transaction t = d.begin(who());
    REQUIRE(t.apply(cmd_set(id_of(1), "generator", JsonValue("x"))));
    REQUIRE(t.commit());
  }
  // Beginning a transaction used to take the mutable layer accessor, which marked the index for a
  // rebuild; with the feed that became a reset that sent every reader back to a full pass.
  CHECK(d.feed_floor() == floor);
  Vector<ObjectId> one = changed(d, before);
  REQUIRE(one.size() == 1);
  CHECK(one[0] == id_of(1));
  const u64 committed = d.revision();
  REQUIRE(d.undo(d.journal().back()));
  one = changed(d, committed);
  REQUIRE(one.size() == 1);
  CHECK(one[0] == id_of(1));
  CHECK(d.feed_floor() == floor);
}

TEST_CASE("doc: after a rebuild the feed cannot answer, and every id is newer than it was") {
  Document d = document_of(3);
  const u64 before = d.revision();
  (void)d.layer(0);  // the mutable accessor: the document can no longer say what changed
  Vector<ObjectId> out;
  CHECK_FALSE(d.changed_since(before, out));
  CHECK(out.empty());
  CHECK(d.feed_floor() > before);
  for (u32 i = 0; i < 3; ++i)
    CHECK(d.revision_of(id_of(i)) > before);
  // From the new floor on it answers again.
  const u64 now = d.revision();
  REQUIRE(d.apply(cmd_set(id_of(0), "generator", JsonValue("y")), nullptr, nullptr));
  CHECK(changed(d, now).size() == 1);
}

TEST_CASE("doc: the feed is bounded, and a reader that fell behind is told to resynchronize") {
  Document d = document_of(8);
  const u64 early = d.revision();
  // 5,000 edits to one small document: past the 4,096-entry floor of the bound.
  for (u32 i = 0; i < 5000; ++i)
    REQUIRE(
        d.apply(cmd_set(id_of(i % 8), "seed", JsonValue(static_cast<u64>(i))), nullptr, nullptr));
  CHECK(d.feed_floor() > early);
  Vector<ObjectId> out;
  CHECK_FALSE(d.changed_since(early, out));
  // A reader that kept up is answered from what is left.
  const u64 recent = d.revision() - 3;
  CHECK(d.changed_since(recent, out));
  CHECK_FALSE(out.empty());
  CHECK(d.validate_index());
}

TEST_CASE("doc: type, parent and records of one id without composing its properties") {
  Document d;
  d.add_layer("base", LayerRole::Base);
  d.add_layer("over", LayerRole::Feature);
  REQUIRE(d.apply(cmd_create(id_of(0), k_prov), nullptr, nullptr));
  JsonValue initial = JsonValue::object();
  initial.set("generator", JsonValue("base"));
  REQUIRE(d.apply(cmd_create(id_of(1), k_prov, id_of(0), std::move(initial)), nullptr, nullptr));
  d.set_edit_layer(1);
  REQUIRE(d.apply(cmd_set(id_of(1), "generator", JsonValue("over")), nullptr, nullptr));

  CHECK(d.type_of(id_of(1)) == k_prov);
  CHECK(d.type_of(id_of(42)).empty());
  CHECK(d.parent_of(id_of(1)) == id_of(0));
  CHECK(d.parent_of(id_of(0)).is_null());

  // Weakest first, so the last value a visitor sees for a property is the composed one.
  Vector<u32> layers;
  std::string last;
  d.visit_records(id_of(1), [&](u32 layer, const ObjectRecord& r) {
    layers.push_back(layer);
    if (const JsonValue* v = r.properties.find_value("generator"))
      last = std::string(v->as_string());
  });
  REQUIRE(layers.size() == 2);
  CHECK(layers[0] == 0);
  CHECK(layers[1] == 1);
  CHECK(last == "over");
  CHECK(last == d.property(id_of(1), "generator")->as_string());
}
