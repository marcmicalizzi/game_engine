#pragma once

// The one feedback the player's world may have on the dunes, and its bound (docs/subsystems/
// terrain.md, "The feedback rule"; ADR-0043). The owner would like footprints to change the dunes'
// future; the storage rule forbids a coupling that grows, because a footprint that perturbed the
// base field's primitives would make every later evaluation of that field depend on history. So:
//
//   - The base field never reads the deformation overlay (overlay.h). Not a footprint, not a pit.
//   - The **only** feedback is a **lag**: per tile and per band, a small quantized distance by
//   which
//     the band's lattice is held back upwind while it passes the tile — "your walls changed where
//     the dune came to rest". It is a few bytes of the tile's fixed-size state (`TileLag`, 4
//     bytes), it saturates (`FeedbackRules::lag_max_units`), and between tile centres it is
//     interpolated, so the surface stays continuous across tiles.
//   - It is nudged by **large obstacles only**, a bounded amount a day: a dug pit above a volume
//     nudges the tile's stored lag one unit, at most once a game day (overlay.h); a declared drift
//     above a volume — a wall's, the ruins' or one the player built — holds its tile back one unit
//     for every day since it was declared, as a closed form of time that needs no storage at all
//     (`derived_lag_units`).
//
// So the history a lag carries is at most `lag_max_units` of it, per band per tile, and the field
// that reads it is still a function of (seed, tile lags, t): the same lags at the same time are the
// same dunes, however the lags came to be.
//
// **Why per tile and not per primitive.** A primitive belongs to a cell of its band's moving
// lattice and migrates through tiles: an offset stored with the primitive would have to be found
// wherever the primitive had got to — a lookup into the state of whatever tile it was nudged in,
// arbitrarily far upwind after years — and written into tiles that are not loaded. A lag stored
// with the tile holds back *every* primitive of the band while it passes, which is what an obstacle
// does to sand, and costs one read of the tile and its eight neighbours.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <domain/terrain/dunes.h>

#include <span>

namespace engine::terrain {

// A tile's stored feedback: one lag per band, in `FeedbackRules::lag_unit_mm` units. 4 bytes.
struct TileLag {
  u8 units[k_bands] = {};
  u8 reserved = 0;
  constexpr bool operator==(const TileLag&) const = default;
  bool zero() const noexcept { return units[0] == 0 && units[1] == 0 && units[2] == 0; }
};

struct FeedbackRules {
  // The lag's quantum and its ceiling: 8 cm a unit, 96 units (7.68 m).
  i32 lag_unit_mm = 80;
  i32 lag_max_units = 96;
  // A pit nudges its tile when the sand dug out of it is at least this much (1 m^3), counted
  // over the cells deeper than `pit_depth_mm`.
  i64 pit_volume_mm3 = 1'000'000'000;
  i32 pit_depth_mm = 100;
  // A declared drift holds its tile back from the day it was declared if it is at least this
  // large (1 m^3).
  i64 drift_volume_mm3 = 1'000'000'000;
};

// The sand a wall face asks for against it (the ruins' `Drift`, in this module's terms, so the
// module does not depend on the ruins capability): a face from `from` to `to` with an outward
// normal, a drift `height` against it falling to nothing `reach` out, declared on `since_day`
// (0 for a ruin that stood before the world began). World mm.
struct DriftDecl {
  i64 from_x = 0, from_z = 0;
  i64 to_x = 0, to_z = 0;
  i32 normal_x_q14 = 0, normal_z_q14 = 0;
  i32 height_mm = 300;
  i32 reach_mm = 1600;
  i64 since_day = 0;
};

// The drift's volume, mm^3: the face's length times its triangular section.
i64 drift_volume_mm3(const DriftDecl& drift) noexcept;
// The drift's steady-state height at a world point, µm: `height` at the face, falling linearly to
// nothing `reach` out, along the face's length. 0 behind the face or off its ends.
i64 drift_height_um(const DriftDecl& drift, i64 x, i64 z) noexcept;
// The lag the declared drifts of one tile hold it back by at `day`: one unit a day since the
// earliest drift large enough was declared, saturating. A closed form of time: no storage.
u32 derived_lag_units(std::span<const DriftDecl> drifts, i64 day,
                      const FeedbackRules& rules) noexcept;

// The lag of every tile that has one, and the interpolation between tile centres the dune field
// reads (dunes.h, `Gather::lag`). A tile with no entry has no lag. What a host puts here is the
// stored lag plus the derived one, per tile, clamped to the ceiling.
class LagField {
 public:
  explicit LagField(i64 tile_mm = 32'000, const FeedbackRules& rules = {});

  void set(TileCoord tile, TileLag lag);
  TileLag get(TileCoord tile) const noexcept;
  void clear() noexcept { tiles_.clear(); }
  u32 size() const noexcept { return tiles_.size(); }

  // The lag at a world point for a band, mm: bilinear between the four nearest tile centres, so it
  // is continuous everywhere and a tile's own centre reads its own lag exactly.
  i64 lag_mm(u32 band, i64 x, i64 z) const noexcept;
  i64 max_lag_mm() const noexcept {
    return static_cast<i64>(rules_.lag_max_units) * rules_.lag_unit_mm;
  }
  i64 tile_mm() const noexcept { return tile_mm_; }

 private:
  i64 tile_mm_;
  FeedbackRules rules_;
  HashMap<u64, TileLag> tiles_;
};

}  // namespace engine::terrain
