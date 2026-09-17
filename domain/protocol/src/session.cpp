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

// ---- Session ---------------------------------------------------------------------------------

Session::Session(io::Vfs& vfs, std::string id, std::string dir)
    : vfs_(&vfs), id_(std::move(id)), dir_(std::move(dir)) {}

bool Session::create(std::string_view name, RpcError& error) {
  std::string message;
  if (!DocumentStore::create(*vfs_, dir_, name, doc_, manifest_, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return true;
}

bool Session::load(RpcError& error) {
  std::string message;
  if (!DocumentStore::load(*vfs_, dir_, doc_, manifest_, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return true;
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

bool Session::apply(std::span<const doc::Command> commands, doc::Attribution attribution,
                    bool atomic, ApplyResult& result, RpcError& error) {
  return commit_commands(commands, std::move(attribution), atomic, true, doc_.edit_layer(), result,
                         error);
}

bool Session::commit_commands(std::span<const doc::Command> commands, doc::Attribution attribution,
                              bool atomic, bool strict, u32 layer, ApplyResult& result,
                              RpcError& error) {
  if (attribution.timestamp_unix_ms == 0) attribution.timestamp_unix_ms = time::wall_unix_ms();
  // The patch names the layer it edited, so undo and redo find it again; the session's own edit
  // layer is restored before anything is written, because the manifest records it.
  const u32 edit_layer = doc_.edit_layer();
  doc_.set_edit_layer(layer);
  doc::Transaction tx = doc_.begin(std::move(attribution));
  u32 applied = 0;
  for (const doc::Command& command : commands) {
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

  // A new commit discards the redo tail.
  const bool truncated = manifest_.undo_position < doc_.journal().size();
  if (truncated) doc_.truncate_journal(manifest_.undo_position);
  tx.commit();
  doc_.set_edit_layer(edit_layer);
  manifest_.undo_position = doc_.journal().size();
  result.committed = true;
  result.patch_index = doc_.journal().size() - 1;

  std::string message;
  const bool journaled =
      truncated ? DocumentStore::write_journal(*vfs_, dir_, doc_.journal(), &message)
                : DocumentStore::append_journal(*vfs_, dir_, doc_.journal().back(), &message);
  if (!journaled) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return persist(error);
}

bool Session::merge(const MergeParams& params, MergeResult& result, RpcError& error) {
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
  if (!doc::merge_layers(doc_.layer(static_cast<u32>(base)), doc_.layer(static_cast<u32>(ours)),
                         doc_.layer(static_cast<u32>(theirs)), options, merged, &message)) {
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
  const Vector<doc::Command> commands = doc::diff_layers(doc_.layer(index), merged.merged);
  doc::Attribution attribution;
  attribution.actor = "doc.merge";
  attribution.role = "merge";
  attribution.task = params.output_layer;
  attribution.rationale = "three-way merge of " + params.ours_layer + " and " +
                          params.theirs_layer + " over " + params.base_layer;
  ApplyResult applied;
  // The commands replay a diff between two layers, so they are applied the way undo and redo
  // apply theirs: without preconditions that speak about the composed document.
  if (!commit_commands(commands, std::move(attribution), true, false, index, applied, error))
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
  u32 done = 0;
  while (done < steps && manifest_.undo_position > 0) {
    const doc::Patch& patch = doc_.journal()[manifest_.undo_position - 1];
    if (!doc_.undo(patch)) {
      error = make_error(codes::k_document_error,
                         "undo failed at patch " + std::to_string(manifest_.undo_position - 1));
      return false;
    }
    --manifest_.undo_position;
    ++done;
  }
  result.position = manifest_.undo_position;
  result.stepped = done;
  return done == 0 || persist(error);
}

bool Session::redo(u32 steps, StepResult& result, RpcError& error) {
  u32 done = 0;
  while (done < steps && manifest_.undo_position < doc_.journal().size()) {
    const doc::Patch& patch = doc_.journal()[manifest_.undo_position];
    if (!doc_.redo(patch)) {
      error = make_error(codes::k_document_error,
                         "redo failed at patch " + std::to_string(manifest_.undo_position));
      return false;
    }
    ++manifest_.undo_position;
    ++done;
  }
  result.position = manifest_.undo_position;
  result.stepped = done;
  return done == 0 || persist(error);
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
