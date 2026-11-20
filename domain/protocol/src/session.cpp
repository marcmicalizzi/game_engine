#include "subject_digest.h"

#include <core/json/json.h>
#include <core/platform/random.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/doc/merge.h>
#include <domain/doc/partition.h>
#include <domain/protocol/paths.h>
#include <domain/protocol/record_paths.h>
#include <domain/protocol/session.h>

namespace engine::protocol {

using doc::DocumentStore;

// The wire enums mirror domain/doc's, so the conversion below is a cast.
static_assert(static_cast<u8>(doc::MergeConflict::PropertyBothChanged) ==
                      static_cast<u8>(MergeConflictKind::PropertyBothChanged) &&
                  static_cast<u8>(doc::MergeConflict::DeletedAndModified) ==
                      static_cast<u8>(MergeConflictKind::DeletedAndModified) &&
                  static_cast<u8>(doc::MergeConflict::CreatedBothDifferent) ==
                      static_cast<u8>(MergeConflictKind::CreatedBothDifferent) &&
                  static_cast<u8>(doc::MergeConflict::ParentCycle) ==
                      static_cast<u8>(MergeConflictKind::ParentCycle),
              "engine.protocol.MergeConflictKind must number doc::MergeConflict::Kind's values");

namespace {

constexpr std::string_view k_leases_file = "leases.json";
constexpr std::string_view k_proposals_file = "proposals.json";
constexpr std::string_view k_proposals_dir = "proposals";
constexpr std::string_view k_transactions_file = "transactions.json";

// This host process's name for the leases its connections take (`LeaseInfo.host`): 64 random
// bits, made once. A clock and the process would do as well for telling hosts apart; the secure
// source is simply what the listener already uses, with the clock behind it if it fails.
const std::string& host_instance() {
  static const std::string name = [] {
    u8 bytes[8] = {};
    if (!platform::secure_random(std::span<u8>(bytes, sizeof(bytes)))) {
      u64 t = static_cast<u64>(time::wall_unix_ms());
      for (u8& b : bytes) {
        b = static_cast<u8>(t);
        t >>= 8;
      }
    }
    constexpr char k_digits[] = "0123456789abcdef";
    std::string text;
    for (const u8 b : bytes) {
      text.push_back(k_digits[b >> 4]);
      text.push_back(k_digits[b & 15]);
    }
    return text;
  }();
  return name;
}

std::string hex_of(doc::ObjectId id) {
  char hex[Id128::k_hex_length + 1];
  id.to_hex(hex);
  return std::string(hex, Id128::k_hex_length);
}

std::string tile_text(doc::TileCoord tile) {
  return "(" + std::to_string(tile.x) + ", " + std::to_string(tile.y) + ")";
}

JsonValue tile_json(doc::TileCoord tile) {
  JsonValue t = JsonValue::array();
  t.push_back(JsonValue(tile.x));
  t.push_back(JsonValue(tile.y));
  return t;
}

f64 ms_since(i64 start_ns) { return static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6; }

// `leases.json` and `transactions.json` at version 1 counted their ids in `next_id`; version 2
// draws them (side_file.h), and the step drops the counter.
bool drop_next_id(JsonValue& object, schema::ReadContext&) {
  if (object.is_object()) object.as_object().erase(std::string_view("next_id"));
  return true;
}

// `ledger.json` at version 1 counted its task and review ids; version 2 draws them, and each item
// carries a revision that moves on every change (ledger.cpp). The step drops the two counters and
// gives every item revision 1, so 0 stays what a `review.close` that names none sends.
bool drop_ledger_counters(JsonValue& object, schema::ReadContext&) {
  if (!object.is_object()) return true;
  object.as_object().erase(std::string_view("next_task"));
  object.as_object().erase(std::string_view("next_review"));
  if (JsonValue* reviews = object.find("reviews"); reviews != nullptr && reviews->is_array()) {
    for (JsonValue& r : reviews->as_array()) {
      if (r.is_object() && r.find("revision") == nullptr) r.set("revision", JsonValue(u64{1}));
    }
  }
  return true;
}

// `ledger.json` at version 3 held items without subject digests; version 4's items carry
// `raised_digest` and `decided_at` and their subjects `assets`, every one an addition with an empty
// default. LedgerFile itself adds no field at 4 (its 2-to-3 step, the ledger's `revision`, is the
// generated additive one), so this step is registered rather than declared additive — schemac
// refuses `@additive` on a version that adds no field — and there is nothing in the JSON for it to
// change: an item raised before digests reads with an empty stamp, which no digest equals, so its
// detail says it changed since it was raised.
bool ledger_items_gain_digests(JsonValue&, schema::ReadContext&) { return true; }

// The mark a host holds on a document while its socket connections may hold leases there: the
// lock of `leases.<host>` (`leases.<host>.lock`, side_file.h), taken with the first such lease and
// let go with the session or the process. Another host's open asks it before it drops that host's
// connection leases as a gone host's (load_side_files).
std::string host_mark_path(std::string_view dir, std::string_view host) {
  return DocumentStore::path_of(dir, "leases." + std::string(host));
}

// Whether the host named `host` still holds its mark on `dir`. No mark, or a mark nobody holds, is
// a host that has gone (a host's name is drawn once per process and never reused, so a free mark
// is never taken again, and is removed here when `may_remove`: the session's gate on writes beside
// its document said yes). A mark that cannot be opened is counted alive: the lease then stands
// until its own expiry rather than being dropped from a live host.
bool host_alive(const io::Vfs& vfs, std::string_view dir, std::string_view host, bool may_remove) {
  const std::string mark = host_mark_path(dir, host);
  if (!vfs.exists(mark + ".lock")) return false;
  std::string native;
  if (vfs.resolve(mark, native, /*for_write=*/false) != io::Status::Ok) return true;
  SideFileLock probe;
  std::string message;
  if (!probe.acquire(native, 0, message)) return true;
  if (may_remove) (void)vfs.remove(mark + ".lock");
  return false;
}

void register_side_file_steps() {
  static const bool registered = [] {
    schema::MigrationRegistry::global().add({"engine.protocol.LeaseFile", 1, &drop_next_id});
    schema::MigrationRegistry::global().add({"engine.protocol.TransactionsFile", 1, &drop_next_id});
    schema::MigrationRegistry::global().add(
        {"engine.protocol.LedgerFile", 1, &drop_ledger_counters});
    schema::MigrationRegistry::global().add(
        {"engine.protocol.LedgerFile", 3, &ledger_items_gain_digests});
    return true;
  }();
  (void)registered;
}
}  // namespace

// ---- Guard ------------------------------------------------------------------------------------
//
// What a client's write into a layer is checked against (plan 06 §6.5): the proposal's owner and
// state, the caller's role, and the leases. One per write: `begin` makes the checks that depend on
// the layer alone and decides whether the records need checking at all; `before` records where
// each object the write names stands before its first change; `finish` checks every one of them
// where it stood and where it stands now. A call that needs no record checks — no role restricts
// it and the document does not require leases — pays one branch per command.
//
// Records are placed by their composed position, not by the edit layer's record (policy.h,
// `place_of`), and are checked on the lease layer: a proposal's target, or the layer itself.

class Session::Guard {
 public:
  Guard(Session& session, const Caller* caller, i64 now_ms)
      : session_(session), caller_(caller), now_ms_(now_ms) {}

  // `step`: an undo or redo replays a patch the journal already accepted, so the proposal's owner
  // and state are not asked again — undoing a promotion's emptying patch has to work, and the
  // proposal is closed by then — while the role and the leases are, because they say who may
  // change those records now.
  bool begin(u32 layer, bool step, RpcError& error) {
    if (caller_ == nullptr) return true;
    if (!session_.refresh_leases(error)) return false;
    const doc::Document& d = session_.doc_;
    const std::string& name = d.layer(layer).name();
    lease_layer_ = name;
    const ProposalTable::Entry* proposal = session_.proposals_.find(name);
    const Role* role = caller_->role;
    if (proposal != nullptr && step) {
      lease_layer_ = proposal->info.target;
    } else if (proposal != nullptr) {
      const ProposalInfo& info = proposal->info;
      if (info.state != ProposalState::Open) {
        error = forbidden("proposal " + name + " is " +
                              (info.state == ProposalState::Promoted ? "promoted" : "rejected") +
                              " and closed: doc.propose_layer with its name reopens it",
                          "proposal closed", role != nullptr ? role->name : caller_->who.role);
        return false;
      }
      if (info.owner != caller_->who.actor) {
        JsonValue extra = JsonValue::object();
        extra.set("layer", JsonValue(name));
        extra.set("owner", JsonValue(info.owner));
        error = forbidden("proposal " + name + " belongs to " + info.owner + ", and only its " +
                              "owner writes it (this call's actor is '" + caller_->who.actor + "')",
                          "proposal owner", role != nullptr ? role->name : caller_->who.role,
                          std::move(extra));
        return false;
      }
      lease_layer_ = info.target;
    }
    if (role != nullptr) {
      if (role->review_required && proposal == nullptr) {
        JsonValue extra = JsonValue::object();
        extra.set("layer", JsonValue(name));
        error = forbidden("role '" + role->name + "' needs review: it writes only to proposal " +
                              "layers it owns (doc.propose_layer), and another role promotes " +
                              "them; " + name + " is not one",
                          "review required", role->name, std::move(extra));
        return false;
      }
      if (!any_glob_match(role->layers, lease_layer_)) {
        JsonValue extra = JsonValue::object();
        extra.set("layer", JsonValue(lease_layer_));
        error = forbidden(
            "role '" + role->name + "' may not change layer " + lease_layer_ +
                (proposal != nullptr ? " (the target of proposal " + name + ")" : std::string()),
            "layer", role->name, std::move(extra));
        return false;
      }
    }
    const i32 lease_index = d.find_layer(lease_layer_);
    partition_ = d.layer(lease_index >= 0 ? static_cast<u32>(lease_index) : layer).partition();
    records_ = role != nullptr || session_.leases_.required();
    return true;
  }

  void before(doc::ObjectId id) {
    if (!records_ || id.is_null() || seen_.contains(id)) return;
    seen_.insert(id, touched_.size());
    Touched& t = touched_.emplace_back();
    t.id = id;
    place_of(session_.doc_, id, partition_, t.before);
  }

  // Every object `before` recorded, where it stood and where it stands.
  bool finish(RpcError& error) {
    if (!records_) return true;
    Place after;
    for (const Touched& t : touched_) {
      if (t.before.defined && !check(t.id, t.before, error)) return false;
      place_of(session_.doc_, t.id, partition_, after);
      if (after.defined && !check(t.id, after, error)) return false;
    }
    return true;
  }

  void reserve(u32 count) {
    if (!records_) return;
    touched_.reserve(count);
    seen_.reserve(count);
  }

 private:
  struct Touched {
    doc::ObjectId id;
    Place before;
  };

  JsonValue where(doc::ObjectId id, const Place& place) const {
    JsonValue extra = JsonValue::object();
    extra.set("layer", JsonValue(lease_layer_));
    extra.set("object", JsonValue(hex_of(id)));
    if (place.tiled) extra.set("tile", tile_json(place.tile));
    extra.set("type", JsonValue(place.type()));
    return extra;
  }

  std::string describe(doc::ObjectId id, const Place& place) const {
    std::string s = "object " + hex_of(id);
    s += place.tiled ? " in tile " + tile_text(place.tile) : std::string(" (no tile)");
    s += " of layer " + lease_layer_;
    return s;
  }

  bool check(doc::ObjectId id, const Place& place, RpcError& error) const {
    if (const Role* role = caller_->role) {
      if (place.tiled) {
        bool inside = role->tiles.empty();
        for (const TileRange& r : role->tiles)
          inside = inside || tile_in(r, place.tile);
        if (!inside) {
          error = forbidden("role '" + role->name + "' may not change " + describe(id, place) +
                                ": the tile is outside the role's ranges",
                            "tile", role->name, where(id, place));
          return false;
        }
      } else if (!role->untiled) {
        error = forbidden("role '" + role->name + "' may not change " + describe(id, place) +
                              ": the role changes only records that stand in a tile",
                          "untiled", role->name, where(id, place));
        return false;
      }
      const std::string_view type = place.type();
      if (!type.empty() && !any_glob_match(role->object_types, type)) {
        error = forbidden("role '" + role->name + "' may not change objects of type " +
                              std::string(type) + " (" + describe(id, place) + ")",
                          "type", role->name, where(id, place));
        return false;
      }
    }
    if (!session_.leases_.required()) return true;
    const LeaseInfo* other = nullptr;
    const LeaseTable::Cover cover =
        session_.leases_.check(caller_->who.actor, lease_layer_,
                               place.tiled ? &place.tile : nullptr, place.type(), now_ms_, &other);
    if (cover == LeaseTable::Cover::covered) return true;
    JsonValue extra = where(id, place);
    if (cover == LeaseTable::Cover::held_by_other) {
      extra.set("lease", schema::to_json(*other));
      error = make_error(codes::k_lease_conflict,
                         describe(id, place) + " is covered by lease " + std::to_string(other->id) +
                             ", held by " + other->actor +
                             (other->task.empty() ? std::string() : " for task " + other->task) +
                             " until " + std::to_string(other->expires_unix_ms) +
                             " (unix ms): wait for it, or work elsewhere",
                         std::move(extra));
      return false;
    }
    error = make_error(
        codes::k_lease_required,
        "this document requires leases, and " + caller_->who.actor + " holds none covering " +
            describe(id, place) +
            (place.type().empty() ? std::string() : " (type " + std::string(place.type()) + ")") +
            ": lease.acquire its tile or its type on " + lease_layer_ + " first",
        std::move(extra));
    return false;
  }

  Session& session_;
  const Caller* caller_;
  i64 now_ms_;
  std::string lease_layer_;
  doc::LayerPartition partition_;
  bool records_ = false;
  Vector<Touched> touched_;
  HashMap<doc::ObjectId, u32> seen_;
};

// ---- Session ---------------------------------------------------------------------------------

Session::Session(io::Vfs& vfs, std::string id, std::string dir)
    : vfs_(&vfs), id_(std::move(id)), dir_(std::move(dir)) {
  register_side_file_steps();
}

bool Session::create(std::string_view name, RpcError& error) {
  std::string message;
  if (!DocumentStore::create(*vfs_, dir_, name, doc_, manifest_, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return load_side_files(error);
}

bool Session::load(RpcError& error) {
  std::string message;
  if (!DocumentStore::load(*vfs_, dir_, doc_, manifest_, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return load_side_files(error);
}

bool Session::check_writable(RpcError& error) const {
  if (read_only_as_.empty()) return true;
  error = path_not_found("path", read_only_as_, io::Access::write);
  return false;
}

bool Session::persist(RpcError& error) {
  if (!check_writable(error)) return false;
  std::string message;
  if (test_fail_save_) {
    test_fail_save_ = false;
    unsaved_ = true;
    error = make_error(codes::k_io_error, "the document's save failed for the test");
    return false;
  }
  if (!DocumentStore::save(*vfs_, dir_, doc_, manifest_, &message)) {
    // The store leaves the files as they were when a save fails before its log is whole, so they
    // may now hold less than memory does (a commit already journaled, an undo already made).
    unsaved_ = true;
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  unsaved_ = false;
  return true;
}

bool Session::save(RpcError& error) { return persist(error); }

// ---- side files: leases.json, proposals.json, proposals/<layer>.base.jsonl
// -----------------------

std::string Session::base_file(std::string_view proposal) const {
  return DocumentStore::path_of(DocumentStore::path_of(dir_, k_proposals_dir),
                                DocumentStore::layer_dir_name(proposal) + ".base.jsonl");
}

bool Session::load_side_files(RpcError& error) {
  bool present = false;
  // A session that may not be written reads every table without its lock, which would make a lock
  // file in its directory (side_file.h), and removes no gone host's mark: the gate's answer.
  RpcError refused;
  const bool may_write = may_write_side_files(refused);
  LeaseFile leases;
  if (!read_side_file_locked(*vfs_, DocumentStore::path_of(dir_, k_leases_file), leases, present,
                             error, may_write))
    return false;
  // A lease a client connection took lives no longer than the connection, and connections are a
  // host's: one in the file that another host granted was left by a host that has gone, with its
  // client (two hosts on one document at once is unsupported). Dropped here, and the file written
  // again, so it stops standing against everyone else. This host's own stay: a session closed and
  // opened again while the client is still connected keeps its client's lease, and the session
  // manager drops those of this host's connections that ended meanwhile
  // (`drop_ended_connection_leases`).
  // Looked for without the lock, which a document with nothing to drop never takes, and dropped
  // under it from the table as it stands then.
  // A host that is alive holds its mark (`host_mark_path`), so its clients' leases stay: before
  // the mark, `host` alone could not tell a live host from a gone one, and a second host's open (a
  // read any credential may make) ended a live host's socket clients' leases.
  const auto orphan = [this, may_write](const LeaseInfo& l) {
    return l.connection != 0 && l.host != host_instance() &&
           !host_alive(*vfs_, dir_, l.host, may_write);
  };
  bool orphans = false;
  for (const LeaseInfo& l : leases.leases)
    orphans = orphans || orphan(l);
  leases_.assign(std::move(leases));
  // A session that may not be written leaves them in the file, where they refuse nothing it could
  // do anyway.
  if (orphans && may_write &&
      !update_leases(
          [&](bool& write) {
            const LeaseFile& file = leases_.file();
            for (u32 i = file.leases.size(); i > 0; --i) {
              if (!orphan(file.leases[i - 1])) continue;
              leases_.remove(file.leases[i - 1].id);
              write = true;
            }
            return true;
          },
          error))
    return false;

  ProposalsFile proposals;
  if (!read_side_file_locked(*vfs_, DocumentStore::path_of(dir_, k_proposals_file), proposals,
                             present, error, may_write))
    return false;
  proposals_.assign(std::move(proposals));

  transactions_ = TransactionsFile{};
  if (!read_side_file_locked(*vfs_, DocumentStore::path_of(dir_, k_transactions_file),
                             transactions_, present, error, may_write))
    return false;
  for (const ProposalTable::Entry& e : proposals_.entries()) {
    ProposalTable::Entry* entry = proposals_.find(e.info.layer);
    const std::string path = base_file(entry->info.layer);
    std::string text;
    const io::Status s = vfs_->read(path, text);
    if (s == io::Status::NotFound) continue;
    if (s != io::Status::Ok) {
      error = make_error(codes::k_io_error, path + ": " + io::status_name(s));
      return false;
    }
    std::string_view rest = text;
    while (!rest.empty()) {
      const usize nl = rest.find('\n');
      std::string_view line = rest.substr(0, nl);
      rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.empty()) continue;
      JsonValue value;
      ProposalBaseEntry base;
      schema::ReadContext ctx;
      if (!parse_json(line, value).ok || !schema::from_json(base, value, ctx)) {
        error = make_error(codes::k_io_error, path + ": a line that is not a ProposalBaseEntry");
        return false;
      }
      ProposalTable::note_base(*entry, base.id, base.record ? &*base.record : nullptr);
    }
  }
  return true;
}

std::string Session::leases_path() const { return DocumentStore::path_of(dir_, k_leases_file); }

bool Session::hold_host_mark(RpcError& error) {
  if (host_mark_.held()) return true;
  if (!may_write_side_files(error)) return false;
  return lock_side_file(*vfs_, host_mark_path(dir_, host_instance()), host_mark_, error);
}

std::string Session::transactions_path() const {
  return DocumentStore::path_of(dir_, k_transactions_file);
}

bool Session::refresh_leases(RpcError& error) {
  LeaseFile file;
  bool present = false;
  if (!read_side_file_locked(*vfs_, leases_path(), file, present, error, writable())) return false;
  leases_.assign(std::move(file));
  return true;
}

// proposals.json through the side files' one helper: the file is read afresh under its lock and
// only this call's proposal is replaced in it (or added), so two hosts that each change a different
// proposal both keep theirs. It was written whole from this host's table, which dropped every
// proposal another host had opened or closed since this one read the file, and on Windows a reader
// outside the lock made a writer's rename fail. A proposal's layer is the document's, which a host
// still holds in memory and saves whole: that is not made safe here (two hosts changing layers at
// once is unsupported), only the table.
bool Session::save_proposals(std::string_view layer, RpcError& error) {
  if (!may_write_side_files(error)) return false;
  const ProposalTable::Entry* mine = proposals_.find(layer);
  if (mine == nullptr) return true;
  ProposalInfo info = mine->info;
  info.touched = 0;  // derived from the base when reported, never stored
  ProposalsFile file;
  return update_side_file(
      *vfs_, DocumentStore::path_of(dir_, k_proposals_file), file,
      [&](ProposalsFile& f, bool& write) {
        write = true;
        for (ProposalInfo& p : f.proposals) {
          if (p.layer != layer) continue;
          p = std::move(info);
          return true;
        }
        f.proposals.push_back(std::move(info));
        return true;
      },
      error);
}

bool Session::note_proposal_touches(const doc::Patch& patch, RpcError& error) {
  ProposalTable::Entry* entry = proposals_.find(patch.layer);
  if (entry == nullptr || entry->info.state != ProposalState::Open) return true;
  const i32 target = doc_.find_layer(entry->info.target);
  if (target < 0) return true;
  const doc::Layer& t = static_cast<const doc::Document&>(doc_).layer(static_cast<u32>(target));
  std::string lines;
  for (const doc::Command& c : patch.forward) {
    if (!ProposalTable::note_base(*entry, c.id, t.find(c.id))) continue;
    write_json(schema::to_json(entry->base.back()), lines, JsonWriteOptions{.pretty = false});
    lines.push_back('\n');
  }
  if (lines.empty()) return true;
  std::string message;
  if (!DocumentStore::append_side_file(*vfs_, base_file(patch.layer), lines, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return true;
}

// ---- transactions ------------------------------------------------------------------------------

bool Session::apply(std::span<const doc::Command> commands, doc::Attribution attribution,
                    bool atomic, ApplyResult& result, RpcError& error) {
  return commit_commands(commands, std::move(attribution), atomic, true, doc_.edit_layer(), nullptr,
                         result, error);
}

bool Session::apply(std::span<const doc::Command> commands, const Caller& caller, bool atomic,
                    std::string_view layer, ApplyResult& result, RpcError& error) {
  u32 index = doc_.edit_layer();
  if (!layer.empty()) {
    const i32 found = doc_.find_layer(layer);
    if (found < 0) {
      error = make_error(codes::k_not_found, "no layer named " + std::string(layer));
      return false;
    }
    index = static_cast<u32>(found);
  }
  return commit_commands(commands, caller.who, atomic, true, index, &caller, result, error);
}

bool Session::commit_commands(std::span<const doc::Command> commands, doc::Attribution attribution,
                              bool atomic, bool strict, u32 layer, const Caller* caller,
                              ApplyResult& result, RpcError& error, const Staging* staging,
                              u64 transaction) {
  // Refused before the document changes in memory, not at the save (Session::writable).
  if (!check_writable(error)) return false;
  if (attribution.timestamp_unix_ms == 0) attribution.timestamp_unix_ms = time::wall_unix_ms();
  Guard guard(*this, caller, attribution.timestamp_unix_ms);
  if (!guard.begin(layer, false, error)) return false;
  guard.reserve(static_cast<u32>(commands.size()));
  // The patch names the layer it edited, so undo and redo find it again; the session's own edit
  // layer is restored before anything is written, because the manifest records it.
  const u32 edit_layer = doc_.edit_layer();
  doc_.set_edit_layer(layer);
  doc::Transaction tx = doc_.begin(std::move(attribution));
  u32 applied = 0;
  // Staging: the transaction's own commands come first and must all apply again, since another
  // client may have committed under them; the call's own are counted and remembered.
  const u32 held = staging != nullptr ? staging->held : 0;
  bool held_ok = true;
  bool unheld = false;
  for (u32 i = 0; i < commands.size(); ++i) {
    const doc::Command& command = commands[i];
    guard.before(command.id);
    // A client's command may name only blobs the store holds (D13, session_history.cpp): a
    // reference to bytes that are not there would be a damaged document from its first save.
    if (strict && !blobs_held(command, result)) {
      unheld = true;
      if (i < held) held_ok = false;
      continue;
    }
    const bool ok = tx.apply(command, strict);
    if (i < held) {
      held_ok = held_ok && ok;
    } else if (ok) {
      ++applied;
      if (staging != nullptr) staging->accepted->push_back(i - held);
    }
  }
  // **The paths the records gain** (record_paths.h; protocol.md, "Paths a document's records
  // hold"): read off the records as they now stand, composed and in the layer written, so a
  // create, a set, a retype into a placement and a merge's or a promotion's replayed diff are one
  // check. A refused path is refused as any path is, and the whole call with it, before anything
  // is journaled or staged.
  {
    Vector<doc::ObjectId> ids;
    ids.reserve(static_cast<u32>(commands.size()));
    for (const doc::Command& command : commands)
      ids.push_back(command.id);
    RpcError refused_path;
    if (!check_stored_paths(roots_, doc_, layer,
                            std::span<const doc::ObjectId>(ids.data(), ids.size()), dir_,
                            refused_path)) {
      tx.rollback();
      doc_.set_edit_layer(edit_layer);
      if (staging != nullptr) staging->accepted->clear();
      result = ApplyResult{};
      error = std::move(refused_path);
      return false;
    }
  }
  if (staging != nullptr) {
    for (const auto& d : tx.diagnostics()) {
      Diagnostic diagnostic;
      diagnostic.path = d.path;
      diagnostic.message = d.message;
      result.diagnostics.push_back(std::move(diagnostic));
    }
    result.applied = applied;
    const bool refused = !held_ok || applied == 0 || (atomic && (!tx.ok() || unheld));
    // A staging call checks the write as a commit would, then takes it back: the records change
    // only at the transaction's commit.
    const bool allowed = refused || guard.finish(error);
    tx.rollback();
    doc_.set_edit_layer(edit_layer);
    if (refused || !allowed) {
      staging->accepted->clear();
      result.applied = 0;
      if (!allowed) result = ApplyResult{};
      return allowed;
    }
    return true;
  }
  for (const auto& d : tx.diagnostics()) {
    Diagnostic diagnostic;
    diagnostic.path = d.path;
    diagnostic.message = d.message;
    result.diagnostics.push_back(std::move(diagnostic));
  }
  result.applied = applied;
  if (applied == 0 || (atomic && (!tx.ok() || unheld))) {
    tx.rollback();
    doc_.set_edit_layer(edit_layer);
    result.committed = false;
    return true;
  }
  // A refusal takes the whole transaction back: a write is the caller's to make or not, never
  // half of one.
  if (!guard.finish(error)) {
    tx.rollback();
    doc_.set_edit_layer(edit_layer);
    result = ApplyResult{};
    return false;
  }

  // A new commit discards the redo tail.
  const bool truncated = manifest_.undo_position < doc_.journal().size();
  if (truncated) doc_.truncate_journal(manifest_.undo_position);
  tx.set_transaction(transaction);
  tx.commit();
  doc_.set_edit_layer(edit_layer);
  manifest_.undo_position = doc_.journal().size();
  result.committed = true;
  result.patch_index = doc_.journal().size() - 1;

  // Journaled before the save: a save that dies leaves this commit as the first patch of the redo
  // tail, and the undo position the manifest keeps says which state the layer files hold. The
  // store cuts a dropped redo tail off the file in place and appends; it does not rewrite it.
  std::string message;
  const bool journaled = DocumentStore::append_journal(*vfs_, dir_, doc_, &message);
  if (!journaled) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  if (!persist(error)) return false;
  publish_journal("commit", static_cast<u32>(doc_.journal().size() - 1),
                  caller != nullptr ? caller->connection : 0);
  return note_proposal_touches(doc_.journal().back(), error);
}

// The Guard's checks of commit_commands without the commit, for a write of several patches that has
// to be refused before any of them lands (doc.rollback_to, session_history.cpp). The transaction is
// rolled back, so nothing is journaled or saved; its ids are stamped in the change feed twice over,
// which a reader finds unchanged.
bool Session::check_commands(std::span<const doc::Command> commands,
                             const doc::Attribution& attribution, u32 layer, const Caller* caller,
                             RpcError& error) {
  if (commands.empty()) return true;
  const i64 now =
      attribution.timestamp_unix_ms != 0 ? attribution.timestamp_unix_ms : time::wall_unix_ms();
  // The role's and the leases' checks need a caller; the paths the records gain are checked for
  // every writer (record_paths.h), as commit_commands checks them.
  const bool guarded = caller != nullptr;
  Guard guard(*this, caller, now);
  if (guarded && !guard.begin(layer, false, error)) return false;
  if (guarded) guard.reserve(static_cast<u32>(commands.size()));
  const u32 edit_layer = doc_.edit_layer();
  doc_.set_edit_layer(layer);
  doc::Transaction tx = doc_.begin(attribution);
  Vector<doc::ObjectId> ids;
  ids.reserve(static_cast<u32>(commands.size()));
  for (const doc::Command& command : commands) {
    if (guarded) guard.before(command.id);
    ids.push_back(command.id);
    (void)tx.apply(command, false);
  }
  bool ok = !guarded || guard.finish(error);
  if (ok)
    ok = check_stored_paths(roots_, doc_, layer,
                            std::span<const doc::ObjectId>(ids.data(), ids.size()), dir_, error);
  tx.rollback();
  doc_.set_edit_layer(edit_layer);
  return ok;
}

void Session::publish_journal(std::string_view action, u32 index, u64 connection) const {
  if (events_ == nullptr || !events_->wanted(EventKind::Journal, id_)) return;
  if (index >= doc_.journal().size()) return;
  const doc::Patch& patch = doc_.journal()[index];
  JournalEvent event;
  event.session = id_;
  event.connection = connection;
  event.action = std::string(action);
  event.index = index;
  event.layer = patch.layer;
  event.attribution = patch.attribution;
  // Each record once, in the order the commands name them: a patch is short, and the ids are what
  // a subscriber reads again, so a linear check is the right cost.
  u32 total = 0;
  for (const doc::Command& c : patch.forward) {
    bool seen = false;
    for (const Id128& id : event.ids) {
      if (id == c.id) {
        seen = true;
        break;
      }
    }
    if (seen) continue;
    ++total;
    if (event.ids.size() < k_event_max_ids) event.ids.push_back(c.id);
  }
  event.ids_total = total;
  event.revision = doc_.revision();
  event.journal_length = static_cast<u32>(doc_.journal().size());
  event.undo_position = manifest_.undo_position;
  events_->publish(EventKind::Journal, id_, schema::to_json(event));
}

void Session::publish_session(std::string_view change, const LeaseInfo* lease, u64 connection,
                              std::string_view reason) const {
  if (events_ == nullptr || !events_->wanted(EventKind::Session, id_)) return;
  SessionChangeEvent event;
  event.session = id_;
  event.change = std::string(change);
  event.path = shown_path_;  // every subscriber's, so never the host's own path
  event.connection = connection;
  event.reason = std::string(reason);
  if (lease != nullptr) {
    event.lease = lease->id;
    event.actor = lease->actor;
    event.layer = lease->layer;
  }
  events_->publish(EventKind::Session, id_, schema::to_json(event));
}

void Session::publish_ledger(std::string_view change, u64 task, u64 review, std::string_view actor,
                             u64 connection, u64 touched_task) {
  // Every entry the call's own change made is its event's, never "ledger_changed".
  flush_ledger_written(task != 0 ? task : touched_task, review);
  if (events_ == nullptr || !events_->wanted(EventKind::Session, id_)) return;
  SessionChangeEvent event;
  event.session = id_;
  event.change = std::string(change);
  event.path = shown_path_;
  event.connection = connection;
  event.actor = std::string(actor);
  event.task = task;
  event.review = review;
  events_->publish(EventKind::Session, id_, schema::to_json(event));
}

void Session::publish_proposal(std::string_view change, std::string_view layer,
                               std::string_view actor, u64 connection) const {
  if (events_ == nullptr || !events_->wanted(EventKind::Session, id_)) return;
  SessionChangeEvent event;
  event.session = id_;
  event.change = std::string(change);
  event.path = shown_path_;
  event.connection = connection;
  event.actor = std::string(actor);
  event.layer = std::string(layer);
  events_->publish(EventKind::Session, id_, schema::to_json(event));
}

bool Session::merge(const MergeParams& params, MergeResult& result, RpcError& error) {
  return merge(params, nullptr, result, error);
}

bool Session::merge(const MergeParams& params, const Caller* caller, MergeResult& result,
                    RpcError& error) {
  const i32 base = doc_.find_layer(params.base_layer);
  const i32 ours = doc_.find_layer(params.ours_layer);
  const i32 theirs = doc_.find_layer(params.theirs_layer);
  if (base < 0 || ours < 0 || theirs < 0) {
    const std::string& missing =
        base < 0 ? params.base_layer : (ours < 0 ? params.ours_layer : params.theirs_layer);
    error = make_error(codes::k_not_found, "no layer named " + missing);
    return false;
  }
  if (params.output_layer.empty()) {
    error = make_error(codes::k_invalid_argument, "output_layer is required");
    return false;
  }

  doc::MergeOptions options;
  switch (params.prefer) {
    case MergePrefer::Ours: options.prefer_on_conflict = doc::MergeOptions::Ours; break;
    case MergePrefer::Theirs: options.prefer_on_conflict = doc::MergeOptions::Theirs; break;
    case MergePrefer::Neither: options.prefer_on_conflict = doc::MergeOptions::Neither; break;
  }
  doc::MergeResult merged;
  std::string message;
  // The const accessor: reading a layer is not an edit, and the mutable one marks the composed
  // index (and the change feed a materialized world reads) for a rebuild.
  const doc::Document& read = doc_;
  if (!doc::merge_layers(read.layer(static_cast<u32>(base)), read.layer(static_cast<u32>(ours)),
                         read.layer(static_cast<u32>(theirs)), options, merged, &message)) {
    error = make_error(codes::k_document_error, std::move(message));
    return false;
  }
  result.applied_ours = merged.applied_ours;
  result.applied_theirs = merged.applied_theirs;
  for (const doc::MergeConflict& c : merged.conflicts) {
    MergeConflictInfo info;
    info.object = c.object;
    info.property = c.property;
    info.kind = static_cast<MergeConflictKind>(c.kind);
    info.base = c.base;
    info.ours = c.ours;
    info.theirs = c.theirs;
    result.conflicts.push_back(std::move(info));
  }

  i32 output = doc_.find_layer(params.output_layer);
  if (output < 0) {
    // The layer itself is not part of the transaction: undoing the merge empties it again, it
    // does not take it off the stack.
    output = static_cast<i32>(doc_.add_layer(params.output_layer, merged.merged.role()));
    if (!persist(error)) return false;
  }

  const u32 index = static_cast<u32>(output);
  const Vector<doc::Command> commands =
      doc::diff_layers(static_cast<const doc::Document&>(doc_).layer(index), merged.merged);
  doc::Attribution attribution;
  const bool attributed = caller != nullptr && params.attribution.has_value();
  if (attributed) {
    attribution = caller->who;
    const std::string what = "three-way merge of " + params.ours_layer + " and " +
                             params.theirs_layer + " over " + params.base_layer;
    attribution.rationale =
        attribution.rationale.empty() ? what : what + ": " + attribution.rationale;
  } else {
    attribution.actor = "doc.merge";
    attribution.role = "merge";
    attribution.task = params.output_layer;
    attribution.rationale = "three-way merge of " + params.ours_layer + " and " +
                            params.theirs_layer + " over " + params.base_layer;
  }
  ApplyResult applied;
  // The commands replay a diff between two layers, so they are applied the way undo and redo
  // apply theirs: without preconditions that speak about the composed document.
  if (!commit_commands(commands, std::move(attribution), true, false, index, caller, applied,
                       error))
    return false;
  if (!applied.committed && !commands.empty()) {
    std::string why = "the merged layer could not be written";
    if (!applied.diagnostics.empty()) why += ": " + applied.diagnostics[0].message;
    error = make_error(codes::k_document_error, std::move(why));
    return false;
  }
  result.committed = applied.committed;
  result.patch_index = applied.patch_index;
  return true;
}

bool Session::undo(u32 steps, StepResult& result, RpcError& error) {
  return step(true, steps, nullptr, result, error);
}

bool Session::redo(u32 steps, StepResult& result, RpcError& error) {
  return step(false, steps, nullptr, result, error);
}

bool Session::undo(u32 steps, const Caller* caller, StepResult& result, RpcError& error) {
  return step(true, steps, caller, result, error);
}

bool Session::redo(u32 steps, const Caller* caller, StepResult& result, RpcError& error) {
  return step(false, steps, caller, result, error);
}

bool Session::step(bool backwards, u32 steps, const Caller* caller, StepResult& result,
                   RpcError& error) {
  if (!check_writable(error)) return false;
  u32 done = 0;
  const i64 now = time::wall_unix_ms();
  // The patches stepped, in order, for their events once the step is saved.
  Vector<u32> stepped;
  const auto finish = [&](bool ok) {
    result.position = manifest_.undo_position;
    result.stepped = done;
    // Whatever stepped before a refusal stays stepped, and is saved.
    if (done > 0) {
      RpcError save_error;
      if (!persist(save_error)) {
        if (!ok) return false;
        error = std::move(save_error);
        return false;
      }
      for (const u32 at : stepped)
        publish_journal(backwards ? "undo" : "redo", at,
                        caller != nullptr ? caller->connection : 0);
    }
    return ok;
  };
  while (done < steps) {
    const bool more =
        backwards ? manifest_.undo_position > 0 : manifest_.undo_position < doc_.journal().size();
    if (!more) break;
    const u32 at = backwards ? manifest_.undo_position - 1 : manifest_.undo_position;
    const doc::Patch& patch = doc_.journal()[at];
    const i32 layer = doc_.find_layer(patch.layer);
    if (layer < 0) {
      error = make_error(codes::k_document_error, std::string(backwards ? "undo" : "redo") +
                                                      " failed at patch " + std::to_string(at) +
                                                      ": its layer " + patch.layer + " is gone");
      return finish(false);
    }
    Guard guard(*this, caller, now);
    if (!guard.begin(static_cast<u32>(layer), true, error)) return finish(false);
    guard.reserve(patch.forward.size());
    for (const doc::Command& c : patch.forward)
      guard.before(c.id);
    if (!(backwards ? doc_.undo(patch) : doc_.redo(patch))) {
      error = make_error(codes::k_document_error, std::string(backwards ? "undo" : "redo") +
                                                      " failed at patch " + std::to_string(at));
      return finish(false);
    }
    // An undo or a redo that brings a path back brings it back to this host, whose roots may not be
    // the ones it was written under (another host, another machine): admitted again.
    Vector<doc::ObjectId> ids;
    ids.reserve(static_cast<u32>(patch.forward.size()));
    for (const doc::Command& c : patch.forward)
      ids.push_back(c.id);
    if (!guard.finish(error) ||
        !check_stored_paths(roots_, doc_, static_cast<u32>(layer),
                            std::span<const doc::ObjectId>(ids.data(), ids.size()), dir_, error)) {
      // Put the step back: undo and redo of one patch are each other's inverse.
      (void)(backwards ? doc_.redo(patch) : doc_.undo(patch));
      return finish(false);
    }
    manifest_.undo_position = backwards ? at : at + 1;
    stepped.push_back(at);
    ++done;
  }
  return finish(true);
}

namespace {

// A partition is either absent (one file) or complete. A tile size of zero would silently mean
// "one file" and hide a caller's mistake, so it is refused instead.
bool check_partition(const std::optional<doc::LayerPartition>& partition, RpcError& error) {
  if (!partition.has_value() || partition->tile_size > 0) return true;
  error = make_error(codes::k_invalid_argument, "partition tile_size must be greater than zero");
  return false;
}

}  // namespace

bool Session::add_layer(std::string_view name, doc::LayerRole role, bool make_edit,
                        const std::optional<doc::LayerPartition>& partition, RpcError& error) {
  if (name.empty()) {
    error = make_error(codes::k_invalid_argument, "layer name is required");
    return false;
  }
  if (doc_.find_layer(name) >= 0) {
    error = make_error(codes::k_invalid_argument, "layer already exists: " + std::string(name));
    return false;
  }
  if (!check_partition(partition, error)) return false;
  const u32 index = doc_.add_layer(std::string(name), role);
  if (partition.has_value()) doc_.set_layer_partition(index, *partition);
  if (make_edit) doc_.set_edit_layer(index);
  return persist(error);
}

bool Session::set_partition(std::string_view name,
                            const std::optional<doc::LayerPartition>& partition, RpcError& error) {
  const i32 index = doc_.find_layer(name);
  if (index < 0) {
    error = make_error(codes::k_not_found, "no layer named " + std::string(name));
    return false;
  }
  if (!check_partition(partition, error)) return false;
  std::string message;
  if (!DocumentStore::repartition(*vfs_, dir_, doc_, manifest_, static_cast<u32>(index),
                                  partition.value_or(doc::LayerPartition{}), &message, nullptr)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return true;
}

bool Session::set_edit_layer(std::string_view name, RpcError& error) {
  const i32 index = doc_.find_layer(name);
  if (index < 0) {
    error = make_error(codes::k_not_found, "no layer named " + std::string(name));
    return false;
  }
  doc_.set_edit_layer(static_cast<u32>(index));
  return persist(error);
}

void Session::layers(Vector<LayerInfo>& out) const {
  out.clear();
  for (u32 i = 0; i < doc_.layer_count(); ++i) {
    const doc::Layer& layer = doc_.layer(i);
    LayerInfo info;
    info.name = layer.name();
    info.role = layer.role();
    info.records = layer.size();
    info.is_edit = i == doc_.edit_layer();
    if (layer.partitioned()) {
      info.partition = layer.partition();
      info.tiles = doc::tile_count(layer);
    }
    if (const ProposalTable::Entry* p = proposals_.find(layer.name())) {
      ProposalInfo proposal = p->info;
      proposal.touched = p->base.size();
      info.proposal = std::move(proposal);
    }
    out.push_back(std::move(info));
  }
}

SessionInfo Session::info() const {
  SessionInfo info;
  info.session = id_;
  info.path = dir_;
  info.name = manifest_.name;
  layers(info.layers);
  info.journal_length = doc_.journal().size();
  info.undo_position = manifest_.undo_position;
  for (const u64 h : holders_)
    info.holders.push_back(h);
  return info;
}

// ---- holders -----------------------------------------------------------------------------------

bool Session::held_by(u64 holder) const noexcept {
  for (const u64 h : holders_) {
    if (h == holder) return true;
  }
  return false;
}

void Session::hold(u64 holder) {
  if (held_by(holder)) return;
  // In order, so `holders` reads the same however the clients arrived.
  u32 at = holders_.size();
  while (at > 0 && holders_[at - 1] > holder)
    --at;
  holders_.insert(at, holder);
}

bool Session::let_go(u64 holder) {
  for (u32 i = 0; i < holders_.size(); ++i) {
    if (holders_[i] == holder) {
      holders_.erase_at(i);
      return true;
    }
  }
  return false;
}

u32 Session::release_connection_leases(u64 connection, i64 now_ms) {
  if (connection == 0) return 0;
  bool any = false;
  for (const LeaseInfo& l : leases_.leases())
    any = any || l.connection == connection;
  if (!any) return 0;
  // Nobody is waiting for an answer: a save that fails leaves the lease in the file, where the
  // next open of the document drops it anyway (it names a connection of a host that has gone).
  Vector<LeaseInfo> gone;
  RpcError ignored;
  (void)update_leases(
      [&](bool& write) {
        for (const LeaseInfo& l : leases_.leases()) {
          if (l.connection == connection && l.host == host_instance()) gone.push_back(l);
        }
        for (const LeaseInfo& l : gone)
          leases_.remove(l.id);
        write = !gone.empty() || leases_.expire(now_ms) > 0;
        return true;
      },
      ignored);
  for (const LeaseInfo& l : gone)
    publish_session("lease_released", &l, connection, "connection closed");
  return gone.size();
}

u32 Session::drop_ended_connection_leases(std::span<const u64> ended) {
  if (ended.empty()) return 0;
  const auto ended_here = [&](const LeaseInfo& l) {
    if (l.connection == 0 || l.host != host_instance()) return false;
    for (const u64 c : ended) {
      if (c == l.connection) return true;
    }
    return false;
  };
  bool any = false;
  for (const LeaseInfo& l : leases_.leases())
    any = any || ended_here(l);
  if (!any) return 0;
  // As at the load: a save that fails leaves the lease in the file, and it is dropped again at the
  // next open.
  Vector<u64> gone;
  RpcError ignored;
  (void)update_leases(
      [&](bool& write) {
        for (const LeaseInfo& l : leases_.leases()) {
          if (ended_here(l)) gone.push_back(l.id);
        }
        for (const u64 id : gone)
          leases_.remove(id);
        write = !gone.empty();
        return true;
      },
      ignored);
  return gone.size();
}

// ---- leases ------------------------------------------------------------------------------------

namespace {

constexpr f64 k_max_ttl_seconds = 24.0 * 3600.0;

bool check_ttl(f64 ttl_seconds, RpcError& error) {
  if (ttl_seconds >= 1.0 && ttl_seconds <= k_max_ttl_seconds) return true;
  error = make_error(codes::k_invalid_argument,
                     "ttl_seconds must be from 1 to 86400 (a lease is time-bounded)");
  return false;
}

i64 expiry(i64 now_ms, f64 ttl_seconds) { return now_ms + static_cast<i64>(ttl_seconds * 1000.0); }

bool check_actor(const Caller& caller, RpcError& error) {
  if (caller.who.actor.find_first_not_of(" \t") != std::string::npos) return true;
  error = make_error(codes::k_invalid_argument,
                     "attribution.actor is required: a lease and a proposal belong to someone");
  return false;
}

}  // namespace

bool Session::acquire_lease(const LeaseAcquireParams& params, const Caller& caller, i64 now_ms,
                            LeaseInfo& out, RpcError& error) {
  if (!check_actor(caller, error) || !check_ttl(params.ttl_seconds, error)) return false;
  const i32 layer = doc_.find_layer(params.layer);
  if (layer < 0) {
    error = make_error(codes::k_not_found, "no layer named " + params.layer);
    return false;
  }
  if (const ProposalTable::Entry* p = proposals_.find(params.layer)) {
    error = make_error(codes::k_invalid_argument,
                       params.layer + " is a proposal layer: lease its target, " + p->info.target +
                           ". An edit in a proposal is covered by a lease on the layer it "
                           "promotes into");
    return false;
  }
  if (params.tiles.empty() == params.object_types.empty()) {
    error = make_error(codes::k_invalid_argument,
                       "a lease names tiles or object_types, one of the two and not both");
    return false;
  }
  for (const TileRange& r : params.tiles) {
    if (r.x0 > r.x1 || r.y0 > r.y1) {
      error = make_error(codes::k_invalid_argument,
                         "a tile range's low corner (x0, y0) must not be above its high one");
      return false;
    }
  }
  for (const std::string& t : params.object_types) {
    if (t.empty() || t.find('*') != std::string::npos) {
      error = make_error(codes::k_invalid_argument,
                         "object_types are exact qualified type names, with no wildcard");
      return false;
    }
  }
  if (const Role* role = caller.role) {
    JsonValue extra = JsonValue::object();
    extra.set("layer", JsonValue(params.layer));
    if (!any_glob_match(role->layers, params.layer)) {
      error = forbidden("role '" + role->name + "' may not change layer " + params.layer +
                            ", so it may not lease it",
                        "layer", role->name, std::move(extra));
      return false;
    }
    for (const TileRange& r : params.tiles) {
      if (!role->tiles.empty() && !range_within(r, role->tiles)) {
        error = forbidden("role '" + role->name + "' may not lease tiles (" + std::to_string(r.x0) +
                              ", " + std::to_string(r.y0) + ")..(" + std::to_string(r.x1) + ", " +
                              std::to_string(r.y1) + "): the range is not inside one of the role's",
                          "tile", role->name, std::move(extra));
        return false;
      }
    }
    for (const std::string& t : params.object_types) {
      if (!any_glob_match(role->object_types, t)) {
        error = forbidden("role '" + role->name + "' may not lease type " + t, "type", role->name,
                          std::move(extra));
        return false;
      }
    }
  }
  LeaseInfo wanted;
  wanted.actor = caller.who.actor;
  wanted.role = caller.who.role;
  wanted.task = caller.who.task;
  wanted.layer = params.layer;
  wanted.tiles = params.tiles;
  wanted.object_types = params.object_types;
  wanted.acquired_unix_ms = now_ms;
  wanted.expires_unix_ms = expiry(now_ms, params.ttl_seconds);
  // A client's lease lives as long as its connection at most (protocol.md, "Several clients, one
  // host"): one that vanishes must not hold tiles for the rest of a ttl nobody will renew.
  wanted.connection = caller.connection;
  if (caller.connection != 0) wanted.host = host_instance();
  // The mark another host's open tells this host's live leases by (load_side_files).
  if (caller.connection != 0 && !hold_host_mark(error)) return false;
  const bool ok = update_leases(
      [&](bool& write) {
        if (leases_.expire(now_ms) > 0) write = true;
        if (const LeaseInfo* held = leases_.overlap(wanted, now_ms)) {
          JsonValue data = JsonValue::object();
          data.set("lease", schema::to_json(*held));
          error = make_error(codes::k_lease_conflict,
                             "lease " + std::to_string(held->id) + " on " + held->layer +
                                 ", held by " + held->actor +
                                 (held->task.empty() ? std::string() : " for task " + held->task) +
                                 ", overlaps what was asked for; it expires at " +
                                 std::to_string(held->expires_unix_ms) +
                                 " (unix ms). Ask again after that, or for tiles nobody holds",
                             std::move(data));
          return false;
        }
        const u64 id = leases_.add(std::move(wanted));
        out = *leases_.find(id);
        write = true;
        return true;
      },
      error);
  if (!ok) return false;
  publish_session("lease_acquired", &out, caller.connection);
  return true;
}

bool Session::renew_lease(u64 lease, f64 ttl_seconds, const Caller& caller, i64 now_ms,
                          LeaseInfo& out, RpcError& error) {
  if (!check_actor(caller, error) || !check_ttl(ttl_seconds, error)) return false;
  return update_leases(
      [&](bool& write) {
        LeaseInfo* held = leases_.find(lease);
        if (held == nullptr || held->expires_unix_ms <= now_ms) {
          const bool lapsed = held != nullptr;
          if (lapsed) write = leases_.expire(now_ms) > 0;
          error = make_error(codes::k_not_found,
                             "no live lease " + std::to_string(lease) +
                                 (lapsed ? ": it expired and was dropped; lease.acquire again"
                                         : ": lease.list shows the live ones"));
          return false;
        }
        if (held->actor != caller.who.actor) {
          error = forbidden("lease " + std::to_string(lease) + " is held by " + held->actor +
                                ", and only its holder renews it",
                            "lease holder", caller.who.role);
          return false;
        }
        held->expires_unix_ms = expiry(now_ms, ttl_seconds);
        out = *held;
        write = true;
        return true;
      },
      error);
}

bool Session::release_lease(u64 lease, const Caller& caller, i64 now_ms, LeaseInfo& out,
                            RpcError& error) {
  if (!check_actor(caller, error)) return false;
  const bool ok = update_leases(
      [&](bool& write) {
        const LeaseInfo* held = leases_.find(lease);
        if (held == nullptr) {
          error = make_error(codes::k_not_found, "no lease " + std::to_string(lease) +
                                                     " (an expired lease is dropped by itself)");
          return false;
        }
        if (held->actor != caller.who.actor) {
          error = forbidden("lease " + std::to_string(lease) + " is held by " + held->actor +
                                ", and only its holder releases it",
                            "lease holder", caller.who.role);
          return false;
        }
        out = *held;
        leases_.remove(lease);
        (void)leases_.expire(now_ms);
        write = true;
        return true;
      },
      error);
  if (!ok) return false;
  publish_session("lease_released", &out, caller.connection);
  return true;
}

bool Session::list_leases(std::string_view layer, std::string_view actor, i64 now_ms,
                          LeasesResult& out, RpcError& error) {
  return update_leases(
      [&](bool& write) {
        out.expired = leases_.expire(now_ms);
        write = out.expired > 0;
        out.required = leases_.required();
        out.now_unix_ms = now_ms;
        for (const LeaseInfo& l : leases_.leases()) {
          if (!layer.empty() && l.layer != layer) continue;
          if (!actor.empty() && l.actor != actor) continue;
          out.leases.push_back(l);
        }
        return true;
      },
      error, /*read=*/true);
}

bool Session::require_leases(bool required, i64 now_ms, LeasesResult& out, RpcError& error) {
  return update_leases(
      [&](bool& write) {
        leases_.set_required(required);
        out.expired = leases_.expire(now_ms);
        out.required = required;
        out.now_unix_ms = now_ms;
        for (const LeaseInfo& l : leases_.leases())
          out.leases.push_back(l);
        write = true;
        return true;
      },
      error);
}

// ---- proposal layers ---------------------------------------------------------------------------

bool Session::propose_layer(const ProposeLayerParams& params, const Caller& caller,
                            RpcError& error) {
  if (!check_actor(caller, error)) return false;
  if (params.name.empty()) {
    error = make_error(codes::k_invalid_argument, "name is required");
    return false;
  }
  const i32 target = doc_.find_layer(params.target);
  if (target < 0) {
    error = make_error(codes::k_not_found, "no layer named " + params.target);
    return false;
  }
  const doc::Document& read = doc_;
  const doc::Layer& target_layer = read.layer(static_cast<u32>(target));
  if (proposals_.find(params.target) != nullptr ||
      target_layer.role() == doc::LayerRole::Proposal) {
    error = make_error(codes::k_invalid_argument,
                       params.target +
                           " is itself a proposal: a proposal targets a layer "
                           "that holds accepted work");
    return false;
  }
  if (const Role* role = caller.role;
      role != nullptr && !any_glob_match(role->layers, params.target)) {
    JsonValue extra = JsonValue::object();
    extra.set("layer", JsonValue(params.target));
    error = forbidden("role '" + role->name + "' may not change layer " + params.target +
                          ", so it may not propose into it",
                      "layer", role->name, std::move(extra));
    return false;
  }
  const i64 now = time::wall_unix_ms();
  const i32 existing = doc_.find_layer(params.name);
  if (existing >= 0) {
    ProposalTable::Entry* entry = proposals_.find(params.name);
    if (entry == nullptr) {
      error = make_error(codes::k_invalid_argument, "layer already exists: " + params.name);
      return false;
    }
    ProposalInfo& info = entry->info;
    if (info.state == ProposalState::Open) {
      error = make_error(codes::k_invalid_argument,
                         "proposal " + params.name + " is already open, owned by " + info.owner);
      return false;
    }
    // A closed proposal is empty unless an undo took its promotion's emptying back; either way
    // its records merge afresh, against the target as it stands, once it is reopened.
    if (info.owner != caller.who.actor || info.target != params.target) {
      error = make_error(codes::k_invalid_argument,
                         "proposal " + params.name + " (" + info.owner + ", over " + info.target +
                             ") can be reopened only by its owner, over the same target; choose "
                             "another name");
      return false;
    }
    info.state = ProposalState::Open;
    info.owner_role = caller.who.role;
    info.task = caller.who.task;
    info.rationale = caller.who.rationale;
    info.opened_unix_ms = now;
    info.closed_by.clear();
    info.closed_unix_ms = 0;
    info.patch_index.reset();
    ProposalTable::clear_base(*entry);
    (void)vfs_->remove(base_file(params.name));
    if (params.edit) {
      doc_.set_edit_layer(static_cast<u32>(existing));
      if (!persist(error)) return false;
    }
    if (!save_proposals(params.name, error)) return false;
    publish_proposal("proposal_reopened", params.name, caller.who.actor, caller.connection);
    return true;
  }

  std::optional<doc::LayerPartition> partition;
  if (target_layer.partitioned()) partition = target_layer.partition();
  if (!add_layer(params.name, doc::LayerRole::Proposal, params.edit, partition, error))
    return false;
  ProposalInfo info;
  info.layer = params.name;
  info.target = params.target;
  info.owner = caller.who.actor;
  info.owner_role = caller.who.role;
  info.task = caller.who.task;
  info.rationale = caller.who.rationale;
  info.opened_unix_ms = now;
  proposals_.add(std::move(info));
  (void)vfs_->remove(base_file(params.name));  // a leftover of an earlier layer of that name
  if (!save_proposals(params.name, error)) return false;
  publish_proposal("proposal_opened", params.name, caller.who.actor, caller.connection);
  return true;
}

bool Session::clear_layer(u32 layer, doc::Attribution attribution, std::optional<u32>& patch_index,
                          RpcError& error) {
  Vector<doc::Command> commands;
  for (auto [id, record] : static_cast<const doc::Document&>(doc_).layer(layer).records())
    commands.push_back(doc::cmd_remove_record(id));
  patch_index.reset();
  if (commands.empty()) return true;
  ApplyResult applied;
  if (!commit_commands(commands, std::move(attribution), true, false, layer, nullptr, applied,
                       error))
    return false;
  if (applied.committed) patch_index = applied.patch_index;
  return true;
}

namespace {

// A diagnostic's identity for comparing two validations: its path and its message.
std::string key_of(const doc::Diagnostic& d) {
  std::string key = d.path;
  key.push_back('\n');
  key.append(d.message);
  return key;
}

}  // namespace

bool Session::promote(const PromoteParams& params, const Caller& caller, PromoteResult& result,
                      RpcError& error) {
  ProposalTable::Entry* entry = proposals_.find(params.proposal);
  if (entry == nullptr) {
    error = make_error(codes::k_not_found, "no proposal named " + params.proposal);
    return false;
  }
  if (entry->info.state != ProposalState::Open) {
    error =
        make_error(codes::k_invalid_argument,
                   "proposal " + params.proposal + " is already " +
                       (entry->info.state == ProposalState::Promoted ? "promoted" : "rejected"));
    return false;
  }
  const i32 p = doc_.find_layer(entry->info.layer);
  const i32 t = doc_.find_layer(entry->info.target);
  if (p < 0 || t < 0) {
    error = make_error(codes::k_document_error, "proposal " + params.proposal + " or its target " +
                                                    entry->info.target +
                                                    " is no longer in the document");
    return false;
  }
  if (const Role* role = caller.role) {
    if (role->review_required) {
      error = forbidden("role '" + role->name + "' needs review, so it may propose but not " +
                            "promote: a role without review promotes " + params.proposal,
                        "review required", role->name);
      return false;
    }
    if (!any_glob_match(role->layers, entry->info.target)) {
      JsonValue extra = JsonValue::object();
      extra.set("layer", JsonValue(entry->info.target));
      error = forbidden("role '" + role->name + "' may not change layer " + entry->info.target +
                            ", so it may not promote into it",
                        "layer", role->name, std::move(extra));
      return false;
    }
  }
  result.proposal = entry->info.layer;
  result.target = entry->info.target;

  // The merge. `ours` is the target now; `base` is the target with each record the proposal holds
  // put back as the proposal first found it (proposals.h); `theirs` is that base with the
  // proposal's opinions composed on. A record the proposal holds with no recorded base is merged
  // against the target as it stands, so it can take the proposal's side but never conflict.
  i64 started = time::monotonic_ns();
  const doc::Document& read = doc_;
  const doc::Layer& target = read.layer(static_cast<u32>(t));
  const doc::Layer& proposal = read.layer(static_cast<u32>(p));
  doc::Layer base = target;
  for (auto [id, record] : proposal.records()) {
    const ProposalBaseEntry* b = ProposalTable::base_of(*entry, id);
    if (b == nullptr) continue;
    if (b->record.has_value()) {
      base.set(*b->record);
    } else {
      base.remove(id);
    }
  }
  const doc::Layer theirs = compose_onto(base, proposal);
  doc::MergeOptions options;
  switch (params.prefer) {
    case PromotePrefer::Refuse:
    case PromotePrefer::Target: options.prefer_on_conflict = doc::MergeOptions::Ours; break;
    case PromotePrefer::Proposal: options.prefer_on_conflict = doc::MergeOptions::Theirs; break;
    case PromotePrefer::Base: options.prefer_on_conflict = doc::MergeOptions::Neither; break;
  }
  doc::MergeResult merged;
  std::string message;
  if (!doc::merge_layers(base, target, theirs, options, merged, &message)) {
    error = make_error(codes::k_document_error, std::move(message));
    return false;
  }
  result.applied_target = merged.applied_ours;
  result.applied_proposal = merged.applied_theirs;
  for (const doc::MergeConflict& c : merged.conflicts) {
    MergeConflictInfo info;
    info.object = c.object;
    info.property = c.property;
    info.kind = static_cast<MergeConflictKind>(c.kind);
    info.base = c.base;
    info.ours = c.ours;
    info.theirs = c.theirs;
    result.conflicts.push_back(std::move(info));
  }
  // What this merge shows, and the refusal of a promotion decided on a dry run that showed
  // something else (protocol.md, "What a promotion is tied to"): checked before validation and
  // before anything is committed, so a refused promotion changes nothing.
  result.digest =
      promotion_digest(entry->info.layer, entry->info.target, proposal, result.conflicts);
  if (!params.digest.empty() && params.digest != result.digest) {
    JsonValue data = JsonValue::object();
    data.set("reason", JsonValue("proposal moved"));
    data.set("proposal", JsonValue(entry->info.layer));
    data.set("digest", JsonValue(result.digest));
    error = make_error(codes::k_invalid_argument,
                       "proposal " + params.proposal +
                           " or the target's values at its conflicts changed since the dry run "
                           "this promotion was decided on: dry-run it again and decide on what it "
                           "shows now",
                       std::move(data));
    return false;
  }
  const Vector<doc::Command> commands = doc::diff_layers(target, merged.merged);
  {
    doc::ObjectId last;
    for (const doc::Command& c : commands) {
      if (result.records == 0 || !(c.id == last)) ++result.records;
      last = c.id;
    }
  }
  result.merge_ms = ms_since(started);

  // Validation, before anything is committed: the accepted world — every layer that is not a
  // proposal — as it is, and with the target replaced by the merged layer. What the second finds
  // and the first did not is what the promotion would introduce; problems already there, or in
  // other agents' proposals, are not this promotion's to fix and do not block it.
  started = time::monotonic_ns();
  {
    doc::Document world;
    u32 world_target = 0;
    for (u32 i = 0; i < read.layer_count(); ++i) {
      const doc::Layer& l = read.layer(i);
      if (l.role() == doc::LayerRole::Proposal || proposals_.find(l.name()) != nullptr) continue;
      if (i == static_cast<u32>(t)) world_target = world.layer_count();
      world.add_layer(doc::Layer(l));
    }
    Vector<doc::Diagnostic> before;
    (void)world.validate(before);
    world.layer(world_target) = std::move(merged.merged);
    Vector<doc::Diagnostic> after;
    (void)world.validate(after);
    // A multiset of what was there: a problem found twice before and three times after is one new.
    HashMap<std::string, u32> known;
    for (const doc::Diagnostic& d : before)
      ++known[key_of(d)];
    for (const doc::Diagnostic& d : after) {
      if (u32* n = known.find_value(key_of(d)); n != nullptr && *n > 0) {
        --*n;
        continue;
      }
      Diagnostic item;
      item.path = d.path;
      item.message = d.message;
      result.validation.push_back(std::move(item));
    }
  }
  result.validate_ms = ms_since(started);

  if (!result.conflicts.empty() && params.prefer == PromotePrefer::Refuse) {
    result.refused = std::to_string(result.conflicts.size()) +
                     " conflict(s) between the proposal and what the target took since the "
                     "proposal first touched those records: resolve them, or promote again with "
                     "prefer \"Target\", \"Proposal\" or \"Base\"";
  } else if (!result.validation.empty()) {
    result.refused = "the merged world has " + std::to_string(result.validation.size()) +
                     " validation problem(s) it does not have now; fix them in the proposal";
  }
  result.ok = result.refused.empty();
  if (!result.ok || params.dry_run) return true;

  // Commit: the target's patch, under the promoter, with the proposal's own reason in the
  // rationale (plan 06 §6.8: an accepted proposal records why next to the change), and then the
  // proposal's emptying patch. Neither is checked against leases: the promotion is the merge the
  // leases existed to keep clean, and a record the proposal holds was checked when it was written.
  started = time::monotonic_ns();
  ProposalInfo& info = entry->info;
  doc::Attribution attribution = caller.who;
  attribution.timestamp_unix_ms = 0;
  std::string why = "promote " + info.layer + " (" + info.owner +
                    (info.task.empty() ? std::string() : ", task " + info.task) +
                    (info.rationale.empty() ? std::string() : ": " + info.rationale) + ") into " +
                    info.target;
  if (!caller.who.rationale.empty()) why += ": " + caller.who.rationale;
  attribution.rationale = std::move(why);
  ApplyResult applied;
  if (!commit_commands(commands, attribution, true, false, static_cast<u32>(t), nullptr, applied,
                       error))
    return false;
  if (!applied.committed && !commands.empty()) {
    std::string reason = "the merged target could not be written";
    if (!applied.diagnostics.empty()) reason += ": " + applied.diagnostics[0].message;
    error = make_error(codes::k_document_error, std::move(reason));
    return false;
  }
  if (applied.committed) result.patch_index = applied.patch_index;
  // Closed before it is emptied, so the emptying patch records no merge base.
  entry = proposals_.find(params.proposal);
  entry->info.state = ProposalState::Promoted;
  entry->info.closed_by = caller.who.actor;
  entry->info.closed_unix_ms = time::wall_unix_ms();
  entry->info.patch_index = result.patch_index;
  attribution.rationale = "empty " + info.layer + ": promoted into " + info.target;
  if (!clear_layer(static_cast<u32>(p), std::move(attribution), result.cleared_patch_index, error))
    return false;
  entry = proposals_.find(params.proposal);
  ProposalTable::clear_base(*entry);
  (void)vfs_->remove(base_file(params.proposal));
  if (!save_proposals(params.proposal, error)) return false;
  result.commit_ms = ms_since(started);
  result.promoted = true;
  publish_proposal("proposal_promoted", params.proposal, caller.who.actor, caller.connection);
  return true;
}

bool Session::reject(const RejectParams& params, const Caller& caller, RpcError& error) {
  ProposalTable::Entry* entry = proposals_.find(params.proposal);
  if (entry == nullptr) {
    error = make_error(codes::k_not_found, "no proposal named " + params.proposal);
    return false;
  }
  if (entry->info.state != ProposalState::Open) {
    error = make_error(codes::k_invalid_argument, "proposal " + params.proposal + " is not open");
    return false;
  }
  const i32 p = doc_.find_layer(entry->info.layer);
  if (p < 0) {
    error = make_error(codes::k_document_error,
                       "proposal " + params.proposal + " is no longer in the document");
    return false;
  }
  // Its owner may always withdraw it; anyone else needs a role that may change the target.
  if (const Role* role = caller.role; role != nullptr && caller.who.actor != entry->info.owner &&
                                      !any_glob_match(role->layers, entry->info.target)) {
    error = forbidden("role '" + role->name + "' may not change layer " + entry->info.target +
                          ", so it may not reject a proposal into it",
                      "layer", role->name);
    return false;
  }
  entry->info.state = ProposalState::Rejected;
  entry->info.closed_by = caller.who.actor;
  entry->info.closed_unix_ms = time::wall_unix_ms();
  doc::Attribution attribution = caller.who;
  attribution.timestamp_unix_ms = 0;
  attribution.rationale =
      "reject " + params.proposal +
      (caller.who.rationale.empty() ? std::string() : ": " + caller.who.rationale);
  std::optional<u32> cleared;
  if (!clear_layer(static_cast<u32>(p), std::move(attribution), cleared, error)) return false;
  entry = proposals_.find(params.proposal);
  ProposalTable::clear_base(*entry);
  (void)vfs_->remove(base_file(params.proposal));
  if (!save_proposals(params.proposal, error)) return false;
  publish_proposal("proposal_rejected", params.proposal, caller.who.actor, caller.connection);
  return true;
}

// ---- SessionManager --------------------------------------------------------------------------

Session* SessionManager::open(const SessionOpenParams& params, u64 holder, RpcError& error,
                              std::string_view read_only_as, std::string_view shown,
                              const DocumentGuard* guard) {
  if (params.path.empty()) {
    error = make_error(codes::k_invalid_argument, "path is required");
    return nullptr;
  }
  // Native paths are normalized so the same directory spelled two ways is one session.
  const std::string path =
      params.path.find("://") == std::string::npos ? io::normalize_path(params.path) : params.path;
  for (const auto& s : sessions_) {
    if (s->dir() == path) {
      // One document, however many clients: the second opener shares the first's session.
      if (read_only_as.empty()) s->set_read_only({});
      // A session opened with no guard (an in-process caller's) is protected from the first open
      // that brings one; a guarded one is never protected twice.
      if (guard != nullptr && guard->begin != nullptr && s->guard().begin == nullptr) {
        s->set_guard(*guard);
        guard->begin(guard->context, s->dir());
      }
      s->hold(holder);
      return s.get();
    }
  }
  auto session = std::make_unique<Session>(*vfs_, "s" + std::to_string(next_id_), path);
  session->set_read_only(std::string(read_only_as));
  if (DocumentStore::exists(*vfs_, path)) {
    if (!session->load(error)) return nullptr;
  } else if (params.create) {
    const std::string name = params.name.empty() ? std::string(io::file_name(path)) : params.name;
    if (!session->create(name, error)) return nullptr;
  } else {
    error = make_error(codes::k_not_found, "no document at " + path);
    return nullptr;
  }
  ++next_id_;
  // What a connection left here when this document last closed in this process: the leases of
  // the ones that have ended since go, and the transactions of the ones still connected are
  // theirs again.
  (void)session->drop_ended_connection_leases({ended_.data(), ended_.size()});
  for (u32 i = parked_.size(); i > 0; --i) {
    if (parked_[i - 1].dir != path) continue;
    session->restore_transaction_hold(parked_[i - 1].hold);
    parked_.erase_at(i - 1);
  }
  session->set_events(events_);
  session->set_roots(roots_);
  // A ledger another process writes is pushed only where there is someone to push it to.
  session->watch_ledger(watches_ledgers() ? &ledger_waker_ : nullptr);
  session->set_shown_path(std::string(shown));
  session->hold(holder);
  if (guard != nullptr && guard->begin != nullptr) {
    session->set_guard(*guard);
    guard->begin(guard->context, session->dir());
  }
  sessions_.push_back(std::move(session));
  sessions_.back()->publish_session("opened");
  return sessions_.back().get();
}

void SessionManager::set_events(Subscriptions* events) noexcept {
  events_ = events;
  for (const auto& s : sessions_)
    s->set_events(events);
  sync_ledger_watches();
}

void SessionManager::set_roots(const io::Roots* roots) noexcept {
  roots_ = roots;
  for (const auto& s : sessions_)
    s->set_roots(roots);
}

Session* SessionManager::find(std::string_view id) noexcept {
  for (const auto& s : sessions_) {
    if (s->id() == id) return s.get();
  }
  return nullptr;
}

Session* SessionManager::require(std::string_view id, RpcError& error) noexcept {
  Session* s = find(id);
  if (s == nullptr) {
    error = make_error(codes::k_session_not_found, id.empty() ? std::string("session is required")
                                                              : "no session " + std::string(id));
  }
  return s;
}

bool SessionManager::close(std::string_view id) {
  for (u32 i = 0; i < sessions_.size(); ++i) {
    if (sessions_[i]->id() == id) {
      note_closing(*sessions_[i]);
      sessions_[i]->publish_session("closed");
      // Its directory is free once it is closed, however it came to close: a session.close, a
      // dropped connection, the host's own clean-up.
      const DocumentGuard guard = sessions_[i]->guard();
      const std::string dir = sessions_[i]->dir();
      sessions_.erase_at(i);
      if (guard.end != nullptr) guard.end(guard.context, dir);
      return true;
    }
  }
  return false;
}

bool SessionManager::release(std::string_view id, u64 holder, bool& closed) {
  closed = false;
  Session* s = find(id);
  if (s == nullptr || !s->let_go(holder)) return false;
  if (s->holders().empty()) closed = close(id);
  return true;
}

void SessionManager::note_closing(const Session& session) {
  for (const LeaseInfo& l : session.leases().leases()) {
    if (l.connection == 0 || l.host != host_instance()) continue;
    u32 at = detached_.size();
    while (at > 0 && detached_[at - 1] > l.connection)
      --at;
    if (at > 0 && detached_[at - 1] == l.connection) continue;
    detached_.insert(at, l.connection);
  }
  for (const Session::TransactionHold& h : session.transaction_holds())
    parked_.push_back({std::string(session.dir()), h});
}

void SessionManager::release_connection(u64 connection, i64 now_ms) {
  if (connection == 0) return;
  // What it left in documents no session has open now: a lease there is dropped when the
  // document is next opened, and a transaction's hold is let go now.
  for (u32 i = 0; i < detached_.size(); ++i) {
    if (detached_[i] != connection) continue;
    detached_.erase_at(i);
    ended_.push_back(connection);
    break;
  }
  for (u32 i = parked_.size(); i > 0; --i) {
    if (parked_[i - 1].hold.connection == connection) parked_.erase_at(i - 1);
  }
  // Leases first, on every session (a client may lease on a session it did not open, such as the
  // host's --doc), then the holds; a session closed by the second loop is gone, so it walks a copy
  // of the ids.
  for (const auto& s : sessions_) {
    (void)s->release_connection_leases(connection, now_ms);
    (void)s->release_connection_transactions(connection);
  }
  Vector<std::string> held;
  for (const auto& s : sessions_) {
    if (s->held_by(connection)) held.push_back(s->id());
  }
  for (const std::string& id : held) {
    bool closed = false;
    (void)release(id, connection, closed);
  }
}

}  // namespace engine::protocol
