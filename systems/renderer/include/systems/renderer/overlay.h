#pragma once

// The overlay: 2D triangle lists composited onto the frame's colour image after everything else the
// frame draws (ADR-0054; docs/subsystems/renderer.md, "The overlay"). It is how the tools UI
// reaches the screen — engine-editor hands Dear ImGui's draw lists over in this shape every frame —
// and it is the seam the game UI will share once R45 chooses one.
//
// The vocabulary is the engine's, not ImGui's: a vertex is a position in the target's pixels, a UV
// and a packed colour, which is ImGui's `ImDrawVert` byte for byte so a host copies its lists
// without converting them, but nothing here includes ImGui and nothing below the host knows it is
// there. A command is a scissor rectangle, an overlay texture and a run of indices.
//
// **SDR only, for now.** The overlay draws into the encoded picture with no curve of its own,
// blending "over" in the space the output encode wrote (`display_output`, display.slang), which is
// exactly what an immediate-mode UI's colours are authored in on an SDR target. On a PQ or scRGB
// target that is wrong — the UI has to be composited at paper white, in the output's primaries,
// between the curve and the encode (renderer.md, "HDR output", the UI's seam) — so a frame that
// asks for an overlay on an HDR target is refused rather than drawn wrongly.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/shader_library.h>

#include <span>
#include <string>

namespace engine::renderer {

// One vertex: a position in the target's pixels (y down), a texture coordinate, and a colour packed
// r in the low byte (straight alpha). 20 bytes, `ImDrawVert`'s layout, read as five words by
// `shaders/overlay.slang`.
struct OverlayVertex {
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 u = 0.0f;
  f32 v = 0.0f;
  u32 rgba = 0;
};
static_assert(sizeof(OverlayVertex) == 20, "OverlayVertex mirrors overlay.slang's five words");

// One draw: the triangles `indices[first_index, first_index + index_count)`, each index plus
// `vertex_offset`, textured by `texture` (an id from `OverlayPass::add_texture`), clipped to the
// rectangle (target pixels, min inclusive, max exclusive, clamped to the target).
struct OverlayCommand {
  f32 clip_x0 = 0.0f;
  f32 clip_y0 = 0.0f;
  f32 clip_x1 = 0.0f;
  f32 clip_y1 = 0.0f;
  u32 texture = 0;
  u32 first_index = 0;
  u32 index_count = 0;
  u32 vertex_offset = 0;
};

// A frame's overlay, in draw order. The spans are read while the frame is recorded and copied into
// the frame slot's own buffers, so they need outlive only `SceneRenderer::submit_frame`.
struct OverlayDrawData {
  std::span<const OverlayVertex> vertices;
  std::span<const u32> indices;
  std::span<const OverlayCommand> commands;
};

// Mirrors `OverlayDrawParams` in shaders/overlay.slang: the push block, 48 bytes.
struct OverlayDrawParams {
  u64 vertices = 0;
  u64 indices = 0;
  f32 scale[2] = {0.0f, 0.0f};
  f32 translate[2] = {-1.0f, -1.0f};
  u32 first_index = 0;
  u32 vertex_offset = 0;
  u32 texture = 0;
  u32 sampler = 0;
};
static_assert(sizeof(OverlayDrawParams) == 48, "OverlayDrawParams mirrors overlay.slang");

// The pass itself: one pipeline (alpha-blended, `gfx::BlendMode::Alpha`), one linear sampler, the
// overlay's textures in the scene's bindless set, and per frame slot a host-visible vertex buffer
// and index buffer that grow to the largest frame seen and are then reused, so a steady UI
// allocates nothing a frame. `SceneRenderer` owns one and records it as the frame's last raster
// pass; a host reaches it through `SceneRenderer::overlay()` for its textures.
class OverlayPass {
 public:
  static constexpr u32 k_no_texture = ~u32{0};

  OverlayPass() noexcept = default;
  ~OverlayPass();
  ENGINE_NON_COPYABLE(OverlayPass);

  bool create(const gfx::Device& device, gfx::BindlessSet& bindless, gfx::ShaderLibrary& shaders,
              gfx::Format color_format, u32 frames_in_flight, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }
  // Forgets the bindless set, for a teardown whose set is already gone: `destroy` then frees the
  // images, buffers and pipeline and releases no slots.
  void detach_bindless() noexcept { bindless_ = nullptr; }

  // An RGBA8 texture (straight alpha, rows tightly packed) the commands can name; `k_no_texture`,
  // with `error`, when it could not be made. Uploaded at once (a blocking submission): a UI makes
  // its font atlas once and its images rarely.
  u32 add_texture(u32 width, u32 height, const void* rgba, std::string* error = nullptr);
  // Retires a texture. **No frame in flight may still read it**: `SceneRenderer::remove_overlay_
  // texture` waits for the device before it calls this.
  void remove_texture(u32 texture) noexcept;
  u32 texture_count() const noexcept { return live_textures_; }

  // Copies a frame's lists into frame slot `slot`'s buffers, growing them (the outgrown ones go to
  // `frames`, freed when the slot comes round). False, with `error`, for a command naming no live
  // texture or a run past the lists.
  bool stage(u32 slot, const OverlayDrawData& data, gfx::FrameContext& frames,
             std::string* error = nullptr);
  // Records the draws `stage` staged into slot `slot`, over a `width` x `height` target: an
  // unflipped viewport, each command's scissor, and one non-indexed draw per command. Inside a
  // raster pass on the target, with the bindless set bound.
  void record(gfx::CommandList commands, u32 slot, const OverlayDrawData& data, u32 width,
              u32 height) const noexcept;

 private:
  struct Texture {
    gfx::ImageResource image;
    gfx::ImageViewHandle view;
    u32 slot = gfx::BindlessSet::k_invalid_slot;
  };
  struct SlotBuffers {
    gfx::BufferResource vertices;
    gfx::BufferResource indices;
  };

  const gfx::Device* device_ = nullptr;
  gfx::BindlessSet* bindless_ = nullptr;
  gfx::PipelineHandle pipeline_;
  gfx::SamplerHandle sampler_;
  u32 sampler_slot_ = gfx::BindlessSet::k_invalid_slot;
  Vector<Texture> textures_;
  u32 live_textures_ = 0;
  Vector<SlotBuffers> slots_;
};

}  // namespace engine::renderer
