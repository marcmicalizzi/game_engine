// A session's blobs and checkpoints (roadmap D13, D14; docs/subsystems/protocol.md, "Blobs and
// checkpoints"). The store and the repository are domain/doc's (blob_store.h, checkpoint.h); what is
// here is what a session adds: the checks a client's write gets, the proposals' merge bases as roots
// a collection keeps, and a rollback made of journaled, undoable patches rather than files put back.
#include <core/hash/base64.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/doc/blob_store.h>
#include <domain/doc/checkpoint.h>
#include <domain/protocol/session.h>

namespace engine::protocol {

using doc::BlobStore;
using doc::Checkpoints;

namespace {

bool same_partition(const doc::LayerPartition& a, const doc::LayerPartition& b) {
  const bool at = a.tile_size > 0;
  const bool bt = b.tile_size > 0;
  if (at != bt) return false;
  return !at || (a.tile_size == b.tile_size && a.property == b.property);
}

// The error a failed checkpoint call answers: 1006 when there is no git to run at all.
RpcError checkpoint_error(std::string message) {
  std::string version, why;
  if (!Checkpoints::available(&version, &why)) return make_error(codes::k_unavailable, why);
  return make_error(codes::k_io_error, std::move(message));
}

}  // namespace

bool Session::blobs_held(const doc::Command& command, ApplyResult& result) const {
  // Most commands name no blob; the walk is over the command's own value and record.
  HashSet<std::string> names;
  BlobStore::references(command, names);
  bool held = true;
  for (const std::string& name : names) {
    if (BlobStore::has(*vfs_, dir_, name)) continue;
    Diagnostic d;
    d.path = doc::describe_path(doc_.layer(doc_.edit_layer()).name(), command.id, command.name);
    d.message = "names blob " + name +
                ", which the document's store does not hold: doc.put_blob it first";
    result.diagnostics.push_back(std::move(d));
    held = false;
  }
  return held;
}

bool Session::put_blob(std::string_view bytes, std::string_view media, BlobResult& out,
                       RpcError& error) {
  std::string message;
  if (!BlobStore::put(*vfs_, dir_, bytes, media, out.ref, &out.created, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  out.path = BlobStore::path_of(dir_, out.ref.blob);
  return true;
}

bool Session::collect_blobs(const CollectBlobsParams& params, CollectBlobsResult& out,
                            RpcError& error) {
  if (!(params.min_age_s >= 0)) {
    error = make_error(codes::k_invalid_argument, "min_age_s must be zero or more");
    return false;
  }
  // An open proposal's merge base is the target's records as the proposal first found them, which a
  // promotion with prefer Base can write back: their references are kept like the journal's.
  Vector<JsonValue> roots;
  for (const ProposalTable::Entry& entry : proposals_.entries())
    for (const ProposalBaseEntry& base : entry.base)
      roots.push_back(schema::to_json(base));
  doc::BlobCollectOptions options;
  options.min_age_ms = static_cast<i64>(params.min_age_s * 1000.0);
  options.dry_run = params.dry_run;
  doc::BlobCollectReport report;
  std::string message;
  if (!BlobStore::collect(*vfs_, dir_, doc_, std::span<const JsonValue>(roots.data(), roots.size()),
                          options, report, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  out.stored = report.stored;
  out.stored_bytes = report.stored_bytes;
  out.referenced = report.referenced;
  out.kept_young = report.kept_young;
  out.removed_bytes = report.removed_bytes;
  out.removed = std::move(report.removed);
  out.missing = std::move(report.missing);
  return true;
}

bool Session::checkpoint(std::string_view label, const Caller& caller, doc::CheckpointInfo& out,
                         RpcError& error) {
  // The label is a client's text that becomes the commit's subject: refused, not altered.
  std::string message;
  if (!Checkpoints::check_label(label, &message)) {
    error = make_error(codes::k_invalid_argument, std::move(message));
    return false;
  }
  // Every change is saved before its answer, so this writes nothing unless a load found files out
  // of their canonical form; then the checkpoint holds the canonical ones.
  if (!persist(error)) return false;
  doc::Attribution who = caller.who;
  if (who.timestamp_unix_ms == 0) who.timestamp_unix_ms = time::wall_unix_ms();
  if (!Checkpoints::take(*vfs_, dir_, doc_, manifest_, label, who, out, &message)) {
    error = checkpoint_error(std::move(message));
    return false;
  }
  publish_session("checkpoint", nullptr, caller.connection);
  return true;
}

bool Session::rollback_to(const RollbackParams& params, const Caller& caller, RollbackResult& out,
                          RpcError& error) {
  std::string message;
  doc::CheckpointInfo info;
  if (!Checkpoints::find(*vfs_, dir_, params.checkpoint, info, &message)) {
    std::string version, why;
    error = Checkpoints::available(&version, &why) ? make_error(codes::k_not_found, message)
                                                   : make_error(codes::k_unavailable, why);
    return false;
  }
  doc::CheckpointState state;
  if (!Checkpoints::read(*vfs_, dir_, info.id, state, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  out = RollbackResult{};
  out.checkpoint = info;
  out.dry_run = params.dry_run;

  doc::Attribution who = caller.who;
  if (who.timestamp_unix_ms == 0) who.timestamp_unix_ms = time::wall_unix_ms();
  const std::string what =
      "rollback to checkpoint " + info.id.substr(0, 12) + " \"" + info.label + "\"";
  who.rationale = who.rationale.empty() ? what : what + ": " + who.rationale;

  // The plan, layer by layer. The checkpoint's proposal layers are not restored: a proposal is its
  // owner's work in progress and the protocol's table, not the accepted world.
  struct Step {
    std::string name;
    std::string action;
    i32 index = -1;  // in the stack now; -1 until an added layer is appended
    doc::LayerRole role = doc::LayerRole::Base;
    const doc::Layer* target = nullptr;  // null: emptied
    Vector<doc::Command> commands;
    bool repartition = false;
    doc::LayerPartition partition;
    std::optional<doc::LayerPartition> original;  // the form it has now, for a take-back
  };
  Vector<Step> steps;
  const doc::Document& read = doc_;
  for (u32 i = 0; i < read.layer_count(); ++i) {
    const doc::Layer& now = read.layer(i);
    Step step;
    step.name = now.name();
    step.index = static_cast<i32>(i);
    step.role = now.role();
    if (now.partitioned()) step.original = now.partition();
    if (proposals_.find(now.name()) != nullptr || now.role() == doc::LayerRole::Proposal) {
      step.action = "kept";
      steps.push_back(std::move(step));
      continue;
    }
    for (const doc::Layer& l : state.layers)
      if (l.name() == now.name() && l.role() != doc::LayerRole::Proposal) step.target = &l;
    if (step.target != nullptr) {
      step.action = "restored";
      step.commands = doc::diff_layers(now, *step.target);
      step.partition = step.target->partition();
      step.repartition = !same_partition(now.partition(), step.target->partition());
    } else {
      step.action = "emptied";
      step.commands = doc::diff_layers(now, doc::Layer(now.name(), now.role()));
    }
    steps.push_back(std::move(step));
  }
  for (const doc::Layer& l : state.layers) {
    if (l.role() == doc::LayerRole::Proposal || read.find_layer(l.name()) >= 0) continue;
    Step step;
    step.name = l.name();
    step.action = "added";
    step.role = l.role();
    step.target = &l;
    step.commands = doc::diff_layers(doc::Layer(l.name(), l.role()), l);
    step.partition = l.partition();
    step.repartition = l.partitioned();
    steps.push_back(std::move(step));
  }

  // Blobs the checkpoint's layers name that the store has collected since.
  HashSet<std::string> named;
  for (const doc::Layer& l : state.layers)
    for (const auto& entry : l.records())
      for (const auto& property : entry.second.properties)
        BlobStore::references(property.second, named);
  Vector<std::string> missing;
  for (const std::string& name : named)
    if (!BlobStore::has(*vfs_, dir_, name)) missing.push_back(name);

  // Every existing layer's write is checked before anything changes.
  for (const Step& step : steps) {
    if (step.index < 0 || step.commands.empty()) continue;
    if (!check_commands(step.commands, who, static_cast<u32>(step.index), &caller, error))
      return false;
  }
  const auto report = [&](const Step& step, std::optional<u32> patch) {
    RollbackLayer layer;
    layer.layer = step.name;
    layer.action = step.action;
    layer.records = static_cast<u32>(step.commands.size());
    layer.patch_index = patch;
    layer.repartitioned = step.repartition && step.action != "kept";
    out.layers.push_back(std::move(layer));
  };
  if (params.dry_run) {
    for (const Step& step : steps)
      report(step, std::nullopt);
    out.blobs_restored = static_cast<u32>(missing.size());
    out.edit_layer = read.find_layer(state.manifest.edit_layer) >= 0
                         ? state.manifest.edit_layer
                         : read.layer(read.edit_layer()).name();
    return true;
  }

  // From here the rollback changes the document, and it lands whole or not at all (b68 review
  // B10): the checks above passed for every layer, so what can still refuse a layer is a save that
  // fails, and then everything this rollback changed is taken back — the layers that had landed
  // undone newest first, as `doc.undo` would (they stand in the redo tail, which the next commit
  // drops, as a commit whose save died does), the storage forms put back and the added layers taken
  // off — and the refusal is answered. Only if that fails too does part of it stand, and then the
  // error's `data` is the RollbackResult of exactly what stands: `doc.undo` with its `patches`
  // steps takes those back. Blobs written back from the checkpoint stay; a collection decides.
  const u32 start = manifest_.undo_position;
  Vector<u32> landed;  // the steps whose patch landed, in order
  Vector<u32> reformed;  // the steps whose storage form was changed, in order
  u32 added = 0;
  const auto take_back = [&](RpcError cause) {
    u32 standing = manifest_.undo_position - start;
    std::string why;
    if (test_fail_take_back_) {
      why = "refused for the test";
    } else if (standing > 0) {
      StepResult stepped;
      RpcError undo_error;
      const bool ok = this->step(true, standing, nullptr, stepped, undo_error);
      standing -= stepped.stepped;
      if (!ok || standing > 0) why = undo_error.message.empty() ? "undo stopped" : undo_error.message;
    }
    if (why.empty()) {
      for (u32 i = reformed.size(); i-- > 0;) {
        const Step& s = steps[reformed[i]];
        RpcError form_error;
        if (s.action != "added" && !set_partition(s.name, s.original, form_error) && why.empty())
          why = form_error.message;
      }
    }
    if (why.empty() && added > 0) {
      for (u32 n = 0; n < added; ++n)
        (void)doc_.remove_layer(doc_.layer_count() - 1);
      added = 0;
      RpcError save_error;
      if (!persist(save_error)) why = save_error.message;
    }
    if (why.empty()) {
      error = make_error(cause.code, cause.message +
                                         "; nothing of the rollback stands: what had landed was "
                                         "taken back");
      return false;
    }
    RollbackResult stands;
    stands.checkpoint = out.checkpoint;
    stands.blobs_restored = out.blobs_restored;
    stands.patches = standing;
    stands.committed = standing > 0;
    for (u32 i = 0; i < standing && i < landed.size(); ++i) {
      const Step& s = steps[landed[i]];
      RollbackLayer layer;
      layer.layer = s.name;
      layer.action = s.action;
      layer.records = static_cast<u32>(s.commands.size());
      layer.patch_index = start + i;
      layer.repartitioned = s.repartition;
      stands.layers.push_back(std::move(layer));
    }
    stands.edit_layer = static_cast<const doc::Document&>(doc_).layer(doc_.edit_layer()).name();
    error = make_error(cause.code,
                       cause.message + "; the rollback could not be taken back (" + why + "): " +
                           std::to_string(standing) + " of its layers stand, and doc.undo with " +
                           std::to_string(standing) + " steps takes them back",
                       schema::to_json(stands));
    return false;
  };

  // Layers the checkpoint had and the stack lost come back on top, empty, outside the transactions
  // (as a merge's new output layer does): undoing the rollback empties them, it does not take them
  // off the stack. Each is then checked as the others were, and a refusal takes them off again.
  for (Step& step : steps) {
    if (step.action != "added") continue;
    step.index = static_cast<i32>(doc_.add_layer(step.name, step.role));
    ++added;
  }
  if (added > 0) {
    for (const Step& step : steps) {
      if (step.action != "added" || step.commands.empty()) continue;
      if (!check_commands(step.commands, who, static_cast<u32>(step.index), &caller, error)) {
        for (u32 n = 0; n < added; ++n)
          (void)doc_.remove_layer(doc_.layer_count() - 1);
        return false;
      }
    }
    if (RpcError save_error; !persist(save_error)) return take_back(std::move(save_error));
  }

  // The bytes the checkpoint's references need, from the checkpoint itself.
  if (!missing.empty()) {
    Vector<std::pair<std::string, std::string>> blobs;
    if (!Checkpoints::read_blobs(*vfs_, dir_, info.id,
                                 std::span<const std::string>(missing.data(), missing.size()),
                                 blobs, &message))
      return take_back(make_error(codes::k_io_error, std::move(message)));
    for (const auto& [name, bytes] : blobs) {
      doc::BlobRef ref;
      if (!BlobStore::put(*vfs_, dir_, bytes, "", ref, nullptr, &message) || ref.blob != name)
        return take_back(
            make_error(codes::k_io_error, "blob " + name + " could not be restored: " + message));
      ++out.blobs_restored;
    }
  }

  // Storage forms first (no record changes, nothing journaled), then one patch per layer.
  for (u32 i = 0; i < steps.size(); ++i) {
    const Step& step = steps[i];
    if (!step.repartition || step.action == "kept") continue;
    const std::optional<doc::LayerPartition> form =
        step.partition.tile_size > 0 ? std::optional<doc::LayerPartition>(step.partition)
                                     : std::nullopt;
    RpcError form_error;
    if (!set_partition(step.name, form, form_error)) return take_back(std::move(form_error));
    reformed.push_back(i);
  }
  u32 changed = 0;  // the layers with a patch, counted for fail_rollback_for_test
  for (u32 i = 0; i < steps.size(); ++i) {
    const Step& step = steps[i];
    if (step.action == "kept" || step.commands.empty()) {
      report(step, std::nullopt);
      continue;
    }
    if (changed++ == test_fail_rollback_at_)
      return take_back(make_error(codes::k_io_error, "layer " + step.name +
                                                         " could not be saved (refused for the "
                                                         "test)"));
    ApplyResult applied;
    RpcError commit_error;
    const u32 before = manifest_.undo_position;
    // The commands replay a diff, so they are applied as a merge's are: without the preconditions
    // that speak about the composed document.
    const bool ok = commit_commands(step.commands, who, true, false, static_cast<u32>(step.index),
                                    &caller, applied, commit_error);
    // A commit whose save failed has still landed in the document and the journal.
    if (manifest_.undo_position > before) landed.push_back(i);
    if (!ok) return take_back(std::move(commit_error));
    if (!applied.committed) {
      std::string why = "layer " + step.name + " could not be rolled back";
      if (!applied.diagnostics.empty()) why += ": " + applied.diagnostics[0].message;
      return take_back(make_error(codes::k_document_error, std::move(why)));
    }
    ++out.patches;
    report(step, applied.patch_index);
  }

  const i32 edit = doc_.find_layer(state.manifest.edit_layer);
  if (edit >= 0 && static_cast<u32>(edit) != doc_.edit_layer()) {
    doc_.set_edit_layer(static_cast<u32>(edit));
    if (!persist(error)) return false;
  }
  // The const accessor: the mutable one would mark the composed index for a rebuild.
  out.edit_layer = read.layer(read.edit_layer()).name();
  out.committed = out.patches > 0;
  publish_session("rollback", nullptr, caller.connection);
  return true;
}

}  // namespace engine::protocol
