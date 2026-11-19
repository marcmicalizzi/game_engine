// A command naming a former name (roadmap D41; docs/subsystems/doc.md, "A command that names a
// former name"): a client's (strict) is refused, the message naming what to write instead; the
// engine's own replay of a journal written before the rename is renamed and applied. The type is
// `doc_test_types.schema`'s `Marker` (formerly `Beacon`, its `label` formerly `title`; `Glow`'s
// `strength` formerly `power`).
#include <core/json/json.h>
#include <domain/doc/document.h>

#include <doctest/doctest.h>

#include <string>
#include <utility>

using namespace engine;
using namespace engine::doc;

namespace {
JsonValue json(std::string_view text) {
  JsonValue v;
  REQUIRE(parse_json(text, v).ok);
  return v;
}

ObjectRecord record(ObjectId id, std::string type, std::string_view properties) {
  ObjectRecord r;
  r.id = id;
  r.type = std::move(type);
  const JsonValue p = json(properties);
  for (auto [key, value] : p.as_object())
    r.properties.insert_or_assign(key, value);
  return r;
}
Attribution who() {
  Attribution a;
  a.actor = "author";
  a.role = "director";
  a.task = "t1";
  a.rationale = "D41";
  a.timestamp_unix_ms = 1000;
  return a;
}
const ObjectId k_gate = Id128::from_parts(0xD41, 1);
const ObjectId k_well = Id128::from_parts(0xD41, 2);

}  // namespace

TEST_CASE("former names: a client's command is refused with the current name, a replay renamed") {
  Document doc;
  doc.add_layer("base", LayerRole::Base);
  doc.layer(0).set(
      record(k_gate, "engine.doc.test.Marker", R"({"label": "gate", "position": [1, 0, 2]})"));

  // A client generated from the schema before the rename (strict): refused, naming what to write.
  const auto refusal = [&](const Command& c) {
    Transaction tx = doc.begin(who());
    CHECK_FALSE(tx.apply(c, true));
    REQUIRE(!tx.diagnostics().empty());
    std::string message = tx.diagnostics().front().message;
    tx.rollback();
    return message;
  };
  CHECK(refusal(cmd_set(k_gate, "title", JsonValue("north"))) ==
        "'title' is a former name of engine.doc.test.Marker's 'label': write 'label'");
  CHECK(refusal(cmd_clear(k_gate, "title")).find("write 'label'") != std::string::npos);
  CHECK(refusal(cmd_set(k_gate, "glow", json(R"({"power": 3})"))) ==
        "the value of 'glow' names a field by a former name: write {\"strength\":3}");
  CHECK(refusal(cmd_create(k_well, "engine.doc.test.Beacon", {}, json(R"({"label": "well"})"))) ==
        "'engine.doc.test.Beacon' is a former name of 'engine.doc.test.Marker': write "
        "'engine.doc.test.Marker'");
  CHECK(refusal(cmd_create(k_well, "engine.doc.test.Marker", {}, json(R"({"title": "well"})")))
            .find("write 'label'") != std::string::npos);
  {
    const Document& read = doc;
    CHECK(read.layer(0).find(k_gate)->properties.find_value("title") == nullptr);
    CHECK_FALSE(read.exists(k_well));
  }

  // The engine's own replay of a patch journaled before the rename (not strict): renamed and
  // applied, so the record never holds the field under two names.
  Patch old;
  old.layer = "base";
  old.forward.push_back(cmd_set(k_gate, "title", JsonValue("north")));
  old.forward.push_back(cmd_create(k_well, "engine.doc.test.Beacon", {},
                                   json(R"({"title": "well", "glow": {"power": 1}})")));
  REQUIRE(doc.redo(old));
  const Document& read = doc;
  const ObjectRecord* gate = read.layer(0).find(k_gate);
  REQUIRE(gate != nullptr);
  CHECK(gate->properties.find_value("title") == nullptr);
  REQUIRE(gate->properties.find_value("label") != nullptr);
  CHECK(*gate->properties.find_value("label") == JsonValue("north"));
  const ObjectRecord* well = read.layer(0).find(k_well);
  REQUIRE(well != nullptr);
  CHECK(well->type == "engine.doc.test.Marker");
  CHECK(well->properties.find_value("label") != nullptr);
  REQUIRE(well->properties.find_value("glow") != nullptr);
  CHECK(well->properties.find_value("glow")->find("strength") != nullptr);
  Vector<Diagnostic> diagnostics;
  CHECK(doc.validate(diagnostics));
}
