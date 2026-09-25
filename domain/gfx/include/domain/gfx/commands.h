#pragma once

// Recording commands (docs/subsystems/gfx.md, "The RHI surface and the backend surface"). A
// `CommandList` is what a render graph pass body, `submit_immediate` and the timers are handed:
// the backend's command buffer behind a handle, with the commands the renderer records as
// methods. It is a value, 8 bytes, copied into a pass body the way the Vulkan command buffer
// was, and it owns nothing — the frame context or the immediate submission owns the buffer.
//
//     graph.add_pass("cull", gfx::PassKind::Compute, setup,
//         [&](gfx::CommandList cmd, gfx::RenderGraph&) {
//           cmd.bind_pipeline(gfx::BindPoint::Compute, cull.pipeline);
//           cmd.push_constants(cull.layout, gfx::ShaderStage::Compute, 0, sizeof(p), &p);
//           cmd.dispatch(groups, 1, 1);
//         });
//
// **Barriers are not here.** A pass declares what it reads and writes and the render graph
// derives every barrier and layout transition (render_graph.h); the two that are recorded by
// hand have functions of their own (`acceleration_build_barrier` in acceleration.h, and the
// backend's `image_barrier` for the RHI's own tests). Nor is anything the renderer does not
// record: a command is added here when something above the RHI needs it, and the RHI's own tests
// reach the rest through the backend header set.
//
// Every method is one backend command and nothing else — no state is cached, nothing is checked
// beyond what the backend checks — so moving a pass body onto this could not change what it
// records. Each costs one call more than naming the backend's function directly did, which at a
// few hundred commands a frame is nothing a measurement can find.

#include <core/base/types.h>
#include <domain/gfx/rhi.h>

#include <span>

namespace engine::gfx {

class CommandList {
 public:
  constexpr CommandList() noexcept = default;
  constexpr explicit CommandList(CommandListHandle handle) noexcept : handle_(handle) {}

  constexpr CommandListHandle handle() const noexcept { return handle_; }
  constexpr bool valid() const noexcept { return handle_.valid(); }

  // ---- pipelines and parameters ----
  void bind_pipeline(BindPoint point, PipelineHandle pipeline) const noexcept;
  void push_constants(PipelineLayoutHandle layout, ShaderStage stages, u32 offset, u32 size,
                      const void* data) const noexcept;

  // ---- compute ----
  void dispatch(u32 x, u32 y, u32 z) const noexcept;
  void dispatch_indirect(BufferHandle buffer, u64 offset) const noexcept;

  // ---- draws (inside a raster pass) ----
  void set_viewport(const Viewport& viewport) const noexcept;
  void set_scissor(const Rect2D& scissor) const noexcept;
  void draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
            u32 first_instance) const noexcept;
  void draw_indirect(BufferHandle buffer, u64 offset, u32 draw_count, u32 stride) const noexcept;
  void bind_index_buffer(BufferHandle buffer, u64 offset, IndexType type) const noexcept;
  void draw_indexed_indirect(BufferHandle buffer, u64 offset, u32 draw_count,
                             u32 stride) const noexcept;
  // Mesh shaders: DeviceFeatures::mesh_shader.
  void draw_mesh_tasks(u32 x, u32 y, u32 z) const noexcept;
  void draw_mesh_tasks_indirect(BufferHandle buffer, u64 offset, u32 draw_count,
                                u32 stride) const noexcept;

  // ---- transfers ----
  // `size` may be k_whole_size; `offset` and `size` are multiples of four.
  void fill_buffer(BufferHandle buffer, u64 offset, u64 size, u32 value) const noexcept;
  // At most 65536 bytes, a multiple of four, recorded inline in the command buffer.
  void update_buffer(BufferHandle buffer, u64 offset, u64 size, const void* data) const noexcept;
  void copy_buffer(BufferHandle src, BufferHandle dst,
                   std::span<const BufferCopy> regions) const noexcept;
  void copy_buffer(BufferHandle src, BufferHandle dst, const BufferCopy& region) const noexcept {
    copy_buffer(src, dst, std::span<const BufferCopy>(&region, 1));
  }

 private:
  CommandListHandle handle_;
};

static_assert(sizeof(CommandList) == 8);

}  // namespace engine::gfx
