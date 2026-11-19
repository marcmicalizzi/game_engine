#pragma once

// The subject digest (docs/subsystems/protocol.md, "What a close is tied to"; roadmap A56): one
// SHA-256 over what a review item is about, so that a person's close is tied to the state the
// person's page drew and not to whatever the document has become between the drawing and the
// click. The stream it hashes, in order:
//
//   1. a version line;
//   2. the document's layer stack, weakest first, each layer's name and role (a reorder, an added
//      or a removed layer changes what composes);
//   3. the layer the subject is judged in, by name;
//   4. every record the subject names and every `ChildOf` ancestor of one, sorted by id and each
//      once: its composed line (doc::append_composed_record: type, parent, properties, or a mark
//      that it is gone or deleted), and when the subject names a layer, the record that layer holds
//      itself (a proposal's own opinion), or a mark that it holds none;
//   5. every blob those records and the subject name, by its SHA-256 name (its content), and
//      whether the store holds it;
//   6. every file the subject lists and every file those records name (a string property whose
//      last component has an extension), by its spelling, with the SHA-256 of its bytes read now,
//      or "not readable" when the host's roots do not admit it for reading or it is not there,
//      which never opens it.
//
// **It hashes content and never trusts a file's time or size**: an asset's bytes are read at every
// computation. Nothing is cached here; protocol.md says where a hash may be reused and where it is
// always read again.

#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>

#include <string_view>

namespace engine::protocol {

// The digest of `subject` in `session` now, with what it covered, into `out` (`same` is left to
// the caller, which knows the stamp). A relative asset path resolves against the session's
// document directory, as every path a record names does (protocol.md, "Where a relative path
// starts").
void subject_digest(const Context& ctx, const Session& session, const ReviewSubject& subject,
                    SubjectState& out);

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
