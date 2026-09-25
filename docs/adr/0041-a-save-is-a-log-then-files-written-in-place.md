# ADR-0041: A document save writes only what changed, as one log and then each file in place

- **Status:** Proposed
- **Date:** 2026-09-25
- **Plan references:** docs/plan/03-data-model.md §3.2 (the authoring document and its files), §3.7 (tile-partitioned layers); docs/plan/06-agent-tooling.md §6.5 (many agents on one document); experiment E12 ([write-up](../experiments/e12-proposal-layers-and-leases.md)).
- **Docs touched:** [doc](../subsystems/doc.md#saving), [protocol](../subsystems/protocol.md), [E12](../experiments/e12-proposal-layers-and-leases.md)

## Context

E12 found that an agent's edit cost 87–158 ms and a promotion about 400 ms on Windows, and that nearly all of it was `DocumentStore::save`: every commit visited every layer — serialized and rewrote each single-file layer, read and parsed each partitioned layer's `index.json`, made its directories, listed its tiles — and wrote each file it did write atomically, as a new file renamed over the old. With one proposal layer per agent, an edit paid for every other agent's layer.

Measuring the file operations one at a time (`doc.fs.*` in `domain/doc/bench`) showed that this machine's file system charges by the operation and by its kind, not by the byte: a rename over a file costs 2.1 ms, a new file 0.7 ms, `create_directories` on a path that exists 4.4 ms (MSVC creates every prefix in turn, and `Vfs::write` and `Vfs::append` call it before every write), opening a file for reading as well as writing or truncating it 5–12 ms once it is 64 KiB or more — and opening it for writing alone, writing over it without truncating and cutting it to length 0.5–0.6 ms at any size up to a megabyte.

And the old save was not atomic across files: a process that died mid-save could leave an index and its tiles disagreeing, which the next load refused, or a manifest of one state over layer files of the other.

Three shapes were on the table. **Keep each file atomic (a rename each) and only write fewer of them**: simple, and still three to five times the cost of an in-place write per file, with the cross-file problem unsolved. **Content-addressed files with the manifest as the commit point**: atomic and cheap, but it changes the on-disk format, which is a contract — the canonical layer files are what git diffs, merges compare and every reader parses. **A redo log written before the files, then each file in place**: the format at rest unchanged, one extra file created and removed per save, and a save that is all or nothing across files.

## Decision

1. **A save writes only what changed.** The document records per layer the ids its commands edited since the last save, or that the layer must be looked at whole (it arrived, changed form, or was handed out through the mutable accessor). The store keeps what it last wrote or read of the directory — every file's length and 64-bit hash, each partitioned layer's index — inside the document, and writes a file only when its canonical bytes differ. A layer no commit touched is not visited. When the store knows nothing of the directory it writes everything and asks the disk what to remove, as every save used to. The bytes on disk are byte for byte what writing every file would have left; a test holds it to that against a reference writer.
2. **Each file is written once, in place**: opened for writing alone, not truncated, written from the start, cut to the new length. Directories are made only when a write finds one missing.
3. **A save is all or nothing through one log**, `save.pending` in the document directory, written before any file of the document is touched: every file to write with its bytes, every file to remove, and a last line hashing the rest. Then the writes, the removals, the manifest last, and the log's removal. A log that is not whole means its save touched nothing.
4. **`load` reads through a whole log and never writes**: the document it returns is what that save would have left, on a read-only mount as on a writable one, and a process that only reads never changes a directory another may be saving into. **The first save of a loaded document carries the log out** (or removes a torn one) before doing anything of its own.
5. **No flush to the device.** The guarantee is against a process that dies, not against the power going, as before.

## Consequences

- An edit to one agent's proposal costs what that edit writes: on this machine a journal line, the log, the one or two files and the manifest, and the log's removal. `doc.store.*` and E12's tests carry the numbers ([doc](../subsystems/doc.md#what-a-save-costs)).
- A save that dies leaves the previous state (with the commit first in the journal's redo tail, since the journal is appended before the save) or the next one, and never a mixture; the load that follows succeeds either way. Before, it could fail.
- **An individual file is no longer atomic on its own.** A reader that does not go through `DocumentStore` — git, an editor, a copy taken mid-save — can see a file half written while a save is in progress, where before it saw the old file or the new. Only such a reader is affected, and only while a save runs; the log is how the store's own readers see a whole state.
- `save.pending` is a new name in a document directory. It never exists at rest; one found there is a save that did not finish, and it must not be committed or deleted by hand without loading the document with a writer.
- The store's knowledge is only right while this store is the directory's one writer, which ADR-0039 already assumes. `Document::mark_all_dirty()` is how a caller says otherwise.
- `Vfs::write` and `Vfs::append` still pay `create_directories` on every call. The store avoids them; every other caller does not, and the right fix for them is in `foundation/io` (ask whether the directory exists first), not here.

## Revisit when

- A document has to survive a power cut (git checkpointing, plan 03): the log is where an `fsync` would go, once, before the files are written.
- More than one process has to write a document at once: the store's knowledge and the log both assume one writer.
- `foundation/io` offers an in-place write and a `make_directories` that looks before it creates: the store's private writer should move there.
- A platform's rename turns out cheaper than its in-place write: the log could then write new files and rename them, at the same guarantee.
