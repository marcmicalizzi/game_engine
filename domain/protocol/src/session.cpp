#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/doc/merge.h>
#include <domain/doc/partition.h>
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

template <class T>
bool read_json_file(const io::Vfs& vfs, const std::string& path, T& out, bool& present,
                    RpcError& error) {
  std::string text;
  const io::Status s = vfs.read(path, text);
  present = s == io::Status::Ok;
  if (s == io::Status::NotFound) return true;
  if (s != io::Status::Ok) {
    error = make_error(codes::k_io_error, path + ": " + io::status_name(s));
    return false;
  }
  JsonValue value;
  if (const JsonParseResult r = parse_json(text, value); !r.ok) {
    error = make_error(codes::k_io_error, path + ": " + r.message);
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, value, ctx)) {
    std::string message = path + ": invalid";
    if (!ctx.diagnostics.empty()) message += ": " + ctx.diagnostics[0].message;
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return true;
}

// Written beside the document through the store (DocumentStore::write_side_file): atomically, not
// at all when the bytes are the ones there, and without `Vfs::write`'s question about every parent
// directory, which costs more than the write on Windows (docs/subsystems/doc.md, "Writing a file").
template <class T>
bool write_json_file(const io::Vfs& vfs, const std::string& path, const T& value, RpcError& error) {
  std::string text = write_json(schema::to_json(value));
  text.push_back('\n');
  std::string message;
  if (!DocumentStore::write_side_file(vfs, path, text, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return true;
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
    : vfs_(&vfs), id_(std::move(id)), dir_(std::move(dir)) {}

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

bool Session::persist(RpcError& error) {
  std::string message;
  if (!DocumentStore::save(*vfs_, dir_, doc_, manifest_, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
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
  LeaseFile leases;
  if (!read_json_file(*vfs_, DocumentStore::path_of(dir_, k_leases_file), leases, present, error))
    return false;
  leases_.assign(std::move(leases));

  ProposalsFile proposals;
  if (!read_json_file(*vfs_, DocumentStore::path_of(dir_, k_proposals_file), proposals, present,
                      error))
    return false;
  proposals_.assign(std::move(proposals));
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

bool Session::save_leases(RpcError& error) {
  return write_json_file(*vfs_, DocumentStore::path_of(dir_, k_leases_file), leases_.file(), error);
}

bool Session::save_proposals(RpcError& error) {
  return write_json_file(*vfs_, DocumentStore::path_of(dir_, k_proposals_file),
                         proposals_.to_file(), error);
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
                              ApplyResult& result, RpcError& error) {
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
  for (const doc::Command& command : commands) {
    guard.before(command.id);
    if (tx.apply(command, strict)) ++applied;
  }
  for (const auto& d : tx.diagnostics()) {
    Diagnostic diagnostic;
    diagnostic.path = d.path;
    diagnostic.message = d.message;
    result.diagnostics.push_back(std::move(diagnostic));
  }
  result.applied = applied;
  if (applied == 0 || (atomic && !tx.ok())) {
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
  return note_proposal_touches(doc_.journal().back(), error);
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
  u32 done = 0;
  const i64 now = time::wall_unix_ms();
  const auto finish = [&](bool ok) {
    result.position = manifest_.undo_position;
    result.stepped = done;
    // Whatever stepped before a refusal stays stepped, and is saved.
    if (done > 0) {
      RpcError save_error;
      if (!persist(save_error) && ok) {
        error = std::move(save_error);
        return false;
      }
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
    if (!guard.finish(error)) {
      // Put the step back: undo and redo of one patch are each other's inverse.
      (void)(backwards ? doc_.redo(patch) : doc_.undo(patch));
      return finish(false);
    }
    manifest_.undo_position = backwards ? at : at + 1;
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
  return info;
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
  const u32 expired = leases_.expire(now_ms);
  LeaseInfo wanted;
  wanted.actor = caller.who.actor;
  wanted.role = caller.who.role;
  wanted.task = caller.who.task;
  wanted.layer = params.layer;
  wanted.tiles = params.tiles;
  wanted.object_types = params.object_types;
  wanted.acquired_unix_ms = now_ms;
  wanted.expires_unix_ms = expiry(now_ms, params.ttl_seconds);
  if (const LeaseInfo* held = leases_.overlap(wanted, now_ms)) {
    JsonValue data = JsonValue::object();
    data.set("lease", schema::to_json(*held));
    error = make_error(codes::k_lease_conflict,
                       "lease " + std::to_string(held->id) + " on " + held->layer + ", held by " +
                           held->actor +
                           (held->task.empty() ? std::string() : " for task " + held->task) +
                           ", overlaps what was asked for; it expires at " +
                           std::to_string(held->expires_unix_ms) +
                           " (unix ms). Ask again after that, or for tiles nobody holds",
                       std::move(data));
    if (expired > 0) {
      RpcError ignored;
      (void)save_leases(ignored);
    }
    return false;
  }
  const u64 id = leases_.add(std::move(wanted));
  if (!save_leases(error)) return false;
  out = *leases_.find(id);
  return true;
}

bool Session::renew_lease(u64 lease, f64 ttl_seconds, const Caller& caller, i64 now_ms,
                          LeaseInfo& out, RpcError& error) {
  if (!check_actor(caller, error) || !check_ttl(ttl_seconds, error)) return false;
  LeaseInfo* held = leases_.find(lease);
  if (held == nullptr || held->expires_unix_ms <= now_ms) {
    const bool lapsed = held != nullptr;
    if (lapsed) {
      leases_.expire(now_ms);
      RpcError ignored;
      (void)save_leases(ignored);
    }
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
  return save_leases(error);
}

bool Session::release_lease(u64 lease, const Caller& caller, i64 now_ms, LeaseInfo& out,
                            RpcError& error) {
  if (!check_actor(caller, error)) return false;
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
  return save_leases(error);
}

bool Session::list_leases(std::string_view layer, std::string_view actor, i64 now_ms,
                          LeasesResult& out, RpcError& error) {
  out.expired = leases_.expire(now_ms);
  out.required = leases_.required();
  out.now_unix_ms = now_ms;
  for (const LeaseInfo& l : leases_.leases()) {
    if (!layer.empty() && l.layer != layer) continue;
    if (!actor.empty() && l.actor != actor) continue;
    out.leases.push_back(l);
  }
  return out.expired == 0 || save_leases(error);
}

bool Session::require_leases(bool required, i64 now_ms, LeasesResult& out, RpcError& error) {
  leases_.set_required(required);
  out.expired = leases_.expire(now_ms);
  out.required = required;
  out.now_unix_ms = now_ms;
  for (const LeaseInfo& l : leases_.leases())
    out.leases.push_back(l);
  return save_leases(error);
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
    return save_proposals(error);
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
  return save_proposals(error);
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
  if (!save_proposals(error)) return false;
  result.commit_ms = ms_since(started);
  result.promoted = true;
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
  return save_proposals(error);
}

// ---- SessionManager --------------------------------------------------------------------------

Session* SessionManager::open(const SessionOpenParams& params, RpcError& error) {
  if (params.path.empty()) {
    error = make_error(codes::k_invalid_argument, "path is required");
    return nullptr;
  }
  // Native paths are normalized so the same directory spelled two ways is one session.
  const std::string path =
      params.path.find("://") == std::string::npos ? io::normalize_path(params.path) : params.path;
  for (const auto& s : sessions_) {
    if (s->dir() == path) return s.get();
  }
  auto session = std::make_unique<Session>(*vfs_, "s" + std::to_string(next_id_), path);
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
  sessions_.push_back(std::move(session));
  return sessions_.back().get();
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
      sessions_.erase_at(i);
      return true;
    }
  }
  return false;
}

}  // namespace engine::protocol
