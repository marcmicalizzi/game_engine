#include <systems/renderer/overlay.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::renderer {

OverlayPass::~OverlayPass() { destroy(); }

bool OverlayPass::create(const gfx::Device& device, gfx::BindlessSet& bindless,
                         gfx::ShaderLibrary& shaders, gfx::Format color_format,
                         u32 frames_in_flight, std::string* error) {
  destroy();
  const gfx::Shader* shader = shaders.get("overlay", error);
  if (shader == nullptr) return false;
  gfx::GraphicsPipelineDesc desc;
  desc.vertex = shader->module;
  desc.vertex_entry = "vs_overlay";
  desc.fragment = shader->module;
  desc.fragment_entry = "fs_overlay";
  desc.layout = bindless.pipeline_layout();
  desc.color_format = color_format;
  desc.blend = gfx::BlendMode::Alpha;
  if (!gfx::create_graphics_pipeline(device, desc, pipeline_, error)) return false;
  device_ = &device;
  bindless_ = &bindless;
  if (!gfx::create_sampler(device, gfx::Filter::Linear, sampler_, error)) {
    destroy();
    return false;
  }
  sampler_slot_ = bindless.add_sampler(sampler_);
  if (sampler_slot_ == gfx::BindlessSet::k_invalid_slot) {
    if (error != nullptr) *error = "the overlay's sampler found no free bindless slot";
    destroy();
    return false;
  }
  slots_.resize(frames_in_flight > 0 ? frames_in_flight : 1u);
  return true;
}

void OverlayPass::destroy() noexcept {
  if (device_ == nullptr) return;
  for (u32 t = 0; t < textures_.size(); ++t)
    remove_texture(t);
  textures_.clear();
  for (SlotBuffers& s : slots_) {
    if (s.vertices.buffer.valid()) gfx::destroy_buffer(*device_, s.vertices);
    if (s.indices.buffer.valid()) gfx::destroy_buffer(*device_, s.indices);
  }
  slots_.clear();
  if (sampler_slot_ != gfx::BindlessSet::k_invalid_slot && bindless_ != nullptr) {
    bindless_->release_sampler(sampler_slot_, 0);
    bindless_->recycle(0);
  }
  sampler_slot_ = gfx::BindlessSet::k_invalid_slot;
  if (sampler_.valid()) gfx::destroy_sampler(*device_, sampler_);
  sampler_ = {};
  if (pipeline_.valid()) gfx::destroy_pipeline(*device_, pipeline_);
  pipeline_ = {};
  device_ = nullptr;
  bindless_ = nullptr;
}

u32 OverlayPass::add_texture(u32 width, u32 height, const void* rgba, std::string* error) {
  if (device_ == nullptr || width == 0 || height == 0 || rgba == nullptr) {
    if (error != nullptr) *error = "an overlay texture needs a created pass, a size and pixels";
    return k_no_texture;
  }
  Texture texture;
  const u64 bytes = u64{width} * height * 4u;
  if (!gfx::upload_image_2d(*device_, width, height, gfx::Format::R8G8B8A8Unorm, rgba, bytes,
                            texture.image, error)) {
    return k_no_texture;
  }
  if (!gfx::create_image_view(*device_, texture.image, texture.view, error)) {
    gfx::destroy_image(*device_, texture.image);
    return k_no_texture;
  }
  texture.slot = bindless_->add_sampled_image(texture.view, gfx::ImageLayout::ShaderReadOnly);
  if (texture.slot == gfx::BindlessSet::k_invalid_slot) {
    if (error != nullptr) *error = "an overlay texture found no free bindless slot";
    gfx::destroy_image_view(*device_, texture.view);
    gfx::destroy_image(*device_, texture.image);
    return k_no_texture;
  }
  // A freed id is reused before the table grows, so a UI that remakes its atlas keeps it small.
  for (u32 t = 0; t < textures_.size(); ++t) {
    if (textures_[t].slot == gfx::BindlessSet::k_invalid_slot) {
      textures_[t] = texture;
      ++live_textures_;
      return t;
    }
  }
  textures_.push_back(texture);
  ++live_textures_;
  return static_cast<u32>(textures_.size() - 1);
}

void OverlayPass::remove_texture(u32 texture) noexcept {
  if (device_ == nullptr || texture >= textures_.size()) return;
  Texture& t = textures_[texture];
  if (t.slot == gfx::BindlessSet::k_invalid_slot) return;
  if (bindless_ != nullptr) {
    bindless_->release_sampled_image(t.slot, 0);
    bindless_->recycle(0);
  }
  gfx::destroy_image_view(*device_, t.view);
  gfx::destroy_image(*device_, t.image);
  t = Texture{};
  --live_textures_;
}

namespace {

// Grows `buffer` to hold `bytes`, doubling, the outgrown one handed to the frame context.
bool ensure_capacity(const gfx::Device& device, gfx::BufferResource& buffer, u64 bytes,
                     gfx::FrameContext& frames, std::string* error) {
  if (buffer.buffer.valid() && buffer.size >= bytes) return true;
  u64 capacity = buffer.size > 0 ? buffer.size : 64u * 1024u;
  while (capacity < bytes)
    capacity *= 2u;
  if (buffer.buffer.valid()) frames.defer_destroy(buffer);
  buffer = gfx::BufferResource{};
  return gfx::create_buffer(device, capacity,
                            gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, true,
                            buffer, error);
}

}  // namespace

bool OverlayPass::stage(u32 slot, const OverlayDrawData& data, gfx::FrameContext& frames,
                        std::string* error) {
  if (device_ == nullptr || slot >= slots_.size()) {
    if (error != nullptr) *error = "the overlay pass was not created for this frame slot";
    return false;
  }
  for (const OverlayCommand& c : data.commands) {
    if (c.texture >= textures_.size() ||
        textures_[c.texture].slot == gfx::BindlessSet::k_invalid_slot) {
      if (error != nullptr) *error = "an overlay command names a texture the overlay has not";
      return false;
    }
    if (u64{c.first_index} + c.index_count > data.indices.size()) {
      if (error != nullptr) *error = "an overlay command's indices run past the frame's list";
      return false;
    }
  }
  SlotBuffers& s = slots_[slot];
  const u64 vertex_bytes = data.vertices.size() * sizeof(OverlayVertex);
  const u64 index_bytes = data.indices.size() * sizeof(u32);
  if (!ensure_capacity(*device_, s.vertices, vertex_bytes > 0 ? vertex_bytes : 4u, frames, error) ||
      !ensure_capacity(*device_, s.indices, index_bytes > 0 ? index_bytes : 4u, frames, error)) {
    return false;
  }
  if (vertex_bytes > 0) std::memcpy(s.vertices.mapped, data.vertices.data(), vertex_bytes);
  if (index_bytes > 0) std::memcpy(s.indices.mapped, data.indices.data(), index_bytes);
  return true;
}

void OverlayPass::record(gfx::CommandList commands, u32 slot, const OverlayDrawData& data,
                         u32 width, u32 height) const noexcept {
  if (device_ == nullptr || slot >= slots_.size() || width == 0 || height == 0) return;
  const SlotBuffers& s = slots_[slot];
  commands.bind_pipeline(gfx::BindPoint::Graphics, pipeline_);
  // The render graph sets a y-flipped viewport for the frame's passes (clip space y up); the
  // overlay's positions are pixels, y down, so it takes the target as it is.
  gfx::Viewport viewport;
  viewport.width = static_cast<f32>(width);
  viewport.height = static_cast<f32>(height);
  commands.set_viewport(viewport);
  OverlayDrawParams params;
  params.vertices = s.vertices.address;
  params.indices = s.indices.address;
  params.scale[0] = 2.0f / static_cast<f32>(width);
  params.scale[1] = 2.0f / static_cast<f32>(height);
  params.sampler = sampler_slot_;
  const f32 w = static_cast<f32>(width);
  const f32 h = static_cast<f32>(height);
  for (const OverlayCommand& c : data.commands) {
    if (c.index_count == 0) continue;
    const f32 x0 = std::clamp(std::floor(c.clip_x0), 0.0f, w);
    const f32 y0 = std::clamp(std::floor(c.clip_y0), 0.0f, h);
    const f32 x1 = std::clamp(std::ceil(c.clip_x1), 0.0f, w);
    const f32 y1 = std::clamp(std::ceil(c.clip_y1), 0.0f, h);
    if (x1 <= x0 || y1 <= y0) continue;
    gfx::Rect2D scissor;
    scissor.offset.x = static_cast<i32>(x0);
    scissor.offset.y = static_cast<i32>(y0);
    scissor.extent.width = static_cast<u32>(x1 - x0);
    scissor.extent.height = static_cast<u32>(y1 - y0);
    commands.set_scissor(scissor);
    params.first_index = c.first_index;
    params.vertex_offset = c.vertex_offset;
    params.texture = textures_[c.texture].slot;
    commands.push_constants(bindless_->pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(params),
                            &params);
    commands.draw(c.index_count, 1, 0, 0);
  }
}

}  // namespace engine::renderer
