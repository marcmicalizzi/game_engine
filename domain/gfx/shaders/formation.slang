// **A world formation** (roadmap W85; renderer.md, "Formation bands"; the grounding,
// docs/experiments/formation-field.md). No bindings and no entry points, like brdf.slang and
// sky.slang: the terrain resolve includes it through terrain_material.slang, and the gfx test's
// probe (formation_probe.slang) holds it to its f64 twin in domain/gfx/formation.h.
//
// The bed coordinate is n . (p - origin) - fold(t . (p - origin)). **No absolute position is
// formed** (ADR-0053): the integer part, the frame's cell less the origin in whole millimetres, is
// dotted with the directions as integers over 2^24 and reduced modulo the period in int64; only the
// remainder and the frame-local part become floats, so the precision is the same anywhere
// (formation.h has the argument).
#ifndef ENGINE_FORMATION_SLANG
#define ENGINE_FORMATION_SLANG

static const uint k_formation_max_beds = 8u;
static const uint k_formation_valid = 1u;
// Where the two sequences sit in the block (gfx::FormationParams).
static const uint64_t k_formation_beds_offset = 96;
static const uint64_t k_formation_macro_offset = 512;
static const float k_formation_q = 1.0 / 16777216.0;  // 2^-24
static const float k_formation_tau = 6.28318530717958647;

// gfx::FormationBedGpu, 48 bytes.
struct FormationBed {
  float4 value;     // rgb tint, roughness offset
  float4 integral;  // the sequence's value integrated from its base to this bed's base
  float start_m;
  float thickness_m;
  float resistance;
  uint pad;
};

// gfx::FormationSequence, 416 bytes.
struct FormationSequence {
  uint count;
  uint period_mm;
  float period_m;
  uint pad;
  float4 total;
  FormationBed beds[k_formation_max_beds];
};

// gfx::FormationParams, 928 bytes.
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
  FormationSequence beds;
  FormationSequence macro;
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

// The folded coordinate at a point, reduced for each sequence: `bed` and `macro` agree with the
// twin's coordinate modulo their periods, and `gradient` is the coordinate's per metre.
struct FormationPoint {
  float bed;
  float macro;
  float3 gradient;
};

// `cell` is the frame's eye cell and `local` the point from that cell's corner in metres (the
// eye's local and residual plus the frame-local point).
FormationPoint formation_coordinate(FormationParams* f, int3 cell, float3 local) {
  const int64_t dx = int64_t(cell.x) * int64_t(64000) - f->origin_x_mm;
  const int64_t dy = int64_t(cell.y) * int64_t(64000) - f->origin_y_mm;
  const int64_t dz = int64_t(cell.z) * int64_t(64000) - f->origin_z_mm;
  const int64_t sn = dx * f->normal_x_q + dy * f->normal_y_q + dz * f->normal_z_q;
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
  FormationPoint p;
  p.bed = formation_reduce(sn, f->beds.period_mm) + ln - fold;
  p.macro = formation_reduce(sn, f->macro.period_mm) + ln - fold;
  p.gradient = n - slope * t;
  return p;
}

// Which bed of the sequence a coordinate is in, the fraction across it, and what it carries.
struct FormationBedSample {
  uint index;
  float fraction;
  float4 value;
  float resistance;
};

uint formation_find(FormationSequence* s, float y) {
  uint i = 0u;
  for (uint k = 1u; k < s->count; ++k)
    if (y >= s->beds[k].start_m) i = k;
  return i;
}

FormationBedSample formation_bed(FormationSequence* s, float coordinate) {
  FormationBedSample out;
  out.index = 0u;
  out.fraction = 0.0;
  out.value = float4(1.0, 1.0, 1.0, 0.0);
  out.resistance = 0.0;
  if (s->count == 0u) return out;
  const float period = s->period_m;
  const float y = coordinate - period * floor(coordinate / period);
  const uint i = formation_find(s, y);
  out.index = i;
  out.fraction = saturate((y - s->beds[i].start_m) / s->beds[i].thickness_m);
  out.value = s->beds[i].value;
  out.resistance = s->beds[i].resistance;
  return out;
}

float4 formation_integral(FormationSequence* s, float x) {
  const float period = s->period_m;
  const float k = floor(x / period);
  const float y = x - k * period;
  const uint i = formation_find(s, y);
  return k * s->total + s->beds[i].integral + (y - s->beds[i].start_m) * s->beds[i].value;
}

// The sequence's value averaged over `width` metres of coordinate (a box), so an edge is as wide
// as the pixel's footprint across the beds and a sequence finer than a pixel reads as its mean.
float4 formation_filtered(FormationSequence* s, float coordinate, float width) {
  if (s->count == 0u) return float4(1.0, 1.0, 1.0, 0.0);
  if (width <= 1e-6) return formation_bed(s, coordinate).value;
  return (formation_integral(s, coordinate + 0.5 * width) -
          formation_integral(s, coordinate - 0.5 * width)) / width;
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
  const float4 bed = formation_filtered((FormationSequence*)(address + k_formation_beds_offset), p.bed, width);
  const float4 macro = formation_filtered((FormationSequence*)(address + k_formation_macro_offset), p.macro, width);
  return float4(bed.rgb * macro.rgb, bed.w + macro.w);
}

#endif
