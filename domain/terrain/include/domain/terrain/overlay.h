#pragma once

// The deformation overlay: what the player changed, and nothing else (docs/subsystems/terrain.md,
// "The overlay and its bound"; plan 03 §3.5, 05 §5.13; ADR-0043). Footprints, digging and sand a
// player dumps are **stamps** applied to a per-tile grid of fixed resolution and fixed size, which
// **decays toward the base function** as the wind fills it: every vertex moves toward zero by an
// amount proportional to the sand flux that has blown since (the wind record's magnitude integral,
// closed form), faster where sand is loose and not at all on rock, and snaps to zero once it is
// within `OverlayRules::bury_mm` of it. A footprint is gone from storage when it is buried.
//
// **What the grid holds.** One `i16` per vertex, millimetres, 128 x 128 a tile — at the world's 32
// m tile, a vertex every 25 cm, which is plan 05 §5.13's coarse deterministic CPU grid (25–50 cm
// cells) that gameplay reads for foot depth, movement penalty, tracking and audio; the GPU
// refinement the renderer will stamp from the same events is its own business. The value is the
// **deviation from the steady state**: the base dunes plus the drift every declared wall face asks
// for (feedback.h, `drift_height_um`). A ruin's drift is therefore already there when its tile
// first comes in — the walls are older than the player — and costs nothing to store; dig it out and
// the deviation is negative, and the wind refills it toward the drift's steady state, which is "a
// wall's drift grows toward its declared steady state". A wall the player builds declares its drift
// on the day it is built (`declare_drift`), which starts it empty.
//
// **Exact whatever the cadence.** Decay by an amount, then a snap to zero near zero, composes: two
// steps from t0 to t1 and t1 to t2 are one step from t0 to t2, bit for bit, because each vertex's
// amount is a difference of floors of one absolute function of time. So an overlay advanced every
// tick, one advanced once a day, and one caught up when its tile comes back after a month are the
// same bytes, and a save between any two of them loads to the same world. Stamps queue (a bounded
// queue of recent stamps not yet baked in, `k_stamp_queue`) and bake in time order at the next
// advance or when the queue is full; baked eagerly or late they give the same grid, which the tests
// assert.
//
// **The bound, as a number.** A tile's persistent record is the header, the 16 x 16 blocks of the
// grid that are not all zero, and the queue: at most `k_overlay_record_max_bytes` = 33,848 bytes,
// whatever happened on the tile and for however long. A tile whose grid is all zero, whose lag is
// zero and whose queue is empty stores nothing. How long a tile can keep a record after the last
// stamp is bounded too: a vertex is never deeper than `max_depth_mm`, so it is buried within that
// depth's worth of flux (`burial_time_us` says when), 33 days of the default wind for the deepest
// pit. A player who survives for weeks stores what the last few weeks' footprints left unburied,
// which is no more than one who survived a day left after a day.
//
// **Seams.** A tile owns its vertices [0, 128) x [0, 128); its far edge is its neighbour's first
// row, read from the neighbour's overlay when that is loaded (tile.h). A stamp near an edge is
// applied to every loaded tile its footprint covers, by the host.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/feedback.h>
#include <domain/terrain/wind.h>

#include <span>
#include <string>

namespace engine::terrain {

inline constexpr u32 k_overlay_cells = 128;  // vertices a side
inline constexpr u32 k_overlay_block = 16;   // a block's side, in vertices
inline constexpr u32 k_overlay_blocks = k_overlay_cells / k_overlay_block;  // blocks a side
inline constexpr u32 k_stamp_queue = 32;
inline constexpr u32 k_overlay_header_bytes = 56;
inline constexpr u32 k_stamp_bytes = 32;
inline constexpr usize k_overlay_record_max_bytes =
    k_overlay_header_bytes +
    k_overlay_blocks * k_overlay_blocks * k_overlay_block * k_overlay_block * sizeof(i16) +
    k_stamp_queue * k_stamp_bytes;
static_assert(k_overlay_record_max_bytes == 33'848);
inline constexpr u32 k_overlay_record_version = 1;

enum class StampKind : u8 { footprint = 0, dig = 1, fill = 2 };
const char* stamp_kind_name(StampKind kind) noexcept;

// One deformation: a footprint, a dug hole, or sand dumped. 32 bytes, also its stored form.
struct Stamp {
  i64 time_us = 0;
  i32 x_mm = 0;  // world, its centre
  i32 z_mm = 0;
  u16 radius_mm = 150;  // the ellipse's semi-axis along `yaw`
  u16 radius2_mm = 60;  // and across it
  u16 yaw_turn = 0;     // binary angle
  i16 depth_mm = 30;    // how far down (footprint, dig) or up (fill)
  i16 rim_mm = 0;       // the rim a footprint or a dig raises round itself
  u8 kind = 0;          // StampKind
  u8 reserved[5] = {};
};

struct OverlayRules {
  // µm of sand the wind puts back per cm^2 of flux that has blown over a vertex: 11 fills a 3 cm
  // footprint in about half a day of the default mean wind and a 2 m pit in about 33 days.
  i32 fill_um_per_cm2 = 11;
  // A vertex within this of the steady state is buried: zero, and stored as nothing.
  i32 bury_mm = 5;
  // No stamp takes a vertex deeper or higher than this: what bounds a record's life.
  i32 max_depth_mm = 2'000;
  FeedbackRules feedback;
  u64 hash() const noexcept;
};

class Overlay {
 public:
  Overlay() = default;

  // An empty overlay for `tile` at `time_us`; the fill rates start at loose sand everywhere.
  void reset(TileCoord tile, i64 tile_mm, i64 time_us);
  // The fill rate of every vertex from the field: rock (a ridge) takes no sand and no footprint.
  // Derived, never stored; call after `reset` or `read`.
  void set_fill_rates(const DuneField& field);

  // Queue a stamp. False (and nothing queued) if it is earlier than the last stamp or than the
  // overlay's own time: time runs one way. A full queue is baked first.
  bool push(const Stamp& stamp, const WindRecord& wind, const OverlayRules& rules);
  // A drift declared now starts empty: its vertices' deviation becomes at most minus the drift.
  void declare_drift(const DriftDecl& drift, const WindRecord& wind, const OverlayRules& rules);
  // Bake the queue in time order, then decay to `time_us` (no-op for an earlier time).
  void advance(i64 time_us, const WindRecord& wind, const OverlayRules& rules);

  TileCoord tile() const noexcept { return tile_; }
  i64 tile_mm() const noexcept { return tile_mm_; }
  i64 spacing_mm() const noexcept { return tile_mm_ / k_overlay_cells; }
  i64 as_of_us() const noexcept { return as_of_us_; }
  u32 pending() const noexcept { return queue_count_; }
  TileLag lag() const noexcept { return lag_; }
  i64 last_nudge_day() const noexcept { return last_nudge_day_; }
  // The deviation at a vertex the tile owns, mm.
  i16 at(u32 i, u32 j) const noexcept { return cells_[j * k_overlay_cells + i]; }
  // At a world point: the nearest vertex this tile owns, 0 outside it. The gameplay read.
  i32 deviation_mm(i64 x, i64 z) const noexcept;
  // Every vertex zero, no lag, nothing queued: a tile that stores nothing.
  bool empty() const noexcept;
  u32 nonzero_blocks() const noexcept;
  // The sand removed below the surface, mm^3 (what a pit's nudge is measured by).
  i64 dug_volume_mm3(i32 below_mm) const noexcept;

  // The persistent record: nothing for an empty overlay, else the header, the non-zero blocks
  // and the queue, little-endian. At most `k_overlay_record_max_bytes`.
  void write(Vector<u8>& out, const OverlayRules& rules) const;
  // False with a reason for a record that is not one, is another tile's, or was written under
  // other rules. Fill rates are reset; call `set_fill_rates`.
  bool read(std::span<const u8> bytes, TileCoord tile, i64 tile_mm, const OverlayRules& rules,
            std::string* error = nullptr);

  // When the last vertex would be buried if nothing more happened: the time after which the tile
  // stores nothing (the queue baked first). `as_of_us()` for an empty grid; INT64_MAX when the
  // wind never blows or a vertex cannot fill.
  i64 burial_time_us(const WindRecord& wind, const OverlayRules& rules) const;

 private:
  void bake(const WindRecord& wind, const OverlayRules& rules);
  void decay_to(i64 time_us, const WindRecord& wind, const OverlayRules& rules);
  void apply(const Stamp& stamp, const OverlayRules& rules);
  // Recompute the non-zero bit of the blocks in `candidates`; the others keep theirs.
  void refresh_blocks(u64 candidates) noexcept;
  static u64 blocks_between(i64 i_lo, i64 j_lo, i64 i_hi, i64 j_hi) noexcept;

  TileCoord tile_;
  i64 tile_mm_ = 32'000;
  i64 as_of_us_ = 0;
  i64 last_nudge_day_ = -2147483648LL;  // INT32_MIN: no nudge yet, and what a record stores for it
  TileLag lag_;
  Vector<i16> cells_;  // k_overlay_cells^2, deviation mm
  Vector<u8> rate_;    // k_overlay_cells^2, 255 loose sand, 0 rock
  u64 block_mask_ = 0;
  Stamp queue_[k_stamp_queue];
  u32 queue_count_ = 0;
};

}  // namespace engine::terrain
