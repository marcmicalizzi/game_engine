// A checkpoint written before a rename reads in the new names (roadmap D41, after D28;
// docs/subsystems/doc.md, "A renamed type or field"): its overrides are renamed by the type its own
// stack composes for them, as a loaded document's are, so `doc.rollback_to` diffs like with like.
// The document is written as a build before the rename wrote it — records set through the layer,
// which renames nothing. The type is `doc_test_types.schema`'s `Marker` (formerly `Beacon`, its
// `label` formerly `title`; `Glow`'s `strength` formerly `power`).
#include <core/json/json.h>
#include <domain/doc/checkpoint.h>
#include <domain/doc/document_store.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

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
const JsonValue* property(const Layer& layer, ObjectId id, std::string_view name) {
  const ObjectRecord* r = layer.find(id);
  return r != nullptr ? r->properties.find_value(name) : nullptr;
}
const ObjectId k_gate = Id128::from_parts(0xD41, 1);

}  // namespace

TEST_CASE("former names: a checkpoint's overrides are renamed before a rollback diffs them") {
  std::string version, error;
  if (!Checkpoints::available(&version, &error)) {
    MESSAGE("no git on this machine, so the checkpoint case does not run: " << error);
    return;
  }
  test::TempDir tmp("engine_doc_former_checkpoint");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  const u32 notes = doc.add_layer("notes", LayerRole::Feature);
  // As the build before the rename wrote it: the type and the field under their former names, and
  // an override in a stronger layer naming the field the same way.
  doc.layer(0).set(record(k_gate, "engine.doc.test.Beacon",
                          R"({"title": "gate", "position": [1, 0, 2], "glow": {"power": 2}})"));
  doc.layer(notes).set(record(k_gate, "", R"({"title": "the north gate"})"));
  REQUIRE_MESSAGE(DocumentStore::save(vfs, dir, doc, manifest, &error), error);
  CheckpointInfo taken;
  REQUIRE_MESSAGE(
      Checkpoints::take(vfs, dir, doc, manifest, "before the rename", who(), taken, &error), error);

  CheckpointState state;
  REQUIRE_MESSAGE(Checkpoints::read(vfs, dir, taken.id, state, &error), error);
  REQUIRE(state.layers.size() == 2);
  const ObjectRecord* defined = state.layers[0].find(k_gate);
  REQUIRE(defined != nullptr);
  CHECK(defined->type == "engine.doc.test.Marker");
  CHECK(property(state.layers[0], k_gate, "label") != nullptr);
  // The override: renamed by the type the checkpoint's own stack composes for it.
  CHECK(property(state.layers[1], k_gate, "title") == nullptr);
  REQUIRE(property(state.layers[1], k_gate, "label") != nullptr);
  CHECK(*property(state.layers[1], k_gate, "label") == JsonValue("the north gate"));
  CHECK(state.kept_names.empty());

  // What a rollback does: the loaded document, renamed by its load, diffed against the checkpoint.
  // Like with like, so nothing differs and no old name is written back.
  Document loaded;
  DocumentManifest loaded_manifest;
  REQUIRE_MESSAGE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error), error);
  const Document& read = loaded;
  for (u32 l = 0; l < 2; ++l)
    CHECK(diff_layers(read.layer(l), state.layers[l]).empty());
}
