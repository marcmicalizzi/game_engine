// **A texture's levels resident by an exact bound** (R172 stage 1; ADR-0065; docs/subsystems/
// texture.md, "Levels resident by what the frame samples").
//
// The load uploads a texture the bound manages from its floor (the levels of `renderer.textures.
// floor` texels a side and smaller), into a pair of a reserved run of bindless slots. Once a
// recorded frame, before anything is staged, the bound is worked out where the observer is: per
// mesh, the nearest any instance of it comes to the eye a lead ahead, against its least texel
// density over every cluster (`texture::least_cluster_density`) at the instance's largest axis
// scale, gives the finest level a pixel of that mesh can read; per texture, the finest over the
// meshes that sample it. A texture that wants a finer top than it holds is **promoted**: its levels
// from the new top are read from the `.tex` by range on the reader thread, made into a new image in
// a free pair, staged into the frame's "terrain upload" pass, and the material records that name it
// are rewritten to the new pair in the same pass, behind the barrier that waits for the frames in
// flight; the old image and pair go `k_joint_slots + 1` recorded frames later. A cut (the first
// frame, `FrameDesc::history_reset`, `texture_bound_whole`) brings every wanted level in before the
// frame draws, on the recording thread.
//
// **Why a level the bound leaves out changes no pixel.** A pixel at angle phi off the axis of a
// rectilinear camera with `f` pixels a radian spans at least `cos^2 phi / f` radians along any
// screen axis, so on a surface at distance `r` at least `r cos^2 phi / f` metres; through a
// triangle whose least stretched direction carries `sigma` UV a metre, at instance scale `s`, on an
// image of shorter side `n`, that is at least `n sigma r cos^2 phi / (f s)` texels a pixel step
// along x and along y. Vulkan's anisotropic level of detail is log2 of max(rho_max / N, rho_min) >=
// log2 rho_min with the exact rho; an implementation may take each rho from its components' maximum
// (down to 1/sqrt 2 of the length: half a level) and round the anisotropy N up to a whole sample
// count (rho_max / ceil(rho_max / rho_min) >= rho_min / 2: one level), and the measure is taken
// over the float vertices where the rasterizers read the 16-bit grid. So the finest level a pixel
// reads is at least floor(log2(that) - margin), the margin
// `renderer.textures.bound_margin`, 1.53125 levels by default (1.5 for the two implementation
// freedoms, 1/32 for the grid and the LOD's fixed point).

#include "texture_bound_state.h"

#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/geometry/material_sampling.h>
#include <domain/gfx/bindless.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/gpu_scene.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <renderer_log.h>
#include <utility>

namespace engine::renderer {

namespace {

// Off by default: on the engine's own scenes the bound keeps every level by the end of a walk
// (their sets are shared by every house and tiled at about a thousand texels a metre, so the
// nearest instance wants level 0 from 30 m at 4K) and costs a first frame of 0.17 to 0.33 s and a
// settle at an undeclared jump; it saves nothing until a demotion and stage 2's trim (texture.md,
// "Stage 1: the bound, built").
tunables::Bool bound_on{"renderer.textures.bound", false,
                        "Hold the load's built textures by level: upload from the floor, and bring "
                        "in off the frame the levels an observer a lead ahead could read, which "
                        "changes no pixel (texture.md, \"Levels resident by what the frame "
                        "samples\"). Read once per load"};
tunables::Int bound_floor{"renderer.textures.floor", 128, 1, 16'384,
                          "Texels a side: every texture the bound holds keeps its levels this "
                          "size and smaller whatever an observer can read (the floor)"};
tunables::Float bound_margin{
    "renderer.textures.bound_margin", 1.53125, 0.0, 8.0,
    "Levels the bound keeps above the finest it computes a pixel can read: an implementation's "
    "freedom in the anisotropic level of detail (1.5) and the grid's rounding (1/32)"};
tunables::Float bound_lead_m{"renderer.textures.lead_m", 2.0, 0.0, 10'000.0,
                             "Metres the bound looks ahead of the observer at the least"};
tunables::Int bound_lead_frames{
    "renderer.textures.lead_frames", 30, 0, 6'000,
    "Frames of the observer's last step the bound looks ahead beyond `lead_m`"};
tunables::Float bound_jump_m{
    "renderer.textures.jump_m", 1.0, 0.0, 1'000'000.0,
    "Metres the eye moves in one frame past which the bound takes the frame for a cut: it brings "
    "the levels it wants in before the frame draws, and the step does not stretch the lead"};
tunables::Int bound_promote_kib{
    "renderer.textures.promote_kib", 4'096, 0, 1 << 20,
    "KiB of promoted texture levels one frame stages; one larger goes alone (0: no limit)"};
tunables::Bool bound_wait{"renderer.textures.bound_wait", false,
                          "A frame whose observer can read a level the bound has not brought in "
                          "yet brings it in before drawing, on the recording thread (tests)"};
// Stage 2 (texture.md, "Stage 2: the trim under a ceiling"). The ceiling is this row's own, so that
// ADR-0063's open decisions stay open: whichever budget that ADR settles on sets it.
tunables::Float residency_ceiling_mib{
    "renderer.textures.ceiling_mib", 0.0, 0.0, 16'777'216.0,
    "MiB of texture the scene may hold before the residency trims: under pressure the levels no "
    "frame asked for within `hold_s` go, least recently asked first and only as far as the ceiling "
    "needs, never below the floor (0, the default: no ceiling, nothing is trimmed and the textures "
    "are the load's). Read once per load"};
tunables::Float residency_hold_s{
    "renderer.textures.hold_s", 8.0, 0.0, 3'600.0,
    "Seconds of the frames' own clock a level stays after the feedback last asked for it or it was "
    "brought in: under a ceiling, nothing younger is trimmed (a minimum age, not a clock)"};

// W167 (W47's stage 1; texture.md, "Stage 3: the bytes go back to the device").
tunables::Int residency_chunk_mib{
    "renderer.textures.chunk_mib", 64, 1, 4'096,
    "MiB of a chunk of the device memory the held textures' images are placed in: a chunk goes "
    "back "
    "to the device when its last image goes, so a trim's bytes leave the driver's usage. Read once "
    "per load"};
tunables::Int residency_device_budget_mib{
    "renderer.textures.device_budget_mib", 0, 0, 1 << 20,
    "MiB of the device the process may hold: with no `ceiling_mib` named, the textures' ceiling is "
    "this less what everything else holds on the device, worked out at the first frame, at a cut "
    "and "
    "every 10 s of the frames' clock (0, the default: none named, and the ceiling is "
    "`ceiling_mib`'s "
    "alone). Read once per load"};

constexpr u32 k_spare_pairs = 64;  // pairs a promotion takes while its old one waits out the frames
constexpr u64 aligned16(u64 v) noexcept { return (v + 15) & ~u64{15}; }
constexpr f64 k_never = 1.0e30;  // a mesh no instance places: it asks nothing

u32 floor_top_of(u32 width, u32 height, u32 levels, u32 floor) noexcept {
  u32 k = 0;
  while (k + 1 < levels && std::max(texture::texture_level_extent(width, k),
                                    texture::texture_level_extent(height, k)) > floor) {
    ++k;
  }
  return k;
}

void reader(TextureBoundState* s) {
  std::unique_lock<std::mutex> lock(s->mutex);
  for (;;) {
    s->wake.wait(lock, [s] {
      return s->stop || (!s->queue.empty() && (s->in_hand == 0 || s->in_hand < s->in_hand_bound));
    });
    if (s->stop) return;
    TextureBoundState::Read read = std::move(s->queue.back());
    s->queue.pop_back();
    lock.unlock();
    read.ok = texture::read_texture_file_levels(*read.path, read.first, read.data, nullptr);
    lock.lock();
    s->in_hand += read.data.data.size() + read.data.normal_variance.size();
    s->done.push_back(std::move(read));
  }
}

}  // namespace

namespace {
// The texture a key names, or null: one the bound does not hold, or dropped past the run's room.
const u32* find_held(const TextureBoundState& s, u64 key) noexcept {
  const u32* at = s.by_key.find_value(key);
  return at != nullptr && *at < s.textures.size() ? at : nullptr;
}
}  // namespace

bool texture_bound_enabled() {
  return bound_on.get() || residency_ceiling_mib.get() > 0.0 ||
         residency_device_budget_mib.get() > 0;
}

namespace {
// Bytes a 4x4 block takes in a texture's format (BC1 and BC4 eight, the rest sixteen).
u64 block_bytes(gfx::Format format) noexcept {
  return format == gfx::Format::Bc1RgbUnorm || format == gfx::Format::Bc1RgbSrgb ||
                 format == gfx::Format::Bc4Unorm
             ? 8u
             : 16u;
}
// A level's bytes as the blocks count them, its variance chain's (BC4) beside it: what a trim plans
// to release and a promotion to add, before the allocator rounds.
u64 level_bytes_of(const TextureBoundState::Texture& tex, u32 level) noexcept {
  const u64 blocks = u64{(texture::texture_level_extent(tex.width, level) + 3) / 4} *
                     ((texture::texture_level_extent(tex.height, level) + 3) / 4);
  return blocks * (block_bytes(tex.format) + (tex.variance ? 8u : 0u));
}
// The chunks' account into the use a test and a measurement read (W167).
void sync_chunk_use(TextureBoundState& s) noexcept {
  const gfx::ImageChunkUse chunks = s.chunks.use();
  s.use.chunks = chunks.chunks;
  s.use.chunk_bytes = chunks.chunk_bytes;
  s.use.chunk_used_bytes = chunks.used_bytes;
  s.use.chunks_made = chunks.made;
  s.use.chunks_freed = chunks.freed;
  s.use.most_chunk_bytes = chunks.most_chunk_bytes;
}
u64 chain_bytes_from(const TextureBoundState::Texture& tex, u32 top) noexcept {
  u64 bytes = 0;
  for (u32 l = top; l < tex.levels; ++l)
    bytes += level_bytes_of(tex, l);
  return bytes;
}
}  // namespace

void GpuScene::TextureBoundDelete::operator()(TextureBoundState* state) const noexcept {
  delete state;
}

const TextureBoundUse& GpuScene::texture_bound_use() const noexcept {
  static const TextureBoundUse none{};
  return texture_bound_ != nullptr ? texture_bound_->use : none;
}

gfx::ImageChunks* GpuScene::texture_bound_chunks() noexcept {
  return texture_bound_ != nullptr && texture_bound_->chunks.valid() ? &texture_bound_->chunks
                                                                     : nullptr;
}

void GpuScene::destroy_held_image(gfx::ImageChunks* chunks, gfx::ImageResource& image) noexcept {
  if (chunks != nullptr) {
    chunks->destroy_image(image);
  } else if (device_ != nullptr) {
    gfx::destroy_image(*device_, image);
  }
}

u8* GpuScene::staged_bytes_of(gfx::BufferHandle buffer) noexcept {
  if (staging_ring_.mapped != nullptr && staging_ring_.buffer == buffer)
    return static_cast<u8*>(staging_ring_.mapped);
  for (const gfx::BufferResource& own : pending_staging_)
    if (own.buffer == buffer) return static_cast<u8*>(own.mapped);
  return nullptr;
}

u32 GpuScene::texture_bound_slot(u64 key) const noexcept {
  if (texture_bound_ == nullptr || texture_bound_->bound) return ~0u;
  const u32* at = find_held(*texture_bound_, key);
  if (at == nullptr || !texture_bound_->textures[*at].placed) return ~0u;
  return texture_bound_->first_slot + 2 * texture_bound_->textures[*at].pair;
}

void GpuScene::texture_bound_admitted(u32 first_material,
                                      std::span<const gfx::ResolveMaterial> made) {
  if (texture_bound_ == nullptr || texture_bound_->bound) return;
  TextureBoundState& s = *texture_bound_;
  for (u32 e = 0; e < made.size(); ++e) {
    const gfx::ResolveMaterial& record = made[e];
    u32 copy = ~0u;
    for (const u32 slot :
         {record.albedo_texture, record.metallic_roughness_texture, record.normal_texture,
          record.occlusion_texture, record.emissive_texture}) {
      if (slot < s.first_slot || slot >= s.first_slot + 2 * s.pairs) continue;
      if (((slot - s.first_slot) & 1u) != 0) continue;  // a variance slot is named by its normal's
      const u32 t = s.pair_texture[(slot - s.first_slot) / 2];
      if (t == ~0u) continue;
      if (copy == ~0u) {
        if (!s.free_materials.empty()) {
          copy = s.free_materials.back();
          s.free_materials.pop_back();
          s.materials[copy] = TextureBoundState::MaterialCopy{first_material + e, record};
        } else {
          copy = s.materials.size();
          s.materials.push_back(TextureBoundState::MaterialCopy{first_material + e, record});
        }
        s.copy_of_material.insert(first_material + e, copy);
      }
      Vector<u32>& named = s.textures[t].materials;
      if (std::find(named.begin(), named.end(), copy) == named.end()) named.push_back(copy);
    }
  }
}

void GpuScene::texture_bound_retired(u32 first_material, u32 count) noexcept {
  if (texture_bound_ == nullptr || texture_bound_->copy_of_material.empty()) return;
  TextureBoundState& s = *texture_bound_;
  for (u32 e = first_material; e < first_material + count; ++e) {
    const u32* at = s.copy_of_material.find_value(e);
    if (at == nullptr) continue;
    const u32 copy = *at;
    s.copy_of_material.erase(e);
    // Its record names the pairs its textures hold now (a swap keeps it current).
    const gfx::ResolveMaterial& record = s.materials[copy].record;
    for (const u32 slot :
         {record.albedo_texture, record.metallic_roughness_texture, record.normal_texture,
          record.occlusion_texture, record.emissive_texture}) {
      if (slot < s.first_slot || slot >= s.first_slot + 2 * s.pairs) continue;
      if (((slot - s.first_slot) & 1u) != 0) continue;
      const u32 t = s.pair_texture[(slot - s.first_slot) / 2];
      if (t == ~0u) continue;
      Vector<u32>& named = s.textures[t].materials;
      for (u32 i = 0; i < named.size(); ++i) {
        if (named[i] != copy) continue;
        named[i] = named.back();
        named.pop_back();
        break;
      }
    }
    s.free_materials.push_back(copy);
  }
}

bool GpuScene::texture_bound_manages(u64 key) const noexcept {
  if (texture_bound_ == nullptr) return false;
  const u32* at = find_held(*texture_bound_, key);
  return at != nullptr && texture_bound_->textures[*at].placed;
}

// ---- the load's half ------------------------------------------------------------------------

bool GpuScene::texture_bound_plan(std::string* error) {
  destroy_texture_bound();
  if (!texture_bound_enabled() || device_ == nullptr || !device_->features().texture_compression_bc)
    return true;
  const SceneData& d = *data_;
  const u32 mesh_count = d.sources.size();
  // A mesh none of whose textures the bound holds: one an instance deforms (the density is the rest
  // pose's), the terrain (its layers are sampled by their own rule) and a water body (its own).
  Vector<u8> excluded(mesh_count, u8{0});
  for (const gfx::InstanceDesc& instance : instance_table_)
    if (instance.mesh < mesh_count && instance.deform != gfx::k_invalid_deform)
      excluded[instance.mesh] = 1;
  if (d.terrain_mesh < mesh_count) excluded[d.terrain_mesh] = 1;
  for (u32 m = 0; m < mesh_count && m < d.mesh_info.size(); ++m)
    if (d.mesh_info[m].water.has_value()) excluded[m] = 1;
  // A key one of its users cannot have bounded is uploaded whole for all: an alpha-tested
  // material's base colour (the rays' mask tests and the micromap bake read level 0 at any
  // distance), any texture of an excluded or open mesh.
  HashMap<u64, u8> refused;
  for (u32 m = 0; m < mesh_count; ++m) {
    const SourceMesh& mesh = d.sources[m];
    const bool can = excluded[m] == 0 && !mesh.texture_ddc.empty() &&
                     mesh.texture_least_density.size() == mesh.textures.size() &&
                     mesh.textures.size() == mesh.data.images.size();
    for (const assets::Material& material : mesh.data.materials) {
      const i32 images[geometry::k_material_slots] = {
          material.base_color_image, material.metallic_roughness_image, material.normal_image,
          material.occlusion_image, material.emissive_image};
      for (u32 slot = 0; slot < geometry::k_material_slots; ++slot) {
        const i32 image = images[slot];
        if (image < 0 || static_cast<u32>(image) >= mesh.textures.size()) continue;
        const u64 key = mesh.textures[static_cast<u32>(image)].key;
        if (key == 0) continue;
        const bool masked =
            slot == geometry::k_slot_base_color && material.alpha_mode == assets::k_alpha_mask;
        if (!can || masked) refused.try_emplace(key, u8{1});
      }
    }
  }
  auto s = std::make_unique<TextureBoundState>();
  s->mesh_users.resize(mesh_count);
  s->mesh_sphere.assign(mesh_count, Vec4{});
  for (u32 m = 0; m < mesh_count; ++m) {
    const SourceMesh& mesh = d.sources[m];
    if (mesh.texture_least_density.size() != mesh.textures.size() || excluded[m] != 0) continue;
    s->mesh_sphere[m] = mesh.texture_sphere;
    for (u32 i = 0; i < mesh.textures.size(); ++i) {
      const u64 key = mesh.textures[i].key;
      if (key == 0 || refused.find_value(key) != nullptr) continue;
      bool sampled = false;
      for (const assets::Material& material : mesh.data.materials) {
        for (const i32 image :
             {material.base_color_image, material.metallic_roughness_image, material.normal_image,
              material.occlusion_image, material.emissive_image}) {
          sampled = sampled || image == static_cast<i32>(i);
        }
      }
      if (!sampled) continue;
      u32 t = 0;
      if (const u32* at = s->by_key.find_value(key)) {
        t = *at;
      } else {
        t = s->textures.size();
        s->by_key.insert(key, t);
        TextureBoundState::Texture texture;
        texture.key = key;
        texture.path = texture::texture_cache_path(mesh.texture_ddc, key);
        s->textures.push_back(std::move(texture));
      }
      const f64 sigma = mesh.texture_least_density[i];
      const f64 log2_sigma = sigma > 0.0 ? std::log2(sigma) : -k_never;
      bool merged = false;
      for (TextureBoundState::User& user : s->mesh_users[m]) {
        if (user.texture != t) continue;
        user.log2_texels_per_m = std::min(user.log2_texels_per_m, log2_sigma);
        merged = true;
      }
      if (!merged) s->mesh_users[m].push_back(TextureBoundState::User{t, log2_sigma});
    }
  }
  if (s->textures.empty()) return true;
  // The run: a pair a texture and a few spare, while the set has freed nothing and its next slots
  // are consecutive. Never more than half of what is left, so the textures the bound does not
  // hold, the variance chains and a tile texture room still find theirs; past it, keys are dropped
  // from the end and uploaded whole.
  const u32 capacity = gfx::BindlessConfig{}.sampled_images;
  const u32 live = bindless_.live_sampled_images();
  const u32 room = capacity > live ? (capacity - live) / 4 : 0u;
  u32 wanted = s->textures.size() + k_spare_pairs;
  if (wanted > room) {
    const u32 keep = room > k_spare_pairs ? room - k_spare_pairs : 0u;
    ENGINE_LOG_WARN(log_renderer, "texture bound: fewer bindless pairs than textures",
                    log::field("textures", s->textures.size()), log::field("held", keep));
    if (keep == 0) return true;
    s->textures.resize(keep);  // a key whose index is past it is not held (`find_held`)
    for (Vector<TextureBoundState::User>& users : s->mesh_users) {
      u32 kept = 0;
      for (u32 u = 0; u < users.size(); ++u)
        if (users[u].texture < keep) users[kept++] = users[u];
      users.resize(kept);
    }
    wanted = keep + k_spare_pairs;
  }
  const u8 white[4] = {255, 255, 255, 255};
  if (!gfx::upload_image_2d(*device_, 1, 1, gfx::Format::R8G8B8A8Unorm, white, sizeof(white),
                            s->placeholder, error) ||
      !gfx::create_image_view(*device_, s->placeholder, s->placeholder_view, error)) {
    return false;
  }
  for (u32 k = 0; k < 2 * wanted; ++k) {
    const u32 slot =
        bindless_.add_sampled_image(s->placeholder_view, gfx::ImageLayout::ShaderReadOnly);
    if (k == 0) s->first_slot = slot;
    if (slot == gfx::BindlessSet::k_invalid_slot || slot != s->first_slot + k) {
      if (error != nullptr) *error = "the texture bound's bindless slots are not consecutive";
      gfx::destroy_image_view(*device_, s->placeholder_view);
      gfx::destroy_image(*device_, s->placeholder);
      return false;
    }
  }
  s->pairs = wanted;
  s->free_pairs.reserve(wanted);
  for (u32 p = wanted; p-- > 0;)
    s->free_pairs.push_back(p);
  // Stage 2: what the residency reads once per load, and everything its frames touch, made now.
  s->bound = bound_on.get();
  s->ceiling = static_cast<u64>(residency_ceiling_mib.get() * 1'048'576.0);
  // W167: a device budget names the ceiling where `ceiling_mib` does not; until the first frame
  // can say what the rest of the process holds, the budget itself stands in (nothing is trimmed
  // before a frame has run).
  s->budget = u64(residency_device_budget_mib.get()) * 1'048'576;
  if (s->ceiling == 0) s->ceiling = s->budget;
  s->use.budget = s->budget;
  // The held images in chunks of their own: every texture's image and its variance, each with
  // one leaving beside it and a move or a swap in flight.
  if (!s->chunks.create(*device_, u64(residency_chunk_mib.get()) * 1'048'576,
                        s->textures.size() * 6u + 64u, error)) {
    gfx::destroy_image_view(*device_, s->placeholder_view);
    gfx::destroy_image(*device_, s->placeholder);
    return false;
  }
  s->hold_s = residency_hold_s.get();
  s->use.ceiling = s->ceiling;
  s->pair_texture.assign(wanted, ~0u);
  s->pair_top.assign(wanted, 0u);
  s->pair_since.assign(wanted, u64{0});
  s->candidates.reserve(s->textures.size() * 16u);
  s->plan_top.assign(s->textures.size(), 0u);
  s->frames_leaving = k_joint_slots + 1;
  s->leaving.reserve(wanted);
  s->mesh_q.assign(mesh_count, k_never);
  s->mesh_q_now.assign(mesh_count, k_never);
  s->order.reserve(s->textures.size());
  s->queue.reserve(s->textures.size() * 3u);
  s->done.reserve(s->textures.size() * 3u);
  s->taken.reserve(s->textures.size() * 3u);
  s->in_hand_bound = std::max<u64>(u64{64} << 20, u64(bound_promote_kib.get()) * 1024 * 4);
  pending_images_.reserve(pending_images_.size() + 16);
  texture_bound_.reset(s.release());
  return true;
}

u32 GpuScene::texture_bound_first_level(u64 key, const texture::TextureData& t) const noexcept {
  if (texture_bound_ == nullptr) return ~0u;
  const u32* at = find_held(*texture_bound_, key);
  if (at == nullptr || texture_bound_->free_pairs.size() <= k_spare_pairs) return ~0u;
  const u32 floor =
      floor_top_of(t.width, t.height, t.levels.size(), static_cast<u32>(bound_floor.get()));
  // A texture at its floor already has nothing to leave out. Without the bound (a ceiling alone)
  // the load uploads it whole: nothing is released without pressure.
  if (floor == 0) return ~0u;
  return texture_bound_->bound ? floor : 0u;
}

u32 GpuScene::texture_bound_place(u64 key, const texture::TextureData& t, u32 first,
                                  const gfx::ImageResource& image, gfx::ImageViewHandle view) {
  TextureBoundState& s = *texture_bound_;
  TextureBoundState::Texture& tex = s.textures[*find_held(s, key)];
  tex.width = t.width;
  tex.height = t.height;
  tex.levels = t.levels.size();
  tex.format = image.format;
  tex.floor_top =
      floor_top_of(t.width, t.height, t.levels.size(), static_cast<u32>(bound_floor.get()));
  tex.resident_top = first;
  tex.wanted_top = tex.now_top = first;
  tex.image = image;
  tex.view = view;
  tex.pair = s.free_pairs.back();
  s.free_pairs.pop_back();
  s.pair_texture[tex.pair] = *find_held(s, key);
  s.pair_top[tex.pair] = first;
  s.pair_since[tex.pair] = 0;
  tex.record = load_texture_records_.size();  // the caller's next record
  tex.placed = true;
  const u32 slot = s.first_slot + 2 * tex.pair;
  bindless_.update_sampled_image(slot, view, gfx::ImageLayout::ShaderReadOnly);
  ++s.use.managed;
  s.use.whole_bytes += t.data.size() + t.normal_variance.size();
  s.use.resident_bytes += image.bytes;
  s.use.peak_bytes = std::max(s.use.peak_bytes, s.use.resident_bytes);
  sync_chunk_use(s);
  return slot;
}

u32 GpuScene::texture_bound_place_variance(u64 key, const gfx::ImageResource& image,
                                           gfx::ImageViewHandle view) {
  TextureBoundState& s = *texture_bound_;
  TextureBoundState::Texture& tex = s.textures[*find_held(s, key)];
  tex.variance = true;
  tex.variance_image = image;
  tex.variance_view = view;
  const u32 slot = s.first_slot + 2 * tex.pair + 1;
  bindless_.update_sampled_image(slot, view, gfx::ImageLayout::ShaderReadOnly);
  s.use.resident_bytes += image.bytes;
  s.use.peak_bytes = std::max(s.use.peak_bytes, s.use.resident_bytes);
  sync_chunk_use(s);
  return slot;
}

void GpuScene::texture_bound_materials(const Vector<gfx::ResolveMaterial>& table,
                                       const Vector<u8>& is_material) {
  if (texture_bound_ == nullptr) return;
  TextureBoundState& s = *texture_bound_;
  Vector<u32> pair_texture(s.pairs, ~0u);
  for (u32 t = 0; t < s.textures.size(); ++t)
    if (s.textures[t].placed) pair_texture[s.textures[t].pair] = t;
  for (u32 e = 0; e < table.size(); ++e) {
    if (e < is_material.size() && is_material[e] == 0) continue;
    const gfx::ResolveMaterial& record = table[e];
    const u32 named_slots[geometry::k_material_slots] = {
        record.albedo_texture, record.metallic_roughness_texture, record.normal_texture,
        record.occlusion_texture, record.emissive_texture};
    u32 copy = ~0u;
    for (const u32 slot : named_slots) {
      if (slot < s.first_slot || slot >= s.first_slot + 2 * s.pairs) continue;
      if (((slot - s.first_slot) & 1u) != 0) continue;  // a variance slot is named by its normal's
      const u32 t = pair_texture[(slot - s.first_slot) / 2];
      if (t == ~0u) continue;
      if (copy == ~0u) {
        copy = s.materials.size();
        s.materials.push_back(TextureBoundState::MaterialCopy{e, record});
      }
      Vector<u32>& named = s.textures[t].materials;
      if (std::find(named.begin(), named.end(), copy) == named.end()) named.push_back(copy);
    }
  }
  // Nothing placed (every user's mesh drew decoded): the run stays, holding nothing.
  ENGINE_LOG_INFO(log_renderer, "texture bound", log::field("textures", s.use.managed),
                  log::field("pairs", s.pairs), log::field("materials", s.materials.size()),
                  log::field("whole_bytes", s.use.whole_bytes),
                  log::field("floor_bytes", s.use.resident_bytes));
}

void GpuScene::texture_bound_whole() noexcept {
  if (texture_bound_ == nullptr) return;
  texture_bound_->whole = true;
  texture_bound_->settled = false;  // the next frame brings every level in before it draws
}

// ---- stage 2: the residency's feedback --------------------------------------------------------

bool GpuScene::texture_residency_run(u32& first_slot, u32& count) const noexcept {
  if (texture_bound_ == nullptr || texture_bound_->ceiling == 0 || texture_bound_->use.managed == 0)
    return false;
  first_slot = texture_bound_->first_slot;
  count = 2 * texture_bound_->pairs;
  return true;
}

u64 GpuScene::texture_residency_frame() const noexcept {
  return texture_bound_ != nullptr ? texture_bound_->frames : 0u;
}

void GpuScene::texture_residency_fold(const u32* codes, u32 count, u64 frame) noexcept {
  if (texture_bound_ == nullptr) return;
  TextureBoundState& s = *texture_bound_;
  // texture_feedback.slang's code: (lambda + 32) * 256 of the resident image, never 0xffffffff
  // where a pixel asked; the chain's level is its floor plus the pair's top.
  for (u32 p = 0; p < s.pairs && 2 * p < count; ++p) {
    const u32 code = codes[2 * p];
    const u32 t = s.pair_texture[p];
    if (code == 0xffffffffu || t == ~0u || s.pair_since[p] > frame) continue;
    TextureBoundState::Texture& tex = s.textures[t];
    const f64 lambda = static_cast<f64>(code) / 256.0 - 32.0;
    const i64 level =
        std::max<i64>(0, static_cast<i64>(std::floor(lambda)) + static_cast<i64>(s.pair_top[p]));
    const u32 asked = static_cast<u32>(std::min<i64>(level, i64{tex.levels} - 1));
    tex.asked_level = std::min(tex.asked_level, asked);
    for (u32 l = asked; l < 16; ++l)
      tex.asked_at[l] = s.now;
  }
}

void GpuScene::texture_residency_feedback_ms(f64 ms) noexcept {
  if (texture_bound_ == nullptr) return;
  TextureBoundUse& use = texture_bound_->use;
  ++use.feedback_frames;
  use.feedback_ms_total += ms;
  use.feedback_ms_max = std::max(use.feedback_ms_max, ms);
}

void GpuScene::destroy_texture_bound() noexcept {
  if (texture_bound_ == nullptr) return;
  TextureBoundState& s = *texture_bound_;
  sync_chunk_use(s);
  if (s.use.managed != 0) {  // what the run held and cost, once, for a measurement to read
    ENGINE_LOG_INFO(
        log_renderer, "texture bound use", log::field("textures", s.use.managed),
        log::field("whole_bytes", s.use.whole_bytes),
        log::field("resident_bytes", s.use.resident_bytes),
        log::field("peak_bytes", s.use.peak_bytes), log::field("promotions", s.use.promotions),
        log::field("promoted_bytes", s.use.promoted_bytes), log::field("settles", s.use.settles),
        log::field("late", s.use.late), log::field("evaluations", s.use.evaluations),
        log::field("worst_admit_ms", s.use.worst_admit_ms),
        log::field("worst_settle_ms", s.use.worst_settle_ms),
        log::field("last_evaluate_ms", s.use.last_evaluate_ms), log::field("ceiling", s.ceiling),
        log::field("demotions", s.use.demotions),
        log::field("released_bytes", s.use.released_bytes),
        log::field("pressure_frames", s.use.pressure_frames),
        log::field("refused_in_hold", s.use.refused_in_hold),
        log::field("asked_late", s.use.asked_late),
        log::field("worst_demote_ms", s.use.worst_demote_ms), log::field("moves", s.use.moves),
        log::field("moved_bytes", s.use.moved_bytes), log::field("chunks", s.use.chunks),
        log::field("chunk_bytes", s.use.chunk_bytes),
        log::field("chunk_used_bytes", s.use.chunk_used_bytes),
        log::field("chunks_made", s.use.chunks_made),
        log::field("chunks_freed", s.use.chunks_freed),
        log::field("most_chunk_bytes", s.use.most_chunk_bytes), log::field("budget", s.budget));
  }
  if (s.worker.joinable()) {
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      s.stop = true;
    }
    s.wake.notify_all();
    s.worker.join();
  }
  if (device_ != nullptr) {
    const auto destroy = [&](gfx::ImageResource& image, gfx::ImageViewHandle& view) {
      if (view.valid()) gfx::destroy_image_view(*device_, view);
      if (image.image.valid()) s.chunks.destroy_image(image);  // W167: every held image is one
      view = {};
      image = {};
    };
    for (TextureBoundState::Texture& tex : s.textures) {
      destroy(tex.image, tex.view);
      destroy(tex.variance_image, tex.variance_view);
    }
    for (TextureBoundState::Leaving& gone : s.leaving) {
      destroy(gone.image, gone.view);
      destroy(gone.variance_image, gone.variance_view);
    }
    if (s.placeholder_view.valid()) gfx::destroy_image_view(*device_, s.placeholder_view);
    if (s.placeholder.image.valid()) gfx::destroy_image(*device_, s.placeholder);
    s.chunks.destroy();
  }
  texture_bound_.reset();
}

// ---- the frame's half ---------------------------------------------------------------------------

namespace {

// Log2 of the least footprint a pixel can have on an instance's surface over its scale: the term
// the bound adds to a mesh's log2 texels a metre to get log2 texels a pixel.
struct BoundView {
  WorldPos eye{};
  f64 f = 1.0;
  f64 c2 = 1.0;
  f64 near = 0.01;
};

}  // namespace

void GpuScene::texture_bound_frame(WorldPos eye, f32 fov_y, f32 znear, u32 width, u32 height,
                                   bool cut, f64 dt_s) {
  if (texture_bound_ == nullptr) return;
  TextureBoundState& s = *texture_bound_;
  ++s.frames;
  s.now += dt_s > 0.0 ? dt_s : 1.0 / 60.0;
  // The images let go `frames_leaving` recorded frames ago: no frame that could sample them is
  // left.
  for (u32 i = 0; i < s.leaving.size();) {
    TextureBoundState::Leaving& gone = s.leaving[i];
    if (--gone.frames != 0) {
      ++i;
      continue;
    }
    const u32 slot = s.first_slot + 2 * gone.pair;
    bindless_.update_sampled_image(slot, s.placeholder_view, gfx::ImageLayout::ShaderReadOnly);
    bindless_.update_sampled_image(slot + 1, s.placeholder_view, gfx::ImageLayout::ShaderReadOnly);
    gfx::destroy_image_view(*device_, gone.view);
    s.chunks.destroy_image(gone.image);  // W167: its chunk goes back when this was its last
    if (gone.variance_image.image.valid()) {
      gfx::destroy_image_view(*device_, gone.variance_view);
      s.chunks.destroy_image(gone.variance_image);
    }
    s.free_pairs.push_back(gone.pair);
    s.pair_texture[gone.pair] = ~0u;
    s.leaving[i] = s.leaving.back();
    s.leaving.pop_back();
  }
  if (s.use.managed == 0) return;

  // The view: pixels a radian at the centre, and cos^2 of the angle to the frame's corner.
  BoundView view;
  view.eye = eye;
  const f64 half_h = 0.5 * static_cast<f64>(height);
  const f64 half_w = 0.5 * static_cast<f64>(width);
  view.f = half_h / std::tan(0.5 * static_cast<f64>(fov_y));
  view.c2 = view.f * view.f / (view.f * view.f + half_w * half_w + half_h * half_h);
  view.near = std::min(0.01, static_cast<f64>(znear));
  // A step longer than `renderer.textures.jump_m` is a teleport the host did not call a cut (a
  // camera path's repeat, a jump to a marker): taken as a velocity it would stretch the lead over
  // the whole scene and bring every level in, so it is taken as the cut it is.
  const f64 moved = s.have_eye ? length(eye - s.last_eye) : 0.0;
  const bool jump = moved > bound_jump_m.get();
  const bool settle = cut || !s.settled || jump;
  const f64 step = jump ? 0.0 : moved;
  s.last_eye = eye;
  s.have_eye = true;
  const f64 lead = bound_lead_m.get() + step * static_cast<f64>(bound_lead_frames.get());
  // W167: a device budget named and no ceiling: the textures may hold the budget less what
  // everything else on the device holds (the driver's usage for the process, less the held images'
  // chunks and the other textures' images), worked out at the first frame, at a cut and every 10 s
  // of the frames' clock. A driver query: never more often than that.
  if (s.budget != 0 && residency_ceiling_mib.get() <= 0.0 &&
      (settle || s.now - s.budget_at >= 10.0)) {
    s.budget_at = s.now;
    gfx::MemoryBudget memory;
    if (device_->memory_budget(memory) && memory.valid) {
      const u64 held = s.chunks.use().chunk_bytes;
      const u64 others = texture_bytes_ - std::min(texture_bytes_, s.use.resident_bytes);
      const u64 rest = memory.used_bytes - std::min(memory.used_bytes, held + others);
      s.ceiling = s.budget > rest ? std::max<u64>(s.budget - rest, 1u) : 1u;
      s.use.ceiling = s.ceiling;
    }
  }
  // A ceiling without the bound (stage 2 alone) evaluates nothing: its levels come from the load
  // whole and leave only by the trim.
  const bool evaluate =
      s.bound && (settle || view.f != s.eval_f || view.c2 != s.eval_c2 || lead > s.eval_lead ||
                  length(eye - s.eval_eye) > 0.5 * s.eval_lead || s.frames - s.eval_frame >= 60);
  bool late_now = false;
  if (evaluate) {
    const i64 started = time::monotonic_ns();
    s.eval_eye = eye;
    s.eval_f = view.f;
    s.eval_c2 = view.c2;
    s.eval_lead = lead;
    s.eval_frame = s.frames;
    ++s.use.evaluations;
    std::fill(s.mesh_q.begin(), s.mesh_q.end(), k_never);
    std::fill(s.mesh_q_now.begin(), s.mesh_q_now.end(), k_never);
    const f64 log2_c2_over_f = std::log2(view.c2 / view.f);
    const u32 count = std::min<u32>(instance_count_, instance_table_.size());
    for (u32 i = 0; i < count; ++i) {
      const gfx::InstanceDesc& instance = instance_table_[i];
      const u32 m = instance.mesh;
      if (m >= s.mesh_users.size() || s.mesh_users[m].empty()) continue;
      const Vec4 sphere = s.mesh_sphere[m];
      const auto w = [](f32 v) { return static_cast<f64>(v); };
      f64 scale = 0.0;  // the largest axis scale: the length of the 3x4's largest column
      for (u32 c = 0; c < 3; ++c) {
        const Vec4* r = instance.rows;
        const f64 x = w(c == 0 ? r[0].x : c == 1 ? r[0].y : r[0].z);
        const f64 y = w(c == 0 ? r[1].x : c == 1 ? r[1].y : r[1].z);
        const f64 z = w(c == 0 ? r[2].x : c == 1 ? r[2].y : r[2].z);
        scale = std::max(scale, std::sqrt(x * x + y * y + z * z));
      }
      if (!(scale > 0.0)) continue;
      const auto row = [&](const Vec4& r, i32 cell) {
        return static_cast<f64>(cell) * k_world_cell_m + w(r.w) + w(r.x) * w(sphere.x) +
               w(r.y) * w(sphere.y) + w(r.z) * w(sphere.z);
      };
      const WorldPos centre{row(instance.rows[0], instance.cell.x),
                            row(instance.rows[1], instance.cell.y),
                            row(instance.rows[2], instance.cell.z)};
      const f64 gap = length(centre - eye) - static_cast<f64>(sphere.w) * scale;
      const f64 r_lead = std::max(gap - lead, view.near);
      const f64 r_now = std::max(gap, view.near);
      const f64 log2_scale = std::log2(scale);
      s.mesh_q[m] = std::min(s.mesh_q[m], std::log2(r_lead) + log2_c2_over_f - log2_scale);
      s.mesh_q_now[m] = std::min(s.mesh_q_now[m], std::log2(r_now) + log2_c2_over_f - log2_scale);
    }
    for (TextureBoundState::Texture& tex : s.textures) {
      tex.wanted_top = s.whole ? 0u : tex.floor_top;
      tex.now_top = tex.wanted_top;
    }
    const f64 margin = bound_margin.get();
    for (u32 m = 0; m < s.mesh_users.size(); ++m) {
      if (s.mesh_users[m].empty() || s.mesh_q[m] >= k_never) continue;
      for (const TextureBoundState::User& user : s.mesh_users[m]) {
        TextureBoundState::Texture& tex = s.textures[user.texture];
        if (!tex.placed) continue;
        const f64 log2_side =
            std::log2(static_cast<f64>(std::max(1u, std::min(tex.width, tex.height))));
        const auto level = [&](f64 q) {
          const f64 lambda = user.log2_texels_per_m + log2_side + q - margin;
          return lambda <= 0.0 ? 0u : static_cast<u32>(std::min(std::floor(lambda), 31.0));
        };
        tex.wanted_top = std::min(tex.wanted_top, level(s.mesh_q[m]));
        tex.now_top = std::min(tex.now_top, level(s.mesh_q_now[m]));
      }
    }
    for (TextureBoundState::Texture& tex : s.textures) {
      if (!tex.placed) continue;
      if (tex.now_top < tex.resident_top && !settle) {
        late_now = true;
        if (!tex.late) ++s.use.late;
        tex.late = true;
      } else if (tex.now_top >= tex.resident_top) {
        tex.late = false;
      }
    }
    s.use.last_evaluate_ms = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
  }

  // The reads that arrived since the last frame.
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    for (TextureBoundState::Read& read : s.done)
      s.taken.push_back(std::move(read));
    s.done.clear();
  }
  const auto admit = [&](u32 t, texture::TextureData& tail, u32 first, bool move) {
    TextureBoundState::Texture& tex = s.textures[t];
    if (s.free_pairs.empty()) return false;
    const u32 levels = tail.levels.size();
    gfx::ImageResource image;
    gfx::ImageViewHandle view_handle;
    gfx::ImageResource variance;
    gfx::ImageViewHandle variance_view;
    std::string error;
    const auto undo = [&] {
      if (view_handle.valid()) gfx::destroy_image_view(*device_, view_handle);
      if (image.image.valid()) s.chunks.destroy_image(image);
      if (variance_view.valid()) gfx::destroy_image_view(*device_, variance_view);
      if (variance.image.valid()) s.chunks.destroy_image(variance);
    };
    const bool with_variance = tex.variance && !tail.normal_variance.empty();
    if (levels == 0 || levels > gfx::k_max_image_levels ||
        !s.chunks.create_sampled_image_levels(tail.width, tail.height, tex.format, levels, image,
                                              &error) ||
        !gfx::create_image_view(*device_, image, view_handle, &error) ||
        (with_variance &&
         (!s.chunks.create_sampled_image_levels(tail.width, tail.height, gfx::Format::Bc4Unorm,
                                                levels, variance, &error) ||
          !gfx::create_image_view(*device_, variance, variance_view, &error)))) {
      undo();
      ENGINE_LOG_WARN(log_renderer, "texture bound: no image for a promotion",
                      log::field("key", tex.key), log::field("error", error));
      return false;
    }
    u64 staged = 0;
    for (u32 l = 0; l < levels; ++l) {
      staged += aligned16(tail.level_bytes(l).size());
      if (with_variance) staged += aligned16(tail.normal_variance_level_bytes(l).size());
    }
    const u64 records_at = staged;
    staged += u64{tex.materials.size()} * sizeof(gfx::ResolveMaterial);
    gfx::BufferResource staging;
    u64 at = 0;
    if (!stage_bytes(staged, staging, at, &error)) {
      undo();
      return false;
    }
    u8* base = static_cast<u8*>(staging.mapped);
    u64 offset = at;
    TerrainUpdate::ImageUpload upload;
    upload.image = image;
    upload.staging = staging.buffer;
    for (u32 l = 0; l < levels; ++l) {
      const std::span<const u8> bytes = tail.level_bytes(l);
      upload.offsets[l] = offset;
      std::memcpy(base + offset, bytes.data(), bytes.size());
      offset += aligned16(bytes.size());
    }
    pending_images_.push_back(upload);
    if (with_variance) {
      upload.image = variance;
      for (u32 l = 0; l < levels; ++l) {
        const std::span<const u8> bytes = tail.normal_variance_level_bytes(l);
        upload.offsets[l] = offset;
        std::memcpy(base + offset, bytes.data(), bytes.size());
        offset += aligned16(bytes.size());
      }
      pending_images_.push_back(upload);
    }
    // The new pair, written before any material names it; the records that name the old one
    // rewritten in the same pass that copies the levels.
    const u32 pair = s.free_pairs.back();
    s.free_pairs.pop_back();
    s.pair_texture[pair] = t;
    s.pair_top[pair] = first;
    s.pair_since[pair] = s.frames;
    const u32 old_slot = s.first_slot + 2 * tex.pair;
    const u32 slot = s.first_slot + 2 * pair;
    bindless_.update_sampled_image(slot, view_handle, gfx::ImageLayout::ShaderReadOnly);
    if (with_variance)
      bindless_.update_sampled_image(slot + 1, variance_view, gfx::ImageLayout::ShaderReadOnly);
    u64 record_at = at + records_at;
    for (const u32 c : tex.materials) {
      gfx::ResolveMaterial& record = s.materials[c].record;
      for (u32* field :
           {&record.albedo_texture, &record.metallic_roughness_texture, &record.normal_texture,
            &record.occlusion_texture, &record.emissive_texture}) {
        if (*field == old_slot) *field = slot;
      }
      std::memcpy(base + record_at, &record, sizeof(record));
      // **One copy a record** (R186 P2): a material with two textures promoted in one frame was
      // copied twice, each copy carrying the earlier change, and the copies of one pass are not
      // ordered against each other, so the last was not sure to win. The later bytes take the
      // earlier copy's place instead.
      const TerrainUpdate::GeometryCopy copy{
          staging.buffer, materials.buffer,
          gfx::BufferCopy{record_at, u64{s.materials[c].index} * sizeof(gfx::ResolveMaterial),
                          sizeof(gfx::ResolveMaterial)}};
      bool joined = false;
      for (TerrainUpdate::GeometryCopy& earlier : pending_geometry_) {
        if (earlier.dst != copy.dst) continue;
        if (earlier.region.dst_offset == copy.region.dst_offset &&
            earlier.region.size == copy.region.size) {
          earlier = copy;
          joined = true;
        } else if (earlier.region.dst_offset <= copy.region.dst_offset &&
                   copy.region.dst_offset + copy.region.size <=
                       earlier.region.dst_offset + earlier.region.size) {
          // W167: a tile's mesh admitted this frame copies its whole block of records, which
          // covers this one; its staged bytes take the record instead, since a second copy into the
          // same bytes in one pass is not ordered against the first.
          if (u8* bytes = staged_bytes_of(earlier.src)) {
            std::memcpy(bytes + earlier.region.src_offset +
                            (copy.region.dst_offset - earlier.region.dst_offset),
                        &record, sizeof(record));
            joined = true;
          }
        }
      }
      if (!joined) pending_geometry_.push_back(copy);
      record_at += sizeof(gfx::ResolveMaterial);
    }
    pending_geometry_bytes_ += staged;
    const u64 old_bytes = tex.image.bytes + tex.variance_image.bytes;
    const u64 new_bytes = image.bytes + variance.bytes;
    s.leaving.push_back(TextureBoundState::Leaving{tex.image, tex.view, tex.variance_image,
                                                   tex.variance_view, tex.pair, s.frames_leaving});
    tex.image = image;
    tex.view = view_handle;
    tex.variance_image = variance;
    tex.variance_view = variance_view;
    const bool demote = first > tex.resident_top;
    tex.pair = pair;
    tex.resident_top = first;
    if (tex.asked_top != ~0u && tex.asked_top >= first) tex.asked_top = ~0u;
    texture_bytes_ = texture_bytes_ - old_bytes + new_bytes;
    if (tex.record < load_texture_records_.size()) {
      load_texture_records_[tex.record].slot = slot;
      load_texture_records_[tex.record].bytes = new_bytes;
      load_texture_records_[tex.record].top = first;
    }
    s.use.resident_bytes = s.use.resident_bytes - old_bytes + new_bytes;
    u64 leaving_bytes = 0;
    for (const TextureBoundState::Leaving& gone : s.leaving)
      leaving_bytes += gone.image.bytes + gone.variance_image.bytes;
    s.use.peak_bytes = std::max(s.use.peak_bytes, s.use.resident_bytes + leaving_bytes);
    if (move) {  // W167: the same levels, out of a draining chunk
      ++s.use.moves;
      s.use.moved_bytes += new_bytes;
    } else if (demote) {  // a trim's (stage 2)
      ++s.use.demotions;
      s.use.released_bytes += old_bytes > new_bytes ? old_bytes - new_bytes : 0u;
    } else {
      ++s.use.promotions;
      s.use.promoted_bytes += new_bytes;
      // Brought in now: as young as an ask, so the hold keeps it as long as one.
      for (u32 l = first; l < 16; ++l)
        tex.asked_at[l] = std::max(tex.asked_at[l], s.now);
    }
    return true;
  };

  const bool now = s.bound && (settle || (late_now && bound_wait.get()));
  if (now) {
    // Every wanted level before the frame draws: a cut, the first frame, or a test's wait.
    const i64 started = time::monotonic_ns();
    for (u32 t = 0; t < s.textures.size(); ++t) {
      TextureBoundState::Texture& tex = s.textures[t];
      const u32 want = settle ? tex.wanted_top : tex.now_top;
      if (!tex.placed || want >= tex.resident_top) continue;
      texture::TextureData tail;
      std::string error;
      if (!texture::read_texture_file_levels(tex.path, want, tail, &error) ||
          tail.levels.size() != tex.levels - want) {
        ENGINE_LOG_WARN(log_renderer, "texture bound: a promotion's levels did not read",
                        log::field("path", tex.path), log::field("error", error));
        continue;
      }
      admit(t, tail, want, false);
    }
    s.settled = true;
    if (settle) ++s.use.settles;
    s.use.worst_settle_ms =
        std::max(s.use.worst_settle_ms, static_cast<f64>(time::monotonic_ns() - started) / 1.0e6);
  }
  // Off the frame: ask the reader for each texture that wants more than it has or has asked for,
  // and admit what it brought within the frame's allowance, finest need first. Without a ceiling
  // what a texture wants is the bound's; under one (stage 2) it is what the feedback asked —
  // a level ahead where its asks are getting finer, the bound's lead where the pool has room for
  // it, never finer than the bound.
  bool asked = false;
  for (u32 t = 0; t < s.textures.size(); ++t) {
    TextureBoundState::Texture& tex = s.textures[t];
    if (!tex.placed) continue;
    u32 target = tex.wanted_top;
    if (s.ceiling != 0) {
      const u32 asked_level = tex.asked_level;
      target = tex.resident_top;
      if (asked_level < tex.resident_top) {
        ++s.use.asked_late;
        const bool nearing = tex.last_asked != ~0u && asked_level < tex.last_asked;
        target = nearing && asked_level > 0 ? asked_level - 1 : asked_level;
      }
      if (asked_level != ~0u) tex.last_asked = asked_level;
      tex.asked_level = ~0u;
      if (s.bound && tex.wanted_top < target &&
          texture_bytes_ + chain_bytes_from(tex, tex.wanted_top) <=
              s.ceiling + chain_bytes_from(tex, tex.resident_top)) {
        target = tex.wanted_top;
      }
      if (s.bound) target = std::max(target, tex.wanted_top);
    }
    if (target >= tex.resident_top || tex.demote_top != ~0u) continue;
    if (tex.asked_top != ~0u && tex.asked_top <= target) continue;
    tex.asked_top = target;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.queue.push_back(TextureBoundState::Read{t, target, &tex.path, {}, false, false, 0, false});
    asked = true;
  }
  // **The trim** (stage 2; ADR-0065 decision 3 and its 2026-11-17 settlement): only over the
  // ceiling, only levels no frame asked for and nothing brought in within the hold, least recently
  // asked first, only as far as the ceiling needs, never below the floor. A texture's levels are
  // asked finest-oldest (an ask of level l is an ask of every coarser one), so the candidates in
  // order of their last ask take each texture's levels from its top down.
  if (s.ceiling != 0 && texture_bytes_ > s.ceiling) {
    ++s.use.pressure_frames;
    const u64 over = texture_bytes_ - s.ceiling;
    if (over > s.releasing) {
      const u64 need = over - s.releasing;
      s.candidates.clear();
      for (u32 t = 0; t < s.textures.size(); ++t) {
        const TextureBoundState::Texture& tex = s.textures[t];
        s.plan_top[t] = tex.resident_top;
        if (!tex.placed || tex.asked_top != ~0u || tex.demote_top != ~0u) continue;
        for (u32 l = tex.resident_top; l < tex.floor_top && l < 16; ++l) {
          if (s.now - tex.asked_at[l] < s.hold_s) break;
          s.candidates.push_back(TextureBoundState::Candidate{tex.asked_at[l], t, l});
        }
      }
      std::sort(s.candidates.begin(), s.candidates.end(),
                [](const TextureBoundState::Candidate& a, const TextureBoundState::Candidate& b) {
                  if (a.asked_at != b.asked_at) return a.asked_at < b.asked_at;
                  if (a.texture != b.texture) return a.texture < b.texture;
                  return a.level < b.level;
                });
      u64 freed = 0;
      for (const TextureBoundState::Candidate& c : s.candidates) {
        if (freed >= need) break;
        if (s.plan_top[c.texture] != c.level) continue;
        s.plan_top[c.texture] = c.level + 1;
        freed += level_bytes_of(s.textures[c.texture], c.level);
      }
      for (u32 t = 0; t < s.textures.size(); ++t) {
        TextureBoundState::Texture& tex = s.textures[t];
        if (s.plan_top[t] <= tex.resident_top) continue;
        const u64 frees =
            chain_bytes_from(tex, tex.resident_top) - chain_bytes_from(tex, s.plan_top[t]);
        tex.demote_top = s.plan_top[t];
        s.releasing += frees;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.queue.push_back(
            TextureBoundState::Read{t, s.plan_top[t], &tex.path, {}, false, true, frees, false});
        asked = true;
      }
    }
  }
  // **The bytes go back to the device** (W167, W47's stage 1; texture.md, "Stage 3: the bytes go
  // back to the device"). A trim's and a promotion's old images leave holes in the chunks the held
  // images live in, and the driver is charged for a chunk until its last image goes. Where the
  // holes in the chunks that hold anything pass one and a half chunks, the sparsest of them is
  // drained: it takes no new image, and every texture with an image in it is moved — its levels
  // from the same top read again by range and re-placed through the promotion's swap, inside the
  // frame's allowance — so the chunk empties and goes back. One chunk drains at a time.
  if (s.chunks.valid()) {
    if (s.draining != ~0u && !s.chunks.draining(s.draining)) s.draining = ~0u;  // it went back
    const gfx::ImageChunkUse chunks = s.chunks.use();
    const u64 chunk = s.chunks.chunk_size();
    const u64 spare = u64{chunks.empty} * chunk;
    const u64 holes = chunks.chunk_bytes - std::min(chunks.chunk_bytes, chunks.used_bytes + spare);
    if (s.draining == ~0u && holes >= chunk + chunk / 2) {
      u64 used = 0;
      const u32 sparsest = s.chunks.sparsest_chunk(&used);
      // Only where what it holds fits in the other chunks' holes: a move that made a new chunk
      // would give nothing back.
      if (sparsest != ~0u && used <= chunk && holes - std::min(holes, chunk - used) >= used) {
        s.chunks.set_draining(sparsest, true);
        s.draining = sparsest;
        ++s.use.drains;
      }
    }
    if (s.draining != ~0u) {
      for (u32 t = 0; t < s.textures.size(); ++t) {
        TextureBoundState::Texture& tex = s.textures[t];
        if (!tex.placed || tex.moving || tex.asked_top != ~0u || tex.demote_top != ~0u) continue;
        if (s.chunks.chunk_of(tex.image) != s.draining &&
            s.chunks.chunk_of(tex.variance_image) != s.draining)
          continue;
        tex.moving = true;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.queue.push_back(
            TextureBoundState::Read{t, tex.resident_top, &tex.path, {}, false, false, 0, true});
        asked = true;
      }
    }
  }
  if (asked) {
    if (!s.worker.joinable()) s.worker = std::thread(reader, &s);
    s.wake.notify_one();
  }
  if (!s.taken.empty()) {
    u64 consumed = 0;  // the bytes of reads admitted or dropped this frame, out of the hand
    const i64 started = time::monotonic_ns();
    const u64 allowance = static_cast<u64>(bound_promote_kib.get()) * 1024;
    u64 spent = 0;
    u32 kept = 0;
    bool demoted = false;
    for (u32 r = 0; r < s.taken.size(); ++r) {
      TextureBoundState::Read& read = s.taken[r];
      TextureBoundState::Texture& tex = s.textures[read.texture];
      // A demotion is dropped where a level it would release was asked (or brought in) within the
      // hold since it was planned: what the trim releases is never younger than the hold.
      const bool in_hold = read.demote && read.first > 0 && read.first <= 16 &&
                           s.now - tex.asked_at[read.first - 1] < s.hold_s;
      // A move (W167) is dropped where the texture was swapped since it was asked (its new image is
      // in another chunk already) or where its chunk is no longer draining.
      const bool still_draining =
          s.draining != ~0u && (s.chunks.chunk_of(tex.image) == s.draining ||
                                s.chunks.chunk_of(tex.variance_image) == s.draining);
      const bool stale =
          !read.ok || read.data.levels.size() != tex.levels - read.first ||
          (read.move     ? read.first != tex.resident_top || !still_draining
           : read.demote ? tex.demote_top != read.first || read.first <= tex.resident_top || in_hold
                         : read.first >= tex.resident_top);
      const auto settle_read = [&] {
        if (read.move) {
          tex.moving = false;
        } else if (read.demote) {
          if (tex.demote_top == read.first) tex.demote_top = ~0u;
          s.releasing -= std::min(s.releasing, read.frees);
        } else if (tex.asked_top == read.first) {
          tex.asked_top = ~0u;
        }
      };
      if (stale) {
        if (!read.ok) {
          ENGINE_LOG_WARN(log_renderer, "texture bound: a promotion's levels did not read",
                          log::field("path", *read.path));
        }
        if (in_hold) ++s.use.refused_in_hold;
        settle_read();
        consumed += read.data.data.size() + read.data.normal_variance.size();
        continue;
      }
      const u64 bytes = read.data.data.size() + read.data.normal_variance.size();
      if ((allowance != 0 && spent != 0 && spent + bytes > allowance) ||
          !admit(read.texture, read.data, read.first, read.move)) {
        if (kept != r) s.taken[kept] = std::move(read);
        ++kept;
        continue;
      }
      if (read.demote || read.move) {
        settle_read();
        demoted = demoted || read.demote;
      }
      spent += bytes;
      consumed += bytes;
    }
    if (consumed != 0) {
      {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.in_hand -= std::min(s.in_hand, consumed);
      }
      s.wake.notify_one();
    }
    if (demoted) {
      s.use.worst_demote_ms =
          std::max(s.use.worst_demote_ms, static_cast<f64>(time::monotonic_ns() - started) / 1.0e6);
    }
    s.taken.resize(kept);
    if (spent != 0) {
      s.use.worst_admit_ms =
          std::max(s.use.worst_admit_ms, static_cast<f64>(time::monotonic_ns() - started) / 1.0e6);
    }
  }
  sync_chunk_use(s);
}

}  // namespace engine::renderer
