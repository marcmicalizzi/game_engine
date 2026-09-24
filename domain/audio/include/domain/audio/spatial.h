#pragma once

// The source as an object, and the model that turns it into what the decode stage needs
// (docs/subsystems/audio.md, "Sources are objects").
//
// A playing voice is a **source**: a clip and a `SourceSpatial` block — position, orientation,
// directivity, spread, distance model, and whether it is a 2D (non-diegetic) source — and never a
// pair of channel gains. Two stages turn that into speaker feeds, and each is a seam:
//
//   1. **The source model** — `spatialize()` here — reads the block and the listener and produces a
//      `SpatialParams`: which way the source is in listener space, how loud distance and
//      directivity leave it, how wide it is. Steam Audio (docs/plan/05-simulation.md §5.11) is
//      meant to fill the same `SpatialParams` from its own simulation — adding occlusion,
//      transmission and air absorption to the attenuation — so the mixer does not change when it
//      arrives.
//   2. **The decode stage** — decoder.h — turns a source's signal and its `SpatialParams` into the
//      declared layout's channels. That is where panning lives, and the only place that knows how
//      many speakers there are.
//
// The block is complete now even where the v0 decoder ignores a field: the stereo panner uses the
// direction's lateral component and the spread, and a VBAP or ambisonic decoder will use the rest
// of the direction; orientation and directivity already shape the level through the cone.
//
// **Determinism.** Everything here is +, -, *, / and sqrt on f32, which IEEE 754 makes the same on
// every compiler and C library once contraction is off (ADR-0035). The pan law and the cone need
// trigonometry and do not call the C library for it: `sin_quarter` and `cos_degrees` are fixed
// polynomials, because `std::sin` is allowed to differ between MSVC and glibc in the last bit —
// which would make the mix, and the hash the determinism test pins, a property of the toolchain.

#include <core/base/types.h>
#include <core/math/math.h>

#include <schemas/audio.h>

namespace engine::audio {

// Set on `SourceSpatial::flags`: a non-diegetic source (music, interface, narration). It bypasses
// the source model — no position, no attenuation — and the decode stage maps its channels onto
// the layout by its `ChannelMapping` and pan instead of spatializing it.
inline constexpr u8 k_source_2d = 1u << 0;

// The per-source spatial block: the object, all the way down.
struct SourceSpatial {
  Vec3 position;
  // The source's facing is its -z axis (core/math: -z ahead). Unit length; the producer
  // normalizes it.
  Quat orientation;
  // Full level at and inside `min_distance` (clamped to at least 1 mm); what happens between it
  // and `max_distance` is `distance_model`'s.
  f32 min_distance = 1.0f;
  f32 max_distance = 50.0f;
  // The cone, as cosines of its half-angles so the audio thread never takes a trigonometric
  // function: 1 is a zero-width cone, -1 is a full sphere. `make_cone` converts from degrees.
  f32 cone_inner_cos = -1.0f;
  f32 cone_outer_cos = -1.0f;
  f32 cone_outer_gain = 1.0f;
  // 0 is a point; 1 fills the layout.
  f32 spread = 0.0f;
  DistanceModel distance_model = DistanceModel::InverseTapered;
  Directivity directivity = Directivity::Omni;
  ChannelMapping mapping = ChannelMapping::Inherit;
  u8 flags = 0;  // k_source_2d
};

struct Cone {
  f32 inner_cos = -1.0f;
  f32 outer_cos = -1.0f;
};

// Full cone widths in degrees (OpenAL's convention: 360 is omnidirectional) to the cosines of the
// half-angles, through `cos_degrees`. The outer cone is widened to the inner one if narrower.
Cone make_cone(f32 inner_degrees, f32 outer_degrees) noexcept;

// The listener as the producer hands it over: a position and an orientation that need not be
// normalized. A zero `forward` means -z and a zero or parallel `up` means +y (core/math's
// convention: right-handed, y up, -z ahead).
struct Listener {
  Vec3 position;
  Vec3 forward{0.0f, 0.0f, -1.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};
};

// The same listener reduced once, on the controlling thread, to an orthonormal basis, so the audio
// thread never normalizes a vector per source per block.
struct ListenerBasis {
  Vec3 position;
  Vec3 right{1.0f, 0.0f, 0.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};
  Vec3 forward{0.0f, 0.0f, -1.0f};
};

ListenerBasis make_listener_basis(const Listener& listener) noexcept;

// **The seam between the source model and the decode stage.** What the source model produces for
// one source, per block.
struct SpatialParams {
  // Unit vector toward the source in listener space: +x right, +y up, -z ahead. Zero when the
  // source sits on the listener, which a decoder treats as "everywhere at once".
  Vec3 direction;
  // Distance and directivity gain in [0, 1]; later also occlusion and transmission.
  f32 attenuation = 1.0f;
  // Copied from the block: how much of the layout the source fills.
  f32 spread = 0.0f;
};

// The distance laws (`DistanceModel`), for `min < d < max`; full level at and inside `min`:
//
//   InverseTapered   (min / d) * (max - d) / (max - min)     silent at max
//   Linear           (max - d) / (max - min)                 silent at max
//   None             1
//
// Inverse distance is the free-field law (-6 dB per doubling). The taper is there so that the
// level *reaches zero* at `max_distance` rather than stopping at min/max and staying audible for
// ever, which is what lets the LOD policy take a voice away from an emitter past its
// `max_distance` without anyone hearing it go. Both are continuous and never rise with distance.
f32 distance_gain(DistanceModel model, f32 distance, f32 min_distance, f32 max_distance) noexcept;

// The cone: 1 inside the inner cone, `outer_gain` outside the outer one, and linear in the cosine
// between — monotonic in the angle off the source's axis, and free of any inverse cosine.
// `cos_off_axis` is the cosine of the angle between the source's facing and the direction from
// the source to the listener.
f32 cone_gain(f32 cos_off_axis, f32 inner_cos, f32 outer_cos, f32 outer_gain) noexcept;

// The source model: fills the seam from the block and the listener. A 2D source gets the neutral
// params (no direction, attenuation 1): it is not in the world.
SpatialParams spatialize(const SourceSpatial& source, const ListenerBasis& listener) noexcept;

// sin(pi/2 * t) for t in [0, 1]: a fixed Taylor polynomial through the x^11 term, evaluated in f32
// in one order, with the ends pinned (0 at t <= 0, 1 at t >= 1). Its truncation error is 5.7e-8
// and it is within a few f32 ulps of the true sine across the range.
f32 sin_quarter(f32 t) noexcept;

// cos(degrees) for any angle, through `sin_quarter`. Deterministic, like everything here.
f32 cos_degrees(f32 degrees) noexcept;

struct PanGains {
  f32 left = 1.0f;
  f32 right = 1.0f;
};

// A mono signal between two speakers: constant power, left = cos(theta) and right = sin(theta)
// with theta = (pan + 1) * pi/4, so the sum of the squares is 1 wherever the source sits and the
// centre is -3 dB in each channel. `pan` is clamped to [-1, 1].
PanGains pan_constant_power(f32 pan) noexcept;

// A stereo signal: balance, unity at the centre. Panning right attenuates the left channel
// linearly and leaves the right alone. A stereo clip already carries its own image, and a constant
// power law would take 3 dB off a centred music bed for nothing.
PanGains pan_balance(f32 pan) noexcept;

}  // namespace engine::audio
