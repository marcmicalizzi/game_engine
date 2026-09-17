#pragma once

// Structural three-way merge of layers (docs/plan/03-data-model.md §3.3, ADR-0002).
//
// Two layers that diverged from a common base are merged per object and per property, never by
// text. Objects are identified by Id128, so renaming one is not a conflict and moving it in the
// tree is a change to one field. Conflicts arise only where the two sides say different things
// about the same thing: the same property set to different values, a deletion against a
// modification, the same id created twice with different content, a reparent that would close a
// cycle. Everything else merges silently, and the result is canonical: with no conflicts,
// merge(base, a, b) and merge(base, b, a) write the same LayerFile JSON.
//
// A layer merge sees only what the layers hold. Whether the composed document that results is
// valid (types, missing parents, objects defined twice) is `Document::validate`'s question, and
// the merge deliberately does not answer it: a merge that refused to produce a layer would lose
// the work on both sides.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <domain/doc/document.h>

#include <string>

namespace engine::doc {

// One thing the two sides disagreed about.
//
// `property` names a property of the object, or one of the pseudo-properties "$type" and
// "$parent" for the record's own fields (a schema field name never contains '$'), and is empty
// for a conflict about the whole record. `base`, `ours`, and `theirs` are the three values, as
// JSON: a property value, a type name, an id as 32 hex characters, a whole record as an object,
// and JSON null for "absent" (a property neither set nor overridden, a record that is not
// there, a layer with no opinion about the parent).
struct MergeConflict {
  ObjectId object;
  std::string property;
  enum Kind : u8 {
    // The same property (or "$type", or "$parent") changed to different values on both sides.
    PropertyBothChanged = 0,
    // One side deleted the object, the other modified it.
    DeletedAndModified = 1,
    // Both sides created the object under the same id, with different content.
    CreatedBothDifferent = 2,
    // Taking the reparent would close a cycle in the merged layer; base's parent is kept.
    ParentCycle = 3,
  };
  Kind kind = PropertyBothChanged;
  JsonValue base;
  JsonValue ours;
  JsonValue theirs;
};

// "PropertyBothChanged", "DeletedAndModified", "CreatedBothDifferent", "ParentCycle".
const char* conflict_kind_name(MergeConflict::Kind kind) noexcept;

struct MergeOptions {
  enum Prefer : u8 {
    // Conflicting properties keep ours; an object both sides created keeps ours.
    Ours = 0,
    // The same, resolved toward theirs.
    Theirs = 1,
    // Conflicting properties keep base's value, and an object both sides created differently is
    // left out, because base has none. A deletion against a modification still keeps the
    // object, with base's record: the modifications are not applied either.
    Neither = 2,
  };
  Prefer prefer_on_conflict = Ours;
};

struct MergeResult {
  // Takes base's name and role; the caller names the layer it writes the records into.
  Layer merged{std::string(), LayerRole::Base};
  Vector<MergeConflict> conflicts;
  // Changes taken from each side: one per property set or cleared, one each for a changed type,
  // parent, or deletion flag, and one for a record added or removed whole. A change both sides
  // made identically counts for both; a change dropped by a conflict counts for neither.
  u32 applied_ours = 0;
  u32 applied_theirs = 0;
};

// Merges `ours` and `theirs`, which both descend from `base`, into `out.merged`.
//
// Rules, per object (identified by id) and per property:
//
//   changed on one side only            take that side
//   changed on both to the same value   take it, silently
//   changed on both, different values   PropertyBothChanged; keeps ours (see MergeOptions)
//   deleted on one side, untouched      deleted
//   deleted on one side, modified       DeletedAndModified; the object is kept, modified
//   created on both, identical          take it, silently
//   created on both, different          CreatedBothDifferent; keeps ours (see MergeOptions)
//   reparent that would close a cycle   ParentCycle; keeps base's parent
//
// A side *deletes* an object when its record is gone from the layer or its `deleted` flag went
// from false to true; when the two sides delete by different means the record is removed. The
// merged layer never keeps an empty override-only record (no type, no parent, not deleted, no
// properties), the same normalization `ClearProperty` does.
//
// Returns false only when `base` is not a sound ancestor: its own records already form a parent
// cycle, so "keeps base's parent" would mean nothing. Conflicts are not failures; they are
// reported in `out.conflicts` and the merged layer is complete either way.
bool merge_layers(const Layer& base, const Layer& ours, const Layer& theirs, MergeResult& out,
                  std::string* error);
bool merge_layers(const Layer& base, const Layer& ours, const Layer& theirs,
                  const MergeOptions& options, MergeResult& out, std::string* error);

}  // namespace engine::doc
