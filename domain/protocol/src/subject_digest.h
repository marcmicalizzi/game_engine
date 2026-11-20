#pragma once

// The subject digest (docs/subsystems/protocol.md, "What a close is tied to"; roadmap A56): one
// SHA-256 over what a review item is about, so that a person's close is tied to the state the
// person's page drew and not to whatever the document has become between the drawing and the
// click. The stream it hashes, in order:
//
//   1. a version line;
//   2. the layer the subject is judged in, by name (the stack itself is not: a layer changes the
//      digest by what it composes for the subject, which step 3 holds, and never by being there);
//   3. every record the subject names and every `ChildOf` ancestor of one, sorted by id and each
//      once: its composed line (doc::append_composed_record: type, parent, properties, or a mark
//      that it is gone or deleted), and when the subject names a layer, the record that layer holds
//      itself (a proposal's own opinion), or a mark that it holds none;
//   4. every blob those records and the subject name, by its SHA-256 name (its content), and
//      whether the store holds it;
//   5. every file the subject lists and every file those records name (a string property whose
//      last component has an extension), by its spelling, with the SHA-256 of its bytes read now,
//      or a mark saying why it was not read;
//   6. a line for each bound the subject passed, naming the bound and the figure.
//
// **It hashes content and never trusts a file's time or size**: an asset's bytes are read at every
// computation. Nothing is cached here; protocol.md says where a hash may be reused and where it is
// always read again.
//
// **It is computed in two halves** (protocol.md, "What it costs"): the document's part composed
// under the host's dispatch lock (`compose_subject`), then the files read with the lock let go
// (`read_subject_files`, which touches nothing of the host's), then the two joined under the lock
// again once the document is found to be what it was (`same_document`). The ledger's methods drive
// the passes (ledger.cpp).

#include <core/hash/sha256.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>

#include <string>
#include <string_view>

namespace engine::protocol {

// What the digest is computed for: a raise refuses a subject over a bound, where a detail and a
// close digest what is over as a line saying so, which differs from the stamp.
enum class DigestFor : u8 { raise, detail, close };

// The four bounds (tunables `protocol.review.*`, protocol.md, "What it costs"), read once a
// computation and never inside its loops.
struct DigestBounds {
  u64 files = 0;          // files one subject names in all, listed and from its records together
  u64 file_bytes = 0;     // one file's bytes
  u64 subject_bytes = 0;  // every file's bytes together
  u64 records = 0;        // records and their ancestors composed, each once
  static DigestBounds now() noexcept;
};

// A bound a subject passed: the tunable's name and the figure (a count or a size, never a path).
struct DigestOver {
  const char* bound = nullptr;
  u64 limit = 0;
  u64 figure = 0;
};

// One pass: what the document half composed, and what the files half read.
struct SubjectPlan {
  Sha256 hash;  // the stream up to the files
  // The document as it was composed: compared under the lock again before the pass is used.
  const Session* session = nullptr;
  const doc::Document* document = nullptr;
  u64 revision = 0;
  usize journal = 0;
  u32 undo_position = 0;
  u32 layers = 0;
  u32 records = 0;
  u32 blobs = 0;
  Vector<Id128> gone;
  struct File {
    std::string spelled;
    std::string native;  // empty: the roots did not admit it
    std::string line;    // the hash, or why it was not read; filled by `read_subject_files`
    bool read = false;
  };
  Vector<File> files;
  Vector<DigestOver> over;
  bool refuse_over = false;  // a raise's: a bound passed stops the reads before the first byte
  u64 bytes_read = 0;
};

// The document half, under the dispatch lock: the records' and blobs' lines and the files to read,
// each admitted through the roots now. A relative asset path resolves against the session's
// document directory, as every path a record names does (protocol.md, "Where a relative path
// starts").
void compose_subject(const Context& ctx, const Session& session, const ReviewSubject& subject,
                     const DigestBounds& bounds, SubjectPlan& plan);
// The files half, with no lock of the host's held: reads only what the plan holds.
void read_subject_files(SubjectPlan& plan, const DigestBounds& bounds);
// Whether the session's document is the one the plan composed, under the lock again.
bool same_document(const Session* session, const SubjectPlan& plan) noexcept;
// The two halves joined into `out` (`same` is left to the caller, which knows the stamp).
void finish_subject(SubjectPlan& plan, SubjectState& out);

// What a promotion's merge showed (protocol.md, "What a promotion is tied to"), as 64 lowercase
// hex: the proposal and its target by name, every record the proposal layer holds as it holds it
// (in id order), and each conflict's object, property, kind and the target's value there (`ours`).
// A dry run answers it, and a promotion carrying it is refused when the same merge now gives
// another: the owner added, changed or removed an opinion, or the target's value at a conflict
// moved, or a conflict came or went. The target's values elsewhere take no part: the promotion
// takes them as the merge does whatever they are.
std::string promotion_digest(std::string_view proposal, std::string_view target,
                             const doc::Layer& layer, const Vector<MergeConflictInfo>& conflicts);

}  // namespace engine::protocol
