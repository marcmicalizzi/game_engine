#pragma once

// **A world formation** (roadmap W85; docs/subsystems/renderer.md, "Formation bands"; the
// grounding, docs/experiments/formation-field.md): beds that dip, fold and repeat through the
// world, read by the terrain resolve as a band layer (`shaders/formation.slang`) and by the rock
// solvers as the bedding of a cliff (`domain/rocks`, rocks.md "Geology"). This header is the
// field's one CPU definition: the GPU block the shader reads (`FormationParams`, pinned by the size
// table and mirrored once, in formation.slang), the packing from the scene's
// `engine.scene.Formation` with its validation, and the **double-precision twin** of the shader's
// two functions, which the rocks solver calls and the gfx test holds the shader to
// (`formation_tests.cpp`).
//
// **Why integers, and why the precision holds anywhere.** No absolute position is formed on the
// GPU (ADR-0053). The bed coordinate is `n . (p - origin)`; the shader has the frame's eye cell
// (whole 64 m) and a float offset from it, and the field's origin in whole millimetres, so it takes
// `D = cell * 64000 - origin_mm` in int64 and the float rest separately. The normal and the dip
// direction are **defined** as integers over 2^24 (`k_formation_q_bits`): `n . D` is then an exact
// int64 product sum (|D| < 2^37 mm, 137,000 km, keeps it inside 2^63), reduced modulo the
// sequence's period — also whole millimetres, shifted by 24 bits — before anything becomes a float.
// What reaches float is a remainder under one period plus the frame-local part, so the coordinate
// carries the same few micrometres of error 10 km or 10,000 km from the origin as beside it. The
// fold's phase is reduced the same way modulo its wavelength. The twin below evaluates the same
// definition (the quantized directions, the millimetre origin, the millimetre thicknesses and
// wavelengths) in f64 without any reduction, which is what lets a test hold the two to 1e-3 m.

#include <core/base/types.h>
#include <core/math/deterministic.h>
#include <core/math/math.h>

#include <cmath>
#include <numbers>
#include <schemas/scene.h>
#include <string>

namespace engine::gfx {

inline constexpr u32 k_formation_max_beds = 8;
inline constexpr u32 k_formation_q_bits = 24;
inline constexpr f64 k_formation_q = 1.0 / 16777216.0;  // 2^-24
// FormationParams::flags: the block holds a formation (an all-zero block is none).
inline constexpr u32 k_formation_valid = 1u;

// One bed on the GPU: its value (rgb tint, roughness offset), the integral of the sequence's value
// from the sequence's base to this bed's base (what the box filter differences), where it starts
// and how thick it is in metres (both whole millimetres), and its resistance.
struct FormationBedGpu {
  Vec4 value{1.0f, 1.0f, 1.0f, 0.0f};
  Vec4 integral{};
  f32 start_m = 0.0f;
  f32 thickness_m = 0.0f;
  f32 resistance = 0.0f;
  u32 pad = 0;
};
static_assert(sizeof(FormationBedGpu) == 48);

struct FormationSequence {
  u32 count = 0;      // 0: no sequence, a tint of one everywhere
  u32 period_mm = 0;  // the sum of the beds' whole-millimetre thicknesses
  f32 period_m = 0.0f;
  u32 pad = 0;
  Vec4 total{};  // the value's integral over one period
  FormationBedGpu beds[k_formation_max_beds] = {};
};
static_assert(sizeof(FormationSequence) == 416);

// **The field's GPU block**, one per scene, named by `TerrainRecord::formation` (a device
// address; zero is no formation), mirrored in formation.slang. 928 bytes.
struct FormationParams {
  i64 origin_mm[3] = {};  // the origin, whole millimetres
  i64 normal_q[3] = {};   // the beds' unit normal times 2^24, rounded: the normal is this over 2^24
  i64 dip_q[3] = {};      // the horizontal dip direction the same way
  u32 fold_mm[2] = {};    // the folds' wavelengths, whole millimetres (0: that fold is off)
  f32 fold_amp_m[2] = {};
  f32 soft_m = 0.0f;
  u32 flags = 0;  // k_formation_valid
  FormationSequence beds;
  FormationSequence macro;
};
static_assert(sizeof(FormationParams) == 928);
static_assert(sizeof(FormationParams) % 16 == 0);

// The schema's ranges (scene.schema `Formation`, `FormationBed`), with a sentence naming the first
// field out of them.
inline bool validate_formation(const scene::Formation& f, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = "formation: " + why;
    return false;
  };
  const auto finite = [](f64 v) { return std::isfinite(v); };
  if (!finite(f.origin.x) || !finite(f.origin.y) || !finite(f.origin.z) ||
      std::fabs(f.origin.x) > 1e8 || std::fabs(f.origin.y) > 1e8 || std::fabs(f.origin.z) > 1e8)
    return fail("origin must be finite and within 100,000 km");
  if (!(f.dip_deg >= -85.0f && f.dip_deg <= 85.0f))
    return fail("dip_deg must be in [-85, 85] (a bed is never vertical here)");
  if (!(f.azimuth_deg >= -180.0f && f.azimuth_deg <= 180.0f))
    return fail("azimuth_deg must be in [-180, 180]");
  if (!(f.fold_amplitude_m >= 0.0f && f.fold_amplitude_m <= 1000.0f) ||
      !(f.fold2_amplitude_m >= 0.0f && f.fold2_amplitude_m <= 1000.0f))
    return fail("a fold amplitude must be in [0, 1000] m");
  if (!(f.fold_wavelength_m >= 1.0f && f.fold_wavelength_m <= 100000.0f) ||
      !(f.fold2_wavelength_m >= 1.0f && f.fold2_wavelength_m <= 100000.0f))
    return fail("a fold wavelength must be in [1, 100000] m");
  if (!(f.soft_m >= 0.0f && f.soft_m <= 10.0f)) return fail("soft_m must be in [0, 10] m");
  if (f.beds.empty()) return fail("beds must hold at least one bed");
  if (f.beds.size() > k_formation_max_beds || f.macro_beds.size() > k_formation_max_beds)
    return fail("a sequence holds at most 8 beds");
  for (u32 s = 0; s < 2; ++s) {
    for (const scene::FormationBed& b : s == 0 ? f.beds : f.macro_beds) {
      const std::string who = (s == 0 ? "beds " : "macro_beds ") + b.name;
      if (!(b.thickness_m >= 0.01f && b.thickness_m <= 1000.0f))
        return fail(who + ": thickness_m must be in [0.01, 1000] m");
      const Vec3 t = b.tint.value_or(Vec3{1.0f, 1.0f, 1.0f});
      for (const f32 c : {t.x, t.y, t.z})
        if (!(c >= 0.0f && c <= 2.0f)) return fail(who + ": tint must be in [0, 2]");
      if (!(b.roughness >= -1.0f && b.roughness <= 1.0f))
        return fail(who + ": roughness must be in [-1, 1]");
      if (!(b.resistance >= 0.0f && b.resistance <= 1.0f))
        return fail(who + ": resistance must be in [0, 1]");
    }
  }
  return true;
}

inline void pack_formation_sequence(const Vector<scene::FormationBed>& beds,
                                    FormationSequence& out) {
  const FormationSequence empty;
  out = empty;
  out.count = static_cast<u32>(beds.size());
  u64 start_mm = 0;
  f64 integral[4] = {};
  for (u32 i = 0; i < out.count; ++i) {
    const scene::FormationBed& b = beds[i];
    const u64 thick_mm = static_cast<u64>(std::llround(static_cast<f64>(b.thickness_m) * 1000.0));
    const Vec3 t = b.tint.value_or(Vec3{1.0f, 1.0f, 1.0f});
    FormationBedGpu& g = out.beds[i];
    g.value = Vec4{t.x, t.y, t.z, b.roughness};
    g.integral = Vec4{static_cast<f32>(integral[0]), static_cast<f32>(integral[1]),
                      static_cast<f32>(integral[2]), static_cast<f32>(integral[3])};
    g.start_m = static_cast<f32>(static_cast<f64>(start_mm) / 1000.0);
    g.thickness_m = static_cast<f32>(static_cast<f64>(thick_mm) / 1000.0);
    g.resistance = b.resistance;
    const f64 m = static_cast<f64>(thick_mm) / 1000.0;
    integral[0] += static_cast<f64>(t.x) * m;
    integral[1] += static_cast<f64>(t.y) * m;
    integral[2] += static_cast<f64>(t.z) * m;
    integral[3] += static_cast<f64>(b.roughness) * m;
    start_mm += thick_mm;
  }
  out.period_mm = static_cast<u32>(start_mm);
  out.period_m = static_cast<f32>(static_cast<f64>(start_mm) / 1000.0);
  out.total = Vec4{static_cast<f32>(integral[0]), static_cast<f32>(integral[1]),
                   static_cast<f32>(integral[2]), static_cast<f32>(integral[3])};
}

// The scene's formation into its GPU block: false, with the validation's sentence, when it is out
// of range.
inline bool pack_formation(const scene::Formation& f, FormationParams& out, std::string* error) {
  // A named default rather than `FormationParams{}`: GCC 13 at -O2 stops with an internal compiler
  // error (gimplify.cc:774) on that temporary of nested default-initialized arrays.
  const FormationParams empty;
  out = empty;
  if (!validate_formation(f, error)) return false;
  out.origin_mm[0] = std::llround(f.origin.x * 1000.0);
  out.origin_mm[1] = std::llround(f.origin.y * 1000.0);
  out.origin_mm[2] = std::llround(f.origin.z * 1000.0);
  // rocks.md's convention (detail::make_fabric): the dip direction t is (cos az, 0, sin az) and the
  // normal leans toward it by the dip, so the beds go down along t.
  const f64 dip = static_cast<f64>(f.dip_deg) * std::numbers::pi / 180.0;
  const f64 az = static_cast<f64>(f.azimuth_deg) * std::numbers::pi / 180.0;
  const f64 t[3] = {det::cos(az), 0.0, det::sin(az)};
  const f64 n[3] = {t[0] * det::sin(dip), det::cos(dip), t[2] * det::sin(dip)};
  for (u32 i = 0; i < 3; ++i) {
    out.normal_q[i] = std::llround(n[i] * 16777216.0);
    out.dip_q[i] = std::llround(t[i] * 16777216.0);
  }
  out.fold_mm[0] =
      f.fold_amplitude_m > 0.0f ? static_cast<u32>(std::llround(static_cast<f64>(f.fold_wavelength_m) * 1000.0)) : 0u;
  out.fold_mm[1] = f.fold2_amplitude_m > 0.0f
                       ? static_cast<u32>(std::llround(static_cast<f64>(f.fold2_wavelength_m) * 1000.0))
                       : 0u;
  out.fold_amp_m[0] = out.fold_mm[0] != 0 ? f.fold_amplitude_m : 0.0f;
  out.fold_amp_m[1] = out.fold_mm[1] != 0 ? f.fold2_amplitude_m : 0.0f;
  out.soft_m = f.soft_m;
  out.flags = k_formation_valid;
  pack_formation_sequence(f.beds, out.beds);
  pack_formation_sequence(f.macro_beds, out.macro);
  return true;
}

// ---- The twin, in f64 ---------------------------------------------------------------------------

// The field's geometry as the twin reads it: the definition the GPU block holds, widened.
struct FormationFrame {
  f64 origin[3] = {};  // metres, the whole-millimetre origin
  f64 normal[3] = {};  // normal_q / 2^24
  f64 dip[3] = {};     // dip_q / 2^24
  f64 fold_len[2] = {};
  f64 fold_amp[2] = {};
};
inline FormationFrame formation_frame(const FormationParams& p) {
  FormationFrame f;
  for (u32 i = 0; i < 3; ++i) {
    f.origin[i] = static_cast<f64>(p.origin_mm[i]) / 1000.0;
    f.normal[i] = static_cast<f64>(p.normal_q[i]) * k_formation_q;
    f.dip[i] = static_cast<f64>(p.dip_q[i]) * k_formation_q;
  }
  for (u32 k = 0; k < 2; ++k) {
    f.fold_len[k] = static_cast<f64>(p.fold_mm[k]) / 1000.0;
    f.fold_amp[k] = p.fold_mm[k] != 0 ? static_cast<f64>(p.fold_amp_m[k]) : 0.0;
  }
  return f;
}

// formation.slang's `formation_coordinate` without its reduction: the folded bed coordinate at a
// world point (metres), and its gradient, in f64.
struct FormationCoordinate {
  f64 coordinate = 0.0;
  f64 gradient[3] = {};
};
inline FormationCoordinate formation_coordinate(const FormationFrame& f, f64 x, f64 y, f64 z) {
  const f64 d[3] = {x - f.origin[0], y - f.origin[1], z - f.origin[2]};
  const f64 along = f.dip[0] * d[0] + f.dip[1] * d[1] + f.dip[2] * d[2];
  f64 fold = 0.0;
  f64 slope = 0.0;
  for (u32 k = 0; k < 2; ++k) {
    if (f.fold_len[k] <= 0.0) continue;
    const f64 w = 2.0 * std::numbers::pi / f.fold_len[k];
    fold += f.fold_amp[k] * det::sin(w * along);
    slope += f.fold_amp[k] * w * det::cos(w * along);
  }
  FormationCoordinate c;
  c.coordinate = f.normal[0] * d[0] + f.normal[1] * d[1] + f.normal[2] * d[2] - fold;
  for (u32 i = 0; i < 3; ++i)
    c.gradient[i] = f.normal[i] - slope * f.dip[i];
  return c;
}

// formation.slang's `formation_bed`: which bed of the sequence a coordinate is in (its index in the
// sequence, bottom first), the fraction across it, and its tint, roughness offset and resistance.
struct FormationBedSample {
  u32 index = 0;
  f64 fraction = 0.0;
  Vec4 value{1.0f, 1.0f, 1.0f, 0.0f};
  f32 resistance = 0.0f;
};
inline FormationBedSample formation_bed(const FormationSequence& s, f64 coordinate) {
  FormationBedSample out;
  if (s.count == 0) return out;
  const f64 period = static_cast<f64>(s.period_mm) / 1000.0;
  const f64 y = coordinate - period * std::floor(coordinate / period);
  u32 i = 0;
  while (i + 1 < s.count && y >= static_cast<f64>(s.beds[i + 1].start_m))
    ++i;
  const FormationBedGpu& b = s.beds[i];
  out.index = i;
  out.fraction = (y - static_cast<f64>(b.start_m)) / static_cast<f64>(b.thickness_m);
  out.fraction = out.fraction < 0.0 ? 0.0 : (out.fraction > 1.0 ? 1.0 : out.fraction);
  out.value = b.value;
  out.resistance = b.resistance;
  return out;
}

// The integral of the sequence's value from its base to `x` (any x, through whole periods): what
// `formation_filtered` differences.
inline void formation_integral(const FormationSequence& s, f64 x, f64 out[4]) {
  const f64 period = static_cast<f64>(s.period_mm) / 1000.0;
  const f64 k = std::floor(x / period);
  const f64 y = x - k * period;
  u32 i = 0;
  while (i + 1 < s.count && y >= static_cast<f64>(s.beds[i + 1].start_m))
    ++i;
  const FormationBedGpu& b = s.beds[i];
  const f64 in = y - static_cast<f64>(b.start_m);
  const f32 total[4] = {s.total.x, s.total.y, s.total.z, s.total.w};
  const f32 integral[4] = {b.integral.x, b.integral.y, b.integral.z, b.integral.w};
  const f32 value[4] = {b.value.x, b.value.y, b.value.z, b.value.w};
  for (u32 c = 0; c < 4; ++c)
    out[c] = k * static_cast<f64>(total[c]) + static_cast<f64>(integral[c]) +
             in * static_cast<f64>(value[c]);
}

// formation.slang's `formation_filtered`: the sequence's value averaged over `width` metres of
// coordinate about `coordinate` (a box), the point's own value below a micrometre. A width over a
// period tends to the sequence's mean, which is what a band far away should read as.
inline Vec4 formation_filtered(const FormationSequence& s, f64 coordinate, f64 width) {
  if (s.count == 0) return Vec4{1.0f, 1.0f, 1.0f, 0.0f};
  if (width <= 1e-6) return formation_bed(s, coordinate).value;
  f64 hi[4], lo[4];
  formation_integral(s, coordinate + 0.5 * width, hi);
  formation_integral(s, coordinate - 0.5 * width, lo);
  return Vec4{static_cast<f32>((hi[0] - lo[0]) / width), static_cast<f32>((hi[1] - lo[1]) / width),
              static_cast<f32>((hi[2] - lo[2]) / width), static_cast<f32>((hi[3] - lo[3]) / width)};
}

}  // namespace engine::gfx
