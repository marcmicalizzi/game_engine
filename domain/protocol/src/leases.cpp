// The lease table (leases.h).
#include <domain/protocol/leases.h>
#include <domain/protocol/policy.h>

#include <algorithm>

namespace engine::protocol {

namespace {

bool live(const LeaseInfo& lease, i64 now_ms) noexcept { return lease.expires_unix_ms > now_ms; }

bool names(const Vector<std::string>& list, std::string_view name) noexcept {
  for (const std::string& n : list) {
    if (n == name) return true;
  }
  return false;
}

}  // namespace

bool lease_covers(const LeaseInfo& lease, std::string_view layer, const doc::TileCoord* tile,
                  std::string_view type) noexcept {
  if (lease.layer != layer) return false;
  if (tile != nullptr) {
    for (const TileRange& r : lease.tiles) {
      if (tile_in(r, *tile)) return true;
    }
  }
  return !type.empty() && names(lease.object_types, type);
}

u32 LeaseTable::expire(i64 now_ms) {
  u32 dropped = 0;
  for (u32 i = file_.leases.size(); i > 0; --i) {
    if (!live(file_.leases[i - 1], now_ms)) {
      file_.leases.erase_at(i - 1);
      ++dropped;
    }
  }
  return dropped;
}

const LeaseInfo* LeaseTable::overlap(const LeaseInfo& wanted, i64 now_ms) const noexcept {
  for (const LeaseInfo& held : file_.leases) {
    if (held.actor == wanted.actor || held.layer != wanted.layer || !live(held, now_ms)) continue;
    for (const TileRange& a : held.tiles) {
      for (const TileRange& b : wanted.tiles) {
        if (ranges_overlap(a, b)) return &held;
      }
    }
    for (const std::string& type : wanted.object_types) {
      if (names(held.object_types, type)) return &held;
    }
  }
  return nullptr;
}

u64 LeaseTable::add(LeaseInfo lease) {
  lease.id = file_.next_id++;
  const u64 id = lease.id;
  file_.leases.push_back(std::move(lease));  // ids only grow, so the table stays in id order
  return id;
}

LeaseInfo* LeaseTable::find(u64 id) noexcept {
  for (LeaseInfo& l : file_.leases) {
    if (l.id == id) return &l;
  }
  return nullptr;
}

bool LeaseTable::remove(u64 id) {
  for (u32 i = 0; i < file_.leases.size(); ++i) {
    if (file_.leases[i].id == id) {
      file_.leases.erase_at(i);
      return true;
    }
  }
  return false;
}

LeaseTable::Cover LeaseTable::check(std::string_view actor, std::string_view layer,
                                    const doc::TileCoord* tile, std::string_view type, i64 now_ms,
                                    const LeaseInfo** other) const noexcept {
  bool mine = false;
  for (const LeaseInfo& lease : file_.leases) {
    if (!live(lease, now_ms) || !lease_covers(lease, layer, tile, type)) continue;
    if (lease.actor != actor) {
      if (other != nullptr) *other = &lease;
      return Cover::held_by_other;
    }
    mine = true;
  }
  return mine ? Cover::covered : Cover::not_held;
}

void LeaseTable::assign(LeaseFile file) {
  file_ = std::move(file);
  std::sort(file_.leases.begin(), file_.leases.end(),
            [](const LeaseInfo& a, const LeaseInfo& b) { return a.id < b.id; });
  u64 next = 1;
  for (const LeaseInfo& l : file_.leases)
    next = l.id >= next ? l.id + 1 : next;
  if (file_.next_id < next) file_.next_id = next;
}

}  // namespace engine::protocol
