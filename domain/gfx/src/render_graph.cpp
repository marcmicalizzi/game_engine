#include <core/base/assert.h>
#include <core/log/log.h>
#include <core/memory/memory.h>
#include <core/time/time.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/render_graph.h>

#include <memory>

namespace engine::gfx {

// The barriers are Vulkan's own records, built once by compile() and handed to
// vkCmdPipelineBarrier2 by execute() as they are; the header keeps them behind a pointer so it
// names no Vulkan type (render_graph.h).
struct RenderGraph::Barriers {
  Vector<VkMemoryBarrier2> buffer;
  Vector<VkImageMemoryBarrier2> image;
  Vector<VkImageMemoryBarrier2> final;  // after the last pass (set_final_layout)
};

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_graph, "gfx.graph");

struct AccessInfo {
  VkPipelineStageFlags2 stage;
  VkAccessFlags2 access;
  VkImageLayout layout;
  bool writes;
};

AccessInfo access_info(Access access) noexcept {
  switch (access) {
    case Access::ComputeRead:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
              VK_IMAGE_LAYOUT_GENERAL, false};
    case Access::ComputeWrite:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_IMAGE_LAYOUT_GENERAL, true};
    case Access::ComputeReadWrite:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_IMAGE_LAYOUT_GENERAL, true};
    case Access::SampledRead:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false};
    case Access::TransferRead:
      return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false};
    case Access::TransferWrite:
      return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true};
    case Access::ColorAttachment:
      return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true};
    case Access::DepthAttachment:
      return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, true};
    case Access::IndirectRead:
      return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::MeshRead:
      return {VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
              VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::FragmentRead:
      return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::FragmentReadWrite:
      return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, true};
    case Access::VertexRead:
      return {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::IndexRead:
      return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::AccelerationBuildRead:
      return {VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
              VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::AccelerationBuildWrite:
      return {VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, true};
    case Access::RayQueryRead:
      return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::FragmentRayQueryRead:
      return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_IMAGE_LAYOUT_UNDEFINED, false};
  }
  return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
          true};
}

VkImageAspectFlags aspect_for(Format format) noexcept {
  switch (vk::native(format)) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

}  // namespace

bool access_writes(Access access) noexcept { return access_info(access).writes; }

// ---- PassBuilder -----------------------------------------------------------------------------

void PassBuilder::read(RgBuffer buffer, Access access) {
  graph_->add_use(pass_, false, buffer.index, access, access_writes(access));
}
void PassBuilder::write(RgBuffer buffer, Access access) {
  graph_->add_use(pass_, false, buffer.index, access, true);
}
void PassBuilder::read(RgImage image, Access access) {
  graph_->add_use(pass_, true, image.index, access, access_writes(access));
}
void PassBuilder::write(RgImage image, Access access) {
  graph_->add_use(pass_, true, image.index, access, true);
}
void PassBuilder::color_attachment(RgImage image, LoadOp load, ClearColor clear) {
  graph_->add_use(pass_, true, image.index, Access::ColorAttachment, true);
  graph_->add_attachment(pass_, image.index, load, clear, false);
}
void PassBuilder::depth_attachment(RgImage image, LoadOp load, float clear_depth) {
  graph_->add_use(pass_, true, image.index, Access::DepthAttachment, true);
  ClearColor value{};  // depth in the first float, stencil 0: VkClearDepthStencilValue's layout
  value.float32[0] = clear_depth;
  graph_->add_attachment(pass_, image.index, load, value, true);
}
void PassBuilder::render_area(u32 width, u32 height) {
  graph_->set_pass_area(pass_, width, height);
}

// ---- RenderGraph -----------------------------------------------------------------------------

RenderGraph::RenderGraph(const Device& device) : device_(&device) {
  barriers_ = static_cast<Barriers*>(mem::allocate(sizeof(Barriers), alignof(Barriers)));
  std::construct_at(barriers_);
}

RenderGraph::~RenderGraph() {
  reset();
  std::destroy_at(barriers_);
  mem::deallocate(barriers_, sizeof(Barriers), alignof(Barriers));
}

void RenderGraph::reset() noexcept {
  for (BufferNode& b : buffers_) {
    if (!b.imported) destroy_buffer(*device_, b.resource);
  }
  for (ImageNode& i : images_) {
    destroy_image_view(*device_, i.view);
    if (!i.imported) destroy_image(*device_, i.resource);
  }
  buffers_.clear();
  images_.clear();
  uses_.clear();
  attachments_.clear();
  passes_.clear();
  barriers_->buffer.clear();
  barriers_->image.clear();
  barriers_->final.clear();
  pass_image_layouts_.clear();
  arena_.reset();
  current_pass_ = ~u32{0};
  compiled_ = false;
  stats_ = Stats{};
}

RgBuffer RenderGraph::create_buffer(const char* name, const RgBufferDesc& desc) {
  ENGINE_VERIFY(!compiled_, "RenderGraph: cannot add resources after compile");
  BufferNode node{};
  node.name = name;
  node.desc = desc;
  buffers_.push_back(node);
  return RgBuffer{buffers_.size() - 1};
}

RgImage RenderGraph::create_image(const char* name, const RgImageDesc& desc) {
  ENGINE_VERIFY(!compiled_, "RenderGraph: cannot add resources after compile");
  ImageNode node{};
  node.name = name;
  node.desc = desc;
  images_.push_back(node);
  return RgImage{images_.size() - 1};
}

RgBuffer RenderGraph::import_buffer(const char* name, const BufferResource& buffer,
                                    PipelineStage last_stage, MemoryAccess last_access) {
  ENGINE_VERIFY(!compiled_, "RenderGraph: cannot add resources after compile");
  BufferNode node{};
  node.name = name;
  node.desc.size = buffer.size;
  node.resource = buffer;
  node.imported = true;
  node.has_producer = true;
  node.state.stage = last_stage;
  node.state.access = last_access;
  node.state.written = any(last_access & (MemoryAccess::MemoryWrite | MemoryAccess::ShaderWrite |
                                          MemoryAccess::ShaderStorageWrite |
                                          MemoryAccess::TransferWrite | MemoryAccess::HostWrite));
  buffers_.push_back(node);
  return RgBuffer{buffers_.size() - 1};
}

RgImage RenderGraph::import_image(const char* name, const ImageResource& image,
                                  ImageLayout current_layout, PipelineStage last_stage,
                                  MemoryAccess last_access) {
  ENGINE_VERIFY(!compiled_, "RenderGraph: cannot add resources after compile");
  ImageNode node{};
  node.name = name;
  node.desc.width = image.width;
  node.desc.height = image.height;
  node.desc.format = image.format;
  node.resource = image;
  node.imported = true;
  node.has_producer = true;
  node.state.stage = last_stage;
  node.state.access = last_access;
  node.state.layout = current_layout;
  node.state.written = true;  // unknown history: be conservative about the first barrier
  node.final_layout = current_layout;
  images_.push_back(node);
  return RgImage{images_.size() - 1};
}

u32 RenderGraph::add_pass_raw(const char* name, PassKind kind, ExecuteFn body, void* context) {
  ENGINE_VERIFY(!compiled_, "RenderGraph: cannot add passes after compile");
  Pass pass{};
  pass.name = name;
  pass.kind = kind;
  pass.execute = body;
  pass.context = context;
  pass.first_use = uses_.size();
  pass.first_attachment = attachments_.size();
  passes_.push_back(pass);
  return passes_.size() - 1;
}

void RenderGraph::set_pass_area(u32 pass, u32 width, u32 height) {
  ENGINE_VERIFY(pass == passes_.size() - 1, "RenderGraph: PassBuilder used outside its pass setup");
  ENGINE_VERIFY(passes_[pass].kind == PassKind::Raster,
                "RenderGraph: render_area needs a Raster pass");
  passes_[pass].area = Extent2D{width, height};
}

void RenderGraph::add_attachment(u32 pass, u32 image, LoadOp load, const ClearColor& clear,
                                 bool depth) {
  ENGINE_VERIFY(pass == passes_.size() - 1, "RenderGraph: PassBuilder used outside its pass setup");
  ENGINE_VERIFY(passes_[pass].kind == PassKind::Raster,
                "RenderGraph: attachments need a Raster pass");
  Attachment a{};
  a.image = image;
  a.load = load;
  a.clear = clear;
  a.depth = depth;
  attachments_.push_back(a);
  ++passes_[pass].attachment_count;
}

bool RenderGraph::create_attachment_views(std::string* error) {
  for (const Attachment& a : attachments_) {
    ImageNode& node = images_[a.image];
    if (node.view) continue;
    if (!create_image_view(*device_, node.resource, node.view, error)) {
      if (error != nullptr) error->insert(0, std::string("attachment view '") + node.name + "': ");
      return false;
    }
  }
  return true;
}

void RenderGraph::begin_rendering(CommandList commands, const Pass& pass) {
  VkRenderingAttachmentInfo colors[8]{};
  VkRenderingAttachmentInfo depth{};
  u32 color_count = 0;
  bool has_depth = false;
  render_area_ = pass.area;  // attachments below override an explicit area
  for (u32 i = pass.first_attachment; i < pass.first_attachment + pass.attachment_count; ++i) {
    const Attachment& a = attachments_[i];
    const ImageNode& node = images_[a.image];
    VkRenderingAttachmentInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    info.imageView = vk::native(node.view);
    info.imageLayout = a.depth ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    info.loadOp = vk::native(a.load);
    info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (a.depth) {
      info.clearValue.depthStencil = {a.clear.float32[0], 0};
    } else {
      info.clearValue.color = vk::native(a.clear);
    }
    if (a.depth) {
      depth = info;
      has_depth = true;
    } else if (color_count < 8) {
      colors[color_count++] = info;
    }
    render_area_ = Extent2D{node.resource.width, node.resource.height};
  }
  VkRenderingInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.renderArea = {{0, 0}, vk::native(render_area_)};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = color_count;
  rendering.pColorAttachments = colors;
  rendering.pDepthAttachment = has_depth ? &depth : nullptr;
  vkCmdBeginRendering(vk::native(commands), &rendering);
  // Negative height flips Vulkan's y-down framebuffer to the y-up clip space core/math produces,
  // so counter-clockwise triangles in y-up space are the front faces.
  Viewport viewport{};
  viewport.y = static_cast<float>(render_area_.height);
  viewport.width = static_cast<float>(render_area_.width);
  viewport.height = -static_cast<float>(render_area_.height);
  viewport.min_depth = 0.0f;
  viewport.max_depth = 1.0f;
  commands.set_viewport(viewport);
  commands.set_scissor(Rect2D{{0, 0}, render_area_});
}

void RenderGraph::add_use(u32 pass, bool is_image, u32 index, Access access, bool write) {
  ENGINE_VERIFY(pass == passes_.size() - 1, "RenderGraph: PassBuilder used outside its pass setup");
  Use use{};
  use.is_image = is_image;
  use.index = index;
  use.access = access;
  use.write = write;
  uses_.push_back(use);
  ++passes_[pass].use_count;
}

bool RenderGraph::allocate_transients(std::string* error) {
  for (BufferNode& b : buffers_) {
    if (b.imported) continue;
    if (!::engine::gfx::create_buffer(*device_, b.desc.size, b.desc.usage, b.desc.host_visible,
                                      b.resource, error)) {
      if (error != nullptr) error->insert(0, std::string("transient buffer '") + b.name + "': ");
      return false;
    }
    ++stats_.transient_buffers;
  }
  for (ImageNode& i : images_) {
    if (i.imported) continue;
    if (!::engine::gfx::create_image_2d(*device_, i.desc.width, i.desc.height, i.desc.format,
                                        i.desc.usage, i.resource, error)) {
      if (error != nullptr) error->insert(0, std::string("transient image '") + i.name + "': ");
      return false;
    }
    ++stats_.transient_images;
  }
  return true;
}

void RenderGraph::compute_barriers() {
  // Walk passes in order, tracking each resource's last stage/access/layout, and emit a
  // barrier before a pass when the transition needs one: after any write, before any write,
  // or when an image changes layout. Read-after-read with the same layout needs nothing and
  // only widens the recorded stage mask so a later barrier covers every reader.
  for (u32 p = 0; p < passes_.size(); ++p) {
    Pass& pass = passes_[p];
    pass.first_buffer_barrier = barriers_->buffer.size();
    pass.first_image_barrier = barriers_->image.size();
    pass.first_layout = pass_image_layouts_.size();
    for (u32 u = pass.first_use; u < pass.first_use + pass.use_count; ++u) {
      const Use& use = uses_[u];
      const AccessInfo info = access_info(use.access);
      const bool writes = use.write || info.writes;
      if (use.is_image) {
        ImageNode& node = images_[use.index];
        State& s = node.state;
        const ImageLayout layout = vk::wrap(info.layout);
        const bool layout_change = s.layout != layout;
        const bool needs = s.written || writes || layout_change;
        if (needs) {
          VkImageMemoryBarrier2 barrier{};
          barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
          barrier.srcStageMask = s.stage != PipelineStage::None
                                     ? vk::native(s.stage)
                                     : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
          barrier.srcAccessMask = vk::native(s.access);
          barrier.dstStageMask = info.stage;
          barrier.dstAccessMask = info.access;
          barrier.oldLayout = vk::native(s.layout);
          barrier.newLayout = info.layout;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = vk::native(node.resource.image);
          barrier.subresourceRange = {aspect_for(node.resource.format), 0, 1, 0, 1};
          barriers_->image.push_back(barrier);
          ++pass.image_barrier_count;
          ++stats_.image_barriers;
          if (layout_change) ++stats_.layout_transitions;
          s.stage = vk::pipeline_stage(info.stage);
          s.access = vk::memory_access(info.access);
        } else {
          s.stage |= vk::pipeline_stage(info.stage);
          s.access |= vk::memory_access(info.access);
        }
        s.layout = layout;
        s.written = writes;
        node.final_layout = layout;
        pass_image_layouts_.push_back(layout);
      } else {
        BufferNode& node = buffers_[use.index];
        State& s = node.state;
        const bool needs = s.written || (writes && s.stage != PipelineStage::None);
        if (needs) {
          VkMemoryBarrier2 barrier{};
          barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
          barrier.srcStageMask = s.stage != PipelineStage::None
                                     ? vk::native(s.stage)
                                     : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
          barrier.srcAccessMask = vk::native(s.access);
          barrier.dstStageMask = info.stage;
          barrier.dstAccessMask = info.access;
          barriers_->buffer.push_back(barrier);
          ++pass.buffer_barrier_count;
          ++stats_.buffer_barriers;
          s.stage = vk::pipeline_stage(info.stage);
          s.access = vk::memory_access(info.access);
        } else {
          s.stage |= vk::pipeline_stage(info.stage);
          s.access |= vk::memory_access(info.access);
        }
        s.written = writes;
      }
    }
  }
}

void RenderGraph::set_final_layout(RgImage image, ImageLayout layout) {
  ENGINE_VERIFY(!compiled_, "RenderGraph: set_final_layout before compile");
  ENGINE_VERIFY(image.index < images_.size(), "RenderGraph::set_final_layout: invalid handle");
  images_[image.index].requested_final = layout;
}

bool RenderGraph::compile(std::string* error) {
  ENGINE_VERIFY(!compiled_, "RenderGraph::compile: already compiled");
  // Validate: indices in range, every read has a producer before it (imported counts).
  for (u32 p = 0; p < passes_.size(); ++p) {
    const Pass& pass = passes_[p];
    for (u32 u = pass.first_use; u < pass.first_use + pass.use_count; ++u) {
      const Use& use = uses_[u];
      const usize count = use.is_image ? images_.size() : buffers_.size();
      if (use.index >= count) {
        if (error != nullptr)
          *error = std::string("pass '") + pass.name + "' uses an invalid resource handle";
        return false;
      }
      bool& has_producer =
          use.is_image ? images_[use.index].has_producer : buffers_[use.index].has_producer;
      const char* name = use.is_image ? images_[use.index].name : buffers_[use.index].name;
      if (!use.write && !access_writes(use.access) && !has_producer) {
        if (error != nullptr) {
          *error = std::string("pass '") + pass.name + "' reads '" + name +
                   "' before anything writes it";
        }
        return false;
      }
      if (use.write || access_writes(use.access)) has_producer = true;
    }
  }
  if (!allocate_transients(error)) return false;
  if (!create_attachment_views(error)) return false;
  compute_barriers();
  for (ImageNode& node : images_) {
    if (node.requested_final == ImageLayout::Undefined ||
        node.requested_final == node.state.layout) {
      continue;
    }
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = node.state.stage != PipelineStage::None
                               ? vk::native(node.state.stage)
                               : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = vk::native(node.state.access);
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;  // consumed by the submit's signal
    barrier.dstAccessMask = VK_ACCESS_2_NONE;
    barrier.oldLayout = vk::native(node.state.layout);
    barrier.newLayout = vk::native(node.requested_final);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk::native(node.resource.image);
    barrier.subresourceRange = {aspect_for(node.resource.format), 0, 1, 0, 1};
    barriers_->final.push_back(barrier);
    node.state.layout = node.requested_final;
    node.state.stage = PipelineStage::None;
    node.state.access = MemoryAccess::None;
    node.final_layout = node.requested_final;
    ++stats_.image_barriers;
    ++stats_.layout_transitions;
  }
  for (const Pass& pass : passes_) {
    if (pass.kind == PassKind::Raster) ++stats_.raster_passes;
  }
  stats_.passes = passes_.size();
  stats_.buffers = buffers_.size();
  stats_.images = images_.size();
  compiled_ = true;
  ENGINE_LOG_DEBUG(log_graph, "graph compiled", log::field("passes", stats_.passes),
                   log::field("buffer_barriers", stats_.buffer_barriers),
                   log::field("image_barriers", stats_.image_barriers));
  return true;
}

void RenderGraph::execute(CommandList commands) {
  ENGINE_VERIFY(compiled_, "RenderGraph::execute: compile first");
  const VkCommandBuffer cb = vk::native(commands);
  // Each pass's recording is timed, two clock reads a pass: a frame whose recording is slow says
  // which pass it was in (`pass_cpu_ns`), which a total cannot.
  i64 mark = time::monotonic_ns();
  for (u32 p = 0; p < passes_.size(); ++p) {
    Pass& pass = passes_[p];
    if (pass.buffer_barrier_count > 0 || pass.image_barrier_count > 0) {
      VkDependencyInfo dependency{};
      dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      dependency.memoryBarrierCount = pass.buffer_barrier_count;
      dependency.pMemoryBarriers = barriers_->buffer.data() + pass.first_buffer_barrier;
      dependency.imageMemoryBarrierCount = pass.image_barrier_count;
      dependency.pImageMemoryBarriers = barriers_->image.data() + pass.first_image_barrier;
      vkCmdPipelineBarrier2(cb, &dependency);
    }
    current_pass_ = p;
    const bool raster =
        pass.kind == PassKind::Raster && (pass.attachment_count > 0 || pass.area.width > 0);
    if (raster) begin_rendering(commands, pass);
    pass.execute(commands, *this, pass.context);
    if (raster) vkCmdEndRendering(cb);
    const i64 now = time::monotonic_ns();
    pass.cpu_ns = now - mark;
    mark = now;
  }
  if (!barriers_->final.empty()) {
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = barriers_->final.size();
    dependency.pImageMemoryBarriers = barriers_->final.data();
    vkCmdPipelineBarrier2(cb, &dependency);
  }
  current_pass_ = ~u32{0};
}

const BufferResource& RenderGraph::buffer(RgBuffer handle) const noexcept {
  ENGINE_ASSERT(handle.index < buffers_.size(), "RenderGraph::buffer: invalid handle");
  return buffers_[handle.index].resource;
}

const ImageResource& RenderGraph::image(RgImage handle) const noexcept {
  ENGINE_ASSERT(handle.index < images_.size(), "RenderGraph::image: invalid handle");
  return images_[handle.index].resource;
}

ImageLayout RenderGraph::image_layout(RgImage handle) const noexcept {
  if (current_pass_ == ~u32{0} || handle.index >= images_.size()) return ImageLayout::Undefined;
  const Pass& pass = passes_[current_pass_];
  u32 layout_slot = pass.first_layout;
  for (u32 u = pass.first_use; u < pass.first_use + pass.use_count; ++u) {
    const Use& use = uses_[u];
    if (!use.is_image) continue;
    if (use.index == handle.index) return pass_image_layouts_[layout_slot];
    ++layout_slot;
  }
  return ImageLayout::Undefined;
}

ImageViewHandle RenderGraph::image_view(RgImage handle) const noexcept {
  return handle.index < images_.size() ? images_[handle.index].view : ImageViewHandle{};
}

ImageLayout RenderGraph::final_layout(RgImage handle) const noexcept {
  return handle.index < images_.size() ? images_[handle.index].final_layout
                                       : ImageLayout::Undefined;
}

}  // namespace engine::gfx
