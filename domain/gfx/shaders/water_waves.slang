// **The water's height function** (docs/subsystems/water.md, "The mesh" and "Shading"; the
// grounding, docs/experiments/shore.md, "The wave as height"; roadmap S100). No bindings and no entry
// points, like brdf.slang: the pool pass (deform.slang's water stage) lifts every vertex of a body
// with a wind by `water_vertex_lift`, and the resolve's water branch (water.slang) shades it with
// `water_surface_slope`, the gradient of the same function, so the normal is the surface's. Mirrored
// in double by domain/gfx/tests/water_reference.h; change one and the other in the same commit.
//
// Offshore, a body's own waves: an open body's sixteen Gerstner waves from the spectrum (their
// vertical displacement a sin(phase)), a closed one's four sines at the material's amplitude. Near
// the shore **the depth governs** the wind's peak wave, a train: it shoals by linear theory's
// coefficient (Green's law in shallow water) until it breaks at H = 0.78 d (McCowan 1894), and is
// then the saturated bore H = 0.78 d to the shore; its crest sharpens as it nears breaking; its
// wavenumber is the dispersion relation's at the local depth; and its phase is the **arrival time**
// of the train at the place (gfx::water_arrival_field, an eikonal solved on the depth map's grid at
// load), so the crests slow and bunch as they come in and neighbouring places are in sequence.
// Over the bank the swash's front (water.slang's ballistic profile, its phase the train's arrival
// at the shoreline) decides whether the vertex is the sheet or parked under the ground.
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
  float pad;
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

// ---- the body's surface record (gfx::WaterSurface, water_swash.h) --------------------------------
static const float k_water_breaking_gamma = 0.78;  // gfx::k_water_breaking_gamma (McCowan 1894)
static const float k_water_bank_park = 0.05;       // gfx::k_water_bank_park_m
static const uint k_water_surface_spectrum = 1u;   // gfx::k_water_surface_spectrum
static const uint k_water_surface_open = 2u;       // gfx::k_water_surface_open
static const float k_water_train_deep_kd = 1.0;    // gfx::k_water_train_deep_kd
static const float k_water_train_shallow_kd = 0.35;
static const uint k_water_surface_waves = 16u;     // gfx::k_water_waves

// Mirrors gfx::WaterSurface (608 bytes).
struct WaterSurface {
  uint64_t field;  // float2 per texel: (signed depth, arrival s), rows along +z
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
  float pad;
  WaterWave waves[16];
};

// The field at body-local p: the bilinear depth and arrival, and the bilinear's own gradient in the
// cell (zero along an axis the point is clamped on).
struct WaterField {
  float depth;
  float arrival;
  float2 depth_grad;
  float2 arrival_grad;
};

WaterField water_field_at(WaterSurface* s, float2 p) {
  WaterField f = (WaterField)0;
  const uint n = s->size;
  if (s->field == 0 || n < 2) return f;
  const float last = float(n - 1);
  const float2 g = (p - s->origin) / s->texel_m;
  const float2 c = clamp(g, float2(0.0, 0.0), float2(last, last));
  const float2 i0f = min(floor(c), float2(last - 1.0, last - 1.0));
  const uint ix = uint(i0f.x);
  const uint iz = uint(i0f.y);
  const float2 fr = c - i0f;
  float2* texels = (float2*)s->field;
  const float2 a = texels[iz * n + ix];
  const float2 b = texels[iz * n + ix + 1u];
  const float2 e = texels[(iz + 1u) * n + ix];
  const float2 h = texels[(iz + 1u) * n + ix + 1u];
  const float2 v = lerp(lerp(a, b, fr.x), lerp(e, h, fr.x), fr.y);
  const float inside_x = g.x >= 0.0 && g.x <= last ? 1.0 / s->texel_m : 0.0;
  const float inside_z = g.y >= 0.0 && g.y <= last ? 1.0 / s->texel_m : 0.0;
  const float2 dx = lerp(b - a, h - e, fr.y) * inside_x;
  const float2 dz = lerp(e - a, h - b, fr.x) * inside_z;
  f.depth = v.x;
  f.arrival = v.y;
  f.depth_grad = float2(dx.x, dz.x);
  f.arrival_grad = float2(dx.y, dz.y);
  return f;
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
// `dtheta` is its derivative in theta.
float water_train_shape(float h0, float k0, float d, float theta, out float dtheta) {
  dtheta = 0.0;
  if (!(d > 0.0) || !(h0 > 0.0)) return 0.0;
  const float shoaled = h0 * water_shoaling(water_wavenumber(k0, d), d);
  const float limit = k_water_breaking_gamma * d;
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

// A closed body's waves face the shore its wind blows towards: the train's height (and the swash's
// reach) is a quarter on a shore the wind blows away from, whole where it blows straight in. An
// open body's swell comes in everywhere. Its own gradient is left out of the slope (a few percent
// of it, where the shore turns).
float water_facing(WaterSurface* s, float2 depth_grad) {
  if ((s->flags & k_water_surface_open) != 0) return 1.0;
  const float len = length(depth_grad);
  if (!(len > 1e-3)) return 1.0;
  return 0.25 + 0.75 * clamp(-dot(s->wind, depth_grad / len), 0.0, 1.0);
}

// The waves' weight against the train's: 1 where k0 d >= 1, 0 under 0.35, smoothstep between.
float water_offshore_weight(WaterSurface* s, float d, out float dd) {
  const float lo = k_water_train_shallow_kd / max(s->k0, 1e-6);
  const float hi = k_water_train_deep_kd / max(s->k0, 1e-6);
  const float x = clamp((d - lo) / (hi - lo), 0.0, 1.0);
  dd = d > lo && d < hi ? 6.0 * x * (1.0 - x) / (hi - lo) : 0.0;
  return x * x * (3.0 - 2.0 * x);
}

// The height of the water at body-local p over the still level (the tide's not included), its
// gradient, and the Gerstner crests' sharpening of the normal (`lift`). `footprint` (metres, 0 for
// the mesh) fades a wave shorter than twice it out of the gradient, as the four sines always were.
struct WaterHeight {
  float height;
  float2 grad;
  float lift;
};

WaterHeight water_height(WaterSurface* s, float2 p, WaterField f, float t, float footprint) {
  WaterHeight r = (WaterHeight)0;
  const float d = f.depth;
  float dw = 0.0;
  const float w_off = s->k0 > 0.0 && s->height_m > 0.0 ? water_offshore_weight(s, d, dw) : 1.0;
  // Offshore: the spectrum's sixteen, or the four sines.
  float off = 0.0;
  float2 off_grad = float2(0.0, 0.0);
  if (w_off > 0.0 || dw != 0.0) {
    if ((s->flags & k_water_surface_spectrum) != 0) {
      for (uint i = 0; i < k_water_surface_waves; ++i) {
        const WaterWave wave = s->waves[i];
        const float lambda = k_water_two_pi / wave.k;
        const float along = dot(wave.direction, p) / lambda;
        const float travel = frac(wave.cycles * (t / k_water_period_s));
        const float cyc = frac(frac(along) - travel + wave.phase);
        const float keep =
            footprint > 0.0 ? clamp(lambda / (2.0 * footprint) - 1.0, 0.0, 1.0) : 1.0;
        const float angle = k_water_two_pi * cyc;
        off += wave.amplitude * keep * sin(angle);
        off_grad += wave.direction * (wave.amplitude * wave.k * keep * cos(angle));
        r.lift += wave.steepness * keep * w_off * sin(angle);
      }
    } else {
      for (uint i = 0; i < 4; ++i) {
        const float lambda = s->wavelength * k_water_length[i];
        const float a = s->amplitude * k_water_amplitude[i];
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
        const float keep =
            footprint > 0.0 ? clamp(lambda / (2.0 * footprint) - 1.0, 0.0, 1.0) : 1.0;
        off += a * keep * sin(k_water_two_pi * cyc);
        off_grad += dir * (a * k * keep * cos(k_water_two_pi * cyc));
      }
    }
  }
  // Near the shore: the train.
  float train = 0.0;
  float2 train_grad = float2(0.0, 0.0);
  if (w_off < 1.0 && d > 0.0 && s->height_m > 0.0 && s->period_s > 0.0) {
    const float c = water_train_cycles(s, t, f.arrival);
    const float theta = k_water_two_pi * frac(c);
    const float local_k = water_wavenumber(s->k0, d);
    const float keep =
        footprint > 0.0 ? clamp(k_water_two_pi / local_k / (2.0 * footprint) - 1.0, 0.0, 1.0)
                        : 1.0;
    float d_env = 0.0;
    const float env = water_train_envelope(c, d_env);
    const float facing = water_facing(s, f.depth_grad);
    float d_theta = 0.0;
    const float shape = water_train_shape(s->height_m, s->k0, d, theta, d_theta);
    // The shape's change with depth, by a central difference over a millimetre's part of it.
    const float eps = 1e-3 + 1e-3 * d;
    float unused = 0.0;
    const float d_depth = (water_train_shape(s->height_m, s->k0, d + eps, theta, unused) -
                           water_train_shape(s->height_m, s->k0, max(d - eps, 1e-4), theta, unused)) /
                          (d + eps - max(d - eps, 1e-4));
    // grad c = -grad tau / T: the local wavenumber, along the way the train travels.
    const float2 grad_c = -f.arrival_grad / s->period_s;
    train = env * facing * shape;
    train_grad = facing * keep *
                 (env * (d_depth * f.depth_grad + d_theta * k_water_two_pi * grad_c) +
                  shape * d_env * grad_c);
    train *= keep;
  }
  r.height = w_off * off + (1.0 - w_off) * train;
  r.grad = w_off * off_grad + (1.0 - w_off) * train_grad + (off - train) * dw * f.depth_grad;
  return r;
}

// An open body's tide at the frame's phase of M2, metres above the level.
float water_surface_tide(WaterSurface* s, float tide_phase) {
  return s->tide_m > 0.0 ? s->tide_m * sin(k_water_two_pi * tide_phase) : 0.0;
}

// The along-shore variation of the swash's reach, [0.85, 1.15]: smooth over `along_m` (about two
// peak wavelengths, the crest's length under a spread sea), so the front is one connected line.
float water_reach_variation(WaterSurface* s, float2 p) {
  return 0.85 + 0.3 * water_value_noise(p / max(s->along_m, 1e-3) + float2(17.3, 5.1));
}

// The swash's front above the still (and tide's) level at p and t: the ballistic profile of the
// wave whose crest reached the shoreline last, its reach R times the group's shape for that wave
// times the along-shore variation times the shore's facing. `uprush` says which half it is in.
float water_surface_front(WaterSurface* s, float2 p, WaterField f, float t, out bool uprush) {
  uprush = false;
  if (!(s->run_up_m > 0.0) || !(s->period_s > 0.0)) return 0.0;
  const float c = water_train_cycles(s, t, f.arrival);
  const float k = floor(c);
  const float u = c - k;
  uprush = u < k_swash_uprush;
  const uint j = uint(int(k) + int(k_swash_group) * 64) % k_swash_group;
  const float reach =
      s->run_up_m * water_swash_group(j) * water_reach_variation(s, p) * water_facing(s, f.depth_grad);
  const float z_lo = -k_swash_rundown * s->run_up_m;
  return z_lo + (reach - z_lo) * water_swash_shape(u);
}

// **A vertex's lift** (the pool's water stage): how far above its rest place, the still level, a
// vertex at body-local p stands at the clock t, `edge` its distance in from the outline (UV.x).
// Water deeper than the swash's band takes the waves and the tide, faded in from an open body's
// outline over `fade_m` so its edge meets the flat sea past it; over the bank (and where the
// backwash bares the bottom) the vertex is the sheet's surface where the front covers it — the
// ground plus the sheet's thickness, half the front's height over the ground up to a quarter of R
// and 2 cm — and 5 cm under the ground where it does not, which the depth test hides (never higher
// than the front can reach, so a parked vertex under a cliff stands no wall up). The two blend over
// the band. A body with no field is its waves alone.
float water_vertex_lift(WaterSurface* s, float2 p, float edge, float t, float tide_phase,
                        out float2 grad) {
  grad = float2(0.0, 0.0);
  const WaterField f = water_field_at(s, p);
  const float tide = water_surface_tide(s, tide_phase);
  const float fade = (s->flags & k_water_surface_open) != 0 && s->fade_m > 0.0
                         ? smoothstep(0.0, s->fade_m, edge)
                         : 1.0;
  const WaterHeight w = water_height(s, p, f, t, 0.0);
  const float water = (tide + w.height) * fade;
  if (s->field == 0) {
    grad = w.grad * fade;
    return water;
  }
  const float d = f.depth + tide;
  const float band = 0.2 * s->run_up_m + 0.05;
  if (d >= band) {
    grad = w.grad * fade;
    return water;
  }
  const float g = -d;  // the ground over the instant's still level
  bool uprush = false;
  const float front = water_surface_front(s, p, f, t, uprush);
  float bank;
  if (front > g) {
    bank = tide + g + min(0.5 * (front - g), 0.25 * s->run_up_m + 0.02);
  } else {
    bank = tide + min(g, 1.15 * s->run_up_m + 0.1) - k_water_bank_park;
  }
  const float share = 1.0 - smoothstep(0.0, band, d);
  grad = w.grad * fade * (1.0 - share);
  return lerp(water, bank, share);
}

#endif  // ENGINE_WATER_WAVES_SLANG
