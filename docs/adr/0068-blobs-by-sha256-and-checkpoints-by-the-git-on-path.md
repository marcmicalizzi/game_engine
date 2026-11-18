# ADR-0068: A document's blobs are named by SHA-256, its checkpoints are commits by the git on PATH, and a rollback is a journaled edit

- **Status:** Proposed
- **Date:** 2026-11-17
- **Plan references:** docs/plan/03-data-model.md §3.2, §3.3; docs/plan/06-agent-tooling.md §6.4, §6.9
- **Docs touched:** `docs/subsystems/doc.md` (Blobs, Checkpoints), `docs/subsystems/protocol.md` (Blobs and checkpoints), `docs/subsystems/hash.md`, `docs/subsystems/platform.md`, `content/roles/README.md`, `docs/plan/03-data-model.md` §3.2, `docs/plan/06-agent-tooling.md` §6.4

## Context

Plan 03 §3.2 keeps large binary content (heightfields, masks, splines of thousands of points) out of the document's JSON as content-addressed files referenced by hash, and §3.3 and 06 §6.4 make a checkpoint a git commit with the journal segment as its body and `rollback_to(checkpoint)` a restore. Neither was built (roadmap D13, D14). Four questions had real costs either way:

1. **The hash that names a blob.** The engine's `hash_bytes` is 64 bits and a table hash; the name is a blob's whole identity, and two contents sharing one would make a document read the wrong bytes without a word.
2. **How a checkpoint reaches git.** The tree links no git library, ADR-0014 rules out a copyleft one, and nothing may be downloaded. The two ways left: run the `git` on PATH, or write git's object kinds here (SHA-1, zlib's format, blob, tree, commit, a ref).
3. **What `rollback_to` does to the document.** Put the checkpoint's files back and reload, or edit the document into the checkpoint's state.
4. **Where the repository lives.** The document directory's own `.git`, an enclosing repository's, or a directory of the engine's choosing.

## Decision

1. **A blob is named by the SHA-256 of its bytes**, 64 lowercase hexadecimal characters, and stored once per document as `blobs/<2>/<62>`, the file being the bytes and nothing else. A property holds an `engine.doc.BlobRef` (`blob`, `size`, `media`); any JSON object whose `blob` member is a name is a reference wherever it stands. A blob is written when it is put, atomically, never by a save; a read checks the bytes against the name; a client's command naming a blob the store lacks is refused; a collection is explicit and removes only what no layer, no journal patch (undo history and redo tail) and no open proposal's merge base names, past a grace period.
2. **A checkpoint runs the `git` on PATH.** git is a program the engine runs, not a library it links or ships, so its licence does not reach the engine. Without git, the checkpoint methods answer 1006 Unavailable with the reason. The repository is run apart from the user's git configuration (no hooks, signing, line-ending conversion or automatic maintenance; repository and work tree named outright) and tracks `manifest.json`, `layers/` and `blobs/` only. A checkpoint is read back by `git archive`, never checked out over the document.
3. **A rollback is a forward edit**: per layer it changes, one journaled patch of `diff_layers(now, checkpoint)`, through the same commit path and checks as every write — role, tiles, types, leases — all checked before any is committed. Open proposals are left as they are; a layer the checkpoint lacks is emptied and kept; one it had that the stack lost is appended; a collected blob is written back from the checkpoint.
4. **The repository is the document directory's own `.git`** (`Checkpoints::k_git_dir`).

## Consequences

- The runtime world follows a rollback through the change feed as it follows any edit, the journal says who rolled back to what and why, every open session sees it, and `doc.undo` takes it back. Nothing in the history is lost: a rollback never rewrites git's history or the journal.
- A checkpoint needs git installed and costs a handful of processes (about 20–60 ms each on Windows); the repository is git's own, packed and verified by git, and readable by every git tool. No git format is reimplemented, so a person running `git gc` or committing by hand in the directory breaks nothing.
- A document kept inside another repository becomes an embedded repository there once it is checkpointed. Choosing the enclosing repository instead would write commits into a game's history behind its author's back.
- A blob reachable by the journal is never collected, so a document whose journal is never trimmed keeps every blob it ever committed. That is the honest cost of undo reaching back that far; trimming the journal at a checkpoint is the lever, and is not built.
- The authored heightfield (W66) and the terrain masks are the consumers named; neither is moved yet (doc.md, "Blobs", says what moving the heightfield takes).

## Revisit when

- A host must checkpoint on a machine with no git (a tester's), or the processes' cost shows in a measured agent loop: then write the object kinds, behind the same five functions.
- Journals grow long enough that the blobs they keep alive matter: then a journal trimmed at a checkpoint, with the checkpoint holding what undo can no longer reach.
- A game wants its documents' history in its own repository: then the repository's location becomes a setting.
