#pragma once

// Proposal layers (docs/plan/06-agent-tooling.md §6.5; docs/subsystems/protocol.md, "Proposal
// layers and promotion"): an agent writes to a layer of its own above a target, and acceptance
// promotes the layer's opinions into the target by structural three-way merge, validated before
// anything is committed.
//
// A proposal's own properties — target, owner, state, why it exists — are kept in the document's
// `proposals.json` (schema `ProposalsFile`) and reported by `doc.layers` as the layer's
// `proposal`. They are the protocol's, not `domain/doc`'s: a proposal layer is an ordinary layer
// in memory, on disk and in the journal, with the role `Proposal`, and nothing below the protocol
// needs to know who owns it.
//
// **The merge base is per record, taken on first touch.** When the proposal first changes an
// object, the target's record for it, as it stands then, is recorded
// (`proposals/<layer>.base.jsonl`, one `ProposalBaseEntry` a line, appended). That is the base the
// promotion merges against for that object — not a copy of the whole target taken when the proposal
// was opened. Two reasons: it costs what the proposal touches rather than what the target holds,
// and it is the state the agent actually built on, so a change promoted into the target *before*
// the agent first touched a record is never reported against it as a conflict. ADR-0039 has the
// comparison.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <domain/doc/document.h>

#include <schemas/protocol.h>
#include <span>
#include <string_view>

namespace engine::protocol {

class ProposalTable {
 public:
  struct Entry {
    ProposalInfo info;
    // The target's record for every object the proposal has touched, as the proposal first found
    // it; `record` absent when the target had none. Indexed by id.
    Vector<ProposalBaseEntry> base;
    HashMap<doc::ObjectId, u32> base_index;
  };

  Entry* find(std::string_view layer) noexcept;
  const Entry* find(std::string_view layer) const noexcept;
  std::span<const Entry> entries() const noexcept { return {entries_.data(), entries_.size()}; }
  Entry& add(ProposalInfo info);

  // Records the target's record for `id` as the proposal's base when it has none yet. Returns
  // whether it was new; the new entry is `entry.base.back()`.
  static bool note_base(Entry& entry, doc::ObjectId id, const doc::ObjectRecord* target_record);
  static const ProposalBaseEntry* base_of(const Entry& entry, doc::ObjectId id) noexcept;
  static void clear_base(Entry& entry) noexcept;

  // The metadata, without the bases (those live in their own files).
  ProposalsFile to_file() const;
  void assign(ProposalsFile file);

 private:
  Vector<Entry> entries_;
};

// `over` with `proposal`'s opinions composed on top, written as one layer: what `over` would hold
// had the proposal's commands been applied to it. A set property replaces over's, a parent the
// proposal assigns replaces over's, a type it gives defines the object. A tombstone drops the
// record when `over` defines the object (what `DeleteObject` does in the defining layer) and
// tombstones it otherwise. An override that ends up saying nothing is not kept. The result takes
// over's name, role and partition.
doc::Layer compose_onto(const doc::Layer& over, const doc::Layer& proposal);

}  // namespace engine::protocol
