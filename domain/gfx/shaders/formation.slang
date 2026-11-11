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
// **The sequence is a table of up to 512 beds** (W94), drawn on the CPU from a seed or authored,
// and found by a binary search: nine steps, where W85's eight beds took seven of a linear one. A
// bed's base stands at `start_m + dot(base_amp_m, g)`, g the two lateral fields at the point, so a
// bed thickens, thins and pinches out along the strike; the table carries the prefix sums that keep
// the box filter two lookups with the tops moved (formation.h, "Variation along strike").
#ifndef ENGINE_FORMATION_SLANG
#define ENGINE_FORMATION_SLANG

static const uint k_formation_max_beds = 512u;
static const uint k_formation_valid = 1u;
// Where the two sequences sit in the block (gfx::FormationParams): a 192-byte head, then two
// tables of 64 + 512 * 96 bytes.
static const uint64_t k_formation_beds_offset = 192;
static const uint64_t k_formation_macro_offset = 49408;
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

// gfx::FormationSequence, 64 + 512 * 96 bytes. Read through a pointer, never copied.
struct FormationSequence {
  uint count;
  uint period_mm;
  float period_m;
  uint pad;
  float4 mean;
  float4 lateral_total0;
  float4 lateral_total1;
  FormationBed beds[k_formation_max_beds];
};

// gfx::FormationParams, 98,624 bytes: a 192-byte head and the two sequences (the offsets above).
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
  FormationSequence macro;  // at 49408
};

// a mod m in [0, m) for m > 0, on magnitudes (terrain_material.slang's caution: no signed `%`).
int64_t formation_mod(int64_t a, int64_t m) {
  const uint64_t magnitude = uint64_t(a < 0 ? -a : a);
  const int64_t r = int64_t(magnitude % uint64_t(m));
  return (a < 0 && r != 0) ? m - r : r;
}

// The remainder of `s` (metres times 1000 * 2^24) modulo `period_mm`, in metres.
float formation_reduce(int64_t s, uint period_mm) {
  if (period_mm == 0u) return 0.0;
  const int64_t m = int64_t(period_mm) << 24;
  return float(formation_mod(s, m)) * k_formation_q * 0.001;
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
  const int64_t m = int64_t(cell_mm) << 24;
  const int64_t r = formation_mod(s, m);
  // s - r is a whole number of cells: divide its magnitude.
  const int64_t whole = s - r;
  const uint64_t cells = uint64_t(whole < 0 ? -whole : whole) / uint64_t(m);
  const uint cell0 = whole < 0 ? uint(0u - uint(cells)) : uint(cells);
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

// The bed whose base is the last at or under y, with the tops moved by the lateral fields: a
// binary search, at most ten steps for 512 beds (gfx::formation_find, step for step).
uint formation_find(FormationSequence* s, float y, float2 g) {
  uint lo = 0u;
  uint hi = s->count;
  for (uint step = 0u; step < 10u && hi - lo > 1u; ++step) {
    const uint mid = (lo + hi) >> 1;
    if (y >= s->beds[mid].start_m + dot(s->beds[mid].base_amp_m, g))
      lo = mid;
    else
      hi = mid;
  }
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
  const float base = s->beds[i].start_m + dot(s->beds[i].base_amp_m, g);
  float top = period;
  if (i + 1u < s->count) top = s->beds[i + 1u].start_m + dot(s->beds[i + 1u].base_amp_m, g);
  out.index = i;
  out.fraction = top - base > 1e-9 ? saturate((y - base) / (top - base)) : 0.0;
  out.value = s->beds[i].value;
  out.resistance = s->beds[i].resistance;
  return out;
}

// The integral of the sequence's value less its mean, from the period's base to x, with the tops
// moved (gfx::formation_deviation).
float4 formation_deviation(FormationSequence* s, float x, float2 g) {
  const float period = s->period_m;
  const float k = floor(x / period);
  const float y = x - k * period;
  const uint i = formation_find(s, y, g);
  return k * (g.x * s->lateral_total0 + g.y * s->lateral_total1) + s->beds[i].integral +
         g.x * s->beds[i].lateral0 + g.y * s->beds[i].lateral1 +
         (y - s->beds[i].start_m) * (s->beds[i].value - s->mean);
}

// The sequence's value averaged over `width` metres of coordinate (a box), so an edge is as wide
// as the pixel's footprint across the beds and a sequence finer than a pixel reads as its mean.
float4 formation_filtered(FormationSequence* s, float coordinate, float width, float2 g) {
  if (s->count == 0u) return float4(1.0, 1.0, 1.0, 0.0);
  if (width <= 1e-6) return formation_bed(s, coordinate, g).value;
  return s->mean + (formation_deviation(s, coordinate + 0.5 * width, g) -
                    formation_deviation(s, coordinate - 0.5 * width, g)) / width;
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
  const float4 bed = formation_filtered((FormationSequence*)(address + k_formation_beds_offset),
                                        p.bed, width, p.lateral);
  const float4 macro = formation_filtered((FormationSequence*)(address + k_formation_macro_offset),
                                          p.macro, width, p.lateral);
  return float4(bed.rgb * macro.rgb, bed.w + macro.w);
}

#endif
