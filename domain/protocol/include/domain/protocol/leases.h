#pragma once

// Leases (docs/plan/06-agent-tooling.md §6.5; docs/subsystems/protocol.md, "Leases"): a claim an
// actor holds on (layer, tile set) or (layer, object-type set) before it edits, time-bounded and
// visible to everyone, so that two agents about to edit the same thing find out when they ask for
// it rather than when their work is merged.
//
// One table per document, kept in the document directory as `leases.json` (schema `LeaseFile`)
// and written before every answer that changed it, the way the document's own files are: a host
// started for one call (engine-cli spawns one per invocation) sees the leases an earlier host
// granted, and a host restarted after a deadline does not silently drop them. Expiry is by the
// host's wall clock (`time::wall_unix_ms`), so it means the same thing to every process on the
// machine; an expired lease covers nothing, and is dropped on the next acquisition or list.
//
// **Acquisition refuses an overlap with another actor's live lease**: tile ranges that intersect
// on the same layer, or a type both name on the same layer. A tile lease and a type lease on one
// layer can both cover a record (a light standing in a leased tile, under a lease on lights); that
// is not decidable from the two scopes, so it is refused at the edit instead (1009, naming the
// other holder), which is still before any merge. Overlapping one's own leases is allowed.
//
// **Whether edits need a lease is the document's choice** (`required`, `lease.require`), off by
// default: every document and test written before leases existed edits as it did, and a
// single-agent document pays nothing. When it is on, every writing `doc.*` call checks each record
// it touches — by the object's composed position before and after the change, and its type — for
// a live lease of the caller's that covers it and none of anyone else's (session.cpp's `Guard`).
// The check allocates nothing: a table is tens of leases, walked in place.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/doc/partition.h>

#include <schemas/protocol.h>
#include <span>
#include <string_view>

namespace engine::protocol {

class LeaseTable {
 public:
  bool required() const noexcept { return file_.required; }
  void set_required(bool required) noexcept { file_.required = required; }
  std::span<const LeaseInfo> leases() const noexcept {
    return {file_.leases.data(), file_.leases.size()};
  }

  // Drops the leases whose expiry is at or before `now_ms`; returns how many.
  u32 expire(i64 now_ms);

  // Another actor's live lease that overlaps `wanted` (same layer, and intersecting tile ranges or
  // a shared type), or null.
  const LeaseInfo* overlap(const LeaseInfo& wanted, i64 now_ms) const noexcept;

  // Adds a lease, giving it the next id, and returns that id.
  u64 add(LeaseInfo lease);
  LeaseInfo* find(u64 id) noexcept;
  bool remove(u64 id);

  enum class Cover : u8 {
    // A live lease of the actor covers it and nobody else's does.
    covered,
    // No live lease of the actor covers it.
    not_held,
    // Another actor's live lease covers it; `other` says whose.
    held_by_other,
  };
  // The one check an edit makes per record state: `tile` null for a record with no tile, `type`
  // empty for one with no type. Another holder wins over the caller's own cover, because two
  // actors covering one record is exactly the overlap leases exist to prevent.
  Cover check(std::string_view actor, std::string_view layer, const doc::TileCoord* tile,
              std::string_view type, i64 now_ms, const LeaseInfo** other) const noexcept;

  const LeaseFile& file() const noexcept { return file_; }
  // Replaces the table with a file's contents, sorting its leases by id.
  void assign(LeaseFile file);

 private:
  LeaseFile file_;  // leases in id order
};

// Whether `lease` covers a record on `layer` in `tile` (null: none) of `type` (empty: none).
bool lease_covers(const LeaseInfo& lease, std::string_view layer, const doc::TileCoord* tile,
                  std::string_view type) noexcept;

}  // namespace engine::protocol
