// The engine's own accumulation at native resolution (docs/subsystems/renderer.md, "Sampling over
// time: the accumulation"; ADR-0064; roadmap R158).
//
// **What the accumulated picture is held to.** In `mean` mode a capture draws one whole cycle of
// sub-pixel offsets from a reset and leaves the plain mean of those frames, so the picture is a
// function of the scene and the camera, and the reference is made here from the same frames drawn
// one by one with the inputs on and no accumulation: decoded, averaged in linear light, encoded
// again. The two agree within two codes, and that bound is rounding, not noise: each of the
// reference's frames is eight bits (half a code each, and no more than half a code in their mean,
// since the transfer is convex), the accumulated picture is eight bits (half a code), and the
// accumulated frames went through the half-float linear image and its encode, which is a code from
// the resolve's own encode on under 2 % of channels and never more (frame_pass_tests.cpp).
//
// The blend a session would run (`on`) is held to that mean by what it is for (a held view's error
// against the mean is under half a single frame's), to a replay (the same frames from a reset draw
// the same bytes), to a reset (the frame after one is the frame a fresh renderer draws) and to its
// history test (the pixels a moved instance leaves show what is behind it, not a trail).
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <domain/gfx/display.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
  ~Gpu() { device.destroy(); }
};

struct ScopedTunable {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  ScopedTunable(const char* name, const char* value) {
    tunable = tunables::find(name);
    REQUIRE_MESSAGE(tunable != nullptr, name);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(value));
  }
  ~ScopedTunable() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
};

constexpr u32 k_width = 256;
constexpr u32 k_height = 144;
constexpr u32 k_pixels = k_width * k_height;
constexpr u32 k_phases = 8;

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc,
             RasterMode raster = RasterMode::Hardware) {
    if (!load_scene(desc, data, error)) return false;
    // The picture's own rasterizer on whatever the device is (mesh shaders, or the vertex tier), no
    // shadows, and no dither: a difference of frames has to be the offsets' alone.
    RenderSettings settings;
    settings.raster = raster;
    settings.shadows = ShadowMode::Off;
    settings.dither = false;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      error = unavailable_reason(availability, device);
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    rd.frames_in_flight = 2;
    return renderer.create(device, scene, resolved, rd, &error);
  }

  bool shoot(const FrameDesc& frame, CapturedFrame& out, bool ids = false) {
    CaptureChannels channels;
    channels.ids = ids;
    return renderer.capture(frame, channels, out, &error);
  }
};

SceneDesc ground_scene() {
  SceneDesc desc;
  desc.meshes.push_back("");
  return desc;
}

FrameDesc held_frame(const Rig& rig) {
  FrameDesc frame;
  frame.camera = orbit_camera_at(rig.data.center, rig.data.radius, 22.0f, 0.4f, k_orbit_pitch);
  return frame;
}

// The mean of `frames` in linear light, encoded again: the reference a `mean` capture is held to.
Vector<u8> mean_of(const Vector<CapturedFrame>& frames) {
  Vector<u8> out(k_pixels * 4);
  for (u32 i = 0; i < k_pixels * 4; ++i) {
    f64 sum = 0.0;
    for (const CapturedFrame& frame : frames)
      sum += static_cast<f64>(gfx::sdr_decode(static_cast<f32>(frame.color[i]) / 255.0f));
    const f64 mean = sum / static_cast<f64>(frames.size());
    out[i] = static_cast<u8>(std::lround(255.0 * std::pow(mean, 1.0 / 2.2)));
  }
  return out;
}

struct Difference {
  u32 worst = 0;       // codes, over the three colour channels of every pixel
  f64 mean_abs = 0.0;  // codes
  f64 rms = 0.0;       // codes
  u32 over8 = 0;       // channels more than eight codes apart
};

Difference difference(const Vector<u8>& a, const Vector<u8>& b) {
  Difference d;
  f64 sum = 0.0;
  f64 sum2 = 0.0;
  for (u32 p = 0; p < k_pixels; ++p) {
    for (u32 c = 0; c < 3; ++c) {
      const i32 delta = static_cast<i32>(a[p * 4 + c]) - static_cast<i32>(b[p * 4 + c]);
      const u32 apart = static_cast<u32>(delta < 0 ? -delta : delta);
      d.worst = apart > d.worst ? apart : d.worst;
      d.over8 += apart > 8 ? 1u : 0u;
      sum += apart;
      sum2 += static_cast<f64>(apart) * apart;
    }
  }
  d.mean_abs = sum / (k_pixels * 3.0);
  d.rms = std::sqrt(sum2 / (k_pixels * 3.0));
  return d;
}

// The cycle's frames drawn one by one by a renderer with the inputs and no accumulation: frame k
// from a reset is drawn at the k-th offset.
bool cycle_frames(const gfx::Device& device, Vector<CapturedFrame>& frames, std::string& why) {
  ScopedTunable inputs("renderer.reconstruction.inputs", "true");
  Rig rig;
  if (!rig.build(device, ground_scene())) {
    why = rig.error;
    return false;
  }
  if (rig.resolved.accumulation != Accumulation::Off || !rig.renderer.reconstruction().native()) {
    why = "the reference renderer accumulates";
    return false;
  }
  FrameDesc frame = held_frame(rig);
  frames.resize(k_phases);
  for (u32 k = 0; k < k_phases; ++k) {
    frame.history_reset = k == 0;
    if (!rig.shoot(frame, frames[k])) {
      why = rig.error;
      return false;
    }
  }
  return true;
}

}  // namespace

// Off: nothing of it exists, which is what every other test of the renderer runs as.
TEST_CASE("accumulation: off by default, and then the renderer keeps nothing of it") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, ground_scene()), rig.error);
  CHECK(rig.resolved.accumulation == Accumulation::Off);
  CHECK_FALSE(rig.resolved.reconstruction_inputs);
  CHECK(rig.renderer.accumulation_bytes() == 0);
  CHECK(rig.renderer.motion_bytes() == 0);
  CHECK(rig.renderer.reconstruction().native());
}

// The converged frame is a function of the scene: a `mean` capture against the cycle's frames
// averaged here. Fails without the offset reaching the frames (the mean would be one frame), with a
// weight other than one over the count, and with a history read anywhere but the pixel's own place.
TEST_CASE("accumulation: a mean capture is the mean of the cycle's offset frames") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  std::string why;
  Vector<CapturedFrame> frames;
  REQUIRE_MESSAGE(cycle_frames(gpu.device, frames, why), why);
  const Vector<u8> reference = mean_of(frames);

  ScopedTunable mode("renderer.reconstruction.accumulation", "mean");
  ScopedTunable phases("renderer.reconstruction.phases", "8");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, ground_scene()), rig.error);
  REQUIRE(rig.resolved.accumulation == Accumulation::Mean);
  CHECK(rig.resolved.reconstruction_inputs);  // the accumulation turns its inputs on
  // ADR-0064: an accumulated frame is a reconstructed frame, whatever its size.
  CHECK(rig.renderer.reconstruction().provider == ReconstructionProvider::Engine);
  CHECK_FALSE(rig.renderer.reconstruction().native());
  CHECK(rig.renderer.accumulation_bytes() >= u64{k_pixels} * 16);
  const FrameDesc frame = held_frame(rig);
  CapturedFrame mean;
  REQUIRE_MESSAGE(rig.shoot(frame, mean), rig.error);
  const Difference apart = difference(mean.color, reference);
  const Difference single = difference(frames[0].color, reference);
  MESSAGE("mean capture against the reference: worst "
          << apart.worst << " codes, mean " << apart.mean_abs << "; one frame against it: worst "
          << single.worst << ", rms " << single.rms << ", " << single.over8
          << " channels over 8 codes");
  // The fixture's premise, not a bound on the pass: the offsets change the picture at its edges.
  CHECK(single.over8 > 50);
  CHECK(apart.worst <= 2);  // rounding: the file's first comment has the sum
  CHECK(apart.mean_abs <= 0.5);
  // A second capture is the same cycle from a reset: the same bytes.
  CapturedFrame again;
  REQUIRE_MESSAGE(rig.shoot(frame, again), rig.error);
  CHECK(std::memcmp(mean.color.data(), again.color.data(), mean.color.size()) == 0);
}

// The blend a session runs, on a held view: sixteen frames from a reset are nearer the mean than
// one frame is by more than half, a replay draws the same bytes, and the frame after a reset is the
// frame a renderer that never drew anything else draws.
TEST_CASE("accumulation: a held view converges, replays to the byte and restarts at a reset") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  std::string why;
  Vector<CapturedFrame> frames;
  REQUIRE_MESSAGE(cycle_frames(gpu.device, frames, why), why);
  const Vector<u8> reference = mean_of(frames);

  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  ScopedTunable phases("renderer.reconstruction.phases", "8");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, ground_scene()), rig.error);
  REQUIRE(rig.resolved.accumulation == Accumulation::On);
  FrameDesc frame = held_frame(rig);
  CapturedFrame first;
  CapturedFrame held;
  for (u32 k = 0; k < 16; ++k) {
    frame.history_reset = k == 0;
    REQUIRE_MESSAGE(rig.shoot(frame, k == 0 ? first : held), rig.error);
  }
  const Difference one = difference(first.color, reference);
  const Difference sixteen = difference(held.color, reference);
  MESSAGE("against the cycle's mean, rms codes: one frame "
          << one.rms << ", sixteen accumulated " << sixteen.rms << " (worst " << sixteen.worst
          << ")");
  CHECK(one.rms > 1.0);  // the fixture's premise: one frame is not the mean
  CHECK(sixteen.rms <= 0.5 * one.rms);

  // A replay: the same sixteen frames from a reset.
  CapturedFrame replay;
  for (u32 k = 0; k < 16; ++k) {
    frame.history_reset = k == 0;
    REQUIRE_MESSAGE(rig.shoot(frame, replay), rig.error);
  }
  CHECK(std::memcmp(held.color.data(), replay.color.data(), held.color.size()) == 0);

  // A reset after another view: nothing of that view is left in the frame.
  FrameDesc elsewhere = frame;
  elsewhere.camera = orbit_camera_at(rig.data.center, rig.data.radius, 200.0f, 0.4f, k_orbit_pitch);
  elsewhere.history_reset = false;
  CapturedFrame other;
  for (u32 k = 0; k < 4; ++k)
    REQUIRE_MESSAGE(rig.shoot(elsewhere, other), rig.error);
  frame.history_reset = true;
  CapturedFrame restarted;
  REQUIRE_MESSAGE(rig.shoot(frame, restarted), rig.error);
  CHECK(std::memcmp(first.color.data(), restarted.color.data(), first.color.size()) == 0);
}

// The history test: an instance that moves across the picture leaves no trail. The pixels it
// covered and no longer covers show what is behind it in the very next frame, as near what they
// converge to as a quarter of what the mover's own colour was from it; a blend that kept the
// history there would leave nine tenths. Fails with the names' test removed.
TEST_CASE("accumulation: the pixels a moved instance leaves hold no trail of it") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  SceneDesc desc;
  desc.meshes.push_back("");
  SceneInstance ground;
  desc.instances.push_back(ground);
  SceneInstance mover;
  mover.transform.position = Vec3{0.0f, 2.0f, 0.0f};
  mover.transform.scale = Vec3{0.25f, 0.25f, 0.25f};
  desc.instances.push_back(mover);
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc), rig.error);

  FrameDesc frame = held_frame(rig);
  CapturedFrame before;
  for (u32 k = 0; k < 16; ++k) {
    frame.history_reset = k == 0;
    REQUIRE_MESSAGE(rig.shoot(frame, before, true), rig.error);
  }
  SceneInstance placed = mover;
  placed.transform.position = mover.transform.position + Vec3{rig.data.radius * 0.25f, 0.0f, 0.0f};
  const u32 index = 1;
  std::string why;
  REQUIRE_MESSAGE(rig.renderer.move_instances(std::span<const u32>(&index, 1),
                                              std::span<const SceneInstance>(&placed, 1), &why),
                  why);
  CapturedFrame after;
  REQUIRE_MESSAGE(rig.shoot(frame, after, true), rig.error);
  CapturedFrame settled;
  for (u32 k = 0; k < 24; ++k)
    REQUIRE_MESSAGE(rig.shoot(frame, settled, true), rig.error);

  // The pixels the mover left whose colour under it was far from what they settle to.
  const auto luma = [](const Vector<u8>& color, u32 p) {
    return 0.2126 * color[p * 4] + 0.7152 * color[p * 4 + 1] + 0.0722 * color[p * 4 + 2];
  };
  u32 left = 0;
  u32 counted = 0;
  f64 was = 0.0;
  f64 is = 0.0;
  for (u32 p = 0; p < k_pixels; ++p) {
    if (before.ids[p * k_id_words] != 1 || after.ids[p * k_id_words] == 1 ||
        settled.ids[p * k_id_words] == 1) {
      continue;
    }
    ++left;
    const f64 under = std::abs(luma(before.color, p) - luma(settled.color, p));
    if (under < 16.0) continue;
    ++counted;
    was += under;
    is += std::abs(luma(after.color, p) - luma(settled.color, p));
  }
  MESSAGE("the mover left " << left << " pixels, " << counted
                            << " of them 16 codes or more from what they settle to: mean "
                            << (counted > 0 ? was / counted : 0.0) << " codes under the mover, "
                            << (counted > 0 ? is / counted : 0.0) << " the frame after");
  REQUIRE(counted > 30);  // the fixture's premise
  CHECK(is <= 0.25 * was);
}

// The vertex tier runs it: the pass reads buffers and images and no rasterizer, and the vertex
// tier's visibility buffer is the mesh path's word for word (the vertex path tests), so the same
// frames accumulate to the same bytes. `RasterMode::Vertex` takes the vertex tier on a card with
// mesh shaders too. Fails if the pass is left out of the vertex tier's frame, or reads anything the
// two paths write differently.
TEST_CASE("accumulation: the vertex tier accumulates the mesh path's bytes") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  ScopedTunable phases("renderer.reconstruction.phases", "8");
  CapturedFrame drawn[2];
  const RasterMode modes[2] = {RasterMode::Hardware, RasterMode::Vertex};
  for (u32 m = 0; m < 2; ++m) {
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, ground_scene(), modes[m]), rig.error);
    REQUIRE(rig.resolved.vertex_path == (modes[m] == RasterMode::Vertex));
    REQUIRE(rig.resolved.accumulation == Accumulation::On);
    FrameDesc frame = held_frame(rig);
    for (u32 k = 0; k < 12; ++k) {
      frame.history_reset = k == 0;
      REQUIRE_MESSAGE(rig.shoot(frame, drawn[m]), rig.error);
    }
  }
  const Difference apart = difference(drawn[0].color, drawn[1].color);
  MESSAGE("mesh path against the vertex tier after twelve frames: worst " << apart.worst
                                                                          << " codes");
  CHECK(std::memcmp(drawn[0].color.data(), drawn[1].color.data(), drawn[0].color.size()) == 0);
}
