#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/material_sampling.h>
#include <domain/gfx/bindless.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/image/decode.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/rt_capacity.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;
constexpr gfx::BufferUsage k_args = k_address | gfx::BufferUsage::Indirect |
                                    gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
// The visible list is read back by a capture, which resolves a visibility id into the instance
// and cluster it names; that is the only reason it is a transfer source.
constexpr gfx::BufferUsage k_readable = k_address | gfx::BufferUsage::TransferSrc;
// The streaming feedback arrays: cleared by the frame's reset pass and copied back to the host
// one frame slot later, so both directions are transfers.
constexpr gfx::BufferUsage k_transfer =
    gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;

// The budget, in vertices. `deform_pool_kib` is what the caller asked for (0 is the default); the
// clamp **down** to `whole_mesh_vertices` is what keeps a single character costing exactly what it
// cost before the suballocation existed, and makes overflow impossible on any scene small enough
// for the layout E25 built. Both ends are stated rather than hidden (ADR-0017): the summary
// reports the pool bytes and what the whole-mesh layout would have taken beside them.
// **A moving terrain's cut is the pool's largest tenant by far**: every cluster of the ground in
// view, a view and a cascade at a time, where a crowd's is a few thousand. An entry the budget has
// no room for draws its rest pose, and for a terrain level that is a jump — the one thing a moving
// terrain must never do — so a scene with terrain levels takes at least this budget
// (renderer.md, "The dunes in time-lapse", has what the erg's cut needs).
tunables::Int terrain_pool_mib{
    "renderer.terrain.pool_mib", 768, 1, 16'384,
    "The deformed-vertex pool's budget, MiB, at least, for a scene whose "
    "terrain moves (its cut is the pool's largest tenant)"};

// **A field is copied a budget a frame.** The erg's grid is 67 MB a field, and copied whole it cost
// a frame 7 ms of GPU on a quiet RTX 5090 and 13-22 ms beside other work (renderer.md, "The dunes
// in time-lapse", the costs) — a hitch every few frames at a game day a second. The next field is
// ready well before the surface needs it, so it goes over in pieces and is shown only once its
// last piece has been recorded: 8 MiB is under a millisecond.
tunables::Int terrain_upload_mib{
    "renderer.terrain.upload_mib", 8, 0, 4'096,
    "MiB of evaluated terrain fields copied onto the device a frame, at most (0: all at once; a "
    "field the frame must show is copied whole whatever the budget)"};

// **The terrain's staging ring** (renderer.md, "What a frame waits for"; GpuScene::
// terrain_chunk_fits): what a window's frames stage of a rebuild's tiles and slot records goes
// through it — `renderer.terrain.tile_upload_mib` a frame, for as many frames as are in flight —
// so it holds several frames of that with room to spare, and an offscreen frame that uploads a
// whole rebuild at once usually fits too (a far square's move is 4.3 MiB).
tunables::Int terrain_staging_mib{
    "renderer.terrain.staging_mib", 32, 1, 4'096,
    "MiB of the terrain's staging ring, which every chunk upload and slot record is staged through "
    "(a staging that does not fit makes a buffer of its own and is counted)"};

// A device-local buffer of `capacity` bytes whose first `bytes` are `data`: the scene's part of a
// stream the terrain rings' slots extend (renderer.md, "The rings in the scene"). The rest is
// whatever the allocator gave, which nothing reads before a chunk's copy has written it. Without
// rings the capacity is the data and this is `upload_buffer`.
bool upload_room(const gfx::Device& device, const void* data, u64 bytes, u64 capacity,
                 gfx::BufferUsage usage, gfx::BufferResource& out, std::string* error) {
  if (capacity <= bytes) return gfx::upload_buffer(device, data, bytes, usage, out, error);
  if (!gfx::create_buffer(
          device, capacity,
          usage | gfx::BufferUsage::TransferDst | gfx::BufferUsage::ShaderDeviceAddress, false, out,
          error)) {
    return false;
  }
  if (bytes == 0) return true;
  gfx::BufferResource staging;
  if (!gfx::create_buffer(device, bytes, gfx::BufferUsage::TransferSrc, true, staging, error))
    return false;
  std::memcpy(staging.mapped, data, static_cast<usize>(bytes));
  const bool ok = gfx::submit_immediate(
      device,
      [&](gfx::CommandList cb) {
        cb.copy_buffer(staging.buffer, out.buffer, gfx::BufferCopy{0, 0, bytes});
      },
      error);
  gfx::destroy_buffer(device, staging);
  return ok;
}

u32 pool_budget_vertices(const ResolvedSettings& resolved, u64 whole_mesh_vertices) noexcept {
  u64 kib = resolved.settings.deform_pool_kib > 0 ? u64{resolved.settings.deform_pool_kib}
                                                  : u64{k_default_deform_pool_kib};
  if (resolved.terrain_levels) kib = std::max<u64>(kib, u64(terrain_pool_mib.get()) * 1024);
  if (kib < k_min_deform_pool_kib) kib = k_min_deform_pool_kib;
  u64 vertices = kib * 1024 / (3 * sizeof(f32));
  if (vertices < 1) vertices = 1;
  if (vertices > whole_mesh_vertices) vertices = whole_mesh_vertices;
  return static_cast<u32>(vertices);
}

// The GPU format a built texture is sampled as: the format and colour space the build decided,
// from the same material slots the decode path guesses from, so the two agree.
gfx::Format texture_gpu_format(const texture::TextureData& t) noexcept {
  const bool srgb = t.color_space == texture::ColorSpace::srgb;
  switch (t.format) {
    case texture::TextureFormat::rgba8:
      return srgb ? gfx::Format::R8G8B8A8Srgb : gfx::Format::R8G8B8A8Unorm;
    case texture::TextureFormat::bc1:
      return srgb ? gfx::Format::Bc1RgbSrgb : gfx::Format::Bc1RgbUnorm;
    case texture::TextureFormat::bc3: return srgb ? gfx::Format::Bc3Srgb : gfx::Format::Bc3Unorm;
    case texture::TextureFormat::bc4: return gfx::Format::Bc4Unorm;
    case texture::TextureFormat::bc5: return gfx::Format::Bc5Unorm;
    case texture::TextureFormat::bc7: return srgb ? gfx::Format::Bc7Srgb : gfx::Format::Bc7Unorm;
  }
  return gfx::Format::Undefined;
}

// A glTF sampler as the device's (docs/subsystems/renderer.md, "Materials"). `mipmapped` is the
// built path: the whole chain, blended as the sampler asks, anisotropic where it minifies linearly.
// The decoded path reads level 0 alone and keeps what it always had but the wrap and the filters.
gfx::SamplerDesc sampler_desc(const geometry::TextureSampler& sampler, bool mipmapped) noexcept {
  auto address = [](geometry::TextureWrap wrap) {
    switch (wrap) {
      case geometry::TextureWrap::repeat: return gfx::SamplerAddressMode::Repeat;
      case geometry::TextureWrap::mirrored_repeat: return gfx::SamplerAddressMode::MirroredRepeat;
      case geometry::TextureWrap::clamp_to_edge: break;
    }
    return gfx::SamplerAddressMode::ClampToEdge;
  };
  auto filter = [](geometry::TextureFilter f) {
    return f == geometry::TextureFilter::nearest ? gfx::Filter::Nearest : gfx::Filter::Linear;
  };
  gfx::SamplerDesc desc;
  desc.address_u = address(sampler.wrap_s);
  desc.address_v = address(sampler.wrap_t);
  desc.mag = filter(sampler.mag);
  desc.min = filter(sampler.min);
  desc.mip = sampler.mip == geometry::TextureFilter::nearest ? gfx::SamplerMipmapMode::Nearest
                                                             : gfx::SamplerMipmapMode::Linear;
  desc.mipmapped = mipmapped;
  desc.max_anisotropy = 16.0f;
  return desc;
}

// **One upload per distinct image per scene** (docs/subsystems/renderer.md, "One upload per
// distinct image"). Every (mesh, image) a material samples is keyed by what the texture is made
// from — the `.tex` build key for a built one, which covers the image's bytes and its build
// options, or `hash_bytes` over the encoded bytes and the format it goes up as for a decoded one —
// and a key the scene already holds hands back that texture's bindless slot instead of uploading
// again. The key is the content and not the path: a kit's members each embed the same atlas and
// name it by no path at all, two files at different paths can hold the same bytes, and two images
// called `atlas.png` in two kits' folders hold different ones.
constexpr u32 k_form_built = ~u32{0};  // `SceneTexture::form` for a built texture

struct SceneTexture {
  u64 content = 0;  // the `.tex` build key, or hash_bytes over the image's encoded bytes
  u32 form = 0;     // k_form_built, or the gfx::Format a decoded image went up as
  u32 slot = 0;     // its bindless slot
  u64 bytes = 0;    // what the allocator gave it
  // What a built one is stored as: the material's BC5-normal flag reads it for a mesh that shares
  // the texture as well as for the mesh that uploaded it.
  texture::TextureFormat stored = texture::TextureFormat::rgba8;
};

// The scene's textures by content. With sharing off it finds nothing, and every mesh uploads its
// own as every build before 2026-09-25 did. A 64-bit collision between two different (content,
// form) pairs is not taken for a match: the entry keeps both halves and is compared whole, so the
// worst a collision can do is upload the second texture again.
class SceneTextures {
 public:
  explicit SceneTextures(bool share) noexcept : share_(share) {}
  const SceneTexture* find(u64 content, u32 form) const noexcept {
    if (!share_) return nullptr;
    const u32* index = index_.find_value(hash_combine(content, form));
    if (index == nullptr) return nullptr;
    const SceneTexture& found = textures_[*index];
    return found.content == content && found.form == form ? &found : nullptr;
  }
  void add(const SceneTexture& texture) {
    if (share_) index_.try_emplace(hash_combine(texture.content, texture.form), textures_.size());
    textures_.push_back(texture);
  }

 private:
  bool share_;
  Vector<SceneTexture> textures_;
  HashMap<u64, u32> index_;
};

// One sampled image of a mesh on the built path: its `.tex` build key, the texture read from the
// derived-data root — or nothing, when the scene already holds that key and the read would be for
// a texture nobody uploads — and the format it is stored as either way.
struct BuiltImage {
  u64 key = 0;
  texture::TextureData data;
  texture::TextureFormat stored = texture::TextureFormat::rgba8;
};

// A mesh's built textures (docs/subsystems/texture.md, "In the renderer"): one per image the
// resolve samples — base colour, metallic-roughness, normal, occlusion, emissive — found in the
// derived-data root through the container's records, and read with their identity checked against
// the bytes they were built from. An image the container carries is found by its recorded key; one
// named by path is hashed as the file is *now*, so a repainted image finds its new texture or none,
// never the stale one. **All or nothing**: false, with the reason, as soon as one is missing, so a
// mesh draws wholly from built textures or wholly as it always did, never a mix of the two.
// `needed` is how many images the resolve samples; zero leaves the mesh on the old path, which
// for a mesh with no textures is no path at all. A key `known` already holds is not read again:
// its identity was checked when the mesh that uploaded it read it, and the file is the same file.
bool find_built_textures(const SourceMesh& mesh, const SceneTextures& known,
                         Vector<BuiltImage>& out, u32& needed, std::string& why) {
  out.clear();
  needed = 0;
  const assets::MeshData& data = mesh.data;
  Vector<bool> sampled(data.images.size(), false);
  for (const assets::Material& material : data.materials) {
    for (const i32 image :
         {material.base_color_image, material.metallic_roughness_image, material.normal_image,
          material.occlusion_image, material.emissive_image}) {
      if (image >= 0 && static_cast<u32>(image) < sampled.size() &&
          !sampled[static_cast<u32>(image)]) {
        sampled[static_cast<u32>(image)] = true;
        ++needed;
      }
    }
  }
  if (needed == 0) return false;
  if (mesh.texture_ddc.empty()) {
    why = "the load reads no derived-data cache";
    return false;
  }
  if (mesh.textures.size() != data.images.size()) {
    why = "the container records no built textures (built before cache version 14, or with none)";
    return false;
  }
  out.resize(data.images.size());
  for (u32 i = 0; i < sampled.size(); ++i) {
    if (!sampled[i]) continue;
    const std::string image = "image " + std::to_string(i);
    const geometry::ClusterFileTexture& record = mesh.textures[i];
    texture::TextureBuildOptions options;
    if (record.options == 0 || !texture::unpack_texture_options(record.options, options)) {
      why = image + " has no texture record this build reads";
      return false;
    }
    u64 source_hash = record.source_hash;
    u64 key = record.key;
    if (key == 0) {
      const assets::ImageRef& ref = data.images[i];
      if (ref.uri.empty()) {
        why = image + " has neither bytes nor a path";
        return false;
      }
      const std::string file =
          mesh.image_dir.empty() ? ref.uri : io::join_path(mesh.image_dir, ref.uri);
      std::string bytes;
      if (io::read_file(file, bytes) != io::Status::Ok) {
        why = image + " ('" + file + "') cannot be read";
        return false;
      }
      source_hash = hash_bytes(bytes.data(), bytes.size());
      key = texture::texture_cache_key(source_hash, options);
    }
    out[i].key = key;
    if (const SceneTexture* held = known.find(key, k_form_built); held != nullptr) {
      out[i].stored = held->stored;
      continue;
    }
    const std::string path = texture::texture_cache_path(mesh.texture_ddc, key);
    if (!io::exists(path)) {
      why = image + " has not been built (engine-content build --cache builds it)";
      return false;
    }
    std::string error;
    if (!texture::read_texture_file(path, out[i].data, &error)) {
      why = image + ": " + error;
      return false;
    }
    if (out[i].data.source_hash != source_hash || out[i].data.build_key != key) {
      why = image + ": '" + path + "' was built from other bytes or options";
      return false;
    }
    out[i].stored = out[i].data.format;
  }
  return true;
}

}  // namespace

GpuScene::~GpuScene() { destroy(); }

bool GpuScene::create(const gfx::Device& device, const SceneData& data,
                      const ResolvedSettings& resolved, std::string* error,
                      TerrainLevelSet* rings) {
  destroy();
  device_ = &device;
  data_ = &data;
  rings_ = rings;
  ray_tracing_ = resolved.rt_chain;
  // A scene deforms when the settings say every instance does, or when any instance is skinned.
  // The two are independent: `--deform wave` on a scene with a skinned character deforms the
  // rigid instances procedurally and skins the skinned one, because a `DeformDesc` names one kind
  // per instance and the skinned instance's kind is skinning.
  skinned_ = data.skinned();
  skinned_instances_ = skinned_ ? data.skinned_instances : 0;
  max_joints_ = skinned_ ? data.max_joints : 0;
  deform_ = resolved.deform_pass;
  // `resolve_settings` has already refused streaming for a scene with no page table, but a caller
  // may hand the two apart; a scene that cannot be streamed is uploaded whole rather than half.
  streamed_ = resolved.stream && data.paged();
  page_count_ = streamed_ ? data.pages.pages.size() : 0u;
  view_count_ = resolved.view_count > 0 ? resolved.view_count : 1;
  // Before anything is sized: a cascade's run is part of the visible list, and so of the pool's
  // per-entry table.
  cascade_count_ = resolved.csm ? resolved.shadow_cascades : 0u;
  cluster_count_ = data.cluster_count();
  leaf_count_ = data.leaf_count();
  instance_count_ = data.instances.size();
  pair_count_ = data.pair_count;
  // A scene read whole lays its per-pair buffers out by exactly its pairs, as it always has; one
  // whose instances come and go starts there too and grows when a tail passes it.
  pair_stride_ = pair_count_;
  dynamic_ = data.dynamic;
  static_instances_ = instance_count_;
  static_pairs_ = pair_count_;
  resolved_ = resolved;
  triangles_per_cluster_ = geometry::ClusterLodOptions{}.max_triangles;
  instance_table_ = data.instances;
  scene_vertex_count_ = data.lod.mesh.vertices.size();
  scene_triangle_count_ = data.lod.mesh.triangles.size();
  vertex_capacity_ = scene_vertex_count_;
  triangle_capacity_ = scene_triangle_count_;
  // The terrain rings' slots, beside the scene's own meshes and instances: more clusters, pairs,
  // instances and arena room than the scene has, reserved now so that nothing about the scene's
  // layout ever has to move for a ring (renderer.md, "The rings in the scene").
  if (rings != nullptr && resolved.terrain_levels && rings->valid() && !dynamic_ && !streamed_) {
    if (!lay_out_rings(resolved, *rings, error)) {
      destroy();
      return false;
    }
    pair_stride_ = pair_count_;
  }
  // The visibility id is `pair << 8 | triangle` in 32 bits (gfx.md, "The tie rule"), so a scene
  // names at most 2^24 pairs; past that two pairs would share an id and the picture would be wrong
  // without a word, so refuse the scene instead.
  if (pair_count_ > k_max_pairs) {
    if (error != nullptr) {
      *error = "the scene has " + std::to_string(pair_count_) +
               " (instance, cluster) pairs; the visibility id names at most 16777216";
    }
    destroy();
    return false;
  }
  // Instances that come and go are rigid and untraced in v0: the ray tracing chain's top-level
  // records and bottom-level set, and the deformed-vertex pool's table, are laid out once per
  // instance, and nothing rewrites them for a tail (renderer.md, "Instances that come and go").
  if (dynamic_ && (ray_tracing_ || deform_)) {
    if (error != nullptr) {
      *error = ray_tracing_ ? "a scene whose instances come and go between frames has no ray "
                              "tracing chain yet: draw it with --shadows off or csm and a raster "
                              "path"
                            : "a scene whose instances come and go between frames draws rigid "
                              "instances only: no --deform, skinning or morph channels yet";
    }
    destroy();
    return false;
  }

  if (!bindless_.create(device, gfx::BindlessConfig{}, error) ||
      !create_streaming(resolved, error) || !upload_geometry(resolved, error) ||
      !upload_materials(resolved, error) || !create_working_set(resolved, error) ||
      !create_ray_tracing(resolved, error)) {
    destroy();
    return false;
  }
  // The sand's detail: the terrain's ground, asked for the wind the ripples lie across, and the
  // time its surface stands at — the scene's own until a moving terrain's motion moves it
  // (renderer.md, "The sand close up").
  if (data.terrain.enabled && data.terrain.has_detail && data.terrain_mesh < data.parts.size()) {
    ground_ = std::make_unique<TerrainSampler>(data.terrain);
    ground_time_s_ = data.terrain.time_s;
    ground_previous_s_ = data.terrain.time_s;
  }

  // The rings' chunks built round the camera before the scene was, each into a slot of its ring,
  // staged now that every buffer they are copied into exists, and drawn from the first frame.
  if (!ring_slots_.empty()) {
    for (RingSlots& ring : ring_slots_) {
      for (TerrainChunk& chunk : rings->chunks(ring.level)) {
        if (chunk.lod.mesh.clusters.empty() || ring.free_slots.empty()) continue;
        const u32 s = ring.free_slots.back();
        ring.free_slots.pop_back();
        if (!stage_chunk(ring, s, chunk, error)) {
          destroy();
          return false;
        }
        ring.slots[s].on = true;
      }
    }
  }
  // One MeshDesc per mesh: its own grid, its range of the shared cluster array, the one
  // quantized stream every mesh of the scene indexes, and where a deformed instance's positions
  // come from instead — the frame's pool, the deform table, and this mesh's cluster templates.
  // It is uploaded last because those three addresses have to exist first. A ring's slot is a
  // mesh too: its chunk's grid and clusters when it draws one, no clusters when it does not.
  Vector<gfx::MeshDesc>& mesh_descs = mesh_descs_;
  mesh_descs.clear();
  const auto described = [&](const geometry::ClusterMeshPart& part) {
    gfx::MeshDesc desc{};
    desc.quant = Vec4{part.quant_origin, part.quant_scale};
    desc.quantized = quantized.address;
    desc.first_cluster = part.first_cluster;
    desc.cluster_count = part.cluster_count;
    desc.deform_pool = deform_pool.address;
    desc.deform_slots = deform_slots.address;
    desc.templates = clas_templates.addresses.address;
    desc.skin = skin.address;
    return desc;
  };
  for (const geometry::ClusterMeshPart& part : data.parts)
    mesh_descs.push_back(described(part));
  for (RingSlots& ring : ring_slots_) {
    for (u32 s = 0; s < ring.slots.size(); ++s) {
      TerrainSlot& slot = ring.slots[s];
      // The template every state of the slot is written from: the scene's addresses, the slot's
      // clusters; `quant` and the count are the chunk's while it draws one.
      slot.desc = described(part_of(ring.first_mesh + s));
      gfx::MeshDesc desc = slot.desc;
      desc.quant = slot.quant;
      desc.cluster_count = slot.loaded && slot.on ? slot.clusters : 0u;
      mesh_descs.push_back(desc);
    }
  }
  // A hole of a dynamic scene's pair space names a **null mesh**, one past the scene's, with no
  // clusters: the cull pass finds the pair past every cluster that mesh has and rejects it before
  // it reads a cluster (cluster_cull.slang). A scene read whole has no holes and no such mesh.
  if (dynamic_) {
    null_mesh_ = mesh_descs.size();
    gfx::MeshDesc null_desc{};
    null_desc.first_cluster = cluster_count_;
    null_desc.cluster_count = 0;
    mesh_descs.push_back(null_desc);
  }
  if (!gfx::upload_buffer(device, mesh_descs.data(), mesh_descs.size() * sizeof(gfx::MeshDesc),
                          k_storage, meshes, error)) {
    destroy();
    return false;
  }
  if (!dynamic_) {
    if (!upload_pair_table(error)) {
      destroy();
      return false;
    }
    // The rings' first chunks, copied in now and waited for: this is `create`, where uploads are
    // one-shot, and the first frame draws them. Their records are in `meshes` already.
    if (!pending_geometry_.empty()) {
      const bool copied = gfx::submit_immediate(
          device,
          [&](gfx::CommandList cb) {
            for (const TerrainUpdate::GeometryCopy& copy : pending_geometry_)
              cb.copy_buffer(copy.src, copy.dst, copy.region);
          },
          error);
      for (gfx::BufferResource& staging : pending_staging_)
        gfx::destroy_buffer(device, staging);
      pending_staging_.clear();
      pending_geometry_.clear();
      pending_geometry_bytes_ = 0;
      if (!copied) {
        destroy();
        return false;
      }
    }
    // The first fill went through buffers of its own, once; every upload from here on is staged
    // through the ring.
    if (!ring_slots_.empty() && !create_staging_ring(error)) {
      destroy();
      return false;
    }
    rings_ = nullptr;
    return true;
  }
  // A dynamic scene's tables are sets, one per frame in flight (two until a renderer asks for
  // more), each kept at a capacity — the instance table in pages of 1,024 slots, the pair table as
  // long as the stride — and each written by the first frame that reads it: everything here is
  // pending, so the first frame flips to a set and uploads the load's instances and expands their
  // pairs, as it would a change. Nothing is written from the CPU at all.
  layout_.reset(static_instances_, static_pairs_);
  compact_pct_ = tile_compact_pct_tunable();
  tail_keys_.assign(instance_count_, InstanceKey{});
  instance_capacity_ =
      static_cast<u32>((u64{instance_count_} + u64{instance_count_} / 2 + 1024) / 1024 * 1024);
  sets_.resize(2);
  for (TableSet& set : sets_) {
    if (!create_table_set(set, error)) {
      destroy();
      return false;
    }
  }
  current_set_ = 0;
  instances = sets_[0].instances;
  pair_table = sets_[0].pairs;
  tables_changed_ = true;
  return true;
}

bool GpuScene::create_table_set(TableSet& set, std::string* error) {
  set.pending.clear();
  set.all = true;
  constexpr gfx::BufferUsage k_table =
      k_storage | gfx::BufferUsage::TransferDst | gfx::BufferUsage::ShaderDeviceAddress;
  return gfx::create_buffer(*device_, u64{instance_capacity_} * sizeof(gfx::InstanceDesc), k_table,
                            false, set.instances, error) &&
         (pair_stride_ == 0 || gfx::create_buffer(*device_, u64{pair_stride_} * 2u * sizeof(u32),
                                                  k_table, false, set.pairs, error));
}

void GpuScene::retire(gfx::BufferResource& buffer, Vector<gfx::BufferResource>* retired) noexcept {
  if (!buffer.buffer.valid()) return;
  if (retired != nullptr) {
    retired->push_back(buffer);
  } else {
    gfx::destroy_buffer(*device_, buffer);
  }
  buffer = gfx::BufferResource{};
}

gfx::InstanceDesc GpuScene::null_instance(u32 first_pair) const noexcept {
  gfx::InstanceDesc out{};
  out.mesh = null_mesh_;
  out.first_pair = first_pair;
  out.scale_max = 0.0f;
  out.flags = 0;
  return out;
}

// The {instance, cluster} of every pair, the inverse of `pair_of`: what the resolve reads a pixel's
// id through. It is the scene's, not the frame's, so it is written when the scene is — once, for a
// scene read whole — and it costs the resolve one load per pixel, the same one `visible[entry]` was
// when the id was the entry. Going through the view's pair-to-entry table instead was a second,
// dependent load on every pixel and cost the TITAN Xp's resolve 2-3%
// (docs/experiments/visible-order.md).
bool GpuScene::upload_pair_table(std::string* error) {
  gfx::destroy_buffer(*device_, pair_table);
  if (pair_count_ == 0) return true;
  // At most 2^24 pairs (checked by the caller), so twice that is a u32.
  Vector<u32> table(pair_count_ * 2u, 0u);
  for (u32 i = 0; i < instance_table_.size(); ++i) {
    const gfx::InstanceDesc& instance = instance_table_[i];
    const geometry::ClusterMeshPart& part = part_of(instance.mesh);
    for (u32 c = 0; c < part.cluster_count; ++c) {
      table[(instance.first_pair + c) * 2u] = i;
      table[(instance.first_pair + c) * 2u + 1u] = part.first_cluster + c;
    }
  }
  return gfx::upload_buffer(*device_, table.data(), u64{table.size()} * sizeof(u32), k_storage,
                            pair_table, error);
}

bool GpuScene::set_dynamic_instances(std::span<const SceneInstance> tail,
                                     std::span<const DynamicBlock> blocks, TileChange& change,
                                     Vector<gfx::BufferResource>* retired, std::string* error,
                                     bool compact) {
  change = TileChange{};
  if (device_ == nullptr || !dynamic_) {
    if (error != nullptr) *error = "the scene was not loaded to take instances between frames";
    return false;
  }
  // No blocks is the whole tail as one, which is what a caller that knows no tiles hands over.
  const DynamicBlock whole{0, 0, static_cast<u32>(tail.size())};
  if (blocks.empty() && !tail.empty()) blocks = std::span<const DynamicBlock>(&whole, 1);
  // Everything is checked before anything changes: the blocks cover the tail in order, every
  // instance names a mesh the scene has and is rigid, and — in the layout — the keys are distinct
  // and the pairs fit the visibility id.
  u64 next = 0;
  for (const DynamicBlock& block : blocks) {
    if (block.first != next) {
      if (error != nullptr) {
        *error = "the tail's blocks must cover it in order: block " + std::to_string(block.key) +
                 " starts at instance " + std::to_string(block.first) + ", not " +
                 std::to_string(next);
      }
      return false;
    }
    next += block.count;
  }
  if (next != tail.size()) {
    if (error != nullptr) {
      *error = "the tail's blocks cover " + std::to_string(next) + " of its " +
               std::to_string(tail.size()) + " instances";
    }
    return false;
  }
  requests_.resize(static_cast<u32>(blocks.size()));
  for (u32 b = 0; b < blocks.size(); ++b) {
    const DynamicBlock& block = blocks[b];
    TileLayout::Request& request = requests_[b];
    request = TileLayout::Request{};
    request.key = block.key;
    request.first = block.first;
    request.instances = block.count;
    // The same key holding exactly these instances is the same block, and costs nothing but this
    // comparison — which is the one part of a change that still reads the whole tail, because the
    // caller hands it over whole. A slot's key names only a mesh the scene has, so a block that
    // matches needs no other check and its pairs are what it held.
    const TileBlock* held = layout_.find(block.key);
    if (held != nullptr && held->instances == block.count) {
      bool same = true;
      for (u32 k = 0; k < block.count && same; ++k) {
        const SceneInstance& source = tail[block.first + k];
        same = source.joints == 0 && tail_keys_[held->inst_begin + k] == key_of(source);
      }
      if (same) {
        request.same = true;
        request.pairs = held->pairs;
        continue;
      }
    }
    u64 pairs = 0;
    for (u32 i = block.first; i < block.first + block.count; ++i) {
      const SceneInstance& source = tail[i];
      gfx::InstanceDesc probe;
      // The placement's own checks: a mesh the scene has not and a skinned instance are refused
      // with its sentence.
      if (source.mesh >= data_->parts.size() || source.joints != 0) {
        if (!make_instance(*data_, source, 0, probe, error)) return false;
      }
      pairs += data_->parts[source.mesh].cluster_count;
    }
    request.pairs = pairs > k_max_pairs ? k_max_pairs + 1u : static_cast<u32>(pairs);
  }
  if (!layout_.apply(std::span<const TileLayout::Request>(requests_.data(), requests_.size()),
                     compact_pct_, compact, k_max_pairs, writes_, change.layout, error)) {
    return false;
  }
  // Committed: the slots the layout wrote, then the buffers grown to hold them.
  if (!write_slots(tail, blocks, std::span<const SlotWrite>(writes_.data(), writes_.size()),
                   change.slots_written, error) ||
      !fit_tables(change, retired, error)) {
    return false;
  }
  if (change.layout.compacted) ++compactions_;
  if (change.slots_written > 0 || change.grew) tables_changed_ = true;
  return true;
}

bool GpuScene::write_slots(std::span<const SceneInstance> tail,
                           std::span<const DynamicBlock> blocks, std::span<const SlotWrite> writes,
                           u32& written, std::string* error) {
  written = 0;
  const u32 end = layout_.instance_end();
  instance_table_.resize(end);
  tail_keys_.resize(end);
  for (const SlotWrite& write : writes) {
    if (write.inst_count == 0) continue;
    u32 first_pair = write.pair_begin;
    const DynamicBlock* block = write.source < blocks.size() ? &blocks[write.source] : nullptr;
    for (u32 k = 0; k < write.inst_count; ++k) {
      const u32 slot = write.inst_begin + k;
      if (block != nullptr && k < write.live) {
        const SceneInstance& source = tail[block->first + k];
        gfx::InstanceDesc& desc = instance_table_[slot];
        if (!make_instance(*data_, source, first_pair, desc, error)) return false;
        desc.material_base =
            source.mesh < mesh_material_base_.size() ? mesh_material_base_[source.mesh] : 0u;
        tail_keys_[slot] = key_of(source);
        first_pair += data_->parts[source.mesh].cluster_count;
      } else {
        // Past the live instances: a null instance at the pairs' end, so a pair of the slack lands
        // on it and the slots stay sorted by `first_pair`.
        instance_table_[slot] = null_instance(first_pair);
        tail_keys_[slot] = InstanceKey{};
      }
    }
    // Every set has to take these slots, each when a frame next flips to it.
    for (TableSet& set : sets_) {
      if (!set.all)
        set.pending.push_back(InstanceRange{write.inst_begin, write.inst_begin + write.inst_count});
    }
    written += write.inst_count;
  }
  return true;
}

// The buffers a change's layout needs. Past the stride, every per-pair buffer is laid out again,
// with room to grow — half as much again, in whole pages of 4,096 pairs, so a world that fills its
// rings reallocates a handful of times and not on every tile — and past the instance capacity, in
// pages of 1,024 slots, likewise. Either way the old buffers go to `retired`, because a frame in
// flight may still be reading them, and every table set is made again and written whole by the
// frame that next flips to it.
bool GpuScene::fit_tables(TileChange& change, Vector<gfx::BufferResource>* retired,
                          std::string* error) {
  const u32 pair_end = layout_.pair_end();
  const u32 inst_end = layout_.instance_end();
  bool sets_again = false;
  if (pair_end > pair_stride_) {
    u64 stride = u64{pair_end} + u64{pair_end} / 2;
    stride = (stride + 4095) / 4096 * 4096;
    if (stride > k_max_pairs) stride = k_max_pairs;
    retire(visible, retired);
    retire(shadow_args, retired);
    retire(pair_entries, retired);
    retire(sw_args, retired);
    for (u32 i = 0; i < 2; ++i) {
      retire(draw_args[i], retired);
      retire(flags[i], retired);
    }
    retire(vertex_headers, retired);
    retire(vertex_records, retired);
    retire(vertex_indices, retired);
    pair_stride_ = static_cast<u32>(stride);
    if (!create_working_set(resolved_, error)) {
      // Nothing to draw with: the caller has to treat the scene as lost, as it would a failed
      // `create`.
      pair_stride_ = 0;
      return false;
    }
    change.grew = true;
    sets_again = true;
  }
  if (inst_end > instance_capacity_) {
    u64 capacity = u64{inst_end} + u64{inst_end} / 2;
    capacity = (capacity + 1023) / 1024 * 1024;
    instance_capacity_ = static_cast<u32>(capacity);
    sets_again = true;
  }
  if (sets_again) {
    // `instances` and `pair_table` still name the current set's old buffers, which the frames in
    // flight read; the next frame flips to a new set.
    for (TableSet& set : sets_) {
      retire(set.instances, retired);
      retire(set.pairs, retired);
      if (!create_table_set(set, error)) return false;
    }
    tables_changed_ = true;
  }
  instance_count_ = inst_end;
  pair_count_ = pair_end;
  return true;
}

bool GpuScene::reserve_table_sets(u32 frames_in_flight, std::string* error) {
  if (!dynamic_) return true;
  const u32 want = frames_in_flight > 2 ? frames_in_flight : 2u;
  while (sets_.size() < want) {
    TableSet set;
    if (!create_table_set(set, error)) {
      retire(set.instances, nullptr);
      retire(set.pairs, nullptr);
      return false;
    }
    sets_.push_back(std::move(set));
  }
  if (table_staging_.size() < frames_in_flight) table_staging_.resize(frames_in_flight);
  // A set added now has never been read, so it is written whole whenever a frame first flips to
  // it, like every set of a new scene.
  return true;
}

// The frame's half of a change (docs/subsystems/renderer.md, "Instances that come and go"). The
// set the frame flips to was last read by a frame at least as many frames ago as there are sets,
// and there are at least as many sets as frames in flight, so that frame is done and the set can
// be written; the frames still in flight read the others. What it writes is every slot changed
// since it was last current — its own pending runs, sorted and merged — copied out of this frame
// slot's staging, and then the pairs of the live instances among them, which the GPU expands.
bool GpuScene::prepare_tables(u32 slot, TableUpdate& out, std::string* error) {
  out.write = false;
  out.slots = out.expand = 0;
  out.pairs = 0;
  out.copies.clear();
  out.set = current_set_;
  if (!dynamic_ || !tables_changed_ || sets_.empty()) return true;
  tables_changed_ = false;
  current_set_ = (current_set_ + 1) % sets_.size();
  TableSet& set = sets_[current_set_];
  instances = set.instances;
  pair_table = set.pairs;
  out.set = current_set_;
  Vector<InstanceRange>& runs = set.pending;
  if (set.all) {
    runs.clear();
    runs.push_back(InstanceRange{0, instance_count_});
    set.all = false;
  }
  std::sort(runs.begin(), runs.end(),
            [](const InstanceRange& a, const InstanceRange& b) { return a.begin < b.begin; });
  u32 merged = 0;
  for (u32 i = 0; i < runs.size(); ++i) {
    InstanceRange run = runs[i];
    if (run.end > instance_count_) run.end = instance_count_;
    if (run.begin >= run.end) continue;
    if (merged > 0 && run.begin <= runs[merged - 1].end) {
      if (run.end > runs[merged - 1].end) runs[merged - 1].end = run.end;
    } else {
      runs[merged++] = run;
    }
  }
  runs.resize(merged);
  u32 slot_count = 0;
  u32 expand = 0;
  for (const InstanceRange& run : runs) {
    slot_count += run.end - run.begin;
    for (u32 i = run.begin; i < run.end; ++i)
      expand += instance_table_[i].mesh < data_->parts.size() ? 1u : 0u;
  }
  if (slot_count == 0) {
    runs.clear();
    return true;
  }
  const u64 instance_bytes = u64{slot_count} * sizeof(gfx::InstanceDesc);
  const u64 bytes = instance_bytes + u64{expand} * sizeof(u32);
  if (slot >= table_staging_.size()) table_staging_.resize(slot + 1);
  gfx::BufferResource& staging = table_staging_[slot];
  if (staging.size < bytes) {
    // The slot's last frame is done, so its staging can go now.
    gfx::destroy_buffer(*device_, staging);
    const u64 size = (bytes + bytes / 2 + 65535) / 65536 * 65536;
    if (!gfx::create_buffer(*device_, size,
                            gfx::BufferUsage::TransferSrc | gfx::BufferUsage::Storage |
                                gfx::BufferUsage::ShaderDeviceAddress,
                            true, staging, error)) {
      return false;
    }
  }
  auto* base = static_cast<u8*>(staging.mapped);
  auto* list = reinterpret_cast<u32*>(base + instance_bytes);
  u64 at = 0;
  u32 listed = 0;
  u64 pairs = 0;
  for (const InstanceRange& run : runs) {
    const u64 run_bytes = u64{run.end - run.begin} * sizeof(gfx::InstanceDesc);
    std::memcpy(base + at, instance_table_.data() + run.begin, static_cast<usize>(run_bytes));
    out.copies.push_back(
        gfx::BufferCopy{at, u64{run.begin} * sizeof(gfx::InstanceDesc), run_bytes});
    at += run_bytes;
    for (u32 i = run.begin; i < run.end; ++i) {
      const u32 mesh = instance_table_[i].mesh;
      if (mesh >= data_->parts.size()) continue;
      list[listed++] = i;
      pairs += data_->parts[mesh].cluster_count;
    }
  }
  runs.clear();
  out.write = true;
  out.staging = staging;
  out.list_address = staging.address + instance_bytes;
  out.slots = slot_count;
  out.expand = listed;
  out.pairs = pairs;
  return true;
}

u32 GpuScene::tail_index(u32 slot) const noexcept {
  if (!dynamic_ || slot < static_instances_ || slot >= instance_count_) return ~0u;
  const TileBlock* block = layout_.block_of_slot(slot);
  if (block == nullptr || slot - block->inst_begin >= block->instances) return ~0u;
  return block->tail_first + (slot - block->inst_begin);
}

void GpuScene::tail_indices(Vector<u32>& out) const {
  out.assign(instance_count_, ~0u);
  if (!dynamic_) return;
  for (const TileBlock& block : layout_.blocks()) {
    for (u32 k = 0; k < block.instances && block.inst_begin + k < instance_count_; ++k)
      out[block.inst_begin + k] = block.tail_first + k;
  }
}

bool GpuScene::validate_tables(std::string* why) const {
  auto fail = [&](const std::string& what) {
    if (why != nullptr) *why = what;
    return false;
  };
  if (instance_table_.size() != instance_count_) return fail("the table is not the slot count");
  for (u32 i = 1; i < instance_count_; ++i) {
    if (instance_table_[i].first_pair < instance_table_[i - 1].first_pair) {
      return fail("slot " + std::to_string(i) + "'s first pair is below the one before it");
    }
  }
  if (!dynamic_) return true;
  if (!layout_.validate(why)) return false;
  u32 live = 0;
  for (const TileBlock& block : layout_.blocks()) {
    u32 pair = block.pair_begin;
    for (u32 k = 0; k < block.inst_cap && block.inst_begin + k < instance_count_; ++k) {
      const gfx::InstanceDesc& desc = instance_table_[block.inst_begin + k];
      if (k < block.instances) {
        if (desc.mesh >= data_->parts.size() || desc.first_pair != pair) {
          return fail("block " + std::to_string(block.key) + "'s slot " + std::to_string(k) +
                      " is not its instance");
        }
        pair += data_->parts[desc.mesh].cluster_count;
      } else if (desc.mesh != null_mesh_ || desc.first_pair != block.pair_begin + block.pairs) {
        return fail("block " + std::to_string(block.key) + "'s slack is not null at its end");
      }
    }
    if (pair != block.pair_begin + block.pairs) return fail("a block's pairs are not its count");
    live += block.instances;
  }
  u32 named = 0;
  for (u32 i = static_instances_; i < instance_count_; ++i)
    named += instance_table_[i].mesh < data_->parts.size() ? 1u : 0u;
  if (named != live) return fail("a slot outside every block names an instance");
  return true;
}

bool GpuScene::upload_geometry(const ResolvedSettings& resolved, std::string* error) {
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  // Positions go to the GPU on the mesh-wide 16-bit grid: six bytes a vertex instead of twelve.
  const u64 float_position_bytes = u64{lod.mesh.vertices.size()} * sizeof(Vec3);
  const u64 quantized_position_bytes =
      u64{lod.mesh.quantized.size()} * sizeof(u16) + sizeof(gfx::MeshDesc);
  // The descriptors are scene-sized whatever the budget — the cull pass tests every pair of every
  // frame — and under streaming the `ClusterDesc` array is also what a page upload **patches**:
  // when a page lands in a slot its clusters' `vertex_offset` and `triangle_offset` are rewritten
  // to point into that slot, so the copy that puts the array here is a transfer destination.
  const gfx::BufferUsage cluster_usage =
      streamed_ ? (k_storage | gfx::BufferUsage::TransferDst) : k_storage;
  // With terrain rings every stream below is the scene's and then the rings' slots and arenas
  // (`lay_out_rings`): `cluster_count_` and the capacities count both, the scene's arrays only
  // their own part.
  const u64 scene_clusters = data_->cluster_count();
  if (!upload_room(device, lod.mesh.clusters.data(), scene_clusters * sizeof(geometry::ClusterDesc),
                   u64{cluster_count_} * sizeof(geometry::ClusterDesc), cluster_usage, clusters,
                   error) ||
      !upload_room(device, lod.lod.data(), scene_clusters * sizeof(geometry::ClusterLodDesc),
                   u64{cluster_count_} * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                   error)) {
    return false;
  }
  // Everything below this line is a *payload* stream: under streaming it is a page pool of
  // fixed-size slots that `create_streaming` already allocated, and nothing is uploaded here. A
  // ring vertex v's three u16 are at byte 6 v like the scene's, and the last one is read as two
  // words, so the arena's end keeps four bytes of room.
  const bool ring_room = !ring_slots_.empty();
  if (!streamed_ &&
      (!upload_room(device, lod.mesh.quantized.data(), u64{lod.mesh.quantized.size()} * sizeof(u16),
                    ring_room ? vertex_capacity_ * 6 + 4 : 0, k_storage, quantized, error) ||
       !upload_room(device, lod.mesh.triangles.data(), u64{lod.mesh.triangles.size()} * sizeof(u32),
                    triangle_capacity_ * sizeof(u32), k_storage, triangles, error))) {
    return false;
  }
  // The deformed-vertex pool, its per-instance table, and the per-entry allocation table. The
  // pool is device-local: nothing reads it back, and the cluster acceleration structure builds
  // take it as a build input.
  //
  // **The pool is a budget, not a sum over the population.** E25 gave every deformed instance a
  // block as long as its mesh's whole cluster-ordered vertex range — which is what made the pool
  // the binding memory constraint, 12.2 MB for 1,024 foxes and 1.7 GB if they had been
  // FlightHelmets, to write the ~1,300 clusters a frame draws. What is allocated here is
  // `RenderSettings::deform_pool_kib`, clamped **down** to what the whole-mesh layout would have
  // needed, so a single character costs what it always did and a scene small enough for that
  // layout can never overflow. `deform_alloc.slang` suballocates it per frame from the cull pass's
  // own visible list, one block per (instance, cluster) pair.
  if (deform_) {
    const u32 total_vertices = lod.mesh.vertices.size();
    u64 whole_mesh_vertices = 0;
    for (u32 i = 0; i < instance_count_; ++i) {
      // A skinned instance is deformed whatever the settings say; a rigid one only under
      // `--deform`, so a scene of one character and a hundred props allocates pool blocks for the
      // character alone and every prop stays on the instruction-for-instruction rigid path.
      const bool ring_slot = instance_table_[i].mesh >= data_->parts.size();
      const u32 instance_joints =
          skinned_ && i < data_->instance_joints.size() ? data_->instance_joints[i] : 0u;
      const geometry::ClusterMeshPart& part = part_of(instance_table_[i].mesh);
      // A terrain level (renderer.md, "The dunes in time-lapse"): the terrain's instance, whose
      // heights the pool pass reads out of the level's two fields — level 0, the scene's grid —
      // and every ring slot's (`lay_out_rings` gave them their levels).
      const bool terrain_level =
          resolved.terrain_levels && (instance_table_[i].mesh == data_->terrain_mesh || ring_slot);
      if (instance_joints == 0 && part.morph_channel_count == 0 && !resolved.settings.deform &&
          !terrain_level)
        continue;
      const u32 next = ring_slot ? part.first_vertex + part.cluster_count * 64u
                       : instance_table_[i].mesh + 1 < data_->parts.size()
                           ? data_->parts[instance_table_[i].mesh + 1].first_vertex
                           : total_vertices;
      gfx::DeformDesc desc{};
      // The chain's stage mask. A skinned instance runs the skinning stage; a `--deform` one runs
      // the procedural stage with the kind the flag named. They are separate bits, so an instance
      // that is both runs both — which is the thing E25 said needed a wider field. A terrain level
      // runs the terrain stage and nothing else.
      desc.stages =
          (instance_joints > 0 ? gfx::k_deform_stage_skin : 0u) |
          (resolved.settings.deform ? gfx::k_deform_stage_procedural | resolved.settings.deform_kind
                                    : 0u) |
          (part.morph_channel_count > 0 ? gfx::k_deform_stage_static | gfx::k_deform_stage_pose
                                        : 0u);
      if (terrain_level) {
        desc.stages = gfx::k_deform_stage_terrain;
        if (!ring_slot) instance_table_[i].terrain = 1;  // a slot's is its ring's level already
      }
      desc.first_vertex = part.first_vertex;
      desc.first_channel = part.first_morph_channel;
      desc.channel_count = part.morph_channel_count;
      deform_mesh_clusters_.push_back(part.cluster_count);
      // `joints` and `joint_count` stay zero in the *static* table: they are what a frame fills
      // in, in its own copy. A frame that hands over no matrices therefore leaves the instance at
      // its rest pose rather than reading an address from a previous frame.
      whole_mesh_vertices += next - part.first_vertex;
      instance_table_[i].deform = deform_descs_.size();
      deform_descs_.push_back(desc);
      deform_instance_.push_back(i);
    }
    deform_whole_mesh_bytes_ = whole_mesh_vertices * 3 * sizeof(f32);
    // Each shadow cascade's cut deforms its own copy of the pairs it draws, so the clamp that keeps
    // a small scene from ever overflowing allows the camera's whole meshes once per cascade too.
    deform_pool_vertices_ =
        pool_budget_vertices(resolved, whole_mesh_vertices * (u64{1} + cascade_count_));
    deform_pool_bytes_ = u64{deform_pool_vertices_} * 3 * sizeof(f32);
    constexpr gfx::BufferUsage k_pool_usage = k_address | gfx::BufferUsage::TransferDst |
                                              gfx::BufferUsage::TransferSrc |
                                              gfx::BufferUsage::AccelerationBuildInput;
    // One indirect dispatch block per (view, run): every view's pool pass covers its own cut, and
    // a cluster two views both draw gets a block in each, written twice with the same value.
    bool ok =
        gfx::create_buffer(device, deform_pool_bytes_, k_pool_usage, false, deform_pool, error) &&
        gfx::create_buffer(device, u64{visible_entries()} * sizeof(u32), k_address, false,
                           deform_slots, error) &&
        gfx::create_buffer(
            device, sizeof(gfx::DeformAlloc),
            k_address | gfx::BufferUsage::TransferSrc | gfx::BufferUsage::TransferDst, false,
            deform_alloc, error) &&
        gfx::upload_buffer(device, deform_descs_.data(),
                           deform_descs_.size() * sizeof(gfx::DeformDesc), k_storage, deform_table,
                           error) &&
        gfx::create_buffer(
            device, u64{gfx::k_draw_args_bytes} * (k_visible_runs * view_count_ + cascade_count_),
            k_args, false, deform_args, error);
    // The per-frame side of skinning: `k_joint_slots` regions of bone matrices and the same
    // number of copies of the deform table, both host-visible and persistently mapped, so a tick
    // is one memcpy of the span plus one rewrite of a table of 24-byte records. Nothing here is
    // touched again after `create`.
    if (ok && skinned_) {
      const u64 joint_region = joint_bytes();
      ok = gfx::create_buffer(device, joint_region * k_joint_slots, k_address, true, joints,
                              error) &&
           gfx::create_buffer(device,
                              u64{deform_descs_.size()} * sizeof(gfx::DeformDesc) * k_joint_slots,
                              k_address, true, deform_frames, error);
      if (ok) {
        // Every region starts as the static table, so a frame only ever rewrites the two words
        // that change and a slot that has never been written is still a valid rest pose.
        for (u32 slot = 0; slot < k_joint_slots; ++slot) {
          std::memcpy(deform_frame(slot), deform_descs_.data(),
                      deform_descs_.size() * sizeof(gfx::DeformDesc));
        }
        std::memset(joints.mapped, 0, joint_region * k_joint_slots);
      }
    }
    if (ok && !lod.mesh.morph_channels.empty()) ok = create_morph(resolved, error);
    if (ok && resolved.terrain_levels) ok = create_terrain(resolved, error);
    if (!ok) return false;
    ENGINE_LOG_INFO(
        log_renderer, "deformed-vertex pool", log::field("mode", deform_name(resolved.settings)),
        log::field("instances", deform_descs_.size()),
        log::field("skinned_instances", skinned_instances_), log::field("max_joints", max_joints_),
        log::field("pool_vertices", deform_pool_vertices_),
        log::field("pool_bytes", deform_pool_bytes_),
        log::field("whole_mesh_bytes", deform_whole_mesh_bytes_),
        log::field("slot_bytes", u64{visible_entries()} * sizeof(u32)),
        log::field("joint_bytes", skinned_ ? joint_bytes() * k_joint_slots : u64{0}));
  }
  // The float positions stay only for the frames that build acceleration structures: the
  // cluster structure builds read them. Under streaming they are one more page-pool stream, for
  // the reason the patched offsets force: a CLAS record addresses a cluster's vertices as
  // `vertices + vertex_offset * 12`, and `vertex_offset` is slot-relative.
  if (ray_tracing_ && !streamed_ &&
      !upload_room(device, lod.mesh.vertices.data(), float_position_bytes,
                   vertex_capacity_ * sizeof(Vec3), k_storage | gfx::k_build_input_usage, vertices,
                   error)) {
    return false;
  }
  ENGINE_LOG_INFO(log_renderer, "positions quantized",
                  log::field("vertices", lod.mesh.vertices.size()),
                  log::field("float_bytes", float_position_bytes),
                  log::field("quantized_bytes", quantized_position_bytes),
                  log::field("grid_step", lod.mesh.quant_scale));
  // The per-vertex binding stream, when the scene has one. It is the *mesh's* — cluster-ordered
  // and parallel to the positions, so one address serves every mesh of the scene exactly as the
  // quantized stream does, and a crowd of a hundred characters built from one mesh shares it.
  // Only the bone matrices are per instance (`DeformDesc::joints`).
  if (skinned_ && !gfx::upload_buffer(device, lod.mesh.skin.data(),
                                      u64{lod.mesh.skin.size()} * sizeof(geometry::SkinBinding),
                                      k_storage, skin, error)) {
    return false;
  }
  if (streamed_) return true;
  return upload_room(device, lod.mesh.attributes.data(),
                     u64{lod.mesh.attributes.size()} * sizeof(geometry::VertexAttributes),
                     vertex_capacity_ * sizeof(geometry::VertexAttributes), k_storage, attributes,
                     error);
}

// What one page costs the pool, stream by stream, which is what an upload copies and what the
// per-frame byte budget counts. The `ClusterDesc` records are in it because they are copied with
// the payload — they carry the slot-relative offsets and are useless without it.
//
// **Every sub-block is 16-byte aligned**, and the padding is counted, because this number is both
// the staging ring's layout and the floor the upload budget is raised to: a budget computed
// without the padding would be a byte or two short of the largest page, which is a page that never
// loads and a scene that never converges. One formula, used by both, is the only way those two
// cannot drift apart.
GpuScene::PageStage GpuScene::page_stage_layout(u32 page) const noexcept {
  PageStage out;
  if (!streamed_ || page >= page_count_) return out;
  const geometry::ClusterPageDesc& desc = data_->pages.pages[page];
  auto align16 = [](u64 value) { return (value + 15) & ~u64{15}; };
  out.ray_tracing = ray_tracing_;
  out.clusters = 0;
  out.quantized = align16(u64{desc.cluster_count} * sizeof(geometry::ClusterDesc));
  out.attributes = align16(out.quantized + u64{desc.vertex_count} * 3 * sizeof(u16));
  out.triangles =
      align16(out.attributes + u64{desc.vertex_count} * sizeof(geometry::VertexAttributes));
  out.total = align16(out.triangles + u64{desc.triangle_count} * sizeof(u32));
  if (!ray_tracing_) return out;
  // The float positions a CLAS build reads, and their 8-bit indices.
  out.vertices = out.total;
  out.indices8 = align16(out.vertices + u64{desc.vertex_count} * sizeof(Vec3));
  out.total = align16(out.indices8 + u64{desc.triangle_count} * 3);
  return out;
}

u32* GpuScene::residency_slot(u32 slot) noexcept {
  return static_cast<u32*>(residency.mapped) + u64{slot} * page_count_;
}

u64 GpuScene::residency_slot_address(u32 slot) const noexcept {
  return residency.address + u64{slot} * page_count_ * sizeof(u32);
}

u64 GpuScene::stream_params_address(u32 slot) const noexcept {
  return stream_params.address + u64{slot} * sizeof(gfx::StreamParams);
}

// The page pool and the tables the drawing rule reads. Runs before `upload_geometry`, because that
// is where the payload streams either go up whole or do not go up at all.
//
// **How many slots.** The budget is in page bytes, the pool is in slots, and the two have to agree
// on the worst case or a frame would admit a page with nowhere to put it. The bound is exact and
// cheap: sort the pages by size and take them smallest first until the budget is spent — no set of
// pages within the budget can be larger than that count. The pages of a real mesh are 85–95% full,
// so the pool is about a tenth larger than the budget it serves, which is the price of a
// fixed-size slot and is what makes an eviction a slot that can be reused without touching
// anything else.
// The morph stream and the chain's two morph stages (geometry.md, "Morph channels"; gfx.md, "The
// deform chain"). Three things are created here and only here:
//
//   - the stream's six arrays and the `gfx::MorphParams` block that names them. They are the
//     *scene's*: `merge_cluster_meshes` concatenated every mesh's channels and keyed the slices
//     by the global cluster index, so one block serves every mesh and every view of a frame.
//   - one weights region per frame slot, `2 * channels` floats per deformed instance. The static
//     half is written once here and again whenever the settings' weights change; the pose half is
//     what a frame writes from `FrameDesc::morph_weights`.
//   - the static shape caches, handed out in instance order until the budget runs out. An
//     instance that gets none keeps `DeformDesc::cache` at zero and runs its static stage every
//     frame over the cut — the same answer for more work, which is the same graceful degradation
//     the pool's `k_no_pool_slot` gives.
bool GpuScene::create_morph(const ResolvedSettings& resolved, std::string* error) {
  const gfx::Device& device = *device_;
  const geometry::ClusterMesh& mesh = data_->lod.mesh;
  morph_channel_count_ = mesh.morph_channels.size();
  if (morph_channel_count_ == 0 || deform_descs_.empty()) return true;

  if (!gfx::upload_buffer(device, mesh.morph_channels.data(),
                          u64{morph_channel_count_} * sizeof(geometry::MorphChannel), k_storage,
                          morph_channels, error) ||
      !gfx::upload_buffer(device, mesh.morph_cluster_slices.data(),
                          u64{mesh.morph_cluster_slices.size()} * sizeof(u32), k_storage,
                          morph_directory, error) ||
      !gfx::upload_buffer(device, mesh.morph_slices.data(),
                          u64{mesh.morph_slices.size()} * sizeof(geometry::MorphSlice), k_storage,
                          morph_slices, error) ||
      !gfx::upload_buffer(device, mesh.morph_indices.data(), mesh.morph_indices.size(), k_storage,
                          morph_indices, error) ||
      !gfx::upload_buffer(device, mesh.morph_deltas.data(),
                          u64{mesh.morph_deltas.size()} * sizeof(i16), k_storage, morph_deltas,
                          error)) {
    return false;
  }
  if (!mesh.morph_normal_deltas.empty() &&
      !gfx::upload_buffer(device, mesh.morph_normal_deltas.data(),
                          u64{mesh.morph_normal_deltas.size()} * sizeof(i16), k_storage,
                          morph_normals, error)) {
    return false;
  }
  gfx::MorphParams params{};
  params.channels = morph_channels.address;
  params.directory = morph_directory.address;
  params.slices = morph_slices.address;
  params.indices = morph_indices.address;
  params.deltas = morph_deltas.address;
  params.normal_deltas = mesh.morph_normal_deltas.empty() ? 0 : morph_normals.address;
  if (!gfx::upload_buffer(device, &params, sizeof(params), k_storage, morph_params, error))
    return false;

  // The deformed normal pool, parallel to the position pool: one octahedral word a vertex
  // against the position's twelve bytes, which is why carrying deformed normals costs a third of
  // what carrying deformed positions does.
  if (!gfx::create_buffer(device, u64{deform_pool_vertices_} * sizeof(u32), k_address, false,
                          deform_normals, error)) {
    return false;
  }
  const u64 weight_floats = morph_weight_floats();
  if (!gfx::create_buffer(device, weight_floats * sizeof(f32) * k_joint_slots, k_address, true,
                          morph_weights, error)) {
    return false;
  }
  std::memset(morph_weights.mapped, 0, weight_floats * sizeof(f32) * k_joint_slots);

  // The caches. Budget first, then hand out blocks in instance order.
  const u64 budget = u64{resolved.settings.static_shape_kib > 0 ? resolved.settings.static_shape_kib
                                                                : k_default_static_shape_kib} *
                     1024;
  const u32 total_vertices = data_->lod.mesh.vertices.size();
  Vector<u32> cache_offset(deform_descs_.size(), ~0u);
  u64 used = 0;
  for (u32 d = 0; d < deform_descs_.size(); ++d) {
    if ((deform_descs_[d].stages & gfx::k_deform_stage_static) == 0) continue;
    const u32 instance = deform_instance_[d];
    const geometry::ClusterMeshPart& part = data_->parts[instance_table_[instance].mesh];
    const u32 next = instance_table_[instance].mesh + 1 < data_->parts.size()
                         ? data_->parts[instance_table_[instance].mesh + 1].first_vertex
                         : total_vertices;
    const u64 bytes = u64{next - part.first_vertex} * sizeof(gfx::DeformCacheVertex);
    if (used + bytes > budget) continue;
    cache_offset[d] = static_cast<u32>(used / sizeof(gfx::DeformCacheVertex));
    used += bytes;
    ++static_cached_instances_;
  }
  static_cache_bytes_ = used;
  if (used > 0 && !gfx::create_buffer(device, used, k_address, false, static_cache, error)) {
    return false;
  }
  for (u32 d = 0; d < deform_descs_.size(); ++d) {
    gfx::DeformDesc& desc = deform_descs_[d];
    desc.weights = morph_weights.address;  // the frame overrides this with its own slot
    if (cache_offset[d] != ~0u) {
      desc.cache = static_cache.address + u64{cache_offset[d]} * sizeof(gfx::DeformCacheVertex);
    }
  }
  // The static weights, written into every slot's static half. They are the scene's, so they are
  // written once here and again only when the caller changes them.
  set_static_weights(resolved.settings.morph_static_weights);

  // **What a morphing instance needs from the cull pass**, exactly as a skinned one does: the
  // cluster spheres and both LOD spheres are the *rest* pose's, and a vertex a channel moves is
  // no longer inside them. `bounds_padding` is added to all three, so the bound here is
  // `geometry::morph_bounds_padding`'s — the sum over channels of |weight| x the channel's
  // largest displacement, which is the triangle inequality and nothing cleverer, because the
  // channels of a face move the same region in the same direction as often as not.
  //
  // The static half is known (the weights are the scene's); the **pose** half is not, so every
  // channel is priced at weight 1, which is the range a glTF weights track lives in. That is
  // conservative in the honest direction — a sphere too big draws a cluster that might have been
  // culled, a sphere too small drops a limb off the screen — and it is a per-instance float, so
  // tightening it later costs nothing to anyone.
  for (u32 d = 0; d < deform_descs_.size(); ++d) {
    const gfx::DeformDesc& desc = deform_descs_[d];
    if (desc.channel_count == 0) continue;
    f32 padding = 0.0f;
    for (u32 c = 0; c < desc.channel_count; ++c) {
      const geometry::MorphChannel& channel = mesh.morph_channels[desc.first_channel + c];
      const u32 at = desc.first_channel + c;
      const f32 weight = at < resolved.settings.morph_static_weights.size()
                             ? resolved.settings.morph_static_weights[at]
                             : 0.0f;
      const f32 magnitude = weight < 0.0f ? -weight : weight;
      padding += (magnitude + 1.0f) * channel.max_displacement;
    }
    gfx::InstanceDesc& instance = instance_table_[deform_instance_[d]];
    instance.bounds_padding += padding;
  }
  // Re-upload the table now that the caches and the weights are on it.
  gfx::destroy_buffer(device, deform_table);
  if (!gfx::upload_buffer(device, deform_descs_.data(),
                          deform_descs_.size() * sizeof(gfx::DeformDesc), k_storage, deform_table,
                          error)) {
    return false;
  }
  // A morphed scene needs the **per-frame** table even when nothing is skinned, because the pose
  // weights live in a per-slot region and the record has to point at this slot's. A skinned scene
  // already has one; this is the case where morphs alone force it.
  if (!has_frame_table() &&
      !gfx::create_buffer(device,
                          u64{deform_descs_.size()} * sizeof(gfx::DeformDesc) * k_joint_slots,
                          k_address, true, deform_frames, error)) {
    return false;
  }
  for (u32 slot = 0; slot < k_joint_slots; ++slot) {
    std::memcpy(deform_frame(slot), deform_descs_.data(),
                deform_descs_.size() * sizeof(gfx::DeformDesc));
  }
  ENGINE_LOG_INFO(log_renderer, "morph channels", log::field("channels", morph_channel_count_),
                  log::field("deltas", mesh.morph_delta_count),
                  log::field("slices", mesh.morph_slices.size()),
                  log::field("cached_instances", static_cached_instances_),
                  log::field("cache_bytes", static_cache_bytes_),
                  log::field("normal_pool_bytes", u64{deform_pool_vertices_} * sizeof(u32)));
  return true;
}

void GpuScene::set_static_weights(std::span<const f32> weights) {
  if (morph_channel_count_ == 0 || morph_weights.mapped == nullptr) return;
  // Every slot, not just the current one: the static weights are the scene's, so a slot the frame
  // loop has not reached yet must already hold them rather than the previous character's.
  for (u32 slot = 0; slot < k_joint_slots; ++slot) {
    f32* region = morph_weight_slot(slot);
    for (u32 d = 0; d < deform_descs_.size(); ++d) {
      const u32 first = deform_descs_[d].first_channel;
      const u32 count = deform_descs_[d].channel_count;
      // The static half is the first `morph_channel_count_` floats of this entry's pair of halves.
      f32* statics = region + u64{d} * 2 * morph_channel_count_;
      for (u32 c = 0; c < count; ++c)
        statics[c] = first + c < weights.size() ? weights[first + c] : 0.0f;
    }
  }
  static_cache_dirty_ = true;
}

// ---- terrain levels (renderer.md, "The dunes in time-lapse")
// -------------------------------------
//
// The device side of a moving terrain, and nothing about when it moves: `TerrainMotion` decides
// that, evaluates the fields off the frame, and hands them over through `terrain_upload` and
// `terrain_show`. Level 0 is the scene's own grid, drawn by the terrain mesh's instance.
bool GpuScene::create_terrain(const ResolvedSettings& resolved, std::string* error) {
  (void)resolved;
  const gfx::Device& device = *device_;
  terrain_.clear();
  terrain_upload_budget_ = static_cast<u64>(terrain_upload_mib.get()) * 1024 * 1024;
  u32 instance = ~0u;
  for (u32 i = 0; i < instance_count_; ++i) {
    if (instance_table_[i].terrain == 1) {
      instance = i;
      break;
    }
  }
  if (instance == ~0u) return true;
  // A level's vertices are placed on its lattice in mesh space, which the cull pass and the pool
  // pass take for the world; a terrain's instance is the identity, and anything else is refused
  // rather than drawn in the wrong place.
  const Mat4& world = instance_table_[instance].world;
  const Mat4 identity = Mat4::identity();
  if (std::memcmp(&world, &identity, sizeof(Mat4)) != 0) {
    if (error != nullptr) *error = "a moving terrain's instance must have the identity transform";
    return false;
  }
  // Level 0 is the scene's grid; with rings, level k > 0 is ring level k of the ring set, whose
  // fields cover a window of the ring's lattice as large as any layout's (`field_window`).
  const u32 level_count = ring_slots_.empty() ? 1u : rings_->level_count();
  for (u32 k = 0; k < level_count; ++k) {
    TerrainLevel level;
    if (k == 0) {
      level.lattice = terrain_scene_lattice(data_->terrain);
      level.instance = instance;
      // Under the world's tiles the grid draws nothing (its hole covers the world) and nothing
      // evaluates a field for it: a slot of one sample stands for its fields (renderer.md, "The
      // ground from the world's tiles").
      const bool drawn = ring_slots_.empty() || rings_->grid_drawn();
      level.capacity = drawn ? u64{level.lattice.size} * level.lattice.size : 1u;
      level.slots = drawn ? 3u : 1u;
    } else {
      level.lattice = rings_->lattice(k);
      level.capacity = rings_->field_capacity(k);
      level.slots = 4;
    }
    for (u32 s = 0; s < level.slots; ++s) {
      if (!gfx::create_buffer(device, level.capacity * sizeof(f32),
                              k_address | gfx::BufferUsage::TransferDst, false, level.fields[s],
                              error)) {
        for (u32 t = 0; t < s; ++t)
          gfx::destroy_buffer(device, level.fields[t]);
        return false;
      }
    }
    level.skirt = k == 0 ? 0.0f : rings_->skirt_m(k);
    // A world tile's border along a coarser tile is drawn from that tile's level, which its
    // vertices name (renderer.md, "The ground from the world's tiles").
    level.flags = k > 0 && rings_->shares_vertices() ? gfx::k_terrain_level_named : 0u;
    terrain_.push_back(std::move(level));
  }
  // The per-frame tables, one region per frame slot like the joint matrices: the frame writes its
  // own and a slot is not reused until the frame that last had it is done.
  if (!gfx::create_buffer(device,
                          u64{k_joint_slots} * terrain_.size() * sizeof(gfx::TerrainLevelDesc),
                          k_address, true, terrain_table_, error)) {
    return false;
  }
  std::memset(terrain_table_.mapped, 0, terrain_table_.size);
  // The moved field's normals go into the normal pool, which a scene with morph channels already
  // has; one without makes it here, parallel to the position pool.
  if (!deform_normals.buffer.valid() &&
      !gfx::create_buffer(device, u64{deform_pool_vertices_} * sizeof(u32), k_address, false,
                          deform_normals, error)) {
    return false;
  }
  // The fields' staging lists at more than a level can have in flight — a field evaluating, its
  // pieces uploading and a re-centre's pair — so taking one back allocates nothing on the frame's
  // thread (`terrain_staging`).
  const u32 stagings = 2 * k_terrain_field_slots * terrain_.size();
  field_pool_.reserve(stagings);
  field_retired_.reserve(stagings);
  for (Vector<gfx::BufferResource>& returning : field_returning_)
    returning.reserve(stagings);
  u64 bytes = 0;
  for (const TerrainLevel& l : terrain_)
    bytes += l.capacity * sizeof(f32) * l.slots;
  ENGINE_LOG_INFO(log_renderer, "terrain levels", log::field("levels", terrain_.size()),
                  log::field("field_bytes", bytes),
                  log::field("normal_pool_bytes", u64{deform_pool_vertices_} * sizeof(u32)));
  return true;
}

bool GpuScene::terrain_staging(u32 level, u64 samples, gfx::BufferResource& out,
                               std::string* error) {
  // The smallest free buffer the field fits in: one level's buffers never go to a larger level's
  // fields while a smaller one would do.
  const u64 bytes = samples * sizeof(f32);
  u32 best = ~0u;
  for (u32 i = 0; i < field_pool_.size(); ++i) {
    if (field_pool_[i].size < bytes) continue;
    if (best == ~0u || field_pool_[i].size < field_pool_[best].size) best = i;
  }
  if (best != ~0u) {
    out = field_pool_[best];
    field_pool_[best] = field_pool_[field_pool_.size() - 1];
    field_pool_.pop_back();
    return true;
  }
  // None free: one the level's whole field fits in, so it serves any window of the level later.
  const u64 capacity = level < terrain_.size() ? terrain_[level].capacity * sizeof(f32) : 0;
  if (!gfx::create_buffer(*device_, std::max(bytes, capacity), gfx::BufferUsage::TransferSrc, true,
                          out, error)) {
    return false;
  }
  ++field_staging_made_;
  return true;
}

bool GpuScene::terrain_slot_uploaded(u32 level, u32 slot) const noexcept {
  if (level >= terrain_.size()) return false;
  if (terrain_upload_budget_ == 0) return true;  // the next frame records every piece
  for (const TerrainLevel::Pending& p : terrain_[level].pending) {
    if (p.slot == slot && !p.urgent) return false;
  }
  return true;
}

bool GpuScene::terrain_upload(u32 level, u32 slot, const gfx::TerrainField& window,
                              const gfx::BufferResource& staging, std::string* error, bool urgent) {
  if (level >= terrain_.size() || slot >= terrain_[level].slots) {
    if (error != nullptr) *error = "terrain_upload: no such level or field slot";
    return false;
  }
  TerrainLevel& l = terrain_[level];
  const u64 bytes = u64{window.nx} * window.nz * sizeof(f32);
  if (bytes > l.capacity * sizeof(f32) || bytes > staging.size) {
    if (error != nullptr) *error = "terrain_upload: the window is larger than the field slot";
    return false;
  }
  l.windows[slot] = window;
  l.windows[slot].heights = l.fields[slot].address;
  TerrainLevel::Pending p;
  p.staging = staging;
  p.field = l.fields[slot];
  p.slot = slot;
  p.bytes = bytes;
  p.urgent = urgent;
  l.pending.push_back(p);
  return true;
}

void GpuScene::terrain_show(u32 level, u32 slot_a, u32 slot_b, f32 blend, f32 padding,
                            Vec4 hole) noexcept {
  if (level >= terrain_.size()) return;
  TerrainLevel& l = terrain_[level];
  l.shown_a = slot_a < l.slots ? slot_a : ~0u;
  l.shown_b = slot_b < l.slots ? slot_b : ~0u;
  l.blend = blend;
  l.padding = padding;
  l.hole = hole;
}

void GpuScene::terrain_prepare(u32 slot, TerrainUpdate& out) {
  out.table = 0;
  out.copies.clear();
  out.fields.clear();
  out.retire.clear();
  out.geometry.clear();
  out.geometry_bytes = 0;
  const u32 region = slot < k_joint_slots ? slot : slot % k_joint_slots;
  // The staging ring's share of the frame that last had this slot is free (that frame is done);
  // everything staged since the last frame, with the records below, is this one's (the end).
  if (staging_ring_.mapped != nullptr) ring_release(region);
  // The fields' staging buffers the frame that last had this slot was the last to read are free
  // again, and the ones handed back uncopied since the last frame come back with this one.
  if (region < k_joint_slots) {
    Vector<gfx::BufferResource>& returning = field_returning_[region];
    for (const gfx::BufferResource& staging : returning)
      field_pool_.push_back(staging);
    returning.clear();
    for (const gfx::BufferResource& staging : field_retired_)
      returning.push_back(staging);
    field_retired_.clear();
  }
  if (terrain_.empty() || terrain_table_.mapped == nullptr) return;
  auto* table =
      static_cast<gfx::TerrainLevelDesc*>(terrain_table_.mapped) + u64{region} * terrain_.size();
  // The fields' copies, a budget a frame (`renderer.terrain.upload_mib`), in the order they were
  // handed over, a field in as many pieces as the budget cuts it into. A slot is shown only once
  // its last piece is recorded (`terrain_slot_uploaded`), so a piece left for the next frame is
  // never drawn half-copied — except an `urgent` one, which a frame about to show it copies whole,
  // and every piece when the budget is 0. Until 2026-09-26 a frame recorded eight copies whole and
  // left the rest, and the rings test caught an inner ring a frame's worth of sand behind: an
  // offscreen frame that catches the surface up installs several fields a level, each into the
  // slot the one before freed, and the ninth copy's slot was shown holding what it held before.
  // A copy that a later one into the same slot replaces is dropped — two copies into one buffer in
  // one pass would race — and its staging comes back with the frame's (an earlier frame may have
  // copied a piece of it, and this one is done after that one).
  Vector<gfx::BufferResource>* returning =
      region < k_joint_slots ? &field_returning_[region] : &field_retired_;
  u64 budget = terrain_upload_budget_ == 0 ? ~u64{0} : terrain_upload_budget_;
  for (u32 k = 0; k < terrain_.size(); ++k) {
    TerrainLevel& l = terrain_[k];
    // What is left for later frames is compacted to the list's front in place (`left` never passes
    // `c`, and the look ahead reads only past it), so the list keeps what it grew to and a frame
    // allocates nothing.
    u32 left = 0;
    for (u32 c = 0; c < l.pending.size(); ++c) {
      TerrainLevel::Pending p = l.pending[c];
      bool superseded = false;
      for (u32 d = c + 1; d < l.pending.size(); ++d)
        superseded = superseded || l.pending[d].field.buffer == p.field.buffer;
      if (superseded) {
        returning->push_back(p.staging);
        continue;
      }
      const u64 room = p.urgent ? p.bytes - p.done : std::min(budget, p.bytes - p.done);
      if (room == 0) {
        l.pending[left++] = p;
        continue;
      }
      TerrainUpdate::Copy copy;
      copy.staging = p.staging;
      copy.field = p.field;
      copy.offset = p.done;
      copy.bytes = room;
      copy.last = p.done + room == p.bytes;
      out.copies.push_back(copy);
      if (!p.urgent) budget -= room;
      p.done += room;
      if (!copy.last) {
        l.pending[left++] = p;
      } else {
        returning->push_back(p.staging);  // free once this frame is done
      }
    }
    l.pending.resize(left);
    gfx::TerrainLevelDesc desc{};
    desc.origin = Vec2{static_cast<f32>(l.lattice.origin_x), static_cast<f32>(l.lattice.origin_z)};
    desc.spacing = static_cast<f32>(l.lattice.spacing);
    desc.blend = l.blend;
    desc.padding = l.padding;
    desc.hole = l.hole;
    desc.skirt = l.skirt;
    desc.flags = l.flags;
    if (l.shown_a != ~0u) {
      desc.a = l.windows[l.shown_a];
      out.fields.push_back(l.fields[l.shown_a]);
    }
    if (l.shown_b != ~0u && l.shown_b != l.shown_a) {
      desc.b = l.windows[l.shown_b];
      out.fields.push_back(l.fields[l.shown_b]);
    }
    table[k] = desc;
  }
  out.table =
      terrain_table_.address + u64{region} * terrain_.size() * sizeof(gfx::TerrainLevelDesc);
  // The rings' chunk uploads since the last frame, and the records of every slot turned on or off,
  // each written once with the state it ends in (a slot turned off and another on in one frame are
  // two records, never one written twice).
  if (!pending_mesh_writes_.empty() && meshes.buffer.valid()) {
    std::sort(pending_mesh_writes_.begin(), pending_mesh_writes_.end());
    pending_mesh_writes_.erase(
        std::unique(pending_mesh_writes_.begin(), pending_mesh_writes_.end()),
        pending_mesh_writes_.end());
    // Through the staging ring, which this frame's share of is released when its slot comes round;
    // until 2026-10-04 a buffer made for them every frame that turned a slot on or off.
    const u64 bytes = u64{pending_mesh_writes_.size()} * sizeof(gfx::MeshDesc);
    u64 ring_at = 0;
    const bool in_ring = ring_take(bytes, ring_at);
    gfx::BufferResource staging = in_ring ? staging_ring_ : gfx::BufferResource{};
    std::string error;
    if (!in_ring && staging_ring_.mapped != nullptr) {
      ++ring_overflows_;
      ring_overflow_bytes_ += bytes;
    }
    if (in_ring ||
        gfx::create_buffer(*device_, bytes, gfx::BufferUsage::TransferSrc, true, staging, &error)) {
      auto* descs = reinterpret_cast<gfx::MeshDesc*>(static_cast<u8*>(staging.mapped) + ring_at);
      for (u32 k = 0; k < pending_mesh_writes_.size(); ++k) {
        const u32 mesh = pending_mesh_writes_[k];
        descs[k] = mesh_descs_[mesh];
        pending_geometry_.push_back(TerrainUpdate::GeometryCopy{
            staging.buffer, meshes.buffer,
            gfx::BufferCopy{ring_at + u64{k} * sizeof(gfx::MeshDesc),
                            u64{mesh} * sizeof(gfx::MeshDesc), sizeof(gfx::MeshDesc)}});
      }
      pending_geometry_bytes_ += bytes;
      if (!in_ring) pending_staging_.push_back(staging);
    } else {
      ENGINE_LOG_ERROR(log_renderer, "a terrain ring's slot records could not be staged",
                       log::field("error", error));
    }
    pending_mesh_writes_.clear();
  }
  // Swapped and cleared rather than moved from, so both lists keep what they grew to and a frame
  // of chunk uploads allocates nothing once they have (the long-flight test counts it).
  std::swap(out.geometry, pending_geometry_);
  pending_geometry_.clear();
  for (const gfx::BufferResource& staging : pending_staging_)
    out.retire.push_back(staging);
  pending_staging_.clear();
  out.geometry_bytes = pending_geometry_bytes_;
  pending_geometry_bytes_ = 0;
  if (staging_ring_.mapped != nullptr) ring_slot_end_[region] = ring_head_;
}

// ---- the rings' slots (renderer.md, "The rings in the scene")
// ------------------------------------

const geometry::ClusterMeshPart& GpuScene::part_of(u32 mesh) const noexcept {
  const u32 scene = data_->parts.size();
  return mesh < scene ? data_->parts[mesh] : slot_parts_[mesh - scene];
}

bool GpuScene::take_range(Vector<Range>& free, u64 count, Range& out) noexcept {
  if (count == 0) {
    out = Range{};
    return true;
  }
  // First fit: the arenas turn over a ring's chunks a re-centre at a time, and chunks are all about
  // one size, so a first fit leaves few holes a chunk does not fit.
  for (u32 k = 0; k < free.size(); ++k) {
    if (free[k].count < count) continue;
    out = Range{free[k].offset, count};
    free[k].offset += count;
    free[k].count -= count;
    if (free[k].count == 0) free.erase(free.begin() + k);
    return true;
  }
  return false;
}

void GpuScene::give_range(Vector<Range>& free, Range range) noexcept {
  if (range.count == 0) return;
  // Kept sorted by offset, and merged with a neighbour it touches.
  u32 at = 0;
  while (at < free.size() && free[at].offset < range.offset)
    ++at;
  free.insert(free.begin() + at, range);
  if (at + 1 < free.size() && free[at].offset + free[at].count == free[at + 1].offset) {
    free[at].count += free[at + 1].count;
    free.erase(free.begin() + at + 1);
  }
  if (at > 0 && free[at - 1].offset + free[at - 1].count == free[at].offset) {
    free[at - 1].count += free[at].count;
    free.erase(free.begin() + at);
  }
}

bool GpuScene::lay_out_rings(const ResolvedSettings& resolved, TerrainLevelSet& rings,
                             std::string* error) {
  (void)resolved;
  ring_slots_.clear();
  slot_parts_.clear();
  u64 bytes = 0;
  for (u32 level = 1; level < rings.level_count(); ++level) {
    const TerrainLevelSet::Capacity c = rings.capacity(level);
    if (c.slots == 0 || c.clusters_per_slot == 0) continue;
    RingSlots ring;
    ring.level = level;
    ring.first_mesh = data_->parts.size() + slot_parts_.size();
    ring.first_instance = instance_table_.size();
    ring.first_cluster = cluster_count_;
    ring.clusters_per_slot = c.clusters_per_slot;
    ring.vertex_base = vertex_capacity_;
    ring.triangle_base = triangle_capacity_;
    ring.vertex_capacity = c.vertices;
    ring.triangle_capacity = c.triangles;
    ring.slots.resize(c.slots);
    for (u32 s = c.slots; s-- > 0;)
      ring.free_slots.push_back(s);  // slot 0 is taken first
    // The arenas' free lists at the most they can hold, so a swap that frees ranges allocates
    // nothing on the frame's thread (renderer.md, "Ground to the horizon"): free ranges are kept
    // apart and merged with a range they touch, so two of them always have a slot's range between
    // them, and there are at most a range more than the slots — and one more while `give_range`
    // inserts before it merges. Until 2026-10-03 they grew as they fragmented, and a far square's
    // move, which frees a whole strip of tiles at once, took them to a new length: the 5
    // allocations the long flight counted with far levels.
    ring.free_vertices.reserve(c.slots + 2);
    ring.free_triangles.reserve(c.slots + 2);
    ring.free_vertices.push_back(Range{vertex_capacity_, c.vertices});
    ring.free_triangles.push_back(Range{triangle_capacity_, c.triangles});
    for (u32 s = 0; s < c.slots; ++s) {
      // A slot's mesh owns a fixed run of the cluster array and of the pairs, `clusters_per_slot`
      // long, whichever chunk it draws; the cull pass drops a pair past the chunk's own clusters
      // as it drops a streamed world's hole (the MeshDesc's count is the chunk's).
      geometry::ClusterMeshPart part;
      part.first_cluster = cluster_count_ + s * c.clusters_per_slot;
      part.cluster_count = c.clusters_per_slot;
      part.leaf_cluster_count = c.clusters_per_slot;
      part.first_vertex = static_cast<u32>(vertex_capacity_);
      slot_parts_.push_back(part);
      gfx::InstanceDesc instance{};
      gfx::set_instance_transform(instance, Mat4::identity());
      instance.mesh = ring.first_mesh + s;
      instance.first_pair = pair_count_ + s * c.clusters_per_slot;
      instance.terrain = level + 1;
      instance_table_.push_back(instance);
    }
    const u64 slot_clusters = u64{c.slots} * c.clusters_per_slot;
    if (u64{pair_count_} + slot_clusters > k_max_pairs) {
      if (error != nullptr)
        *error =
            "the terrain rings' slots would take the scene past the 2^24 pairs the "
            "visibility id names; lower terrain.rings.*";
      return false;
    }
    cluster_count_ += static_cast<u32>(slot_clusters);
    pair_count_ += static_cast<u32>(slot_clusters);
    instance_count_ += c.slots;
    vertex_capacity_ += c.vertices;
    triangle_capacity_ += c.triangles;
    bytes +=
        slot_clusters * (sizeof(geometry::ClusterDesc) + sizeof(geometry::ClusterLodDesc) + 4) +
        c.vertices * (6 + sizeof(geometry::VertexAttributes) + (ray_tracing_ ? sizeof(Vec3) : 0)) +
        c.triangles * (sizeof(u32) + (ray_tracing_ ? 3 : 0));
    ring_slots_.push_back(std::move(ring));
  }
  // A frame's slot records: a swap turns at most every slot off and every slot on.
  u32 all_slots = 0;
  for (const RingSlots& ring : ring_slots_)
    all_slots += ring.slots.size();
  pending_mesh_writes_.reserve(2 * all_slots);
  terrain_ring_bytes_ = bytes;
  ENGINE_LOG_INFO(log_renderer, "terrain ring slots", log::field("rings", ring_slots_.size()),
                  log::field("instances", instance_count_), log::field("pairs", pair_count_),
                  log::field("bytes", bytes));
  return true;
}

bool GpuScene::create_staging_ring(std::string* error) {
  const u64 bytes = u64(terrain_staging_mib.get()) * 1024 * 1024;
  if (!gfx::create_buffer(*device_, bytes, gfx::BufferUsage::TransferSrc, true, staging_ring_,
                          error)) {
    return false;
  }
  // Touched once, here: the system commits a mapped page on its first write, and a page first
  // written inside a frame is a fault the frame's thread pays for (renderer.md, "What a frame waits
  // for").
  std::memset(staging_ring_.mapped, 0, static_cast<usize>(bytes));
  ring_head_ = ring_tail_ = 0;
  for (u64& end : ring_slot_end_)
    end = 0;
  return true;
}

bool GpuScene::ring_fits(u64 bytes) const noexcept {
  const u64 capacity = staging_ring_.size;
  if (staging_ring_.mapped == nullptr || bytes == 0 || bytes > capacity) return false;
  const u64 at = ring_head_ % capacity;
  // A staging never wraps: one that would run past the end starts again at the front.
  const u64 skip = at + bytes > capacity ? capacity - at : 0;
  return ring_head_ + skip + bytes - ring_tail_ <= capacity;
}

bool GpuScene::ring_take(u64 bytes, u64& offset) noexcept {
  bytes = (bytes + 15) & ~u64{15};
  if (!ring_fits(bytes)) return false;
  const u64 capacity = staging_ring_.size;
  const u64 at = ring_head_ % capacity;
  if (at + bytes > capacity) ring_head_ += capacity - at;
  offset = ring_head_ % capacity;
  ring_head_ += bytes;
  ring_peak_ = std::max(ring_peak_, ring_head_ - ring_tail_);
  return true;
}

void GpuScene::ring_release(u32 region) noexcept {
  // Frames complete in order, so everything the frame that last had this slot took — and every
  // frame before it — is done with: the tail moves up to where that frame's share ended.
  if (region >= k_joint_slots) return;
  ring_tail_ = std::max(ring_tail_, ring_slot_end_[region]);
  ring_slot_end_[region] = ring_tail_;
}

u64 GpuScene::chunk_staging_bytes(const TerrainChunk& chunk) const noexcept {
  const geometry::ClusterMesh& mesh = chunk.lod.mesh;
  const u64 nc = mesh.clusters.size();
  const u64 nv = mesh.vertices.size();
  const u64 nt = mesh.triangles.size();
  const auto aligned = [](u64 v) { return (v + 15) & ~u64{15}; };
  return aligned(nc * sizeof(geometry::ClusterDesc)) +
         aligned(nc * sizeof(geometry::ClusterLodDesc)) + aligned(nv * 6) +
         aligned(nv * sizeof(geometry::VertexAttributes)) + aligned(nt * sizeof(u32)) +
         (ray_tracing_ ? aligned(nv * sizeof(Vec3)) + aligned(nt * 3) : 0);
}

bool GpuScene::terrain_chunk_fits(const TerrainChunk& chunk) const noexcept {
  return ring_fits(chunk_staging_bytes(chunk));
}

bool GpuScene::stage_chunk(RingSlots& ring, u32 s, TerrainChunk& chunk, std::string* error) {
  const geometry::ClusterMesh& mesh = chunk.lod.mesh;
  const u32 nc = mesh.clusters.size();
  const u64 nv = mesh.vertices.size();
  const u64 nt = mesh.triangles.size();
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  if (nc > ring.clusters_per_slot) {
    return fail("a terrain ring's chunk has " + std::to_string(nc) +
                " clusters, more than a slot's " + std::to_string(ring.clusters_per_slot) +
                " (raise renderer.terrain.ring_slack)");
  }
  TerrainSlot& slot = ring.slots[s];
  if (!take_range(ring.free_vertices, nv, slot.vertices)) {
    return fail("a terrain ring's vertex arena has no room for a chunk of " + std::to_string(nv) +
                " (raise renderer.terrain.ring_slack)");
  }
  if (!take_range(ring.free_triangles, nt, slot.triangles)) {
    give_range(ring.free_vertices, slot.vertices);
    slot.vertices = Range{};
    return fail("a terrain ring's triangle arena has no room for a chunk of " + std::to_string(nt) +
                " (raise renderer.terrain.ring_slack)");
  }
  const u64 v0 = slot.vertices.offset;
  const u64 t0 = slot.triangles.offset;
  // One staging buffer for the chunk, its streams one after another at 16-byte offsets.
  const auto aligned = [](u64 v) { return (v + 15) & ~u64{15}; };
  const u64 at_clusters = 0;
  const u64 at_lods = aligned(at_clusters + u64{nc} * sizeof(geometry::ClusterDesc));
  const u64 at_quant = aligned(at_lods + u64{nc} * sizeof(geometry::ClusterLodDesc));
  const u64 at_attributes = aligned(at_quant + nv * 6);
  const u64 at_triangles = aligned(at_attributes + nv * sizeof(geometry::VertexAttributes));
  const u64 at_vertices = aligned(at_triangles + nt * sizeof(u32));
  const u64 at_indices = aligned(at_vertices + (ray_tracing_ ? nv * sizeof(Vec3) : 0));
  const u64 total = aligned(at_indices + (ray_tracing_ ? nt * 3 : 0));
  // Into the staging ring when it has room; a buffer of its own when it does not (the first fill,
  // or an offscreen frame staging a whole rebuild), which the frame retires once it is done.
  u64 ring_at = 0;
  const bool in_ring = ring_take(total, ring_at);
  gfx::BufferResource staging;
  if (in_ring) {
    staging = staging_ring_;
  } else {
    if (!gfx::create_buffer(*device_, total, gfx::BufferUsage::TransferSrc, true, staging, error)) {
      give_range(ring.free_vertices, slot.vertices);
      give_range(ring.free_triangles, slot.triangles);
      slot.vertices = slot.triangles = Range{};
      return false;
    }
    if (staging_ring_.mapped != nullptr) {
      ++ring_overflows_;
      ring_overflow_bytes_ += total;
    }
  }
  u8* base = static_cast<u8*>(staging.mapped) + ring_at;
  // The clusters, with their vertex and triangle offsets moved from the chunk's arrays to the
  // arenas' (the streaming path patches a page's clusters the same way).
  auto* clusters_out = reinterpret_cast<geometry::ClusterDesc*>(base + at_clusters);
  for (u32 c = 0; c < nc; ++c) {
    clusters_out[c] = mesh.clusters[c];
    clusters_out[c].vertex_offset += static_cast<u32>(v0);
    clusters_out[c].triangle_offset += static_cast<u32>(t0);
  }
  std::memcpy(base + at_lods, chunk.lod.lod.data(), u64{nc} * sizeof(geometry::ClusterLodDesc));
  std::memcpy(base + at_quant, mesh.quantized.data(), nv * 6);
  // A skirt vertex's rest normal is turned straight down: the terrain stage reads it to hang the
  // vertex under the field by the ring's skirt depth (deform.slang), and the pool's normals are
  // the ones the resolve shades with.
  auto* attributes_out = reinterpret_cast<geometry::VertexAttributes*>(base + at_attributes);
  const u32 down = geometry::encode_normal_oct(Vec3{0.0f, -1.0f, 0.0f});
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    attributes_out[v] = mesh.attributes[v];
    if (v < mesh.vertex_source.size() && mesh.vertex_source[v] >= chunk.grid_vertices)
      attributes_out[v].normal_oct = down;
  }
  std::memcpy(base + at_triangles, mesh.triangles.data(), nt * sizeof(u32));
  if (ray_tracing_) {
    std::memcpy(base + at_vertices, mesh.vertices.data(), nv * sizeof(Vec3));
    // `gfx::pack_cluster_indices`' bytes, written straight into the staging buffer: that function
    // fills a list, which was an allocation on the frame's thread for every chunk staged with the
    // ray tracing chain on.
    u8* indices = base + at_indices;
    for (u64 t = 0; t < nt; ++t) {
      const u32 triangle = mesh.triangles[static_cast<u32>(t)];
      indices[t * 3 + 0] = static_cast<u8>(triangle & 0xffu);
      indices[t * 3 + 1] = static_cast<u8>((triangle >> 8) & 0xffu);
      indices[t * 3 + 2] = static_cast<u8>((triangle >> 16) & 0xffu);
    }
  }
  const u64 cluster_at = u64{ring.first_cluster} + u64{s} * ring.clusters_per_slot;
  const auto copy = [&](const gfx::BufferResource& dst, u64 src, u64 dst_offset, u64 size) {
    if (size == 0) return;
    pending_geometry_.push_back(TerrainUpdate::GeometryCopy{
        staging.buffer, dst.buffer, gfx::BufferCopy{ring_at + src, dst_offset, size}});
  };
  copy(clusters, at_clusters, cluster_at * sizeof(geometry::ClusterDesc),
       u64{nc} * sizeof(geometry::ClusterDesc));
  copy(lods, at_lods, cluster_at * sizeof(geometry::ClusterLodDesc),
       u64{nc} * sizeof(geometry::ClusterLodDesc));
  copy(quantized, at_quant, v0 * 6, nv * 6);
  copy(attributes, at_attributes, v0 * sizeof(geometry::VertexAttributes),
       nv * sizeof(geometry::VertexAttributes));
  copy(triangles, at_triangles, t0 * sizeof(u32), nt * sizeof(u32));
  if (ray_tracing_) {
    copy(vertices, at_vertices, v0 * sizeof(Vec3), nv * sizeof(Vec3));
    copy(indices8, at_indices, t0 * 3, nt * 3);
  }
  if (!in_ring) pending_staging_.push_back(staging);
  pending_geometry_bytes_ += total;
  slot.quant = Vec4{mesh.quant_origin, mesh.quant_scale};
  slot.clusters = nc;
  slot.loaded = true;
  slot.on = false;
  ++terrain_chunk_uploads_;
  terrain_chunk_upload_bytes_ += total;
  chunk.slot = s;
  // The GPU scene has it: the DAG's host copy is not read again (a chunk is rebuilt from its key's
  // cells whenever the key changes), and a ring's DAGs are gigabytes on the erg.
  chunk.lod = geometry::ClusterLodMesh{};
  return true;
}

u32 GpuScene::terrain_slots(u32 level) const noexcept {
  for (const RingSlots& ring : ring_slots_)
    if (ring.level == level) return ring.slots.size();
  return 0;
}

u32 GpuScene::terrain_slot_clusters(u32 level) const noexcept {
  for (const RingSlots& ring : ring_slots_)
    if (ring.level == level) return ring.clusters_per_slot;
  return 0;
}

u32 GpuScene::terrain_free_slots(u32 level) const noexcept {
  for (const RingSlots& ring : ring_slots_)
    if (ring.level == level) return ring.free_slots.size();
  return 0;
}

GpuScene::ArenaFree GpuScene::terrain_arena_free(u32 level) const noexcept {
  ArenaFree out;
  for (const RingSlots& ring : ring_slots_) {
    if (ring.level != level) continue;
    for (const Range& r : ring.free_vertices) {
      out.vertices += r.count;
      out.largest_vertices = std::max(out.largest_vertices, r.count);
    }
    for (const Range& r : ring.free_triangles) {
      out.triangles += r.count;
      out.largest_triangles = std::max(out.largest_triangles, r.count);
    }
    out.vertex_capacity = ring.vertex_capacity;
    out.triangle_capacity = ring.triangle_capacity;
  }
  return out;
}

bool GpuScene::terrain_chunk_upload(u32 level, TerrainChunk& chunk, std::string* error) {
  for (RingSlots& ring : ring_slots_) {
    if (ring.level != level) continue;
    if (ring.free_slots.empty()) {
      if (error != nullptr) *error = "a terrain ring has no free slot for a chunk";
      return false;
    }
    const u32 s = ring.free_slots.back();
    ring.free_slots.pop_back();
    if (!stage_chunk(ring, s, chunk, error)) {
      ring.free_slots.push_back(s);
      return false;
    }
    return true;
  }
  if (error != nullptr) *error = "no terrain ring at that level";
  return false;
}

void GpuScene::terrain_chunk_show(u32 level, u32 s, bool on) noexcept {
  for (RingSlots& ring : ring_slots_) {
    if (ring.level != level || s >= ring.slots.size()) continue;
    TerrainSlot& slot = ring.slots[s];
    const u32 mesh = ring.first_mesh + s;
    slot.on = on && slot.loaded;
    gfx::MeshDesc desc = slot.desc;
    desc.quant = slot.quant;
    desc.cluster_count = slot.on ? slot.clusters : 0u;
    mesh_descs_[mesh] = desc;
    pending_mesh_writes_.push_back(mesh);
    if (!on && slot.loaded) {
      // Nothing recorded from the next frame on reads it, and the copy that reuses its ranges is
      // ordered after the frames in flight that may (the frame imports the geometry as last read
      // everywhere), so the slot and its arena ranges are free at once.
      give_range(ring.free_vertices, slot.vertices);
      give_range(ring.free_triangles, slot.triangles);
      slot.vertices = slot.triangles = Range{};
      slot.loaded = false;
      slot.clusters = 0;
      ring.free_slots.push_back(s);
    }
    return;
  }
}

bool GpuScene::create_streaming(const ResolvedSettings& resolved, std::string* error) {
  if (!streamed_) return true;
  const gfx::Device& device = *device_;
  const geometry::ClusterPages& table = data_->pages;
  // A group is the siblings a cut refines into together, and the drawing rule's residency test is
  // over the whole group; the GPU tests one cluster's page instead, which is the same thing only
  // while a group is in one page. `build_cluster_pages` guarantees it. Say so here rather than
  // draw a cracked surface if a future layout stops guaranteeing it.
  for (u32 c = 1; c < cluster_count_; ++c) {
    if (data_->lod.lod[c].group != data_->lod.lod[c - 1].group) continue;
    if (table.page_of_cluster[c] == table.page_of_cluster[c - 1]) continue;
    if (error != nullptr) {
      *error = "geometry streaming: group " + std::to_string(data_->lod.lod[c].group) +
               " spans two pages, and the GPU drawing rule tests a cluster's page for its group's";
    }
    return false;
  }
  u64 total_bytes = 0;
  Vector<u32> sizes(page_count_);
  for (u32 p = 0; p < page_count_; ++p) {
    const geometry::ClusterPageDesc& desc = table.pages[p];
    sizes[p] = desc.bytes;
    total_bytes += desc.bytes;
    slot_vertices_ = desc.vertex_count > slot_vertices_ ? desc.vertex_count : slot_vertices_;
    slot_triangles_ = desc.triangle_count > slot_triangles_ ? desc.triangle_count : slot_triangles_;
  }
  page_budget_bytes_ = resolved.settings.page_budget_bytes;
  if (page_budget_bytes_ == 0 || page_budget_bytes_ > total_bytes) page_budget_bytes_ = total_bytes;
  std::sort(sizes.begin(), sizes.end());
  u64 spent = 0;
  page_slots_ = 0;
  for (u32 p = 0; p < page_count_ && spent + sizes[p] <= page_budget_bytes_; ++p) {
    spent += sizes[p];
    ++page_slots_;
  }
  // The root pages are pinned whatever the budget says — a mesh missing one cannot be drawn at all
  // — so the pool always has room for them ([geometry](geometry.md), "The residency model").
  u32 roots = 0;
  for (u32 p = 0; p < page_count_; ++p)
    roots += (table.pages[p].flags & geometry::k_page_root) != 0 ? 1u : 0u;
  if (page_slots_ < roots) page_slots_ = roots;
  if (page_slots_ == 0) page_slots_ = 1;
  if (page_slots_ > page_count_) page_slots_ = page_count_;

  u64 largest_payload = 0;
  for (u32 p = 0; p < page_count_; ++p)
    largest_payload = std::max(largest_payload, page_payload_bytes(p));
  upload_budget_bytes_ = resolved.settings.upload_budget_bytes;
  if (upload_budget_bytes_ == 0) upload_budget_bytes_ = k_default_upload_budget;
  // A budget no page fits in would never converge, so it is raised to one page rather than
  // accepted and reported as a scene that never finishes loading.
  if (upload_budget_bytes_ < largest_payload)
    upload_budget_bytes_ = static_cast<u32>(largest_payload);
  max_requests_ = page_count_ < k_max_page_requests ? page_count_ : k_max_page_requests;

  const u64 slot_vertices = u64{page_slots_} * slot_vertices_;
  const u64 slot_triangles = u64{page_slots_} * slot_triangles_;
  constexpr gfx::BufferUsage k_pool =
      k_address | gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  const gfx::BufferUsage rt_pool = k_pool | gfx::k_build_input_usage;
  Vector<u32> page_index(cluster_count_);
  Vector<u32> child_ranges(u64{cluster_count_} * 2);
  for (u32 c = 0; c < cluster_count_; ++c) {
    page_index[c] = table.page_of_cluster[c];
    child_ranges[c * 2 + 0] = table.children[c].first_cluster;
    child_ranges[c * 2 + 1] = table.children[c].cluster_count;
  }
  bool ok =
      gfx::create_buffer(device, slot_vertices * 3 * sizeof(u16), k_pool, false, quantized,
                         error) &&
      gfx::create_buffer(device, slot_vertices * sizeof(geometry::VertexAttributes), k_pool, false,
                         attributes, error) &&
      gfx::create_buffer(device, slot_triangles * sizeof(u32), k_pool, false, triangles, error) &&
      gfx::upload_buffer(device, table.pages.data(),
                         u64{page_count_} * sizeof(geometry::ClusterPageDesc), k_storage,
                         page_table, error) &&
      gfx::upload_buffer(device, page_index.data(), u64{cluster_count_} * sizeof(u32), k_storage,
                         page_of_cluster, error) &&
      gfx::upload_buffer(device, child_ranges.data(), u64{cluster_count_} * 2 * sizeof(u32),
                         k_storage, page_children, error) &&
      // Host-visible, one region per frame slot: the host writes the residency the cull pass of
      // the *next* frame reads, and a slot is not reused until the GPU has finished the frame that
      // last had it — the same argument that makes the joint buffer safe.
      gfx::create_buffer(device, u64{page_count_} * sizeof(u32) * k_stream_slots, k_address, true,
                         residency, error) &&
      gfx::create_buffer(device, u64{page_count_} * sizeof(u32), k_address | k_transfer, false,
                         page_used, error) &&
      gfx::create_buffer(device, u64{max_requests_} * sizeof(geometry::PageRequest),
                         k_address | k_transfer, false, page_requests, error) &&
      gfx::create_buffer(device, sizeof(u32), k_address | k_transfer, false, request_count,
                         error) &&
      gfx::create_buffer(device, u64{page_count_} * sizeof(u32), k_address | k_transfer, false,
                         request_mask, error) &&
      gfx::create_buffer(device, sizeof(gfx::StreamParams) * k_stream_slots, k_address, true,
                         stream_params, error) &&
      gfx::create_buffer(device, u64{upload_budget_bytes_} * k_stream_slots,
                         gfx::BufferUsage::TransferSrc, true, page_stage, error);
  if (ok && ray_tracing_) {
    ok =
        gfx::create_buffer(device, slot_vertices * sizeof(Vec3), rt_pool, false, vertices, error) &&
        gfx::create_buffer(device, slot_triangles * 3, rt_pool, false, indices8, error);
  }
  if (!ok) return false;
  std::memset(residency.mapped, 0, u64{page_count_} * sizeof(u32) * k_stream_slots);
  // Every address but the residency array is the same in all three blocks, which is the whole
  // reason there are three: one word of the block changes per frame and nothing else does.
  auto* blocks = static_cast<gfx::StreamParams*>(stream_params.mapped);
  for (u32 slot = 0; slot < k_stream_slots; ++slot) {
    gfx::StreamParams& block = blocks[slot];
    block = gfx::StreamParams{};
    block.pages = page_table.address;
    block.page_of_cluster = page_of_cluster.address;
    block.children = page_children.address;
    block.residency = residency_slot_address(slot);
    block.used = page_used.address;
    block.requests = page_requests.address;
    block.request_count = request_count.address;
    block.request_mask = request_mask.address;
  }
  const u64 pool_bytes = quantized.size + attributes.size + triangles.size + vertices.size +
                         indices8.size + page_stage.size;
  geometry_bytes_ = total_bytes;
  stream_bytes_ = pool_bytes + page_table.size + page_of_cluster.size + page_children.size;
  ENGINE_LOG_INFO(
      log_renderer, "geometry streaming", log::field("pages", page_count_),
      log::field("slots", page_slots_), log::field("page_bytes", total_bytes),
      log::field("budget_bytes", page_budget_bytes_), log::field("pool_bytes", pool_bytes),
      log::field("table_bytes", stream_bytes_ - pool_bytes),
      log::field("slot_vertices", slot_vertices_), log::field("slot_triangles", slot_triangles_),
      log::field("upload_budget", upload_budget_bytes_));
  return true;
}

bool GpuScene::upload_materials(const ResolvedSettings& resolved, std::string* error) {
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  if (!gfx::create_sampler(device, gfx::Filter::Linear, sampler_, error)) return false;
  const u32 sampler_slot = bindless_.add_sampler(sampler_);
  // The materials' samplers, one per distinct combination a textured slot asks for, made when
  // the first slot that asks for it is met — so a scene with no textures has exactly the bindless
  // set it always had, and a thousand materials on the glTF default share one sampler.
  bool sampler_failed = false;
  auto material_sampler = [&](const geometry::TextureSampler& wanted, bool mipmapped) -> u32 {
    const u32 key = geometry::pack_texture_sampler(wanted) | (mipmapped ? 0x80000000u : 0u);
    for (const MaterialSampler& known : material_samplers_)
      if (known.key == key) return known.slot;
    MaterialSampler made;
    made.key = key;
    if (!gfx::create_sampler(device, sampler_desc(wanted, mipmapped), made.sampler, error)) {
      sampler_failed = true;
      return 0;
    }
    made.slot = bindless_.add_sampler(made.sampler);
    material_samplers_.push_back(made);
    return made.slot;
  };
  texture_bytes_ = 0;
  textures_built_ = 0;
  textures_decoded_ = 0;
  texture_references_ = 0;
  textures_shared_ = 0;
  texture_bytes_saved_ = 0;
  share_textures_ = resolved.settings.share_textures;
  transform_conflicts_ = 0;
  Vector<gfx::ResolveMaterial> material_table;
  Vector<u32> cluster_material(cluster_count_);
  Vector<u32> mesh_material_base(data_->parts.size(), 0u);
  if (data_->heightfield) {
    // A procedural ripple texture, linear-sampled through the bindless set.
    constexpr u32 k_texture_size = 256;
    Vector<u8> texels(k_texture_size * k_texture_size * 4);
    for (u32 y = 0; y < k_texture_size; ++y) {
      for (u32 x = 0; x < k_texture_size; ++x) {
        const f32 fx = static_cast<f32>(x);
        const f32 fy = static_cast<f32>(y);
        const f32 ripple = 0.5f + 0.5f * std::sin(fx * 0.25f + 2.0f * std::sin(fy * 0.08f));
        const f32 grain =
            0.5f + 0.5f * std::sin(fx * 1.7f + fy * 2.3f) * std::sin(fy * 1.1f - fx * 0.7f);
        const u8 v = static_cast<u8>((0.62f + 0.3f * ripple + 0.08f * grain) * 255.0f);
        u8* t = &texels[(y * k_texture_size + x) * 4];
        t[0] = t[1] = t[2] = v;
        t[3] = 255;
      }
    }
    if (!gfx::upload_image_2d(device, k_texture_size, k_texture_size, gfx::Format::R8G8B8A8Unorm,
                              texels.data(), texels.size(), procedural_texture_, error) ||
        !gfx::create_image_view(device, procedural_texture_, procedural_view_, error)) {
      return false;
    }
    texture_bytes_ += procedural_texture_.bytes;
    const u32 texture_slot =
        bindless_.add_sampled_image(procedural_view_, gfx::ImageLayout::ShaderReadOnly);
    // Materials: a flat table indexed per cluster by the height band of the cluster's center.
    material_table.resize(3);
    material_table[0].albedo = Vec4{0.86f, 0.72f, 0.46f, 0.9f};  // sand
    material_table[1].albedo = Vec4{0.42f, 0.40f, 0.38f, 0.7f};  // rock
    material_table[2].albedo = Vec4{0.92f, 0.94f, 0.97f, 0.4f};  // snow
    for (u32 i = 0; i < 2; ++i) {  // sand and rock carry the ripple texture at different scales
      material_table[i].albedo_texture = texture_slot;
      material_table[i].sampler = sampler_slot;
      material_table[i].uv_scale = i == 0 ? 24.0f : 9.0f;
    }
    for (u32 i = 0; i < cluster_count_; ++i) {
      const f32 y = lod.mesh.clusters[i].center.y;
      cluster_material[i] = y < -0.15f ? 0u : y < 0.65f ? 1u : 2u;
    }
  } else {
    // Materials from the files, one mesh's table after the last: every instance adds its mesh's
    // base to the cluster's material index, so the clusters keep mesh-local indices. Images are
    // decoded on the CPU (embedded bytes or a file beside the glTF) and uploaded once each, into
    // one bindless slot every material which names the image shares — every material of the
    // scene, below, not only of the mesh; an image that fails to decode leaves its slot empty with
    // a warning. Base color is color and goes up as sRGB, so that sampling returns linear;
    // metallic-roughness and normal maps are data, not color, and go up UNORM. A glTF never gives
    // one image both roles, so the format an image is first asked for is the one it keeps.
    //
    // **Built textures** (docs/subsystems/texture.md, "In the renderer"): when the content build
    // has made every image this mesh's materials sample into a block-compressed, mipmapped `.tex`
    // in the derived-data root — which the container's records name — those are uploaded as they
    // are stored, with the whole chain, and sampled through the mipmapping sampler with the UV
    // derivatives the resolve computes (`k_material_mipped`). Otherwise the mesh takes exactly the
    // path above: decode, one level, level-0 sampling. All or nothing per mesh, so no material
    // mixes the two samplers.
    //
    // **One upload per distinct image per scene** (docs/subsystems/renderer.md): a mesh whose image
    // has the content of one an earlier mesh uploaded — the same `.tex` key built, the same bytes
    // and format decoded — takes that texture's slot. A kit's nineteen members embedding one atlas
    // upload it once, not nineteen times; the picture is the same bytes, because the texel data
    // each material reads is.
    SceneTextures scene_textures(share_textures_);
    for (u32 m = 0; m < data_->sources.size(); ++m) {
      const SourceMesh& source_mesh = data_->sources[m];
      const assets::MeshData& mesh_data = source_mesh.data;
      mesh_material_base[m] = material_table.size();
      Vector<BuiltImage> built;
      u32 sampled_images = 0;
      std::string why;
      bool use_built = find_built_textures(source_mesh, scene_textures, built, sampled_images, why);
      if (use_built && !device.features().texture_compression_bc) {
        use_built = false;
        why = "the device has no textureCompressionBC";
        ENGINE_LOG_WARN(log_renderer, "built textures unusable", log::field("mesh", m),
                        log::field("reason", why));
      }
      if (sampled_images != 0) {
        ENGINE_LOG_INFO(log_renderer, "mesh textures", log::field("mesh", m),
                        log::field("from", use_built ? "built" : "decoded"),
                        log::field("images", sampled_images), log::field("reason", why));
      }
      Vector<u32> image_slot(mesh_data.images.size(), gfx::k_no_texture);
      Vector<bool> image_tried(mesh_data.images.size(), false);
      const std::string& mesh_dir = source_mesh.image_dir;  // the glTF's or the container's
      auto texture_slot_of = [&](i32 image_index, gfx::Format format) -> u32 {
        if (image_index < 0 || static_cast<u32>(image_index) >= mesh_data.images.size())
          return gfx::k_no_texture;
        const u32 index = static_cast<u32>(image_index);
        if (image_tried[index]) return image_slot[index];
        image_tried[index] = true;
        const assets::ImageRef& ref = mesh_data.images[index];
        gfx::ImageResource uploaded;
        gfx::ImageViewHandle view;
        std::string image_error;
        bool ok = false;
        // The texture's content, and the scene's texture of that content if it has one yet.
        SceneTexture made;
        made.form = use_built ? k_form_built : static_cast<u32>(format);
        auto share = [&]() -> const SceneTexture* {
          const SceneTexture* held = scene_textures.find(made.content, made.form);
          if (held == nullptr) return nullptr;
          ++texture_references_;
          ++textures_shared_;
          texture_bytes_saved_ += held->bytes;
          image_slot[index] = held->slot;
          return held;
        };
        if (use_built) {
          made.content = built[index].key;
          made.stored = built[index].stored;
          if (share() != nullptr) return image_slot[index];
          // As stored: the blocks of every level, one staging copy, no decode.
          const texture::TextureData& t = built[index].data;
          Vector<gfx::ImageLevelData> levels;
          levels.reserve(t.levels.size());
          for (u32 l = 0; l < t.levels.size(); ++l) {
            const std::span<const u8> bytes = t.level_bytes(l);
            levels.push_back(gfx::ImageLevelData{bytes.data(), bytes.size()});
          }
          ok = gfx::upload_image_2d_levels(
                   device, t.width, t.height, texture_gpu_format(t),
                   std::span<const gfx::ImageLevelData>(levels.data(), levels.size()), uploaded,
                   &image_error) &&
               gfx::create_image_view(device, uploaded, view, &image_error);
          if (!ok && uploaded.image.valid()) gfx::destroy_image(device, uploaded);
          if (ok) ++textures_built_;
        } else {
          // The encoded bytes — embedded, or the file as it is now — are what a decoded texture
          // is keyed by, so they are read and hashed before anything is decoded: a repeat costs
          // the read and the hash, not the decode and the upload. `image::read_image` is exactly
          // this read followed by `decode_image`, so the pixels are the ones it gave.
          std::string file_bytes;
          std::span<const u8> encoded;
          if (!ref.bytes.empty()) {
            encoded = std::span<const u8>(ref.bytes.data(), ref.bytes.size());
            ok = true;
          } else if (!ref.uri.empty()) {
            const std::string path = mesh_dir.empty() ? ref.uri : io::join_path(mesh_dir, ref.uri);
            const io::Status status = io::read_file(path, file_bytes);
            ok = status == io::Status::Ok;
            if (ok) {
              encoded = std::span<const u8>(reinterpret_cast<const u8*>(file_bytes.data()),
                                            file_bytes.size());
            } else {
              image_error = "read_image: " + path + ": " + io::status_name(status);
            }
          } else {
            image_error = "image has neither bytes nor a uri";
          }
          image::Image decoded;
          if (ok) {
            made.content = hash_bytes(encoded.data(), encoded.size());
            if (share() != nullptr) return image_slot[index];
            ok = image::decode_image(encoded, decoded, 4, &image_error);
          }
          if (ok && (!gfx::upload_image_2d(device, decoded.width, decoded.height, format,
                                           decoded.pixels.data(), decoded.pixels.size(), uploaded,
                                           &image_error) ||
                     !gfx::create_image_view(device, uploaded, view, &image_error))) {
            if (uploaded.image.valid()) gfx::destroy_image(device, uploaded);
            ok = false;
          }
          if (ok) ++textures_decoded_;
        }
        if (!ok) {
          ENGINE_LOG_WARN(log_renderer, "texture skipped", log::field("image", index),
                          log::field("name", ref.name), log::field("error", image_error));
          return gfx::k_no_texture;
        }
        texture_bytes_ += uploaded.bytes;
        ++texture_references_;
        textures_.push_back(uploaded);
        texture_views_.push_back(view);
        image_slot[index] = bindless_.add_sampled_image(view, gfx::ImageLayout::ShaderReadOnly);
        made.slot = image_slot[index];
        made.bytes = uploaded.bytes;
        scene_textures.add(made);
        return image_slot[index];
      };
      for (const assets::Material& source : mesh_data.materials) {
        gfx::ResolveMaterial material;
        material.albedo =
            Vec4{source.base_color.x, source.base_color.y, source.base_color.z, source.roughness};
        // The emissive factor, which the emissive texture multiplies where there is one.
        material.emissive = Vec4{source.emissive, source.metallic};
        // Colour goes up sRGB and data UNORM; the order is the order an image shared by two slots
        // keeps the format of (base colour first, as it always was).
        material.albedo_texture =
            texture_slot_of(source.base_color_image, gfx::Format::R8G8B8A8Srgb);
        material.metallic_roughness_texture =
            texture_slot_of(source.metallic_roughness_image, gfx::Format::R8G8B8A8Unorm);
        material.normal_texture = texture_slot_of(source.normal_image, gfx::Format::R8G8B8A8Unorm);
        material.occlusion_texture =
            texture_slot_of(source.occlusion_image, gfx::Format::R8G8B8A8Unorm);
        material.emissive_texture =
            texture_slot_of(source.emissive_image, gfx::Format::R8G8B8A8Srgb);
        material.normal_scale = source.normal_scale;
        material.occlusion_strength = source.occlusion_strength;
        material.uv_scale = 1.0f;
        // The terrain's material draws the sand's detail when its scene asks for it, weighted by
        // the sand share its metallic-roughness map carries in alpha (renderer.md, "The sand close
        // up"). The rings' slots draw with this material too.
        if (m == data_->terrain_mesh && data_->terrain.enabled && data_->terrain.has_detail)
          material.flags |= gfx::k_material_ground_detail;
        if (use_built) {
          material.flags |= gfx::k_material_mipped;
          // A BC5 normal map holds x and y; the shader rebuilds z (material.slang).
          const i32 normal = source.normal_image;
          if (normal >= 0 && static_cast<u32>(normal) < built.size() &&
              built[static_cast<u32>(normal)].stored == texture::TextureFormat::bc5) {
            material.flags |= gfx::k_material_normal_rg;
          }
        }
        // Each textured slot's own sampler, and one UV transform for the material: the base
        // colour's where it is textured, the first textured slot's otherwise. A slot with no
        // texture asks for no sampler and borrows `sampler` (gfx::k_same_sampler), which it never
        // reads.
        const u32 slot_texture[geometry::k_material_slots] = {
            material.albedo_texture, material.metallic_roughness_texture, material.normal_texture,
            material.occlusion_texture, material.emissive_texture};
        u32 slot_sampler[geometry::k_material_slots];
        i32 transform_slot = -1;
        for (u32 s = 0; s < geometry::k_material_slots; ++s) {
          slot_sampler[s] = gfx::k_same_sampler;
          if (slot_texture[s] == gfx::k_no_texture) continue;
          slot_sampler[s] = material_sampler(source.sampling[s].sampler, use_built);
          if (transform_slot < 0) transform_slot = static_cast<i32>(s);
        }
        if (sampler_failed) return false;
        material.sampler = slot_sampler[geometry::k_slot_base_color] != gfx::k_same_sampler
                               ? slot_sampler[geometry::k_slot_base_color]
                               : (transform_slot >= 0 ? slot_sampler[transform_slot] : 0u);
        material.samplers_mr_normal = slot_sampler[geometry::k_slot_metallic_roughness] |
                                      (slot_sampler[geometry::k_slot_normal] << 16);
        material.samplers_occlusion_emissive = slot_sampler[geometry::k_slot_occlusion] |
                                               (slot_sampler[geometry::k_slot_emissive] << 16);
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
        material_table.push_back(material);
      }
      const u32 local_count = material_table.size() - mesh_material_base[m];
      gfx::ResolveMaterial plain;
      plain.albedo = Vec4{0.8f, 0.8f, 0.8f, 0.6f};
      material_table.push_back(plain);  // the default for a primitive that names no material
      const geometry::ClusterMeshPart& part = data_->parts[m];
      for (u32 i = 0; i < part.cluster_count; ++i) {
        const i32 material = source_mesh.part_material[source_mesh.part_of_cluster[i]];
        cluster_material[part.first_cluster + i] =
            material >= 0 && static_cast<u32>(material) < local_count ? static_cast<u32>(material)
                                                                      : local_count;
      }
    }
  }
  if (texture_references_ != 0) {
    ENGINE_LOG_INFO(
        log_renderer, "scene textures", log::field("distinct", textures_built_ + textures_decoded_),
        log::field("references", texture_references_), log::field("shared", textures_shared_),
        log::field("bytes", texture_bytes_), log::field("bytes_saved", texture_bytes_saved_),
        log::field("sharing", resolved.settings.share_textures));
  }
  if (!material_samplers_.empty() || transform_conflicts_ != 0) {
    ENGINE_LOG_INFO(log_renderer, "material samplers",
                    log::field("samplers", material_samplers_.size()),
                    log::field("materials", material_table.size()),
                    log::field("transform_conflicts", transform_conflicts_));
  }
  if (transform_conflicts_ != 0) {
    ENGINE_LOG_WARN(log_renderer, "texture transforms differ within a material",
                    log::field("materials", transform_conflicts_),
                    log::field("drawn_with", "the base colour's, or the first textured slot's"));
  }
  // A ring slot draws with the terrain's own material — the maps over the terrain's UVs, which a
  // ring's chunks carry in the terrain's frame — at material 0 of each of its clusters (the
  // table's zeros), from the terrain mesh's base.
  if (!slot_parts_.empty()) {
    const u32 terrain_base =
        data_->terrain_mesh < data_->parts.size() ? mesh_material_base[data_->terrain_mesh] : 0u;
    mesh_material_base.resize(data_->parts.size() + slot_parts_.size(), terrain_base);
  }
  // Now that the tables are laid out, every instance knows where its mesh's materials start — and
  // so does every instance a dynamic scene takes later, which is why the table is kept.
  for (gfx::InstanceDesc& instance : instance_table_)
    instance.material_base = mesh_material_base[instance.mesh];
  mesh_material_base_ = mesh_material_base;
  material_count_ = material_table.size();
  // A dynamic scene's instance table is a set per frame in flight, made by `create` once the mesh
  // table (and its null mesh) exists.
  return gfx::upload_buffer(device, material_table.data(),
                            u64{material_count_} * sizeof(gfx::ResolveMaterial), k_storage,
                            materials, error) &&
         gfx::upload_buffer(device, cluster_material.data(), u64{cluster_count_} * sizeof(u32),
                            k_storage, cluster_materials, error) &&
         (dynamic_ || gfx::upload_buffer(device, instance_table_.data(),
                                         u64{instance_count_} * sizeof(gfx::InstanceDesc),
                                         k_storage, instances, error));
}

bool GpuScene::create_working_set(const ResolvedSettings& resolved, std::string* error) {
  const gfx::Device& device = *device_;
  // One visible list for the whole frame, in three runs per view: the hardware pass 1, the
  // hardware pass 2, and the software rasterizer. A visibility id names an entry of the whole
  // list, so the resolve of any view reads one array however many draws of however many views
  // filled it, and each draw's `visible_offset` is where its run starts. Every run is as long as
  // the pair count, which is as many entries as any one draw can produce.
  //
  // Run-major (`visible_base`): run r of view v starts at `(r * views + v) * pair_count`, so the
  // first run of every view is one contiguous range at the front. That range is what the ray
  // tracing chain builds the union of the views' cuts from in a single dispatch.
  //
  // Every run is `pair_stride_` long: the pair count for a scene read whole, a capacity above it
  // for one whose instances come and go (`set_dynamic_instances`), so a tail that changes length
  // moves no run.
  const u64 visible_entry_bytes = 2 * sizeof(u32);
  visible_run_bytes_ = u64{pair_stride_} * visible_entry_bytes;
  // The visibility id names a pair, not an entry (docs/subsystems/gfx.md, "The tie rule"), so the
  // resolve reads the entry a pair was drawn as out of this: one word per pair per view, written
  // by the cull pass for every pair it draws. A pair is drawn at most once per view per frame, so
  // one table per view serves every run, and a word from an earlier frame is never read.
  // A shadow cascade's run follows every view's three (`cascade_base`), with an argument block of
  // its own.
  bool ok = gfx::create_buffer(device,
                               visible_run_bytes_ * (k_visible_runs * view_count_ + cascade_count_),
                               k_readable, false, visible, error) &&
            (cascade_count_ == 0 ||
             gfx::create_buffer(device, u64{gfx::k_draw_args_bytes} * cascade_count_, k_args, false,
                                shadow_args, error)) &&
            gfx::create_buffer(device, u64{pair_stride_} * view_count_ * sizeof(u32), k_address,
                               false, pair_entries, error) &&
            gfx::create_buffer(device, u64{gfx::k_draw_args_bytes} * view_count_, k_args, false,
                               sw_args, error);
  for (u32 i = 0; i < 2; ++i) {
    ok = ok &&
         gfx::create_buffer(device, u64{gfx::k_draw_args_bytes} * view_count_, k_args, false,
                            draw_args[i], error) &&
         gfx::create_buffer(device, u64{pair_stride_} * view_count_ * sizeof(u32),
                            k_address | gfx::BufferUsage::TransferDst, false, flags[i], error);
  }
  // The vertex path's indexed draw: for each view and each of the two hardware runs, run-major like
  // the visible list, a header, a record per pair and an index array. The array holds
  // the scene's own bound — every triangle of every pair, which no cut can exceed — or the budget,
  // whichever is smaller; past the budget a survivor is drawn by the fallback, not dropped.
  if (ok && resolved.vertex_indexed) {
    const geometry::ClusterLodMesh& lod = data_->lod;
    Vector<u64> mesh_triangles(data_->parts.size() + slot_parts_.size(), 0);
    for (u32 m = 0; m < data_->parts.size(); ++m) {
      const geometry::ClusterMeshPart& part = data_->parts[m];
      for (u32 c = 0; c < part.cluster_count; ++c)
        mesh_triangles[m] += lod.mesh.clusters[part.first_cluster + c].triangle_count;
    }
    // A ring slot may draw any chunk it has room for: its clusters, full.
    for (u32 k = 0; k < slot_parts_.size(); ++k)
      mesh_triangles[data_->parts.size() + k] =
          u64{slot_parts_[k].cluster_count} * triangles_per_cluster_;
    // A scene whose instances come and go has no bound its load could know: the budget, which a
    // cut past it survives through the fallback draw rather than by dropping anything. (Its table
    // also holds the null instances of its holes, whose mesh is past the scene's.)
    u64 bound = k_vertex_index_budget;
    if (!dynamic_) {
      bound = 0;
      for (const gfx::InstanceDesc& instance : instance_table_)
        bound += mesh_triangles[instance.mesh];
    }
    vertex_index_capacity_ =
        static_cast<u32>(bound < k_vertex_index_budget ? bound : k_vertex_index_budget);
    if (vertex_index_capacity_ == 0) vertex_index_capacity_ = 1;  // a scene of no triangles
    // Two hardware runs a view, and one region per shadow cascade behind them: a cascade's cut is
    // the camera's LOD over the cascade's box, drawn with the same indexed draw.
    const u64 runs = u64{2} * view_count_ + cascade_count_;
    ok = gfx::create_buffer(device, runs * sizeof(gfx::VertexDrawHeader), k_args, false,
                            vertex_headers, error) &&
         gfx::create_buffer(device, runs * pair_stride_ * sizeof(gfx::VertexDrawRecord), k_address,
                            false, vertex_records, error) &&
         gfx::create_buffer(device, runs * vertex_index_capacity_ * gfx::k_vertex_draw_index_bytes,
                            k_address | gfx::BufferUsage::Index, false, vertex_indices, error);
  }
  return ok;
}

bool GpuScene::create_ray_tracing(const ResolvedSettings& resolved, std::string* error) {
  if (!ray_tracing_) return true;
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  // **What a frame could build is `views * pairs`; what it does build is its cut.** A pair's base
  // geometry index is its entry in the visible list, so the largest geometry index is the last
  // entry of the views' first runs, which run-major ordering puts at `views * pair_count - 1`, and
  // every per-frame set is created able to *name* any of them. What it is created able to *hold*
  // is the capacity (docs/subsystems/renderer.md, "The ray tracing chain's memory"): until
  // 2026-09-24 that was the union too, 6,144 bytes of cluster structure reserved for every pair in
  // every view — 16.2 GB for the desert overlook's landmarks on a surround, whose frames build at
  // most 43,291 clusters. Now it is what `SceneRenderer` measures the frames to need.
  rt_union_clusters_ = pair_count_ * view_count_;
  Vector<u8> packed;
  // Under streaming the 8-bit indices are one more page-pool stream, filled a page at a time
  // beside the float positions, because a CLAS record addresses them at the same slot-relative
  // `triangle_offset` the rasterizers read.
  if (!streamed_) {
    gfx::pack_cluster_indices(
        std::span<const u32>(lod.mesh.triangles.data(), lod.mesh.triangles.size()), packed);
  }
  gfx::ClusterSetLimits limits;
  limits.max_clusters = rt_union_clusters_;
  limits.max_triangles_per_cluster = triangles_per_cluster_;
  limits.max_vertices_per_cluster = geometry::ClusterLodOptions{}.max_vertices;
  // A shadow caster's geometry index is its visible index too, and the casters are the list's run
  // `k_caster_run`, so with them the largest index is that run's last entry. The record count is
  // unchanged: in one view a pair is drawn or a caster, never both, so the union of the views'
  // runs still holds at most `views * pair_count` clusters.
  //
  // **The device bounds that index, and the frame's size does not move it** (renderer.md, "The
  // ray tracing chain's index space"). `resolve_settings` drops the casters, or the chain, when
  // the scene's pairs in every view would pass `cluster_max_geometry_index`; a caller that went
  // around it is refused here with both numbers, where it used to learn only that "the driver gave
  // no sizes" — which is what the ashlar ruins on a three-view surround said, at any resolution.
  const u64 max_index = rt_max_geometry_index(pair_count_, view_count_, resolved.casters);
  gfx::ClusterAsProperties cluster_props;
  const u32 device_max =
      gfx::cluster_as_properties(device, cluster_props) ? cluster_props.max_geometry_index : 0u;
  if (max_index > device_max) {
    if (error != nullptr) {
      *error = "the scene's " + std::to_string(pair_count_) + " pairs in " +
               std::to_string(view_count_) + " view(s)" +
               (resolved.casters ? " with shadow casters" : "") +
               " name cluster geometry indices up to " + std::to_string(max_index) +
               "; the device allows " + std::to_string(device_max) +
               " (resolve_settings drops the casters or the chain for such a scene)";
    }
    return false;
  }
  limits.max_geometry_index = static_cast<u32>(max_index);
  limits.instantiate = resolved.settings.rt_templates;
  rt_limits_ = limits;
  // One instance's structure can reference at most its mesh's clusters in every view (a ring
  // slot's: the clusters it has room for).
  rt_max_per_instance_ = 0;
  for (const gfx::InstanceDesc& instance : instance_table_) {
    rt_max_per_instance_ =
        std::max(rt_max_per_instance_, part_of(instance.mesh).cluster_count * view_count_);
  }

  constexpr gfx::BufferUsage k_record_usage = k_address | gfx::BufferUsage::AccelerationBuildInput;
  // The top-level instance records, once: world transform, the instance as the custom index, and
  // a bottom-level address of zero, which the frame's `tlas_references` dispatch overwrites from
  // `blas_set.addresses` before every top-level build. An instance whose structure a frame did not
  // build keeps a zero there, which the top-level build treats as an inactive instance.
  Vector<gfx::TlasInstance> tlas_records;
  tlas_records.reserve(instance_count_);
  for (u32 i = 0; i < instance_count_; ++i) {
    gfx::TlasInstance record;
    record.transform = instance_table_[i].world;
    record.custom_index = i;
    record.blas = 0;
    tlas_records.push_back(record);
  }
  Vector<u8> tlas_bytes(u64{instance_count_} * gfx::k_instance_record_bytes, u8{0});
  gfx::write_instances(std::span<const gfx::TlasInstance>(tlas_records.data(), tlas_records.size()),
                       tlas_bytes.data());
  bool ok = (streamed_ || upload_room(device, packed.data(), packed.size(),
                                      ring_slots_.empty() ? 0 : triangle_capacity_ * 3 + 4,
                                      gfx::k_build_input_usage, indices8, error)) &&
            gfx::create_buffer(device, sizeof(u32) * gfx::k_cluster_record_count_words,
                               k_record_usage | gfx::BufferUsage::TransferSrc, false, record_count,
                               error) &&
            gfx::create_buffer(device, u64{rt_union_clusters_} * sizeof(u32), k_address, false,
                               slots, error) &&
            // Three words an instance: its drawn clusters, its casters, then its dense record base
            // (`gfx::ClusterRecordParams::instance_counts`).
            gfx::create_buffer(device, u64{instance_count_} * 3 * sizeof(u32),
                               k_address | gfx::BufferUsage::TransferDst, false, instance_counts,
                               error) &&
            gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * instance_count_,
                               k_record_usage, false, blas_records, error) &&
            gfx::create_tlas(device, instance_count_, gfx::k_build_fast_trace, tlas, error) &&
            gfx::upload_buffer(device, tlas_bytes.data(), tlas_bytes.size(),
                               gfx::k_build_input_usage | gfx::BufferUsage::TransferDst | k_storage,
                               rt_instances, error);
  // One template per cluster of the scene, built from the rest pose, with cluster id and base
  // geometry index zero so an instantiate record's offsets are the visible entry outright. Built
  // once, **packed to the size each one is** rather than to the driver's worst case (about 2.2 KB
  // reserved against 504–532 bytes used on the desert overlook), and instantiated from the pool
  // every frame.
  if (ok && resolved.settings.rt_templates) {
    gfx::ClusterSetLimits template_limits = limits;
    template_limits.max_clusters = cluster_count_;
    template_limits.max_geometry_index = 0;
    template_limits.instantiate = false;
    gfx::BufferResource template_records;
    ok = gfx::create_buffer(device, gfx::k_cluster_template_record_bytes * cluster_count_,
                            k_record_usage, true, template_records, error);
    if (ok) {
      Vector<gfx::ClusterBuildInput> template_inputs;
      template_inputs.reserve(cluster_count_);
      for (u32 c = 0; c < cluster_count_; ++c) {
        const geometry::ClusterDesc& desc = lod.mesh.clusters[c];
        gfx::ClusterBuildInput in;
        in.cluster_id = 0;  // the instantiate record's offsets carry the visible entry
        in.triangle_count = desc.triangle_count;
        in.vertex_count = desc.vertex_count;
        in.vertices = vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
        in.indices = indices8.address + u64{desc.triangle_offset} * 3;
        template_inputs.push_back(in);
      }
      gfx::write_cluster_template_records(
          std::span<const gfx::ClusterBuildInput>(template_inputs.data(), template_inputs.size()),
          template_records.mapped);
      ok = gfx::create_packed_cluster_templates(device, template_limits, template_records.address,
                                                clas_templates, error);
    }
    gfx::destroy_buffer(device, template_records);
    if (ok) {
      const auto* sizes = static_cast<const u32*>(clas_templates.sizes.mapped);
      for (u32 c = 0; c < cluster_count_; ++c)
        template_bytes_ += sizes[c];
    }
  }
  if (ok) {
    tlas_slot_ = bindless_.add_acceleration_structure(tlas.handle);
    if (tlas_slot_ == gfx::BindlessSet::k_invalid_slot) {
      ok = false;
      if (error != nullptr) *error = "no bindless slot for the top-level structure";
    }
  }
  if (!ok) return false;
  rt_scene_bytes_ = indices8.size + record_count.size + slots.size + instance_counts.size +
                    blas_records.size + tlas.buffer.size + rt_instances.size +
                    clas_templates.data.size + clas_templates.addresses.size +
                    clas_templates.sizes.size;

  // **The budget, in clusters.** What one cluster of capacity costs comes from the driver's own
  // sizes at a probe count — the structure, its address and size words, its record, its share of
  // the bottom-level set and of the scratch the builds share — so the byte budget the caller gave
  // becomes a count the capacity policy can reason in. Linear in the count, which is how the
  // driver reserves them (6,144 bytes a cluster of 124 triangles and 64 vertices on the RTX 5090).
  constexpr u32 k_probe = 65536;
  gfx::ClusterSetLimits probe = limits;
  probe.max_clusters = k_probe;
  gfx::ClusterBuildSizes set_sizes;
  gfx::ClusterBuildSizes blas_sizes;
  if (!gfx::cluster_set_build_sizes(device, probe, set_sizes) ||
      !gfx::cluster_blas_set_build_sizes(device, instance_count_, k_probe,
                                         std::min(rt_max_per_instance_, k_probe), blas_sizes)) {
    if (error != nullptr) {
      *error = "the driver gave no cluster acceleration structure sizes for " +
               std::to_string(k_probe) + " clusters over " + std::to_string(instance_count_) +
               " bottom-level structures of at most " +
               std::to_string(std::min(rt_max_per_instance_, k_probe)) + " each";
    }
    return false;
  }
  const u64 record_bytes = resolved.settings.rt_templates ? gfx::k_cluster_instantiate_record_bytes
                                                          : gfx::k_cluster_build_record_bytes;
  const u64 probe_bytes = set_sizes.data_bytes + blas_sizes.data_bytes +
                          std::max(set_sizes.scratch_bytes, blas_sizes.scratch_bytes) +
                          u64{k_probe} * (record_bytes + sizeof(u64) + sizeof(u32));
  rt_bytes_per_cluster_ = (probe_bytes + k_probe - 1) / k_probe;
  const u32 budget_mib = resolved.settings.rt_budget_mib != 0 ? resolved.settings.rt_budget_mib
                                                              : rt_budget_mib_tunable();
  const u64 budget_clusters = u64{budget_mib} * 1024 * 1024 / rt_bytes_per_cluster_;
  rt_capacity_limit_ =
      static_cast<u32>(std::max<u64>(1, std::min<u64>(budget_clusters, u64{rt_union_clusters_})));
  // The first capacity, before any frame has said what it builds: the whole union when that fits
  // (which is every scene the tests draw, and so every picture they pin), `k_initial_rt_clusters`
  // otherwise. The renderer grows or shrinks it from the frames' own demand.
  const u32 initial = std::min(rt_capacity_limit_, k_initial_rt_clusters);
  if (!resize_ray_tracing(rt_union_clusters_ <= initial ? rt_union_clusters_ : initial, error)) {
    return false;
  }
  ENGINE_LOG_INFO(
      log_renderer, "ray tracing ready", log::field("pairs", pair_count_),
      log::field("views", view_count_), log::field("union_clusters", rt_union_clusters_),
      log::field("instances", instance_count_), log::field("capacity", rt_capacity_),
      log::field("capacity_limit", rt_capacity_limit_), log::field("budget_mib", budget_mib),
      log::field("bytes_per_cluster", rt_bytes_per_cluster_),
      log::field("clas_bytes", clas_set.data.size), log::field("blas_bytes", blas_set.data.size),
      log::field("scratch_bytes", rt_scratch.size), log::field("rt_bytes", rt_bytes_),
      log::field("templates", resolved.settings.rt_templates ? cluster_count_ : 0u),
      log::field("template_bytes", template_bytes_),
      log::field("template_storage", clas_templates.data.size));
  return true;
}

bool GpuScene::resize_ray_tracing(u32 capacity, std::string* error, bool beyond_budget) {
  if (!ray_tracing_ || device_ == nullptr) return true;
  const gfx::Device& device = *device_;
  const u32 ceiling =
      beyond_budget || rt_capacity_limit_ == 0 ? rt_union_clusters_ : rt_capacity_limit_;
  capacity = std::max(1u, std::min(capacity, ceiling));
  gfx::destroy_buffer(device, records);
  gfx::destroy_cluster_set(device, clas_set);
  gfx::destroy_cluster_blas_set(device, blas_set);
  gfx::destroy_buffer(device, rt_scratch);
  rt_capacity_ = 0;
  gfx::ClusterSetLimits limits = rt_limits_;
  limits.max_clusters = capacity;
  constexpr gfx::BufferUsage k_record_usage = k_address | gfx::BufferUsage::AccelerationBuildInput;
  const u64 record_bytes = limits.instantiate ? gfx::k_cluster_instantiate_record_bytes
                                              : gfx::k_cluster_build_record_bytes;
  bool ok =
      gfx::create_buffer(device, record_bytes * capacity, k_record_usage, false, records, error) &&
      gfx::create_cluster_set(device, limits, clas_set, error) &&
      gfx::create_cluster_blas_set(device, instance_count_, capacity,
                                   std::min(rt_max_per_instance_, capacity), blas_set, error);
  if (ok) {
    u64 scratch_bytes = clas_set.build_scratch_bytes;
    scratch_bytes = std::max(scratch_bytes, blas_set.build_scratch_bytes);
    scratch_bytes = std::max(scratch_bytes, tlas.build_scratch_bytes);
    ok = gfx::create_scratch(device, scratch_bytes, rt_scratch, error);
  }
  if (!ok) {
    gfx::destroy_buffer(device, records);
    gfx::destroy_cluster_set(device, clas_set);
    gfx::destroy_cluster_blas_set(device, blas_set);
    gfx::destroy_buffer(device, rt_scratch);
    rt_bytes_ = rt_scene_bytes_;
    return false;
  }
  rt_capacity_ = capacity;
  rt_bytes_ = rt_scene_bytes_ + records.size + clas_set.data.size + clas_set.addresses.size +
              clas_set.sizes.size + blas_set.data.size + blas_set.addresses.size + rt_scratch.size;
  return true;
}

gfx::GroundDetailParams GpuScene::ground_detail_params() const noexcept {
  if (ground_ == nullptr) return gfx::GroundDetailParams{};
  const TerrainDesc& terrain = data_->terrain;
  const gfx::GroundDetailDesc desc = terrain_detail_desc(terrain);
  gfx::GroundDetailParams block =
      gfx::ground_detail_block(desc, ground_->wind(ground_time_s_), terrain.seed);
  // The ripples' motion (renderer.md, "Ripples that move"), from the two times alone.
  terrain_detail_motion(*ground_, desc, ground_time_s_, ground_previous_s_, block);
  return block;
}

void GpuScene::destroy() noexcept {
  ground_.reset();
  ground_time_s_ = 0.0;
  ground_previous_s_ = 0.0;
  if (device_ == nullptr) return;
  const gfx::Device& device = *device_;
  // A dynamic scene's `instances` and `pair_table` name one of its table sets, which own them.
  if (!sets_.empty()) {
    instances = gfx::BufferResource{};
    pair_table = gfx::BufferResource{};
  }
  for (TableSet& set : sets_) {
    gfx::destroy_buffer(device, set.instances);
    gfx::destroy_buffer(device, set.pairs);
  }
  sets_.clear();
  for (gfx::BufferResource& staging : table_staging_)
    gfx::destroy_buffer(device, staging);
  table_staging_.clear();
  gfx::destroy_buffer(device, staging_ring_);
  ring_head_ = ring_tail_ = 0;
  for (u64& end : ring_slot_end_)
    end = 0;
  ring_peak_ = ring_overflows_ = ring_overflow_bytes_ = 0;
  current_set_ = 0;
  tables_changed_ = false;
  layout_.reset(0, 0);
  tail_keys_.clear();
  requests_.clear();
  writes_.clear();
  compactions_ = 0;
  null_mesh_ = 0;
  gfx::destroy_buffer(device, meshes);
  gfx::destroy_cluster_templates(device, clas_templates);
  gfx::destroy_acceleration_structure(device, tlas);
  gfx::destroy_cluster_blas_set(device, blas_set);
  gfx::destroy_cluster_set(device, clas_set);
  gfx::destroy_buffer(device, rt_scratch);
  gfx::destroy_buffer(device, rt_instances);
  gfx::destroy_buffer(device, blas_records);
  gfx::destroy_buffer(device, instance_counts);
  gfx::destroy_buffer(device, slots);
  gfx::destroy_buffer(device, record_count);
  gfx::destroy_buffer(device, records);
  gfx::destroy_buffer(device, indices8);
  gfx::destroy_buffer(device, page_stage);
  gfx::destroy_buffer(device, stream_params);
  gfx::destroy_buffer(device, request_mask);
  gfx::destroy_buffer(device, request_count);
  gfx::destroy_buffer(device, page_requests);
  gfx::destroy_buffer(device, page_used);
  gfx::destroy_buffer(device, residency);
  gfx::destroy_buffer(device, page_children);
  gfx::destroy_buffer(device, page_of_cluster);
  gfx::destroy_buffer(device, page_table);
  gfx::destroy_buffer(device, deform_frames);
  gfx::destroy_buffer(device, morph_channels);
  gfx::destroy_buffer(device, morph_directory);
  gfx::destroy_buffer(device, morph_slices);
  gfx::destroy_buffer(device, morph_indices);
  gfx::destroy_buffer(device, morph_deltas);
  gfx::destroy_buffer(device, morph_normals);
  gfx::destroy_buffer(device, morph_params);
  gfx::destroy_buffer(device, morph_weights);
  for (TerrainLevel& level : terrain_) {
    for (gfx::BufferResource& field : level.fields)
      gfx::destroy_buffer(device, field);
    // A staging buffer no frame took yet is still this scene's to free.
    for (TerrainLevel::Pending& copy : level.pending)
      gfx::destroy_buffer(device, copy.staging);
  }
  terrain_.clear();
  gfx::destroy_buffer(device, terrain_table_);
  for (gfx::BufferResource& staging : pending_staging_)
    gfx::destroy_buffer(device, staging);
  pending_staging_.clear();
  // The fields' staging buffers: free, on their way back, and handed back uncopied.
  for (gfx::BufferResource& staging : field_pool_)
    gfx::destroy_buffer(device, staging);
  field_pool_.clear();
  for (Vector<gfx::BufferResource>& returning : field_returning_) {
    for (gfx::BufferResource& staging : returning)
      gfx::destroy_buffer(device, staging);
    returning.clear();
  }
  for (gfx::BufferResource& staging : field_retired_)
    gfx::destroy_buffer(device, staging);
  field_retired_.clear();
  field_staging_made_ = 0;
  pending_geometry_.clear();
  pending_geometry_bytes_ = 0;
  pending_mesh_writes_.clear();
  ring_slots_.clear();
  slot_parts_.clear();
  mesh_descs_.clear();
  rings_ = nullptr;
  scene_vertex_count_ = scene_triangle_count_ = vertex_capacity_ = triangle_capacity_ = 0;
  terrain_ring_bytes_ = terrain_chunk_uploads_ = terrain_chunk_upload_bytes_ = 0;
  gfx::destroy_buffer(device, deform_normals);
  gfx::destroy_buffer(device, static_cache);
  gfx::destroy_buffer(device, joints);
  gfx::destroy_buffer(device, deform_args);
  gfx::destroy_buffer(device, deform_table);
  gfx::destroy_buffer(device, deform_alloc);
  gfx::destroy_buffer(device, deform_slots);
  gfx::destroy_buffer(device, deform_pool);
  for (u32 i = 0; i < 2; ++i) {
    gfx::destroy_buffer(device, flags[i]);
    gfx::destroy_buffer(device, draw_args[i]);
  }
  gfx::destroy_buffer(device, sw_args);
  gfx::destroy_buffer(device, shadow_args);
  gfx::destroy_buffer(device, vertex_headers);
  gfx::destroy_buffer(device, vertex_records);
  gfx::destroy_buffer(device, vertex_indices);
  gfx::destroy_buffer(device, pair_entries);
  gfx::destroy_buffer(device, pair_table);
  gfx::destroy_buffer(device, visible);
  gfx::destroy_buffer(device, cluster_materials);
  gfx::destroy_buffer(device, materials);
  gfx::destroy_buffer(device, skin);
  gfx::destroy_buffer(device, attributes);
  gfx::destroy_buffer(device, lods);
  gfx::destroy_buffer(device, triangles);
  gfx::destroy_buffer(device, vertices);
  gfx::destroy_buffer(device, instances);
  gfx::destroy_buffer(device, quantized);
  gfx::destroy_buffer(device, clusters);
  for (gfx::ImageViewHandle view : texture_views_)
    gfx::destroy_image_view(device, view);
  texture_views_.clear();
  for (gfx::ImageResource& image : textures_)
    gfx::destroy_image(device, image);
  textures_.clear();
  gfx::destroy_image_view(device, procedural_view_);
  procedural_view_ = {};
  if (procedural_texture_.image.valid()) gfx::destroy_image(device, procedural_texture_);
  procedural_texture_ = gfx::ImageResource{};
  gfx::destroy_sampler(device, sampler_);
  sampler_ = {};
  for (const MaterialSampler& made : material_samplers_)
    gfx::destroy_sampler(device, made.sampler);
  material_samplers_.clear();
  transform_conflicts_ = 0;
  texture_bytes_ = 0;
  textures_built_ = textures_decoded_ = 0;
  texture_references_ = textures_shared_ = 0;
  texture_bytes_saved_ = 0;
  bindless_.destroy();
  instance_table_.clear();
  deform_descs_.clear();
  deform_instance_.clear();
  deform_mesh_clusters_.clear();
  morph_channel_count_ = static_cached_instances_ = 0;
  static_cache_bytes_ = 0;
  static_cache_dirty_ = false;
  device_ = nullptr;
  data_ = nullptr;
  cluster_count_ = leaf_count_ = instance_count_ = pair_count_ = material_count_ = 0;
  pair_stride_ = static_instances_ = static_pairs_ = instance_capacity_ = 0;
  dynamic_ = false;
  mesh_material_base_.clear();
  triangles_per_cluster_ = 0;
  view_count_ = 1;
  cascade_count_ = 0;
  max_joints_ = skinned_instances_ = deform_pool_vertices_ = 0;
  visible_run_bytes_ = deform_pool_bytes_ = deform_whole_mesh_bytes_ = 0;
  template_bytes_ = rt_bytes_ = rt_scene_bytes_ = rt_bytes_per_cluster_ = 0;
  rt_capacity_ = rt_capacity_limit_ = rt_union_clusters_ = rt_max_per_instance_ = 0;
  rt_limits_ = gfx::ClusterSetLimits{};
  vertex_index_capacity_ = 0;
  tlas_slot_ = gfx::BindlessSet::k_invalid_slot;
  ray_tracing_ = deform_ = skinned_ = streamed_ = false;
  page_count_ = page_slots_ = slot_vertices_ = slot_triangles_ = max_requests_ = 0;
  upload_budget_bytes_ = 0;
  page_budget_bytes_ = stream_bytes_ = geometry_bytes_ = 0;
}

bool GpuScene::pair_cluster(u32 pair, u32& instance, u32& cluster) const noexcept {
  if (pair >= pair_count_ || instance_count_ == 0 || instance_table_.size() < instance_count_ ||
      data_ == nullptr) {
    return false;
  }
  // The last instance whose first pair is at or below `pair`, exactly as scene.slang's
  // `instance_of_pair` finds it — and, as cluster_cull.slang does next, nothing for a pair past
  // that instance's clusters or of a null instance's mesh: a hole of a streamed world's pairs.
  u32 lo = 0;
  u32 hi = instance_count_ - 1;
  while (lo < hi) {
    const u32 mid = (lo + hi + 1) / 2;
    if (instance_table_[mid].first_pair <= pair) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  const gfx::InstanceDesc& desc = instance_table_[lo];
  if (desc.mesh >= data_->parts.size() + slot_parts_.size()) return false;
  const geometry::ClusterMeshPart& part = part_of(desc.mesh);
  if (pair - desc.first_pair >= part.cluster_count) return false;
  instance = lo;
  cluster = part.first_cluster + (pair - desc.first_pair);
  return true;
}

}  // namespace engine::renderer
