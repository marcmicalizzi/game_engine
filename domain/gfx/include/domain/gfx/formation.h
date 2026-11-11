#pragma once

// **A world formation** (roadmap W85, W94; docs/subsystems/renderer.md, "Formation bands"; the
// grounding, docs/experiments/formation-field.md and formation-sequence.md): beds that dip, fold
// and repeat through the world, read by the terrain resolve as a band layer
// (`shaders/formation.slang`) and by the rock solvers as the bedding of a cliff (`domain/rocks`,
// rocks.md "Geology"). This header is the field's one CPU definition: the GPU block the shader
// reads (`FormationParams`, pinned by the size table and mirrored once, in formation.slang), the
// packing from the scene's `engine.scene.Formation` with its validation, the **generator** that
// draws a sequence from a seed and its members (W94), and the **double-precision twin** of the
// shader's functions, which the rocks solvers call and the gfx test holds the shader to
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
// carries the same error 10 km or 10,000 km from the origin as beside it (micrometres for a period
// of metres, a twentieth of a millimetre for one of a kilometre). The fold's phase is reduced the
// same way modulo its wavelength, the lateral fields' modulo their cell, and the fault's side is
// the sign of the same kind of sum. The twin below evaluates the same definition (the quantized
// directions, the millimetre origin, the millimetre thicknesses and wavelengths) in f64 without any
// reduction, which is what lets a test hold the two to 1e-3 m.
//
// **The sequence is a table, and the table is the definition** (W94). A sequence is up to
// `k_formation_max_beds` beds, authored or drawn from a seed by `generate_formation` — on the CPU,
// in integers and `det::` functions, once, at pack time — and both the shader and the twin read the
// same bytes, so which bed a coordinate is in is the same answer on both to the bit of the table.
// The lookup is one word of the table's slice index and a binary search over the few beds it names
// (T94, "The slice index" below; W94 searched the whole table, nine steps for 512 beds), and the
// box filter two lookups at most, whatever the width.
//
// **Variation along strike** (W94). A bed's top is `start + a0 * g0(p) + a1 * g1(p)`: two seeded
// lateral fields g in [0, 1] (value noise on a whole-millimetre lattice along a horizontal
// direction, sharpened so a top rests at either end of its travel for a stretch), each top with its
// own signed amplitudes in whole millimetres. A lens is a bed whose top's amplitude is its whole
// thickness downward. Tops never cross (`pack_formation_table` holds every amplitude to less than
// half of the thinner neighbour, and a lens's base still), the first and last top of the sequence
// do not move, so the period is the same everywhere, and the filter stays two lookups:
// each bed carries, per field, the prefix sum of `amplitude * (value below - value above)` over the
// tops under it, which is exactly what the integral of the varied sequence adds to the plain one.

#include <core/base/types.h>
#include <core/math/deterministic.h>
#include <core/math/math.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <schemas/scene.h>
#include <string>

namespace engine::gfx {

inline constexpr u32 k_formation_max_beds = 512;
inline constexpr u32 k_formation_max_members = 64;
inline constexpr u32 k_formation_q_bits = 24;
inline constexpr f64 k_formation_q = 1.0 / 16777216.0;  // 2^-24
// FormationParams::flags: the block holds a formation (an all-zero block is none).
inline constexpr u32 k_formation_valid = 1u;
// FormationParams::flags: the formation was drawn from members (T94), so **the macro table is the
// members**: a bed's `member` is the macro table's index, a member's base and top are its first
// and last bed's to the float (both tables' starts are the same whole millimetres), and a top
// between two members moves in neither table. The shader then reads a member's value through its
// bed where a pixel's box lies inside one bed, with no second lookup (formation.slang,
// `formation_band`). An authored pair of tables promises none of it and never carries the bit; a
// packer that moves the members' tops must move them alike in both tables to keep it.
inline constexpr u32 k_formation_drawn = 2u;
// FormationBedGpu::kind: what a bed is. Authored beds are 0; a drawn bed is one of its member's
// two lithologies, and a lens (authored or drawn) carries the bit as well.
inline constexpr u32 k_formation_bed_resistant = 1u;
inline constexpr u32 k_formation_bed_recessive = 2u;
inline constexpr u32 k_formation_bed_lens = 4u;
// A member (a macro bed) at or over this resistance is a cliff-former (rocks' exposure rule and
// cliffs generator draw their line at the same half).
inline constexpr f32 k_formation_cliff_resistance = 0.5f;
// The lateral fields: the second's cell is this many of the first's (the golden ratio squared, so
// the two never fall in step), their directions this far either side of the strike, and their
// sharpening (`saturate((noise - 1/2) * gain + 1/2)`).
inline constexpr f64 k_formation_lateral_ratio = 2.618033988749895;
inline constexpr f64 k_formation_lateral_turn_deg[2] = {35.0, -50.0};
inline constexpr f32 k_formation_lateral_gain = 2.5f;
// The most a top moves, as a share of the thinner of the beds it parts: two tops at this share
// leave a bed a tenth of its thickness.
inline constexpr f32 k_formation_max_variation = 0.45f;

// One bed on the GPU, 96 bytes: its value (rgb tint, roughness offset); the integral of the
// sequence's value **less the sequence's mean** from the sequence's base to this bed's base (what
// the box filter differences: a bounded walk about zero where the plain integral grew with the
// section, so a kilometre of beds filters as exactly as five metres did); the same prefix for each
// lateral field's correction; where it starts and how thick it is in metres (both whole
// millimetres, the reference section's); its resistance; the member it is in; how far its **base**
// moves with each lateral field, metres (whole millimetres, signed); and its kind.
struct FormationBedGpu {
  Vec4 value{1.0f, 1.0f, 1.0f, 0.0f};
  Vec4 integral{};
  Vec4 lateral0{};
  Vec4 lateral1{};
  f32 start_m = 0.0f;
  f32 thickness_m = 0.0f;
  f32 resistance = 0.0f;
  u32 member = 0;
  f32 base_amp_m[2] = {};
  u32 kind = 0;
  u32 pad = 0;
};
static_assert(sizeof(FormationBedGpu) == 96);

// **The slice index** (T94). A lookup's question is "which bed's base is the last at or under y",
// and W94 answered it by a binary search over the whole table: nine loads for 512 beds, each
// waiting on the one before, four times a banded pixel (two ends of the box, two sequences) — with
// the loop it ran in, 0.21 of the 0.33 ms the formation cost the resolve at 1080p (renderer.md,
// "Formation bands", "What the formation costs the resolve"). The
// period is cut into `k_formation_slices` equal slices, and each slice carries one word: the first
// bed a coordinate in the slice can be in and how many beds it can be in, **for every value of the
// lateral fields** (a bed's base ranges over `start + [min(0, a0) + min(0, a1), max(0, a0) +
// max(0, a1)]`), with a margin for the float that picks the slice. The search then runs over that
// run of beds — one or two steps on a drawn section — and finds the bed the search over the whole
// table finds, because the run holds every bed whose base can be at or under the coordinate and
// starts at one whose base always is. The table is still the definition: the index is derived from
// it at pack time (`pack_formation_index`), by the shader's twin, and says nothing the beds do not.
inline constexpr u32 k_formation_slices = 1024;

struct FormationSequence {
  u32 count = 0;      // 0: no sequence, a tint of one everywhere
  u32 period_mm = 0;  // the sum of the beds' whole-millimetre thicknesses
  f32 period_m = 0.0f;
  f32 slices_per_m = 0.0f;  // k_formation_slices / period_m: a coordinate's slice is y times this
  Vec4 mean{};              // the value's mean over one period of the reference section
  Vec4 lateral_total0{};    // each lateral field's correction over one whole period
  Vec4 lateral_total1{};
  FormationBedGpu beds[k_formation_max_beds] = {};
  // Per slice: the first candidate bed (low 16 bits) and the candidates' count (high 16, 1 or
  // more).
  u32 slices[k_formation_slices] = {};
};
static_assert(sizeof(FormationSequence) == 64 + 96 * k_formation_max_beds + 4 * k_formation_slices);

// One lateral field: a horizontal direction (x, z) as integers over 2^24, the lattice's cell in
// whole millimetres (0: the field is off, g = 0), its seed and its sharpening.
struct FormationLateralGpu {
  i64 dir_q[2] = {};
  u32 cell_mm = 0;
  u32 seed = 0;
  f32 gain = 0.0f;
  u32 pad = 0;
};
static_assert(sizeof(FormationLateralGpu) == 32);

// **The field's GPU block**, one per scene, named by `TerrainRecord::formation` (a device
// address; zero is no formation), mirrored in formation.slang. 106,816 bytes: a 192-byte head and
// two tables of 512 beds, each with its slice index (T94; 98,624 without the two indexes).
struct FormationParams {
  i64 origin_mm[3] = {};  // the origin, whole millimetres
  i64 normal_q[3] = {};   // the beds' unit normal times 2^24, rounded: the normal is this over 2^24
  i64 dip_q[3] = {};      // the horizontal dip direction the same way
  u32 fold_mm[2] = {};    // the folds' wavelengths, whole millimetres (0: that fold is off)
  f32 fold_amp_m[2] = {};
  f32 soft_m = 0.0f;
  u32 flags = 0;  // k_formation_valid, k_formation_drawn
  FormationLateralGpu lateral[2] = {};
  i64 fault_q[2] = {};      // the fault plane's horizontal unit normal (x, z) times 2^24
  i64 fault_offset_mm = 0;  // where the plane stands along it from the origin
  i64 fault_throw_mm = 0;   // added to the coordinate beyond the plane; 0: no fault
  FormationSequence beds;
  FormationSequence macro;
};
static_assert(offsetof(FormationParams, beds) == 192);
static_assert(sizeof(FormationParams) == 192 + 2 * sizeof(FormationSequence));
static_assert(sizeof(FormationParams) % 16 == 0);

// **The block's head alone** (T94): the 192 bytes before the tables, field for field — the origin,
// the directions, the folds, the lateral fields and the fault, which is everything the coordinate
// and the lateral fields read and nothing a bed lookup does. A caller that wants the formation's
// frame (`pack_formation_frame`, below) packs this and no table: 192 bytes where the whole block
// is 106,816 on its stack. `pack_formation` writes the block's head through the same function
// (`pack_formation_head`) and copies it in, so the two cannot differ.
struct FormationHead {
  i64 origin_mm[3] = {};
  i64 normal_q[3] = {};
  i64 dip_q[3] = {};
  u32 fold_mm[2] = {};
  f32 fold_amp_m[2] = {};
  f32 soft_m = 0.0f;
  u32 flags = 0;
  FormationLateralGpu lateral[2] = {};
  i64 fault_q[2] = {};
  i64 fault_offset_mm = 0;
  i64 fault_throw_mm = 0;
};
static_assert(sizeof(FormationHead) == offsetof(FormationParams, beds));
static_assert(offsetof(FormationHead, normal_q) == offsetof(FormationParams, normal_q));
static_assert(offsetof(FormationHead, dip_q) == offsetof(FormationParams, dip_q));
static_assert(offsetof(FormationHead, fold_mm) == offsetof(FormationParams, fold_mm));
static_assert(offsetof(FormationHead, fold_amp_m) == offsetof(FormationParams, fold_amp_m));
static_assert(offsetof(FormationHead, soft_m) == offsetof(FormationParams, soft_m));
static_assert(offsetof(FormationHead, flags) == offsetof(FormationParams, flags));
static_assert(offsetof(FormationHead, lateral) == offsetof(FormationParams, lateral));
static_assert(offsetof(FormationHead, fault_q) == offsetof(FormationParams, fault_q));
static_assert(offsetof(FormationHead, fault_offset_mm) ==
              offsetof(FormationParams, fault_offset_mm));
static_assert(offsetof(FormationHead, fault_throw_mm) == offsetof(FormationParams, fault_throw_mm));
// The head of a packed block, copied out.
inline FormationHead formation_head(const FormationParams& p) {
  FormationHead h;
  std::memcpy(&h, &p, sizeof(h));
  return h;
}

// ---- The generator's integers -------------------------------------------------------------------

// splitmix64's finalizer, and a key of a seed and two ordinals.
inline u64 formation_mix(u64 x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
inline u64 formation_key(u64 seed, u64 a, u64 b) noexcept {
  return formation_mix(formation_mix(seed ^ (a * 0xD6E8FEB86659FD93ull)) ^ b);
}
// A hash word as a number in [0, 1), 53 bits.
inline f64 formation_unit(u64 h) noexcept {
  return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0);
}
// A standard normal deviate, nearly: six 16-bit uniforms summed (Irwin-Hall), centred and scaled to
// unit variance, so it is within +-4.25 and every step is exact but the one scaling.
inline f64 formation_normal(u64 key) noexcept {
  const u64 a = formation_mix(key);
  const u64 b = formation_mix(a);
  const u64 sum = (a & 0xFFFFu) + ((a >> 16) & 0xFFFFu) + ((a >> 32) & 0xFFFFu) + (a >> 48) +
                  (b & 0xFFFFu) + ((b >> 16) & 0xFFFFu);
  return (static_cast<f64>(sum) / 65536.0 - 3.0) * 1.4142135623730951;
}
// The lateral fields' lattice hash, the one the shader computes too (formation.slang's
// `formation_hash`): 32-bit, so both sides hold the same integers.
inline u32 formation_hash(u32 x) noexcept {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
inline f64 formation_lattice(u32 cell, u32 seed) noexcept {
  return static_cast<f64>(formation_hash(formation_hash(cell) ^ seed) >> 8) * k_formation_q;
}

// ---- Validation ---------------------------------------------------------------------------------

// The schema's ranges (scene.schema `Formation`, `FormationBed`, `FormationMember`), with a
// sentence naming the first field out of them.
inline bool validate_formation(const scene::Formation& f, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = "formation: " + why;
    return false;
  };
  const auto finite = [](f64 v) { return std::isfinite(v); };
  const auto tint_ok = [](const Vec3& t) {
    for (const f32 c : {t.x, t.y, t.z})
      if (!(c >= 0.0f && c <= 2.0f)) return false;
    return true;
  };
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
  if (!(f.lateral_m >= 10.0f && f.lateral_m <= 100000.0f))
    return fail("lateral_m must be in [10, 100000] m");
  if (!(f.fault_throw_m >= -1000.0f && f.fault_throw_m <= 1000.0f))
    return fail("fault_throw_m must be in [-1000, 1000] m");
  if (!(f.fault_azimuth_deg >= -180.0f && f.fault_azimuth_deg <= 180.0f))
    return fail("fault_azimuth_deg must be in [-180, 180]");
  if (!finite(f.fault_offset_m) || std::fabs(f.fault_offset_m) > 1e8)
    return fail("fault_offset_m must be finite and within 100,000 km");
  if (f.beds.empty() && f.members.empty())
    return fail("beds must hold at least one bed, or members a member");
  if (!f.members.empty() && (!f.beds.empty() || !f.macro_beds.empty()))
    return fail("members draw the beds: give members, or beds and macro_beds, not both");
  if (f.beds.size() > k_formation_max_beds || f.macro_beds.size() > k_formation_max_beds)
    return fail("a sequence holds at most 512 beds");
  if (f.members.size() > k_formation_max_members) return fail("members holds at most 64 members");
  for (u32 s = 0; s < 2; ++s) {
    const Vector<scene::FormationBed>& beds = s == 0 ? f.beds : f.macro_beds;
    const u32 count = static_cast<u32>(beds.size());
    for (u32 i = 0; i < count; ++i) {
      const scene::FormationBed& b = beds[i];
      const std::string who = (s == 0 ? "beds " : "macro_beds ") + b.name;
      if (!(b.thickness_m >= 0.01f && b.thickness_m <= 1000.0f))
        return fail(who + ": thickness_m must be in [0.01, 1000] m");
      if (!tint_ok(b.tint.value_or(Vec3{1.0f, 1.0f, 1.0f})))
        return fail(who + ": tint must be in [0, 2]");
      if (!(b.roughness >= -1.0f && b.roughness <= 1.0f))
        return fail(who + ": roughness must be in [-1, 1]");
      if (!(b.resistance >= 0.0f && b.resistance <= 1.0f))
        return fail(who + ": resistance must be in [0, 1]");
      if (!(b.variation >= 0.0f && b.variation <= k_formation_max_variation))
        return fail(who + ": variation must be in [0, 0.45]");
      if (b.lens && (i + 1 == count || (i > 0 && beds[i - 1].lens)))
        return fail(who + ": a lens is not the sequence's last bed, nor two beds running");
    }
  }
  for (const scene::FormationMember& m : f.members) {
    const std::string who = "members " + m.name;
    if (!(m.thickness_m >= 0.5f && m.thickness_m <= 1000.0f))
      return fail(who + ": thickness_m must be in [0.5, 1000] m");
    if (!(m.resistant >= 0.0f && m.resistant <= 1.0f))
      return fail(who + ": resistant must be in [0, 1]");
    if (!(m.bed_m >= 0.05f && m.bed_m <= 100.0f))
      return fail(who + ": bed_m must be in [0.05, 100] m");
    if (!(m.sigma >= 0.0f && m.sigma <= 1.5f)) return fail(who + ": sigma must be in [0, 1.5]");
    if (!tint_ok(m.tint.value_or(Vec3{1.0f, 1.0f, 1.0f})) ||
        !tint_ok(m.resistant_tint.value_or(Vec3{1.0f, 1.0f, 1.0f})) ||
        !tint_ok(m.recessive_tint.value_or(Vec3{1.0f, 1.0f, 1.0f})))
      return fail(who + ": tint must be in [0, 2]");
    if (!(m.tone >= 0.0f && m.tone <= 0.5f)) return fail(who + ": tone must be in [0, 0.5]");
    if (!(m.variation >= 0.0f && m.variation <= k_formation_max_variation))
      return fail(who + ": variation must be in [0, 0.45]");
    if (!(m.lenses >= 0.0f && m.lenses <= 1.0f)) return fail(who + ": lenses must be in [0, 1]");
  }
  return true;
}

// ---- The sequence: a plan of beds, drawn or authored, and its table -----------------------------

// One bed as the packer takes it, whichever way it came.
struct FormationBedPlan {
  u64 thickness_mm = 0;
  Vec4 value{1.0f, 1.0f, 1.0f, 0.0f};
  f32 resistance = 0.0f;
  u32 member = 0;
  u32 kind = 0;          // k_formation_bed_resistant / _recessive, 0 authored
  f32 variation = 0.0f;  // its top's travel, a share of the thinner of the beds it parts
  bool lens = false;
};

// **A sequence drawn from a seed and its members** (W94; the grounding,
// docs/experiments/formation-sequence.md, "The generator"). Each member's two lithologies
// alternate — an embedded chain of two states, whose order is therefore fixed, with independent
// thicknesses (Krumbein and Dacey 1969) — and each bed's thickness is lognormal about its
// lithology's median: the dominant lithology's is `bed_m`, the other's `bed_m * minor / major`, so
// the shares come out as `resistant` says, and a minor lithology under a centimetre is left out.
// A member's last bed takes what is left of it, so the members' tops are where the description put
// them. Everything is whole millimetres, integer hashes and `det::exp`: the same beds on every
// compiler. False, with a sentence, when the members draw more beds than the table holds.
inline bool generate_formation(const scene::Formation& f, Vector<FormationBedPlan>& beds,
                               Vector<FormationBedPlan>& members, std::string* error) {
  beds.clear();
  members.clear();
  const u32 member_count = static_cast<u32>(f.members.size());
  for (u32 m = 0; m < member_count; ++m) {
    const scene::FormationMember& member = f.members[m];
    const u64 total_mm =
        static_cast<u64>(std::llround(static_cast<f64>(member.thickness_m) * 1000.0));
    const f64 share = static_cast<f64>(member.resistant);
    const bool resistant_major = share >= 0.5;
    const f64 major = resistant_major ? share : 1.0 - share;
    const f64 major_mm = static_cast<f64>(std::llround(static_cast<f64>(member.bed_m) * 1000.0));
    const f64 minor_mm = static_cast<f64>(std::llround(major_mm * (1.0 - major) / major));
    const bool alternates = minor_mm >= 10.0;
    const f64 sigma = static_cast<f64>(member.sigma);
    const Vec3 tints[2] = {member.resistant_tint.value_or(Vec3{1.08f, 1.05f, 1.0f}),
                           member.recessive_tint.value_or(Vec3{0.78f, 0.80f, 0.84f})};
    const u64 salt = formation_key(f.seed, 0x4D454D42u, m);  // the member's own stream
    // The first bed's lithology: either, when two alternate.
    bool resistant = alternates ? (formation_key(salt, 1, 0) & 1u) != 0 : resistant_major;
    u64 remaining = total_mm;
    const u32 first = static_cast<u32>(beds.size());
    for (u32 b = 0; remaining > 0; ++b) {
      if (beds.size() >= k_formation_max_beds) {
        if (error != nullptr)
          *error = "formation: members draw more than 512 beds (at members " + member.name +
                   "): thicken bed_m or thin the members";
        return false;
      }
      const bool is_major = resistant == resistant_major;
      const f64 median = is_major ? major_mm : minor_mm;
      const f64 z = formation_normal(formation_key(salt, 2, b));
      i64 drawn = std::llround(median * det::exp(sigma * z));
      if (drawn < 10) drawn = 10;  // a bed is a centimetre or more (McKee and Weir 1953)
      u64 thick = static_cast<u64>(drawn);
      if (thick + 10 > remaining) thick = remaining;  // the member's last bed takes what is left
      FormationBedPlan plan;
      plan.thickness_mm = thick;
      const f64 tone = 1.0 + static_cast<f64>(member.tone) *
                                 (2.0 * formation_unit(formation_key(salt, 3, b)) - 1.0);
      const Vec3& tint = tints[resistant ? 0 : 1];
      const auto toned = [tone](f32 c) {
        const f64 v = static_cast<f64>(c) * tone;
        return static_cast<f32>(v > 2.0 ? 2.0 : v);
      };
      plan.value = Vec4{toned(tint.x), toned(tint.y), toned(tint.z), resistant ? -0.04f : 0.08f};
      const f64 wobble = 0.1 * (2.0 * formation_unit(formation_key(salt, 4, b)) - 1.0);
      plan.resistance = static_cast<f32>((resistant ? 0.85 : 0.2) + wobble);
      plan.member = m;
      plan.kind = resistant ? k_formation_bed_resistant : k_formation_bed_recessive;
      plan.variation = static_cast<f32>(static_cast<f64>(member.variation) *
                                        formation_unit(formation_key(salt, 5, b)));
      plan.lens = alternates && !is_major &&
                  formation_unit(formation_key(salt, 6, b)) < static_cast<f64>(member.lenses);
      beds.push_back(plan);
      remaining -= thick;
      if (alternates) resistant = !resistant;
    }
    // A member's top does not move, so its last bed is no lens.
    if (beds.size() > first) beds[static_cast<u32>(beds.size()) - 1].lens = false;
    FormationBedPlan whole;
    whole.thickness_mm = total_mm;
    const Vec3 tint = member.tint.value_or(Vec3{1.0f, 1.0f, 1.0f});
    whole.value = Vec4{tint.x, tint.y, tint.z, 0.0f};
    whole.resistance = static_cast<f32>(0.2 + 0.65 * share);
    whole.member = m;
    members.push_back(whole);
  }
  return true;
}

// An authored sequence as a plan: the beds as they are written.
inline void plan_formation_beds(const Vector<scene::FormationBed>& beds,
                                Vector<FormationBedPlan>& out) {
  out.clear();
  for (const scene::FormationBed& b : beds) {
    FormationBedPlan plan;
    plan.thickness_mm = static_cast<u64>(std::llround(static_cast<f64>(b.thickness_m) * 1000.0));
    const Vec3 t = b.tint.value_or(Vec3{1.0f, 1.0f, 1.0f});
    plan.value = Vec4{t.x, t.y, t.z, b.roughness};
    plan.resistance = b.resistance;
    plan.variation = b.variation;
    plan.lens = b.lens;
    out.push_back(plan);
  }
}

inline void formation_clear(FormationSequence& s) {
  const FormationBedGpu blank;
  s.count = 0;
  s.period_mm = 0;
  s.period_m = 0.0f;
  s.slices_per_m = 0.0f;
  s.mean = Vec4{};
  s.lateral_total0 = Vec4{};
  s.lateral_total1 = Vec4{};
  for (u32 i = 0; i < k_formation_max_beds; ++i)
    s.beds[i] = blank;
  for (u32 j = 0; j < k_formation_slices; ++j)
    s.slices[j] = 0;
}

// **A table's slice index** (T94; "The slice index", above), from the table as the shader reads
// it: the f32 starts and amplitudes widened, so the bounds are of the numbers the search compares.
// Slice j is the coordinates [j h, (j + 1) h), h the period over `k_formation_slices`, widened by a
// margin either way — a millimetre and 2^-18 of the period, 32 times the float's step at the
// period's far end — because the shader picks the slice from a float product. Its word is the last
// bed whose base is at or under the slice's lower edge wherever the tops stand (bed 0 for want of
// one: its base is the period's start and never moves), and the count from there to the last bed
// whose base can be at or under the upper edge. The bounds rise with the bed (tops never cross)
// and the edges with the slice, so both are one walk through the table; a pinched lens's base can
// sit a float's step under its neighbour's, which the margin covers thirty times over.
inline void pack_formation_index(FormationSequence& s) {
  s.slices_per_m = 0.0f;
  for (u32 j = 0; j < k_formation_slices; ++j)
    s.slices[j] = 0;
  if (s.count == 0 || s.period_mm == 0) return;
  const f64 period = static_cast<f64>(s.period_m);
  s.slices_per_m = static_cast<f32>(static_cast<f64>(k_formation_slices) / period);
  const f64 h = period / static_cast<f64>(k_formation_slices);
  const f64 margin = 0.001 + period / 262144.0;
  const auto low = [&s](u32 i) {
    const FormationBedGpu& b = s.beds[i];
    const f64 a0 = static_cast<f64>(b.base_amp_m[0]), a1 = static_cast<f64>(b.base_amp_m[1]);
    return static_cast<f64>(b.start_m) + (a0 < 0.0 ? a0 : 0.0) + (a1 < 0.0 ? a1 : 0.0);
  };
  const auto high = [&s](u32 i) {
    const FormationBedGpu& b = s.beds[i];
    const f64 a0 = static_cast<f64>(b.base_amp_m[0]), a1 = static_cast<f64>(b.base_amp_m[1]);
    return static_cast<f64>(b.start_m) + (a0 > 0.0 ? a0 : 0.0) + (a1 > 0.0 ? a1 : 0.0);
  };
  u32 first = 0, last = 0;
  for (u32 j = 0; j < k_formation_slices; ++j) {
    const f64 lower = static_cast<f64>(j) * h - margin;
    const f64 upper = static_cast<f64>(j + 1) * h + margin;
    while (first + 1 < s.count && high(first + 1) <= lower)
      ++first;
    while (last + 1 < s.count && low(last + 1) <= upper)
      ++last;
    if (last < first) last = first;
    s.slices[j] = first | ((last - first + 1) << 16);
  }
}

// **How far a top travels** (the top between bed i and bed i + 1 of a plan's first `count` beds):
// whole millimetres, signed, and which lateral field it rides. Zero is a top that stays: the
// sequence's last, a top between two members, a lens's base (the lens is nothing exactly where its
// own top has come down to it), and any top whose bed has no variation. The table's packer and the
// frame-only pack (`formation_plan_moves`) both ask here, so "does any top move" has one answer.
struct FormationTopTravel {
  i64 amplitude_mm = 0;
  u32 field = 0;
};
inline FormationTopTravel formation_top_travel(const Vector<FormationBedPlan>& plan, u32 count,
                                               u64 seed, u32 i) {
  FormationTopTravel out;
  if (i + 1 >= count) return out;
  const FormationBedPlan& b = plan[i];
  const FormationBedPlan& above = plan[i + 1];
  if (above.member != b.member || above.lens) return out;
  const u64 key = formation_key(seed, 0x544F50u, i);
  if (b.lens) {
    out.amplitude_mm = -static_cast<i64>(b.thickness_mm);
  } else if (b.variation > 0.0f) {
    const u64 thinner = b.thickness_mm < above.thickness_mm ? b.thickness_mm : above.thickness_mm;
    const f64 share = static_cast<f64>(
        b.variation > k_formation_max_variation ? k_formation_max_variation : b.variation);
    out.amplitude_mm = static_cast<i64>(share * static_cast<f64>(thinner));  // toward zero
    if ((key & 2u) != 0) out.amplitude_mm = -out.amplitude_mm;  // down, or up, by the hash
  }
  out.field = static_cast<u32>(key & 1u);
  return out;
}
// Whether any top of a plan moves, as `pack_formation_table` would pack it with this seed.
inline bool formation_plan_moves(const Vector<FormationBedPlan>& plan, u64 seed) {
  const u32 count =
      static_cast<u32>(plan.size() < k_formation_max_beds ? plan.size() : k_formation_max_beds);
  for (u32 i = 0; i + 1 < count; ++i)
    if (formation_top_travel(plan, count, seed, i).amplitude_mm != 0) return true;
  return false;
}

// **A plan into its table.** Starts and thicknesses in whole millimetres; the mean; the deviation
// integral; and the tops' travel. Top j (between bed j and j + 1) moves by `amplitude * g` of one
// lateral field, chosen by a hash: a lens's top by its whole thickness downward, any other by its
// `variation` of the thinner of its two beds, up or down by a hash. A top between two members, the
// sequence's last top and a lens's base stay where they are. So bed i's thickness anywhere is at
// least `t_i * (1 - 2 * variation)`, and a lens's is exactly zero where its field is one.
inline void pack_formation_table(const Vector<FormationBedPlan>& plan, u64 seed,
                                 FormationSequence& out) {
  formation_clear(out);
  const u32 count =
      static_cast<u32>(plan.size() < k_formation_max_beds ? plan.size() : k_formation_max_beds);
  out.count = count;
  if (count == 0) return;
  u64 period_mm = 0;
  f64 sum[4] = {};
  for (u32 i = 0; i < count; ++i) {
    const f64 m = static_cast<f64>(plan[i].thickness_mm) / 1000.0;
    const Vec4& v = plan[i].value;
    sum[0] += static_cast<f64>(v.x) * m;
    sum[1] += static_cast<f64>(v.y) * m;
    sum[2] += static_cast<f64>(v.z) * m;
    sum[3] += static_cast<f64>(v.w) * m;
    period_mm += plan[i].thickness_mm;
  }
  const f64 period = static_cast<f64>(period_mm) / 1000.0;
  f64 mean[4];
  for (u32 c = 0; c < 4; ++c)
    mean[c] = sum[c] / period;
  out.period_mm = static_cast<u32>(period_mm);
  out.period_m = static_cast<f32>(period);
  out.mean = Vec4{static_cast<f32>(mean[0]), static_cast<f32>(mean[1]), static_cast<f32>(mean[2]),
                  static_cast<f32>(mean[3])};
  const auto to_vec4 = [](const f64 a[4]) {
    return Vec4{static_cast<f32>(a[0]), static_cast<f32>(a[1]), static_cast<f32>(a[2]),
                static_cast<f32>(a[3])};
  };
  u64 start_mm = 0;
  f64 deviation[4] = {};
  f64 lateral[2][4] = {};
  for (u32 i = 0; i < count; ++i) {
    const FormationBedPlan& b = plan[i];
    FormationBedGpu& g = out.beds[i];
    g.value = b.value;
    g.integral = to_vec4(deviation);
    g.lateral0 = to_vec4(lateral[0]);
    g.lateral1 = to_vec4(lateral[1]);
    g.start_m = static_cast<f32>(static_cast<f64>(start_mm) / 1000.0);
    g.thickness_m = static_cast<f32>(static_cast<f64>(b.thickness_mm) / 1000.0);
    g.resistance = b.resistance;
    g.member = b.member;
    g.kind = b.kind | (b.lens && i + 1 < count ? k_formation_bed_lens : 0u);
    const f64 m = static_cast<f64>(b.thickness_mm) / 1000.0;
    const f64 value[4] = {static_cast<f64>(b.value.x), static_cast<f64>(b.value.y),
                          static_cast<f64>(b.value.z), static_cast<f64>(b.value.w)};
    for (u32 c = 0; c < 4; ++c)
      deviation[c] += (value[c] - mean[c]) * m;
    start_mm += b.thickness_mm;
    // This bed's top, which is the next bed's base.
    if (i + 1 >= count) break;
    const FormationBedPlan& above = plan[i + 1];
    const FormationTopTravel travel = formation_top_travel(plan, count, seed, i);
    if (travel.amplitude_mm == 0) continue;
    const f64 amplitude = static_cast<f64>(travel.amplitude_mm) / 1000.0;
    out.beds[i + 1].base_amp_m[travel.field] = static_cast<f32>(amplitude);
    const f64 next[4] = {static_cast<f64>(above.value.x), static_cast<f64>(above.value.y),
                         static_cast<f64>(above.value.z), static_cast<f64>(above.value.w)};
    for (u32 c = 0; c < 4; ++c)
      lateral[travel.field][c] += amplitude * (value[c] - next[c]);
  }
  out.lateral_total0 = to_vec4(lateral[0]);
  out.lateral_total1 = to_vec4(lateral[1]);
  pack_formation_index(out);
}

// W85's name for packing an authored sequence, kept for its callers: the beds as written, the
// tops' signs and fields from seed 1.
inline void pack_formation_sequence(const Vector<scene::FormationBed>& beds,
                                    FormationSequence& out) {
  Vector<FormationBedPlan> plan;
  plan_formation_beds(beds, plan);
  pack_formation_table(plan, 1, out);
}

// The seeds the two tables' tops take their signs and fields from.
inline u64 formation_table_seed(const scene::Formation& f, u32 table) {
  return formation_key(f.seed, 0x42454453u, table);
}

// **The scene's formation into the block's head and its two plans** (T94): the validation, the
// plans (drawn or authored), and every field of the 192-byte head. False, with the validation's
// sentence, when the formation is out of range or its members draw more beds than a table holds;
// `out` is then the empty head (flags 0). The whole pack and the frame-only pack are both this,
// so a frame read from either is the same to the bit.
inline bool pack_formation_head(const scene::Formation& f, FormationHead& out,
                                Vector<FormationBedPlan>& beds, Vector<FormationBedPlan>& members,
                                std::string* error) {
  // Field by field rather than `out = FormationHead{}`: GCC 13 at -O2 stopped with an internal
  // compiler error (gimplify.cc:774) on such a temporary of the whole block's nested
  // default-initialized arrays (W85), and the form is kept away from here too.
  for (u32 i = 0; i < 3; ++i)
    out.origin_mm[i] = out.normal_q[i] = out.dip_q[i] = 0;
  for (u32 k = 0; k < 2; ++k) {
    out.fold_mm[k] = 0;
    out.fold_amp_m[k] = 0.0f;
    out.lateral[k].dir_q[0] = out.lateral[k].dir_q[1] = 0;
    out.lateral[k].cell_mm = out.lateral[k].seed = out.lateral[k].pad = 0;
    out.lateral[k].gain = 0.0f;
    out.fault_q[k] = 0;
  }
  out.soft_m = 0.0f;
  out.flags = 0;
  out.fault_offset_mm = out.fault_throw_mm = 0;
  beds.clear();
  members.clear();
  if (!validate_formation(f, error)) return false;
  if (!f.members.empty()) {
    if (!generate_formation(f, beds, members, error)) return false;
  } else {
    plan_formation_beds(f.beds, beds);
    plan_formation_beds(f.macro_beds, members);
  }
  out.origin_mm[0] = std::llround(f.origin.x * 1000.0);
  out.origin_mm[1] = std::llround(f.origin.y * 1000.0);
  out.origin_mm[2] = std::llround(f.origin.z * 1000.0);
  // rocks.md's convention (detail::make_fabric): the dip direction t is (cos az, 0, sin az) and the
  // normal leans toward it by the dip, so the beds go down along t.
  const f64 radians = std::numbers::pi / 180.0;
  const f64 dip = static_cast<f64>(f.dip_deg) * radians;
  const f64 az = static_cast<f64>(f.azimuth_deg) * radians;
  const f64 t[3] = {det::cos(az), 0.0, det::sin(az)};
  const f64 n[3] = {t[0] * det::sin(dip), det::cos(dip), t[2] * det::sin(dip)};
  for (u32 i = 0; i < 3; ++i) {
    out.normal_q[i] = std::llround(n[i] * 16777216.0);
    out.dip_q[i] = std::llround(t[i] * 16777216.0);
  }
  out.fold_mm[0] =
      f.fold_amplitude_m > 0.0f
          ? static_cast<u32>(std::llround(static_cast<f64>(f.fold_wavelength_m) * 1000.0))
          : 0u;
  out.fold_mm[1] =
      f.fold2_amplitude_m > 0.0f
          ? static_cast<u32>(std::llround(static_cast<f64>(f.fold2_wavelength_m) * 1000.0))
          : 0u;
  out.fold_amp_m[0] = out.fold_mm[0] != 0 ? f.fold_amplitude_m : 0.0f;
  out.fold_amp_m[1] = out.fold_mm[1] != 0 ? f.fold2_amplitude_m : 0.0f;
  out.soft_m = f.soft_m;
  out.flags = k_formation_valid | (f.members.empty() ? 0u : k_formation_drawn);
  // The lateral fields, only when a top moves: the strike is the dip direction turned a quarter
  // (-sin az, 0, cos az), and each field runs along it turned a little further its own way.
  const bool moves = formation_plan_moves(beds, formation_table_seed(f, 0)) ||
                     formation_plan_moves(members, formation_table_seed(f, 1));
  if (moves) {
    const f64 cell_mm = static_cast<f64>(std::llround(static_cast<f64>(f.lateral_m) * 1000.0));
    for (u32 k = 0; k < 2; ++k) {
      const f64 along = az + (90.0 + k_formation_lateral_turn_deg[k]) * radians;
      out.lateral[k].dir_q[0] = std::llround(det::cos(along) * 16777216.0);
      out.lateral[k].dir_q[1] = std::llround(det::sin(along) * 16777216.0);
      out.lateral[k].cell_mm =
          static_cast<u32>(std::llround(k == 0 ? cell_mm : cell_mm * k_formation_lateral_ratio));
      out.lateral[k].seed = static_cast<u32>(formation_key(f.seed, 0x4C415445u, k));
      out.lateral[k].gain = k_formation_lateral_gain;
    }
  }
  if (f.fault_throw_m != 0.0f) {
    const f64 faz = static_cast<f64>(f.fault_azimuth_deg) * radians;
    out.fault_q[0] = std::llround(det::cos(faz) * 16777216.0);
    out.fault_q[1] = std::llround(det::sin(faz) * 16777216.0);
    out.fault_offset_mm = std::llround(f.fault_offset_m * 1000.0);
    out.fault_throw_mm = std::llround(static_cast<f64>(f.fault_throw_m) * 1000.0);
  }
  return true;
}

// The scene's formation into its GPU block: false, with the validation's sentence, when it is out
// of range or its members draw more beds than a table holds (the block is then empty: flags 0, no
// beds).
inline bool pack_formation(const scene::Formation& f, FormationParams& out, std::string* error) {
  FormationHead head;
  Vector<FormationBedPlan> beds, members;
  const bool ok = pack_formation_head(f, head, beds, members, error);
  std::memcpy(&out, &head, sizeof(head));
  formation_clear(out.beds);
  formation_clear(out.macro);
  if (!ok) return false;
  pack_formation_table(beds, formation_table_seed(f, 0), out.beds);
  pack_formation_table(members, formation_table_seed(f, 1), out.macro);
  return true;
}

// Whether a scene's formation packs — its ranges, and that its members draw no more beds than a
// table holds — with the sentence `pack_formation` would give, and no block (T94).
inline bool check_formation(const scene::Formation& f, std::string* error) {
  FormationHead head;
  Vector<FormationBedPlan> beds, members;
  return pack_formation_head(f, head, beds, members, error);
}

// ---- The twin, in f64 ---------------------------------------------------------------------------

// The field's geometry as the twin reads it: the definition the GPU block's head holds, widened.
struct FormationFrame {
  f64 origin[3] = {};  // metres, the whole-millimetre origin
  f64 normal[3] = {};  // normal_q / 2^24
  f64 dip[3] = {};     // dip_q / 2^24
  f64 fold_len[2] = {};
  f64 fold_amp[2] = {};
  // W94: the fault (throw 0: none) and the two lateral fields (cell 0: off).
  f64 fault[2] = {};
  f64 fault_offset = 0.0;
  f64 fault_throw = 0.0;
  f64 lateral_dir[2][2] = {};
  f64 lateral_cell[2] = {};
  f64 lateral_gain[2] = {};
  u32 lateral_seed[2] = {};
};
inline FormationFrame formation_frame(const FormationHead& p) {
  FormationFrame f;
  for (u32 i = 0; i < 3; ++i) {
    f.origin[i] = static_cast<f64>(p.origin_mm[i]) / 1000.0;
    f.normal[i] = static_cast<f64>(p.normal_q[i]) * k_formation_q;
    f.dip[i] = static_cast<f64>(p.dip_q[i]) * k_formation_q;
  }
  for (u32 k = 0; k < 2; ++k) {
    f.fold_len[k] = static_cast<f64>(p.fold_mm[k]) / 1000.0;
    f.fold_amp[k] = p.fold_mm[k] != 0 ? static_cast<f64>(p.fold_amp_m[k]) : 0.0;
    f.fault[k] = static_cast<f64>(p.fault_q[k]) * k_formation_q;
    f.lateral_dir[k][0] = static_cast<f64>(p.lateral[k].dir_q[0]) * k_formation_q;
    f.lateral_dir[k][1] = static_cast<f64>(p.lateral[k].dir_q[1]) * k_formation_q;
    f.lateral_cell[k] = static_cast<f64>(p.lateral[k].cell_mm) / 1000.0;
    f.lateral_gain[k] = static_cast<f64>(p.lateral[k].gain);
    f.lateral_seed[k] = p.lateral[k].seed;
  }
  f.fault_offset = static_cast<f64>(p.fault_offset_mm) / 1000.0;
  f.fault_throw = static_cast<f64>(p.fault_throw_mm) / 1000.0;
  return f;
}
inline FormationFrame formation_frame(const FormationParams& p) {
  return formation_frame(formation_head(p));
}

// **The frame-only pack** (T94): a scene's formation straight into the frame the twin's
// `formation_coordinate` and `formation_lateral` read, with no block. What a solver that asks only
// where the beds stand wants (rocks' `make_fabric`, a cave plan, a ledge's frame): the whole block
// is 106,816 bytes, and a caller that declared one to read its head put that on its stack. The same
// frame, to the bit, as `formation_frame` of the packed block (`formation_tests.cpp`, "the
// frame-only pack"). False with `pack_formation`'s sentence, and `out` untouched, when the
// formation does not pack.
inline bool pack_formation_frame(const scene::Formation& f, FormationFrame& out,
                                 std::string* error) {
  FormationHead head;
  Vector<FormationBedPlan> beds, members;
  if (!pack_formation_head(f, head, beds, members, error)) return false;
  out = formation_frame(head);
  return true;
}

// formation.slang's `formation_coordinate` without its reduction: the folded bed coordinate at a
// world point (metres), the fault's throw added beyond its plane, and its gradient, in f64.
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
  if (f.fault_throw != 0.0 && f.fault[0] * d[0] + f.fault[1] * d[2] - f.fault_offset > 0.0)
    c.coordinate += f.fault_throw;
  for (u32 i = 0; i < 3; ++i)
    c.gradient[i] = f.normal[i] - slope * f.dip[i];
  return c;
}

// **The lateral fields at a point** (W94): formation.slang's `formation_lateral`. Each is value
// noise along its own horizontal direction — the lattice's values from the integer hash the shader
// computes too, a cubic between two of them — sharpened to rest at 0 and at 1. All zero is the
// reference section: every top where the table's `start_m` puts it.
struct FormationLateral {
  f64 g[2] = {};
};
inline FormationLateral formation_lateral(const FormationFrame& f, f64 x, f64 y, f64 z) {
  (void)y;
  FormationLateral out;
  for (u32 k = 0; k < 2; ++k) {
    if (f.lateral_cell[k] <= 0.0) continue;
    const f64 u =
        (f.lateral_dir[k][0] * (x - f.origin[0]) + f.lateral_dir[k][1] * (z - f.origin[2])) /
        f.lateral_cell[k];
    const f64 whole = std::floor(u);
    const f64 t = u - whole;
    const u32 cell = static_cast<u32>(static_cast<u64>(static_cast<i64>(whole)));
    const f64 a = formation_lattice(cell, f.lateral_seed[k]);
    const f64 b = formation_lattice(cell + 1u, f.lateral_seed[k]);
    const f64 n = a + (b - a) * (t * t * (3.0 - 2.0 * t));
    const f64 g = (n - 0.5) * f.lateral_gain[k] + 0.5;
    out.g[k] = g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
  }
  return out;
}

// formation.slang's `formation_bed`: which bed of the sequence a coordinate is in (its index in the
// sequence, bottom first), the fraction across it, and its tint, roughness offset and resistance.
// W94 adds what the solvers ask of a bed: where its base and top stand **at this point** along the
// coordinate (not reduced: `base <= coordinate < top`), its member, and its kind.
struct FormationBedSample {
  u32 index = 0;
  f64 fraction = 0.0;
  Vec4 value{1.0f, 1.0f, 1.0f, 0.0f};
  f32 resistance = 0.0f;
  f64 base = 0.0;
  f64 top = 0.0;
  u32 member = 0;
  u32 kind = 0;
};

// Where bed i's base stands at a point's lateral fields, metres from the period's start.
inline f64 formation_base(const FormationSequence& s, u32 i, const FormationLateral& g) {
  const FormationBedGpu& b = s.beds[i];
  return static_cast<f64>(b.start_m) + static_cast<f64>(b.base_amp_m[0]) * g.g[0] +
         static_cast<f64>(b.base_amp_m[1]) * g.g[1];
}
// The bed whose base is the last at or under y: the shader's lookup, step for step — y's slice of
// the period, the run of beds its word names ("The slice index"), a binary search over the run.
// The same bed as a search over the whole table (`formation_tests.cpp`, "the slice index").
inline u32 formation_find(const FormationSequence& s, f64 y, const FormationLateral& g) {
  const f64 scaled = (y > 0.0 ? y : 0.0) * static_cast<f64>(s.slices_per_m);
  const u32 slice = scaled < static_cast<f64>(k_formation_slices - 1) ? static_cast<u32>(scaled)
                                                                      : k_formation_slices - 1;
  const u32 word = s.slices[slice];
  u32 lo = word & 0xFFFFu, hi = lo + (word >> 16);
  if (word == 0) hi = s.count;  // a sequence filled by hand, with no index: the whole table
  while (hi - lo > 1) {
    const u32 mid = (lo + hi) >> 1;
    if (y >= formation_base(s, mid, g))
      lo = mid;
    else
      hi = mid;
  }
  return lo;
}

inline FormationBedSample formation_bed(const FormationSequence& s, f64 coordinate,
                                        const FormationLateral& g) {
  FormationBedSample out;
  if (s.count == 0) return out;
  const f64 period = static_cast<f64>(s.period_mm) / 1000.0;
  const f64 y = coordinate - period * std::floor(coordinate / period);
  const u32 i = formation_find(s, y, g);
  const FormationBedGpu& b = s.beds[i];
  const f64 base = formation_base(s, i, g);
  const f64 top = i + 1 < s.count ? formation_base(s, i + 1, g) : period;
  const f64 thick = top - base;
  out.index = i;
  out.fraction = thick > 1e-9 ? (y - base) / thick : 0.0;
  out.fraction = out.fraction < 0.0 ? 0.0 : (out.fraction > 1.0 ? 1.0 : out.fraction);
  out.value = b.value;
  out.resistance = b.resistance;
  out.base = coordinate - (y - base);
  out.top = out.base + thick;
  out.member = b.member;
  out.kind = b.kind;
  return out;
}
// The reference section's bed: every top where the table puts it (W85's query, and what a caller
// without a point gets).
inline FormationBedSample formation_bed(const FormationSequence& s, f64 coordinate) {
  return formation_bed(s, coordinate, FormationLateral{});
}

// The integral of the sequence's value **less its mean** from the period's base to `x` (any x,
// through whole periods), at a point's lateral fields: what `formation_filtered` differences.
inline void formation_deviation(const FormationSequence& s, f64 x, const FormationLateral& g,
                                f64 out[4]) {
  const f64 period = static_cast<f64>(s.period_mm) / 1000.0;
  const f64 k = std::floor(x / period);
  const f64 y = x - k * period;
  const u32 i = formation_find(s, y, g);
  const FormationBedGpu& b = s.beds[i];
  const f64 in = y - static_cast<f64>(b.start_m);
  const f32 mean[4] = {s.mean.x, s.mean.y, s.mean.z, s.mean.w};
  const f32 integral[4] = {b.integral.x, b.integral.y, b.integral.z, b.integral.w};
  const f32 value[4] = {b.value.x, b.value.y, b.value.z, b.value.w};
  const f32 l0[4] = {b.lateral0.x, b.lateral0.y, b.lateral0.z, b.lateral0.w};
  const f32 l1[4] = {b.lateral1.x, b.lateral1.y, b.lateral1.z, b.lateral1.w};
  const f32 t0[4] = {s.lateral_total0.x, s.lateral_total0.y, s.lateral_total0.z,
                     s.lateral_total0.w};
  const f32 t1[4] = {s.lateral_total1.x, s.lateral_total1.y, s.lateral_total1.z,
                     s.lateral_total1.w};
  for (u32 c = 0; c < 4; ++c)
    out[c] = k * (g.g[0] * static_cast<f64>(t0[c]) + g.g[1] * static_cast<f64>(t1[c])) +
             static_cast<f64>(integral[c]) + g.g[0] * static_cast<f64>(l0[c]) +
             g.g[1] * static_cast<f64>(l1[c]) +
             in * (static_cast<f64>(value[c]) - static_cast<f64>(mean[c]));
}
// The integral of the reference section's value from its base to `x` (W85's query).
inline void formation_integral(const FormationSequence& s, f64 x, f64 out[4]) {
  formation_deviation(s, x, FormationLateral{}, out);
  const f32 mean[4] = {s.mean.x, s.mean.y, s.mean.z, s.mean.w};
  for (u32 c = 0; c < 4; ++c)
    out[c] += x * static_cast<f64>(mean[c]);
}

// formation.slang's `formation_filtered`: the sequence's value averaged over `width` metres of
// coordinate about `coordinate` (a box), the point's own value below a micrometre. A width over a
// period tends to the sequence's mean, which is what a band far away should read as. This is the
// definition: the two integrals' difference, always. The shader returns the bed's value outright
// where the box lies inside one bed of one period (T94) — the same number to rounding, and most
// pixels of a near face — and differences the integrals as here everywhere else.
inline Vec4 formation_filtered(const FormationSequence& s, f64 coordinate, f64 width,
                               const FormationLateral& g) {
  if (s.count == 0) return Vec4{1.0f, 1.0f, 1.0f, 0.0f};
  if (width <= 1e-6) return formation_bed(s, coordinate, g).value;
  f64 hi[4], lo[4];
  formation_deviation(s, coordinate + 0.5 * width, g, hi);
  formation_deviation(s, coordinate - 0.5 * width, g, lo);
  const f32 mean[4] = {s.mean.x, s.mean.y, s.mean.z, s.mean.w};
  f32 out[4];
  for (u32 c = 0; c < 4; ++c)
    out[c] = static_cast<f32>(static_cast<f64>(mean[c]) + (hi[c] - lo[c]) / width);
  return Vec4{out[0], out[1], out[2], out[3]};
}
inline Vec4 formation_filtered(const FormationSequence& s, f64 coordinate, f64 width) {
  return formation_filtered(s, coordinate, width, FormationLateral{});
}

// ---- What the solvers read (W94; rocks.md, "What a solver asks the formation") ------------------

// The formation at a world point, with the variation along strike and the fault applied: the
// coordinate and its gradient, the lateral fields there, the bed it is in and the member. A
// formation with no macro sequence is one member: index 0, the whole period, the beds' mean
// resistance weighted by thickness.
struct FormationSample {
  FormationCoordinate at;
  FormationLateral lateral;
  FormationBedSample bed;
  FormationBedSample member;
};
inline FormationBedSample formation_member(const FormationParams& p, f64 coordinate,
                                           const FormationLateral& g) {
  if (p.macro.count > 0) return formation_bed(p.macro, coordinate, g);
  FormationBedSample out;
  if (p.beds.count == 0) return out;
  const f64 period = static_cast<f64>(p.beds.period_mm) / 1000.0;
  f64 weighted = 0.0;
  for (u32 i = 0; i < p.beds.count; ++i)
    weighted +=
        static_cast<f64>(p.beds.beds[i].resistance) * static_cast<f64>(p.beds.beds[i].thickness_m);
  out.base = period * std::floor(coordinate / period);
  out.top = out.base + period;
  out.fraction = (coordinate - out.base) / period;
  out.resistance = static_cast<f32>(weighted / period);
  return out;
}
inline FormationSample formation_sample(const FormationParams& p, const FormationFrame& f, f64 x,
                                        f64 y, f64 z) {
  FormationSample out;
  out.at = formation_coordinate(f, x, y, z);
  out.lateral = formation_lateral(f, x, y, z);
  out.bed = formation_bed(p.beds, out.at.coordinate, out.lateral);
  out.member = formation_member(p, out.at.coordinate, out.lateral);
  return out;
}
inline FormationSample formation_sample(const FormationParams& p, f64 x, f64 y, f64 z) {
  return formation_sample(p, formation_frame(p), x, y, z);
}
// The thickness of section the formation does not repeat inside, metres: the longer of its two
// periods. (An authored pair of sequences repeats together only at their common multiple; a drawn
// one has one period, its members' whole thickness.)
inline f64 formation_section_m(const FormationParams& p) {
  const u32 mm = p.beds.period_mm > p.macro.period_mm ? p.beds.period_mm : p.macro.period_mm;
  return static_cast<f64>(mm) / 1000.0;
}

}  // namespace engine::gfx
