// **A world formation** (roadmap W85, W94; renderer.md, "Formation bands"; the grounding,
// docs/experiments/formation-field.md and formation-sequence.md). No bindings and no entry points,
// like brdf.slang and sky.slang: the terrain resolve includes it through terrain_material.slang,
// and the gfx test's probe (formation_probe.slang) holds it to its f64 twin in
// domain/gfx/formation.h.
//
// The bed coordinate is n . (p - origin) - fold(t . (p - origin)), plus the fault's throw beyond
// its plane. **No absolute position is formed** (ADR-0053): the integer part, the frame's cell less
// the origin in whole millimetres, is dotted with the directions as integers over 2^24 and reduced
// modulo the period in int64; only the remainder and the frame-local part become floats, so the
// precision is the same anywhere (formation.h has the argument).
//
// **The sequence is a table of up to 512 beds** (W94), drawn on the CPU from a seed or authored. A
// bed's base stands at `start_m + dot(base_amp_m, g)`, g the two lateral fields at the point, so a
// bed thickens, thins and pinches out along the strike; the table carries the prefix sums that keep
// the box filter two lookups with the tops moved (formation.h, "Variation along strike").
//
// **A lookup is one word of the slice index and a step or two** (T94; formation.h, "The slice
// index"). The period is cut into 1,024 equal slices, and each slice's word names the beds a
// coordinate in it can be in wherever the tops stand: the binary search runs over those, not over
// the table. W94's search over 512 beds was nine loads, each waiting on the one before, four times
// a pixel: with the loop it ran in, 0.21 of the 0.33 ms the formation cost the resolve at 1080p
// (renderer.md, "Formation bands", "What the formation costs the resolve").
#ifndef ENGINE_FORMATION_SLANG
#define ENGINE_FORMATION_SLANG

static const uint k_formation_max_beds = 512u;
static const uint k_formation_slices = 1024u;
// gfx::FormationParams::flags: the block holds a formation, and the formation was drawn from
// members (gfx::k_formation_drawn: the macro table is the members, tops and all).
static const uint k_formation_valid = 1u;
static const uint k_formation_drawn = 2u;
// Where the two sequences sit in the block (gfx::FormationParams): a 192-byte head, then two
// tables of 64 + 512 * 96 + 1024 * 4 bytes.
static const uint64_t k_formation_beds_offset = 192;
static const uint64_t k_formation_macro_offset = 53504;
static const float k_formation_q = 1.0 / 16777216.0;  // 2^-24
static const float k_formation_tau = 6.28318530717958647;

// gfx::FormationBedGpu, 96 bytes.
struct FormationBed {
  float4 value;     // rgb tint, roughness offset
  float4 integral;  // the sequence's value less its mean, integrated from its base to this bed's
  float4 lateral0;  // each lateral field's correction through this bed's base
  float4 lateral1;
  float start_m;
  float thickness_m;
  float resistance;
  uint member;
  float2 base_amp_m;  // how far this bed's base moves with each lateral field
  uint kind;
  uint pad;
};

// gfx::FormationSequence, 64 + 512 * 96 + 1024 * 4 bytes. Read through a pointer, never copied.
struct FormationSequence {
  uint count;
  uint period_mm;
  float period_m;
  float slices_per_m;  // k_formation_slices / period_m (0 with no beds)
  float4 mean;
  float4 lateral_total0;
  float4 lateral_total1;
  FormationBed beds[k_formation_max_beds];
  // The slice index: for each slice of the period, the first bed a coordinate in it can be in (low
  // 16 bits) and how many beds it can be in (high 16), wherever the tops stand.
  uint slices[k_formation_slices];
};

// gfx::FormationParams, 106,816 bytes: a 192-byte head and the two sequences (the offsets above).
struct FormationParams {
  int64_t origin_x_mm;
  int64_t origin_y_mm;
  int64_t origin_z_mm;
  int64_t normal_x_q;
  int64_t normal_y_q;
  int64_t normal_z_q;
  int64_t dip_x_q;
  int64_t dip_y_q;
  int64_t dip_z_q;
  uint fold0_mm;
  uint fold1_mm;
  float fold0_amp_m;
  float fold1_amp_m;
  float soft_m;
  uint flags;
  // gfx::FormationLateralGpu, twice.
  int64_t lateral0_x_q;
  int64_t lateral0_z_q;
  uint lateral0_cell_mm;
  uint lateral0_seed;
  float lateral0_gain;
  uint lateral0_pad;
  int64_t lateral1_x_q;
  int64_t lateral1_z_q;
  uint lateral1_cell_mm;
  uint lateral1_seed;
  float lateral1_gain;
  uint lateral1_pad;
  int64_t fault_x_q;
  int64_t fault_z_q;
  int64_t fault_offset_mm;
  int64_t fault_throw_mm;
  FormationSequence beds;   // at 192
  FormationSequence macro;  // at 53504
};

// The floor of a / (p * 2^24), its low 32 bits, and `r` = a - (p * 2^24) * floor(a / (p * 2^24)) in
// [0, p * 2^24), for p > 0, on magnitudes (terrain_material.slang's caution: no signed `%`).
// **In 32 bits wherever the numbers allow** (T94): the modulus is p whole millimetres shifted by 24
// bits, so the quotient and the remainder are those of a's whole millimetres by p with a's low 24
// bits carried through, and the whole millimetres fit 32 bits while the frame's cell is within
// 2,147 km of the formation's origin along the direction. The card has no 64-bit division of its
// own, and a banded pixel ran four of them on numbers that are the same for the whole frame; the
// 64-bit form stays for a frame further out, and gives the same integers.
uint formation_floor_div(int64_t a, uint p, out int64_t r) {
  const int64_t whole = a >> 24;  // floor(a / 2^24)
  const int64_t low = a & int64_t(0xFFFFFF);
  uint quotient;
  uint rest;
  if (whole > int64_t(-2147483647) && whole < int64_t(2147483647)) {
    const int w = int(whole);
    const uint magnitude = uint(w < 0 ? -w : w);
    const uint cells = magnitude / p;
    const uint left = magnitude - cells * p;
    const bool below = w < 0 && left != 0u;
    rest = below ? p - left : left;
    quotient = w < 0 ? (0u - cells) - (below ? 1u : 0u) : cells;
  } else {
    const uint64_t magnitude = uint64_t(whole < 0 ? -whole : whole);
    const uint64_t cells = magnitude / uint64_t(p);
    const uint left = uint(magnitude - cells * uint64_t(p));
    const bool below = whole < 0 && left != 0u;
    rest = below ? p - left : left;
    quotient = whole < 0 ? (0u - uint(cells)) - (below ? 1u : 0u) : uint(cells);
  }
  r = (int64_t(rest) << 24) | low;
  return quotient;
}

// The remainder of `s` (metres times 1000 * 2^24) modulo `period_mm`, in metres.
float formation_reduce(int64_t s, uint period_mm) {
  if (period_mm == 0u) return 0.0;
  int64_t r;
  formation_floor_div(s, period_mm, r);
  return float(r) * k_formation_q * 0.001;
}

// gfx::formation_hash, the lateral fields' lattice: the same integers on both sides.
uint formation_hash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float formation_lattice(uint cell, uint seed) {
  return float(formation_hash(formation_hash(cell) ^ seed) >> 8) * k_formation_q;
}

// One lateral field at a point (gfx::formation_lateral): value noise along a horizontal direction
// on a lattice of `cell_mm`, sharpened to rest at 0 and 1. `s` is the integer part's distance along
// the direction (millimetres times 2^24) and `local` the float rest, metres.
float formation_lateral_field(int64_t s, float local, uint cell_mm, uint seed, float gain) {
  if (cell_mm == 0u) return 0.0;
  // Which cell of the lattice the integer part is in (the quotient's low 32 bits: the hash's
  // argument) and how far into it, from one division (W94 took the remainder and then the
  // quotient, two).
  int64_t r;
  const uint cell0 = formation_floor_div(s, cell_mm, r);
  const float len = float(cell_mm) * 0.001;
  const float u = (float(r) * k_formation_q * 0.001 + local) / len;
  const float step = floor(u);
  const float t = u - step;
  const uint cell = cell0 + uint(int(step));
  const float a = formation_lattice(cell, seed);
  const float b = formation_lattice(cell + 1u, seed);
  const float n = lerp(a, b, t * t * (3.0 - 2.0 * t));
  return saturate((n - 0.5) * gain + 0.5);
}

// The folded coordinate at a point, reduced for each sequence: `bed` and `macro` agree with the
// twin's coordinate modulo their periods, `gradient` is the coordinate's per metre, and `lateral`
// the two lateral fields there (W94).
struct FormationPoint {
  float bed;
  float macro;
  float3 gradient;
  float2 lateral;
};

// `cell` is the frame's eye cell and `local` the point from that cell's corner in metres (the
// eye's local and residual plus the frame-local point).
FormationPoint formation_coordinate(FormationParams* f, int3 cell, float3 local) {
  const int64_t dx = int64_t(cell.x) * int64_t(64000) - f->origin_x_mm;
  const int64_t dy = int64_t(cell.y) * int64_t(64000) - f->origin_y_mm;
  const int64_t dz = int64_t(cell.z) * int64_t(64000) - f->origin_z_mm;
  int64_t sn = dx * f->normal_x_q + dy * f->normal_y_q + dz * f->normal_z_q;
  const int64_t st = dx * f->dip_x_q + dy * f->dip_y_q + dz * f->dip_z_q;
  const float3 n = float3(float(f->normal_x_q), float(f->normal_y_q), float(f->normal_z_q)) *
                   k_formation_q;
  const float3 t = float3(float(f->dip_x_q), float(f->dip_y_q), float(f->dip_z_q)) * k_formation_q;
  const float ln = dot(n, local);
  const float lt = dot(t, local);
  float fold = 0.0;
  float slope = 0.0;
  if (f->fold0_mm != 0u) {
    const float len = float(f->fold0_mm) * 0.001;
    const float w = k_formation_tau / len;
    const float u = formation_reduce(st, f->fold0_mm) + lt;
    fold += f->fold0_amp_m * sin(w * u);
    slope += f->fold0_amp_m * w * cos(w * u);
  }
  if (f->fold1_mm != 0u) {
    const float len = float(f->fold1_mm) * 0.001;
    const float w = k_formation_tau / len;
    const float u = formation_reduce(st, f->fold1_mm) + lt;
    fold += f->fold1_amp_m * sin(w * u);
    slope += f->fold1_amp_m * w * cos(w * u);
  }
  // The fault: beyond its plane the coordinate is the throw greater, in whole millimetres, added
  // before the reduction. Which side is the sign of a sum that is small exactly where it matters.
  if (f->fault_throw_mm != int64_t(0)) {
    const int64_t sf = dx * f->fault_x_q + dz * f->fault_z_q - f->fault_offset_mm * int64_t(16777216);
    const float side = float(sf) * k_formation_q * 0.001 +
                       (float(f->fault_x_q) * local.x + float(f->fault_z_q) * local.z) * k_formation_q;
    if (side > 0.0) sn += f->fault_throw_mm * int64_t(16777216);
  }
  FormationPoint p;
  p.bed = formation_reduce(sn, f->beds.period_mm) + ln - fold;
  // A drawn formation's members are its macro sequence, so the two periods are one and so is the
  // remainder (T94: an int64 division a pixel that gave the number the line above just had).
  p.macro = p.bed;
  if (f->macro.period_mm != f->beds.period_mm)
    p.macro = formation_reduce(sn, f->macro.period_mm) + ln - fold;
  p.gradient = n - slope * t;
  p.lateral = float2(0.0, 0.0);
  if (f->lateral0_cell_mm != 0u) {
    const float2 d0 = float2(float(f->lateral0_x_q), float(f->lateral0_z_q)) * k_formation_q;
    const float2 d1 = float2(float(f->lateral1_x_q), float(f->lateral1_z_q)) * k_formation_q;
    p.lateral.x = formation_lateral_field(dx * f->lateral0_x_q + dz * f->lateral0_z_q,
                                          dot(d0, local.xz), f->lateral0_cell_mm, f->lateral0_seed,
                                          f->lateral0_gain);
    p.lateral.y = formation_lateral_field(dx * f->lateral1_x_q + dz * f->lateral1_z_q,
                                          dot(d1, local.xz), f->lateral1_cell_mm, f->lateral1_seed,
                                          f->lateral1_gain);
  }
  return p;
}

// Which bed of the sequence a coordinate is in, the fraction across it, and what it carries.
struct FormationBedSample {
  uint index;
  float fraction;
  float4 value;
  float resistance;
};

// Where bed i's base stands at a point's lateral fields (gfx::formation_base).
float formation_base(FormationSequence* s, uint i, float2 g) {
  return s->beds[i].start_m + dot(s->beds[i].base_amp_m, g);
}

// One halving of the run [lo, hi), or nothing once it is down to one bed.
void formation_halve(FormationSequence* s, float y, float2 g, inout uint lo, inout uint hi) {
  if (hi - lo > 1u) {
    const uint mid = (lo + hi) >> 1;
    if (y >= formation_base(s, mid, g))
      lo = mid;
    else
      hi = mid;
  }
}

// The bed whose base is the last at or under y, with the tops moved by the lateral fields
// (gfx::formation_find, step for step): y's slice of the period names the beds it can be in, and a
// binary search runs over those — none or one step on a drawn section where the whole table took
// nine. **The halvings are written out, nine for the 512 beds a run can hold, and not a loop**
// (T94): with a loop here the resolve cost every terrain pixel of every scene 0.08 ms at 1080p on
// an RTX 5090 — a scene with no formation, where none of this runs, included — and with the same
// steps written out it cost none (renderer.md, "Formation bands", "What a loop in the lookup
// cost"). A step past the run's end is one compare.
uint formation_find(FormationSequence* s, float y, float2 g) {
  const uint slice = min(uint(max(y, 0.0) * s->slices_per_m), k_formation_slices - 1u);
  const uint word = s->slices[slice];
  uint lo = word & 0xFFFFu;
  uint hi = lo + (word >> 16);
  if (word == 0u) hi = s->count;  // a table with no index: the whole of it
  formation_halve(s, y, g, lo, hi);
  formation_halve(s, y, g, lo, hi);
  formation_halve(s, y, g, lo, hi);
  formation_halve(s, y, g, lo, hi);
  formation_halve(s, y, g, lo, hi);
  formation_halve(s, y, g, lo, hi);
  formation_halve(s, y, g, lo, hi);
  formation_halve(s, y, g, lo, hi);
  formation_halve(s, y, g, lo, hi);
  return lo;
}

FormationBedSample formation_bed(FormationSequence* s, float coordinate, float2 g) {
  FormationBedSample out;
  out.index = 0u;
  out.fraction = 0.0;
  out.value = float4(1.0, 1.0, 1.0, 0.0);
  out.resistance = 0.0;
  if (s->count == 0u) return out;
  const float period = s->period_m;
  const float y = coordinate - period * floor(coordinate / period);
  const uint i = formation_find(s, y, g);
  const float base = formation_base(s, i, g);
  float top = period;
  if (i + 1u < s->count) top = formation_base(s, i + 1u, g);
  out.index = i;
  out.fraction = top - base > 1e-9 ? saturate((y - base) / (top - base)) : 0.0;
  out.value = s->beds[i].value;
  out.resistance = s->beds[i].resistance;
  return out;
}

// The integral of the sequence's value less its mean, from the period's base to the coordinate
// `k` whole periods and `y` up, which is in bed `i`, with the tops moved (gfx::formation_deviation).
float4 formation_deviation(FormationSequence* s, float k, float y, uint i, float2 g) {
  return k * (g.x * s->lateral_total0 + g.y * s->lateral_total1) + s->beds[i].integral +
         g.x * s->beds[i].lateral0 + g.y * s->beds[i].lateral1 +
         (y - s->beds[i].start_m) * (s->beds[i].value - s->mean);
}

// The sequence's value averaged over `width` metres of coordinate (a box), so an edge is as wide
// as the pixel's footprint across the beds and a sequence finer than a pixel reads as its mean.
// **A box inside one bed is that bed's value** (T94), which is what the integral's difference
// gives to rounding: the lower end is looked up, and when the upper end is under the next bed's
// base in the same period nothing more is read — most pixels of a near face, whose footprint is
// centimetres across beds of decimetres and more. A point (a width under a micrometre, the path
// tracer's rays) is the same case. Otherwise the upper end is looked up too and the two integrals
// differenced, as W94 did for every pixel.
// `bed` is the bed the whole box is inside, or `k_formation_max_beds` when it spans a boundary.
float4 formation_filtered(FormationSequence* s, float coordinate, float width, float2 g,
                          out uint bed) {
  bed = k_formation_max_beds;
  if (s->count == 0u) return float4(1.0, 1.0, 1.0, 0.0);
  const float period = s->period_m;
  const float half = width > 1e-6 ? 0.5 * width : 0.0;
  const float kl = floor((coordinate - half) / period);
  const float kh = floor((coordinate + half) / period);
  const float yl = (coordinate - half) - kl * period;
  const float yh = (coordinate + half) - kh * period;
  const uint il = formation_find(s, yl, g);
  bool inside = kl == kh;
  if (inside && il + 1u < s->count) inside = yh < formation_base(s, il + 1u, g);
  if (inside) {
    bed = il;
    return s->beds[il].value;
  }
  const uint ih = formation_find(s, yh, g);
  return s->mean + (formation_deviation(s, kh, yh, ih, g) -
                    formation_deviation(s, kl, yl, il, g)) / width;
}
float4 formation_filtered(FormationSequence* s, float coordinate, float width, float2 g) {
  uint bed;
  return formation_filtered(s, coordinate, width, g, bed);
}

// The band at a point: both sequences' tints multiplied, their roughness offsets added, each
// box-filtered over the wider of the field's `soft_m` and **two** pixels' footprint across the beds
// (the coordinate's change along the pixel's two edges; zero for the path tracer's rays). Two,
// because a box nulls a period only as wide as itself: a one-pixel box left a sequence of a
// two-pixel period alternating row by row on a far hill (the v31 capture, 2026-11-05), and a
// two-pixel box nulls everything past Nyquist that a one-pixel one let through.
float4 formation_band(uint64_t address, FormationPoint p, float3 dp_dx, float3 dp_dy) {
  FormationParams* f = (FormationParams*)address;
  const float across = max(abs(dot(p.gradient, dp_dx)), abs(dot(p.gradient, dp_dy)));
  const float width = max(f->soft_m, 2.0 * across);
  FormationSequence* beds = (FormationSequence*)(address + k_formation_beds_offset);
  FormationSequence* members = (FormationSequence*)(address + k_formation_macro_offset);
  uint inside;
  const float4 bed = formation_filtered(beds, p.bed, width, p.lateral, inside);
  // **A drawn formation's members are its macro sequence** (T94; `k_formation_drawn`): a bed's
  // `member` is the macro table's index, and a member's base and top stand where its first and last
  // bed's do, to the float (both tables' starts are the same whole millimetres, and a top between
  // two members does not move in either). So a box inside one bed is inside that bed's member, and
  // the macro's value is that member's: the number the macro's own lookup returns, for one load.
  // An authored pair of tables says nothing of the kind and is looked up.
  float4 macro;
  if (inside != k_formation_max_beds && (f->flags & k_formation_drawn) != 0u)
    macro = members->beds[beds->beds[inside].member].value;
  else
    macro = formation_filtered(members, p.macro, width, p.lateral);
  return float4(bed.rgb * macro.rgb, bed.w + macro.w);
}

#endif
