#pragma once

// The authoring document (docs/plan/03-data-model.md §3.2–3.3, ADR-0002).
//
// A Document is an ordered stack of Layers, weakest first. Each layer holds sparse
// ObjectRecords: a record with a type defines an object; a record without one overrides
// properties, the parent, or deletion of an object defined in a weaker layer. Composition is
// "strongest layer wins per (object, property)"; there are no other composition operators.
//
// All mutation goes through Commands applied to the document's edit layer. Every applied
// command yields an inverse (a RestoreRecord of the edit layer's previous record), so
// transactions can roll back and patches can be undone. Layers serialize to canonical JSON
// through the generated LayerFile schema, so files diff cleanly in git.
//
// Composition is answered from an index, not by scanning: a Document keeps every id's resolved
// position in the layer stack and the live children of every parent, maintained by apply, undo,
// redo, add_layer, and remove_layer. Never scan the layers for an id; ask the document.
//
// v0 scope: single-process, in-memory. A layer is one file, or a set of tile files when it
// carries a partition (domain/doc/partition.h). Leases and the journal's git integration
// arrive later.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/hash_map.h>
#include <core/containers/hash_set.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>

#include <optional>
#include <schemas/doc.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::doc {

using ObjectId = Id128;
using schema::Diagnostic;

class Layer {
 public:
  Layer(std::string name, LayerRole role) : name_(std::move(name)), role_(role) {}

  const std::string& name() const noexcept { return name_; }
  LayerRole role() const noexcept { return role_; }

  const ObjectRecord* find(ObjectId id) const noexcept { return records_.find_value(id); }
  ObjectRecord* find(ObjectId id) noexcept { return records_.find_value(id); }
  // Returns the record for `id`, creating an empty override record if absent.
  ObjectRecord& ensure(ObjectId id);
  void set(ObjectRecord record);
  bool remove(ObjectId id) noexcept { return records_.erase(id) != 0; }
  const FlatMap<ObjectId, ObjectRecord>& records() const noexcept { return records_; }
  u32 size() const noexcept { return records_.size(); }

  // Storage form (domain/doc/partition.h): a set of tile files when tile_size is greater than
  // zero, one file otherwise. It says how the layer is written, not what it holds, so it takes
  // no part in equality: two layers with the same records are equal however they are stored.
  const LayerPartition& partition() const noexcept { return partition_; }
  bool partitioned() const noexcept { return partition_.tile_size > 0; }
  void set_partition(LayerPartition partition) { partition_ = std::move(partition); }

  // Canonical file form: records in id order. A tile file is this, restricted to the records
  // of one tile, so a partitioned layer's files carry the same header as a single-file one.
  LayerFile to_file() const;
  LayerFile to_file(std::span<const ObjectId> ids) const;
  static Layer from_file(LayerFile&& file);
  std::string to_json_text() const;
  static bool from_json_text(std::string_view text, Layer& out, schema::ReadContext& ctx);

  friend bool operator==(const Layer& a, const Layer& b) {
    return a.name_ == b.name_ && a.role_ == b.role_ && a.records_ == b.records_;
  }

 private:
  std::string name_;
  LayerRole role_;
  LayerPartition partition_;
  FlatMap<ObjectId, ObjectRecord> records_;
};

// The composed view of one object. Views into layer storage; valid until the next mutation.
struct ResolvedObject {
  ObjectId id;
  std::string_view type;
  ObjectId parent;  // null id = root
  FlatMap<std::string_view, const JsonValue*> properties;
  u32 defining_layer = 0;
  bool deleted = false;
};

class Transaction;

class Document {
 public:
  Document() = default;
  ENGINE_NON_COPYABLE(Document);
  Document(Document&&) noexcept = default;
  Document& operator=(Document&&) noexcept = default;

  // --- layers -----------------------------------------------------------------------------

  // Appends a layer; later layers are stronger. Returns its index. The edit layer stays where
  // it was: a fresh document edits its first layer, and a caller that wants to edit the new
  // one says so with set_edit_layer.
  u32 add_layer(std::string name, LayerRole role);
  u32 add_layer(Layer&& layer);
  // Takes a layer off the stack with everything it holds. The edit layer follows it down when
  // it was above, and a document never loses its last layer, so removing it fails. Layer
  // indices below the removed one are unchanged; every index above it drops by one, which is
  // why this rebuilds the composed index instead of patching it.
  bool remove_layer(u32 index);
  u32 layer_count() const noexcept { return layers_.size(); }
  // The mutable accessor hands out storage the composed index describes, so it marks the index
  // for a rebuild on the next query. Prefer apply() for edits and the const accessor for reads.
  Layer& layer(u32 index) noexcept {
    index_dirty_ = true;
    return layers_[index];
  }
  const Layer& layer(u32 index) const noexcept { return layers_[index]; }
  // Index of the named layer, or -1.
  i32 find_layer(std::string_view name) const noexcept;
  // Storage form of a layer (domain/doc/partition.h). Setting it changes no record, so the
  // composed index is untouched; DocumentStore::repartition writes the new form.
  void set_layer_partition(u32 index, LayerPartition partition);

  void set_edit_layer(u32 index) noexcept;
  u32 edit_layer() const noexcept { return edit_layer_; }

  // --- composed queries -------------------------------------------------------------------
  //
  // Every one of these is answered from the composed index: a hash lookup, plus a visit to the
  // layers that actually hold a record for the id. None of them walks the layer stack.

  // Defined in some layer, whether or not deleted.
  bool is_defined(ObjectId id) const noexcept;
  // Defined and not deleted.
  bool exists(ObjectId id) const noexcept;
  bool resolve(ObjectId id, ResolvedObject& out) const;
  // Composed property value, or nullptr when the object or property is absent.
  const JsonValue* property(ObjectId id, std::string_view name) const noexcept;
  // Live objects, sorted by id.
  Vector<ObjectId> objects() const;
  // Live children of `parent` (null id = roots), sorted by id.
  Vector<ObjectId> children(ObjectId parent) const;
  // Live objects, without building the list.
  u32 object_count() const noexcept;

  // --- composed index -----------------------------------------------------------------------

  // Recomputes the index from the layers. Queries do this by themselves after the mutable
  // layer accessor has been used; call it directly only to pay the cost at a chosen moment.
  void rebuild_index() const;
  // Compares the maintained index against a linear recomputation and reports every
  // disagreement as a diagnostic under "$index". The invariant every mutation must keep, and
  // what the randomized command test checks after each step.
  bool validate_index(Vector<Diagnostic>* out = nullptr) const;

  // --- change tracking ------------------------------------------------------------------------

  // Ids whose record changed in some layer since the last clear_dirty(). DocumentStore::save
  // rewrites only the tiles these fall in; an id is reported for every layer that holds it,
  // which can rewrite one tile more than strictly necessary and never one fewer.
  const HashSet<ObjectId>& dirty() const noexcept { return dirty_; }
  void clear_dirty() noexcept { dirty_.clear(); }
  // Marks every indexed id dirty: what a store does when it cannot tell what is on disk.
  void mark_all_dirty();

  // --- mutation ---------------------------------------------------------------------------

  // Applies one command to the edit layer. On success fills `inverse` (if given) with the
  // command that restores the previous state. On a precondition failure nothing changes and a
  // diagnostic is appended (if given). `strict = false` skips existence and cycle checks, for
  // applying diffs to layers whose base is not loaded.
  bool apply(const Command& command, Command* inverse, Vector<Diagnostic>* diagnostics,
             bool strict = true);

  Transaction begin(Attribution attribution);
  const Vector<Patch>& journal() const noexcept { return journal_; }
  // Journal maintenance for stores: replace it after loading, or drop the redo tail (patches
  // from `count` on) before a new commit.
  void set_journal(Vector<Patch>&& patches) noexcept { journal_ = std::move(patches); }
  void truncate_journal(u32 count) {
    if (count < journal_.size()) journal_.resize(count);
  }
  // Applies a patch's inverse commands in reverse order (or its forward commands in order).
  bool undo(const Patch& patch);
  bool redo(const Patch& patch);

  // --- validation -------------------------------------------------------------------------

  // Checks every record against the schema registry (types, fields, values), parents, cycles,
  // and definition uniqueness. Returns true when nothing was appended.
  bool validate(Vector<Diagnostic>& out) const;

 private:
  friend class Transaction;

  // One id's composed position across the stack. `layer_mask` names the layers holding a record
  // for the id so a query visits only those; bit 63 stands for "layer 63 and every layer above
  // it", so a stack deeper than 63 costs a scan of the tail and nothing else breaks.
  struct IndexEntry {
    ObjectId parent;         // composed parent; the null id is the root
    u32 defining_layer = 0;  // strongest layer whose record carries a type
    u32 record_count = 0;    // layers holding a record for this id
    u64 layer_mask = 0;
    bool defined = false;
    bool deleted = false;

    bool live() const noexcept { return defined && !deleted; }
  };

  void commit(Patch&& patch) { journal_.push_back(std::move(patch)); }
  bool would_cycle(ObjectId id, ObjectId new_parent) const;
  void prune(Layer& layer, ObjectId id) noexcept;

  IndexEntry compose(ObjectId id) const;
  void ensure_index() const {
    if (index_dirty_) rebuild_index();
  }
  // Recomputes one id's entry and moves it in the live set and the children map. Every mutation
  // path calls this for the id it touched, and for nothing else: a record only ever changes the
  // composition of its own object.
  void touch(ObjectId id);
  void link_child(ObjectId parent, ObjectId id) const;
  void unlink_child(ObjectId parent, ObjectId id) const;
  // Folds the ids whose liveness changed into the sorted live list, in one merge pass.
  void flush_live() const;
  // Visits every layer holding a record for `id`, weakest first.
  template <class Fn>
  void for_each_record(ObjectId id, const IndexEntry& entry, Fn&& fn) const;

  Vector<Layer> layers_;
  Vector<Patch> journal_;
  u32 edit_layer_ = 0;

  // The composed index. Mutable because it is a cache of what the layers already say: queries
  // are const and rebuild or flush it as needed.
  mutable HashMap<ObjectId, IndexEntry> index_;
  mutable HashMap<ObjectId, Vector<ObjectId>> children_;
  mutable Vector<ObjectId> live_;          // sorted by id
  mutable Vector<ObjectId> live_pending_;  // ids whose liveness changed since the last flush
  mutable bool index_dirty_ = false;
  HashSet<ObjectId> dirty_;
};

// Groups commands so they commit or roll back together. Rolls back on destruction if neither
// commit() nor rollback() was called.
class Transaction {
 public:
  Transaction(Document& doc, Attribution attribution);
  ~Transaction();
  ENGINE_NON_COPYABLE(Transaction);

  // Applies to the edit layer and records the inverse. Returns false and records a diagnostic
  // on failure; the transaction can still be committed with what succeeded, or rolled back.
  // `strict = false` skips the existence and cycle checks, for the same reason Document::apply
  // takes the flag: replaying a diff or a merge whose base the composed document does not hold.
  bool apply(const Command& command, bool strict = true);
  bool ok() const noexcept { return diagnostics_.empty(); }
  const Vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }
  const Patch& patch() const noexcept { return patch_; }

  bool commit();
  void rollback();

 private:
  Document* doc_;
  Patch patch_;
  Vector<Diagnostic> diagnostics_;
  bool finished_ = false;
};

// --- structural diff ------------------------------------------------------------------------

// One property's change between two layers: the value it takes in `to`, or its removal.
struct PropertyChange {
  std::string name;
  // The value in `to`; meaningless when `removed`.
  JsonValue value;
  bool removed = false;
};

// What changed about one object's record between two layers, field by field and property by
// property: the machine-readable form of a diff. `diff_layers` renders it as commands and
// `merge_layers` (merge.h) reasons about it.
struct RecordDiff {
  ObjectId id;
  // The record is absent in `from`, or absent in `to`. Never both, and never with any of the
  // fields below: read the whole record from the layer that has it.
  bool added = false;
  bool removed = false;
  bool type_changed = false;
  bool parent_changed = false;
  bool deleted_changed = false;
  // What `to` holds, for the fields marked changed above.
  std::string type;
  std::optional<ObjectId> parent;
  bool deleted = false;
  // Changed properties, in name order; `removed` ones are absent from `to`.
  Vector<PropertyChange> properties;
};

// Structural diff of two layers: one entry per object whose record differs, in id order.
Vector<RecordDiff> diff_records(const Layer& from, const Layer& to);

// Commands that turn layer `from` into layer `to`, record by record, in deterministic order.
// Applying them (strict = false) to a copy of `from` yields a layer equal to `to`. This is
// `diff_records` rendered as commands: a whole-record restore or removal where no command can
// express the change (an added or removed record, a changed type or deletion flag, a parent
// override that `to` drops), fine-grained commands otherwise.
Vector<Command> diff_layers(const Layer& from, const Layer& to);

// Command constructors.
Command cmd_create(ObjectId id, std::string type, ObjectId parent = {},
                   JsonValue initial_properties = {});
Command cmd_delete(ObjectId id);
Command cmd_set(ObjectId id, std::string name, JsonValue value);
Command cmd_clear(ObjectId id, std::string name);
Command cmd_set_parent(ObjectId id, ObjectId parent);
Command cmd_remove_record(ObjectId id);
Command cmd_restore(ObjectId id, std::optional<ObjectRecord> record);

// "<layer>/<id hex>" or "<layer>/<id hex>/<property>" for diagnostics.
std::string describe_path(std::string_view layer, ObjectId id, std::string_view property = {});

}  // namespace engine::doc
