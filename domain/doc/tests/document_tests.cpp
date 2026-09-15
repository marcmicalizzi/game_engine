#include <domain/doc/document.h>

#include <core/json/json.h>
#include <schemas/provenance.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::doc;

namespace {

const char* const k_prov = "engine.content.AssetProvenance";

ObjectId id_of(u64 n) { return Id128::from_seed(1, n); }

Attribution who(const char* actor) {
  Attribution a;
  a.actor = actor;
  a.role = "test";
  a.task = "unit";
  return a;
}

}  // namespace

TEST_CASE("doc: create, set, resolve, children") {
  Document d;
  d.add_layer("base", LayerRole::Base);
  const ObjectId root = id_of(1), child = id_of(2);
  Vector<Diagnostic> diags;
  REQUIRE(d.apply(cmd_create(root, k_prov), nullptr, &diags));
  REQUIRE(d.apply(cmd_create(child, k_prov, root), nullptr, &diags));
  REQUIRE(d.apply(cmd_set(root, "generator", JsonValue("hand")), nullptr, &diags));
  CHECK(diags.empty());

  CHECK(d.exists(root));
  CHECK(d.is_defined(child));
  CHECK_FALSE(d.exists(id_of(99)));
  ResolvedObject r;
  REQUIRE(d.resolve(child, r));
  CHECK(r.type == k_prov);
  CHECK(r.parent == root);
  CHECK(r.defining_layer == 0);
  CHECK_FALSE(r.deleted);
  REQUIRE(d.property(root, "generator") != nullptr);
  CHECK(d.property(root, "generator")->as_string() == "hand");
  CHECK(d.property(root, "missing") == nullptr);
  CHECK(d.property(id_of(99), "generator") == nullptr);

  CHECK(d.objects().size() == 2);
  CHECK(d.children(ObjectId{}) == Vector<ObjectId>{root});
  CHECK(d.children(root) == Vector<ObjectId>{child});

  // Initial properties on create.
  JsonValue initial = JsonValue::object();
  initial["license"] = "MIT";
  REQUIRE(d.apply(cmd_create(id_of(3), k_prov, root, std::move(initial)), nullptr, &diags));
  CHECK(d.property(id_of(3), "license")->as_string() == "MIT");
}

TEST_CASE("doc: layering overrides, tombstones, and their removal") {
  Document d;
  const u32 base = d.add_layer("base", LayerRole::Base);
  const u32 feature = d.add_layer("feature", LayerRole::Feature);
  const ObjectId a = id_of(10);

  d.set_edit_layer(base);
  REQUIRE(d.apply(cmd_create(a, k_prov), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set(a, "generator", JsonValue("base-gen")), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set(a, "seed", JsonValue(u64{1})), nullptr, nullptr));

  d.set_edit_layer(feature);
  REQUIRE(d.apply(cmd_set(a, "generator", JsonValue("feature-gen")), nullptr, nullptr));
  CHECK(d.property(a, "generator")->as_string() == "feature-gen");  // strongest wins
  CHECK(d.property(a, "seed")->as_uint() == 1);                       // untouched property shows through
  CHECK(d.layer(feature).find(a)->type.empty());                     // override-only record

  // Clearing the override reveals the base value and prunes the empty record.
  REQUIRE(d.apply(cmd_clear(a, "generator"), nullptr, nullptr));
  CHECK(d.property(a, "generator")->as_string() == "base-gen");
  CHECK(d.layer(feature).find(a) == nullptr);

  // Deleting from the feature layer tombstones; removing the record restores.
  REQUIRE(d.apply(cmd_delete(a), nullptr, nullptr));
  CHECK_FALSE(d.exists(a));
  CHECK(d.is_defined(a));
  CHECK(d.objects().empty());
  CHECK(d.layer(feature).find(a)->deleted);
  REQUIRE(d.apply(cmd_remove_record(a), nullptr, nullptr));
  CHECK(d.exists(a));

  // Deleting from the defining layer drops the definition.
  d.set_edit_layer(base);
  REQUIRE(d.apply(cmd_delete(a), nullptr, nullptr));
  CHECK_FALSE(d.is_defined(a));
  CHECK(d.layer(base).find(a) == nullptr);

  // Reparenting through a stronger layer.
  d.set_edit_layer(base);
  const ObjectId p = id_of(11), q = id_of(12);
  REQUIRE(d.apply(cmd_create(p, k_prov), nullptr, nullptr));
  REQUIRE(d.apply(cmd_create(q, k_prov), nullptr, nullptr));
  d.set_edit_layer(feature);
  REQUIRE(d.apply(cmd_set_parent(q, p), nullptr, nullptr));
  ResolvedObject r;
  REQUIRE(d.resolve(q, r));
  CHECK(r.parent == p);
  CHECK(d.children(p) == Vector<ObjectId>{q});
}

TEST_CASE("doc: preconditions fail cleanly with diagnostics and change nothing") {
  Document d;
  d.add_layer("base", LayerRole::Base);
  const ObjectId a = id_of(20), b = id_of(21);
  REQUIRE(d.apply(cmd_create(a, k_prov), nullptr, nullptr));
  REQUIRE(d.apply(cmd_create(b, k_prov, a), nullptr, nullptr));
  const Layer snapshot = d.layer(0);

  Vector<Diagnostic> diags;
  CHECK_FALSE(d.apply(cmd_create(a, k_prov), nullptr, &diags));                 // duplicate
  CHECK_FALSE(d.apply(cmd_create(id_of(22), ""), nullptr, &diags));             // no type
  CHECK_FALSE(d.apply(cmd_create(id_of(22), k_prov, id_of(99)), nullptr, &diags));  // missing parent
  CHECK_FALSE(d.apply(cmd_set(id_of(99), "x", JsonValue(i64{1})), nullptr, &diags));  // missing object
  CHECK_FALSE(d.apply(cmd_set(a, "", JsonValue(i64{1})), nullptr, &diags));    // no name
  CHECK_FALSE(d.apply(cmd_set_parent(a, b), nullptr, &diags));                 // cycle: b's parent is a
  CHECK_FALSE(d.apply(cmd_set_parent(a, a), nullptr, &diags));                 // self
  CHECK_FALSE(d.apply(cmd_delete(id_of(99)), nullptr, &diags));                // missing
  CHECK_FALSE(d.apply(cmd_set(ObjectId{}, "x", JsonValue()), nullptr, &diags));  // null id
  CHECK(diags.size() == 9);
  CHECK(diags[0].message == "object already exists");
  CHECK(diags[5].message == "parent would create a cycle");
  CHECK(diags[0].path.find("base/") == 0);
  CHECK(d.layer(0) == snapshot);

  // Non-strict application skips the checks (used for diffs).
  CHECK(d.apply(cmd_set(id_of(99), "x", JsonValue(i64{1})), nullptr, nullptr, false));
  CHECK(d.layer(0).find(id_of(99)) != nullptr);
}

TEST_CASE("doc: transactions commit with inverses, roll back, undo, and redo") {
  Document d;
  d.add_layer("base", LayerRole::Base);
  const ObjectId a = id_of(30), b = id_of(31);
  REQUIRE(d.apply(cmd_create(a, k_prov), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set(a, "generator", JsonValue("before")), nullptr, nullptr));
  const Layer before = d.layer(0);

  {
    Transaction tx = d.begin(who("agent-1"));
    CHECK(tx.apply(cmd_set(a, "generator", JsonValue("after"))));
    CHECK(tx.apply(cmd_create(b, k_prov, a)));
    CHECK(tx.apply(cmd_set(b, "license", JsonValue("Apache-2.0"))));
    CHECK_FALSE(tx.apply(cmd_set(id_of(99), "x", JsonValue())));  // fails, transaction continues
    CHECK_FALSE(tx.ok());
    CHECK(tx.diagnostics().size() == 1);
    CHECK(tx.patch().forward.size() == 3);
    CHECK(tx.patch().inverse.size() == 3);
    CHECK(tx.commit());
    CHECK_FALSE(tx.commit());  // once
  }
  REQUIRE(d.journal().size() == 1);
  const Patch& patch = d.journal()[0];
  CHECK(patch.attribution.actor == "agent-1");
  CHECK(patch.attribution.timestamp_unix_ms > 0);
  CHECK(patch.layer == "base");
  CHECK(patch.inverse[0].kind == CommandKind::RestoreRecord);
  REQUIRE(patch.inverse[0].record.has_value());
  CHECK(patch.inverse[0].record->properties.find_value("generator")->as_string() == "before");
  CHECK_FALSE(patch.inverse[1].record.has_value());  // b did not exist: inverse removes it
  CHECK(d.property(a, "generator")->as_string() == "after");
  CHECK(d.exists(b));

  const Layer after = d.layer(0);
  CHECK(d.undo(patch));
  CHECK(d.layer(0) == before);
  CHECK(d.redo(patch));
  CHECK(d.layer(0) == after);

  // Rollback on scope exit.
  {
    Transaction tx = d.begin(who("agent-2"));
    CHECK(tx.apply(cmd_delete(b)));
    CHECK(tx.apply(cmd_set(a, "seed", JsonValue(u64{5}))));
    CHECK_FALSE(d.exists(b));
  }
  CHECK(d.layer(0) == after);
  CHECK(d.journal().size() == 1);

  // Explicit rollback, then an empty commit records nothing.
  {
    Transaction tx = d.begin(who("agent-3"));
    CHECK(tx.apply(cmd_set(a, "seed", JsonValue(u64{6}))));
    tx.rollback();
    CHECK(d.layer(0) == after);
    CHECK(tx.commit() == false);
  }
  {
    Transaction tx = d.begin(who("agent-4"));
    CHECK(tx.commit());
  }
  CHECK(d.journal().size() == 1);

  // Patches serialize through the schema.
  const std::string text = write_json(schema::to_json(patch));
  JsonValue parsed;
  REQUIRE(parse_json(text, parsed).ok);
  Patch back;
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(back, parsed, ctx));
  CHECK(back == patch);
}

TEST_CASE("doc: structural diff transforms one layer into another") {
  Layer from("l", LayerRole::Base);
  Layer to("l", LayerRole::Base);
  // Shared object with property changes.
  {
    ObjectRecord r;
    r.id = id_of(40);
    r.type = k_prov;
    r.parent = ObjectId{};
    r.properties.insert("generator", JsonValue("a"));
    r.properties.insert("seed", JsonValue(u64{1}));
    r.properties.insert("gone", JsonValue(true));
    from.set(r);
    r.properties.erase("gone");
    r.properties.insert_or_assign("generator", JsonValue("b"));
    r.properties.insert("license", JsonValue("MIT"));
    r.parent = id_of(41);
    to.set(r);
  }
  // Only in `from`.
  {
    ObjectRecord r;
    r.id = id_of(42);
    r.type = k_prov;
    from.set(r);
  }
  // Only in `to`.
  {
    ObjectRecord r;
    r.id = id_of(41);
    r.type = k_prov;
    to.set(r);
  }
  // Type change forces a whole-record restore.
  {
    ObjectRecord r;
    r.id = id_of(43);
    r.type = k_prov;
    from.set(r);
    r.type = "engine.doc.Attribution";
    to.set(r);
  }

  const Vector<Command> forward = diff_layers(from, to);
  CHECK(forward.size() == 7);  // set_parent, clear gone, set generator, set license, restore 41, remove 42, restore 43
  Document d;
  d.add_layer(Layer(from));
  for (const Command& c : forward) REQUIRE(d.apply(c, nullptr, nullptr, false));
  CHECK(d.layer(0) == to);

  const Vector<Command> backward = diff_layers(to, from);
  for (const Command& c : backward) REQUIRE(d.apply(c, nullptr, nullptr, false));
  CHECK(d.layer(0) == from);

  CHECK(diff_layers(from, from).empty());
}

TEST_CASE("doc: layers save to canonical JSON and load back identically") {
  Layer layer("world/base", LayerRole::Base);
  ObjectRecord r;
  r.id = Id128::from_parts(0x0000000000000001ull, 0x0000000000000002ull);
  r.type = k_prov;
  r.parent = ObjectId{};
  r.properties.insert("seed", JsonValue(u64{42}));
  r.properties.insert("generator", JsonValue("proc"));
  layer.set(r);
  ObjectRecord o;
  o.id = Id128::from_parts(0x0000000000000001ull, 0x0000000000000001ull);
  o.deleted = true;
  layer.set(o);

  const std::string text = layer.to_json_text();
  const std::string expected =
      "{\n"
      "  \"name\": \"world/base\",\n"
      "  \"objects\": [\n"
      "    {\n"
      "      \"deleted\": true,\n"
      "      \"id\": \"00000000000000010000000000000001\",\n"
      "      \"parent\": null,\n"
      "      \"properties\": {},\n"
      "      \"type\": \"\"\n"
      "    },\n"
      "    {\n"
      "      \"deleted\": false,\n"
      "      \"id\": \"00000000000000010000000000000002\",\n"
      "      \"parent\": \"00000000000000000000000000000000\",\n"
      "      \"properties\": {\n"
      "        \"generator\": \"proc\",\n"
      "        \"seed\": 42\n"
      "      },\n"
      "      \"type\": \"engine.content.AssetProvenance\"\n"
      "    }\n"
      "  ],\n"
      "  \"role\": \"Base\"\n"
      "}";
  CHECK(text == expected);

  Layer back("", LayerRole::Session);
  schema::ReadContext ctx;
  REQUIRE(Layer::from_json_text(text, back, ctx));
  CHECK(back == layer);
  CHECK(back.to_json_text() == text);

  schema::ReadContext bad;
  CHECK_FALSE(Layer::from_json_text("{ nope", back, bad));
  CHECK(bad.diagnostics.size() == 1);
}

TEST_CASE("doc: validation against the schema registry") {
  Document d;
  const u32 base = d.add_layer("base", LayerRole::Base);
  const u32 top = d.add_layer("top", LayerRole::Feature);
  const ObjectId good = id_of(50), bad_type = id_of(51), orphan = id_of(52);
  d.set_edit_layer(base);
  JsonValue props = JsonValue::object();
  props["generator"] = "gen";
  props["seed"] = u64{7};
  props["license_class"] = "Permissive";
  REQUIRE(d.apply(cmd_create(good, k_prov, ObjectId{}, std::move(props)), nullptr, nullptr));
  Vector<Diagnostic> diags;
  CHECK(d.validate(diags));
  CHECK(diags.empty());

  // Unknown type, unknown property, wrong value type, transient property, missing parent, dangling override, duplicate definition.
  REQUIRE(d.apply(cmd_create(bad_type, "engine.nope.Type"), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set(good, "not_a_field", JsonValue(i64{1})), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set(good, "seed", JsonValue("not a number")), nullptr, nullptr));
  REQUIRE(d.apply(cmd_set_parent(bad_type, id_of(99)), nullptr, nullptr, false));
  d.set_edit_layer(top);
  REQUIRE(d.apply(cmd_set(orphan, "generator", JsonValue("x")), nullptr, nullptr, false));
  REQUIRE(d.apply(cmd_create(good, k_prov), nullptr, nullptr, false));  // second definition

  CHECK_FALSE(d.validate(diags));
  std::string joined;
  for (const Diagnostic& x : diags) joined += x.path + ": " + x.message + "\n";
  MESSAGE(joined);
  CHECK(joined.find("unknown object type 'engine.nope.Type'") != std::string::npos);
  CHECK(joined.find("/not_a_field: unknown property") != std::string::npos);
  CHECK(joined.find("/seed: expected an integer") != std::string::npos);
  CHECK(joined.find("parent does not exist") != std::string::npos);
  CHECK(joined.find("override record for an object no layer defines") != std::string::npos);
  CHECK(joined.find("defined in more than one layer") != std::string::npos);
}
