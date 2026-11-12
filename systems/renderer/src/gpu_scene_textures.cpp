// Textures that arrive after the load (docs/subsystems/texture.md, "Textures that arrive after the
// load"; roadmap R147): the scene's table of built textures by `.tex` build key, the admission of
// one without a wait, its release, and the one function that makes a material's record for the
// load's meshes and an admitted mesh's alike.
//
// **Why whole textures by reference.** A tile's mesh arrives at its ring's distance with a material
// set a solver chose by role, so its textures are few and shared, and their residency is the
// mesh's: a texture lives exactly as long as something that names it. The table is the load's own
// sharing rule (one upload per distinct `.tex`, gpu_scene.cpp) carried past the load: the load's
// textures are in it, pinned, and a tile's mesh that names one of them uploads nothing.
//
// **Why a reserved run of pairs.** The resolve reads a normal map's variance chain at the slot
// after its normal's (`k_material_normal_variance`), and a free list of single slots cannot
// promise two adjacent ones once textures have come and gone. So the scene takes
// 2 x `renderer.textures.slots` consecutive sampled-image slots when it is made — nothing has
// freed a descriptor yet, so they are consecutive — and a texture owns a pair for its life; a free
// pair holds a 1 x 1 placeholder.
//
// **Why an image may be written while frames are in flight.** The image is new, its pair's
// descriptors are written before any material names them, and the frame that copies its levels
// (`gfx::record_image_levels_upload`, in the pass that copies an admitted mesh's streams) is the
// first whose materials can name it. On the way out the order is the reverse: the material's entry
// is free at once, but the image, its view and its pair wait `texture_frames_` recorded frames —
// one more than can be in flight — because a frame recorded before the release may sample it.

#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/geometry/material_sampling.h>
#include <domain/gfx/bindless.h>
#include <domain/texture/texture_file.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/gpu_scene.h>

#include <algorithm>
#include <cstring>
#include <renderer_log.h>
#include <string>

namespace engine::renderer {

// gpu_scene.cpp's, which the load's textures and samplers are made by: one definition each.
namespace detail {
gfx::Format built_texture_format(const texture::TextureData& t) noexcept;
bool built_texture_mean_of(const texture::TextureData& t, Vec4& mean);
gfx::SamplerDesc material_sampler_desc(const geometry::TextureSampler& sampler, bool mipmapped);
}  // namespace detail

namespace {

tunables::Int textures_slots{
    "renderer.textures.slots", 1'024, 0, 4'096,
    "Built textures that arrived with a tile's mesh a streamed scene holds at once, at most (two "
    "bindless sampled-image slots each, reserved when the scene is made; 0: none, and such a mesh "
    "draws its materials' constants). Only a scene with a mesh room reserves them"};

tunables::Int textures_budget_mib{
    "renderer.textures.budget_mib", 1'024, 0, 65'536,
    "MiB of device memory the textures that arrived with a tile's mesh may hold (a 2K set is "
    "18.7 MiB). A texture past it is refused, and its mesh draws its materials' constants"};

constexpr u64 aligned16(u64 v) noexcept { return (v + 15) & ~u64{15}; }

}  // namespace

bool GpuScene::lay_out_texture_room(std::string* error) {
  texture_room_ = TextureRoom{};
  texture_use_ = TextureUse{};
  texture_refusal_said_ = false;
  if (mesh_room_.slots == 0) return true;
  // One more than the frames that can be in flight: a frame slot is recorded again only once the
  // frame that last had it is done, so after this many recorded frames every earlier one is.
  texture_frames_ = k_joint_slots + 1;
  const u32 wanted = static_cast<u32>(textures_slots.get());
  const u64 budget = static_cast<u64>(textures_budget_mib.get()) * 1024 * 1024;
  publish_texture_keys();
  if (wanted == 0 || budget == 0) return true;
  const u8 white[4] = {255, 255, 255, 255};
  if (!gfx::upload_image_2d(*device_, 1, 1, gfx::Format::R8G8B8A8Unorm, white, sizeof(white),
                            texture_placeholder_, error) ||
      !gfx::create_image_view(*device_, texture_placeholder_, texture_placeholder_view_, error)) {
    return false;
  }
  // The run, a slot at a time while the set gives the next one: a device whose bindless set is
  // small gives a shorter run, and the room says how many pairs it has.
  u32 run = 0;
  u32 first = 0;
  for (; run < wanted * 2; ++run) {
    const u32 slot =
        bindless_.add_sampled_image(texture_placeholder_view_, gfx::ImageLayout::ShaderReadOnly);
    if (slot == gfx::BindlessSet::k_invalid_slot) break;
    if (run == 0) first = slot;
    if (slot != first + run) {
      if (error != nullptr)
        *error =
            "the texture room's bindless slots are not consecutive (a descriptor was freed "
            "before the scene's textures were laid out)";
      return false;
    }
  }
  texture_room_.pairs = run / 2;
  texture_room_.first_slot = first;
  texture_room_.budget = budget;
  tile_textures_.reserve(texture_room_.pairs);
  leaving_textures_.reserve(texture_room_.pairs);
  free_texture_pairs_.reserve(texture_room_.pairs);
  for (u32 p = texture_room_.pairs; p-- > 0;)
    free_texture_pairs_.push_back(p);  // pair 0 is taken first
  pending_images_.reserve(16);
  ENGINE_LOG_INFO(log_renderer, "texture room", log::field("pairs", texture_room_.pairs),
                  log::field("first_slot", first),
                  log::field("budget_mib", textures_budget_mib.get()),
                  log::field("load_textures", load_textures_.size()));
  return true;
}

void GpuScene::publish_texture_keys() {
  std::lock_guard<std::mutex> lock(texture_keys_mutex_);
  texture_keys_.clear();
  for (const auto& held : load_textures_)
    texture_keys_.push_back(held.first);
  for (const TileTexture& held : tile_textures_)
    texture_keys_.push_back(held.key);
  std::sort(texture_keys_.begin(), texture_keys_.end());
}

bool GpuScene::texture_resident(u64 key) const {
  std::lock_guard<std::mutex> lock(texture_keys_mutex_);
  return std::binary_search(texture_keys_.begin(), texture_keys_.end(), key);
}

bool GpuScene::find_texture(u64 key, TextureInfo& out) const noexcept {
  if (const TextureInfo* pinned = load_textures_.find_value(key)) {
    out = *pinned;
    return true;
  }
  for (const TileTexture& held : tile_textures_) {
    if (held.key != key) continue;
    out = held.info;
    return true;
  }
  return false;
}

GpuScene::TextureAdmit GpuScene::acquire_texture(const PlacementTexture& texture,
                                                 std::string* why) {
  const auto refuse = [&](std::string sentence) {
    ++texture_use_.refused;
    if (why != nullptr) *why = std::move(sentence);
    return TextureAdmit::refused;
  };
  const u64 key = texture.key;
  if (load_textures_.find_value(key) != nullptr) {
    ++texture_use_.shared;  // the load's: pinned, nothing to count
    return TextureAdmit::held;
  }
  for (TileTexture& held : tile_textures_) {
    if (held.key != key) continue;
    ++held.refs;
    ++texture_use_.references;
    ++texture_use_.shared;
    return TextureAdmit::held;
  }
  // Let go a moment ago and wanted again: its image is still there, and so is its pair.
  for (u32 i = 0; i < leaving_textures_.size(); ++i) {
    if (leaving_textures_[i].key != key) continue;
    TileTexture back = leaving_textures_[i];
    leaving_textures_[i] = leaving_textures_.back();
    leaving_textures_.pop_back();
    --texture_use_.leaving;
    texture_use_.leaving_bytes -= back.bytes;
    back.refs = 1;
    back.frames = 0;
    tile_textures_.push_back(back);
    ++texture_use_.textures;
    ++texture_use_.references;
    texture_use_.bytes += back.bytes;
    ++texture_use_.shared;
    ++texture_use_.revived;
    publish_texture_keys();
    return TextureAdmit::held;
  }
  if (texture_room_.pairs == 0) {
    return refuse(
        "the scene keeps no room for a texture that arrives with a tile's mesh "
        "(renderer.textures.slots or renderer.textures.budget_mib is 0, or the scene has "
        "no mesh room)");
  }
  const texture::TextureData& t = texture.data;
  if (t.levels.empty()) return TextureAdmit::need_bytes;
  const gfx::Format format = detail::built_texture_format(t);
  const bool compressed = t.format != texture::TextureFormat::rgba8;
  if (format == gfx::Format::Undefined || t.levels.size() > gfx::k_max_image_levels)
    return refuse("a texture of a format or a chain the upload does not take");
  if (compressed && !device_->features().texture_compression_bc)
    return refuse("the device has no textureCompressionBC");
  const bool variance = normal_filter_tunable() && !t.normal_variance.empty();
  const u64 staged = texture.staged_bytes();
  if (texture_use_.bytes + staged > texture_room_.budget) {
    return refuse("the textures that arrived with tiles' meshes hold " +
                  std::to_string(texture_use_.bytes) + " bytes and this one is " +
                  std::to_string(staged) + " (renderer.textures.budget_mib)");
  }
  if (free_texture_pairs_.empty()) {
    return refuse("every one of the " + std::to_string(texture_room_.pairs) +
                  " texture pairs holds a texture (renderer.textures.slots)");
  }
  TileTexture made;
  made.key = key;
  made.refs = 1;
  std::string error;
  const auto undo = [&] {
    if (made.view.valid()) gfx::destroy_image_view(*device_, made.view);
    if (made.variance_view.valid()) gfx::destroy_image_view(*device_, made.variance_view);
    if (made.image.image.valid()) gfx::destroy_image(*device_, made.image);
    if (made.variance.image.valid()) gfx::destroy_image(*device_, made.variance);
  };
  const u32 levels = t.levels.size();
  if (!gfx::create_sampled_image_levels(*device_, t.width, t.height, format, levels, made.image,
                                        &error) ||
      !gfx::create_image_view(*device_, made.image, made.view, &error) ||
      (variance &&
       (!gfx::create_sampled_image_levels(*device_, t.width, t.height, gfx::Format::Bc4Unorm,
                                          levels, made.variance, &error) ||
        !gfx::create_image_view(*device_, made.variance, made.variance_view, &error)))) {
    undo();
    return refuse("the device made no image for it: " + error);
  }
  // Its levels into staging, each at a 16-byte offset (a multiple of every block size), the
  // variance chain's after them; the next frame's upload pass copies them out.
  gfx::BufferResource staging;
  u64 at = 0;
  if (!stage_bytes(staged, staging, at, &error)) {
    undo();
    return refuse("no staging for it: " + error);
  }
  u8* base = static_cast<u8*>(staging.mapped);
  TerrainUpdate::ImageUpload upload;
  upload.image = made.image;
  upload.staging = staging.buffer;
  u64 offset = at;
  for (u32 l = 0; l < levels; ++l) {
    const std::span<const u8> bytes = t.level_bytes(l);
    upload.offsets[l] = offset;
    std::memcpy(base + offset, bytes.data(), bytes.size());
    offset += aligned16(bytes.size());
  }
  pending_images_.push_back(upload);
  pending_geometry_bytes_ += staged;
  if (variance) {
    upload.image = made.variance;
    for (u32 l = 0; l < levels; ++l) {
      const std::span<const u8> bytes = t.normal_variance_level_bytes(l);
      upload.offsets[l] = offset;
      std::memcpy(base + offset, bytes.data(), bytes.size());
      offset += aligned16(bytes.size());
    }
    pending_images_.push_back(upload);
  }
  ENGINE_ASSERT(offset - at <= staged, "a texture's staged bytes are what it said");
  made.pair = free_texture_pairs_.back();
  free_texture_pairs_.pop_back();
  made.bytes = made.image.bytes + made.variance.bytes;
  made.info.slot = texture_room_.first_slot + 2 * made.pair;
  made.info.normal_rg = t.format == texture::TextureFormat::bc5;
  made.info.normal_variance = variance;
  made.info.has_mean = detail::built_texture_mean_of(t, made.info.mean);
  // The pair's descriptors: no material names them before the frame that copies the levels, so no
  // frame in flight reads what is written here.
  bindless_.update_sampled_image(made.info.slot, made.view, gfx::ImageLayout::ShaderReadOnly);
  if (variance) {
    bindless_.update_sampled_image(made.info.slot + 1, made.variance_view,
                                   gfx::ImageLayout::ShaderReadOnly);
  }
  tile_textures_.push_back(made);
  ++texture_use_.textures;
  ++texture_use_.references;
  texture_use_.bytes += made.bytes;
  ++texture_use_.uploaded;
  texture_use_.staged_bytes += staged;
  publish_texture_keys();
  return TextureAdmit::held;
}

void GpuScene::release_texture(u64 key) noexcept {
  for (u32 i = 0; i < tile_textures_.size(); ++i) {
    TileTexture& held = tile_textures_[i];
    if (held.key != key) continue;
    --texture_use_.references;
    if (--held.refs != 0) return;
    // Nobody holds it: out of the table now, its image and its pair once the frames that could
    // have sampled it are done (`texture_frame`).
    TileTexture leaving = held;
    leaving.frames = texture_frames_;
    tile_textures_[i] = tile_textures_.back();
    tile_textures_.pop_back();
    leaving_textures_.push_back(leaving);
    --texture_use_.textures;
    texture_use_.bytes -= leaving.bytes;
    ++texture_use_.leaving;
    texture_use_.leaving_bytes += leaving.bytes;
    ++texture_use_.released;
    publish_texture_keys();
    return;
  }
}

void GpuScene::texture_frame() noexcept {
  for (u32 i = 0; i < leaving_textures_.size();) {
    TileTexture& leaving = leaving_textures_[i];
    if (--leaving.frames != 0) {
      ++i;
      continue;
    }
    const u32 slot = texture_room_.first_slot + 2 * leaving.pair;
    bindless_.update_sampled_image(slot, texture_placeholder_view_,
                                   gfx::ImageLayout::ShaderReadOnly);
    bindless_.update_sampled_image(slot + 1, texture_placeholder_view_,
                                   gfx::ImageLayout::ShaderReadOnly);
    gfx::destroy_image_view(*device_, leaving.view);
    gfx::destroy_image(*device_, leaving.image);
    if (leaving.variance.image.valid()) {
      gfx::destroy_image_view(*device_, leaving.variance_view);
      gfx::destroy_image(*device_, leaving.variance);
    }
    free_texture_pairs_.push_back(leaving.pair);
    --texture_use_.leaving;
    texture_use_.leaving_bytes -= leaving.bytes;
    leaving_textures_[i] = leaving_textures_.back();
    leaving_textures_.pop_back();
  }
}

void GpuScene::destroy_texture_room() noexcept {
  if (device_ == nullptr) return;
  for (Vector<TileTexture>* list : {&tile_textures_, &leaving_textures_}) {
    for (TileTexture& held : *list) {
      gfx::destroy_image_view(*device_, held.view);
      gfx::destroy_image(*device_, held.image);
      if (held.variance.image.valid()) {
        gfx::destroy_image_view(*device_, held.variance_view);
        gfx::destroy_image(*device_, held.variance);
      }
    }
    list->clear();
  }
  // An upload never recorded names an image the lists above just destroyed: nothing to free.
  pending_images_.clear();
  free_texture_pairs_.clear();
  load_textures_.clear();
  if (texture_placeholder_view_.valid())
    gfx::destroy_image_view(*device_, texture_placeholder_view_);
  texture_placeholder_view_ = {};
  if (texture_placeholder_.image.valid()) gfx::destroy_image(*device_, texture_placeholder_);
  texture_placeholder_ = {};
  texture_room_ = TextureRoom{};
  {
    std::lock_guard<std::mutex> lock(texture_keys_mutex_);
    texture_keys_.clear();
  }
}

// ---- one function makes a material
// ----------------------------------------------------------------

bool GpuScene::material_sampler_slot(const geometry::TextureSampler& wanted, bool mipmapped,
                                     u32& slot, std::string* error) {
  // One per distinct combination a textured slot asks for, made when the first slot that asks for
  // it is met — so a scene with no textures has exactly the bindless set it always had, and a
  // thousand materials on the glTF default share one sampler.
  const u32 key = geometry::pack_texture_sampler(wanted) | (mipmapped ? 0x80000000u : 0u);
  for (const MaterialSampler& known : material_samplers_) {
    if (known.key != key) continue;
    slot = known.slot;
    return true;
  }
  MaterialSampler made;
  made.key = key;
  if (!gfx::create_sampler(*device_, detail::material_sampler_desc(wanted, mipmapped), made.sampler,
                           error)) {
    return false;
  }
  made.slot = bindless_.add_sampler(made.sampler);
  material_samplers_.push_back(made);
  slot = made.slot;
  return true;
}

bool GpuScene::fill_material(const assets::Material& source, bool built, const TextureInfo* tex,
                             gfx::ResolveMaterial& out, std::string* error) {
  gfx::ResolveMaterial material;
  material.albedo =
      Vec4{source.base_color.x, source.base_color.y, source.base_color.z, source.roughness};
  // The emissive factor, which the emissive texture multiplies where there is one.
  material.emissive = Vec4{source.emissive, source.metallic};
  material.albedo_texture = tex[geometry::k_slot_base_color].slot;
  material.metallic_roughness_texture = tex[geometry::k_slot_metallic_roughness].slot;
  material.normal_texture = tex[geometry::k_slot_normal].slot;
  material.occlusion_texture = tex[geometry::k_slot_occlusion].slot;
  material.emissive_texture = tex[geometry::k_slot_emissive].slot;
  material.normal_scale = source.normal_scale;
  material.occlusion_strength = source.occlusion_strength;
  material.uv_scale = 1.0f;
  if (built) {
    material.flags |= gfx::k_material_mipped;
    // A BC5 normal map holds x and y; the shader rebuilds z (material.slang).
    const TextureInfo& normal = tex[geometry::k_slot_normal];
    if (normal.normal_rg) material.flags |= gfx::k_material_normal_rg;
    if (normal.normal_variance && material.normal_texture != gfx::k_no_texture)
      material.flags |= gfx::k_material_normal_variance;
  }
  // Each textured slot's own sampler, and one UV transform for the material: the base colour's
  // where it is textured, the first textured slot's otherwise. A slot with no texture asks for no
  // sampler and borrows `sampler` (gfx::k_same_sampler), which it never reads.
  const u32 slot_texture[geometry::k_material_slots] = {
      material.albedo_texture, material.metallic_roughness_texture, material.normal_texture,
      material.occlusion_texture, material.emissive_texture};
  u32 slot_sampler[geometry::k_material_slots];
  i32 transform_slot = -1;
  for (u32 s = 0; s < geometry::k_material_slots; ++s) {
    slot_sampler[s] = gfx::k_same_sampler;
    if (slot_texture[s] == gfx::k_no_texture) continue;
    if (!material_sampler_slot(source.sampling[s].sampler, built, slot_sampler[s], error))
      return false;
    if (transform_slot < 0) transform_slot = static_cast<i32>(s);
  }
  material.sampler = slot_sampler[geometry::k_slot_base_color] != gfx::k_same_sampler
                         ? slot_sampler[geometry::k_slot_base_color]
                         : (transform_slot >= 0 ? slot_sampler[transform_slot] : 0u);
  material.samplers_mr_normal = slot_sampler[geometry::k_slot_metallic_roughness] |
                                (slot_sampler[geometry::k_slot_normal] << 16);
  material.samplers_occlusion_emissive =
      slot_sampler[geometry::k_slot_occlusion] | (slot_sampler[geometry::k_slot_emissive] << 16);
  if (transform_slot >= 0) {
    const geometry::TextureTransform& transform = source.sampling[transform_slot].transform;
    bool conflict = false;
    for (u32 s = 0; s < geometry::k_material_slots; ++s) {
      if (slot_texture[s] != gfx::k_no_texture && source.sampling[s].transform != transform)
        conflict = true;
    }
    if (conflict) ++transform_conflicts_;
    gfx::set_uv_transform(material, transform.offset, transform.rotation, transform.scale);
  }
  // An alpha-tested material (renderer.md, "Alpha-tested geometry"): after the transform, which
  // clears the two words the cutoff and the factor's alpha go in.
  if (source.alpha_mode == assets::k_alpha_mask)
    gfx::set_alpha_mask(material, source.alpha_cutoff, source.base_color.w);
  out = material;
  return true;
}

const char* GpuScene::fill_stochastic(gfx::ResolveMaterial& material,
                                      const assets::Material& source, bool built,
                                      const TextureInfo* tex) noexcept {
  if (!built) return "its textures are not built (the means come from the built chain)";
  const auto mean_of = [&](i32 image, u32 slot, Vec4& out) {
    out = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    if (image < 0) return true;
    if (!tex[slot].has_mean) return false;
    out = tex[slot].mean;
    return true;
  };
  Vec4 colour_texel;
  Vec4 roughness_texel;
  if (!mean_of(source.base_color_image, geometry::k_slot_base_color, colour_texel) ||
      !mean_of(source.metallic_roughness_image, geometry::k_slot_metallic_roughness,
               roughness_texel)) {
    return "a texture's mean did not decode";
  }
  if (!gfx::set_stochastic(
          material,
          Vec3{material.albedo.x * colour_texel.x, material.albedo.y * colour_texel.y,
               material.albedo.z * colour_texel.z},
          material.albedo.w * roughness_texel.y)) {
    return "the material is alpha-tested or water, whose word the means would take";
  }
  return nullptr;
}

}  // namespace engine::renderer
