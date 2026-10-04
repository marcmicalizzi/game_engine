// The ground's detail: wind ripples and grain on a terrain's sand, a **function of the surface
// point's world (x, z)**, the scene's seed and the wind, with no texture coordinates, no tiles and
// nothing that meets at an edge (docs/subsystems/gfx.md, "The ground's detail"; the C++ side and
// the reasons are in domain/gfx/ground_detail.h). It is shared, like brdf.slang: it declares no
// binding, no push constant and no entry point, so the resolve (visibility_resolve.slang, the
// rasterized and the ray-traced picture alike) and the reference path tracer (path_trace.slang)
// include it and shade the same sand. domain/gfx/tests/ground_detail_reference.h is the same
// arithmetic in double precision, and ground_detail_tests.cpp holds the GPU to it.
//
// Three parts:
//
// - **The ripples** (`ground_ripple`): phasor noise (Tricard et al. 2019) — complex Gabor kernels
//   on a jittered lattice, each a sinusoid along the wind within `spread` of it under the bell
//   `(1 - r^2/R^2)^3`, which reaches zero with its slope at the kernel's radius so a kernel leaves no
//   edge where it ends. The sum's argument is the phase; an asymmetric profile is laid on it (a
//   warped cosine, rising over the windward share of a wavelength and falling over the rest, whose
//   slope is continuous because the warp's kink sits where the cosine is flat); and the height is
//   tapered where the sum's magnitude is small, which is where the phase has its singularities — a
//   crest ending, two crests joining — so a defect fades rather than pinches. Everything is
//   differentiated analytically, so the normal needs no finite difference.
// - **The grain** (`ground_value_noise2`): value noise on a lattice of `grain_size` and a quarter of
//   it, from a hash of the lattice point and the seed, smoothstep-interpolated so it has no crease.
//   Two channels, one for the albedo and one for the roughness.
// - **The filter** (`ground_detail_shade`): each term fades by how many of its periods one pixel
//   spans (`ground_fade`), from the footprint the caller measured — the ripples along the wind, per
//   screen axis; the grain by the footprint's longest side — and the ripples' lost slope variance
//   goes into the GGX roughness, so distant sand is rougher and not flatter. The ripples fade in two
//   stages, because their steep lee is a quarter of a wavelength and aliases an octave earlier than
//   the ripple: first the profile eases to a plain sinusoid, then the sinusoid fades. They also fade
//   by the ground's slope, towards the angle of repose, and everything by the sand share.
//
// **No absolute coordinate is ever a float here** (2026-10-04, gfx.md "Far from the origin"): a
// point is its offset from the block's frame origin, a corner of a 1,024 m grid near the eye, and
// every lattice, hash and lane is counted from where the CPU found the origin stands on it, in
// double, so the pattern is the world's own function to 10,000 km and further.

#ifndef ENGINE_GROUND_DETAIL_SLANG
#define ENGINE_GROUND_DETAIL_SLANG

// GroundDetail::flags (gfx::k_ground_*).
static const uint k_ground_ripples = 1u;
static const uint k_ground_grain = 2u;
static const uint k_ground_exposure = 4u;
static const uint k_ground_gradient_grain = 8u;
static const uint k_ground_streaks = 16u;
static const uint k_ground_spacing = 32u;
static const uint k_ground_grainflow = 64u;
static const uint k_ground_patches = 128u;
static const uint k_ground_steering = 256u;
static const uint k_ground_motion = 512u;
// gfx::k_ground_flow_rms, k_ground_flow_slope_rms.
static const float k_ground_flow_rms = 0.213;
static const float k_ground_flow_slope_rms = 0.723;
// gfx::k_ground_flow_body: the tongues' fade, in lane widths a period.
static const float k_ground_flow_body = 0.75;
// Streak kernels per lattice cell (gfx::k_ground_streak_impulses).
static const uint k_ground_streak_impulses = 3u;
// gfx::k_ground_noise_rms, k_ground_noise_slope_rms, k_ground_grain_rms.
static const float k_ground_noise_rms = 0.182;
static const float k_ground_noise_slope_rms = 0.743;
static const float k_ground_grain_rms = 0.28;
// Kernels per lattice cell (gfx::k_ground_impulses).
static const uint k_ground_impulses = 2u;
static const float k_ground_two_pi = 6.28318530717958647692;

// Mirrors gfx::GroundDetailParams up to its lane table (304 of its 560 bytes; the table is read
// through the block's address, `ground_lanes`).
struct GroundDetail {
  float4 wind;            // xy the direction the sand moves (x, z), unit; z sin(spread)
  float wavelength;       // metres crest to crest
  float amplitude;        // metres, half the crest-to-trough height
  float asymmetry;        // the windward share of a wavelength
  float cell;             // metres: a kernel's radius and the lattice's cell
  float magnitude_floor;  // the taper's floor, c^2
  float slope_scale;      // E|grad h|^2 = slope_scale (1/a + 1/(1 - a)) at asymmetry a
  float slope_cos_start;  // the shading normal's y where the ripples start to fade
  float slope_cos_end;    // and where there are none
  float grain_size;       // metres: the coarse octave's cell
  float grain_albedo;     // the albedo's variation, a share either way
  float grain_roughness;  // the roughness's, either way
  uint seed;
  uint flags;
  float lee_tan_start;    // the fall along the wind at which the ripples start to go (exposure)
  float lee_tan_end;      // and where there are none
  uint grain_octaves;     // the gradient grain's octaves (k_ground_gradient_grain)
  float grain_normal;     // the root-mean-square slope its height leans the normal by
  float streak_tan_start;  // the fall along the wind where the streaks start (k_ground_streaks)
  float streak_tan_full;   // and where they are whole
  float streak_width;      // metres, a tongue across
  float streak_length;     // metres, a tongue along the fall line, and the lattice's cell
  float streak_albedo;     // the albedo's share per unit of the tongues' sum
  float streak_roughness;  // the roughness's
  float streak_slope;      // the normal's lean per unit of the sum's gradient
  float streak_normal;     // the root-mean-square slope that is, for the roughness transfer
  float spacing_gain;      // the wavelength's scale per unit of climb into the wind
  float spacing_min;       // (k_ground_spacing) and its clamp
  float spacing_max;
  float flow_tan_start;   // grainflow (k_ground_grainflow): where the lanes start and are whole
  float flow_tan_full;
  float flow_length;      // metres, a lane's segment: the longest a tongue runs
  float flow_width;       // metres, a lane
  float flow_slope;       // the normal's lean per unit of the lanes' derivative
  float flow_normal;      // the root-mean-square slope that is, for the roughness transfer
  float flow_albedo;      // the albedo's share per unit of the lanes' value
  float flow_retired;     // the third pass's widening, retired
  float patch_size;       // patches (k_ground_patches): the coarse cell, metres
  float patch_mid;        // the wavelength's scale 2^(v half_log2) about the mid
  float patch_half_log2;
  float patch_defects;    // the spread's share either way
  float steer_gain;       // steering (k_ground_steering)
  float steer_cos_max;
  float steer_sin_max;
  float ripple_live;      // motion (k_ground_motion): the height kept, 0 in a storm
  float travel;           // metres along the wind, reduced
  float travel_per_frame; // metres this frame
  uint pad6;
  uint pad7;
  float flow_share;       // the grainflow's episodes: the share of lanes present (1: all, always)
  float flow_clock;       // the avalanche clock, a fraction of a cycle
  float flow_clock_step;  // its advance this frame
  uint pad8;
  // The frame (gfx::ground_detail_frame; gfx.md, "Far from the origin"): the point arrives as its
  // offset from `origin`, and each lattice as the cell `origin` falls in (`*_base`, the world's own
  // integers, wrapped to 32 bits) and `origin`'s offset into it (`*_rem`).
  float2 origin;          // metres (x, z), a multiple of gfx::k_ground_frame_cell near the eye
  uint pad9;
  uint pad10;
  int2 ripple_base;       // the kernels' lattice, `cell`
  float2 ripple_rem;
  int2 grain_base;        // the grain's, `grain_size`; its finer octaves by doubling
  float2 grain_rem;
  int2 patch_base;        // the patches', `patch_size`
  float2 patch_rem;
  int2 streak_base;       // the streaks', `streak_length`
  float2 streak_rem;
};

// One grainflow direction at the frame's origin (gfx::GroundLane): the lane line at or before it
// and its distance past it, the segment it is in and its distance into it.
struct GroundLane {
  int lane;
  int segment;
  float across;
  float along;
};
// gfx::k_ground_lanes_offset: the sixteen lanes behind the block.
static const uint64_t k_ground_lanes_offset = 304;
GroundLane* ground_lanes(uint64_t block) { return (GroundLane*)(block + k_ground_lanes_offset); }

// PCG, as sampling.slang's `pcg_hash`: one multiply-add and a permutation, the same bits on every
// device and in the CPU mirror.
uint ground_pcg(uint v) {
  const uint state = v * 747796405u + 2891336453u;
  const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

// A lattice point's hash: the seed and a stream, then the two coordinates, one permutation each,
// so neighbouring points and streams are unrelated.
uint ground_hash(int x, int z, uint seed, uint stream) {
  uint h = ground_pcg(seed ^ (stream * 0x9E3779B9u));
  h = ground_pcg(h ^ uint(x));
  return ground_pcg(h ^ uint(z));
}

// The top 24 bits as [0, 1): exact in a float, so the CPU mirror draws the same numbers.
float ground_unit(uint h) { return float(h >> 8u) * (1.0 / 16777216.0); }

// Where a term stands when one pixel spans `cycles` of its periods: all of it at a quarter or less
// (four pixels a period), none at a half (two, the Nyquist limit), smooth between.
float ground_fade(float cycles) { return 1.0 - smoothstep(0.25, 0.5, cycles); }

// The ripples' height at (x, z), metres, and its gradient over (x, z).
struct GroundRipple {
  float height;
  float2 gradient;
};

// `scale` multiplies the wavelength and the height (the spacing, 1 without k_ground_spacing). It is
// the pixel's own, from its normal, and every kernel reaching the pixel uses it, so the sum stays
// coherent; it enters only through the wavenumber, which multiplies a kernel's offset from its
// centre and never the absolute position, so a change in it slides a crest by at most the kernel's
// radius times the change (renderer.md, "Spacing that follows the wind").
// `w` is the ripples' direction here and `spread` the sine of their kernels' spread about it (the
// block's own wind and spread, or the steering's and the patches'), and `travel` how far they have
// moved along it: every kernel's phase is taken back by k travel, so the crests move and the
// kernels stay where the lattice put them.
// `p` is the point's offset from the frame's origin: the lattice cell `c` it falls in is counted
// from the origin's (`ripple_base`), so the hash draws the world's kernel `ripple_base + c`, and
// the offset from a kernel's centre is the difference of two numbers the size of the frame.
GroundRipple ground_ripple(GroundDetail d, float2 p, float asymmetry, float scale, float2 w,
                           float spread, float travel) {
  const float inv_cell = 1.0 / d.cell;
  const float2 q = (p + d.ripple_rem) * inv_cell;
  const int2 base = int2(floor(q));
  const float2 across = float2(-w.y, w.x);
  const float k = k_ground_two_pi / (d.wavelength * scale);
  // The travel takes back the base wavelength's phase whatever the scale here, so a ripple s times
  // as long and as high moves 1/s as far (a bedform's celerity is the flux over its height): the
  // phase is then one function of the clock everywhere, a whole number of base wavelengths of
  // travel (the block's reduction) moves no crest at any scale, and nothing winds up where the
  // scale varies (renderer.md, "Ripples that move").
  const float shift = k_ground_two_pi * travel / d.wavelength;
  const float inv_r2 = inv_cell * inv_cell;
  // The complex sum S and its two partial derivatives, each a complex number (re, im).
  float2 s = float2(0.0, 0.0);
  float2 sx = float2(0.0, 0.0);
  float2 sz = float2(0.0, 0.0);
  for (int j = -1; j <= 1; ++j) {
    for (int i = -1; i <= 1; ++i) {
      const int2 c = base + int2(i, j);
      const int2 world = d.ripple_base + c;
      for (uint n = 0u; n < k_ground_impulses; ++n) {
        uint h = ground_hash(world.x, world.y, d.seed, n);
        const float ux = ground_unit(h);
        h = ground_pcg(h);
        const float uz = ground_unit(h);
        // The point relative to the kernel's centre, in cells and then in metres: the cell's index
        // and the point's are both counted from the frame's origin, so they are subtracted before
        // anything is scaled.
        const float2 offset = (q - (float2(c) + float2(ux, uz))) * d.cell;
        const float r2 = dot(offset, offset) * inv_r2;
        if (r2 >= 1.0) continue;
        h = ground_pcg(h);
        const float phase0 = ground_unit(h) * k_ground_two_pi;
        h = ground_pcg(h);
        const float js = (ground_unit(h) * 2.0 - 1.0) * spread;
        const float2 dir = w * sqrt(1.0 - js * js) + across * js;
        const float t = 1.0 - r2;
        const float env = t * t * t;
        const float2 denv = offset * (-6.0 * t * t * inv_r2);
        float sn;
        float cs;
        sincos(k * dot(dir, offset) + phase0 - shift, sn, cs);
        const float2 e = float2(cs, sn);
        const float2 de = float2(-sn, cs) * (env * k);
        s += e * env;
        sx += e * denv.x + de * dir.x;
        sz += e * denv.y + de * dir.y;
      }
    }
  }
  GroundRipple out;
  out.height = 0.0;
  out.gradient = float2(0.0, 0.0);
  const float m2 = dot(s, s);
  if (!(m2 > 0.0)) return out;
  // The taper m = |S|^2 / (|S|^2 + c^2) and its gradient; and the phase's gradient times it,
  // (S.re grad S.im - S.im grad S.re) / (|S|^2 + c^2), which is finite where |S| vanishes.
  const float c2 = d.magnitude_floor;
  const float denom = m2 + c2;
  const float taper = m2 / denom;
  const float2 dtaper = float2(dot(s, sx), dot(s, sz)) * (2.0 * c2 / (denom * denom));
  const float2 tapered_dphase =
      float2(s.x * sx.y - s.y * sx.x, s.x * sz.y - s.y * sz.x) / denom;
  // The profile over the phase as a fraction of a turn, (0, 1]: the trough at both ends, the crest
  // at `asymmetry`.
  const float turn = atan2(s.y, s.x) / k_ground_two_pi + 0.5;
  const float a = asymmetry;
  float u;
  float du;
  if (turn < a) {
    u = 0.5 * turn / a;
    du = 0.5 / a;
  } else {
    u = 0.5 + 0.5 * (turn - a) / (1.0 - a);
    du = 0.5 / (1.0 - a);
  }
  float ps;
  float pc;
  sincos(k_ground_two_pi * u, ps, pc);
  const float profile = -pc;
  // d(profile)/d(turn) = 2 pi sin(2 pi u) du, and d(turn) = d(phase) / 2 pi.
  const float dprofile = ps * du;
  const float amplitude = d.amplitude * scale;
  out.height = amplitude * taper * profile;
  out.gradient = (dtaper * profile + tapered_dphase * dprofile) * amplitude;
  return out;
}

// How much the ground at `normal` faces the wind, [0, 1]: 1 on flat ground and ground that climbs
// into the wind, falling to 0 across a band of lee slope. `dot(n.xz, w) / n.y` is the ground's
// fall along the wind — the tangent of its slope down it, negative climbing into the wind and
// positive falling away — so a slope facing across the wind falls along it by less. A function of
// the normal and the wind alone: no data, no seam, and it follows a dune that moves.
float ground_exposure(GroundDetail d, float3 normal) {
  const float fall = dot(normal.xz, d.wind.xy) / max(normal.y, 1e-4);
  return 1.0 - smoothstep(d.lee_tan_start, d.lee_tan_end, fall);
}

// Where `p`, an offset from the frame's origin, falls on a lattice of side `size` whose cell at the
// origin is `base` and which the origin is `rem` into: the world's cell, as 32-bit integers, and
// the fraction inside it. A lattice of a side halved n times from the one `base` and `rem` were made
// for takes `base` times 2^n and the same `rem` (gfx::GroundLattice). The local coordinate is
// counted in tiles of 4,096 cells, which kept the fraction's precision when `p` was the world's
// coordinate and leaves the arithmetic the same bytes it was for a frame at the world's origin.
struct GroundCell {
  int2 i;
  float2 f;
};

GroundCell ground_cell_at(float2 p, float size, int2 base, float2 rem) {
  const float2 x = p + rem;
  const float tile = size * 4096.0;
  const float2 t = floor(x / tile);
  const float2 q = (x - t * tile) / size;
  const float2 f0 = floor(q);
  GroundCell c;
  c.f = q - f0;
  c.i = base + int2(t) * 4096 + int2(f0);
  return c;
}

// Two channels of value noise at `p`, on a lattice of `size`, each in [-1, 1].
float2 ground_lattice2(int2 i, uint seed, uint stream) {
  const uint h = ground_hash(i.x, i.y, seed, stream);
  return float2(float(h & 0xffffu), float(h >> 16u)) * (2.0 / 65535.0) - 1.0;
}

float2 ground_value_noise2(float2 p, float size, int2 base, float2 rem, uint seed, uint stream) {
  const GroundCell cell = ground_cell_at(p, size, base, rem);
  const float2 f = cell.f;
  const int2 i = cell.i;
  const float2 u = f * f * (3.0 - 2.0 * f);
  const float2 v00 = ground_lattice2(i, seed, stream);
  const float2 v10 = ground_lattice2(i + int2(1, 0), seed, stream);
  const float2 v01 = ground_lattice2(i + int2(0, 1), seed, stream);
  const float2 v11 = ground_lattice2(i + int2(1, 1), seed, stream);
  return lerp(lerp(v00, v10, u.x), lerp(v01, v11, u.x), u.y);
}

// Three channels of gradient noise at `p` on a lattice of `size` — the albedo's, the roughness's and
// the grain's height — and the height's gradient over the lattice's coordinates (per cell). Each
// corner's three gradients come from one hash, five bits a component in [-1, 1], so an octave is
// four lattice hashes; the quintic fade makes the value's second derivative continuous, so the
// normal the height leans has no crease. Gradient noise rather than value noise because it has no
// lattice to see: it is zero at every lattice point and its extremes fall between them. The cell
// is `ground_cell_at`'s, as the value noise's is.
struct GroundGrain {
  float3 value;
  float2 gradient;
};

float2 ground_grain_gradient(uint h, uint channel) {
  const uint bits = h >> (10u * channel);
  return (float2(float(bits & 31u), float((bits >> 5u) & 31u)) - 15.5) * (1.0 / 15.5);
}

GroundGrain ground_gradient_noise3(float2 p, float size, int2 base, float2 rem, uint seed,
                                   uint stream) {
  const GroundCell cell = ground_cell_at(p, size, base, rem);
  const float2 f = cell.f;
  const int2 i = cell.i;
  const float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  const float2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
  const uint h00 = ground_hash(i.x, i.y, seed, stream);
  const uint h10 = ground_hash(i.x + 1, i.y, seed, stream);
  const uint h01 = ground_hash(i.x, i.y + 1, seed, stream);
  const uint h11 = ground_hash(i.x + 1, i.y + 1, seed, stream);
  GroundGrain out;
  out.value = float3(0.0, 0.0, 0.0);
  out.gradient = float2(0.0, 0.0);
  [unroll]
  for (uint c = 0u; c < 3u; ++c) {
    const float2 ga = ground_grain_gradient(h00, c);
    const float2 gb = ground_grain_gradient(h10, c);
    const float2 gc = ground_grain_gradient(h01, c);
    const float2 gd = ground_grain_gradient(h11, c);
    const float va = dot(ga, f);
    const float vb = dot(gb, f - float2(1.0, 0.0));
    const float vc = dot(gc, f - float2(0.0, 1.0));
    const float vd = dot(gd, f - float2(1.0, 1.0));
    const float k1 = vb - va;
    const float k2 = vc - va;
    const float k3 = va - vb - vc + vd;
    out.value[c] = va + k1 * u.x + k2 * u.y + k3 * u.x * u.y;
    if (c == 2u) {
      out.gradient = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
                     du * float2(k1 + k3 * u.y, k2 + k3 * u.x);
    }
  }
  return out;
}

// Grainflow streaks: tongues of sand avalanched down a slip face, as a sum of kernels on a lattice
// of `streak_length` fixed in the world, each an ellipse `streak_length` long down the fall line
// and `streak_width` across under the bell `(1 - r^2)^3`, lighter or darker by a draw in [-1, 1].
// **The fall line depends on the normal, and only an offset from a kernel's centre is ever
// projected on it**: the offset is bounded by the kernel's reach, so a thousandth of a radian of
// normal moves the pattern by a thousandth of a metre and not, 3 km from the origin, by metres —
// the ripples' kernels work the same way. The sum and its gradient over (x, z).
struct GroundStreak {
  float value;
  float2 gradient;
};

GroundStreak ground_streaks(GroundDetail d, float2 p, float2 fall) {
  const float inv_cell = 1.0 / d.streak_length;
  const float2 q = (p + d.streak_rem) * inv_cell;
  const int2 base = int2(floor(q));
  const float2 across = float2(-fall.y, fall.x);
  const float inv_a2 = 4.0 / (d.streak_length * d.streak_length);
  const float inv_b2 = 4.0 / (d.streak_width * d.streak_width);
  GroundStreak out;
  out.value = 0.0;
  out.gradient = float2(0.0, 0.0);
  for (int j = -1; j <= 1; ++j) {
    for (int i = -1; i <= 1; ++i) {
      const int2 c = base + int2(i, j);
      const int2 world = d.streak_base + c;
      for (uint n = 0u; n < k_ground_streak_impulses; ++n) {
        uint h = ground_hash(world.x, world.y, d.seed, 32u + n);
        const float ux = ground_unit(h);
        h = ground_pcg(h);
        const float uz = ground_unit(h);
        const float2 offset = (q - (float2(c) + float2(ux, uz))) * d.streak_length;
        const float u = dot(offset, fall);
        const float w = dot(offset, across);
        const float r2 = u * u * inv_a2 + w * w * inv_b2;
        if (r2 >= 1.0) continue;
        h = ground_pcg(h);
        const float v = ground_unit(h) * 2.0 - 1.0;
        const float t = 1.0 - r2;
        out.value += v * (t * t * t);
        out.gradient += (fall * (u * inv_a2) + across * (w * inv_b2)) * (v * -6.0 * t * t);
      }
    }
  }
  return out;
}

// Steering: the ripples' direction on a slope oblique to the wind turns along the contours, since
// the flow climbing it is slowed and deflected. The wind loses `min(steer_gain tan(slope), 1/2)` of
// its component along the ground's fall line — at most half, so where the wind runs nearly along
// the fall line the turn grows no faster than the normal turns (a degree a degree) —
// and the turn is clamped to the scene's largest. Straight up or down a slope it does not turn; on
// the flat, nothing. Arithmetic only, and a function of the normal: only offsets from a kernel's
// centre are ever projected on the result.
float2 ground_steer(GroundDetail d, float2 w, float3 normal) {
  const float2 g = normal.xz / max(normal.y, 1e-4);
  const float gl = length(g);
  if (!(gl > 1e-6)) return w;
  const float2 fall = g / gl;
  const float r = min(d.steer_gain * gl, 0.5);
  const float2 bent = w - fall * (dot(w, fall) * r);
  const float len = length(bent);
  if (!(len > 1e-6)) return w;
  const float2 b = bent / len;
  const float cs = dot(w, b);
  if (cs >= d.steer_cos_max) return b;
  const float sn = w.x * b.y - w.y * b.x;
  return w * d.steer_cos_max + float2(-w.y, w.x) * (sn >= 0.0 ? d.steer_sin_max : -d.steer_sin_max);
}

// Patches: two octaves of the grain's gradient noise at `patch_size` and half it, two channels —
// the wavelength's and the defects' — each scaled to about [-1, 1] and clamped. A function of the
// position alone, so it does not slide when the normal changes, and eight lattice hashes.
float2 ground_patch(GroundDetail d, float2 p) {
  const GroundGrain a =
      ground_gradient_noise3(p, d.patch_size, d.patch_base, d.patch_rem, d.seed, 64u);
  const GroundGrain b =
      ground_gradient_noise3(p, d.patch_size * 0.5, d.patch_base * 2, d.patch_rem, d.seed, 65u);
  const float2 v = (a.value.xy + b.value.xy * 0.5) * (0.5 / (k_ground_noise_rms * 1.118034));
  return clamp(v, -1.0, 1.0);
}

// Grainflow: lanes of avalanched sand down a slip face, long, and seen by their relief. **Lanes in
// sixteen fixed directions** 22.5 degrees apart: for each, a one-dimensional gradient noise of the
// position's own coordinate across that direction, `flow_width` a lane, so a lane is a function of
// position alone, constant along its direction and running without end — the slip face's weight is
// what ends it, at the brink and the toe, as a real one ends. A pixel evaluates the two directions
// nearest its fall line and blends them by the angle between, each normalised so the blend keeps
// the lanes' contrast: **the normal moves only the blend's weights**, never a lane, so a thousandth
// of a radian of it moves nothing by metres however far from the origin; a lane is at most 11.25
// degrees off the true fall line; where two directions blend they cross coarsely at half strength
// each rather than beating. Each lattice point's gradient is an episode of its own (`ground_episode`),
// so a present lane beside absent ones is one chute with its levee, the height of the face. The
// value and its gradient over (x, z); two lattice hashes a direction, four a pixel. (The third
// pass's cells and the widening fan made neighbouring cells' lanes cross at up to 17 degrees and
// beat into ovals: docs/experiments/sand-fourth-pass-2026-10-03.md.)
struct GroundFlow {
  float value;
  float2 gradient;
};

static const uint k_ground_flow_directions = 16u;

// A lane's presence at phase `phase` of its cycle, present for `share` of it: up over the first
// twentieth of that (an avalanche is sudden), down over the last seven tenths (grainfall buries it
// slowly), and 0 for the rest, so the wrap of the phase is no step.
float ground_episode(float phase, float share) {
  const float t = phase / share;
  if (!(t < 1.0)) return 0.0;
  const float up = saturate(t / 0.05);
  const float down = saturate((t - 0.3) / 0.7);
  return up * up * (3.0 - 2.0 * up) * (1.0 - down * down * (3.0 - 2.0 * down));
}

// A smoothstep and its derivative over x.
float2 ground_smooth_d(float e0, float e1, float x) {
  const float t = saturate((x - e0) / (e1 - e0));
  return float2(t * t * (3.0 - 2.0 * t), 6.0 * t * (1.0 - t) / (e1 - e0));
}

// One grainflow tongue, drawn from its lane's hash: its length and base half-width, metres; the
// meander of its centreline (two harmonics, amplitude and wavenumber, phases); and, for about three
// tongues in ten, where down its length it splits into two lobes.
struct GroundTongue {
  float len;
  float half0;
  float a1;
  float k1;
  float p1;
  float a2;
  float k2;
  float p2;
  float split;  // 1 for a tongue that branches, else 0
  float split_at;
};

// A lobe's bump at offset v from its centre over half-width hw, rounded at the toe: the toe's
// edges end before its middle, so a lobe is round in plan.
float ground_lobe(float v, float hw, float tau) {
  const float r = v / hw;
  const float r2 = r * r;
  if (!(r2 < 1.0)) return 0.0;
  const float q = 1.0 - r2;
  return q * q * (1.0 - ground_smooth_d(0.0, 1.0, (tau - 0.8 + 0.12 * r2) / 0.2).x);
}

// The tongue's height (1 at a lobe's crest) at `x` metres across its lane's centre and `tl` metres
// down from its head. A real grainflow (Hunter 1977; Kocurek and Dott 1981): a chute scoured under
// the brink, easing into a lobe of deposit that widens down the face; its centreline
// wanders, more toward the toe, and its width swells and pinches; some lobes split in two, each
// half ending in its own rounded toe. Soft at the head over a tenth of its length and at the toe
// over a fifth. Local coordinates only, so it is exact in float anywhere in the world.
float ground_tongue(GroundTongue g, float x, float tl) {
  const float tau = tl / g.len;
  if (!(tau > 0.0 && tau < 1.0)) return 0.0;
  const float grow = 0.3 + 0.7 * tau;
  const float centre = grow * (g.a1 * sin(g.k1 * tl + g.p1) + g.a2 * sin(g.k2 * tl + g.p2));
  const float pinch = 1.0 + 0.12 * sin(1.7 * g.k2 * tl + g.p1);
  const float half_w = g.half0 * (0.6 + 0.8 * tau) * pinch;
  const float u = x - centre;
  // The head's chute, on the tongue's own centreline: a shallow scour with no levees. (The
  // fourth pass drew levees, `q^2 (2.4 r^2 - 0.6)`; their steep sides, a few centimetres across,
  // came and went along the tongue and under a low sun read as broken light and dark dashes a metre
  // or two long, which is what the owner saw: docs/experiments/sand-fifth-pass-2026-10-04.md.)
  const float r = u / half_w;
  const float r2 = r * r;
  float chute = 0.0;
  if (r2 < 1.0) {
    const float q = 1.0 - r2;
    chute = -0.3 * q * q;
  }
  // The lobe, or two: apart by up to 0.55 half-widths each side, each 0.7 as wide, normalised so
  // the pair is one lobe's height where they have not yet parted.
  const float s = g.split * ground_smooth_d(g.split_at, g.split_at + 0.25, tau).x;
  const float d = s * 0.55 * half_w;
  const float hw = half_w * (1.0 - 0.3 * s);
  const float pair = ground_lobe(u - d, hw, tau) + ground_lobe(u + d, hw, tau);
  const float ro = 2.0 * d / hw;
  const float overlap = ro * ro < 1.0 ? (1.0 - ro * ro) * (1.0 - ro * ro) : 0.0;
  const float lobe = pair / (1.0 + overlap);
  const float lw = ground_smooth_d(0.35, 0.8, tau).x;
  const float cw = 1.0 - ground_smooth_d(0.2, 0.55, tau).x;
  return ground_smooth_d(0.0, 0.1, tau).x * (lw * lobe + cw * chute);
}

// One direction's grainflow at `p`: lanes `2 flow_width` apart across direction k, each a chain of
// tongues along it, one to a segment of `flow_length` (staggered per lane by the golden ratio),
// each running 45 to 95% of the segment with its own start, width (0.7 to 1.3 of `flow_width`),
// lateral jitter, meander, branching and episode. Everything is a function of position alone (the
// lane's coordinate across and along a fixed direction), so the normal never moves it. The height
// and its gradient over (x, z), per metre, the gradient by forward differences of 2 mm in the
// tongue's own local coordinates (the shape is arithmetic; its hashes are drawn once); three lanes,
// three lattice hashes. `p` is the point's offset from the frame's origin, and `lanes[k]` where the
// origin stands across and along direction k, so `sa` and `t` are measured from the origin's lane
// line and segment start, and the world's lane and segment are those integers plus the local ones.
// The stagger is the golden ratio's fraction of the world's lane index in 32-bit fixed point
// (`0x9E3779B9`), exact for every lane; until the frame it was `frac(m * 0.618…)` in float, which
// 420 km out (lane 350,000) rounded a lane's tongues up to 0.2 m along it.
GroundFlow ground_lane_set(GroundDetail d, GroundLane* lanes, float2 p, uint k) {
  const float angle = float(k) * (k_ground_two_pi / float(k_ground_flow_directions));
  const float2 dir = float2(cos(angle), sin(angle));
  const float2 across = float2(-dir.y, dir.x);
  const float spacing = 2.0 * d.flow_width;
  const float seg = d.flow_length;
  const GroundLane lane = lanes[k];
  const float sa = lane.across + dot(p, across);
  const float t = lane.along + dot(p, dir);
  const float mc = floor(sa / spacing + 0.5);
  const float e = 0.002;
  GroundFlow out;
  out.value = 0.0;
  out.gradient = float2(0.0, 0.0);
  for (int i = -1; i <= 1; ++i) {
    const float m = mc + float(i);
    const int world_lane = lane.lane + int(m);
    const float stagger = ground_unit(uint(world_lane) * 0x9E3779B9u) * seg;
    const float j = floor((t - stagger) / seg);
    uint h = ground_hash(world_lane, lane.segment + int(j), d.seed, 48u + k);
    const float jitter = (ground_unit(h) - 0.5) * 0.5 * spacing;
    h = ground_pcg(h);
    const float wscale = 0.7 + 0.6 * ground_unit(h);
    h = ground_pcg(h);
    const float lenf = 0.45 + 0.5 * ground_unit(h);
    h = ground_pcg(h);
    const float startf = ground_unit(h) * (1.0 - lenf);
    float amp = 1.0;
    if (d.flow_share < 1.0) {
      h = ground_pcg(h ^ 0x68E31DA4u);
      amp = ground_episode(frac(d.flow_clock + ground_unit(h)), d.flow_share);
    }
    if (!(amp > 0.0)) continue;
    GroundTongue g;
    g.len = lenf * seg;
    const float tl = t - (j * seg + stagger + startf * seg);
    if (!(tl > 0.0 && tl < g.len)) continue;
    const float x = sa - (m * spacing + jitter);
    g.half0 = 0.5 * d.flow_width * wscale;
    // Meander: a long harmonic of 8 to 16 m and a short one of 2 to 4 m at 0.3 of its amplitude;
    // the centreline's slope stays under about 0.2 (11 degrees).
    h = ground_pcg(h);
    g.a1 = d.flow_width * (0.13 + 0.29 * ground_unit(h));
    h = ground_pcg(h);
    g.k1 = k_ground_two_pi / (8.0 + 8.0 * ground_unit(h));
    h = ground_pcg(h);
    g.p1 = k_ground_two_pi * ground_unit(h);
    g.a2 = 0.3 * g.a1;
    h = ground_pcg(h);
    g.k2 = k_ground_two_pi / (2.0 + 2.0 * ground_unit(h));
    h = ground_pcg(h);
    g.p2 = k_ground_two_pi * ground_unit(h);
    h = ground_pcg(h);
    g.split = ground_unit(h) < 0.3 ? 1.0 : 0.0;
    h = ground_pcg(h);
    g.split_at = 0.45 + 0.2 * ground_unit(h);
    // Out of reach: the farthest a tongue's sand gets from its lane's centre.
    if (abs(x) > 1.6 * spacing) continue;
    const float height = ground_tongue(g, x, tl);
    const float dh_dx = (ground_tongue(g, x + e, tl) - height) / e;
    const float dh_dt = (ground_tongue(g, x, tl + e) - height) / e;
    out.value += amp * height;
    out.gradient += (across * dh_dx + dir * dh_dt) * amp;
  }
  return out;
}

GroundFlow ground_grainflow(GroundDetail d, GroundLane* lanes, float2 p, float2 fall) {
  const float sector = k_ground_two_pi / float(k_ground_flow_directions);
  float turn = atan2(fall.y, fall.x) / sector;
  if (turn < 0.0) turn += float(k_ground_flow_directions);
  const float k0 = min(floor(turn), float(k_ground_flow_directions - 1u));
  // Blended only over the middle fifth of the sector: everywhere else one family of straight lanes,
  // at most 9 degrees off the fall line, and two families cross only within 2.25 degrees of a
  // sector's middle (the normal then turns the weight five times as fast, still 3% of the lanes'
  // rms a milliradian).
  const float t = saturate((turn - k0 - 0.4) / 0.2);
  const float w = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
  const uint i0 = uint(k0);
  const uint i1 = (i0 + 1u) % k_ground_flow_directions;
  const GroundFlow a = ground_lane_set(d, lanes, p, i0);
  const GroundFlow b = ground_lane_set(d, lanes, p, i1);
  const float norm = rsqrt((1.0 - w) * (1.0 - w) + w * w);
  GroundFlow out;
  out.value = (a.value * (1.0 - w) + b.value * w) * norm;
  out.gradient = (a.gradient * (1.0 - w) + b.gradient * w) * norm;
  return out;
}

// What the detail makes of one shaded point, and what the debug view shows of it.
struct GroundShading {
  float3 normal;     // the shading normal with the ripples in it
  float3 albedo;     // with the grain
  float roughness;   // with the grain and the ripples' lost slope variance
  float weight;      // the ripples' share here: the sand share, the slope fade and the exposure
  float exposure;    // how much the ground faces the wind (1 without k_ground_exposure)
  float streak;      // the streaks' share here: the sand share times the slip face's band
  float flow;        // the grainflow lanes' share here, the same way
  float fade;        // the footprint's fade of the ripples
  float ripple;      // the ripples' height over their amplitude, before the weight and the fade
  float grain;       // the albedo grain's coarse octave, before any fade
};

// The detail at a surface point, read from the block at address `block` (gfx::GroundDetailParams).
// The point is `eye + from_eye` in the world, given as the two halves the resolve has (gfx.md,
// "Where a pixel meets its triangle"): `eye` a world position — the camera, or any point the caller
// measured `from_eye` from — and `from_eye` the point less it. **It is never added up in world
// coordinates**: the pattern takes `(eye - origin) + from_eye`, the point's offset from the frame's
// origin, where `eye - origin` is exact (two floats within a frame of each other) and the sum is on
// numbers the size of the frame, a float step of 61 µm 512 m out; a world coordinate 420 km out
// has 3.1 cm (gfx.md, "Far from the origin"). `normal` the shading normal it perturbs (unit; its y
// is the ground's slope the ripples fade by), `dpdx` and `dpdy` the point's world-space change per
// pixel along the screen's two axes (zero for a caller with no footprint, which then draws the
// unfiltered function), `mask` the material's sand share. `full` is the debug view's: the ripple
// height at the scene's own asymmetry and the grain's coarse octave even where the fades leave
// nothing, and no normal (the view draws none).
GroundShading ground_detail_shade(uint64_t block, float3 eye, float3 from_eye, float3 normal,
                                  float3 dpdx, float3 dpdy, float mask, float3 albedo,
                                  float roughness, bool full) {
  const GroundDetail d = ((GroundDetail*)block)[0];
  GroundShading out;
  out.normal = normal;
  out.albedo = albedo;
  out.roughness = roughness;
  out.weight = 0.0;
  out.exposure = 1.0;
  out.streak = 0.0;
  out.flow = 0.0;
  out.fade = 0.0;
  out.ripple = 0.0;
  out.grain = 0.0;
  if (!(mask > 0.0) && !full) return out;
  // The point's offset from the frame's origin. For a frame at the world's origin this is
  // `eye + from_eye`, the world position, to the bit.
  const float2 p = (eye.xz - d.origin) + from_eye.xz;
  if ((d.flags & k_ground_ripples) != 0u) {
    const float slope = smoothstep(d.slope_cos_end, d.slope_cos_start, normal.y);
    // Sand sheltered from the wind is smooth, not unresolved: what the exposure takes is taken
    // from the weight, which the roughness transfer below is weighted by too.
    if ((d.flags & k_ground_exposure) != 0u) out.exposure = ground_exposure(d, normal);
    float weight = mask * slope * out.exposure;
    // A storm flattens them (motion): taken from the weight, since a flat bed is smooth.
    if ((d.flags & k_ground_motion) != 0u) weight *= d.ripple_live;
    // The ripples' direction and spread here: the wind's and the scene's, unless the ground steers
    // them or a patch changes their defects.
    float2 w = d.wind.xy;
    float spread = d.wind.z;
    if ((d.flags & k_ground_steering) != 0u) w = ground_steer(d, w, normal);
    // The spacing: longer ripples where the ground climbs into the wind and the flow speeds up.
    float scale = 1.0;
    if ((d.flags & k_ground_spacing) != 0u) {
      const float climb = -dot(normal.xz, d.wind.xy) / max(normal.y, 1e-4);
      scale = clamp(1.0 + d.spacing_gain * climb, d.spacing_min, d.spacing_max);
    }
    if ((d.flags & k_ground_patches) != 0u) {
      const float2 v = ground_patch(d, p);
      scale *= d.patch_mid * exp2(v.x * d.patch_half_log2);
      spread = min(spread * (1.0 + v.y * d.patch_defects), 0.9);
    }
    // The footprint across the crests: the pixel's step along the wind, and across it by as much
    // as a kernel turned `spread` off the wind would see — per screen axis, the larger.
    const float2 across = float2(-w.y, w.x);
    const float fx = abs(dot(dpdx.xz, w)) + abs(dot(dpdx.xz, across)) * spread;
    const float fy = abs(dot(dpdy.xz, w)) + abs(dot(dpdy.xz, across)) * spread;
    float cycles = max(fx, fy) / (d.wavelength * scale);
    // A time-lapse blurs them as a camera's shutter would: their travel in one frame counts as
    // footprint, so at a quarter of a wavelength a frame they start to go and at a half they are
    // gone, their slope variance into the roughness like any other fade.
    float travel = 0.0;
    if ((d.flags & k_ground_motion) != 0u) {
      travel = d.travel;
      cycles = max(cycles, d.travel_per_frame / (d.wavelength * scale));
    }
    // Two stages. The profile's steep lee is a quarter of a wavelength, so its harmonics alias an
    // octave before the ripple does: they go first — the asymmetry eases to a plain sinusoid
    // between eight pixels a wavelength and four — and the ripple itself after, between four and
    // two.
    const float asymmetry = lerp(0.5, d.asymmetry, ground_fade(2.0 * cycles));
    const float fade = ground_fade(cycles);
    out.weight = weight;
    out.fade = fade;
    const float strength = weight * fade;
    if (strength > 0.0 || full) {
      const GroundRipple r =
          ground_ripple(d, p, full ? d.asymmetry : asymmetry, scale, w, spread, travel);
      out.ripple = r.height / (d.amplitude * scale);
      if (strength > 0.0 && !full) {
        out.normal = normalize(normal - float3(r.gradient.x, 0.0, r.gradient.y) *
                                            (normal.y * strength));
      }
    }
    // What the two stages took of the slope variance, into the roughness: alpha^2 += lost, alpha
    // being the perceptual roughness squared. The ripples' variance is `slope_scale (1/a +
    // 1/(1 - a))` at asymmetry a, and what is drawn is the eased profile's times the fade squared.
    const float full_variance = d.slope_scale * (1.0 / d.asymmetry + 1.0 / (1.0 - d.asymmetry));
    const float drawn_variance =
        d.slope_scale * (1.0 / asymmetry + 1.0 / (1.0 - asymmetry)) * fade * fade;
    const float lost = weight * weight * max(full_variance - drawn_variance, 0.0);
    if (lost > 0.0) {
      const float alpha = roughness * roughness;
      out.roughness = sqrt(sqrt(alpha * alpha + lost));
    }
  }
  if ((d.flags & k_ground_gradient_grain) != 0u) {
    // Octaves from `grain_size` halving, each faded by the footprint as the ripples are and by the
    // position's own precision: a float's step `m` metres out is the power of two under m times
    // 2^-23 (its exponent's bits, exactly), the reconstruction is off by a few such steps, so four
    // of them count as footprint and an octave is gone where its cell is under eight
    // (docs/experiments/sand-detail-second-pass-2026-09-29.md measures what an octave loses there).
    // `p` is the offset from the frame's origin, within 512 m of the eye's, so near the eye the
    // step is at most 61 µm and no octave of a millimetre or more is taken by it: what takes them
    // is the footprint. Until the frame it was the world coordinate, and 420 km out (3.1 cm) it
    // took every octave of the grain (gfx.md, "Far from the origin").
    const float footprint = max(length(dpdx.xz), length(dpdy.xz));
    const float far = max(max(abs(p.x), abs(p.y)), 1.0);
    const float step = asfloat(asuint(far) & 0x7f800000u) * (1.0 / 8388608.0);
    const float reach = max(footprint, 4.0 * step);
    const float octaves = float(d.grain_octaves);
    float3 g = float3(0.0, 0.0, 0.0);
    float2 slope = float2(0.0, 0.0);
    float kept = 0.0;
    float size = d.grain_size;
    int2 base = d.grain_base;
    for (uint o = 0u; o < d.grain_octaves; ++o) {
      const float f = ground_fade(reach / size);
      if (f > 0.0 || (full && o == 0u)) {
        const GroundGrain n = ground_gradient_noise3(p, size, base, d.grain_rem, d.seed, 16u + o);
        if (o == 0u) out.grain = n.value.x * (1.0 / (3.0 * k_ground_noise_rms));
        g += n.value * f;
        slope += n.gradient * f;
      }
      kept += f * f;
      size *= 0.5;
      base *= 2;
    }
    // The albedo and roughness channels to k_ground_grain_rms over all the octaves, the height's
    // slope to `grain_normal`: each octave the same share, so the sand is as rough at a millimetre
    // as at a centimetre.
    const float value_scale = k_ground_grain_rms / (k_ground_noise_rms * sqrt(octaves));
    const float slope_scale = d.grain_normal * mask / (k_ground_noise_slope_rms * sqrt(octaves));
    if (d.grain_normal > 0.0 && !full) {
      const float3 n = out.normal;
      out.normal = normalize(n - float3(slope.x, 0.0, slope.y) * (slope_scale * n.y));
      // What the fades took of the grain's slope variance, into the roughness by the ripples'
      // rule: the faded sand is rougher, not flatter.
      const float lost = d.grain_normal * d.grain_normal * mask * mask * (octaves - kept) / octaves;
      if (lost > 0.0) {
        const float alpha = out.roughness * out.roughness;
        out.roughness = sqrt(sqrt(alpha * alpha + lost));
      }
    }
    out.albedo = albedo * (1.0 + d.grain_albedo * mask * g.x * value_scale);
    out.roughness = saturate(out.roughness + d.grain_roughness * mask * g.y * value_scale);
  } else if ((d.flags & k_ground_grain) != 0u) {
    const float footprint = max(length(dpdx.xz), length(dpdy.xz));
    const float coarse = ground_fade(footprint / d.grain_size);
    const float fine = ground_fade(4.0 * footprint / d.grain_size);
    float2 g = float2(0.0, 0.0);
    if (coarse > 0.0 || full) {
      const float2 v = ground_value_noise2(p, d.grain_size, d.grain_base, d.grain_rem, d.seed, 16u);
      out.grain = v.x;
      g += v * (0.65 * coarse);
    }
    if (fine > 0.0) {
      g += ground_value_noise2(p, d.grain_size * 0.25, d.grain_base * 4, d.grain_rem, d.seed, 17u) *
           (0.35 * fine);
    }
    out.albedo = albedo * (1.0 + d.grain_albedo * mask * g.x);
    out.roughness = saturate(out.roughness + d.grain_roughness * mask * g.y);
  }
  if ((d.flags & k_ground_streaks) != 0u) {
    // Where the ground falls away from the wind near the angle of repose, and by the sand share;
    // faded by the footprint across a tongue, and the slope variance that takes into the roughness.
    const float fall_tan = dot(normal.xz, d.wind.xy) / max(normal.y, 1e-4);
    const float weight = mask * smoothstep(d.streak_tan_start, d.streak_tan_full, fall_tan);
    out.streak = weight;
    if (weight > 0.0 && !full) {
      const float footprint = max(length(dpdx.xz), length(dpdy.xz));
      const float fade = ground_fade(footprint / d.streak_width);
      if (fade > 0.0) {
        const float len = length(normal.xz);
        const float2 fall = len > 1e-6 ? normal.xz / len : d.wind.xy;
        const GroundStreak st = ground_streaks(d, p, fall);
        const float strength = weight * fade;
        out.albedo *= 1.0 + d.streak_albedo * strength * st.value;
        out.roughness = saturate(out.roughness + d.streak_roughness * strength * st.value);
        if (d.streak_slope > 0.0) {
          const float3 n = out.normal;
          out.normal = normalize(n - float3(st.gradient.x, 0.0, st.gradient.y) *
                                         (d.streak_slope * strength * n.y));
        }
      }
      const float lost = weight * weight * d.streak_normal * d.streak_normal * (1.0 - fade * fade);
      if (lost > 0.0) {
        const float alpha = out.roughness * out.roughness;
        out.roughness = sqrt(sqrt(alpha * alpha + lost));
      }
    }
  }
  if ((d.flags & k_ground_grainflow) != 0u) {
    // Where the ground falls away from the wind near the angle of repose; faded by the footprint
    // across a lane, the slope variance that takes into the roughness. Relief first: the albedo
    // moves by a percent or two.
    const float fall_tan = dot(normal.xz, d.wind.xy) / max(normal.y, 1e-4);
    const float weight = mask * smoothstep(d.flow_tan_start, d.flow_tan_full, fall_tan);
    out.flow = weight;
    if (weight > 0.0 && !full) {
      const float footprint = max(length(dpdx.xz), length(dpdy.xz));
      // By the footprint against `k_ground_flow_body` lane widths a period: whole at four pixels a
      // period and gone at two (fifth pass; the fourth faded by a whole lane width, and around
      // 15 cm pixels drew more variation than the pixels hold: ground_detail.h has the numbers). A
      // face avalanching faster than a frame can show (a storm, a time-lapse) is smooth: the
      // lanes fade by the episodes' advance a frame over the share, as the ripples do by their
      // travel.
      const float fade = ground_fade(footprint / (k_ground_flow_body * d.flow_width)) *
                         (d.flow_share < 1.0 ? ground_fade(d.flow_clock_step / d.flow_share) : 1.0);
      if (fade > 0.0) {
        const float len = length(normal.xz);
        const float2 fall = len > 1e-6 ? normal.xz / len : d.wind.xy;
        const GroundFlow lanes = ground_grainflow(d, ground_lanes(block), p, fall);
        const float strength = weight * fade;
        out.albedo *= 1.0 + d.flow_albedo * strength * lanes.value;
        const float3 n = out.normal;
        out.normal = normalize(n - float3(lanes.gradient.x, 0.0, lanes.gradient.y) *
                                       (d.flow_slope * d.flow_width * strength * n.y));
      }
      // With episodes, only about `flow_share` of the lanes carry that variance at once.
      const float lost = weight * weight * d.flow_normal * d.flow_normal * (1.0 - fade * fade) *
                         min(d.flow_share, 1.0);
      if (lost > 0.0) {
        const float alpha = out.roughness * out.roughness;
        out.roughness = sqrt(sqrt(alpha * alpha + lost));
      }
    }
  }
  return out;
}

#endif  // ENGINE_GROUND_DETAIL_SLANG
