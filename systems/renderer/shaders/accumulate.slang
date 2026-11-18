// The engine's own accumulation at native resolution (systems/renderer/src/accumulation_pass.h;
// docs/subsystems/renderer.md, "Sampling over time: the accumulation"; ADR-0064; roadmap R158).
// One thread a pixel between the frame passes and the encode: the frame's linear colour, drawn at
// this frame's sub-pixel offset, is blended into the picture the frames before it accumulated,
// looked up where the motion pass says the pixel's surface was, and written to an image the encode
// reads in the linear image's place.
//
// **What a history is held to** (the page's three guards, in this order):
//   1. *The motion of the nearest surface in the 3x3*: a thin feature's edge pixel takes the
//      feature's motion and not the background's, so the pixel a leaf has just left looks up where
//      the sky behind it was, and not where the leaf was.
//   2. *Whether anything can have been uncovered there*: a tap whose surface was moving last frame
//      as this pixel's is moving now (last frame's motion buffer, within `motion_match` pixels) is
//      this pixel's own, whatever it names: under a held camera, a pan or a sway under the match
//      nothing is uncovered, and the offset alone changes what an edge pixel names every frame.
//      Otherwise *the names*: last frame's visibility buffer says which instance the tap showed,
//      and it counts only if that instance (or the empty sky) is among the instances this frame's
//      3x3 holds. Not "the same instance at the same pixel", which would restart every edge pixel
//      every frame, and those are the pixels that hold nine tenths of the aliasing; and not the
//      names alone, which left a still view of four trees' crowns against a sky 12.6 codes from
//      the reference where the plain mean of the same sixteen frames is 6.0.
//   3. *The colours*: what is left is clipped towards the 3x3's mean, to its mean +/- gamma sigma in
//      YCoCg (Salvi 2016), with gamma wider where the pixel hardly moved.
//
// `mean` mode is none of that: the plain running mean of the frames since the last reset at the
// pixel's own place, so that the picture after one cycle of offsets is the mean of the cycle's
// frames, a function of the scene, which a test can hold to a reference.
//
// **The frames are averaged as the display shows them**, not as radiance: each sample goes through
// the display's curve (`display_tone1` and its ceiling: what `display_output` applies before the
// transfer) before it is blended, and the result goes back through the curve's inverse, so the
// encode after this pass shows exactly the blend. A mean of radiance would let one sample past the
// display's white own its pixel (a glint on a leaf among dark ones drew 11 codes brighter than
// the mean of the frames as drawn), and it would not be the supersampled picture, which is a mean
// of what is seen.

#include "../../../domain/gfx/shaders/display.slang"

[[vk::binding(1, 0)]] RWTexture2D<float4> g_storage_images[];

// Mirrors renderer::AccumulateParams (144 bytes).
struct AccumulateParams {
  uint64_t visibility;       // u64[width * height]: this view's region of this frame's buffer
  uint64_t prev_visibility;  // the same region of last frame's
  uint64_t motion;           // u32[width * height]: the motion pass's words for this view
  uint64_t pairs;            // u32[2 * pair_count]: the scene's {instance, cluster} per pair; 0: none
  uint64_t prev_motion;      // u32[width * height]: last frame's motion words for this view
  uint64_t moved;            // gfx::MovedInstance[moved_count] (64 bytes each), sorted by instance
  uint width;
  uint height;
  uint rect_x;  // the view's rectangle of the three images
  uint rect_y;
  uint color;    // storage slot: the frame's linear colour
  uint history;  // storage slot: what the frames before accumulated
  uint output;   // storage slot: what this frame leaves, which the encode reads
  uint flags;
  uint pair_count;
  float weight;       // this frame's share of a pixel whose history holds
  float clip_still;   // gamma of the colour clip for a pixel that did not move
  float clip_moving;  // and for one that moved a pixel or more; 0 or less: no clip
  float knee;         // the display curve's knee (the sky's shoulder; the ceiling with none)
  float ceiling;      // and its ceiling, in the linear image's units: 1 for an SDR picture
  float motion_match; // pixels: a tap whose surface moved within this of the pixel's is its own
  uint moved_count;
  // **Reactive surfaces** (R158 leg 3): a pixel whose colour is not its surface's alone — a water
  // surface's (`k_material_water`): its reflection and what it refracts move as the eye and the
  // waves do, not as the surface — trusts its history less: it is always clipped (`clip_moving`)
  // and takes at least `reactive_weight` of the new frame. The material of the pixel's pair is
  // `materials[cluster_materials[cluster] + instances[instance].material_base]`, as everywhere;
  // `materials` 0 (a scene with no such material) reads nothing.
  uint64_t materials;          // gfx::ResolveMaterial[] (112 bytes, flags at 44)
  uint64_t cluster_materials;  // u32 per cluster
  uint64_t instances;          // gfx::InstanceDesc[] (96 bytes, material_base at 64)
  float reactive_weight;
  uint filter;  // k_filter_*: how a history is read between its texels
};

static const uint k_filter_bilinear = 0u;
static const uint k_filter_catmull_rom = 1u;

// Whether `instance` is one the host has ever moved (`SceneRenderer::move_instances`): the motion
// pass's table, whose entries are 64 bytes with the instance first. Such an instance can jump
// between two frames, so what it was doing last frame says nothing of where it is now.
bool ever_moved(AccumulateParams* p, uint instance) {
  uint* table = (uint*)p->moved;
  uint lo = 0;
  uint hi = p->moved_count;
  while (lo < hi) {
    const uint mid = (lo + hi) >> 1;
    const uint at = table[mid * 16];
    if (at == instance) return true;
    if (at < instance) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return false;
}

static const uint k_accumulate_history = 1u;  // there is a frame before this one to read
static const uint k_accumulate_mean = 2u;     // the running mean at the pixel's own place
static const uint k_accumulate_names = 4u;    // last frame's names can be read through `pairs`

static const uint k_motion_invalid = 0x7c007c00u;  // gfx::k_motion_invalid
static const uint k_sky = 0xffffffffu;             // the instance of a pixel that holds nothing

struct AccumulatePush {
  uint64_t params;  // AccumulateParams*: this view's block
};
[[vk::push_constant]] ConstantBuffer<AccumulatePush> g_push;

float3 to_ycocg(float3 c) {
  return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 from_ycocg(float3 c) {
  return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// A sample as the display shows it, before the transfer: the curve and its ceiling. Alpha (the
// stand-in sky's mark) is not light and passes through.
float4 shown(AccumulateParams* p, float4 c) {
  const float3 e = max(c.rgb, 0.0);
  return float4(min(float3(display_tone1(e.x, p->knee, p->ceiling),
                           display_tone1(e.y, p->knee, p->ceiling),
                           display_tone1(e.z, p->knee, p->ceiling)),
                    p->ceiling),
                c.a);
}

// The radiance the curve shows as `y`: `display_tone1`'s inverse, the least such value at the
// ceiling, and bounded where the roll-off's inverse runs off (a half holds 65504).
float exposed1(float y, float knee, float ceiling) {
  if (y <= knee || knee >= ceiling) return y;
  const float span = ceiling - knee;
  return min(knee - span * log(max(1.0 - (y - knee) / span, 1e-9)), 65504.0);
}

float4 exposed(AccumulateParams* p, float4 c) {
  return float4(exposed1(c.x, p->knee, p->ceiling), exposed1(c.y, p->knee, p->ceiling),
                exposed1(c.z, p->knee, p->ceiling), c.a);
}

// The instance a visibility word names; the sky for an empty one. A pair past the table (a tail
// that shrank) names nothing this frame holds.
uint instance_of(AccumulateParams* p, uint64_t word) {
  if (word == 0) return k_sky;
  const uint pair = uint(word & 0xffffffffu) >> 8;
  if (p->pairs == 0) return 0;
  if (pair >= p->pair_count) return 0xfffffffeu;
  return ((uint2*)p->pairs)[pair].x;
}

// Whether the surface a visibility word names is a reactive one (above): its material's flags.
bool reactive(AccumulateParams* p, uint64_t word) {
  if (word == 0 || p->materials == 0 || p->pairs == 0) return false;
  const uint pair = uint(word & 0xffffffffu) >> 8;
  if (pair >= p->pair_count) return false;
  const uint2 at = ((uint2*)p->pairs)[pair];
  const uint base = *(uint*)(p->instances + uint64_t(at.x) * 96u + 64u);
  const uint material = ((uint*)p->cluster_materials)[at.y] + base;
  const uint flags = *(uint*)(p->materials + uint64_t(material) * 112u + 44u);
  return (flags & 16u) != 0;  // k_material_water
}

// Catmull-Rom's four weights for a sample `t` past the second of four texels: the cubic through
// the two inner texels with the outer ones setting its slopes (Catmull and Rom 1974). Karis 2014
// names the blur a bilinear history read leaves, "reprojection diffusion"; this is the usual
// answer to it, derived here and not taken from a source read for this row.
float4 catmull_rom_weights(float t) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t,
                0.5 * t3 - 0.5 * t2);
}

// **The sharper read** (R158 leg 3): the history at `base + t` through the 4x4 texels round it,
// Catmull-Rom in both axes, as the display shows them. Only where the four bilinear taps all held
// (the caller's test), so every texel it weighs is of a surface the pixel kept; the outer ring is
// not tested by name, and its weight is at most 0.07 a texel. The result is held between the four
// inner texels' least and greatest, so an edge does not ring. False when a texel is off the view
// or not a number: the bilinear read stands.
bool history_catmull_rom(AccumulateParams* p, int2 corner, int2 size, float2 base, float2 t,
                         out float4 result) {
  result = float4(0.0, 0.0, 0.0, 0.0);
  const int2 first = int2(base) - 1;
  if (first.x < 0 || first.y < 0 || first.x + 3 >= size.x || first.y + 3 >= size.y) return false;
  const float4 wx = catmull_rom_weights(t.x);
  const float4 wy = catmull_rom_weights(t.y);
  float4 lo = float4(65504.0, 65504.0, 65504.0, 65504.0);
  float4 hi = -lo;
  for (int j = 0; j < 4; ++j) {
    float4 row = float4(0.0, 0.0, 0.0, 0.0);
    for (int i = 0; i < 4; ++i) {
      const float4 h = g_storage_images[p->history][corner + first + int2(i, j)];
      if (!all(h == h) || any(abs(h) > 65504.0)) return false;
      const float4 s = shown(p, h);
      row += s * wx[i];
      if (i >= 1 && i <= 2 && j >= 1 && j <= 2) {
        lo = min(lo, s);
        hi = max(hi, s);
      }
    }
    result += row * wy[j];
  }
  result = clamp(result, lo, hi);
  return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void accumulate_main(uint3 id : SV_DispatchThreadID) {
  AccumulateParams* p = (AccumulateParams*)g_push.params;
  if (id.x >= p->width || id.y >= p->height) return;
  const int2 local = int2(id.xy);
  const int2 size = int2(p->width, p->height);
  const int2 corner = int2(p->rect_x, p->rect_y);
  const int2 at = corner + local;
  const float4 colour = shown(p, g_storage_images[p->color][at]);
  float4 result = colour;
  if ((p->flags & k_accumulate_mean) != 0) {
    if ((p->flags & k_accumulate_history) != 0) {
      const float4 before = shown(p, g_storage_images[p->history][at]);
      result = before + (colour - before) * p->weight;
    }
    g_storage_images[p->output][at] = exposed(p, result);
    return;
  }
  if ((p->flags & k_accumulate_history) != 0) {
    // The 3x3 round the pixel: its colours' mean and spread, the nearest surface (reversed-Z, so
    // the largest depth; an empty pixel is the farthest) and the instances it holds.
    uint64_t* now = (uint64_t*)p->visibility;
    const bool names = (p->flags & k_accumulate_names) != 0;
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 sum2 = float3(0.0, 0.0, 0.0);
    float alpha_lo = colour.a;
    float alpha_hi = colour.a;
    uint held[9];
    float nearest = -1.0;
    int2 nearest_at = local;
    for (int k = 0; k < 9; ++k) {
      const int2 n = clamp(local + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), size - 1);
      const float4 c = k == 4 ? colour : shown(p, g_storage_images[p->color][corner + n]);
      const float3 y = to_ycocg(c.rgb);
      sum += y;
      sum2 += y * y;
      alpha_lo = min(alpha_lo, c.a);
      alpha_hi = max(alpha_hi, c.a);
      const uint64_t word = now[n.y * size.x + n.x];
      held[k] = names ? instance_of(p, word) : (word == 0 ? k_sky : 0u);
      const float depth = asfloat(uint(word >> 32));
      if (depth > nearest) {
        nearest = depth;
        nearest_at = n;
      }
    }
    const uint packed = ((uint*)p->motion)[nearest_at.y * size.x + nearest_at.x];
    if (packed != k_motion_invalid) {
      const float2 motion = float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16));
      // Where the pixel's centre was, in last frame's picture; its four neighbours there.
      const float2 was = float2(local) - motion;
      const float2 base = floor(was);
      const float2 t = was - base;
      uint64_t* before = (uint64_t*)p->prev_visibility;
      float4 history = float4(0.0, 0.0, 0.0, 0.0);
      float total = 0.0;
      for (int k = 0; k < 4; ++k) {
        const int2 tap = int2(base) + int2(k & 1, k >> 1);
        if (tap.x < 0 || tap.y < 0 || tap.x >= size.x || tap.y >= size.y) continue;
        const float w = ((k & 1) != 0 ? t.x : 1.0 - t.x) * ((k >> 1) != 0 ? t.y : 1.0 - t.y);
        if (w <= 0.0) continue;
        // A tap is this pixel's own if what it showed was moving as this pixel's surface is (a
        // held view, a pan, a crown whose sway is under the match: nothing can have been
        // uncovered), or else if what it showed is among what the 3x3 holds now.
        // Not for an instance the host moves, which may have stood still last frame and be
        // somewhere else in this one: its place is the names' to decide, always.
        const uint moved = ((uint*)p->prev_motion)[tap.y * size.x + tap.x];
        const uint64_t word = before[tap.y * size.x + tap.x];
        const uint was_held = names ? instance_of(p, word) : (word == 0 ? k_sky : 0u);
        bool known = false;
        if (moved != k_motion_invalid && p->motion_match > 0.0 &&
            (p->moved_count == 0 || (names && !ever_moved(p, was_held)))) {
          const float2 apart = float2(f16tof32(moved & 0xffffu), f16tof32(moved >> 16)) - motion;
          known = dot(apart, apart) <= p->motion_match * p->motion_match;
        }
        if (!known) {
          for (int j = 0; j < 9; ++j)
            known = known || held[j] == was_held;
        }
        if (!known) continue;
        const float4 h = g_storage_images[p->history][corner + tap];
        // A value that is not a number (a frame that shaded one) must not be kept for ever.
        if (!all(h == h) || any(h > 65504.0)) continue;
        history += shown(p, h) * w;
        total += w;
      }
      if (total > 1e-3) {
        history /= total;
        // All four taps held and the read is the sharper one: the bilinear read softens a picture
        // that moves by fractions of a pixel over the blend's length (a slow pan, a sway).
        float4 sharp;
        if (p->filter == k_filter_catmull_rom && total > 0.999 &&
            history_catmull_rom(p, corner, size, base, t, sharp)) {
          history = sharp;
        }
        // A pixel whose surface moved no further than the match is clipped by `clip_still`, which
        // is none by default: its history is its own, and a clip there pulls every thin feature
        // the nine samples of this frame happened to miss back out of the picture. A reactive
        // surface is clipped as a moving one is, whatever it did, and weighs the new frame more.
        const bool shifting = reactive(p, now[local.y * size.x + local.x]);
        const float gamma =
            shifting || dot(motion, motion) > p->motion_match * p->motion_match ? p->clip_moving
                                                                                 : p->clip_still;
        const float weight = shifting ? max(p->weight, p->reactive_weight) : p->weight;
        if (gamma > 0.0) {
          const float3 mean = sum / 9.0;
          const float3 extent = gamma * sqrt(max(sum2 / 9.0 - mean * mean, 0.0)) + 1e-6;
          // Towards the mean until it is inside the box: the clip that keeps the hue.
          const float3 offset = to_ycocg(history.rgb) - mean;
          const float3 over = abs(offset) / extent;
          const float most = max(over.x, max(over.y, over.z));
          if (most > 1.0) history.rgb = from_ycocg(mean + offset / most);
          history.a = clamp(history.a, alpha_lo, alpha_hi);
        }
        result = history + (colour - history) * weight;
      }
    }
  }
  g_storage_images[p->output][at] = exposed(p, max(result, 0.0));
}
