#pragma once

// Content-addressed blobs beside a document (docs/plan/03-data-model.md §3.2; docs/subsystems/doc.md,
// "Blobs"): bytes too large or too binary for a property — a heightfield's grid, a painted mask, a
// generated image — kept once each under the document's directory and named by their SHA-256,
//
//     <dir>/blobs/<first two hex digits>/<the other 62>
//
// and referenced from a property by an `engine.doc.BlobRef` (`{"blob": <name>, "size": n, "media":
// ...}`). The document holds the name, never the bytes, so its JSON stays small and diffable, a merge
// treats the reference as a value like any other, and two references to equal bytes are one blob.
//
// One store per document, not per layer or per tile: a tile file names a blob by its hash, and the
// bytes are found by the hash alone, so moving a record between tiles moves only the reference.
//
// - **The same bytes always get the same name and the same file.** The file is the bytes and nothing
//   else, so the name is a function of the content and the content of the name.
// - **A blob is written when it is put, not when the document is saved**, atomically (a sibling file
//   renamed into place), and never changed after: a save writes no blob and its log names none. A
//   blob nothing references yet is harmless; a reference to a blob that is not there is not, which
//   is why a session refuses a command naming one its store does not hold.
// - **A read checks the bytes against the name** (and the length against the reference's size) and
//   refuses, naming the blob and the file, when they differ: a damaged file is never handed on.
// - **A blob is collected only when nothing can bring its reference back**: no record of any layer
//   names it, no patch of the journal does — the undo history and the redo tail, both of which can
//   put a reference back — no root the caller adds (a proposal's merge base) does, and it is older
//   than a grace period, so a blob put by a client a moment before the commit that will name it is
//   not taken from under it. Collection is explicit (`collect`, `doc.collect_blobs`), never a side
//   effect of a save: it reads the whole document and its journal.

#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <foundation/io/vfs.h>

#include <core/containers/hash_set.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::doc {

// One blob found in a document's store.
struct StoredBlob {
  std::string name;
  u64 size = 0;
  i64 modified_unix_ms = 0;
};

struct BlobCollectOptions {
  // A blob nothing references is kept while it is younger than this: the time between a client's
  // put and the commit that names it. An hour by default.
  i64 min_age_ms = 3600 * 1000;
  // Report what would be removed and remove nothing.
  bool dry_run = false;
  // The clock the age is measured against (time::wall_unix_ms when zero).
  i64 now_unix_ms = 0;
};

struct BlobCollectReport {
  u32 stored = 0;           // blobs in the store before the collection
  u64 stored_bytes = 0;
  u32 referenced = 0;       // distinct names the document, its journal and the roots reference
  u32 kept_young = 0;       // referenced by nothing, but younger than the grace period
  u64 removed_bytes = 0;
  Vector<std::string> removed;  // names removed (or that a dry run would remove), in name order
  Vector<std::string> missing;  // referenced names the store does not hold, in name order
};

class BlobStore {
 public:
  static constexpr std::string_view k_blobs_dir = "blobs";

  // 64 lowercase hexadecimal characters: the form every blob name takes.
  static bool is_name(std::string_view text) noexcept;
  // The SHA-256 of `bytes` as a name.
  static std::string name_of(std::string_view bytes);
  // "<dir>/blobs/<aa>/<rest>", a Vfs path under the document's directory.
  static std::string path_of(std::string_view dir, std::string_view name);

  // Stores `bytes` and fills `out` (name, size, `media`). `created` says whether the file was new;
  // a blob already there whose bytes still hash to its name is left alone, and one that does not —
  // a damaged file — is written again.
  static bool put(const io::Vfs& vfs, std::string_view dir, std::string_view bytes,
                  std::string_view media, BlobRef& out, bool* created, std::string* error);
  // The file is there with the reference's length (any length when `size` is zero). Checks the
  // length, not the bytes: what a command's check costs.
  static bool has(const io::Vfs& vfs, std::string_view dir, std::string_view name,
                  u64 size = 0) noexcept;
  // The bytes, checked against the name (and `size` when it is not zero); false with a sentence
  // naming the blob and the file when they are not there or do not match.
  static bool read(const io::Vfs& vfs, std::string_view dir, std::string_view name, u64 size,
                   std::string& out, std::string* error);
  // Every blob in the store, in name order. Files under `blobs/` that are not blob names (a
  // temporary file a killed put left) are not listed.
  static bool list(const io::Vfs& vfs, std::string_view dir, Vector<StoredBlob>& out,
                   std::string* error);

  // The blob names a JSON value references, anywhere in it: every object whose `blob` member is a
  // name. A value that merely looks like a reference keeps a blob alive and does nothing else.
  static void references(const JsonValue& value, HashSet<std::string>& out);
  // Every name the document can still reach: each record of each layer, and each command of each
  // journal patch, forward and inverse — so the undo history and the redo tail are both covered.
  static void references(const Document& doc, HashSet<std::string>& out);
  // The references in one command's value and record.
  static void references(const Command& command, HashSet<std::string>& out);

  // Removes every blob that nothing in `doc` (above) or in `roots` references and that is older
  // than `options.min_age_ms`, and reports what the store held, what is referenced and missing,
  // and what went.
  static bool collect(const io::Vfs& vfs, std::string_view dir, const Document& doc,
                      std::span<const JsonValue> roots, const BlobCollectOptions& options,
                      BlobCollectReport& report, std::string* error);
};

}  // namespace engine::doc
