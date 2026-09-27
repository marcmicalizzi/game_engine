#pragma once

// The dune field as a function of (world seed, position, game time) (docs/subsystems/terrain.md,
// "The time function"; ADR-0043). Nothing about the dunes' evolution is stored: the height at a
// point at time t is computed from the seed and t alone, at the same cost for any t, and evaluating
// t2 directly is the same bits as evaluating t1 first and then t2, because there is nothing an
// evaluation leaves behind.
//
// **Primitives in an advected lattice.** The field is three **bands** of parametric dune
// primitives, superposed: `draa`, long transverse ridges a couple of wavelengths apart; `crest`,
// transverse crest segments at the scene's wavelength riding on them; and `barchan`, sparse
// crescents on the flats. Each band's primitives are placed by the seed on a lattice of cells, one
// primitive (or none) per cell — a crest line with a height, a length, a bend, a stoss slope and a
// slip face, or a barchan's dome and scoop — and the lattice **moves with the wind**: the whole
// band is displaced by the flux integral to t divided by the band's height,
//
//     D_band(t) = I(t) / H_band                 (Bagnold: celerity = sand flux / dune height)
//
// so the height at p is the band's primitives evaluated at p - D_band(t). Barchans, the lowest
// band, overtake the ridges; the draa hardly move. A primitive is owned by a cell of its band's
// moving lattice, never by a world tile: a primitive owned by a tile would leave it as it
// migrated, and the neighbours a tile had to consult would grow with t, where a lattice that moves
// with the wind makes the neighbours of any point at any time a fixed number of cells.
//
// **What t changes besides position.** A slip face's side and sharpness come from the wind of the
// last 120 and 30 days (`WindRecord::between`, closed form): a sustained wind steepens the lee to
// the angle of repose, a calm month rounds it, a reversal moves the slip face to the other side —
// each blended continuously, so no crest ever flips between two frames. A barchan points its horns
// down the last month's resultant wind. Ripples follow the day's wind.
//
// **Fixed features the sand flows around.** Rock ridges and basins stay where the scene put them,
// never move and are never covered by sand (the sand thins over a ridge and flattens in a basin as
// the renderer's heightfield always did). Near a ridge each band's lattice is **held back** along
// the prevailing wind — the lookup point shifts upwind by a lag that falls off with distance from
// the rock — so every crest bows round the ridge, at every t, deterministically. The same lag is
// the only thing the player's world may feed back into the dunes (feedback.h): a tile's saturating
// per-band lag, interpolated between tile centres, and nothing else. The base field never reads the
// deformation overlay.
//
// **Integer throughout** (fixed.h): millimetres, micrometres, Q16. A seeded choice is made in
// centimetres from the engine's hash; the surface is micrometres. `height_m` converts at the end.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/terrain/wind.h>

#include <span>
#include <string>

namespace engine::terrain {

class LagField;

// A tile of the world's grid: tile (x, z) covers [x, x + 1) * tile size in x and z.
struct TileCoord {
  i32 x = 0;
  i32 z = 0;
  constexpr bool operator==(const TileCoord&) const = default;
};
// x in the high 32 bits and z in the low, each as its two's complement: the packing the world's
// store already files a tile's events and projections under (world.md, "The store's tile id").
constexpr u64 tile_key(TileCoord t) noexcept {
  return static_cast<u64>(static_cast<u32>(t.x)) << 32 | static_cast<u32>(t.z);
}

// A field has at most this many bands (terrain.md, "The band table"): the erg profile uses five.
inline constexpr u32 k_max_bands = 8;
// A tile's stored lag has one unit per slot (feedback.h); band b reads slot min(b, 2). Every nudge
// moves every slot alike, so the slot a band reads changes nothing today, and the record format
// (three bytes) stays what saves already hold.
inline constexpr u32 k_lag_slots = 3;
inline constexpr u32 lag_slot(u32 band) noexcept {
  return band < k_lag_slots ? band : k_lag_slots - 1;
}

enum class PrimitiveKind : u8 { transverse = 0, barchan = 1 };
const char* primitive_kind_name(PrimitiveKind kind) noexcept;

// Where a band may stand relative to the bands before it in the table (terrain.md, "The band
// table"): anywhere; only on their flanks (it fades in across `couple_mm` inside their
// footprints); or only on the floors between them (it fades out across the same).
enum class BandCouple : u8 { none = 0, flanks = 1, floors = 2 };
const char* band_couple_name(BandCouple couple) noexcept;

// A band as a scene describes it, in metres and fractions (`engine.scene.TerrainBand`); the
// renderer and the content tool read it from their own types and hand it over in this one.
struct BandMetres {
  PrimitiveKind kind = PrimitiveKind::transverse;
  f32 height_min = 1.0f, height_max = 2.0f;
  f32 cell = 90.0f;
  f32 share = 1.0f;
  f32 length_min = 0.45f, length_max = 0.85f;
  f32 stoss = 0.4f, bend = 0.25f, sinuosity = 0.0f;
  f32 spread_deg = 20.0f;
  f32 sharpness = 1.0f;
  u32 side_days = 120, sharp_days = 30;
  BandCouple couple = BandCouple::none;
  f32 couple_width = 0.0f;  // metres inside the earlier bands' footprints
  bool far = true;
  // A stylization for cinematic time-lapse rates: the band travels Bagnold's distance times this.
  // 1 is the physical rate (terrain.md, "How far the big dunes move").
  f32 celerity_scale = 1.0f;
  std::string name;
};

// One band of the dune field: a population of primitives of one kind placed on a lattice of
// square cells, at most one a cell. Every seeded choice is made in centimetres (the lattice, the
// heights, the centre's jitter) and fractions of the cell in Q16, so a band is the same bits on
// every toolchain. The empty table is the field's default: the three bands `default_bands` derives
// from the dune height and wavelength, which is what every field was before the table existed.
struct BandDesc {
  PrimitiveKind kind = PrimitiveKind::transverse;
  i64 cell_cm = 9'000;  // the lattice's cell
  i64 height_lo_cm = 90, height_hi_cm = 180;
  i32 presence_q16 = 65536;        // the share of cells that hold a primitive
  i32 half_length_lo_q16 = 29491;  // a crest's half length, of the cell (transverse)
  i32 half_length_hi_q16 = 55706;
  i32 stoss_q16 = 26214;   // the windward width, of the cell (transverse)
  i32 bend_q16 = 16384;    // how far the ends lie downwind, of the cell, either way
  i32 spread_turn = 3641;  // how far a crest strays from square to the wind
  // Added in the band table (not in the default's three, which leave them at these values).
  i32 sinuosity_q16 = 0;      // a crest's meander amplitude, of the cell (transverse)
  i32 sharp_cap_q16 = 65536;  // the sharpest its lee gets: 0 never a slip face
  i32 side_days = 120;        // the wind that decides the slip face's side
  i32 sharp_days = 30;        // and its sharpness (and a barchan's heading)
  BandCouple couple = BandCouple::none;
  i64 couple_mm = 0;  // the width inside the earlier bands' footprints the coupling fades across
  bool far = true;    // kept by `Detail::coarse`
  char name[15] = {};
  // The band's travel over Bagnold's, Q16: 65536, the physical rate, unless a scene stylizes it
  // for a cinematic time-lapse (`BandMetres::celerity_scale`). Its celerity height is the middle
  // of its height range over this (`DuneField::band_height`).
  i32 celerity_q16 = 65536;
};

// Metres to the band's own units: centimetres and Q16 rounded to nearest, degrees to a binary
// angle. Validate the result (`validate_bands`); nothing here refuses.
BandDesc band_from_metres(const BandMetres& band) noexcept;

// The default table: draa two wavelengths apart as tall as the dune height, crest segments at the
// wavelength half as tall, sparse barchans — exactly the constants of the field before the table.
Vector<BandDesc> default_bands(i64 dune_height_mm, i64 wavelength_mm);
// False with a reason for a table a field cannot be built from: empty or longer than
// `k_max_bands`, a band with no cells occupied, heights that are not positive and ordered, a cell
// smaller than the band's own dune (a transverse crest's stoss and lee, a barchan's width), bands
// not in order of their tallest (a band is shaped by the ones before it).
bool validate_bands(std::span<const BandDesc> bands, std::string* error = nullptr);

// The capability's LOD policy (ADR-0027): which terms of the surface an evaluation computes. The
// tile mesh at the world's 25 cm grid is `dunes`; ripples and grain are a few millimetres over 12
// centimetres and only mean something to a sample spacing a quarter of that, the renderer's
// deformation-map refinement (plan 05 §5.13); past a couple of kilometres the barchans are a few
// pixels and `coarse` drops them; `floor` is the ground with no dune at all (what ruins stand on).
enum class Detail : u8 { full = 0, dunes = 1, coarse = 2, floor = 3 };
const char* detail_name(Detail detail) noexcept;
// The detail a sample spacing can carry: ripples need four samples a wavelength.
Detail detail_for_spacing(i64 spacing_mm) noexcept;
// The detail a tile at `distance_mm` from its nearest observer is built at: the policy's function.
Detail detail_for_distance(i64 distance_mm) noexcept;

// A fixed rock ridge: a segment and the width at which its profile reaches zero. mm.
struct RidgeFeature {
  i64 from_x = 0, from_z = 0;
  i64 to_x = 0, to_z = 0;
  i64 width = 100'000;
};
// A fixed basin: a centre and a radius. mm.
struct BasinFeature {
  i64 x = 0, z = 0;
  i64 radius = 100'000;
};

struct FieldDesc {
  u64 seed = 1;
  // The dune field's scale: a typical draa's height and the crest band's spacing. mm.
  i64 dune_height = 3'000;
  i64 wavelength = 90'000;
  // The slow roll of the interdune floor under the dunes, Q16 of `dune_height`.
  i32 roll_q16 = 22938;
  // How far a ridge holds the sand back where it stands, Q16 of the ridge's width.
  i32 ridge_lag_q16 = 19661;
  WindParams wind;
  Vector<RidgeFeature> ridges;
  Vector<BasinFeature> basins;
  // The band table; empty is `default_bands(dune_height, wavelength)`.
  Vector<BandDesc> bands;
};

// Bumped when anything a desc evaluates to changes, so a derived-data entry or a golden hash that
// names this generator is never mistaken for another's.
// 2: the repose limiter (each band's lee absorbs the bands after it; lees measured across the
// local crest) and the band table.
inline constexpr u32 k_generator_version = 2;
u64 field_hash(const FieldDesc& desc) noexcept;

// One primitive as a gather holds it at one time: 88 bytes (tests/size_table.cpp). Positions are
// in its band's moving frame, mm; `height` is µm; the time-dependent shape (`side_q16`,
// `sharp_q16`, the barchan's `dir`) was worked out for the gather's time.
struct Primitive {
  i64 cx = 0, cz = 0;   // centre in the band frame
  i32 ax = 0, az = 0;   // along the crest (a barchan: down the wind), Q14 unit
  i32 height = 0;       // µm
  i32 half_length = 0;  // mm; a barchan: its downwind radius
  i32 stoss = 0;        // mm, the gentle windward width; a barchan: its upwind radius
  i32 bend = 0;         // mm the crest's ends lie downwind of its middle
  i32 reach = 0;        // mm from the centre beyond which it adds nothing
  // Which side the slip face is on, Q16 in [-1, 1]: +1 on the side of the crest's normal (az, -ax),
  // which points down the prevailing wind, -1 the other; between, the two profiles blend.
  i32 side_q16 = 0;
  i32 sharp_q16 = 0;  // Q16: 0 a rounded lee, 1 a slip face at the angle of repose
  i32 lee = 0;        // mm: the slip face's width at the angle of repose (height / tan 34)
  // Q32 reciprocals of the widths a point is divided by, worked out once by the gather: of the half
  // length, the stoss, and a third width — the lee for a transverse crest, the lateral radius (its
  // `bend`) for a barchan.
  u32 inv_half = 0;
  u32 inv_stoss = 0;
  u32 inv_width = 0;
  // A crest's meander: its line moves `meander` mm either side, one wave a cell along its length
  // (Q32 reciprocal of that wavelength), from a seeded phase. Zero for a band with no sinuosity.
  i32 meander = 0;
  u32 inv_meander = 0;
  u16 cell_hash = 0;  // the low bits of its cell's hash, for reports
  u16 meander_phase = 0;
  u8 band = 0;
  u8 kind = 0;  // PrimitiveKind
  // The share of the half length its crest tapers over at each end, Q16: 0.4, or six times its
  // height when that is longer (terrain.md, "The repose limiter"). Zero for a barchan.
  u16 taper_q16 = 0;
  // How far before its brink and past its lee the primitive absorbs the bands after it, mm: its
  // band's `absorb` width (terrain.md, "The repose limiter").
  i32 absorb = 0;
  // How far across its mean line (less its bend) a crest can still add anything, mm: the wider of
  // its stoss and its lee with the lee zone, over the cosine of the most its line can drift, since
  // its profile is measured across the local crest. Zero for a barchan.
  i32 widen = 0;
  u32 inv_taper = 0;  // Q32 reciprocal of `taper_q16` (so Q16 of its reciprocal as a fraction)
};

// A region's primitives at one time: what every point of the region reads, gathered once. A tile
// gathers once and evaluates thousands of points against it.
struct Gather {
  i64 time_us = 0;
  i64 dx[k_max_bands] = {};  // each band's displacement by the wind to `time_us`, mm
  i64 dz[k_max_bands] = {};
  Vector<Primitive> primitives;  // band order, then cell order
  u32 bands = 0;
  u32 band_begin[k_max_bands + 1] = {};
  // Each band's primitives binned by lattice cell: the rectangle of cells the gather covered and,
  // per cell, the index of its primitive or -1. A point visits only the cells whose primitives can
  // reach it, so a band of ten-metre waves costs a point 25 cells, not the thousand a large gather
  // holds.
  i64 grid_i0[k_max_bands] = {};
  i64 grid_j0[k_max_bands] = {};
  u32 grid_ni[k_max_bands] = {};
  u32 grid_nj[k_max_bands] = {};
  u32 grid_begin[k_max_bands] = {};
  Vector<i32> grid;
  const LagField* lag = nullptr;
  // The ripples' direction today and yesterday, Q14, their drift, and today's blend.
  i32 ripple_x = 0, ripple_z = 0, ripple_prev_x = 0, ripple_prev_z = 0;
  i64 ripple_shift = 0, ripple_prev_shift = 0;  // mm
  i32 ripple_amp = 0, ripple_prev_amp = 0;      // µm
  i32 ripple_blend_q16 = 0;                     // 0 yesterday's pattern, 1 today's
};

// The terms a point's height is made of, µm; `height_um` is their sum.
struct Sample {
  i64 floor = 0;      // the interdune floor's roll: fixed in the world
  i64 sand = 0;       // every band, masked by the fixed features
  i64 detail = 0;     // ripples and grain
  u16 ridge_q16 = 0;  // how much of a ridge is under the point
  u16 basin_q16 = 0;  // and of a basin
};

class DuneField {
 public:
  explicit DuneField(const FieldDesc& desc);

  const FieldDesc& desc() const noexcept { return desc_; }
  const WindRecord& wind() const noexcept { return wind_; }
  u64 hash() const noexcept { return hash_; }

  // The most the fixed features and a lag field can hold a band back, mm: what a gather adds to its
  // rectangle so no primitive that a point could see is missed.
  i64 max_lag_mm(const LagField* lag) const noexcept;

  // Every primitive that can reach a point of [x0, x1] x [z0, z1] (world mm) at `time_us`.
  void gather(i64 x0, i64 z0, i64 x1, i64 z1, i64 time_us, const LagField* lag, Gather& out) const;
  // The surface at a world point, against a gather that covers it.
  Sample sample(const Gather& gather, i64 x, i64 z, Detail detail) const noexcept;
  i64 height_um(const Gather& gather, i64 x, i64 z, Detail detail) const noexcept;
  // How much of band `b` its coupling lets stand at a point, Q16: 1 for an uncoupled band, and
  // for a coupled one the fade the evaluation applies there. What the statistics weigh a crest by.
  i64 band_weight_q16(const Gather& gather, u32 b, i64 x, i64 z) const noexcept;
  // One point on its own: gathers a region of one millimetre. For a handful of queries; anything
  // that asks for a grid gathers once.
  i64 height_um(i64 x, i64 z, i64 time_us, Detail detail = Detail::dunes,
                const LagField* lag = nullptr) const;
  // The interdune floor alone (Detail::floor): fixed in time. The ground a ruin stands on — dunes
  // migrate over it and bury what stands there, which is what a ruin in a moving desert is.
  i64 floor_um(i64 x, i64 z) const noexcept;
  // How much ridge and basin are under a point, Q16: what the materials are chosen from, and what
  // the overlay's fill rate is (rock does not take a footprint).
  i32 ridge_q16(i64 x, i64 z) const noexcept;
  i32 basin_q16(i64 x, i64 z) const noexcept;

  // The ruins' height query (`ruins::Ground`, whose signature this matches without this module
  // depending on the ruins capability): `context` is a DuneField, (x, z) metres, and the answer
  // the floor in metres.
  static f32 ground_height(const void* context, f32 x, f32 z) noexcept;

  // The table the field was built from (the default's three when the description had none).
  u32 band_count() const noexcept { return bands_.size(); }
  const BandDesc& band(u32 b) const noexcept { return bands_[b]; }
  const char* band_name(u32 b) const noexcept { return bands_[b].name; }
  // The largest reach of any primitive in a band, mm.
  i64 band_reach(u32 b) const noexcept { return reach_[b]; }
  i64 band_cell(u32 b) const noexcept { return cell_[b]; }
  // The height a band's lattice moves as (its celerity is flux / this), mm: the middle of its
  // range, over its `celerity_scale` when a scene stylizes it.
  i64 band_height(u32 b) const noexcept { return celerity_height_[b]; }

  // Each band's displacement by the wind from time 0 to `time_us`, mm: the closed form.
  void displacement(u32 band, i64 time_us, i64& dx, i64& dz) const noexcept;

  // The primitive of one cell, at a time: exposed for the tests and for reports.
  bool primitive(u32 band, i64 cell_i, i64 cell_j, i64 time_us, Primitive& out) const noexcept;

 private:
  struct TimeShape {
    i64 r120x = 0, r120z = 0;          // the side window's resultant flux (120 days by default)
    i64 r30x = 0, r30z = 0;            // the sharpness window's (30 days)
    i64 sharp_ref = 1;                 // three quarters of that window of the mean flux, one way
    i32 barchan_x = 0, barchan_z = 0;  // Q14: where barchans point this time
  };
  TimeShape time_shape(u32 band, i64 time_us) const noexcept;
  bool make_primitive(u32 band, i64 i, i64 j, const TimeShape& shape,
                      Primitive& out) const noexcept;
  // A primitive's height at an offset from its centre, µm, and a bound on its slope there, Q16 (a
  // tangent) — continuous, and the angle of repose over its lee zone — that the bands after it are
  // scaled by (terrain.md, "The repose limiter").
  struct Value {
    i64 height = 0;
    i64 slope = 0;
    i64 inside = 0;  // mm: how deep inside its footprint the point is, what bands couple to
  };
  // `k_slope` false leaves `slope` and `inside` zero: the last band's are read by nothing.
  template <bool k_slope>
  static Value primitive_value(const Primitive& p, i64 dx, i64 dz) noexcept;
  // A band's sand at a point in its frame (the maximum of its primitives) and its slope bound
  // (the maximum of theirs).
  template <bool k_slope>
  Value band_value(const Gather& gather, u32 band, i64 qx, i64 qz) const noexcept;
  i64 ridge_lag_mm(i64 x, i64 z) const noexcept;
  i64 detail_um(const Gather& gather, i64 x, i64 z) const noexcept;
  // The fixed features at a point: the ridge's profile, the basins' flattening, the ridge lag, and
  // `squeeze_q16`, a bound on how much the lag's gradient compresses the bands' lattice there (the
  // bands stand lower by it), and `cap_um`, the most sand the features let stand there (a cone at
  // 30 degrees from a ridge's line and a basin's flat core; terrain.md, "The repose limiter").
  void features(i64 x, i64 z, i64& ridge_q16, i64& basin_flatten_q16, i64& ridge_lag,
                i64& squeeze_q16, i64& cap_um) const noexcept;

  FieldDesc desc_;
  WindRecord wind_;
  u64 hash_ = 0;
  Vector<BandDesc> bands_;
  i64 cell_[k_max_bands] = {};             // mm
  i64 reach_[k_max_bands] = {};            // mm
  i64 celerity_height_[k_max_bands] = {};  // mm
  i64 absorb_[k_max_bands] = {};           // mm: the lee zone's fade width
  i64 drift_q16_[k_max_bands] = {};
  i64 cap_reach_ = 0;  // mm: where a feature's cap passes the tallest sand the bands can stack //
                       // the most a crest line drifts per unit along it
  i32 roll_x_ = 0, roll_z_ = 0;  // Q14
  i64 roll_length_ = 1;          // mm
  u32 roll_phase_ = 0;
  i64 max_ridge_lag_ = 0;  // mm
};

// Metres from micrometres, the one conversion to a float the field makes.
inline f32 height_m(i64 um) noexcept { return static_cast<f32>(static_cast<f64>(um) * 1e-6); }
// Millimetres from metres, rounded to nearest: how a float query enters the integer field.
i64 to_mm(f32 metres) noexcept;

}  // namespace engine::terrain
