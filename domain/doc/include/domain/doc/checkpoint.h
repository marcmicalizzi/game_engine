#pragma once

// Checkpoints (docs/plan/03-data-model.md §3.3, 06 §6.4; docs/subsystems/doc.md, "Checkpoints"): a
// checkpoint is a commit of the document directory's own git repository, `<dir>/.git`, holding the
// document's files as the last save left them — `manifest.json`, `layers/` and `blobs/`, and nothing
// else (not the journal, the save log, or the protocol's leases and proposals) — with the journal's
// patches since the previous checkpoint as the commit's body. The journal gives fine-grained history;
// git gives coarse, branchable history a person can read with git's own tools.
//
// **The engine runs the `git` on PATH; it links no git library** (ADR-0014: the tree has none, and a
// download is not an option). git is a separate program the engine runs, so its licence does not
// reach the engine's binary. What that costs: a checkpoint needs git installed (`available` says so,
// and the protocol answers 1006 Unavailable without it, naming what is missing), and each one is a
// handful of processes (about 20–60 ms each on Windows). What it buys: the repository is git's own,
// packed, verified and readable by every git tool, and none of git's formats is reimplemented here.
// `ENGINE_GIT` names another executable.
//
// The repository is the engine's, so it is run apart from the user's git configuration: no hooks,
// no signing, no line-ending conversion, no automatic gc, and a fixed committer (the author is the
// checkpoint's actor). Its `info/exclude` keeps every file but the three out of the index.
//
// A checkpoint is read back by `git archive` of the commit (manifest and layers; a blob only when it
// is asked for), never by checking files out over the document: rolling back is the session's
// transaction against the document as it stands (protocol's `doc.rollback_to`), journaled and
// undoable like any other, and this file only says what the checkpoint held.

#include <domain/doc/document.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::doc {

// What a checkpoint held: its manifest and every layer of it, in the manifest's order, each with its
// partition.
struct CheckpointState {
  DocumentManifest manifest;
  Vector<Layer> layers;
};

class Checkpoints {
 public:
  static constexpr std::string_view k_git_dir = ".git";

  // The git executable: `ENGINE_GIT` when set, "git" (found through PATH) otherwise.
  static std::string git_executable();
  // git runs here; `version` is its `--version` line. False with the reason otherwise.
  static bool available(std::string* version, std::string* error);
  // `<dir>/.git` is there.
  static bool initialized(const io::Vfs& vfs, std::string_view dir) noexcept;

  // Commits the document's saved files as one checkpoint and fills `out`. Makes the repository on
  // the first one. `doc` must be saved (the session saves after every change); its journal from the
  // previous checkpoint's undo position to the manifest's is the commit's body.
  static bool take(const io::Vfs& vfs, std::string_view dir, const Document& doc,
                   const DocumentManifest& manifest, std::string_view label,
                   const Attribution& attribution, CheckpointInfo& out, std::string* error);
  // The checkpoints, newest first, at most `limit` (all when zero). None when there is no
  // repository yet.
  static bool list(const io::Vfs& vfs, std::string_view dir, u32 limit, Vector<CheckpointInfo>& out,
                   std::string* error);
  // The checkpoint `id` names (a full or abbreviated commit name), refused when it is not one of
  // this document's.
  static bool find(const io::Vfs& vfs, std::string_view dir, std::string_view id,
                   CheckpointInfo& out, std::string* error);
  // The manifest and layers the checkpoint holds, read from the commit and checked as a load checks
  // them (each file canonical JSON of its schema type, a tile's records in its tile).
  static bool read(const io::Vfs& vfs, std::string_view dir, std::string_view id,
                   CheckpointState& out, std::string* error);
  // The bytes of the named blobs as the checkpoint holds them, as (name, bytes) pairs; a name the
  // checkpoint does not hold is left out.
  static bool read_blobs(const io::Vfs& vfs, std::string_view dir, std::string_view id,
                         std::span<const std::string> names,
                         Vector<std::pair<std::string, std::string>>& out, std::string* error);
};

}  // namespace engine::doc
