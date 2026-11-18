#pragma once

// **The renderer's registration point for a reconstruction provider** (docs/subsystems/renderer.md,
// "Sampling over time: a reconstruction provider"; ADR-0064; roadmap R159). A provider is what
// turns the frame the renderer drew at a sub-pixel offset, with its depth and its motion, into the
// frame the encode shows: the engine's own accumulation (built in, `"engine"`), a null one that
// hands its input on (built in, `"null"`, for the tests: a frame drawn through it is the frame the
// resolve drew), and later a vendor's method that its consumer supplies (roadmap R161), registered
// the way a sky provider registers its model (ADR-0048): by name, from a capability the host links,
// with nothing below a host naming one.
//
// **What a provider is handed** is one struct, `ReconstructionFrame`: the frame pass's context
// (frame_pass.h: the graph it adds its passes to, the colour image in linear exposed radiance, the
// views, the visibility buffer whose high words are the depth, the display's curve), the motion
// pass's buffer and last frame's visibility buffer and motion (renderer.md, "Sampling over time:
// the inputs"), this frame's sub-pixel offset and its phase, and whether the frame before this one
// is this one's history. **What it returns** is one image, `ReconstructionOutput`, which the
// encode reads in the colour image's place: the frame pass report's `color`, so a provider is a
// frame pass at `FramePassStage::Reconstruction` and its passes are declared through the graph like
// every other's, never with a barrier of its own.
//
// The renderer holds one provider at most, chosen by name at load
// (`renderer.reconstruction.provider`; empty is the engine's own whenever
// `renderer.reconstruction.accumulation` is on). Every frame drawn through one is a reconstructed
// frame: its record carries the provider's name and no cost (ADR-0064; renderer.md, "Sampling over
// time: the measurement rule").

#include <core/base/types.h>
#include <systems/renderer/frame_pass.h>
#include <systems/renderer/view_set.h>

#include <string>
#include <string_view>

namespace engine::renderer {

// One frame's inputs, at the frame's own (internal) size; every address is a device address, every
// handle the frame's render graph's, and a reader declares what it reads through `frame->graph`.
struct ReconstructionFrame {
  const FramePassContext* frame = nullptr;  // `color`, `visibility`, `views`, the display's curve
  // The motion pass's buffer (`frame->motion`): per view, from `motion[view]`, a `u32` a source
  // pixel, two halves, where the pixel's surface was in last frame's picture in pixels, neither
  // frame's offset in it; `gfx::k_motion_invalid` where it was behind last frame's camera.
  u64 motion[k_max_views] = {};
  // Last frame's visibility buffer and motion, the same layout as this frame's, imported into the
  // graph as `prev_visibility` and `prev_motion` (invalid with no history).
  gfx::RgBuffer prev_visibility;
  gfx::RgBuffer prev_motion;
  u64 prev_visibility_address[k_max_views] = {};
  u64 prev_motion_address[k_max_views] = {};
  // This frame's sub-pixel offset in source pixels (x right, y down: `ViewSet::update`), its phase
  // and the cycle's length.
  f32 offset_x = 0.0f;
  f32 offset_y = 0.0f;
  u32 phase = 0;
  u32 phases = 1;
  // False at a reset (a cut, a teleport, a load, a resize) and at the first frame: the provider
  // keeps nothing of what it drew before.
  bool history = false;
  // The size the encode reads: the target's. Equal to `frame->width` x `frame->height` until a
  // ratio (roadmap R160).
  u32 output_width = 0;
  u32 output_height = 0;
};

// What a provider leaves: an image of the output's size in the colour image's format and meaning
// (`k_frame_pass_format`, exposed linear radiance, alpha as frame_pass.h says), with its sampled
// slot in the bindless set. Left invalid, the encode reads the colour image itself.
struct ReconstructionOutput {
  gfx::RgImage color;
  u32 color_sampled = gfx::BindlessSet::k_invalid_slot;
};

// What a provider is made with, once at load and again after a resize (`resize`).
struct ReconstructionCreate {
  const gfx::Device* device = nullptr;
  gfx::BindlessSet* bindless = nullptr;
  u32 width = 0;  // the output's size
  u32 height = 0;
  u32 frames_in_flight = 0;
};

// A provider: a name and five functions over a state of its own. `create` returns that state (null
// with `error` refuses the load); `record` adds the provider's passes to `in.frame->graph` and
// fills `out`; `device_bytes` is what it keeps on the device, for the log and the memory report
// (a vendor's library may allocate inside itself, and its page says what it measured).
struct ReconstructionProviderDesc {
  const char* name = nullptr;
  void* (*create)(const ReconstructionCreate& create, std::string* error) = nullptr;
  void (*destroy)(void* state) noexcept = nullptr;
  bool (*resize)(void* state, u32 width, u32 height, std::string* error) = nullptr;
  bool (*record)(void* state, const ReconstructionFrame& in, ReconstructionOutput& out,
                 std::string* error) = nullptr;
  u64 (*device_bytes)(const void* state) noexcept = nullptr;
};

// The names the renderer carries itself: its own accumulation, and the one that hands its input on.
inline constexpr const char* k_reconstruction_engine = "engine";
inline constexpr const char* k_reconstruction_null = "null";
// Providers a process holds at once, the two built in among them.
inline constexpr u32 k_max_reconstruction_providers = 8;

// Registers a provider for every renderer made after it. False when the name is empty, taken, or
// one of the two built in, or when the table is full. Not thread-safe: call it at start-up, before
// any renderer is made, as a capability's registration does.
bool register_reconstruction_provider(const ReconstructionProviderDesc& desc) noexcept;
// The provider of that name, the built-in `null` among them; null for none (and for `engine`,
// which is not a desc: the renderer makes it from its own passes).
const ReconstructionProviderDesc* find_reconstruction_provider(std::string_view name) noexcept;

}  // namespace engine::renderer
