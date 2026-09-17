#pragma once

// Sessions (docs/plan/02-architecture.md §2.2, 06 §6.4): one open document per session, with
// the document store behind it so every committed transaction is on disk before the response
// goes out, and undo and redo positions survive across processes.

#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <domain/protocol/rpc.h>
#include <foundation/io/vfs.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace engine::protocol {

class Session {
 public:
  Session(io::Vfs& vfs, std::string id, std::string dir);
  ENGINE_NON_COPYABLE(Session);

  const std::string& id() const noexcept { return id_; }
  const std::string& dir() const noexcept { return dir_; }
  const std::string& name() const noexcept { return manifest_.name; }
  doc::Document& document() noexcept { return doc_; }
  const doc::Document& document() const noexcept { return doc_; }
  const doc::DocumentManifest& manifest() const noexcept { return manifest_; }
  u32 undo_position() const noexcept { return manifest_.undo_position; }

  bool create(std::string_view name, RpcError& error);
  bool load(RpcError& error);
  bool save(RpcError& error);

  // One transaction: applies every command, commits (dropping any redo tail first), journals,
  // and saves. Command failures are reported in `result.diagnostics`, not as an RPC error;
  // with `atomic` the transaction rolls back when any command fails.
  bool apply(std::span<const doc::Command> commands, doc::Attribution attribution, bool atomic,
             ApplyResult& result, RpcError& error);
  // Three-way merge of three of the document's layers into `params.output_layer`, appending
  // that layer when it is not there yet, as one transaction against it: the merge is journaled,
  // saved, and undone like any other edit. Conflicts are reported in `result`, not as an error.
  bool merge(const MergeParams& params, MergeResult& result, RpcError& error);
  bool undo(u32 steps, StepResult& result, RpcError& error);
  bool redo(u32 steps, StepResult& result, RpcError& error);
  bool add_layer(std::string_view name, doc::LayerRole role, bool make_edit, RpcError& error);
  bool set_edit_layer(std::string_view name, RpcError& error);

  SessionInfo info() const;
  void layers(Vector<LayerInfo>& out) const;

 private:
  bool persist(RpcError& error);
  // One transaction against `layer`, committed, journaled, and saved. `strict` is the document's
  // precondition checking; a merge replays a diff and turns it off, as undo and redo do.
  bool commit_commands(std::span<const doc::Command> commands, doc::Attribution attribution,
                       bool atomic, bool strict, u32 layer, ApplyResult& result, RpcError& error);

  io::Vfs* vfs_;
  std::string id_;
  std::string dir_;
  doc::Document doc_;
  doc::DocumentManifest manifest_;
};

class SessionManager {
 public:
  explicit SessionManager(io::Vfs& vfs) noexcept : vfs_(&vfs) {}
  ENGINE_NON_COPYABLE(SessionManager);

  // Opens (or with `create`, creates) the document at `params.path`. A session already open
  // on the same path is returned instead of a second one.
  Session* open(const SessionOpenParams& params, RpcError& error);
  Session* find(std::string_view id) noexcept;
  // Looks up the session named in params; sets SessionNotFound otherwise.
  Session* require(std::string_view id, RpcError& error) noexcept;
  bool close(std::string_view id);
  usize count() const noexcept { return sessions_.size(); }
  std::span<const std::unique_ptr<Session>> sessions() const noexcept {
    return {sessions_.data(), sessions_.size()};
  }
  io::Vfs& vfs() noexcept { return *vfs_; }

 private:
  io::Vfs* vfs_;
  Vector<std::unique_ptr<Session>> sessions_;
  u32 next_id_ = 1;
};

}  // namespace engine::protocol
