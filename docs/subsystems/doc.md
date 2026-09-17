# doc (domain)

**Purpose.** The authoring document model (docs/plan/03-data-model.md §3.2–3.3, ADR-0002): the text-serialized, layered, stable-ID source of truth that humans and agents edit through commands. Everything the engine protocol, the editor, and the MCP bridge will do to a world goes through this module.

**Model.**
- A `Document` is an ordered `Vector<Layer>`, weakest first. `Layer` holds sparse `ObjectRecord`s keyed by `ObjectId` (`Id128`), sorted.
- A record with a non-empty `type` (a registered schema struct's qualified name) *defines* the object; a record without a type overrides `properties`, `parent`, or `deleted` for an object defined in a weaker layer.
- Composition: the strongest layer wins per (object, property); the strongest layer that sets a parent wins; an object is deleted if any layer at or above its defining layer tombstones it. There are no other composition operators (no inherits, variants, or references-with-overrides).
- Property values are `JsonValue`s validated against the object's schema type on demand (`validate()`), not on every edit, so partial and in-progress states are representable.

**Mutation.** Every change is a schema-typed `Command` applied to the document's edit layer: `CreateObject` (type, parent, optional initial properties), `DeleteObject` (drops the definition when the edit layer defines it, tombstones otherwise), `SetProperty`, `ClearProperty`, `SetParent`, `RemoveRecord`, `RestoreRecord`. Strict application checks preconditions (existence, parent existence, cycles) and reports pathed diagnostics; non-strict application is for applying diffs to layers whose base is not loaded, and a `Transaction` takes the same flag, which is how undo, redo, and a merge are written. Every application yields an inverse (`RestoreRecord` of the previous edit-layer record).

**Transactions and journal.** `Transaction` groups commands with an `Attribution` (actor, role, task, rationale, timestamp), records forward and inverse commands, commits as a `Patch` into the document's journal, or rolls back (automatically on scope exit). `undo(patch)` and `redo(patch)` replay inverses in reverse or forwards in order. Patches are schema types and serialize like everything else.

**Diff.** `diff_records(from, to)` is the machine-readable structural diff: one `RecordDiff` per object whose record differs, in id order, saying whether the record was added or removed and otherwise which of `type`, `parent`, and `deleted` changed and which properties were set or cleared (`PropertyChange`, in name order). `diff_layers(from, to)` is that rendered as commands in the same order: whole-record restore/remove where no command expresses the change (an added or removed record, a changed type or deletion flag, a parent override `to` drops), fine-grained `SetParent`, `ClearProperty`, `SetProperty` otherwise. Applying the diff to `from` yields `to` exactly (tested both directions).

**Structural merge.** `merge_layers(base, ours, theirs, [options,] out, error)` (`merge.h`) merges two layers that diverged from a common ancestor, per object and per property — never by text. Objects are identified by `Id128`, so a rename is a property change and a move is a change to one field; neither is a conflict. The two sides' `diff_records` against base drive it, so the diff machinery is not duplicated.

| What the two sides did | Merged layer | Conflict |
|---|---|---|
| changed a property on one side only | that side's value | — |
| changed it on both to the same value | that value | — |
| changed it on both to different values | ours | `PropertyBothChanged` |
| deleted an object, untouched on the other side | deleted | — |
| deleted it on one side, modified it on the other | kept, with the modifications | `DeletedAndModified` |
| deleted it on both sides | deleted (the record is removed if either side removed it) | — |
| created the same id with identical records | that record | — |
| created the same id with different records | ours | `CreatedBothDifferent` |
| reparented so that the merged layer would have a cycle | base's parent | `ParentCycle` |

A side *deletes* an object when its record is gone from the layer or its `deleted` flag went from false to true. Conflicts name the object and the property; `$type` and `$parent` stand for the record's own fields (a schema field name never contains `$`), and the property is empty for a conflict about the whole record. Each conflict carries the three values as JSON, with null for absent. `MergeOptions::prefer_on_conflict` moves the resolution from `Ours` (the default) to `Theirs`, or to `Neither`, which keeps base's value — except for a deletion against a modification, where the modifications win under both `Ours` and `Theirs`, because a deletion never silently destroys the other side's edits, and `Neither` keeps base's record untouched. `applied_ours` and `applied_theirs` count the changes each side got: one per property set or cleared, one each for a changed type, parent, or deletion flag, one for a record added or removed whole, counting for both sides when both made the same change and for neither when a conflict dropped it.

The merged layer takes base's name and role, never keeps an empty override-only record (the normalization `ClearProperty` does), and is canonical: with no conflicts, `merge(base, a, b)` and `merge(base, b, a)` write the same `LayerFile` JSON. Cycles are judged against the finished layer, so two reparents that are each fine but together close a cycle are caught, and only a layer merge's own links are visible — the composed document's parents are `validate()`'s question. The merge fails, rather than conflicting, only when `base` itself already has a parent cycle: nothing could be kept from it.

**Not merged.** Spatial collisions (two agents placing overlapping objects) are not merge conflicts; validators catch them after the merge (plan 03 §3.3). A merge never refuses to produce a layer over a conflict, because that would lose the work on both sides.

**Files.** A layer serializes through the generated `LayerFile` schema to canonical JSON with records in id order, so two equal layers produce byte-identical files and git diffs are meaningful. Format pinned by test.

**Store.** `DocumentStore` (`document_store.h`) keeps a document as a directory: `manifest.json` (`DocumentManifest`: name, layer stack, edit layer, `undo_position`), `layers/<name>.json` (one canonical `LayerFile` each, written atomically), and `journal.jsonl` (one `Patch` per line). Layer files always hold the current state, so a checkout is readable without replaying the journal, and the journal plus the manifest position make undo and redo survive across processes. Paths go through `io::Vfs`, so documents live under mounts or at native paths.

**Validation.** Unknown types; overrides for objects no layer defines; objects defined in more than one layer; missing parents; parent cycles; unknown, transient, or ill-typed properties (checked by deserializing each value into a scratch instance of the type). Diagnostics are `<layer>/<id hex>/<property>`.

**Not yet (v0).** Tile-partitioned files, leases, proposal-layer promotion, git checkpointing, a composed index for O(1) resolution. `resolve()`, `objects()`, and `children()` are linear scans over layers and records. A merge is between layers, not between document states, and it does not rebase a journal.

**Public API.** `domain/doc/document.h`: `Layer`, `Document`, `Transaction`, `ResolvedObject`, `PropertyChange`, `RecordDiff`, `diff_records`, `diff_layers`, `cmd_*` constructors, `describe_path`. `domain/doc/merge.h`: `MergeConflict`, `conflict_kind_name`, `MergeOptions`, `MergeResult`, `merge_layers`. `domain/doc/document_store.h`: `DocumentStore`. Types from `schemas/doc.schema`: `LayerRole`, `ObjectRecord`, `LayerFile`, `CommandKind`, `Command`, `Attribution`, `Patch`.

**Depends on.** `base`, `containers`, `ids`, `json`, `schema`, `time`, `schemas`, `io`.

**Testing.** `tools/dev.ps1 test -Filter doc`. Covers composition and tombstones across layers, precondition failures leaving state untouched, transactions with commit/rollback/undo/redo and serialized patches, diff round trips in both directions and the structural diff behind them, the pinned file format, and validation against the registered provenance schema. `tests/merge_tests.cpp` takes every merge rule on a hand-built base of three objects in a parent chain, each `MergeOptions` value, commutativity and idempotence compared as canonical JSON, and a seeded randomized run: forty rounds of two ten-edit sequences over a twelve-object tree, each merged layer validated against the registry and checked to hold every edit the other side did not contest.

**Performance notes.** This is the editing path, not the frame loop. The runtime world is a materialization of the document plus persistent state (plan 03 §3.4); the document itself is optimized for diffability and correctness. A composed index cache is the first optimization when editor latency demands it.
