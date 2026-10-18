// The participating medium's mid tier (docs/subsystems/volumetrics.md; plan 04 §4.2, the
// 2026-09-24 direction note): view-aligned froxels filled from the medium's sources, integrated
// front to back, and applied to the frame's linear colour image (the renderer's frame pass,
// systems/renderer/frame_pass.h). Single scattering, the sun shadowed by the cascaded maps and lit
// through the air, the sky's ambient, one constant term for the light scattered more than once.
//
// **No absolute world position.** Every position here is in the frame's space, metres from the
// eye (ADR-0053); what is anchored to the world — the dust's ground, the plumes' brinks, the
// clouds' noise — arrives as an offset the CPU computed in f64 (`MediumParams`).
//
// **No history.** Every value is a function of this frame's block: the same frame is the same
// bytes (volumetrics_gpu_tests.cpp).

#include "../../../domain/gfx/shaders/sky.slang"
#include "../../../domain/gfx/shaders/view_ray.slang"

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(1, 0)]] RWTexture2D<float4> g_storage_images[];
[[vk::binding(2, 0)]] SamplerState g_samplers[];

static const float k_pi = 3.14159265358979323846;

// Mirrors gfx::ShadowCascade (80 bytes) and gfx::ShadowMapParams (400 bytes), as the resolve has
// them (visibility_resolve.slang), which this file may not include: it declares the resolve's own
// push constant and entry points.
struct MediumCascade {
  column_major float4x4 view_proj;
  float texel_world;
  float depth_range;
  float bias;
  float pad;
};
struct MediumShadowMaps {
  MediumCascade cascades[4];
  float4 light_right;
  float4 light_up;
  float4 light_dir;
  uint cascade_count;
  uint resolution;
  uint texture;
  uint sampler;
  float max_slope;
  uint tiles;
  float normal_offset;
  uint light;
};

// Mirrors volumetrics::PlumeSegment (48 bytes): one segment of a crest's brink and its plume.
struct PlumeSegment {
  float4 a;       // xyz the segment's first end (frame space), w the plume's peak extinction /m
  float4 b;       // xyz its second end, w the decay length downwind, m
  float4 shape;   // xy downwind (unit, horizontal), z the spread at the brink, w spread per metre
};

// Mirrors volumetrics::MediumParams (512 bytes): one view's block, per frame slot.
struct MediumParams {
  column_major float4x4 clip_to_eye;  // the view's clip space (reversed-Z depth) to the eye's frame
  column_major float4x4 clip_to_ray;  // the view's pixel to its ray direction (view_ray.slang)
  uint4 grid;          // froxels x, y, z, and the sources (k_source_*)
  float4 slices;       // near, far, ln(far / near), z / ln(far / near)
  float4 sun;          // xyz towards the sun, w the cascaded maps' depth bias in texels
  float4 sun_light;    // rgb the sun's light (outside the air with a sky), w 1 with a sky
  float4 ambient;      // rgb the stand-in's ambient radiance, w the exposure without a sky
  float4 dust;         // x extinction at the ground /m, y scale height m, z the ground's y, w eye alt
  float4 dust_albedo;  // rgb single-scattering albedo, w the phase's g
  float4 terms;        // x the constant multiple-scattering share, y the plumes' g, z, w unused
  uint4 rect;          // the view's rectangle of the colour image
  uint4 source;        // x, y its region of the visibility buffer, z the colour's storage slot, w 0
  float4 plume_grid;   // xy the grid's corner (frame space x, z), z its cell, w cells a side
  float4 cloud;        // x base m, y top m, z coverage, w type
  float4 cloud_origin; // xyz the noise's coordinate at the eye (wrapped, m), w the field's density
  float4 cloud_size;   // xy the march's texels, zw the full view's size it was marched for
  float4 cloud_shadow; // xy the map's corner (frame x, z), z its texel, w texels a side
  uint64_t visibility;    // this view's region of the visibility buffer
  uint64_t froxels;       // uint2[x * y * z]: four halves a cell
  uint64_t sky;           // SkyParams*, 0 for the stand-in
  uint64_t shadow_maps;   // MediumShadowMaps*, 0 without the cascaded maps
  uint64_t plumes;        // PlumeSegment[]
  uint64_t plume_cells;   // uint[cells^2 + 1] offsets into plume_index, then the indices
  uint64_t clouds;        // uint2[cloud texels]: the march's four halves a texel
  uint64_t cloud_shadow_map;  // float[texels^2]: the clouds' transmittance towards the sun
  uint4 pad[5];
};

struct MediumPush {
  uint64_t params;
};
[[vk::push_constant]] ConstantBuffer<MediumPush> g_push;

static const uint k_source_dust = 1u;
static const uint k_source_plumes = 2u;
static const uint k_source_clouds = 4u;
static const uint k_source_shadows = 8u;  // the cascaded maps are drawn this frame

// ---- the scattering model ----------------------------------------------------------------------

// Henyey-Greenstein: the share of light scattered through the angle whose cosine is `mu`, between
// the direction the light travels and the one it leaves in, per steradian.
float hg_phase(float mu, float g) {
  const float g2 = g * g;
  return (1.0 - g2) / (4.0 * k_pi * pow(max(1.0 + g2 - 2.0 * g * mu, 1.0e-4), 1.5));
}

uint2 pack_halves(float4 v) {
  return uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16));
}
float4 unpack_halves(uint2 p) {
  return float4(f16tof32(p.x & 0xffffu), f16tof32(p.x >> 16), f16tof32(p.y & 0xffffu),
                f16tof32(p.y >> 16));
}

// The distance to slice boundary `k` (exponential slices from `near` to `far`).
float slice_distance(MediumParams* p, float k) {
  return p->slices.x * exp(k / float(p->grid.z) * p->slices.z);
}

// ---- the dust haze ------------------------------------------------------------------------------

// Extinction at a point of the frame's space: the ground's density falling off with the height
// above the dust's ground (`dust.z`, the ground under the eye), never more than at the ground.
float dust_extinction(MediumParams* p, float3 x) {
  const float h = max(x.y - p->dust.z, 0.0);
  return p->dust.x * exp(-h / p->dust.y);
}

// The dust's optical depth along the eye's ray `dir` between distances a and b, in closed form:
// the exponential above the ground, and the ground's density below it (volumetrics.h,
// `dust_optical_depth`, is the same arithmetic on the CPU).
float dust_optical_depth(MediumParams* p, float dy, float a, float b) {
  if (b <= a) return 0.0;
  const float sigma0 = p->dust.x;
  const float H = p->dust.y;
  const float g = p->dust.z;  // the ground's y in the frame: h(t) = t dy - g
  // Where the ray crosses the ground's height, if it does going down.
  float t0 = 3.0e38;
  if (dy < 0.0) t0 = max(g / dy, 0.0);
  float tau = 0.0;
  const float e_hi = min(b, t0);
  if (e_hi > a) {
    if (abs(dy) < 1.0e-5) {
      tau += sigma0 * exp(min(g / H, 0.0)) * (e_hi - a);
    } else {
      // integral of sigma0 exp(-(t dy - g) / H) dt
      const float k = dy / H;
      tau += sigma0 * exp(g / H) * (exp(-a * k) - exp(-e_hi * k)) / k;
    }
  }
  if (b > t0) tau += sigma0 * (b - max(a, t0));
  return tau;
}

// ---- the crest plumes (W45) ---------------------------------------------------------------------

// volumetrics.cpp's `plume_density`, the same arithmetic: where the point is in the ribbon, x - a =
// u (b - a) + t w over the ground; nothing upwind of the brink; a half-Gaussian about a centre that
// rises downwind, wider above than below, falling off past the brink's ends, spread and decaying.
static const float k_plume_lift = 0.04;
static const float k_plume_floor = 0.35;
float plume_density(PlumeSegment s, float3 x) {
  const float ex = s.b.x - s.a.x;
  const float ez = s.b.z - s.a.z;
  const float dx = x.x - s.a.x;
  const float dz = x.z - s.a.z;
  const float wx = s.shape.x;
  const float wz = s.shape.y;
  const float det = ex * wz - ez * wx;
  const float len = sqrt(ex * ex + ez * ez);
  if (abs(det) < 1.0e-3 * len || len <= 0.0) return 0.0;
  const float u = (dx * wz - dz * wx) / det;
  const float t = (ex * dz - ez * dx) / det;
  if (t < 0.0) return 0.0;
  const float sigma = s.shape.z + s.shape.w * t;
  const float over = (u < 0.0 ? -u : (u > 1.0 ? u - 1.0 : 0.0)) * len;
  const float lateral = exp(-0.5 * (over / sigma) * (over / sigma));
  const float uc = clamp(u, 0.0, 1.0);
  const float centre = s.a.y + (s.b.y - s.a.y) * uc + k_plume_lift * t;
  const float dy = x.y - centre;
  const float spread = dy >= 0.0 ? sigma : sigma * k_plume_floor;
  const float vertical = exp(-0.5 * (dy / spread) * (dy / spread));
  return s.a.w * (s.shape.z / sigma) * exp(-t / s.b.w) * lateral * vertical;
}

// Every plume over the point's cell of the binning grid.
float plumes_at(MediumParams* p, float3 x) {
  if ((p->grid.w & k_source_plumes) == 0 || p->plumes == 0) return 0.0;
  const float n = p->plume_grid.w;
  const float2 c = (x.xz - p->plume_grid.xy) / p->plume_grid.z;
  if (c.x < 0.0 || c.y < 0.0 || c.x >= n || c.y >= n) return 0.0;
  const uint index = uint(c.y) * uint(n) + uint(c.x);
  uint* cells = (uint*)p->plume_cells;
  PlumeSegment* segments = (PlumeSegment*)p->plumes;
  const uint end = cells[index + 1];
  float sum = 0.0;
  for (uint i = cells[index]; i < end; ++i) sum += plume_density(segments[cells[i]], x);
  return sum;
}

// ---- light at a point --------------------------------------------------------------------------

// The sun's visibility through the cascaded maps at a froxel: one gather of the 2 x 2 texels round
// the point in the first cascade that holds it, each compared against the point's own depth plus
// a bias of `sun.w` texels; 1 outside every cascade.
float medium_cascade(MediumParams* p, float3 x) {
  if ((p->grid.w & k_source_shadows) == 0 || p->shadow_maps == 0) return 1.0;
  MediumShadowMaps* maps = (MediumShadowMaps*)p->shadow_maps;
  const float res = float(maps->resolution);
  const float2 atlas = float2(res * float(maps->tiles), res);
  for (uint c = 0; c < maps->cascade_count; ++c) {
    const MediumCascade cascade = maps->cascades[c];
    const float4 clip = mul(cascade.view_proj, float4(x, 1.0));
    if (clip.z < 0.0 || clip.z > 1.0) continue;
    const float2 t = float2((clip.x * 0.5 + 0.5) * res, (0.5 - clip.y * 0.5) * res);
    if (t.x < 1.0 || t.y < 1.0 || t.x > res - 1.0 || t.y > res - 1.0) continue;
    const float2 at = float2(float(c) * res + t.x, t.y) / atlas;
    const float4 g = g_textures[maps->texture].Gather(g_samplers[maps->sampler], at);
    const float z = clip.z + p->sun.w * cascade.texel_world / cascade.depth_range;
    // Reversed depth towards the light: a texel holding more than the point is between it and the
    // sun.
    const float4 lit = float4(z >= g.x ? 1.0 : 0.0, z >= g.y ? 1.0 : 0.0, z >= g.z ? 1.0 : 0.0,
                              z >= g.w ? 1.0 : 0.0);
    return dot(lit, float4(0.25, 0.25, 0.25, 0.25));
  }
  return 1.0;
}

// The sun's light arriving at a point, unshadowed: through the air above it with a sky, the
// stand-in's white intensity without one.
float3 sun_at(MediumParams* p, float3 x) {
  if (p->sun_light.w == 0.0) return p->sun_light.rgb;
  SkyParams* sky = (SkyParams*)p->sky;
  return sky_light_at(sky, float3(0.0, p->dust.w + x.y, 0.0), p->sun.xyz, p->sun_light.rgb);
}

// The light a medium scatters isotropically from the whole sky: the mean of the two hemispheres'
// ambient radiance, or the stand-in's.
float3 ambient_at(MediumParams* p) {
  if (p->sun_light.w == 0.0) return p->ambient.rgb;
  SkyParams* sky = (SkyParams*)p->sky;
  SkyFrame* frame = (SkyFrame*)sky->frame;
  return 0.5 * (sky_ambient(frame, float3(0.0, 1.0, 0.0)) + sky_ambient(frame, float3(0.0, -1.0, 0.0)));
}

float frame_exposure(MediumParams* p) {
  if (p->sun_light.w == 0.0) return p->ambient.w;
  SkyParams* sky = (SkyParams*)p->sky;
  return sky_exposure((SkyFrame*)sky->frame);
}

// What a unit of scattering coefficient at a point sends towards the eye along `dir`, per metre:
// the sun through the phase function, shadowed, plus the constant share of the sun scattered more
// than once (which reaches the shadow too), plus the sky's ambient.
float3 source_light(MediumParams* p, float3 x, float3 dir, float g, float shadow) {
  const float3 sun = sun_at(p, x);
  const float mu = dot(p->sun.xyz, dir);
  return sun * (hg_phase(mu, g) * shadow + p->terms.x / (4.0 * k_pi)) + ambient_at(p);
}

// ---- the far-field clouds (R83) ----------------------------------------------------------------
//
// volumetrics.cpp's `cloud_density` and its noises, the same arithmetic: value-noise fbm in
// Perlin's role, remapped by inverted Worley cells, a height profile by the type, the coverage's
// threshold, a finer Worley erosion. Lattices wrap at the field's period on x and z, so the noise
// coordinate the CPU reduced (`cloud_origin`) is seamless.

static const float k_cloud_period = 131072.0;
static const float k_cloud_shape = 4096.0;
static const float k_cloud_cell = 2048.0;
static const float k_cloud_detail = 512.0;
static const uint k_cloud_steps = 48;
static const float k_cloud_phase_forward = 0.8;
static const float k_cloud_phase_back = -0.3;
static const float k_cloud_phase_mix = 0.7;
static const float k_cloud_multiple = 0.3;
static const float k_cloud_far = 60000.0;  // the march stops this far along a grazing ray

uint cloud_hash(int x, int y, int z, uint seed) {
  uint h = seed * 0x9E3779B9u ^ uint(x) * 0x85EBCA6Bu ^ uint(y) * 0xC2B2AE35u ^ uint(z) * 0x27D4EB2Fu;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
}
int cloud_wrap(int v, int period) {
  const int m = v % period;
  return m < 0 ? m + period : m;
}
float cloud_unit(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }
float value_noise(float3 q, int period, uint seed) {
  const float3 f = floor(q);
  const int3 i = int3(f);
  float3 t = q - f;
  t = t * t * (3.0 - 2.0 * t);
  float c[8];
  for (uint k = 0; k < 8; ++k) {
    c[k] = cloud_unit(cloud_hash(cloud_wrap(i.x + int(k & 1), period), i.y + int((k >> 1) & 1),
                                 cloud_wrap(i.z + int(k >> 2), period), seed));
  }
  const float x00 = c[0] + (c[1] - c[0]) * t.x, x10 = c[2] + (c[3] - c[2]) * t.x;
  const float x01 = c[4] + (c[5] - c[4]) * t.x, x11 = c[6] + (c[7] - c[6]) * t.x;
  const float y0 = x00 + (x10 - x00) * t.y, y1 = x01 + (x11 - x01) * t.y;
  return y0 + (y1 - y0) * t.z;
}
float worley(float3 q, int period, uint seed) {
  const float3 f = floor(q);
  const int3 i = int3(f);
  float best = 9.0;
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        const uint h = cloud_hash(cloud_wrap(i.x + dx, period), i.y + dy, cloud_wrap(i.z + dz, period),
                                  seed ^ 0x5bd1e995u);
        const float3 d = float3(float(dx) + cloud_unit(h), float(dy) + cloud_unit(h * 0x27d4eb2du),
                                float(dz) + cloud_unit(h * 0x165667b1u)) -
                         (q - f);
        best = min(best, dot(d, d));
      }
    }
  }
  return 1.0 - min(sqrt(best), 1.0);
}

// The layer's extinction at noise coordinate (q.x, q.z) and altitude q.y. `detail` off is the
// coarse shape alone, which the light's march towards the sun reads.
float cloud_density(MediumParams* p, float3 q, bool detail) {
  const float base = p->cloud.x;
  const float top = p->cloud.y;
  const float coverage = p->cloud.z;
  const float h = (q.y - base) / (top - base);
  if (h <= 0.0 || h >= 1.0 || coverage <= 0.0) return 0.0;
  const uint seed = p->source.w;
  const int shape_period = int(k_cloud_period / k_cloud_shape);
  const float3 s = q / k_cloud_shape;
  const float fbm = 0.625 * value_noise(s, shape_period, seed) +
                    0.25 * value_noise(2.0 * s, 2 * shape_period, seed + 1u) +
                    0.125 * value_noise(4.0 * s, 4 * shape_period, seed + 2u);
  const float w = worley(q / k_cloud_cell, int(k_cloud_period / k_cloud_cell), seed);
  const float pw = saturate((fbm - (w - 1.0)) / (2.0 - w));
  const float shape_top = 0.35 + 0.65 * p->cloud.w;
  const float profile = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(0.6 * shape_top, shape_top, h));
  float d = saturate((pw * profile - (1.0 - coverage)) / coverage);
  if (d <= 0.0) return 0.0;
  if (detail) {
    const float e = worley(q / k_cloud_detail, int(k_cloud_period / k_cloud_detail), seed + 7u);
    d = saturate((d - 0.3 * (1.0 - e)) / (1.0 - 0.3 * (1.0 - e)));
  }
  return d * p->cloud_origin.w;
}

// A point of the frame's space as the clouds' noise coordinate: its offset from the eye plus the
// eye's coordinate the CPU computed, and its altitude.
float3 cloud_coordinate(MediumParams* p, float3 x) {
  return float3(p->cloud_origin.x + x.x, p->dust.w + x.y, p->cloud_origin.z + x.z);
}

// Where a ray from the eye's altitude in direction `dir` is inside the slab: [t0, t1], or false.
bool cloud_slab(MediumParams* p, float3 dir, out float t0, out float t1) {
  const float a = p->dust.w;
  const float base = p->cloud.x;
  const float top = p->cloud.y;
  t0 = 0.0;
  t1 = 0.0;
  if (abs(dir.y) < 1.0e-4) {
    if (a <= base || a >= top) return false;
    t1 = k_cloud_far;
    return true;
  }
  const float tb = (base - a) / dir.y;
  const float tt = (top - a) / dir.y;
  t0 = max(min(tb, tt), 0.0);
  t1 = min(max(tb, tt), t0 + k_cloud_far);
  return t1 > t0;
}

float cloud_phase(float mu) {
  return lerp(hg_phase(mu, k_cloud_phase_back), hg_phase(mu, k_cloud_phase_forward), k_cloud_phase_mix);
}

// The clouds' transmittance towards the sun from a point on a horizontal plane of the frame's
// space (the dust's ground), through the slab's coarse shape in eight steps: the map the froxels
// read, `cloud_shadow` texels a side round the eye.
[shader("compute")]
[numthreads(8, 8, 1)]
void cloud_shadow_main(uint3 id : SV_DispatchThreadID) {
  MediumParams* p = (MediumParams*)g_push.params;
  const uint n = uint(p->cloud_shadow.w);
  if (id.x >= n || id.y >= n) return;
  float transmittance = 1.0;
  const float3 sun = p->sun.xyz;
  if (sun.y > 0.02) {
    const float3 g = float3(p->cloud_shadow.x + (float(id.x) + 0.5) * p->cloud_shadow.z, p->dust.z,
                            p->cloud_shadow.y + (float(id.y) + 0.5) * p->cloud_shadow.z);
    const float alt = p->dust.w + g.y;
    const float t0 = max((p->cloud.x - alt) / sun.y, 0.0);
    const float t1 = max((p->cloud.y - alt) / sun.y, t0);
    const float step = (t1 - t0) / 8.0;
    float tau = 0.0;
    for (uint i = 0; i < 8; ++i) {
      const float3 x = g + sun * (t0 + (float(i) + 0.5) * step);
      tau += cloud_density(p, cloud_coordinate(p, x), false) * step;
    }
    transmittance = exp(-tau);
  }
  ((float*)p->cloud_shadow_map)[id.y * n + id.x] = transmittance;
}

// The clouds' transmittance towards the sun at a froxel: where the sun's ray through it crosses the
// map's plane, bilinear.
float cloud_sun(MediumParams* p, float3 x) {
  if ((p->grid.w & k_source_clouds) == 0 || p->cloud_shadow_map == 0 || p->sun.y <= 0.02) return 1.0;
  const float3 sun = p->sun.xyz;
  const float2 at = x.xz - sun.xz * ((x.y - p->dust.z) / sun.y);
  const float n = p->cloud_shadow.w;
  const float2 c = clamp((at - p->cloud_shadow.xy) / p->cloud_shadow.z - 0.5, float2(0.0), float2(n - 1.0));
  const uint2 c0 = uint2(c);
  const uint2 c1 = min(c0 + 1, uint2(uint(n) - 1));
  const float2 f = c - float2(c0);
  float* map = (float*)p->cloud_shadow_map;
  const uint w = uint(n);
  const float a = lerp(map[c0.y * w + c0.x], map[c0.y * w + c1.x], f.x);
  const float b = lerp(map[c1.y * w + c0.x], map[c1.y * w + c1.x], f.x);
  return lerp(a, b, f.y);
}

// The march through the slab, at a quarter of the view's pixels a side: single scattering with the
// two-lobe phase, the sun through the air at the sample's altitude and through the cloud above it
// (four samples towards the sun), one octave of multiple scattering at a quarter of that depth,
// the sky's ambient; then the air between the eye and the cloud from the aerial table. Each texel
// marches along its own pixel's direction, whatever the full-resolution pixels under it hold, so no
// texel carries a surface's depth and the upsample needs no depth weights: the layer is composited
// into sky pixels only. The samples are offset by a hash of the texel — a fixed pattern, no frame in
// it.
[shader("compute")]
[numthreads(8, 8, 1)]
void cloud_march_main(uint3 id : SV_DispatchThreadID) {
  MediumParams* p = (MediumParams*)g_push.params;
  const uint cw = uint(p->cloud_size.x);
  const uint ch = uint(p->cloud_size.y);
  if (id.x >= cw || id.y >= ch) return;
  const float2 pixel = (float2(id.xy) + 0.5) * float(4.0);
  const float2 ndc = float2(pixel.x / p->cloud_size.z * 2.0 - 1.0, 1.0 - pixel.y / p->cloud_size.w * 2.0);
  const float3 dir = view_ray_direction(p->clip_to_ray, ndc);
  float3 light = float3(0.0);
  float transmittance = 1.0;
  float t0, t1;
  if (cloud_slab(p, dir, t0, t1)) {
    const float step = (t1 - t0) / float(k_cloud_steps);
    const float jitter = cloud_unit(cloud_hash(int(id.x), int(id.y), 0, 0x68e31da4u));
    const float mu = dot(p->sun.xyz, dir);
    const float phase = cloud_phase(mu);
    const float3 ambient = ambient_at(p);
    float weighted = 0.0;
    float weight = 0.0;
    for (uint i = 0; i < k_cloud_steps && transmittance > 0.003; ++i) {
      const float t = t0 + (float(i) + jitter) * step;
      const float3 x = dir * t;
      const float3 q = cloud_coordinate(p, x);
      const float s = cloud_density(p, q, true);
      if (s <= 0.0) continue;
      float tau_sun = 0.0;
      if (p->sun.y > -0.1) {
        float d = 0.0;
        float l = 120.0;
        for (uint k = 0; k < 4; ++k) {
          const float3 xs = x + p->sun.xyz * (d + 0.5 * l);
          tau_sun += cloud_density(p, cloud_coordinate(p, xs), false) * l;
          d += l;
          l *= 2.0;
        }
      }
      const float3 sun = sun_at(p, x);
      const float3 source = s * (sun * (phase * exp(-tau_sun) +
                                        k_cloud_multiple * exp(-0.25 * tau_sun) / (4.0 * k_pi)) +
                                 ambient);
      const float slice_t = exp(-s * step);
      light += transmittance * source * (1.0 - slice_t) / s;
      weighted += transmittance * (1.0 - slice_t) * t;
      weight += transmittance * (1.0 - slice_t);
      transmittance *= slice_t;
    }
    // The air between the eye and the cloud, at the cloud's mean distance: what it takes out of the
    // cloud's light and what it scatters in where the cloud now hides the sky behind it.
    if (weight > 0.0 && p->sun_light.w != 0.0) {
      float3 inscatter;
      float air;
      sky_aerial((SkyParams*)p->sky, dir, weighted / weight, inscatter, air);
      light = light * air + inscatter * (1.0 - transmittance);
    }
  }
  ((uint2*)p->clouds)[id.y * cw + id.x] = pack_halves(float4(light, transmittance));
}

// The march's texels at a full-resolution pixel of the view, bilinear.
float4 clouds_at(MediumParams* p, uint2 pixel) {
  const uint cw = uint(p->cloud_size.x);
  const uint ch = uint(p->cloud_size.y);
  const float2 c = clamp((float2(pixel) + 0.5) / 4.0 - 0.5, float2(0.0), float2(float(cw) - 1.0, float(ch) - 1.0));
  const uint2 c0 = uint2(c);
  const uint2 c1 = min(c0 + 1, uint2(cw - 1, ch - 1));
  const float2 f = c - float2(c0);
  uint2* t = (uint2*)p->clouds;
  const float4 a = lerp(unpack_halves(t[c0.y * cw + c0.x]), unpack_halves(t[c0.y * cw + c1.x]), f.x);
  const float4 b = lerp(unpack_halves(t[c1.y * cw + c0.x]), unpack_halves(t[c1.y * cw + c1.x]), f.x);
  return lerp(a, b, f.y);
}

// ---- (a) the froxels ---------------------------------------------------------------------------

float3 froxel_centre(MediumParams* p, uint3 cell, out float3 dir) {
  const float2 ndc = float2((float(cell.x) + 0.5) / float(p->grid.x) * 2.0 - 1.0,
                            1.0 - (float(cell.y) + 0.5) / float(p->grid.y) * 2.0);
  dir = view_ray_direction(p->clip_to_ray, ndc);
  return dir * slice_distance(p, float(cell.z) + 0.5);
}

// Each cell: its extinction and the light it scatters towards the eye per metre (albedo-weighted),
// four halves, from every source at the cell's centre.
[shader("compute")]
[numthreads(4, 4, 4)]
void froxel_fill_main(uint3 id : SV_DispatchThreadID) {
  MediumParams* p = (MediumParams*)g_push.params;
  if (id.x >= p->grid.x || id.y >= p->grid.y || id.z >= p->grid.z) return;
  float3 dir;
  const float3 x = froxel_centre(p, id, dir);
  float sigma = 0.0;
  float3 scatter = float3(0.0);
  if ((p->grid.w & k_source_dust) != 0) {
    const float s = dust_extinction(p, x);
    if (s > 0.0) {
      sigma += s;
      scatter += s * p->dust_albedo.rgb;
    }
  }
  {
    const float s = plumes_at(p, x);
    if (s > 0.0) {
      sigma += s;
      scatter += s * p->dust_albedo.rgb;
    }
  }
  float3 light = float3(0.0);
  if (sigma > 0.0) {
    const float shadow = medium_cascade(p, x) * cloud_sun(p, x);
    light = scatter * source_light(p, x, dir, p->dust_albedo.w, shadow);
  }
  const uint index = (id.z * p->grid.y + id.y) * p->grid.x + id.x;
  ((uint2*)p->froxels)[index] = pack_halves(float4(light, sigma));
}

// ---- (b) the integration -----------------------------------------------------------------------

// Front to back along each column: a slice of extinction s and source S over a length l adds
// S (1 - exp(-s l)) / s through what is already in front of it — the energy-conserving form, exact
// for a constant cell — and the cell then holds the light and the transmittance from the eye to its
// far boundary.
[shader("compute")]
[numthreads(8, 8, 1)]
void froxel_integrate_main(uint3 id : SV_DispatchThreadID) {
  MediumParams* p = (MediumParams*)g_push.params;
  if (id.x >= p->grid.x || id.y >= p->grid.y) return;
  float3 light = float3(0.0);
  float transmittance = 1.0;
  uint2* cells = (uint2*)p->froxels;
  for (uint k = 0; k < p->grid.z; ++k) {
    const uint index = (k * p->grid.y + id.y) * p->grid.x + id.x;
    const float4 cell = unpack_halves(cells[index]);
    const float length = slice_distance(p, float(k + 1)) - slice_distance(p, float(k));
    const float s = cell.w;
    const float slice_t = exp(-s * length);
    const float3 added = s > 1.0e-7 ? cell.rgb * (1.0 - slice_t) / s : cell.rgb * length;
    light += transmittance * added;
    transmittance *= slice_t;
    cells[index] = pack_halves(float4(light, transmittance));
  }
}

// ---- (c) the apply -----------------------------------------------------------------------------

// The integrated column at a pixel's froxel coordinate `uv` and slice coordinate `s` (0 at the near
// boundary, z at the far one): bilinear across the four columns, linear between boundaries.
float4 integrated_at(MediumParams* p, float2 uv, float s) {
  const float2 c = clamp(uv - 0.5, float2(0.0), float2(float(p->grid.x) - 1.0, float(p->grid.y) - 1.0));
  const uint2 c0 = uint2(c);
  const uint2 c1 = min(c0 + 1, uint2(p->grid.x - 1, p->grid.y - 1));
  const float2 f = c - float2(c0);
  const float sz = clamp(s, 0.0, float(p->grid.z));
  // Boundary b's value is cell b - 1's; boundary 0 is the eye: no light, full transmittance.
  const uint b0 = uint(floor(sz));
  const float fz = sz - float(b0);
  const uint b1 = min(b0 + 1, p->grid.z);
  uint2* cells = (uint2*)p->froxels;
  float4 sum = float4(0.0);
  for (uint corner = 0; corner < 4; ++corner) {
    const uint2 cc = uint2((corner & 1) != 0 ? c1.x : c0.x, (corner & 2) != 0 ? c1.y : c0.y);
    const float w = ((corner & 1) != 0 ? f.x : 1.0 - f.x) * ((corner & 2) != 0 ? f.y : 1.0 - f.y);
    float4 v0 = float4(0.0, 0.0, 0.0, 1.0);
    float4 v1 = float4(0.0, 0.0, 0.0, 1.0);
    if (b0 > 0) v0 = unpack_halves(cells[((b0 - 1) * p->grid.y + cc.y) * p->grid.x + cc.x]);
    if (b1 > 0) v1 = unpack_halves(cells[((b1 - 1) * p->grid.y + cc.y) * p->grid.x + cc.x]);
    sum += w * lerp(v0, v1, fz);
  }
  return sum;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void medium_apply_main(uint3 id : SV_DispatchThreadID) {
  MediumParams* p = (MediumParams*)g_push.params;
  if (id.x >= p->rect.z || id.y >= p->rect.w) return;
  const uint2 target = uint2(p->rect.x + id.x, p->rect.y + id.y);
  float4 color = g_storage_images[p->source.z][target];
  // Depth: the high 32 bits of the pixel's visibility word, reversed-Z; 0 where nothing was drawn.
  const uint64_t word = ((uint64_t*)p->visibility)[id.y * p->source.x + id.x];
  const float depth = asfloat(uint(word >> 32));
  const float2 ndc = float2((float(id.x) + 0.5) / float(p->rect.z) * 2.0 - 1.0,
                            1.0 - (float(id.y) + 0.5) / float(p->rect.w) * 2.0);
  const float3 dir = view_ray_direction(p->clip_to_ray, ndc);
  float distance = 3.0e38;
  if (depth > 0.0) {
    const float4 h = mul(p->clip_to_eye, float4(ndc, depth, 1.0));
    distance = length(h.xyz / h.w);
  }
  const float exposure = frame_exposure(p);
  float3 radiance = color.rgb;
  // The far clouds into the sky's pixels first: they stand behind every froxel.
  bool touched = false;
  if (depth <= 0.0 && (p->grid.w & k_source_clouds) != 0 && p->clouds != 0) {
    const float4 cloud = clouds_at(p, id.xy);
    radiance = radiance * cloud.a + cloud.rgb * exposure;
    touched = true;
  }
  // The froxels to the pixel, or to their far boundary and then the dust in closed form beyond it.
  const float2 uv = float2((float(id.x) + 0.5) / float(p->rect.z) * float(p->grid.x),
                           (float(id.y) + 0.5) / float(p->rect.w) * float(p->grid.y));
  const float far = p->slices.y;
  const float s = distance <= p->slices.x ? 0.0 : log(min(distance, far) / p->slices.x) * p->slices.w;
  const float4 inside = integrated_at(p, uv, s);
  float3 light = inside.rgb;
  float transmittance = inside.a;
  if (distance > far && (p->grid.w & k_source_dust) != 0) {
    const float tau = dust_optical_depth(p, dir.y, far, distance);
    const float tail_t = exp(-min(tau, 80.0));
    // Unshadowed past the froxels; the sun and the sky at the far boundary's point stand for the
    // rest of the ray.
    const float3 at = dir * far;
    const float3 tail = p->dust_albedo.rgb * source_light(p, at, dir, p->dust_albedo.w, 1.0);
    light += transmittance * tail * (1.0 - tail_t);
    transmittance *= tail_t;
  }
  if (!touched && transmittance >= 1.0 && all(light <= float3(0.0))) return;
  radiance = radiance * transmittance + light * exposure;
  g_storage_images[p->source.z][target] = float4(radiance, 1.0);
}
