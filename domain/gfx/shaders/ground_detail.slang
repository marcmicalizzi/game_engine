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
static const float k_ground_flow_rms = 0.142;
static const float k_ground_flow_slope_rms = 0.515;
// Streak kernels per lattice cell (gfx::k_ground_streak_impulses).
static const uint k_ground_streak_impulses = 3u;
// gfx::k_ground_noise_rms, k_ground_noise_slope_rms, k_ground_grain_rms.
static const float k_ground_noise_rms = 0.182;
static const float k_ground_noise_slope_rms = 0.743;
static const float k_ground_grain_rms = 0.28;
// Kernels per lattice cell (gfx::k_ground_impulses).
static const uint k_ground_impulses = 2u;
static const float k_ground_two_pi = 6.28318530717958647692;

// Mirrors gfx::GroundDetailParams (224 bytes).
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
  float flow_cell;        // metres, the world lattice's cell
  float flow_width;       // metres, a lane
  float flow_slope;       // the normal's lean per unit of the lanes' derivative
  float flow_normal;      // the root-mean-square slope that is, for the roughness transfer
  float flow_albedo;      // the albedo's share per unit of the lanes' value
  float flow_widening;    // per metre down the fall line
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
};

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
GroundRipple ground_ripple(GroundDetail d, float2 p, float asymmetry, float scale, float2 w,
                           float spread, float travel) {
  const float inv_cell = 1.0 / d.cell;
  const float2 q = p * inv_cell;
  const int2 base = int2(floor(q));
  const float2 across = float2(-w.y, w.x);
  const float k = k_ground_two_pi / (d.wavelength * scale);
  const float shift = k * travel;
  const float inv_r2 = inv_cell * inv_cell;
  // The complex sum S and its two partial derivatives, each a complex number (re, im).
  float2 s = float2(0.0, 0.0);
  float2 sx = float2(0.0, 0.0);
  float2 sz = float2(0.0, 0.0);
  for (int j = -1; j <= 1; ++j) {
    for (int i = -1; i <= 1; ++i) {
      const int2 c = base + int2(i, j);
      for (uint n = 0u; n < k_ground_impulses; ++n) {
        uint h = ground_hash(c.x, c.y, d.seed, n);
        const float ux = ground_unit(h);
        h = ground_pcg(h);
        const float uz = ground_unit(h);
        // The point relative to the kernel's centre, in cells and then in metres: the cell's index
        // and the point's are both near the world's coordinates over the cell, so they are
        // subtracted before anything is scaled.
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

// Two channels of value noise at `p`, on a lattice of `size`, each in [-1, 1]. The lattice is
// counted in tiles of 4,096 cells so that the fraction inside a cell keeps its precision thousands
// of metres from the origin (a 5 mm cell 3 km out is 600,000 cells, where a float has four bits
// left under the point).
float2 ground_lattice2(int2 i, uint seed, uint stream) {
  const uint h = ground_hash(i.x, i.y, seed, stream);
  return float2(float(h & 0xffffu), float(h >> 16u)) * (2.0 / 65535.0) - 1.0;
}

float2 ground_value_noise2(float2 p, float size, uint seed, uint stream) {
  const float tile = size * 4096.0;
  const float2 t = floor(p / tile);
  const float2 q = (p - t * tile) / size;
  const float2 f0 = floor(q);
  const float2 f = q - f0;
  const int2 i = int2(t) * 4096 + int2(f0);
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
// lattice to see: it is zero at every lattice point and its extremes fall between them. The
// lattice is counted in tiles of 4,096 cells as the value noise's is.
struct GroundGrain {
  float3 value;
  float2 gradient;
};

float2 ground_grain_gradient(uint h, uint channel) {
  const uint bits = h >> (10u * channel);
  return (float2(float(bits & 31u), float((bits >> 5u) & 31u)) - 15.5) * (1.0 / 15.5);
}

GroundGrain ground_gradient_noise3(float2 p, float size, uint seed, uint stream) {
  const float tile = size * 4096.0;
  const float2 t = floor(p / tile);
  const float2 q = (p - t * tile) / size;
  const float2 f0 = floor(q);
  const float2 f = q - f0;
  const int2 i = int2(t) * 4096 + int2(f0);
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
  const float2 q = p * inv_cell;
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
      for (uint n = 0u; n < k_ground_streak_impulses; ++n) {
        uint h = ground_hash(c.x, c.y, d.seed, 32u + n);
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
  const GroundGrain a = ground_gradient_noise3(p, d.patch_size, d.seed, 64u);
  const GroundGrain b = ground_gradient_noise3(p, d.patch_size * 0.5, d.seed, 65u);
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

// One direction's lanes at `p`: the noise of `dot(p, across) / width`, its value and gradient.
GroundFlow ground_lane_set(GroundDetail d, float2 p, uint k) {
  const float angle = float(k) * (k_ground_two_pi / float(k_ground_flow_directions));
  const float2 across = float2(-sin(angle), cos(angle));
  const float s = dot(p, across) / d.flow_width;
  const float m = floor(s);
  const float f = s - m;
  const float u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  const float du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
  float g[2];
  [unroll]
  for (uint e = 0u; e < 2u; ++e) {
    uint h = ground_hash(int(m) + int(e), int(k), d.seed, 48u);
    g[e] = ground_unit(h) * 2.0 - 1.0;
    if (d.flow_share < 1.0) {
      h = ground_pcg(h ^ 0x68E31DA4u);
      g[e] *= ground_episode(frac(d.flow_clock + ground_unit(h)), d.flow_share);
    }
  }
  GroundFlow out;
  out.value = g[0] * f * (1.0 - u) + g[1] * (f - 1.0) * u;
  out.gradient = across * ((g[0] * ((1.0 - u) - f * du) + g[1] * (u + (f - 1.0) * du)) /
                           d.flow_width);
  return out;
}

GroundFlow ground_grainflow(GroundDetail d, float2 p, float2 fall) {
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
  const GroundFlow a = ground_lane_set(d, p, i0);
  const GroundFlow b = ground_lane_set(d, p, i1);
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

// The detail at a surface point: `position` in world space, `normal` the shading normal it
// perturbs (unit; its y is the ground's slope the ripples fade by), `dpdx` and `dpdy` the point's
// world-space change per pixel along the screen's two axes (zero for a caller with no footprint,
// which then draws the unfiltered function), `mask` the material's sand share. `full` is the debug
// view's: the ripple height at the scene's own asymmetry and the grain's coarse octave even where
// the fades leave nothing, and no normal (the view draws none).
GroundShading ground_detail_shade(GroundDetail d, float3 position, float3 normal, float3 dpdx,
                                  float3 dpdy, float mask, float3 albedo, float roughness,
                                  bool full) {
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
  const float2 p = position.xz;
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
    // 2^-23 (its exponent's bits, exactly), the resolve's reconstruction is off by a few such steps,
    // so four of them count as footprint and an octave is gone where its cell is under eight
    // (docs/experiments/sand-detail-second-pass-2026-09-29.md measures what an octave loses there).
    const float footprint = max(length(dpdx.xz), length(dpdy.xz));
    const float far = max(max(abs(p.x), abs(p.y)), 1.0);
    const float step = asfloat(asuint(far) & 0x7f800000u) * (1.0 / 8388608.0);
    const float reach = max(footprint, 4.0 * step);
    const float octaves = float(d.grain_octaves);
    float3 g = float3(0.0, 0.0, 0.0);
    float2 slope = float2(0.0, 0.0);
    float kept = 0.0;
    float size = d.grain_size;
    for (uint o = 0u; o < d.grain_octaves; ++o) {
      const float f = ground_fade(reach / size);
      if (f > 0.0 || (full && o == 0u)) {
        const GroundGrain n = ground_gradient_noise3(p, size, d.seed, 16u + o);
        if (o == 0u) out.grain = n.value.x * (1.0 / (3.0 * k_ground_noise_rms));
        g += n.value * f;
        slope += n.gradient * f;
      }
      kept += f * f;
      size *= 0.5;
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
      const float2 v = ground_value_noise2(p, d.grain_size, d.seed, 16u);
      out.grain = v.x;
      g += v * (0.65 * coarse);
    }
    if (fine > 0.0) g += ground_value_noise2(p, d.grain_size * 0.25, d.seed, 17u) * (0.35 * fine);
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
      // A face avalanching faster than a frame can show (a storm, a time-lapse) is smooth: the lanes
      // fade by the episodes' advance a frame over the share, as the ripples do by their travel.
      const float fade = ground_fade(footprint / d.flow_width) *
                         (d.flow_share < 1.0 ? ground_fade(d.flow_clock_step / d.flow_share) : 1.0);
      if (fade > 0.0) {
        const float len = length(normal.xz);
        const float2 fall = len > 1e-6 ? normal.xz / len : d.wind.xy;
        const GroundFlow lanes = ground_grainflow(d, p, fall);
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
