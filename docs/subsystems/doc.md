# doc (domain)

**Purpose.** The authoring document model (docs/plan/03-data-model.md §3.2–3.3, ADR-0002): the text-serialized, layered, stable-ID source of truth that humans and agents edit through commands. Everything the engine protocol, the editor, and the MCP bridge will do to a world goes through this module.

**Model.**
- A `Document` is an ordered `Vector<Layer>`, weakest first. `Layer` holds sparse `ObjectRecord`s keyed by `ObjectId` (`Id128`), sorted.
- A record with a non-empty `type` (a registered schema struct's qualified name) *defines* the object; a record without a type overrides `properties`, `parent`, or `deleted` for an object defined in a weaker layer.
- Composition: the strongest layer wins per (object, property); the strongest layer that sets a parent wins; an object is deleted if any layer at or above its defining layer tombstones it. There are no other composition operators (no inherits, variants, or references-with-overrides).
- Property values are `JsonValue`s validated against the object's schema type on demand (`validate()`), not on every edit, so partial and in-progress states are representable.

**Mutation.** Every change is a schema-typed `Command` applied to the document's edit layer: `CreateObject` (type, parent, optional initial properties), `DeleteObject` (drops the definition when the edit layer defines it, tombstones otherwise), `SetProperty`, `ClearProperty`, `SetParent`, `RemoveRecord`, `RestoreRecord`. Strict application checks preconditions (existence, parent existence, cycles) and reports pathed diagnostics; non-strict application is for applying diffs to layers whose base is not loaded. Every application yields an inverse (`RestoreRecord` of the previous edit-layer record).

**Transactions and journal.** `Transaction` groups commands with an `Attribution` (actor, role, task, rationale, timestamp), records forward and inverse commands, commits as a `Patch` into the document's journal, or rolls back (automatically on scope exit). `undo(patch)` and `redo(patch)` replay inverses in reverse or forwards in order. Patches are schema types and serialize like everything else.

**Diff.** `diff_layers(from, to)` emits commands that transform one layer into another in deterministic order: whole-record restore/remove for added, removed, type-changed, or deletion-changed records; fine-grained `SetParent`, `ClearProperty`, `SetProperty` otherwise. Applying the diff to `from` yields `to` exactly (tested both directions).

**Files.** A layer serializes through the generated `LayerFile` schema to canonical JSON with records in id order, so two equal layers produce byte-identical files and git diffs are meaningful. Format pinned by test.

**Store.** `DocumentStore` (`document_store.h`) keeps a document as a directory: `manifest.json` (`DocumentManifest`: name, layer stack, edit layer, `undo_position`), `layers/<name>.json` (one canonical `LayerFile` each, written atomically), and `journal.jsonl` (one `Patch` per line). Layer files always hold the current state, so a checkout is readable without replaying the journal, and the journal plus the manifest position make undo and redo survive across processes. Paths go through `io::Vfs`, so documents live under mounts or at native paths.

**Validation.** Unknown types; overrides for objects no layer defines; objects defined in more than one layer; missing parents; parent cycles; unknown, transient, or ill-typed properties (checked by deserializing each value into a scratch instance of the type). Diagnostics are `<layer>/<id hex>/<property>`.

**Not yet (v0).** Tile-partitioned files, leases, proposal-layer promotion, structural three-way merge, git checkpointing, a composed index for O(1) resolution. `resolve()`, `objects()`, and `children()` are linear scans over layers and records.

**Public API.** `domain/doc/document.h`: `Layer`, `Document`, `Transaction`, `ResolvedObject`, `diff_layers`, `cmd_*` constructors, `describe_path`. Types from `schemas/doc.schema`: `LayerRole`, `ObjectRecord`, `LayerFile`, `CommandKind`, `Command`, `Attribution`, `Patch`.

**Depends on.** `base`, `containers`, `ids`, `json`, `schema`, `time`, `schemas`, `io`.

**Testing.** `tools/dev.ps1 test -Filter doc`. Covers composition and tombstones across layers, precondition failures leaving state untouched, transactions with commit/rollback/undo/redo and serialized patches, diff round trips, the pinned file format, and validation against the registered provenance schema.

**Performance notes.** This is the editing path, not the frame loop. The runtime world is a materialization of the document plus persistent state (plan 03 §3.4); the document itself is optimized for diffability and correctness. A composed index cache is the first optimization when editor latency demands it.
