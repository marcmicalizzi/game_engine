#pragma once

// A save game (docs/subsystems/world.md, "Save and load"; docs/plan/03-data-model.md §3.5, §3.8;
// ADR-0042): the persistent state of a running world and the drivers of the run, as one directory
// of files with a manifest that names and hashes every one of them.
//
//     <save>/save.json            the manifest (`engine.world.SaveManifest`), written last
//     <save>/document/...         the session's document directory, as its store wrote it
//     <save>/world.db             the persistent store, a `VACUUM INTO` backup (store.md,
//     "Backups") <save>/input.jsonl          the player's input log up to the save's tick, when
//     there is one <save>/input-map.json       and the action map it was recorded against
//
// **State and derived, and where the line is.** State is what nothing else can rebuild: the
// document (its layers, which hold everything the world wrote back, and its journal); the store's
// three tables (the event log, the projections a tile wrote when it went inactive — written
// directly, not folded from the log, so they are not derivable from it today — and the snapshots);
// the clock; the world seed; the tile ring's observers and the tiles it held, which hysteresis and
// the budget make a function of the observers' history rather than of where they are; and the
// input log so far. Derived is everything a load rebuilds from those: the runtime world's entities
// and components (a materialization of the document at the saved tiles), each tile's seed (the
// world seed hashed with the tile), the reconciliation a tile ran when it came in (read again from
// the same snapshot and projections), the input state (a fold of the log), and the document's
// composed index. The plan's "latest snapshot plus events since" is the store's own business
// inside `world.db`: the save carries the whole database, because the projections are not a fold
// of the log yet and `session.events` reads the log's history.
//
// **Written last, verified first.** `write_save` writes every file, then the manifest, so a
// directory without a `save.json` is a save that did not finish. `read_save` refuses, before a load
// touches anything, a save that names a format version newer than this build's (or a format this
// build lacks), and one whose files are not the sizes and hashes the manifest says, naming the
// file.
//
// **Older versions migrate on read.** The manifest's own types follow the schema language's
// `@since` rule: a field introduced after the version the save was written at is refused if present
// and takes its default if absent, and an unknown field is refused, as `core/schema` refuses one
// everywhere. The store's tables migrate when the store is opened (`store.tables`, the store's
// `user_version`), a step per version. The document's records take their type's default for any
// field their type gained, which the materialization driver already does.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/doc/document.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>
#include <foundation/store/database.h>
#include <systems/world/input_observer.h>

#include <schemas/world_save.h>
#include <string>
#include <string_view>

namespace engine::world {

inline constexpr const char* k_save_format = "engine.save";
inline constexpr const char* k_save_manifest_file = "save.json";
inline constexpr const char* k_save_document_dir = "document";
inline constexpr const char* k_save_store_file = "world.db";
inline constexpr const char* k_save_input_log = "input.jsonl";
inline constexpr const char* k_save_input_map = "input-map.json";
// The names `SaveVersion` uses for the two formats that are not schema types.
inline constexpr const char* k_store_tables = "store.tables";
inline constexpr const char* k_input_log_format = "engine.input.log";

// The store's database and SQLite's files beside it, in a document directory: what a save backs up
// rather than copies, and what a copy of the document leaves out.
bool is_store_file(std::string_view name) noexcept;

// This build's version of a format a save names — a registered schema type, `store.tables`,
// `engine.input.log`. False when the build has no such format.
bool current_version(std::string_view name, u32& out);

// What a save is written from, besides the manifest the caller fills.
struct SaveInputs {
  // The session's document directory, native.
  std::string document_dir;
  // The document itself: its record types go in the manifest's versions.
  const doc::Document* document = nullptr;
  // An open connection to the store, or null for a session without one.
  store::Database* store = nullptr;
  // The ring's player, whose log is cut at the manifest's tick; null for none.
  const InputObserver* player = nullptr;
};

// Writes a save into `dir`, which must not exist or must be empty, and must not be inside the
// document directory. `manifest` arrives with the clock, the seed, the ring (its player's observer,
// action and speed) and the state hash; this fills `format`, `version`, `document`, `store`, the
// player's file names, `versions` and `files`, and writes `save.json` last.
bool write_save(std::string_view dir, const SaveInputs& inputs, SaveManifest& manifest,
                std::string& error);

// What `read_save` found besides the manifest.
struct SaveCheck {
  // A step for every format the save wrote at an older version than this build's, as
  // "<format> <from> -> <to>", in the manifest's order.
  Vector<std::string> migrations;
  u64 bytes = 0;  // every file of the set
};

// Reads `<dir>/save.json` and checks the save before anything is loaded from it: the format; every
// version it names against this build's; every file against the size and hash the manifest gives
// it; then the manifest itself under the version it was written at.
bool read_save(std::string_view dir, SaveManifest& out, SaveCheck& check, std::string& error);

// Puts a checked save's document into `target` — which must exist and be empty — and its store at
// `target/world.db`, byte for byte.
bool restore_save_files(std::string_view dir, const SaveManifest& manifest, std::string_view target,
                        std::string& error);

// Reads a save's player files: the action map and the input log.
bool read_save_player(std::string_view dir, const SavePlayer& player, input::ActionMap& map,
                      input::InputLog& log, std::string& error);

}  // namespace engine::world
