// **The eye's adaptation: the frame metered and its exposure stepped** (systems/renderer/src/
// exposure_pass.h; systems/renderer/exposure.h is the same model on the CPU; docs/subsystems/
// renderer.md, "Exposure: the eye's adaptation"; roadmap R168).
//
// `exposure_histogram_main`: one thread for one pixel in each 2 x 2 tile of the metered view of the
// linear image the resolve wrote (the exposed radiance), its luminance taken back to cd/m^2 through
// the exposure the resolve drew at, binned in quarter stops and counted with its centre weight —
// in workgroup memory, then into the frame's bins by integer atomics, so the counts do not depend
// on the order the threads ran in.
//
// `exposure_adapt_main`: one workgroup of `k_bins` threads. Each reads its bin and clears it for the
// next frame; then one thread walks the bins in order, averages the log luminance of the weight
// between the low and high shares, and steps the state towards the exposure that shows that
// luminance at the eye's key. One thread's loop in a fixed order: the same bins give the same bits.

#include "../../../domain/gfx/shaders/sky.slang"

[[vk::binding(1, 0)]] RWTexture2D<float4> g_storage_images[];

static const uint k_bins = 128;  // k_exposure_bins
static const float k_log2_min = -10.0;
static const float k_bins_per_stop = 4.0;
static const uint k_stride = 2;
static const uint k_centre_weight = 16;
static const float k_centre_diameter = 1.0 / 3.0;
static const float k_stand_in_ev100 = 15.0;
static const uint k_snap = 1u;

// Mirrors renderer::ExposureState (exposure.h; 32 bytes).
struct ExposureState {
  float ev100;
  float target_ev100;
  float luminance;
  float base_ev100;
  float scale;
  uint frames;
  uint weight;
  uint pinned;
};

// Mirrors renderer::ExposureParams (exposure_pass.h; 96 bytes).
struct ExposureParams {
  uint64_t histogram;  // uint[k_bins]
  uint64_t state;      // ExposureState*
  uint64_t report;     // ExposureState*: this slot's copy
  uint64_t sky;        // SkyParams*, 0 for the stand-in
  uint rect_x;
  uint rect_y;
  uint rect_width;
  uint rect_height;
  uint color;  // storage slot of the linear image
  uint flags;
  float dt_s;
  float low;
  float high;
  float min_ev100;
  float max_ev100;
  float dark_tau_s;
  float cone_tau_s;
  float rod_tau_s;
  uint pad0;
  uint pad1;
};

struct ExposurePush {
  uint64_t params;  // ExposureParams*
};

[[vk::push_constant]] ConstantBuffer<ExposurePush> g_push;

// The exposure the resolve drew the picture at: the sky's frame (its rule, or a fixed value), or
// the stand-in's noon.
float base_ev100(ExposureParams* p) {
  if (p->sky == 0) return k_stand_in_ev100;
  SkyParams* sky = (SkyParams*)p->sky;
  SkyFrame* frame = (SkyFrame*)sky->frame;
  return frame->exposure.y;
}

groupshared uint g_bins[k_bins];

[numthreads(16, 16, 1)]
void exposure_histogram_main(uint3 id : SV_DispatchThreadID, uint local : SV_GroupIndex) {
  ExposureParams* p = (ExposureParams*)g_push.params;
  if (local < k_bins) g_bins[local] = 0;
  GroupMemoryBarrierWithGroupSync();
  const uint2 at = id.xy * k_stride;
  if (at.x < p->rect_width && at.y < p->rect_height) {
    const uint2 pixel = uint2(p->rect_x, p->rect_y) + at;
    const float3 exposed = max(g_storage_images[p->color][pixel].rgb, 0.0);
    // The exposed value back to cd/m^2: the resolve shows L at L pi / (2.5 2^EV100).
    const float y = dot(exposed, float3(0.2126, 0.7152, 0.0722));
    const float log2_l = log2(max(y, 1e-30)) + log2(2.5 / k_sky_pi) + base_ev100(p);
    const int bin = int(floor((log2_l - k_log2_min) * k_bins_per_stop));
    const uint b = uint(clamp(bin, 0, int(k_bins) - 1));
    // The centre's disc: a third of the view's width across.
    const float2 centre = float2(p->rect_width, p->rect_height) * 0.5;
    const float2 d = float2(at) + 0.5 - centre;
    const float radius = float(p->rect_width) * k_centre_diameter * 0.5;
    const uint w = dot(d, d) <= radius * radius ? k_centre_weight : 1u;
    InterlockedAdd(g_bins[b], w);
  }
  GroupMemoryBarrierWithGroupSync();
  if (local < k_bins && g_bins[local] != 0) {
    uint* bins = (uint*)p->histogram;
    InterlockedAdd(bins[local], g_bins[local]);
  }
}

// exposure.h's model, the same arithmetic.
float exposure_key(float luminance) {
  return 1.03 - 2.0 / (2.0 + log10(luminance + 1.0));
}

float target_ev100(float luminance, float compensation, ExposureParams* p) {
  // The adapting luminance is shown where Reinhard's operator puts it: a / (1 + a).
  const float key = max(exposure_key(luminance), 1e-3);
  const float shown = key / (1.0 + key);
  const float ev = log2(k_sky_pi * luminance / (2.5 * shown)) - compensation;
  return clamp(ev, p->min_ev100, p->max_ev100);
}

[numthreads(128, 1, 1)]
void exposure_adapt_main(uint local : SV_GroupIndex) {
  ExposureParams* p = (ExposureParams*)g_push.params;
  uint* bins = (uint*)p->histogram;
  g_bins[local] = bins[local];
  bins[local] = 0;
  GroupMemoryBarrierWithGroupSync();
  if (local != 0) return;

  ExposureState* state = (ExposureState*)p->state;
  ExposureState s = state[0];
  const bool snap = (p->flags & k_snap) != 0;
  if (snap) s.frames = 0;
  const float base = base_ev100(p);
  // A fixed exposure value pins the frame: the sky's block says so (x the mode, y the value).
  bool pinned = false;
  float compensation = 0.0;
  if (p->sky != 0) {
    SkyParams* sky = (SkyParams*)p->sky;
    pinned = sky->exposure.x > 0.5;
    compensation = sky->exposure.y;
  }

  uint total = 0;
  for (uint b = 0; b < k_bins; ++b)
    total += g_bins[b];
  if (pinned) {
    s.ev100 = base;
    s.target_ev100 = base;
    s.frames = 0;  // the next frame not pinned starts from its own target
  } else if (total > 0) {
    // The mean of log2 L over the weight between the low and the high shares, in bin order.
    const float lo = p->low * float(total);
    const float hi = p->high * float(total);
    uint below = 0;
    float sum = 0.0;
    float count = 0.0;
    for (uint b = 0; b < k_bins; ++b) {
      const uint above = below + g_bins[b];
      const float inside = clamp(float(above), lo, hi) - clamp(float(below), lo, hi);
      sum += inside * (k_log2_min + (float(b) + 0.5) / k_bins_per_stop);
      count += inside;
      below = above;
    }
    // A window that holds no weight (low at or above high) reads the darkest bin.
    const float log2_l = count > 0.0 ? sum / count : k_log2_min;
    const float luminance = exp2(log2_l);
    const float target = target_ev100(luminance, compensation, p);
    s.luminance = luminance;
    s.target_ev100 = target;
    if (s.frames == 0) {
      s.ev100 = target;
    } else {
      // Darkening the picture (the target higher) is the eye's light adaptation, blended between
      // the cones' and the rods' by the rods' share; brightening it is dark adaptation.
      const float rods = 0.04 / (0.04 + luminance);
      const float tau = target < s.ev100 ? p->dark_tau_s
                                         : rods * p->rod_tau_s + (1.0 - rods) * p->cone_tau_s;
      const float k = tau > 0.0 ? 1.0 - exp(-p->dt_s / tau) : 1.0;
      s.ev100 = s.ev100 + (target - s.ev100) * k;
    }
    s.frames += 1;
  } else if (s.frames == 0) {
    // Nothing metered and nothing before it (an empty view): the resolve's own exposure.
    s.ev100 = base;
    s.target_ev100 = base;
  }
  s.base_ev100 = base;
  s.scale = pinned ? 1.0 : clamp(exp2(base - s.ev100), 1.0 / 16777216.0, 16777216.0);
  s.weight = total;
  s.pinned = pinned ? 1u : 0u;
  state[0] = s;
  ExposureState* report = (ExposureState*)p->report;
  report[0] = s;
}
