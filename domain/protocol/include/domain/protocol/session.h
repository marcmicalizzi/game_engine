#pragma once

// Sessions (docs/plan/02-architecture.md §2.2, 06 §6.4): one open document per session, with
// the document store behind it so every committed transaction is on disk before the response
// goes out, and undo and redo positions survive across processes.

#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <domain/protocol/dir_watch.h>
#include <domain/protocol/events.h>
#include <domain/protocol/leases.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/proposals.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/side_file.h>
#include <foundation/io/roots.h>
#include <foundation/io/vfs.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::protocol {

// What keeps an open document's directory from a client's writes for exactly as long as a session
// has it open (protocol.md, "The host's file roots"): `begin` when SessionManager makes the
// session, `end` when it closes, by whichever path it closes. A host's roots are behind
// `context` (`document_guard` in paths.h); SessionManager knows nothing of them.
struct DocumentGuard {
  void* context = nullptr;
  void (*begin)(void* context, std::string_view dir) = nullptr;
  void (*end)(void* context, std::string_view dir) = nullptr;
};

// What a session has seen of its document's `ledger.json` (protocol.md, "The ledger's events"):
// the file's revision, size and content hash, and a hash of each task and review item, so that a
// look after another process's write decides by inequality of the three whether the ledger changed
// and names the entries that did. The size and hash are of the canonical bytes, what this tree
// writes; `raw_size` and `raw_hash` are of the bytes last read, so a notice for a file whose bytes
// are the ones already seen costs a read and a hash and no parse.
struct LedgerSeen {
  bool present = false;
  u64 revision = 0;
  u64 size = 0;
  u64 hash = 0;
  u64 raw_size = 0;
  u64 raw_hash = 0;
  struct Entry {
    u64 id = 0;
    u64 hash = 0;
  };
  Vector<Entry> tasks;  // ascending by id
  Vector<Entry> reviews;
};

// Who is told that a session's watch saw a notice: the listener, which then looks at the ledgers
// (`SessionManager::begin_ledger_looks`). Called on a watch's own thread, so it only signals and
// never waits on the host's lock; cleared before whoever it names goes away.
class LedgerWaker {
 public:
  void set(void (*wake)(void* context), void* context) noexcept;
  void notify() noexcept;

 private:
  std::mutex mutex_;
  void (*wake_)(void* context) = nullptr;
  void* context_ = nullptr;
};

class Session {
 public:
  Session(io::Vfs& vfs, std::string id, std::string dir);
  ENGINE_NON_COPYABLE(Session);

  const std::string& id() const noexcept { return id_; }
  const std::string& dir() const noexcept { return dir_; }
  // The directory as a client may see it, `<root>://<relative>` (paths.h, `portable_path`), set by
  // whoever opened the session against the host's roots; empty when none was set or no root holds
  // it. What the session's events carry: `dir()` is the host's own path and never leaves it.
  const std::string& shown_path() const noexcept { return shown_path_; }
  void set_shown_path(std::string path) { shown_path_ = std::move(path); }
  // Who protects the directory while the session is open; set by SessionManager::open, ended by
  // SessionManager::close. Empty for a session opened with none.
  const DocumentGuard& guard() const noexcept { return guard_; }
  void set_guard(const DocumentGuard& guard) noexcept { guard_ = guard; }
  const std::string& name() const noexcept { return manifest_.name; }
  doc::Document& document() noexcept { return doc_; }
  const doc::Document& document() const noexcept { return doc_; }
  const doc::DocumentManifest& manifest() const noexcept { return manifest_; }
  u32 undo_position() const noexcept { return manifest_.undo_position; }

  bool create(std::string_view name, RpcError& error);
  bool load(RpcError& error);
  bool save(RpcError& error);

  // Whether the session's directory may be written (protocol.md, "The host's file roots"): yes for
  // the host's own document, one made or opened under a writable root and a loaded save's copy; no
  // for a document a client opened from a read-only root. On such a session every write to its
  // directory (a save, a commit, an undo, a lease, a transaction, a proposal) is refused before
  // anything changes, with the answer a path outside the roots gets for the path it was opened by.
  bool writable() const noexcept { return read_only_as_.empty(); }
  // Set before the document is loaded (SessionManager::open); empty makes it writable again.
  void set_read_only(std::string client_path) { read_only_as_ = std::move(client_path); }
  // True when the session may be written; otherwise false with that refusal in `error`.
  bool check_writable(RpcError& error) const;
  // The host's roots, against which every path a record gains is admitted as it lands
  // (record_paths.h): every commit, merge, promotion, rollback, undo and redo. Null (a session no
  // host gave roots): the stored form alone, never an absolute path. Set by `SessionManager`.
  void set_roots(const io::Roots* roots) noexcept { roots_ = roots; }
  const io::Roots* roots() const noexcept { return roots_; }

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

  // Where this session's `journal` and `session` events go (events.h); null publishes none. Set by
  // the SessionManager that opened it.
  void set_events(Subscriptions* events) noexcept { events_ = events; }
  // `event.session` for this session: "opened", "closed", or a lease's change with the lease.
  // `connection` is the one whose call made the change; `reason` says why a lease went when its
  // holder did not ask.
  void publish_session(std::string_view change, const LeaseInfo* lease = nullptr,
                       u64 connection = 0, std::string_view reason = {}) const;
  // `event.session` for a change to the document's task ledger (ledger.cpp): "task_created" and
  // the like, naming the task or the item (the other 0) and the actor who changed it.
  // `touched_task` is a task the call changed beside the item it names (a raise for a task adds the
  // item to it): the call's own too, so it is not told again. What else the write carried —
  // another process's writes, claims this host's clock let lapse — goes first, as
  // "ledger_changed" (`flush_ledger_written`).
  void publish_ledger(std::string_view change, u64 task, u64 review, std::string_view actor,
                      u64 connection, u64 touched_task = 0);
  // `event.session` for a proposal layer (session.cpp): "proposal_opened", "proposal_reopened",
  // "proposal_promoted" or "proposal_rejected", with the layer and the actor who did it, published
  // after `proposals.json` is written (a dry run and a refused promotion publish nothing).
  void publish_proposal(std::string_view change, std::string_view layer, std::string_view actor,
                        u64 connection) const;

  // --- the ledger's events (protocol.md, "The ledger's events"; ledger.cpp) -------------------
  // Watches the document's directory for writes to `ledger.json` (dir_watch.h) and tells `waker`
  // of each notice; what the file holds now is what the session has seen. Null stops watching.
  // The session manager starts a watch on every session while it has events to publish.
  void watch_ledger(LedgerWaker* waker);
  bool watching_ledger() const noexcept { return ledger_watch_ != nullptr; }
  bool ledger_watch_polling() const noexcept;
  // The ledger as this process wrote it (ledger.cpp, `update_ledger`), kept until the call's own
  // event goes out. A read that writes nothing costs nothing here.
  void ledger_written(const LedgerFile& ledger);
  // What the write changed beside the call's own task or item (`own_task`, `own_review`; 0 for
  // none) — another process's writes since the session last saw the ledger, claims the call let
  // lapse — published as "ledger_changed", and the write taken as seen, so the notice the write
  // raises publishes nothing more and no write of this host's swallows another's. `publish_ledger`
  // calls it before the call's own event; a list that let claims lapse, and a look, call it too.
  void flush_ledger_written(u64 own_task, u64 own_review);
  // "ledger_changed": the ids whose entries differ between `from` and `to`, but the two named, and
  // `to`'s revision. False, and nothing sent, when nobody wants it, or when no other entry differs
  // and either the revision does not or this host's own write moved it (the bytes changed and the
  // content did not: a file rewritten by hand with other spacing).
  bool publish_ledger_changed(const LedgerSeen& from, const LedgerSeen& to, u64 own_task = 0,
                              u64 own_review = 0) const;
  // A notice the watch's thread left, taken by a look; and one put back when a look is retried.
  bool take_ledger_notice() noexcept { return ledger_notice_.exchange(false); }
  void note_ledger_notice() noexcept { ledger_notice_.store(true); }
  const LedgerSeen& ledger_seen() const noexcept { return ledger_seen_; }
  // Moves whenever this process sets what it has seen outside a look (its own reads and writes), so
  // a look that read the file before one of them is not taken over what came after.
  u64 ledger_seen_moves() const noexcept { return ledger_seen_moves_; }
  void set_ledger_seen(LedgerSeen seen) { ledger_seen_ = std::move(seen); }

  // --- holders (protocol.md, "Several clients, one host") ------------------------------------
  // Who holds the session open: client connections that opened it, and 0 for the host's own
  // stdio and in-process callers. The SessionManager closes a session when its last holder lets
  // go.
  std::span<const u64> holders() const noexcept { return {holders_.data(), holders_.size()}; }
  bool held_by(u64 holder) const noexcept;
  void hold(u64 holder);
  // False when `holder` did not hold it.
  bool let_go(u64 holder);

  // A client connection has ended: every lease it took (`LeaseInfo.connection`) is released and
  // saved, each with `event.session` "lease_released" and the reason "connection closed". The
  // number released.
  u32 release_connection_leases(u64 connection, i64 now_ms);
  // The same connection's open transactions stop being bound to it and are their opener actor's
  // again, still staged (protocol.md, "Several clients, one host"). The number let go.
  u32 release_connection_transactions(u64 connection);
  // Opened again after its last holder closed it: the leases this host process granted to client
  // connections that have ended since (`ended`) are dropped and saved, as another
  // process's are when the document is loaded. The number dropped.
  u32 drop_ended_connection_leases(std::span<const u64> ended);

  // The client connection each open transaction was begun on (non-zero only), for as long as it
  // lasts. Kept in memory and never in `transactions.json`: connections are this process's, and
  // a transaction outlives the host that a call started, where its actor alone owns it. The
  // session manager keeps them across a close and gives them back at the reopen.
  struct TransactionHold {
    u64 transaction = 0;
    u64 connection = 0;
  };
  std::span<const TransactionHold> transaction_holds() const noexcept {
    return {transaction_holds_.data(), transaction_holds_.size()};
  }
  // A hold the session manager kept while the session was closed; ignored when the transaction
  // is no longer open.
  void restore_transaction_hold(TransactionHold hold);

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

  // --- transactions (plan 06 §6.4; protocol.md, "Transactions a client opens") ----------------
  // A client's transaction is a list of staged commands kept beside the document in
  // `transactions.json`, so that it outlives the host a call started. Each staging call is checked
  // as `apply` checks a write — the commands against the document with the transaction's earlier
  // ones applied, the caller's role, the proposal's owner, the leases — and changes no record; the
  // commit applies them all as one checked transaction and one journal patch, or none of them.
  // Only the opener stages, commits or rolls back: the actor that opened it and, while the client
  // connection it was opened on lasts, that connection alone. `now_ms` is the host's clock.

  bool begin_transaction(const BeginParams& params, const Caller& caller, i64 now_ms,
                         TransactionInfo& out, RpcError& error);
  // `layer` empty is the transaction's own; naming another is refused.
  bool stage(u64 transaction, std::span<const doc::Command> commands, const Caller& caller,
             bool atomic, std::string_view layer, i64 now_ms, ApplyResult& result, RpcError& error);
  // A commit whose commands no longer all apply, or that the role or the leases refuse, lands
  // nothing and leaves the transaction open, for the caller to mend or roll back.
  bool commit_transaction(u64 transaction, const Caller& caller, i64 now_ms, ApplyResult& result,
                          RpcError& error);
  bool rollback_transaction(u64 transaction, const Caller& caller, i64 now_ms, TransactionInfo& out,
                            RpcError& error);
  bool list_transactions(std::string_view actor, i64 now_ms, TransactionsResult& out,
                         RpcError& error);
  // The table as this session last read it under the file's lock; another host may have changed
  // the file since (`doc.transactions` reads it afresh).
  const TransactionsFile& transactions() const noexcept { return transactions_; }
  // The journal patch that names client transaction `transaction`, or -1, wherever it stands: in
  // the document, or in the redo tail (undone, or journaled by a commit whose save then failed).
  i64 landed_patch(u64 transaction) const noexcept;
  // Whether `transaction`'s work is in the document as saved: its patch is in the journal before
  // the undo position, and no save has failed since this session last wrote the document whole. A
  // patch in the redo tail is not landed — a save that failed after the journal leaves the commit
  // there, and the next host reads the undo position the layer files were saved at — so its table
  // entry stays and a retried commit applies it again (protocol.md, "Transactions a client opens").
  bool landed(u64 transaction) const noexcept;
  // Bounds on what a transaction holds (batch 68's review): every staging call applies everything
  // staged so far again, and every call that reads the table reads every open transaction. 1004
  // past them.
  static constexpr u32 k_max_staged_commands = 10'000;
  static constexpr u32 k_max_open_transactions = 64;  // per actor
  // Tests only (policy_tests.cpp): the next write of `transactions.json` fails, as a rename that a
  // scanner holds past its retries does, after everything before it in the call has happened.
  void fail_transactions_write_for_test() noexcept { test_fail_transactions_write_ = true; }
  // **The one gate on every write beside the document**: `transactions.json` and `leases.json`
  // (`update_transactions`, `update_leases`) and `ledger.json` (ledger.cpp's `update_ledger`) ask
  // it before they take a lock or change anything, and a refusal is the call's answer. It is
  // `check_writable`: a session that may not write its directory (one a client opened from a
  // read-only root) refuses here with the answer any refused path gets, and everything it still
  // reads beside the document — the leases, the proposals, the transactions, the ledger — it reads
  // without the lock (`make_lock = false`, side_file.h), so it makes no lock file, removes no gone
  // host's mark and writes nothing there (protocol.md, "The host's file roots").
  bool may_write_side_files(RpcError& error) const { return check_writable(error); }
  // Tests only (policy_tests.cpp): the next save of the document fails before it writes anything,
  // as a layer file a scanner holds or a full disk does, after the journal has taken the patch.
  void fail_save_for_test() noexcept { test_fail_save_ = true; }
  // --- blobs and checkpoints (roadmap D13, D14; session_history.cpp)
  // ------------------------------ docs/subsystems/protocol.md, "Blobs and checkpoints".

  // Bytes into the document's blob store (domain/doc/blob_store.h). Changes no document.
  bool put_blob(std::string_view bytes, std::string_view media, BlobResult& out, RpcError& error);
  // Removes the blobs nothing can bring back: no layer, no journal patch and no open proposal's
  // merge base names them, and they are older than `params.min_age_s`.
  bool collect_blobs(const CollectBlobsParams& params, CollectBlobsResult& out, RpcError& error);
  // The document as saved, committed to its directory's repository (domain/doc/checkpoint.h). 1006
  // when there is no git to run.
  bool checkpoint(std::string_view label, const Caller& caller, doc::CheckpointInfo& out,
                  RpcError& error);
  // Makes every accepted layer what the checkpoint held, as one journaled patch per layer it
  // changes, each checked against the caller's role and the leases like any write; open proposals
  // are left as they are. All or nothing: every layer is checked before any is changed, and a
  // layer refused after that (a save that failed) undoes the layers that had landed, puts back
  // the storage forms and the layers it added, and answers the refusal. When that taking back
  // fails too, the error's `data` is the RollbackResult of what stands, and `doc.undo` with its
  // `patches` steps takes it back.
  bool rollback_to(const RollbackParams& params, const Caller& caller, RollbackResult& out,
                   RpcError& error);
  // Tests only (history_tests.cpp): the rollback's commit of its `layer`th changed layer, from
  // zero, is refused as a failed save would be, after the checks passed; with `take_back_too`, so
  // is the undo of what had landed. Nothing else calls it.
  void fail_rollback_for_test(u32 layer, bool take_back_too) noexcept {
    test_fail_rollback_at_ = layer;
    test_fail_take_back_ = take_back_too;
  }

 private:
  class Guard;
  // A check-only pass of `commit_commands` for a staging call: the first `held` commands are the
  // transaction's, applied again, and the rest are the call's; nothing is committed, and the
  // indices of the call's commands that applied are appended to `accepted`.
  struct Staging {
    u32 held = 0;
    Vector<u32>* accepted = nullptr;
  };

  // Whether `caller` may make this write into `layer` (Guard, without committing anything): the
  // commands are applied in a transaction that is rolled back. What a write made of several
  // patches checks first, so that a refusal changes nothing.
  bool check_commands(std::span<const doc::Command> commands, const doc::Attribution& attribution,
                      u32 layer, const Caller* caller, RpcError& error);
  // False, with a diagnostic in `result` naming the blob, when `command` references a blob the
  // document's store does not hold.
  bool blobs_held(const doc::Command& command, ApplyResult& result) const;

  bool persist(RpcError& error);
  // One transaction against `layer`, committed, journaled, and saved. `strict` is the document's
  // precondition checking; a merge replays a diff and turns it off, as undo and redo do. With a
  // caller, the transaction is checked (Guard, session.cpp) and refused whole on a violation.
  bool commit_commands(std::span<const doc::Command> commands, doc::Attribution attribution,
                       bool atomic, bool strict, u32 layer, const Caller* caller,
                       ApplyResult& result, RpcError& error, const Staging* staging = nullptr,
                       u64 transaction = 0);
  bool step(bool backwards, u32 steps, const Caller* caller, StepResult& result, RpcError& error);
  // After a commit to a proposal layer: the target's record for every object the patch touched
  // that the proposal had not touched before, appended to its base file.
  bool note_proposal_touches(const doc::Patch& patch, RpcError& error);
  // Empties a proposal layer as one journaled patch; `patch_index` is null when it was empty.
  bool clear_layer(u32 layer, doc::Attribution attribution, std::optional<u32>& patch_index,
                   RpcError& error);

  // `event.journal` for the patch at `index`, after it was committed, undone or redone and saved,
  // naming the connection whose call made it (0: stdio, the host, the engine's write-back).
  void publish_journal(std::string_view action, u32 index, u64 connection) const;

  bool load_side_files(RpcError& error);
  bool hold_host_mark(RpcError& error);
  // Writes this host's entry for the proposal `layer` into `proposals.json` under its lock, read
  // afresh, so another host's change to another proposal stays (protocol.md, "Two hosts, one side
  // file").
  bool save_proposals(std::string_view layer, RpcError& error);
  // The read-modify-writes of `transactions.json` and `leases.json` (side_file.h; protocol.md,
  // "Two hosts, one side file"): under the file's lock the table is read afresh into
  // `transactions_` or `leases_`, `change(write)` runs, and the table is written back when it set
  // `write`. Reading `transactions_` afresh also drops the entries the journal shows committed.
  // `read`: the call is a read (`lease.list`, `doc.transactions`) that writes only to let lapsed
  // entries go; on a session that may not be written it reads the table without the lock, runs
  // `change` and writes nothing, where a write is refused by the gate.
  template <class Change>
  bool update_transactions(Change&& change, RpcError& error, bool read = false);
  template <class Change>
  bool update_leases(Change&& change, RpcError& error, bool read = false);
  // `leases_` read afresh (under the file's lock, as every reader reads) before a write is checked
  // against it: a lease another host granted, or its `lease.require`, holds here at once.
  bool refresh_leases(RpcError& error);
  std::string transactions_path() const;
  std::string leases_path() const;
  // The open transaction `id` for the caller, after dropping lapsed ones (`write` set when any
  // was): 1003 when there is none (or it lapsed, or landed), 1008 when another actor opened it or
  // another connection than the one still holding it calls.
  TransactionEntry* find_transaction(u64 id, const Caller& caller, i64 now_ms, bool& write,
                                     RpcError& error);
  // Whether a connection other than the caller's still holds `id` (1008 in `error` when it does).
  bool held_elsewhere(u64 id, const Caller& caller, RpcError& error) const;
  // Drops lapsed transactions, and the holds of every transaction no longer open; the number
  // dropped.
  u32 expire_transactions(i64 now_ms);
  // Drops the open entries whose work has `landed` (a table write that failed after the commit's
  // save); the number dropped. An entry whose patch is only in the redo tail stays.
  u32 drop_landed_transactions();
  void forget_transaction_hold(u64 transaction);
  std::string base_file(std::string_view proposal) const;

  io::Vfs* vfs_;
  std::string id_;
  std::string dir_;
  std::string shown_path_;            // dir_ as `<root>://<relative>`, for events
  DocumentGuard guard_;               // protects dir_ while open (SessionManager)
  std::string read_only_as_;          // the client's path, when the session may not be written
  const io::Roots* roots_ = nullptr;  // the host's, for the paths records gain (set_roots)
  doc::Document doc_;
  doc::DocumentManifest manifest_;
  LeaseTable leases_;
  // Held while this host's socket connections may hold leases here (session.cpp's
  // `host_mark_path`): what another host's open tells this host's live leases by.
  SideFileLock host_mark_;
  ProposalTable proposals_;
  TransactionsFile transactions_;
  Vector<TransactionHold> transaction_holds_;
  Subscriptions* events_ = nullptr;
  u32 test_fail_rollback_at_ = ~0u;  // fail_rollback_for_test
  bool test_fail_take_back_ = false;
  bool test_fail_transactions_write_ = false;  // fail_transactions_write_for_test
  bool test_fail_save_ = false;                // fail_save_for_test
  // A save failed after the document in memory changed, so the files hold less than `doc_` does
  // until a save succeeds: nothing in the journal counts as `landed` meanwhile.
  bool unsaved_ = false;
  Vector<u64> holders_;
  // The ledger's watch (protocol.md, "The ledger's events").
  LedgerSeen ledger_seen_;
  LedgerSeen ledger_written_;  // valid while `ledger_has_written_`
  bool ledger_has_written_ = false;
  u64 ledger_seen_moves_ = 0;
  std::atomic<bool> ledger_notice_{false};
  LedgerWaker* ledger_waker_ = nullptr;
  // Last, so it is destroyed first: its thread reads `ledger_notice_` and `ledger_waker_`.
  std::unique_ptr<DirWatch> ledger_watch_;
};

template <class Change>
bool Session::update_transactions(Change&& change, RpcError& error, bool read) {
  if (read && !writable()) {
    bool present = false;
    if (!read_side_file(*vfs_, transactions_path(), transactions_, present, error)) return false;
    bool write = false;
    (void)drop_landed_transactions();
    return change(write);
  }
  if (!may_write_side_files(error)) return false;
  return update_side_file(
      *vfs_, transactions_path(), transactions_,
      [&](TransactionsFile&, bool& write) {
        if (drop_landed_transactions() > 0) write = true;
        const bool ok = change(write);
        if (write && test_fail_transactions_write_) {
          test_fail_transactions_write_ = false;
          write = false;
          if (ok) error = make_error(codes::k_io_error, "transactions.json: failed for the test");
          return false;
        }
        return ok;
      },
      error);
}

template <class Change>
bool Session::update_leases(Change&& change, RpcError& error, bool read) {
  LeaseFile file;
  if (read && !writable()) {
    bool present = false;
    if (!read_side_file(*vfs_, leases_path(), file, present, error)) return false;
    leases_.assign(std::move(file));
    bool write = false;
    return change(write);
  }
  if (!may_write_side_files(error)) return false;
  return update_side_file(
      *vfs_, leases_path(), file,
      [&](LeaseFile& stored, bool& write) {
        leases_.assign(std::move(stored));
        const bool ok = change(write);
        if (write) stored = leases_.file();
        return ok;
      },
      error);
}

class SessionManager {
 public:
  explicit SessionManager(io::Vfs& vfs) noexcept : vfs_(&vfs) {}
  ENGINE_NON_COPYABLE(SessionManager);

  // Opens (or with `create`, creates) the document at `params.path`, held by `holder` (a client
  // connection, or 0 for the host's own stdio and in-process callers). A session already open on
  // the same path is returned instead of a second one, and `holder` joins its holders: several
  // clients of one host share one document.
  // `read_only_as` non-empty: the document was admitted for reading only (a read-only root), so
  // the session may not be written (`Session::writable`) and its refusals name that path. An
  // already open session is shared as it is, and made writable when this opener may write.
  // `shown` is the directory as `<root>://<relative>` (Session::shown_path), set before the
  // "opened" event is published so no event ever carries the host's own path.
  // `guard` (copied) protects the directory from the moment the session is made until it closes:
  // its `begin` runs once, when this call makes the session (or gives one opened with no guard its
  // first), and its `end` in `close`, which every way a session ends goes through. A session
  // already open and guarded is shared without a second `begin`.
  Session* open(const SessionOpenParams& params, u64 holder, RpcError& error,
                std::string_view read_only_as = {}, std::string_view shown = {},
                const DocumentGuard* guard = nullptr);
  Session* find(std::string_view id) noexcept;
  // Looks up the session named in params; sets SessionNotFound otherwise.
  Session* require(std::string_view id, RpcError& error) noexcept;
  // `holder` lets go of the session (`session.close`); it closes when no holder is left. False
  // when `holder` did not hold it; `closed` says whether it closed.
  bool release(std::string_view id, u64 holder, bool& closed);
  // Closes the session whoever holds it: the host's own clean-up of a session it just opened. The
  // one place a session ends, so the one place its guard ends: `release` and `release_connection`
  // close through it. Destroying the manager is not a close: it publishes no "closed" and ends no
  // guard, since the roots a guard names may be gone first.
  bool close(std::string_view id);
  // A client connection has ended: its leases on every session are released, then its holds,
  // closing each session nobody else holds.
  void release_connection(u64 connection, i64 now_ms);
  usize count() const noexcept { return sessions_.size(); }
  // Where every session's events go (events.h): the sessions open now and every one opened later.
  // A session opened or closed with events set publishes `event.session`.
  void set_events(Subscriptions* events) noexcept;
  Subscriptions* events() const noexcept { return events_; }
  // The host's roots (Context::roots), given to every session open now and opened later, so a path
  // a record gains is admitted as it lands (Session::set_roots). A host sets it once at start,
  // before its own document is opened; `session.open` sets it from the context when it is unset.
  void set_roots(const io::Roots* roots) noexcept;
  const io::Roots* roots() const noexcept { return roots_; }
  std::span<const std::unique_ptr<Session>> sessions() const noexcept {
    return {sessions_.data(), sessions_.size()};
  }
  io::Vfs& vfs() noexcept { return *vfs_; }

  // --- the ledger's events (protocol.md, "The ledger's events"; ledger.cpp) -------------------
  // Who a session's ledger watch wakes on a notice (the listener); null wakes nobody, and the
  // notices wait for the next look.
  void set_ledger_waker(void (*wake)(void* context), void* context) noexcept {
    ledger_waker_.set(wake, context);
  }
  // One look at the ledgers of the sessions whose watch saw a notice (`all`: every watched
  // session's, whatever its watch saw), in three steps so the file is read with the host's lock let
  // go: `begin_ledger_looks` under the host's lock, `read_ledger_look` with no lock of the host's
  // held (it takes the ledger's own lock between processes for the read alone), and
  // `finish_ledger_looks` under the host's lock again, which publishes "ledger_changed" for each
  // ledger whose revision, size or content hash is not what its session last saw. `again` is set
  // when a look raced a write of this process's and was put back for the next.
  struct LedgerLook {
    std::string session;
    std::string path;  // the Vfs path of ledger.json
    bool make_lock = true;
    u64 seen_moves = 0;
    bool seen_present = false;
    u64 seen_raw_size = 0;
    u64 seen_raw_hash = 0;
    bool read = false;  // the read and parse finished
    bool same = false;  // the file's bytes are the ones last seen
    LedgerSeen now;
  };
  void begin_ledger_looks(bool all, Vector<LedgerLook>& looks);
  static void read_ledger_look(io::Vfs& vfs, LedgerLook& look);
  u32 finish_ledger_looks(Vector<LedgerLook>& looks, bool& again);
  // The three on the caller's thread: for a caller that holds the host's lock, and the tests. The
  // number of "ledger_changed" events published.
  u32 look_at_ledgers(bool all);

 private:
  // What a connection left in a session that closed while it was still connected (protocol.md,
  // "Several clients, one host"): the lease stays in the document's `leases.json` and the
  // transaction's hold here, for the document's next open in this process, since a lease and a
  // transaction are on a document's records and the session was only how the connection reached
  // them.
  struct ParkedHold {
    std::string dir;
    Session::TransactionHold hold;
  };
  void note_closing(const Session& session);

  io::Vfs* vfs_;
  // Before the sessions, so it outlives every watch that names it.
  LedgerWaker ledger_waker_;
  Vector<std::unique_ptr<Session>> sessions_;
  u32 next_id_ = 1;
  Subscriptions* events_ = nullptr;
  const io::Roots* roots_ = nullptr;
  // Connections that held a lease in a session closed under them, still connected (ascending),
  // and those of them that have since ended: a reopen drops the second kind's leases.
  // Bounded by the leases taken, not by the connections served.
  Vector<u64> detached_;
  Vector<u64> ended_;
  Vector<ParkedHold> parked_;
};

}  // namespace engine::protocol
