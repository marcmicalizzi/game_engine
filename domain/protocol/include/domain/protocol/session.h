#pragma once

// Sessions (docs/plan/02-architecture.md §2.2, 06 §6.4): one open document per session, with
// the document store behind it so every committed transaction is on disk before the response
// goes out, and undo and redo positions survive across processes.

#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <domain/protocol/leases.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/proposals.h>
#include <domain/protocol/rpc.h>
#include <foundation/io/vfs.h>

#include <memory>
#include <optional>
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
  //
  // This form checks nothing: it is the engine's own writer (the runtime world's write-back,
  // attributed to `system`). A call from a client goes through the `Caller` form below.
  bool apply(std::span<const doc::Command> commands, doc::Attribution attribution, bool atomic,
             ApplyResult& result, RpcError& error);
  // The same for a client's call (plan 06 §6.5): a proposal layer is written only by its owner
  // and only while open, the caller's role must allow the layer and every tile and type the
  // transaction touches, and when the document requires leases every touched record must be
  // covered by a live lease of the caller's and by nobody else's. A refusal rolls the whole
  // transaction back and is an RPC error (1008, 1009 or 1010), not a diagnostic.
  // `layer` names the layer the transaction edits; empty is the session's edit layer.
  bool apply(std::span<const doc::Command> commands, const Caller& caller, bool atomic,
             std::string_view layer, ApplyResult& result, RpcError& error);
  // Three-way merge of three of the document's layers into `params.output_layer`, appending
  // that layer when it is not there yet, as one transaction against it: the merge is journaled,
  // saved, and undone like any other edit. Conflicts are reported in `result`, not as an error.
  // With a caller, the write into the output layer is checked as `apply` checks it, and the
  // journal records the caller's attribution when the params carried one.
  bool merge(const MergeParams& params, MergeResult& result, RpcError& error);
  bool merge(const MergeParams& params, const Caller* caller, MergeResult& result, RpcError& error);
  // Undo and redo, checked like `apply` when a caller is given: a step rewrites the records its
  // patch touched, in the patch's layer, and is refused (and put back) when the caller may not.
  bool undo(u32 steps, StepResult& result, RpcError& error);
  bool redo(u32 steps, StepResult& result, RpcError& error);
  bool undo(u32 steps, const Caller* caller, StepResult& result, RpcError& error);
  bool redo(u32 steps, const Caller* caller, StepResult& result, RpcError& error);
  // `partition` gives the layer the tile-file form (domain/doc/partition.h); without one it is
  // a single file.
  bool add_layer(std::string_view name, doc::LayerRole role, bool make_edit,
                 const std::optional<doc::LayerPartition>& partition, RpcError& error);
  // Converts a layer between the two storage forms and rewrites its files. No record changes.
  bool set_partition(std::string_view name, const std::optional<doc::LayerPartition>& partition,
                     RpcError& error);
  bool set_edit_layer(std::string_view name, RpcError& error);

  SessionInfo info() const;
  void layers(Vector<LayerInfo>& out) const;

  // --- leases (leases.h) ---------------------------------------------------------------------
  // Every change is written to `leases.json` before the answer. `now_ms` is the host's clock.

  bool acquire_lease(const LeaseAcquireParams& params, const Caller& caller, i64 now_ms,
                     LeaseInfo& out, RpcError& error);
  bool renew_lease(u64 lease, f64 ttl_seconds, const Caller& caller, i64 now_ms, LeaseInfo& out,
                   RpcError& error);
  bool release_lease(u64 lease, const Caller& caller, i64 now_ms, LeaseInfo& out, RpcError& error);
  bool list_leases(std::string_view layer, std::string_view actor, i64 now_ms, LeasesResult& out,
                   RpcError& error);
  bool require_leases(bool required, i64 now_ms, LeasesResult& out, RpcError& error);
  const LeaseTable& leases() const noexcept { return leases_; }

  // --- proposal layers (proposals.h) ---------------------------------------------------------

  // Appends a proposal layer over `params.target`, owned by the caller, stored in the target's
  // form (its partition), or reopens an empty closed one the caller owns.
  bool propose_layer(const ProposeLayerParams& params, const Caller& caller, RpcError& error);
  // Merges an open proposal into its target (proposals.h has the base), validates the merged
  // world against the world as it is, and — when nothing stands in the way and it is not a dry
  // run — commits the target's patch and the proposal's emptying patch and closes it.
  bool promote(const PromoteParams& params, const Caller& caller, PromoteResult& result,
               RpcError& error);
  // Empties an open proposal without merging it and closes it.
  bool reject(const RejectParams& params, const Caller& caller, RpcError& error);
  const ProposalTable& proposals() const noexcept { return proposals_; }

 private:
  class Guard;

  bool persist(RpcError& error);
  // One transaction against `layer`, committed, journaled, and saved. `strict` is the document's
  // precondition checking; a merge replays a diff and turns it off, as undo and redo do. With a
  // caller, the transaction is checked (Guard, session.cpp) and refused whole on a violation.
  bool commit_commands(std::span<const doc::Command> commands, doc::Attribution attribution,
                       bool atomic, bool strict, u32 layer, const Caller* caller,
                       ApplyResult& result, RpcError& error);
  bool step(bool backwards, u32 steps, const Caller* caller, StepResult& result, RpcError& error);
  // After a commit to a proposal layer: the target's record for every object the patch touched
  // that the proposal had not touched before, appended to its base file.
  bool note_proposal_touches(const doc::Patch& patch, RpcError& error);
  // Empties a proposal layer as one journaled patch; `patch_index` is null when it was empty.
  bool clear_layer(u32 layer, doc::Attribution attribution, std::optional<u32>& patch_index,
                   RpcError& error);

  bool load_side_files(RpcError& error);
  bool save_leases(RpcError& error);
  bool save_proposals(RpcError& error);
  std::string base_file(std::string_view proposal) const;

  io::Vfs* vfs_;
  std::string id_;
  std::string dir_;
  doc::Document doc_;
  doc::DocumentManifest manifest_;
  LeaseTable leases_;
  ProposalTable proposals_;
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
