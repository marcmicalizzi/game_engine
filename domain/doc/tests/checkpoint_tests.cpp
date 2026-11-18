// Checkpoints as commits of the document directory's own repository (checkpoint.h;
// docs/subsystems/doc.md, "Checkpoints"), through the git on PATH: taken, listed newest first with
// their attribution and journal positions, found by an abbreviated name and refused for a name that
// is not one of this document's, and read back — a single-file layer and a tile-partitioned one —
// record for record as they were saved, with a blob the checkpoint holds read back by name after the
// store collected it. A machine with no git skips every case, saying so; `ENGINE_GIT` naming nothing
// runnable is the unavailable path.
#include <domain/doc/blob_store.h>
#include <domain/doc/checkpoint.h>
#include <domain/doc/document_store.h>

#include <core/schema/json_reflect.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::doc;

using TempDir = test::TempDir;

namespace {

Attribution who(const char* actor, const char* why) {
  Attribution a;
  a.actor = actor;
  a.role = "director";
  a.task = "t1";
  a.rationale = why;
  a.timestamp_unix_ms = 1000;
  return a;
}

bool have_git() {
  std::string version, error;
  if (Checkpoints::available(&version, &error)) return true;
  MESSAGE("no git on this machine, so no checkpoint case runs: " << error);
  return false;
}

JsonValue position(f64 x, f64 z) {
  JsonValue p = JsonValue::array();
  p.push_back(JsonValue(x));
  p.push_back(JsonValue(0.0));
  p.push_back(JsonValue(z));
  return p;
}

// One transaction on the edit layer, journaled and saved as a session does.
void commit(const io::Vfs& vfs, const std::string& dir, Document& doc, DocumentManifest& manifest,
            const Command& c) {
  std::string error;
  doc.truncate_journal(manifest.undo_position);
  Transaction tx = doc.begin(who("author", "edit"));
  REQUIRE(tx.apply(c, false));
  REQUIRE(tx.commit());
  manifest.undo_position = doc.journal().size();
  REQUIRE(DocumentStore::append_journal(vfs, dir, doc, &error));
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
}

}  // namespace

TEST_CASE("checkpoints: taken, listed, found, and read back as they were saved") {
  if (!have_git()) return;
  TempDir tmp("engine_doc_checkpoint");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;
  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  const u32 tiles = doc.add_layer("placements", LayerRole::Feature);
  LayerPartition grid;
  grid.property = "position";
  grid.tile_size = 32.0;
  doc.set_layer_partition(tiles, grid);
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));

  Vector<CheckpointInfo> listed;
  REQUIRE(Checkpoints::list(vfs, dir, 0, listed, &error));
  CHECK(listed.empty());  // no repository yet

  const ObjectId a = Id128::from_parts(3, 1), b = Id128::from_parts(3, 2), f = Id128::from_parts(3, 3);
  doc.set_edit_layer(tiles);
  JsonValue pa = JsonValue::object();
  pa.set("position", position(5, 5));
  commit(vfs, dir, doc, manifest, cmd_create(a, "engine.doc.test.Placement", {}, std::move(pa)));
  JsonValue pb = JsonValue::object();
  pb.set("position", position(100, -40));
  commit(vfs, dir, doc, manifest, cmd_create(b, "engine.doc.test.Placement", {}, std::move(pb)));
  doc.set_edit_layer(0);
  BlobRef ref;
  REQUIRE(BlobStore::put(vfs, dir, std::string(4096, 'q'), "test", ref, nullptr, &error));
  JsonValue pf = JsonValue::object();
  pf.set("claim", JsonValue("the river floods in spring"));
  pf.set("map", schema::to_json(ref));
  commit(vfs, dir, doc, manifest, cmd_create(f, "engine.doc.test.Fact", {}, std::move(pf)));

  // Through the const accessor: the mutable one marks the layer changed, and a checkpoint refuses
  // a document with changes that are not saved.
  const Document& read = doc;
  const Layer base_then = read.layer(0);
  const Layer tiles_then = read.layer(tiles);
  CheckpointInfo first;
  REQUIRE_MESSAGE(Checkpoints::take(vfs, dir, doc, manifest, "three objects",
                                    who("ann", "before the flood"), first, &error),
                  error);
  CHECK(first.id.size() == 40);
  CHECK(first.parent.empty());
  CHECK(first.journal_from == 0);
  CHECK(first.undo_position == 3);
  CHECK(Checkpoints::initialized(vfs, dir));

  // Change everything: move a, delete b, clear the map; then a second checkpoint.
  doc.set_edit_layer(tiles);
  commit(vfs, dir, doc, manifest, cmd_set(a, "position", position(-70, 300)));
  commit(vfs, dir, doc, manifest, cmd_delete(b));
  doc.set_edit_layer(0);
  commit(vfs, dir, doc, manifest, cmd_clear(f, "map"));
  CheckpointInfo second;
  REQUIRE(Checkpoints::take(vfs, dir, doc, manifest, "after the flood\nsecond line",
                            who("bo", "the flood"), second, &error));
  CHECK(second.parent == first.id);
  CHECK(second.journal_from == 3);
  CHECK(second.label == "after the flood second line");

  REQUIRE(Checkpoints::list(vfs, dir, 0, listed, &error));
  REQUIRE(listed.size() == 2);
  CHECK(listed[0].id == second.id);
  CHECK(listed[1].id == first.id);
  CHECK(listed[1].label == "three objects");
  CHECK(listed[1].attribution.actor == "ann");
  CHECK(listed[1].attribution.role == "director");
  CHECK(listed[1].attribution.task == "t1");
  CHECK(listed[1].attribution.rationale == "before the flood");
  CHECK(listed[1].attribution.timestamp_unix_ms == 1000);
  CHECK(listed[1].undo_position == 3);
  CHECK(listed[1].journal_length == 3);
  CHECK(listed[0].journal_from == 3);
  CHECK(listed[0].undo_position == 6);
  REQUIRE(Checkpoints::list(vfs, dir, 1, listed, &error));
  CHECK(listed.size() == 1);

  CheckpointInfo found;
  REQUIRE(Checkpoints::find(vfs, dir, first.id.substr(0, 10), found, &error));
  CHECK(found.id == first.id);
  CHECK_FALSE(Checkpoints::find(vfs, dir, "zz", found, &error));
  CHECK_FALSE(Checkpoints::find(vfs, dir, "0123456789abcdef", found, &error));
  CHECK(error.find("no checkpoint") != std::string::npos);

  // Read back: the first checkpoint's layers, record for record, the tiles included.
  CheckpointState state;
  REQUIRE(Checkpoints::read(vfs, dir, first.id, state, &error));
  CHECK(state.manifest.undo_position == 3);
  REQUIRE(state.layers.size() == 2);
  CHECK(state.layers[0] == base_then);
  CHECK(state.layers[1] == tiles_then);
  CHECK(state.layers[1].partitioned());
  CHECK(state.layers[1].partition().tile_size == 32.0);

  // The blob the first checkpoint held, after the store collected it.
  BlobCollectOptions now;
  now.min_age_ms = 0;
  BlobCollectReport report;
  doc.truncate_journal(0);  // nothing reaches it any more
  REQUIRE(BlobStore::collect(vfs, dir, doc, {}, now, report, &error));
  CHECK_FALSE(BlobStore::has(vfs, dir, ref.blob));
  Vector<std::pair<std::string, std::string>> blobs;
  const std::string names[] = {ref.blob, BlobStore::name_of("not held")};
  REQUIRE(Checkpoints::read_blobs(vfs, dir, first.id, names, blobs, &error));
  REQUIRE(blobs.size() == 1);
  CHECK(blobs[0].first == ref.blob);
  CHECK(blobs[0].second == std::string(4096, 'q'));

  // Nothing of the protocol's or the journal's is in a checkpoint: only the document's files.
  REQUIRE(vfs.write(DocumentStore::path_of(dir, "leases.json"), "{}\n") == io::Status::Ok);
  CheckpointInfo third;
  REQUIRE(Checkpoints::take(vfs, dir, doc, manifest, "with a side file", who("cy", ""), third,
                            &error));
  REQUIRE(Checkpoints::read(vfs, dir, third.id, state, &error));
  CHECK(state.layers.size() == 2);
}

TEST_CASE("checkpoints: refused on a document with unsaved changes, and without git") {
  if (!have_git()) return;
  TempDir tmp("engine_doc_checkpoint_refused");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;
  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  doc.add_layer("more", LayerRole::Feature);
  CheckpointInfo info;
  CHECK_FALSE(Checkpoints::take(vfs, dir, doc, manifest, "x", who("a", ""), info, &error));
  CHECK(error.find("not saved") != std::string::npos);
  CHECK_FALSE(Checkpoints::initialized(vfs, dir));
}
