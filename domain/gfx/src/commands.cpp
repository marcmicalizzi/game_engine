// CommandList: one Vulkan command per method (commands.h). Nothing is cached or checked here, so
// a pass body recorded through this records exactly what it recorded when it named vkCmd*.
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/commands.h>

namespace engine::gfx {

void CommandList::bind_pipeline(BindPoint point, PipelineHandle pipeline) const noexcept {
  vkCmdBindPipeline(vk::native(handle_), vk::native(point), vk::native(pipeline));
}

void CommandList::push_constants(PipelineLayoutHandle layout, ShaderStage stages, u32 offset,
                                 u32 size, const void* data) const noexcept {
  vkCmdPushConstants(vk::native(handle_), vk::native(layout), vk::native(stages), offset, size,
                     data);
}

void CommandList::dispatch(u32 x, u32 y, u32 z) const noexcept {
  vkCmdDispatch(vk::native(handle_), x, y, z);
}

void CommandList::dispatch_indirect(BufferHandle buffer, u64 offset) const noexcept {
  vkCmdDispatchIndirect(vk::native(handle_), vk::native(buffer), offset);
}

void CommandList::set_viewport(const Viewport& viewport) const noexcept {
  VkViewport v{};
  v.x = viewport.x;
  v.y = viewport.y;
  v.width = viewport.width;
  v.height = viewport.height;
  v.minDepth = viewport.min_depth;
  v.maxDepth = viewport.max_depth;
  vkCmdSetViewport(vk::native(handle_), 0, 1, &v);
}

void CommandList::set_scissor(const Rect2D& scissor) const noexcept {
  const VkRect2D rect{{scissor.offset.x, scissor.offset.y},
                      {scissor.extent.width, scissor.extent.height}};
  vkCmdSetScissor(vk::native(handle_), 0, 1, &rect);
}

void CommandList::draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                       u32 first_instance) const noexcept {
  vkCmdDraw(vk::native(handle_), vertex_count, instance_count, first_vertex, first_instance);
}

void CommandList::draw_indirect(BufferHandle buffer, u64 offset, u32 draw_count,
                                u32 stride) const noexcept {
  vkCmdDrawIndirect(vk::native(handle_), vk::native(buffer), offset, draw_count, stride);
}

void CommandList::bind_index_buffer(BufferHandle buffer, u64 offset,
                                    IndexType type) const noexcept {
  vkCmdBindIndexBuffer(vk::native(handle_), vk::native(buffer), offset, vk::native(type));
}

void CommandList::draw_indexed_indirect(BufferHandle buffer, u64 offset, u32 draw_count,
                                        u32 stride) const noexcept {
  vkCmdDrawIndexedIndirect(vk::native(handle_), vk::native(buffer), offset, draw_count, stride);
}

void CommandList::draw_mesh_tasks(u32 x, u32 y, u32 z) const noexcept {
  vkCmdDrawMeshTasksEXT(vk::native(handle_), x, y, z);
}

void CommandList::draw_mesh_tasks_indirect(BufferHandle buffer, u64 offset, u32 draw_count,
                                           u32 stride) const noexcept {
  vkCmdDrawMeshTasksIndirectEXT(vk::native(handle_), vk::native(buffer), offset, draw_count,
                                stride);
}

void CommandList::fill_buffer(BufferHandle buffer, u64 offset, u64 size, u32 value) const noexcept {
  vkCmdFillBuffer(vk::native(handle_), vk::native(buffer), offset, size, value);
}

void CommandList::update_buffer(BufferHandle buffer, u64 offset, u64 size,
                                const void* data) const noexcept {
  vkCmdUpdateBuffer(vk::native(handle_), vk::native(buffer), offset, size, data);
}

void CommandList::copy_buffer(BufferHandle src, BufferHandle dst,
                              std::span<const BufferCopy> regions) const noexcept {
  vkCmdCopyBuffer(vk::native(handle_), vk::native(src), vk::native(dst),
                  static_cast<u32>(regions.size()), vk::native(regions.data()));
}

}  // namespace engine::gfx
