// **The water's height function** (docs/subsystems/water.md, "The mesh" and "Shading"; the
// grounding, docs/experiments/shore.md, "The wave as height" and "Stage B: waves that read as
// water"; roadmap S100). No bindings and no entry points, like brdf.slang: the pool pass
// (deform.slang's water stage) lifts every vertex of a body with a wind by `water_vertex_lift`, and
// the resolve's water branch (water.slang) shades it with `water_surface_normal`, the gradient of
// the same function, so the normal is the surface's. Mirrored in double by domain/gfx/tests/water_reference.h; change one and
// the other in the same commit.
//
// Offshore, the wind's sea: sixteen waves from the fetch-limited spectrum, each a narrow band — a
// carrier under a travelling complex envelope (`water_wave_add`) — for every body with a wind, open
// or closed, and where a closed body's wind has blown over less water than the table's fetch the
// long waves are not grown yet (`grown`, the field's third channel). A body with no wind keeps the
// material's four sines. Near the shore **the depth governs** the wind's peak wave, a train: it
// shoals by linear theory's coefficient (Green's law in shallow water) until it breaks at H = 0.78 d
// (McCowan 1894), and is then the saturated bore H = 0.78 d to the shore; its crest sharpens as it
// nears breaking; its wavenumber is the dispersion relation's at the local depth; and its phase is
// the **arrival time** of the train at the place (gfx::water_arrival_field, an eikonal solved on the
// depth map's grid at load), so the crests slow and bunch as they come in and neighbouring places
// are in sequence. Over the bank the swash's front (water.slang's ballistic profile, its phase the
// train's arrival at the shoreline) says how thick the sheet is, and the surface goes on under the
// ground past it at the same slope, so the mesh meets the ground along the front and not along a
// triangle's edge.
//
// **What the mesh carries** (water.md, "The mesh"): the lift is given the mesh's own spacing as its
// footprint, so a wave four vertices cannot carry is left out of the displacement and stays in the
// normal, which is evaluated per pixel from the same function.
#ifndef ENGINE_WATER_WAVES_SLANG
#define ENGINE_WATER_WAVES_SLANG

static const float k_water_g = 9.81;
static const float k_water_period_s = 1024.0;  // gfx::k_water_period_s
static const float k_water_two_pi = 6.283185307179586;
// The four waves: wavelength and amplitude as shares of the body's, and the turn from the main
// direction as a share of the chop's ninety degrees.
static const float k_water_length[4] = {1.0, 0.62, 0.38, 0.24};
static const float k_water_amplitude[4] = {1.0, 0.55, 0.32, 0.18};
static const float k_water_turn[4] = {0.0, 0.55, -0.8, 0.35};

// A wave's phase at the clock's zero, in [0, 1): an integer hash of the seed and the wave.
float water_phase(uint seed, uint wave) {
  uint h = seed * 0x9E3779B9u + (wave + 1u) * 0x85EBCA6Bu;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return float(h >> 8) * (1.0 / 16777216.0);
}

// Mirrors gfx::WaterWave (32 bytes).
struct WaterWave {
  float2 direction;
  float k;
  float amplitude;
  float cycles;
  float phase;
  float steepness;
  float grow;  // (5/8)(omega_p / omega)^4: gfx::water_grown_share
};

uint water_hash2(int2 c) {
  uint h = uint(c.x) * 0x8DA6B343u ^ uint(c.y) * 0xD8163841u;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

// Value noise in [0, 1] over cells of a unit, smoothly interpolated.
float water_value_noise(float2 p) {
  const float2 c = floor(p);
  float2 f = p - c;
  f = f * f * (3.0 - 2.0 * f);
  const int2 i = int2(c);
  const float a = float(water_hash2(i) >> 8) * (1.0 / 16777216.0);
  const float b = float(water_hash2(i + int2(1, 0)) >> 8) * (1.0 / 16777216.0);
  const float e = float(water_hash2(i + int2(0, 1)) >> 8) * (1.0 / 16777216.0);
  const float g = float(water_hash2(i + int2(1, 1)) >> 8) * (1.0 / 16777216.0);
  return lerp(lerp(a, b, f.x), lerp(e, g, f.x), f.y);
}

// ---- a wave as a narrow band (gfx::k_water_group_along and its neighbours, water_spectrum.h; S100
// stage B; water.md, "The spectrum") ---------------------------------------------------------------
static const float k_water_group_along = 4.0;        // gfx::k_water_group_along
static const float k_water_group_across = 2.5;       // gfx::k_water_group_across
static const float k_water_envelope_gain = 1.6486932;  // gfx::k_water_envelope_gain
static const float k_water_max_lift = 0.7;           // gfx::k_water_max_lift

// gfx::water_envelope_hash.
uint water_envelope_hash(uint along, uint across, uint salt) {
  uint h = along * 0x8DA6B343u ^ across * 0xD8163841u ^ salt * 0xCB1AB31Fu;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

float2 water_envelope_value(uint h) {
  return float2(float(h & 0xffffu), float(h >> 16)) * (1.0 / 32767.5) - 1.0;
}

// The envelope at (u, v) cells — u the way the wave travels, periodic in `cells`, v along its crest
// — its two components and their derivatives in u and in v.
struct WaterEnvelope {
  float2 e;
  float2 du;
  float2 dv;
};

WaterEnvelope water_envelope(float u, float v, float cells, uint salt) {
  const float cu = floor(u);
  const float cv = floor(v);
  const float fu = u - cu;
  const float fv = v - cv;
  const float u0 = cu - cells * floor(cu / cells);
  const float u1 = u0 + 1.0 >= cells ? 0.0 : u0 + 1.0;
  const uint iu0 = uint(u0);
  const uint iu1 = uint(u1);
  const uint iv0 = uint(int(cv));
  const uint iv1 = iv0 + 1u;
  const float2 a = water_envelope_value(water_envelope_hash(iu0, iv0, salt));
  const float2 b = water_envelope_value(water_envelope_hash(iu1, iv0, salt));
  const float2 c = water_envelope_value(water_envelope_hash(iu0, iv1, salt));
  const float2 d = water_envelope_value(water_envelope_hash(iu1, iv1, salt));
  const float su = fu * fu * (3.0 - 2.0 * fu);
  const float sv = fv * fv * (3.0 - 2.0 * fv);
  const float dsu = 6.0 * fu * (1.0 - fu);
  const float dsv = 6.0 * fv * (1.0 - fv);
  const float2 ab = lerp(a, b, su);
  const float2 cd = lerp(c, d, su);
  WaterEnvelope r;
  r.e = lerp(ab, cd, sv) * k_water_envelope_gain;
  r.du = lerp(b - a, d - c, sv) * (dsu * k_water_envelope_gain);
  r.dv = (cd - ab) * (dsv * k_water_envelope_gain);
  return r;
}

// The waves' sum: the height, its gradient, the Gerstner crests' sharpening of the normal (`lift`),
// and the mean square slope the footprint left out (`dropped`), which the shading gives to the
// surface's roughness so a far water keeps its glitter's width.
struct WaterSum {
  float height;
  float2 grad;
  float lift;
  float dropped;
};

// One wave of a table at body-local p and the clock t: `share` scales it (the shore's calm), and
// `young` is grown^(-4/3) - 1 where the sea is younger than the table's (0: as grown). A wave
// shorter than twice the `footprint` (metres: a pixel's, or the mesh's spacing for the lift) is
// left out, and whole at four times it.
void water_wave_add(WaterWave wave, float2 p, float t, float footprint, float share, float young,
                    inout WaterSum r) {
  if (!(wave.amplitude > 0.0)) return;
  const float lambda = k_water_two_pi / wave.k;
  const float keep = footprint > 0.0 ? clamp(lambda / (2.0 * footprint) - 1.0, 0.0, 1.0) : 1.0;
  const float grown = share * (young > 0.0 ? exp(-wave.grow * young) : 1.0);
  const float slope = wave.amplitude * wave.k * grown;
  r.dropped += 0.5 * slope * slope * (1.0 - keep * keep);
  const float weight = grown * keep;
  if (!(weight > 0.0)) return;
  const float clock = t / k_water_period_s;
  const float along = dot(wave.direction, p) / lambda;
  const float cyc = frac(frac(along) - frac(wave.cycles * clock) + wave.phase);
  const float angle = k_water_two_pi * cyc;
  const float sn = sin(angle);
  const float cs = cos(angle);
  const float cells = max(1.0, floor(wave.cycles / (2.0 * k_water_group_along) + 0.5));
  const float2 across = float2(-wave.direction.y, wave.direction.x);
  const float inv_u = 1.0 / (lambda * k_water_group_along);
  const float inv_v = 1.0 / (lambda * k_water_group_across);
  const WaterEnvelope env = water_envelope(along / k_water_group_along - cells * clock,
                                           dot(across, p) * inv_v, cells, asuint(wave.phase));
  const float carrier = env.e.x * sn + env.e.y * cs;
  const float a = wave.amplitude * weight;
  r.height += a * carrier;
  r.grad += wave.direction * (a * wave.k * (env.e.x * cs - env.e.y * sn)) +
            (wave.direction * ((env.du.x * sn + env.du.y * cs) * inv_u) +
             across * ((env.dv.x * sn + env.dv.y * cs) * inv_v)) *
                a;
  r.lift += wave.steepness * weight * carrier;
}

// The swash's shape (gfx::k_swash_*; water_swash.h).
static const uint k_swash_group = 7u;
static const float k_swash_uprush = 0.4;
static const float k_swash_rundown = 0.2;

float water_swash_group(uint j) {
  const float x = (float(j % k_swash_group) + 0.5) / float(k_swash_group);
  return 0.55 + 0.45 * (0.5 - 0.5 * cos(k_water_two_pi * x));
}

float water_swash_shape(float u) {
  const float q = k_swash_uprush;
  const float s = u < q ? u / (2.0 * q) : 0.5 + (u - q) / (2.0 * (1.0 - q));
  return 4.0 * s * (1.0 - s);
}

// **The sheet's duration is the slope's** (gfx::swash_span; S102; shore-mechanics.md, "The
// swash"): a front slowed by gravity alone climbs E in sqrt(2 E / g) / beta seconds, the
// uprush is `k_swash_uprush` of the sheet's life, so a wave of reach `reach` over a rundown of
// `k_swash_rundown run_up` stays sqrt(2 E / g) / (beta q T) periods on the beach — under one on a
// steep shore, several on a gentle one, where the next bore arrives over the backwash.
static const float k_swash_span_min = 0.25;  // gfx::k_swash_span_min
static const float k_swash_span_max = 5.0;   // gfx::k_swash_span_max
static const uint k_swash_overlap = 5u;      // gfx::k_swash_overlap

float water_swash_span(float reach, float run_up, float slope, float period) {
  const float excursion = max(reach + k_swash_rundown * run_up, 0.0);
  return clamp(sqrt(2.0 * excursion / k_water_g) / (max(slope, 1e-3) * k_swash_uprush * period),
               k_swash_span_min, k_swash_span_max);
}

// The front's height over the still level at `c` cycles of the place's train: the highest of the
// sheets still on the beach (gfx::swash_front). `run_up` is the place's R, `jitter` its reach's
// factor; `uprush` says whether the sheet that leads is still climbing.
float water_swash_front_at(float run_up, float jitter, float slope, float period, float c,
                           out bool uprush) {
  const float k = floor(c);
  const float u = c - k;
  const float z_lo = -k_swash_rundown * run_up;
  float z = z_lo;
  uprush = false;
  for (uint i = 0; i < k_swash_overlap; ++i) {
    const uint j = uint(int(k) - int(i) + int(k_swash_group) * 64) % k_swash_group;
    const float a = run_up * water_swash_group(j) * jitter;
    const float v = (u + float(i)) / water_swash_span(a, run_up, slope, period);
    if (!(v < 1.0)) continue;
    const float here = z_lo + (a - z_lo) * water_swash_shape(v);
    if (here > z) {
      z = here;
      uprush = v < k_swash_uprush;
    }
  }
  return z;
}

// ---- the body's surface record (gfx::WaterSurface, water_swash.h) --------------------------------
static const float k_water_breaking_gamma = 0.78;  // gfx::k_water_breaking_gamma (McCowan 1894)
static const float k_water_bank_park = 0.05;       // gfx::k_water_bank_park_m
static const float k_water_mesh_inside = 0.85;     // gfx::k_water_mesh_inside
static const uint k_water_surface_spectrum = 1u;   // gfx::k_water_surface_spectrum
static const uint k_water_surface_open = 2u;       // gfx::k_water_surface_open
static const float k_water_train_deep_kd = 1.0;    // gfx::k_water_train_deep_kd
static const float k_water_train_shallow_kd = 0.35;
static const uint k_water_surface_waves = 16u;     // gfx::k_water_waves

// Mirrors gfx::WaterSurface (648 bytes).
struct WaterSurface {
  // Two float4 per texel, rows along +z: (signed depth, arrival s, grown, shore slope) and (the
  // second train's arrival s, incident, kept, setup m) — the energy-flux field's (water_flux.h).
  uint64_t field;
  float2 origin;
  float texel_m;
  uint size;
  float2 wind;
  float height_m;
  float period_s;
  float k0;
  float run_up_m;
  float slope;
  float tide_m;
  float fade_m;
  uint flags;
  float amplitude;
  float wavelength;
  float chop;
  uint seed;
  float2 direction;
  float along_m;
  float mesh_far_m;
  float mesh_near_m;
  float mesh_grade;
  float swell;        // the wind sea's peak k0 when the train is a swell's (S102); 0: the wind's
  float reach_scale;  // 1.1 sqrt(H0 L0): the run-up of a place from its slope (S102); 0: run_up_m
  float reach_cap;
  // S102, second pass (water_flux.h, `WaterCross`): the first train's own deep-water height (0:
  // `height_m`), and the second train's height, period and deep-water wavenumber (0: none).
  float train_m;
  float cross_m;
  float cross_period_s;
  float cross_k0;
  float pad;
  WaterWave waves[16];
};

// The field at body-local p: the depth, the arrival, the grown share and the foreshore slope of the
// place's own stretch of shore (S102; 0 in a field that carries none), and the first two's
// gradients (zero along an axis the point is clamped on). **Read smoothly** (S100 stage B; shore.md,
// "The shallows"): a quadratic B-spline over the nine texels round the point, which is C1, where a
// bilinear read's gradient jumps at every texel's border and a crest — a level line of the arrival
// — turned a corner there. Since S102's second pass a texel is two float4 and the read gives the
// energy-flux field too: the second train's arrival and its gradient, the share of the unrefracted
// height that reaches the place (`incident`), the share of that breaking left (`kept`) — each 1
// in a field that carries none, whose texels hold 0 — and the setup, metres.
struct WaterField {
  float depth;
  float arrival;
  float grown;
  float slope;
  float2 depth_grad;
  float2 arrival_grad;
  float arrival2;
  float2 arrival2_grad;
  float incident;
  float kept;
  float setup;
};

WaterField water_field_at(WaterSurface* s, float2 p) {
  WaterField f = (WaterField)0;
  f.grown = 1.0;
  f.incident = 1.0;
  f.kept = 1.0;
  const uint n = s->size;
  if (s->field == 0 || n < 2) return f;
  const float last = float(n - 1);
  const float2 g = (p - s->origin) / s->texel_m;
  const float2 c = clamp(g, float2(0.0, 0.0), float2(last, last));
  const float2 mid = floor(c + 0.5);
  const float2 fr = c - mid;  // [-0.5, 0.5]
  const int ix = int(mid.x);
  const int iz = int(mid.y);
  const int hi = int(n) - 1;
  const float3 wx = float3(0.5 * (0.5 - fr.x) * (0.5 - fr.x), 0.75 - fr.x * fr.x,
                           0.5 * (0.5 + fr.x) * (0.5 + fr.x));
  const float3 wz = float3(0.5 * (0.5 - fr.y) * (0.5 - fr.y), 0.75 - fr.y * fr.y,
                           0.5 * (0.5 + fr.y) * (0.5 + fr.y));
  const float3 dwx = float3(fr.x - 0.5, -2.0 * fr.x, fr.x + 0.5);
  const float3 dwz = float3(fr.y - 0.5, -2.0 * fr.y, fr.y + 0.5);
  float4* texels = (float4*)s->field;
  float4 v = float4(0.0, 0.0, 0.0, 0.0);
  float4 e = float4(0.0, 0.0, 0.0, 0.0);
  float2 dx = float2(0.0, 0.0);
  float2 dz = float2(0.0, 0.0);
  float2 d2 = float2(0.0, 0.0);
  for (int j = 0; j < 3; ++j) {
    const uint row = uint(clamp(iz + j - 1, 0, hi)) * n;
    for (int i = 0; i < 3; ++i) {
      const uint at = (row + uint(clamp(ix + i - 1, 0, hi))) * 2u;
      const float4 t = texels[at];
      const float4 t2 = texels[at + 1u];
      v += t * (wx[i] * wz[j]);
      e += t2 * (wx[i] * wz[j]);
      dx += t.xy * (dwx[i] * wz[j]);
      dz += t.xy * (wx[i] * dwz[j]);
      d2 += t2.x * float2(dwx[i] * wz[j], wx[i] * dwz[j]);
    }
  }
  const float inside_x = g.x >= 0.0 && g.x <= last ? 1.0 / s->texel_m : 0.0;
  const float inside_z = g.y >= 0.0 && g.y <= last ? 1.0 / s->texel_m : 0.0;
  dx *= inside_x;
  dz *= inside_z;
  f.depth = v.x;
  f.arrival = v.y;
  f.grown = clamp(v.z, 0.0, 1.0);
  f.slope = v.w;
  f.depth_grad = float2(dx.x, dz.x);
  f.arrival_grad = float2(dx.y, dz.y);
  f.arrival2 = e.x;
  f.arrival2_grad = float2(d2.x * inside_x, d2.y * inside_z);
  f.incident = e.y > 0.0 ? e.y : 1.0;
  f.kept = e.z > 0.0 ? e.z : 1.0;
  f.setup = e.w;
  return f;
}

// **A place's own shore** (S102; gfx::swash_local_run_up): the foreshore slope of its stretch of
// the waterline — the field's, or the body's one number where the field carries none — and the
// run-up that slope gives the wave that reaches it: Stockdon et al.'s formula, no higher than the
// steep shore's ceiling. **Since the second pass** the wave is the place's own — the formula's
// sqrt(H0 L0) with H0 the height refraction left that stretch, `incident` of the body's (the
// field's; coastal engineering's H0' = Kr H0), so a point's swash climbs higher than its lee's —
// and the swash stands on the **setup** its surf raises: `water_place_level`, the field's at the
// place's shore, never under the still level and kept under half the run-up (theory's setup
// through a saturated surf is larger than the share of the run-up Stockdon's fit gives it on a
// gentle beach; the top stays the fit's), and `water_place_run_up` is what the swash climbs over
// that level.
float water_place_slope(WaterSurface* s, WaterField f) { return f.slope > 0.0 ? f.slope : s->slope; }

float water_place_reach(WaterSurface* s, WaterField f) {
  if (!(s->reach_scale > 0.0) || !(f.slope > 0.0)) return s->run_up_m;
  return min(s->reach_scale * sqrt(f.incident) *
                 (0.35 * f.slope + 0.5 * sqrt(0.563 * f.slope * f.slope + 0.004)),
             s->reach_cap);
}

float water_place_level(WaterSurface* s, WaterField f) {
  return clamp(f.setup, 0.0, 0.5 * water_place_reach(s, f));
}

float water_place_run_up(WaterSurface* s, WaterField f) {
  return water_place_reach(s, f) - water_place_level(s, f);
}

// **The dispersion relation at depth** (gfx::water_wavenumber): Fenton and McKee's explicit k d
// and two Newton steps on k d tanh(k d) = k0 d.
float water_wavenumber(float k0, float d) {
  const float depth = max(d, 1e-3);
  const float x0 = k0 * depth;
  float y = x0 * pow(1.0 / tanh(pow(x0, 0.75)), 2.0 / 3.0);
  for (uint i = 0; i < 2; ++i) {
    const float th = tanh(y);
    y -= (y * th - x0) / (th + y * (1.0 - th * th));
  }
  return y / depth;
}

// Linear shoaling (gfx::water_shoaling).
float water_shoaling(float k, float d) {
  const float kd = k * max(d, 1e-3);
  const float n = 2.0 * kd > 30.0 ? 0.5 : 0.5 * (1.0 + 2.0 * kd / sinh(2.0 * kd));
  return sqrt(0.5 / (n * tanh(kd)));
}

// The train's shape at phase theta (radians; the crest at 0) and depth d: H (((1 + cos)/2)^m less its
// mean), m = 1 + 2 b with b the share of the breaking height reached, so the crest sharpens and the
// trough flattens as the wave nears breaking; the mean of ((1 + cos)/2)^m is
// Gamma(m + 1/2) / (sqrt(pi) Gamma(m + 1)), within a percent of 1 / sqrt(pi (m + 1/4)).
// `dtheta` is its derivative in theta. `share` (S102, second pass) is the train's share of the
// breaking height where two trains come ashore together — its deep-water height over the two's
// sum, so the crest the two make where they coincide is the one that stands gamma d — and 1 for a
// train alone.
float water_train_shape(float h0, float k0, float d, float theta, float share, out float dtheta) {
  dtheta = 0.0;
  if (!(d > 0.0) || !(h0 > 0.0)) return 0.0;
  const float shoaled = h0 * water_shoaling(water_wavenumber(k0, d), d);
  const float limit = k_water_breaking_gamma * d * share;
  const float height = min(shoaled, limit);
  const float b = clamp(shoaled / limit, 0.0, 1.0);
  const float m = 1.0 + 2.0 * b;
  const float base = max(0.5 + 0.5 * cos(theta), 0.0);
  const float s = pow(base, m);
  dtheta = height * m * (base > 0.0 ? s / base : 0.0) * (-0.5 * sin(theta));
  return height * (s - 1.0 / sqrt(3.14159265358979 * (m + 0.25)));
}

// The train's cycles at the clock t (seconds in [0, 1024)) and arrival tau: (t - tau) / T, each
// term reduced modulo the group of seven waves, so the angle stays small and the group's envelope
// and the wave's phase (both periodic in seven) never jump.
float water_train_cycles(WaterSurface* s, float t, float arrival) {
  const float ct = t / s->period_s;
  const float ca = arrival / s->period_s;
  const float group = float(k_swash_group);
  return (ct - group * floor(ct / group)) - (ca - group * floor(ca / group));
}

// The group's envelope at cycles c: wave k's crest (c = k) carries the swash's group shape for k.
float water_train_envelope(float c, out float dc) {
  const float x = k_water_two_pi * (c + 0.5) / float(k_swash_group);
  dc = 0.45 * 0.5 * sin(x) * (k_water_two_pi / float(k_swash_group));
  return 0.55 + 0.45 * (0.5 - 0.5 * cos(x));
}

// **The lee** (S100 stage B): the train's height and the swash's reach where the wind has blown
// over `grown` of the body's fetch — the fetch-limited growth law's H ~ F^(1/2) — so a closed body's
// waves are whole on the shore its wind blows to and nothing on the one it blows from. An open
// body's field holds 1. Its own gradient is left out of the slope.
float water_shelter(WaterField f) { return sqrt(clamp(f.grown, 0.0, 1.0)); }

// The wavenumber the wind's table calms by towards the shore: the train's own k0, or with a swell
// (S102) the wind sea's peak, which `swell` carries.
float water_table_k0(WaterSurface* s) { return s->swell > 0.0 ? s->swell : s->k0; }

// The waves' weight against the train's: 1 where k0 d >= 1, 0 under 0.35, smoothstep between.
float water_offshore_weight(float k0, float d, out float dd) {
  const float lo = k_water_train_shallow_kd / max(k0, 1e-6);
  const float hi = k_water_train_deep_kd / max(k0, 1e-6);
  const float x = clamp((d - lo) / (hi - lo), 0.0, 1.0);
  dd = d > lo && d < hi ? 6.0 * x * (1.0 - x) / (hi - lo) : 0.0;
  return x * x * (3.0 - 2.0 * x);
}

// The height of the water at body-local p over the still level (the tide's not included), its
// gradient, the Gerstner crests' sharpening of the normal (`lift`) and the slope variance the
// footprint left out (`dropped`). `footprint` (metres) is the pixel's; for the pool's lift it is 0
// and `mesh` is the mesh's spacing (along a ring, between rings) at the vertex, which the waves
// and the train are held against instead (0, 0: a pixel).
struct WaterHeight {
  float height;
  float2 grad;
  float lift;
  float dropped;
};

WaterHeight water_height(WaterSurface* s, float2 p, WaterField f, float t, float footprint,
                         float2 mesh) {
  WaterHeight r = (WaterHeight)0;
  const float d = f.depth;
  float dw = 0.0;
  const float w_off =
      s->k0 > 0.0 && s->height_m > 0.0 ? water_offshore_weight(water_table_k0(s), d, dw) : 1.0;
  // The train's own weight: what the table gives up to it in the shallows — or, a swell's (S102),
  // whole wherever there is water, beside the wind's table: a swell is there offshore too.
  const bool swell = s->swell > 0.0;
  const float w_train = swell ? 1.0 : 1.0 - w_off;
  // What the energy flux left of the unrefracted, unbroken height here, and the first train's
  // share of the breaking height beside a second (S102, second pass).
  const float flux = f.incident * f.kept;
  const float first_m = s->train_m > 0.0 ? s->train_m : s->height_m;
  const float share = s->cross_m > 0.0 ? first_m / (first_m + s->cross_m) : 1.0;
  // Offshore: the spectrum's sixteen, or the four sines.
  WaterSum off = (WaterSum)0;
  const float wave_footprint = max(footprint, mesh.x);
  if (w_off > 0.0 || dw != 0.0) {
    if ((s->flags & k_water_surface_spectrum) != 0) {
      const float young = pow(max(f.grown, 1e-4), -4.0 / 3.0) - 1.0;
      for (uint i = 0; i < k_water_surface_waves; ++i)
        water_wave_add(s->waves[i], p, t, wave_footprint, 1.0, young, off);
    } else {
      for (uint i = 0; i < 4; ++i) {
        const float lambda = s->wavelength * k_water_length[i];
        const float a = s->amplitude * k_water_amplitude[i];
        // A sine with no height or no length adds nothing — and must not add 0 * NaN: a surface
        // with no waves (wavelength 0) is the rest pose to the bit (deform_tests.cpp, 2026-11-09).
        if (!(a > 0.0) || !(lambda > 0.0)) continue;
        const float turn = s->chop * k_water_turn[i] * (0.5 * 3.14159265358979);
        const float c = cos(turn);
        const float sn = sin(turn);
        const float2 dir = float2(s->direction.x * c - s->direction.y * sn,
                                  s->direction.x * sn + s->direction.y * c);
        const float k = k_water_two_pi / lambda;
        const float cycles = floor(sqrt(k_water_g * k) * (k_water_period_s / k_water_two_pi) + 0.5);
        const float along = dot(dir, p) / lambda;
        const float travel = frac(cycles * (t / k_water_period_s));
        const float cyc = frac(frac(along) - travel + water_phase(s->seed, i));
        const float keep = wave_footprint > 0.0
                               ? clamp(lambda / (2.0 * wave_footprint) - 1.0, 0.0, 1.0)
                               : 1.0;
        off.height += a * keep * sin(k_water_two_pi * cyc);
        off.grad += dir * (a * k * keep * cos(k_water_two_pi * cyc));
        off.dropped += 0.5 * a * k * a * k * (1.0 - keep * keep);
      }
    }
  }
  // Near the shore: the train.
  float train = 0.0;
  float2 train_grad = float2(0.0, 0.0);
  if (w_train > 0.0 && d > 0.0 && s->height_m > 0.0 && s->period_s > 0.0) {
    const float c = water_train_cycles(s, t, f.arrival);
    const float theta = k_water_two_pi * frac(c);
    const float local_k = water_wavenumber(s->k0, d);
    // grad c = -grad tau / T: the local wavenumber, along the way the train travels.
    const float2 grad_c = -f.arrival_grad / s->period_s;
    // What the train is held against: a pixel's footprint, or for the lift the mesh's spacing as
    // the train crosses it — the rings' along the ray from the body's centre, the ring's own
    // across it — so a crest that runs along the rings needs only the rings close together.
    float held = footprint;
    if (mesh.x > 0.0) {
      const float lp = length(p);
      const float lc = length(grad_c);
      float along_ray = 1.0;
      if (lp > 1e-3 && lc > 1e-9) along_ray = abs(dot(p, grad_c)) / (lp * lc);
      held = max(mesh.y * along_ray, mesh.x * sqrt(max(1.0 - along_ray * along_ray, 0.0)));
    }
    const float keep =
        held > 0.0 ? clamp(k_water_two_pi / local_k / (2.0 * held) - 1.0, 0.0, 1.0) : 1.0;
    float d_env = 0.0;
    const float env = water_train_envelope(c, d_env);
    const float shelter = water_shelter(f);
    float d_theta = 0.0;
    // **The height the flux left the place** (S102, second pass; water_flux.h): the train's own
    // deep-water height times what refraction brought here and what breaking kept of it, shoaled
    // by the closed form and capped at the train's share of gamma d.
    const float h1 = (s->train_m > 0.0 ? s->train_m : s->height_m) * flux;
    const float shape = water_train_shape(h1, s->k0, d, theta, share, d_theta);
    // The shape's change with depth, by a central difference over a millimetre's part of it.
    const float eps = 1e-3 + 1e-3 * d;
    float unused = 0.0;
    const float d_depth = (water_train_shape(h1, s->k0, d + eps, theta, share, unused) -
                           water_train_shape(h1, s->k0, max(d - eps, 1e-4), theta, share, unused)) /
                          (d + eps - max(d - eps, 1e-4));
    train = env * shelter * shape;
    train_grad = shelter * keep *
                 (env * (d_depth * f.depth_grad + d_theta * k_water_two_pi * grad_c) +
                  shape * d_env * grad_c);
    train *= keep;
  }
  // **The second train** (S102, second pass; water.md, "The surf"): where the table gives way to
  // the shallows the sea is two trains crossing, not one ruled one — its phase its own arrival's,
  // its period a whole number of waves in the first's group of seven, its height its share of the
  // limit. Its change with depth is left out of the gradient (the phase's carries it).
  float second = 0.0;
  float2 second_grad = float2(0.0, 0.0);
  const float w_cross = 1.0 - w_off;
  if (w_cross > 0.0 && d > 0.0 && s->cross_m > 0.0 && s->cross_period_s > 0.0) {
    const float c2 = frac(frac(t / s->cross_period_s) - frac(f.arrival2 / s->cross_period_s));
    const float2 grad_c2 = -f.arrival2_grad / s->cross_period_s;
    float held = footprint;
    if (mesh.x > 0.0) {
      const float lp = length(p);
      const float lc = length(grad_c2);
      float along_ray = 1.0;
      if (lp > 1e-3 && lc > 1e-9) along_ray = abs(dot(p, grad_c2)) / (lp * lc);
      held = max(mesh.y * along_ray, mesh.x * sqrt(max(1.0 - along_ray * along_ray, 0.0)));
    }
    // What is held against a spacing is kept from twice as long a wave as the first train's is
    // (whole at eight spacings a wavelength, none at four): two crossing trains on a mesh that
    // carries each at four spans a wavelength stand up as facets — dark shards in the surf from
    // above, where the water's colour goes with the faceted surface's depth (the first flight of
    // this pass) — so the mesh takes the second train only where it carries it smoothly, and the
    // normal, which is per pixel, has all of it.
    const float keep =
        held > 0.0
            ? clamp(k_water_two_pi / water_wavenumber(s->cross_k0, d) / (4.0 * held) - 1.0, 0.0, 1.0)
            : 1.0;
    float d_theta = 0.0;
    const float shape = water_train_shape(s->cross_m * flux, s->cross_k0, d, k_water_two_pi * c2,
                                          1.0 - share, d_theta);
    const float shelter = water_shelter(f);
    second = keep * shelter * shape;
    second_grad = (keep * shelter * d_theta * k_water_two_pi) * grad_c2;
  }
  r.height = w_off * off.height + w_train * train + w_cross * second;
  r.grad = w_off * off.grad + w_train * train_grad + w_cross * second_grad +
           (off.height - (swell ? 0.0 : train) - second) * dw * f.depth_grad;
  r.lift = w_off * off.lift;
  r.dropped = w_off * w_off * off.dropped;
  return r;
}

// An open body's tide at the frame's phase of M2, metres above the level.
float water_surface_tide(WaterSurface* s, float tide_phase) {
  return s->tide_m > 0.0 ? s->tide_m * sin(k_water_two_pi * tide_phase) : 0.0;
}

// The along-shore variation of the swash's reach, [0.85, 1.15]: smooth over `along_m` (about two
// peak wavelengths, the crest's length under a spread sea), so the front is one connected line.
// `lobes` (a pixel's; S100 stage B) adds what a mesh's vertices cannot carry: the front's lobes, a
// tenth of the reach either way over about half the swash's width (the run-up over the slope, 0.5
// to 6 m) and half that again at a quarter of the scale, standing where they are from wave to wave
// as a beach's cusps do — so the front is scalloped and its lip ragged, never a ruled line.
float water_reach_variation(WaterSurface* s, float2 p, bool lobes) {
  float v = 0.85 + 0.3 * water_value_noise(p / max(s->along_m, 1e-3) + float2(17.3, 5.1));
  if (lobes) {
    const float lobe = clamp(0.6 * s->run_up_m / max(s->slope, 1e-3), 0.5, 6.0);
    v *= 1.0 + 0.2 * (water_value_noise(p / lobe + float2(3.1, 9.7)) - 0.5) +
         0.1 * (water_value_noise(p * (3.7 / lobe) + float2(11.9, 2.3)) - 0.5);
  }
  return v;
}

// The swash's front above the still (and tide's) level at p and t: the ballistic profile of the
// wave whose crest reached the shoreline last, its reach R times the group's shape for that wave
// times the along-shore variation times the lee's shelter. `uprush` says which half it is in.
float water_surface_front(WaterSurface* s, float2 p, WaterField f, float t, out bool uprush) {
  uprush = false;
  if (!(s->run_up_m > 0.0) || !(s->period_s > 0.0)) return 0.0;
  // The place's own run-up and slope (S102): the sheets that stand on this stretch of beach.
  // **The mesh's sheet ends inside every pixel's front** (`k_water_mesh_inside`): a pixel's reach
  // has the lobes a vertex cannot carry, a tenth and a twentieth either way, and where the mesh's
  // smooth front stood past a pixel's lobed one the water pixels between were bared — the bed drawn
  // through the water's branch, which is not the ground's own picture — in a strip with the mesh's
  // straight edges, up to 1.4 m wide on a 1:30 beach. Held 0.85 of the way, the mesh's edge is
  // under the sheet everywhere and what lies between it and the front is the ground's own pixels,
  // filmed. It showed once the sheets overlapped and their edge stood still to be looked at.
  return water_swash_front_at(water_place_run_up(s, f),
                              water_reach_variation(s, p, false) * water_shelter(f) *
                                  k_water_mesh_inside,
                              water_place_slope(s, f), s->period_s,
                              water_train_cycles(s, t, f.arrival), uprush);
}

// The sheet's surface over ground `g` metres above the instant's still level, where the front
// stands `front` above it: the ground plus half the front's height over it, up to a quarter of R
// and 2 cm — and **past the front the same slope on under the ground**, down to the park's 5 cm, so
// the surface crosses the ground along the front itself (S100 stage B: stage A parked an uncovered
// vertex 5 cm under at once, and the triangle between it and a covered one stood up as a kerb with
// a straight edge). Never higher than the front can reach, so a vertex under a cliff stands no wall
// up. `run_up` is the place's (S102).
float water_sheet_surface(float run_up, float g, float front) {
  const float y = front - g;
  const float sheet = y > 0.0 ? min(0.5 * y, 0.25 * run_up + 0.02) : max(0.5 * y, -k_water_bank_park);
  return min(g, 1.15 * run_up + 0.1) + sheet;
}

// The mesh's spacing at a vertex `edge` metres in from the outline: (along a ring, between rings).
float2 water_mesh_spacing(WaterSurface* s, float edge) {
  return float2(s->mesh_far_m, min(s->mesh_near_m + s->mesh_grade * max(edge, 0.0), s->mesh_far_m));
}

// **An open body's fade at its outline**: its displacement goes to nothing over `fade_m` in from
// the outline, so the mesh's edge meets the flat sea past it — **where the outline stands in deep
// water** (S102). Until then the fade was by the edge's distance alone, and an outline drawn along
// a coast, as a sea's near shore is, took the height out of the surf where it is seen from the
// beach: half of it eight metres off the waterline. In the shallows, where the train has taken
// over from the table, there is no sea past the outline to meet, and the wave keeps its height.
float water_outline_fade(WaterSurface* s, WaterField f, float edge) {
  if ((s->flags & k_water_surface_open) == 0 || !(s->fade_m > 0.0)) return 1.0;
  float unused = 0.0;
  const float deep = s->field != 0 && s->k0 > 0.0 && s->height_m > 0.0
                         ? water_offshore_weight(water_table_k0(s), f.depth, unused)
                         : 1.0;
  return 1.0 - deep * (1.0 - smoothstep(0.0, s->fade_m, edge));
}

// **A vertex's lift** (the pool's water stage): how far above its rest place, the still level, a
// vertex at body-local p stands at the clock t, `edge` its distance in from the outline (UV.x).
// Water deeper than the swash's band takes the waves the mesh can carry and the tide, faded in from
// an open body's outline over `fade_m` so its edge meets the flat sea past it; over the bank (and
// where the backwash bares the bottom) the vertex is the sheet's surface (`water_sheet_surface`).
// The two blend over the band. A body with no field is its waves alone.
float water_vertex_lift(WaterSurface* s, float2 p, float edge, float t, float tide_phase,
                        out float2 grad) {
  grad = float2(0.0, 0.0);
  const WaterField f = water_field_at(s, p);
  const float tide = water_surface_tide(s, tide_phase);
  const float fade = water_outline_fade(s, f, edge);
  const WaterHeight w = water_height(s, p, f, t, 0.0, water_mesh_spacing(s, edge));
  // The mean level the waves stand on (S102, second pass): the field's setup, a set-down of a
  // centimetre outside the surf and a rise through it; over the bank its shore's.
  const float water = (tide + f.setup + w.height) * fade;
  if (s->field == 0) {
    grad = w.grad * fade;
    return water;
  }
  const float level = tide + water_place_level(s, f);
  const float d = f.depth + level;
  const float run_up = water_place_run_up(s, f);
  const float band = 0.2 * run_up + 0.05;
  if (d >= band) {
    grad = w.grad * fade;
    return water;
  }
  bool uprush = false;
  const float front = water_surface_front(s, p, f, t, uprush);
  const float bank = level + water_sheet_surface(run_up, -d, front);
  const float share = 1.0 - smoothstep(0.0, band, d);
  grad = w.grad * fade * (1.0 - share);
  return lerp(water, bank, share);
}

#endif  // ENGINE_WATER_WAVES_SLANG
