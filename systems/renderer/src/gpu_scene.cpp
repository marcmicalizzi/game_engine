#include <core/base/assert.h>
#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/profiling/startup_trace.h>  // stages only: timing, never logic
#include <core/time/time.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/flora_species.h>
#include <domain/geometry/material_sampling.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/water_spectrum.h>
#include <domain/gfx/water_swash.h>
#include <domain/gfx/wet_ground.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/image/decode.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/rt_capacity.h>
#include <systems/renderer/terrain_materials.h>
#include <systems/renderer/water_refract.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

// A tail block's instances (renderer.md, "Instances that come and go"; W81): its own, when the
// caller named them, or its run of the tail handed over beside the blocks.
std::span<const SceneInstance> block_instances(std::span<const SceneInstance> tail,
                                               const DynamicBlock& block) noexcept {
  if (block.instances != nullptr) return {block.instances, block.count};
  return tail.subspan(block.first, block.count);
}

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
// **A swaying tree is a deformed instance too** (renderer.md, "The flora stage"), and an entry the
// budget refuses draws its rest pose beside neighbours that sway — a crack in the crown. A grove's
// cut is a few hundred thousand vertices a view and as many a cascade, so a scene with trees takes
// at least this, clamped down as every budget is to what its whole meshes need.
tunables::Int flora_pool_mib{"renderer.flora.pool_mib", 96, 1, 16'384,
                             "The deformed-vertex pool's budget, MiB, at least, for a scene whose "
                             "trees sway"};

// **A refused commit keeps the device** (renderer.md, "A commit past the tail's budget withholds
// its farthest tiles"; R95, R137). The tail's room — the deform table and its per-frame copies, the
// pose table's regions, the push state — grows between frames when a tile's trees need more, and a
// device that cannot give it fails the allocation with VK_ERROR_OUT_OF_DEVICE_MEMORY and, in the
// POC's 126,309-instance sward, then lost the device. So a commit whose records would grow the room
// past this ceiling withholds its farthest tiles before anything changes (R137), and one whose
// nearest tile alone would is refused with a sentence naming the bytes; the frame goes on without
// them. The species' caster policies (renderer.md, "The caster policy"): false casts every species
// as a tree does — traced, every cascade, its swaying pose — for the before-and-after measurement.

// Since R137 (2026-11-07) a commit past it withholds its farthest changed tiles rather than being
// refused whole, the bound counts a record given back and taken again in the same commit once, and
// the default 0 takes a sixteenth of the device (`resolve_flora_tail_budget`, settings.h).
tunables::Int flora_tail_budget_mib{
    "renderer.flora.tail_budget_mib", 0, 0, 65'536,
    "MiB the streamed trees' room (deform records, part poses, push state) may grow to, 0 for a "
    "sixteenth of the device's memory (1,024 to 4,096); a tile commit past it withholds its "
    "farthest tiles, never an out-of-memory device loss"};

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
  if (resolved.flora_motion) kib = std::max<u64>(kib, u64(flora_pool_mib.get()) * 1024);
  if (kib < k_min_deform_pool_kib) kib = k_min_deform_pool_kib;
  u64 vertices = kib * 1024 / (3 * sizeof(f32));
  if (vertices < 1) vertices = 1;
  if (vertices > whole_mesh_vertices) vertices = whole_mesh_vertices;
  // Never empty: a scene whose only deformable meshes are a tail's drawn rigid has nothing whole to
  // clamp to, and a buffer of no bytes is no buffer.
  if (vertices < 1) vertices = 1;
  return static_cast<u32>(vertices > 0xFFFFFFFFull ? 0xFFFFFFFFull : vertices);
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
  bool normal_variance = false;
  // A built one's linear mean texel (`built_texture_mean`), which a stochastic material's means
  // are made from (R131); `has_mean` false for a decoded one.
  Vec4 mean{};
  bool has_mean = false;
};

// **A built texture's mean** (R131; renderer.md, "Stochastic mesh materials"): the texels of its
// first level of at most 16 x 16, decoded as the GPU samples them, sRGB taken to linear, averaged.
// The chain is filtered in linear light by a normalized kernel with the sampler's edges, so a small
// level's mean is the image's to within that kernel's and the block encoding's rounding — and
// decoding at most 256 texels costs nothing at load, where reading level 0 back would cost a
// decode of the whole image. False for a chain that will not decode.
bool built_texture_mean(const texture::TextureData& t, Vec4& mean) {
  if (t.levels.empty()) return false;
  u32 level = t.levels.size() - 1u;
  for (u32 l = 0; l < t.levels.size(); ++l) {
    if (u64{t.levels[l].width} * t.levels[l].height <= 256u) {
      level = l;
      break;
    }
  }
  Vector<u8> rgba;
  if (!texture::decode_level(t.format, t.level_bytes(level), t.levels[level].width,
                             t.levels[level].height, rgba))
    return false;
  const bool srgb = t.color_space == texture::ColorSpace::srgb;
  f64 sum[4] = {0.0, 0.0, 0.0, 0.0};
  const u32 texels = static_cast<u32>(rgba.size() / 4u);
  if (texels == 0) return false;
  for (u32 i = 0; i < texels; ++i) {
    for (u32 c = 0; c < 4; ++c) {
      const u8 code = rgba[i * 4u + c];
      sum[c] += c < 3 && srgb ? static_cast<f64>(texture::srgb8_to_linear(code))
                              : static_cast<f64>(code) / 255.0;
    }
  }
  const f64 n = static_cast<f64>(texels);
  mean = Vec4{static_cast<f32>(sum[0] / n), static_cast<f32>(sum[1] / n),
              static_cast<f32>(sum[2] / n), static_cast<f32>(sum[3] / n)};
  return true;
}

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
  const Vector<SceneTexture>& all() const noexcept { return textures_; }

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
  bool normal_variance = false;
};

// **The built textures read ahead of the upload, on a pool** (T73; renderer.md, "What the load
// reads"): `upload materials` read every `.tex` whole on the main thread, one after another between
// two uploads — 5,392 files, 3.3 GB for v35. The plan is the keys `find_built_textures` would read,
// in the order it reads them (mesh order, each key once: a later mesh finds an uploaded key in the
// scene's table and reads nothing); they are read in windows of `k_window` files on the prefetch's
// own job system, the next window while this one is uploaded, so at most two windows' bytes are
// held beside what the upload holds anyway. `take` hands a planned key's bytes over, waiting for
// its window if it must; a key it never planned, or one already handed over, is the caller's to
// read, as it always was. The bytes are the file's, so nothing uploaded changes.
class TexturePrefetch {
 public:
  static constexpr u32 k_window = 128;
  static constexpr u32 k_readers = 8;
  enum class Taken : u8 { not_planned, ok, failed };

  explicit TexturePrefetch(const SceneData& data) {
    HashMap<u64, u32> planned;
    for (const SourceMesh& mesh : data.sources) {
      if (mesh.texture_ddc.empty() || mesh.textures.size() != mesh.data.images.size()) continue;
      Vector<bool> sampled(mesh.data.images.size(), false);
      for (const assets::Material& material : mesh.data.materials) {
        for (const i32 image :
             {material.base_color_image, material.metallic_roughness_image, material.normal_image,
              material.occlusion_image, material.emissive_image}) {
          if (image >= 0 && static_cast<u32>(image) < sampled.size())
            sampled[static_cast<u32>(image)] = true;
        }
      }
      for (u32 i = 0; i < sampled.size(); ++i) {
        const u64 key = mesh.textures[i].key;  // zero: named by path, hashed at the upload
        if (!sampled[i] || key == 0 || planned.find_value(key) != nullptr) continue;
        planned.insert(key, items_.size());
        Item item;
        item.key = key;
        item.path = texture::texture_cache_path(mesh.texture_ddc, key);
        items_.push_back(std::move(item));
      }
    }
    index_ = std::move(planned);
    window_count_ = (items_.size() + k_window - 1) / k_window;
    if (window_count_ == 0) return;
    windows_ = std::make_unique<Window[]>(window_count_);
    for (u32 w = 0; w < window_count_; ++w) {
      windows_[w].self = this;
      windows_[w].begin = w * k_window;
      windows_[w].end = std::min<u32>(items_.size(), (w + 1) * k_window);
    }
    // The efficiency pool, which a waiting outside thread never helps (jobs.md): the main thread
    // uploads while the readers read, and never ends up reading the next window itself.
    jobs::JobSystemConfig config;
    config.performance_workers = 1;
    config.efficiency_workers = k_readers;
    pool_.emplace(config);
  }
  ~TexturePrefetch() {
    // Every window scheduled is waited for before its items go: a worker may still be reading.
    for (u32 w = 0; w < window_count_; ++w)
      if (windows_[w].scheduled) pool_->wait(windows_[w].counter);
  }
  TexturePrefetch(const TexturePrefetch&) = delete;
  TexturePrefetch& operator=(const TexturePrefetch&) = delete;

  Taken take(u64 key, texture::TextureData& out, std::string& error) {
    const u32* at = index_.find_value(key);
    if (at == nullptr) return Taken::not_planned;
    Item& item = items_[*at];
    if (item.taken) return Taken::not_planned;
    const u32 w = *at / k_window;
    schedule(w);
    if (w + 1 < window_count_) schedule(w + 1);  // the next window read while this one uploads
    if (!windows_[w].waited) {
      pool_->wait(windows_[w].counter);
      windows_[w].waited = true;
      // The windows before this one are done with: what nobody took goes now, and a mesh that
      // asks for one of them later (an earlier mesh refused before reaching it) reads it itself.
      for (; released_ < w; ++released_) {
        Window& done = windows_[released_];
        if (done.scheduled && !done.waited) {
          pool_->wait(done.counter);  // a window read ahead and never asked for
          done.waited = true;
        }
        for (u32 i = windows_[released_].begin; i < windows_[released_].end; ++i) {
          items_[i].data = texture::TextureData{};
          items_[i].taken = true;
        }
      }
    }
    item.taken = true;
    if (!item.ok) {
      error = std::move(item.error);
      return Taken::failed;
    }
    out = std::move(item.data);
    return Taken::ok;
  }

 private:
  struct Item {
    u64 key = 0;
    std::string path;
    texture::TextureData data;
    std::string error;
    bool ok = false;
    bool taken = false;
  };
  struct Window {
    TexturePrefetch* self = nullptr;
    u32 begin = 0;
    u32 end = 0;
    std::atomic<u32> next{0};
    jobs::Counter counter;
    bool scheduled = false;
    bool waited = false;
  };
  // One runner a thread, each taking the window's next file (scene.cpp's `run_jobs`, for the same
  // reason: never more jobs than the pool's queues hold).
  static void run(void* data) {
    Window& window = *static_cast<Window*>(data);
    for (u32 i = window.begin + window.next.fetch_add(1, std::memory_order_relaxed); i < window.end;
         i = window.begin + window.next.fetch_add(1, std::memory_order_relaxed)) {
      Item& item = window.self->items_[i];
      item.ok = texture::read_texture_file(item.path, item.data, &item.error);
    }
  }
  void schedule(u32 w) {
    Window& window = windows_[w];
    if (window.scheduled) return;
    window.scheduled = true;
    const u32 readers = std::max(pool_->worker_count(jobs::Pool::Efficiency), 1u);
    const u32 runners = std::min<u32>(window.end - window.begin, readers);
    Vector<jobs::Job> list;
    for (u32 r = 0; r < runners; ++r)
      list.push_back(jobs::Job{&TexturePrefetch::run, &window, &window.counter});
    pool_->schedule(jobs::Pool::Efficiency, std::span<const jobs::Job>(list.data(), list.size()),
                    window.counter);
  }

  Vector<Item> items_;
  HashMap<u64, u32> index_;
  std::unique_ptr<Window[]> windows_;
  u32 window_count_ = 0;
  u32 released_ = 0;
  std::optional<jobs::JobSystem> pool_;
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
// `normal_filter` (R97's tunable) off reads a normal map's variance chain as absent.
bool find_built_textures(const SourceMesh& mesh, const SceneTextures& known, bool normal_filter,
                         Vector<BuiltImage>& out, u32& needed, std::string& why,
                         TexturePrefetch* prefetch = nullptr) {
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
      out[i].normal_variance = held->normal_variance;
      continue;
    }
    const std::string path = texture::texture_cache_path(mesh.texture_ddc, key);
    if (!io::exists(path)) {
      why = image + " has not been built (engine-content build --cache builds it)";
      return false;
    }
    std::string error;
    // Read ahead when the prefetch planned this key, here otherwise: the same file either way.
    const TexturePrefetch::Taken taken = prefetch != nullptr
                                             ? prefetch->take(key, out[i].data, error)
                                             : TexturePrefetch::Taken::not_planned;
    const bool read = taken == TexturePrefetch::Taken::ok ||
                      (taken == TexturePrefetch::Taken::not_planned &&
                       texture::read_texture_file(path, out[i].data, &error));
    if (!read) {
      why = image + ": " + error;
      return false;
    }
    if (out[i].data.source_hash != source_hash || out[i].data.build_key != key) {
      why = image + ": '" + path + "' was built from other bytes or options";
      return false;
    }
    out[i].stored = out[i].data.format;
    out[i].normal_variance = normal_filter && !out[i].data.normal_variance.empty();
  }
  return true;
}

// **A body's swash numbers** (water.md, "The shore"; S94): a closed body's own wind over the
// smaller of its fetch and its own longest reach (the map's square's diagonal), an open body's the
// wind its spectrum took; the wind's wave over the fetch, the map's foreshore slope, Stockdon's
// run-up, the tide. The shore's record and the body's surface (S100) both take them from here.
struct WaterSwashNumbers {
  f64 wind = 0.0;
  f64 fetch_m = 0.0;
  f64 slope = 0.0;
  gfx::SwashWave wave{};
  f64 run_up = 0.0;
  f32 tide = 0.0f;
  // S102: the train's direction over deep water; with a swell, the wind sea's peak wavenumber.
  Vec2 direction{1.0f, 0.0f};
  f64 wind_k0 = 0.0;
};
WaterSwashNumbers water_swash_numbers(const SceneWater& water) {
  WaterSwashNumbers n;
  const scene::WaterBody& body = water.body;
  n.wind = static_cast<f64>(body.wind_m_s);
  if (body.open && water.wind_m_s > 0.0f) n.wind = static_cast<f64>(water.wind_m_s);
  // The fetch the scene's reader found (S100 stage B): an open body's `fetch_km`, a closed body's
  // own reach along its wind.
  n.fetch_m = static_cast<f64>(water.fetch_m);
  n.slope = gfx::water_mean_shore_slope(
      water.depth.texels.data(), water.depth.size, static_cast<f64>(water.depth.texel_m),
      static_cast<f64>(water.depth.depth_max), static_cast<f64>(water.depth.bank_max));
  n.wave = gfx::swash_wave(n.wind, n.fetch_m);
  n.direction = water.shading.direction;
  // **A swell** (S102; water.md, "The swell") is the train that shoals, turns, breaks and runs up,
  // from its own direction; the wind's wave keeps the surface offshore.
  if (body.open && body.swell_height_m > 0.0f && body.swell_period_s > 0.0f) {
    if (n.wave.period_s > 0.0) {
      const f64 omega = 6.283185307179586 / gfx::swash_period(n.wave.period_s);
      n.wind_k0 = omega * omega / gfx::k_swash_g;
    } else {
      n.wind_k0 = 1.0;  // no wind sea to calm: any positive number marks the swell
    }
    n.wave.height_m = static_cast<f64>(body.swell_height_m);
    n.wave.period_s = std::min(static_cast<f64>(body.swell_period_s), 30.0);
    const f32 a = radians(body.swell_direction_deg);
    n.direction = Vec2{std::cos(a), std::sin(a)};
  }
  // Stockdon et al.'s, as it was, under the steep shore's ceiling (S102), which only a long low
  // wave on a steep shore reaches.
  n.run_up = std::min(gfx::stockdon_r2(n.wave.height_m, n.wave.period_s, n.slope),
                      gfx::k_swash_steep_cap * n.wave.height_m);
  n.tide = body.open ? body.tide_m : 0.0f;
  return n;
}

// **A body's height function** (S100; water_swash.h, `WaterSurface`; water.md, "The mesh"): the
// body's waves (an open body's spectrum, else the material's four sines), the wind's peak wave and
// its run-up, the tide, and the arrival field over the depth map's grid — the depth decoded from
// the map's 16 bits as the GPU's image is, the eikonal's arrival time beside it.
void water_surface_of(const SceneWater& water, const WaterSwashNumbers& n, gfx::WaterSurface& out,
                      Vector<f32>& field) {
  const SceneWaterDepth& map = water.depth;
  out = gfx::WaterSurface{};
  out.origin = map.origin;
  out.texel_m = map.texel_m;
  out.size = map.size;
  const f64 period = gfx::swash_period(n.wave.period_s);
  const f64 two_pi = 6.283185307179586;
  out.height_m = static_cast<f32>(n.wave.height_m);
  out.period_s = static_cast<f32>(period);
  out.k0 = period > 0.0 ? static_cast<f32>((two_pi / period) * (two_pi / period) / gfx::k_swash_g)
                        : 0.0f;
  out.run_up_m = static_cast<f32>(n.run_up);
  out.slope = static_cast<f32>(n.slope);
  out.tide_m = n.tide;
  const f64 l0 = gfx::k_swash_g * period * period / two_pi;
  out.along_m = static_cast<f32>(std::max(2.0 * l0, 4.0));
  const scene::WaterBody& body = water.body;
  if (body.open) {
    out.flags |= gfx::k_water_surface_open;
    out.fade_m = static_cast<f32>(std::clamp(0.5 * l0, 2.0, 30.0));
  }
  out.amplitude = water.shading.wave_amplitude_m;
  out.wavelength = water.shading.wave_length_m;
  out.chop = water.shading.chop;
  out.seed = water.shading.seed;
  out.direction = water.shading.direction;
  out.wind = water.shading.direction;
  out.mesh_far_m = map.mesh_far_m;
  out.mesh_near_m = map.mesh_near_m;
  out.mesh_grade = map.mesh_grade;
  if (water.waves.size() == gfx::k_water_waves) {
    out.flags |= gfx::k_water_surface_spectrum;
    for (u32 i = 0; i < gfx::k_water_waves; ++i)
      out.waves[i] = water.waves[i];
  }
  const u32 count = map.size * map.size;
  Vector<f32> depth(count);
  const f64 range = static_cast<f64>(map.depth_max) + static_cast<f64>(map.bank_max);
  for (u32 i = 0; i < count; ++i)
    depth[i] = static_cast<f32>(static_cast<f64>(map.texels[i]) / 65535.0 * range -
                                static_cast<f64>(map.bank_max));
  // S102 (water_swash.h): the train comes from a direction, and the field's fourth channel is each
  // texel's own stretch of shore's slope, from which a pixel takes its run-up and its sheet's life.
  out.swell = static_cast<f32>(n.wind_k0);
  out.reach_scale = static_cast<f32>(gfx::swash_reach_scale(n.wave.height_m, n.wave.period_s));
  out.reach_cap =
      static_cast<f32>(gfx::swash_reach_cap(n.wave.height_m, static_cast<f64>(map.bank_max)));
  Vector<f32> arrival(count);
  gfx::water_arrival_field(depth.data(), map.size, static_cast<f64>(map.texel_m), period,
                           arrival.data(), 3, n.direction);
  Vector<f32> shore_slope(count);
  gfx::water_shore_slope_field(depth.data(), map.size, static_cast<f64>(map.texel_m),
                               shore_slope.data());
  // The share of the body's fetch the wind has blown over at each texel (a closed body's lee): the
  // fetch field over the fetch the table was grown at. An open body's sea comes in grown.
  Vector<f32> grown(count);
  if (body.open || !(n.fetch_m > 0.0)) {
    for (u32 i = 0; i < count; ++i)
      grown[i] = 1.0f;
  } else {
    gfx::water_fetch_field(depth.data(), map.size, static_cast<f64>(map.texel_m), out.wind,
                           grown.data());
    for (u32 i = 0; i < count; ++i)
      grown[i] = static_cast<f32>(std::clamp(static_cast<f64>(grown[i]) / n.fetch_m, 0.0, 1.0));
  }
  field.resize(gfx::k_water_field_channels * count);
  for (u32 i = 0; i < count; ++i) {
    field[4 * i] = depth[i];
    field[4 * i + 1] = arrival[i];
    field[4 * i + 2] = grown[i];
    field[4 * i + 3] = shore_slope[i];
  }
}

}  // namespace

// For gpu_scene_textures.cpp: a texture admitted after the load is made by what makes the load's.
namespace detail {
gfx::Format built_texture_format(const texture::TextureData& t) noexcept {
  return texture_gpu_format(t);
}
bool built_texture_mean_of(const texture::TextureData& t, Vec4& mean) {
  return built_texture_mean(t, mean);
}
gfx::SamplerDesc material_sampler_desc(const geometry::TextureSampler& sampler, bool mipmapped) {
  return sampler_desc(sampler, mipmapped);
}
}  // namespace detail

GpuScene::~GpuScene() { destroy(); }

JsonValue GpuScene::pair_summary() const {
  JsonValue out = pair_account_summary(resolved_.pairs);
  if (data_ == nullptr) return out;
  out.set("pair_count", pair_count_);
  out.set("tail_live_pairs", dynamic_pair_count());
  const u64 tail_reserved = pair_count_ - static_pairs_;
  out.set("tail_reserved_pairs", tail_reserved);
  out.set("tail_hole_pairs", tail_reserved - dynamic_pair_count());
  out.set("untraced_pairs", untraced_pairs_live_);
  out.set("static_form_pairs", static_pairs_live_);
  JsonValue rings = JsonValue::array();
  for (const RingSlots& held : ring_slots_) {
    JsonValue ring = JsonValue::object();
    ring.set("level", held.level);
    ring.set("slots_reserved", held.slots.size());
    ring.set("slots_used", held.slots.size() - held.free_slots.size());
    ring.set("reserved_tile_pairs", held.clusters_per_slot);
    ring.set("reserved_pairs", u64{held.slots.size()} * held.clusters_per_slot);
    u64 live = 0;
    for (const TerrainSlot& slot : held.slots)
      if (slot.on) live += slot.clusters;
    ring.set("live_pairs", live);
    rings.push_back(std::move(ring));
  }
  out.set("rings", std::move(rings));
  JsonValue placements = JsonValue::array();
  u32 ring_count = data_->world.ring_count;
  for (const u32 rank : slot_rank_)
    ring_count = std::max(ring_count, rank);
  for (u32 r = 0; r < ring_count; ++r) {
    u64 reserved = 0, live = 0, reserved_slots = 0, used = 0, tiles = 0;
    Vector<u64> form_pairs(data_->parts.size());
    Vector<u32> form_instances(data_->parts.size());
    for (const TileBlock& block : layout_.blocks()) {
      const u32 rank = block.inst_begin < slot_rank_.size() ? slot_rank_[block.inst_begin] : 1u;
      if (rank != r + 1) continue;
      ++tiles;
      reserved += block.pair_cap;
      live += block.pairs;
      reserved_slots += block.inst_cap;
      used += block.instances;
      for (u32 i = block.inst_begin; i < block.inst_begin + block.instances; ++i) {
        const u32 mesh = instance_table_[i].mesh;
        if (mesh >= form_pairs.size()) continue;
        ++form_instances[mesh];
        form_pairs[mesh] += data_->parts[mesh].cluster_count;
      }
    }
    JsonValue ring = JsonValue::object();
    ring.set("ring", r);
    ring.set("tiles", tiles);
    ring.set("slots_reserved", reserved_slots);
    ring.set("slots_used", used);
    ring.set("reserved_pairs", reserved);
    ring.set("live_pairs", live);
    JsonValue forms = JsonValue::array();
    for (u32 mesh = 0; mesh < form_pairs.size(); ++mesh) {
      if (form_instances[mesh] == 0) continue;
      JsonValue form = JsonValue::object();
      form.set("mesh", mesh);
      form.set("source_mesh", mesh < data_->form_source.size() ? data_->form_source[mesh] : mesh);
      form.set("clusters", data_->parts[mesh].cluster_count);
      form.set("instances", form_instances[mesh]);
      form.set("pairs", form_pairs[mesh]);
      forms.push_back(std::move(form));
    }
    ring.set("forms", std::move(forms));
    placements.push_back(std::move(ring));
  }
  out.set("placement_rings", std::move(placements));
  return out;
}

bool GpuScene::create(const gfx::Device& device, const SceneData& data,
                      const ResolvedSettings& requested, std::string* error,
                      TerrainLevelSet* rings) {
  ResolvedSettings resolved;
  const PairAccount account = account_pairs(data, requested, rings);
  resolve_settings(requested.settings, device.features(), &data, resolved, &account);
  destroy();
  device_ = &device;
  data_ = &data;
  rings_ = rings;
  ray_tracing_ = resolved.rt_chain;
  // A traced-primary scene builds its untraced instances' drawn entries (R110), so they count in
  // what a frame can want (`rt_frame_estimate`).
  rt_untraced_drawn_ = resolved.rt_chain && resolved.ray_path;
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
  resolved_ = resolved;
  // Which rigid forms keep static structures, before the geometry is uploaded: their cut is the
  // frontier (renderer.md, "Static structures for rigid forms").
  plan_static_forms();
  triangles_per_cluster_ = geometry::ClusterLodOptions{}.max_triangles;
  instance_table_ = data.instances;
  // **The caster policy, per mesh** (renderer.md, "The caster policy"; R95): the species the
  // generator section names (a distance form keeps its source's), its catalogue row's policy as
  // InstanceDesc flag bits, ORed into every instance of the mesh — here for the load's, in
  // `write_slots` for the tail's. A mesh with no species, or a species with the trees' defaults,
  // has none, and its instances keep exactly the flags they had.
  mesh_policy_.assign(data.parts.size(), 0u);
  for (u32 m = 0; m < data.parts.size(); ++m)
    mesh_policy_[m] = mesh_caster_policy(data, m);
  for (gfx::InstanceDesc& placed : instance_table_) {
    if (placed.mesh < mesh_policy_.size()) placed.flags |= mesh_policy_[placed.mesh];
  }
  policy_counts();  // again by `flora_counts` once the field's records are laid out
  scene_vertex_count_ = data.lod.mesh.vertices.size();
  scene_triangle_count_ = data.lod.mesh.triangles.size();
  vertex_capacity_ = scene_vertex_count_;
  triangle_capacity_ = scene_triangle_count_;
  // The terrain rings' slots, beside the scene's own meshes and instances: more clusters, pairs,
  // instances and arena room than the scene has, reserved now so that nothing about the scene's
  // layout ever has to move for a ring (renderer.md, "The rings in the scene").
  // A scene whose instances come and go takes them too: they are its load's prefix, and the tail's
  // blocks follow them (renderer.md, "Instances that come and go").
  //
  // **The pair accounting, before anything is made** (renderer.md, "The pair accounting"): the
  // load's prefix, the rings' slots and the least a streamed tail must hold — the most pairs one
  // tile of the scene's streamed instances holds drawn whole (`SceneData::tail_floor_pairs`) —
  // against what the visibility id names, or for a scene whose instances come and go with the
  // chain, what the device's cluster geometry indices name over the views (`pair_limit`). Until
  // 2026-10-24 the rings were checked level by level as they were laid out, with a sentence that
  // named no number, and the full v8 forest was refused there with its prefix the real cause.
  accounted_slot_pairs_ = resolved.pairs.slot_pairs;
  if (!pairs_fit(resolved.pairs, error)) {
    destroy();
    return false;
  }
  if (rings != nullptr && resolved.terrain_levels && rings->valid() && !streamed_) {
    if (!lay_out_rings(resolved, *rings, error)) {
      destroy();
      return false;
    }
    pair_stride_ = pair_count_;
  }
  // Room for the meshes that arrive with their tiles, after the rings' slots: mesh records, an
  // arena of clusters and the vertices and triangles it owns, and no instance or pair — a tile's
  // instances are the tail's (gpu_scene_meshes.cpp; renderer.md, "Meshes that come and go").
  if (!lay_out_mesh_slots(error)) {
    destroy();
    return false;
  }
  // The prefix a dynamic scene's tail follows: the load's instances and the levels' slots.
  static_instances_ = instance_count_;
  static_pairs_ = pair_count_;
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
  // **The tail is rigid but for its trees; the prefix deforms as any scene's does** (renderer.md,
  // "The pool and the chain over the tail", "Streamed trees sway"). The deformed-vertex pool's
  // per-instance table is laid out here for the load's instances — the terrain levels, and since
  // 2026-10-18 a skinned or morphed authored instance — whose slots never move; a tail instance is
  // placed with `k_invalid_deform` unless its mesh sways (W74), when it takes a record past the
  // load's from its mesh's free list. The pool's per-frame blocks follow whatever the cull emits,
  // and the per-entry slot table is laid out by the stride (`relayout`). Still refused: `--deform`,
  // which deforms *every* instance, the tail's too (`resolve_settings` turns it off for such a
  // scene), a skinned or morphed instance in the tail (`set_dynamic_instances` names it), and
  // cluster templates, built once from the load's clusters.
  if (dynamic_ && (resolved.settings.deform || (ray_tracing_ && resolved.settings.rt_templates))) {
    if (error != nullptr) {
      *error = resolved.settings.rt_templates && ray_tracing_
                   ? "a scene whose instances come and go between frames builds no cluster "
                     "templates yet: draw it without --rt-templates"
                   : "a scene whose instances come and go between frames deforms its loaded "
                     "instances only: --deform would deform the tail's too";
    }
    destroy();
    return false;
  }
  if (dynamic_) {
    // The instance tables' capacity before anything per instance is sized by it (the chain's
    // tables are), in pages of 1,024 slots with half as much again.
    instance_capacity_ =
        static_cast<u32>((u64{instance_count_} + u64{instance_count_} / 2 + 1024) / 1024 * 1024);
    pair_limit_ = static_cast<u32>(resolved.pairs.limit);
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
        // At its chunk's corner, written with the first frame's copies (the instance table went
        // up with the materials, before the chunks).
        const u32 instance = ring.first_instance + s;
        gfx::set_terrain_corner(instance_table_[instance], chunk.corner_x_mm, chunk.corner_z_mm);
        instance_table_[instance].flags |= gfx::k_instance_uv_from_corner;
        // A dynamic scene's sets are written whole by their first frames, from the table.
        if (!dynamic_) pending_instance_writes_.push_back(instance);
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
  // The room's slots: the scene's addresses and no clusters, until a mesh is admitted into one.
  for (u32 s = 0; s < mesh_room_.slots; ++s)
    mesh_descs.push_back(described(slot_parts_[mesh_part_base_ + s]));
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
  if ((!ring_slots_.empty() || mesh_room_.slots != 0) && !create_staging_ring(error)) {
    destroy();
    return false;
  }
  rings_ = nullptr;
  if (!dynamic_) {
    if (!upload_pair_table(error)) {
      destroy();
      return false;
    }
    return true;
  }
  // A dynamic scene's tables are sets, one per frame in flight (two until a renderer asks for
  // more), each kept at a capacity — the instance table in pages of 1,024 slots, the pair table as
  // long as the stride — and each written by the first frame that reads it: everything here is
  // pending, so the first frame flips to a set and uploads the load's instances and expands their
  // pairs, as it would a change. Nothing is written from the CPU at all — but the terrain levels'
  // slots' pairs, which are the slot's whatever chunk it draws (a slot's mesh record counts only
  // the chunk's clusters, which is what the expansion reads): those are uploaded here once and
  // copied into each set when it is written whole.
  layout_.reset(static_instances_, static_pairs_);
  compact_pct_ = tile_compact_pct_tunable();
  tail_keys_.assign(instance_count_, InstanceKey{});
  if (!ring_slots_.empty()) {
    ring_pair_begin_ = instance_table_[ring_slots_[0].first_instance].first_pair;
    Vector<u32> table(u64{static_pairs_ - ring_pair_begin_} * 2u, 0u);
    for (u32 i = ring_slots_[0].first_instance; i < static_instances_; ++i) {
      const gfx::InstanceDesc& instance = instance_table_[i];
      const geometry::ClusterMeshPart& part = part_of(instance.mesh);
      for (u32 c = 0; c < part.cluster_count; ++c) {
        table[u64{instance.first_pair - ring_pair_begin_ + c} * 2u] = i;
        table[u64{instance.first_pair - ring_pair_begin_ + c} * 2u + 1u] = part.first_cluster + c;
      }
    }
    if (!gfx::upload_buffer(device, table.data(), u64{table.size()} * sizeof(u32),
                            gfx::BufferUsage::TransferSrc, ring_pairs_, error)) {
      destroy();
      return false;
    }
  }
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

gfx::TlasInstance GpuScene::tlas_record(u32 slot) const noexcept {
  gfx::TlasInstance record;
  // The prefix is always live; a tail slot is when it names one of the scene's meshes (a hole's
  // null instance names the null mesh, past them).
  const bool live = slot < instance_table_.size() && (!dynamic_ || slot < static_instances_ ||
                                                      tail_mesh(instance_table_[slot].mesh));
  if (slot < instance_table_.size())
    record.transform = gfx::instance_matrix(instance_table_[slot], WorldEye{});
  record.custom_index = slot;
  // The mask from the instance's flags (R110; `gfx::instance_ray_mask`), which the frame's
  // `tlas_references` dispatch writes again every frame for a live record; a hole stays 0.
  record.mask = live ? gfx::instance_ray_mask(instance_table_[slot].flags) : 0u;
  record.blas = 0;
  return record;
}

void GpuScene::mark_instance_written(u32 instance) {
  if (!dynamic_) {
    pending_instance_writes_.push_back(instance);
    return;
  }
  // Through the table sets, as a tail's slot is: the frame that flips to a set copies it, and the
  // chain's top-level record with it. The caller is between frames, before the next one's
  // `prepare_tables`, so the slot's mesh record (`pending_mesh_writes_`) lands in the same frame.
  for (TableSet& set : sets_) {
    if (!set.all) set.pending.push_back(InstanceRange{instance, instance + 1});
  }
  tables_changed_ = true;
}

bool GpuScene::relayout(std::string* error) {
  if (!relayout_pending_) return true;
  relayout_pending_ = false;
  if (flora_grow_ && !grow_flora_tail(error)) return false;
  // The pool's per-entry slots are laid out by the stride, and every mesh record names them.
  // The allocator's tile scratch follows the same stride (R104).
  if (deform_ && deform_blocks.size < deform_block_bytes()) {
    gfx::destroy_buffer(*device_, deform_blocks);
    if (!gfx::create_buffer(*device_, deform_block_bytes(), k_address, false, deform_blocks,
                            error)) {
      return false;
    }
  }
  if (deform_ && deform_slots.size < u64{visible_entries()} * sizeof(u32)) {
    gfx::destroy_buffer(*device_, deform_slots);
    if (!gfx::create_buffer(*device_, u64{visible_entries()} * sizeof(u32), k_address, false,
                            deform_slots, error)) {
      return false;
    }
    for (gfx::MeshDesc& desc : mesh_descs_)
      desc.deform_slots = deform_slots.address;
    for (RingSlots& ring : ring_slots_) {
      for (TerrainSlot& slot : ring.slots)
        slot.desc.deform_slots = deform_slots.address;
    }
    gfx::destroy_buffer(*device_, meshes);
    if (!gfx::upload_buffer(*device_, mesh_descs_.data(),
                            mesh_descs_.size() * sizeof(gfx::MeshDesc), k_storage, meshes, error)) {
      return false;
    }
  }
  if (ray_tracing_ && !lay_out_ray_tracing(error)) return false;
  return true;
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

// ---- the tail's trees (renderer.md, "Streamed trees sway"; roadmap W74) -----------------------

bool GpuScene::tail_sways(u32 mesh) const noexcept {
  return mesh < flora_meshes_.size() && flora_meshes_[mesh].part_count() > 0 &&
         !(mesh < data_->form_rigid.size() && data_->form_rigid[mesh] != 0);
}

// A swaying instance's padding: its mesh's sway bound, or for a distance form its source's — the
// bound the form was cut for (scene.cpp, `stream_authored`), so the cull's choice over the form is
// the full mesh's.
f32 GpuScene::flora_padding_of(u32 mesh) {
  const u32 source_mesh = mesh < data_->form_source.size() ? data_->form_source[mesh] : mesh;
  const u32 m = source_mesh < flora_meshes_.size() && flora_meshes_[source_mesh].part_count() > 0
                    ? source_mesh
                    : mesh;
  if (flora_padding_[m] < 0.0f) {
    const SourceMesh& source = data_->sources[m];
    const geometry::ClusterMeshPart& part = data_->parts[m];
    flora_padding_[m] = flora_bounds_padding(
        flora_meshes_[m],
        std::span<const Vec3>(data_->lod.mesh.vertices.data() + part.first_vertex,
                              source.motion_vertices.size()),
        std::span<const geometry::ClusterFileMotionVertex>(source.motion_vertices.data(),
                                                           source.motion_vertices.size()));
  }
  return flora_padding_[m];
}

FloraInstance GpuScene::flora_instance(u32 instance, u32 mesh, u32 first_part,
                                       u32 key) const noexcept {
  const gfx::InstanceDesc& placed = instance_table_[instance];
  FloraInstance fi;
  fi.instance = instance;
  fi.mesh = mesh;
  fi.first_part = first_part;
  fi.seed = key * 0x9e3779b9u + 0x632be5abu;
  // World to mesh: the instance's 3x3 inverted, once.
  const Vec3 r0 = placed.rows[0].xyz();
  const Vec3 r1 = placed.rows[1].xyz();
  const Vec3 r2 = placed.rows[2].xyz();
  const Vec3 c0{r0.x, r1.x, r2.x};
  const Vec3 c1{r0.y, r1.y, r2.y};
  const Vec3 c2{r0.z, r1.z, r2.z};
  const f32 det = dot(c0, cross(c1, c2));
  if (det != 0.0f) {
    const f32 inv = 1.0f / det;
    fi.inverse[0] = cross(c1, c2) * inv;
    fi.inverse[1] = cross(c2, c0) * inv;
    fi.inverse[2] = cross(c0, c1) * inv;
  }
  fi.inverse_scale = placed.scale_max > 0.0f ? 1.0f / placed.scale_max : 1.0f;
  return fi;
}

namespace {

u32 tail_motion_key(const SceneInstance& source) noexcept {
  if (source.motion_key != k_motion_key_load) return source.motion_key;
  // No stable name: where it stands, in whole millimetres of x and z, hashed — a function of the
  // instance, never of when its tile came.
  const f64 x = source.origin.x + static_cast<f64>(source.transform.position.x);
  const f64 z = source.origin.z + static_cast<f64>(source.transform.position.z);
  const u64 mx = static_cast<u64>(static_cast<i64>(std::llround(x * 1000.0)));
  const u64 mz = static_cast<u64>(static_cast<i64>(std::llround(z * 1000.0)));
  u64 h = mx * 0x9e3779b97f4a7c15ull ^ (mz + 0x632be59bd9b4e019ull + (mx << 6) + (mx >> 2));
  h ^= h >> 31;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 29;
  return static_cast<u32>(h) | 0x80000000u;  // past any loaded index a scene can have
}

}  // namespace

void GpuScene::release_tail_tree(u32 slot) noexcept {
  if (slot >= tail_tree_of_slot_.size()) return;
  const u32 t = tail_tree_of_slot_[slot];
  if (t == ~0u) return;
  TailTree& tree = tail_trees_[t];
  // Out of the poser's list by the last one taking its place: a pose is a function of the
  // instance alone, so the order the poser runs them in reaches no byte.
  const u32 last = flora_instances_.size() - 1;
  if (tree.at != last) {
    flora_instances_[tree.at] = flora_instances_[last];
    tail_trees_[tail_tree_of_slot_[flora_instances_[tree.at].instance]].at = tree.at;
  }
  flora_instances_.pop_back();
  tree.at = ~0u;
  // Free: first in its mesh's list, last in its class's by age.
  tree.released = flora_frames_;
  tree.mesh_prev = ~0u;
  tree.mesh_next = tail_tree_free_[tree.mesh];
  if (tree.mesh_next != ~0u) tail_trees_[tree.mesh_next].mesh_prev = t;
  tail_tree_free_[tree.mesh] = t;
  tree.old_next = ~0u;
  tree.old_prev = tail_old_tail_[tree.cls];
  if (tree.old_prev != ~0u) {
    tail_trees_[tree.old_prev].old_next = t;
  } else {
    tail_old_head_[tree.cls] = t;
  }
  tail_old_tail_[tree.cls] = t;
  tail_tree_of_slot_[slot] = ~0u;
  flora_recut_ = true;
}

void GpuScene::unlink_tail_tree(u32 t) noexcept {
  TailTree& tree = tail_trees_[t];
  if (tree.mesh_prev != ~0u) {
    tail_trees_[tree.mesh_prev].mesh_next = tree.mesh_next;
  } else {
    tail_tree_free_[tree.mesh] = tree.mesh_next;
  }
  if (tree.mesh_next != ~0u) tail_trees_[tree.mesh_next].mesh_prev = tree.mesh_prev;
  if (tree.old_prev != ~0u) {
    tail_trees_[tree.old_prev].old_next = tree.old_next;
  } else {
    tail_old_head_[tree.cls] = tree.old_next;
  }
  if (tree.old_next != ~0u) {
    tail_trees_[tree.old_next].old_prev = tree.old_prev;
  } else {
    tail_old_tail_[tree.cls] = tree.old_prev;
  }
  tree.mesh_prev = tree.mesh_next = tree.old_prev = tree.old_next = ~0u;
}

void GpuScene::acquire_tail_tree(u32 slot, const SceneInstance& source) {
  const u32 mesh = source.mesh;
  const FloraMesh& shape = flora_meshes_[mesh];
  if (shape.field) {
    // **Grass takes no record** (renderer.md, "The field kind"): its mesh's, shared and never
    // written again, and nothing the poser runs. A tile of a meadow costs the room nothing.
    gfx::InstanceDesc& placed = instance_table_[slot];
    placed.deform = field_deform_[mesh];
    const f32 padding = flora_padding_of(mesh);
    if (padding > placed.bounds_padding) placed.bounds_padding = padding;
    flora_stats_.max_padding =
        std::max(flora_stats_.max_padding, placed.bounds_padding * placed.scale_max);
    return;
  }
  const geometry::ClusterMeshPart& part = data_->parts[mesh];
  const u32 parts = shape.part_count();
  const u32 cls = parts <= 1 ? 0u : static_cast<u32>(std::bit_width(parts - 1u));
  u32 t = tail_tree_free_[mesh];
  const u32 oldest = tail_old_head_[cls];
  if (t != ~0u) {
    // A record its mesh gave back: the same stage, vertices and run of parts, so its entry in the
    // deform table is already what it should be.
    unlink_tail_tree(t);
  } else if (oldest != ~0u &&
             flora_frames_ >= tail_trees_[oldest].released + u64{k_joint_slots} + 1) {
    // **Another mesh's, of the same run size, given back long enough ago that no frame in flight
    // can still draw the tree that held it**: its entry is rewritten for this mesh. Without this a
    // world's records were the sum of every mesh's most at once — 15,101 records and 211 MB along
    // the v8 crossing, where 365 trees swayed at its end — because the species change as a
    // flight crosses the region and each kept the room its own peak took.
    t = oldest;
    unlink_tail_tree(t);
    TailTree& tree = tail_trees_[t];
    tree.mesh = mesh;
    gfx::DeformDesc& desc = deform_descs_[tree.deform];
    desc.first_vertex = part.first_vertex;
    desc.joint_count = parts;
    deform_mesh_clusters_[tree.deform] = part.cluster_count;
    put_deform_record(tree.deform, desc);
    ++flora_stats_.tail_repurposed;
  } else {
    // A new record past every other: an entry of the deform table and a run of the pose table
    // (its class's size, so another mesh of the class can take it later) that no frame has read,
    // written now if they fit what the tables hold, and by `relayout`'s growth, before the next
    // frame, if not.
    t = tail_trees_.size();
    TailTree tree;
    tree.mesh = mesh;
    tree.cls = cls;
    tree.deform = deform_descs_.size();
    tree.first_part = flora_parts_;
    tail_trees_.push_back(tree);
    gfx::DeformDesc desc{};
    desc.stages = gfx::k_deform_stage_flora;
    desc.first_vertex = part.first_vertex;
    desc.first_part = flora_parts_;
    desc.joint_count = parts;
    deform_descs_.push_back(desc);
    deform_instance_.push_back(slot);
    deform_mesh_clusters_.push_back(part.cluster_count);
    flora_parts_ += 1u << cls;
    flora_state_.resize(flora_parts_, FloraPartState{});
    if (deform_descs_.size() > deform_capacity_ || flora_parts_ > flora_part_capacity_) {
      flora_grow_ = true;
      relayout_pending_ = true;
    } else {
      put_deform_record(tree.deform, desc);
    }
  }
  TailTree& tree = tail_trees_[t];
  deform_instance_[tree.deform] = slot;
  // A tree that comes back starts at rest, and sways in the phase its key gives it.
  for (u32 p = 0; p < shape.part_count(); ++p)
    flora_state_[tree.first_part + p] = FloraPartState{};
  gfx::InstanceDesc& placed = instance_table_[slot];
  placed.deform = tree.deform;
  const f32 padding = flora_padding_of(mesh);
  if (padding > placed.bounds_padding) placed.bounds_padding = padding;
  flora_stats_.max_padding =
      std::max(flora_stats_.max_padding, placed.bounds_padding * placed.scale_max);
  tree.at = flora_instances_.size();
  flora_instances_.push_back(flora_instance(slot, mesh, tree.first_part, tail_motion_key(source)));
  tail_tree_of_slot_[slot] = t;
  flora_recut_ = true;
}

// Made again with the device idle (`relayout`): the deform table, its per-frame copies and the
// pose table, half as much again as the records need, every record the CPU holds copied in.
bool GpuScene::grow_flora_tail(std::string* error) {
  flora_grow_ = false;
  bool grew = false;
  if (deform_descs_.size() > deform_capacity_) {
    deform_capacity_ = deform_descs_.size() + deform_descs_.size() / 2;
    grew = true;
  }
  if (flora_parts_ > flora_part_capacity_) {
    flora_part_capacity_ = flora_parts_ + flora_parts_ / 2;
    if (!make_flora_poses(error)) return false;
  }
  flora_state_.reserve(flora_part_capacity_);
  deform_descs_.reserve(deform_capacity_);
  deform_instance_.reserve(deform_capacity_);
  deform_mesh_clusters_.reserve(deform_capacity_);
  tail_trees_.reserve(deform_capacity_ - load_deforms_);
  // The table is written whole again whether or not it grew: a record made past it was not.
  if (!make_deform_tables(grew ? deform_frames.buffer.valid() : false, error)) return false;
  flora_counts();
  return true;
}

// The records the admitted demand makes (R137): per class of run size, a mesh's trees take its own
// free records first — the newest first, the commit's given back among them — and a tree with none
// of its mesh's left takes the class's free record given back longest ago, once no frame in flight
// can read it (`acquire_tail_tree`). So a class makes new records for what its meshes' demands pass
// their own free records by, less the old free records whose own mesh never reaches them: a record
// another mesh takes that its own mesh needed sends that mesh to the class in turn, which changes
// nothing in the sum. Exact when every tree gives back before any takes (`write_slots`).
u64 GpuScene::tail_new_records(u64* new_parts) const noexcept {
  u64 unmet[33] = {};
  u64 surplus[33] = {};
  for (const u32 m : tail_touched_) {
    const u32 n = flora_meshes_[m].part_count();
    const u32 cls = n <= 1 ? 0u : static_cast<u32>(std::bit_width(n - 1u));
    const u32 demand = tail_demand_[m];
    const u32 free = tail_free_[m];
    if (demand > free) {
      unmet[cls] += demand - free;
    } else {
      // The mesh takes its newest records; what it leaves are its oldest, the old ones first.
      surplus[cls] += std::min(tail_old_[m], free - demand);
    }
  }
  u64 made = 0;
  u64 parts = 0;
  for (u32 c = 0; c < 33; ++c) {
    if (unmet[c] <= surplus[c]) continue;
    made += unmet[c] - surplus[c];
    parts += (unmet[c] - surplus[c]) << c;
  }
  if (new_parts != nullptr) *new_parts = parts;
  return made;
}

// What the tail's room is with `new_records` more records of `new_parts` parts in all: the tables
// grown as `grow_flora_tail` grows them, half as much again past the capacity, and costed as
// `flora_counts` costs them — so after `relayout` the two agree to the byte.
u64 GpuScene::tail_room_bytes(u64 new_records, u64 new_parts) const noexcept {
  const u64 entries = u64{deform_descs_.size()} + new_records;
  const u64 parts = u64{flora_parts_} + new_parts;
  const u64 entry_room = entries > deform_capacity_ ? entries + entries / 2 : deform_capacity_;
  const u64 part_room = parts > flora_part_capacity_ ? parts + parts / 2 : flora_part_capacity_;
  const u64 deform_bytes = (entry_room - load_deforms_) * sizeof(gfx::DeformDesc) *
                           (deform_frames.buffer.valid() ? u64{k_joint_slots} + 1 : u64{1});
  const u64 part_bytes = (part_room - load_flora_parts_) *
                         (u64{k_joint_slots} * sizeof(gfx::FloraPartPose) + sizeof(FloraPartState));
  return deform_bytes + part_bytes;
}

bool GpuScene::admit_tail_blocks(std::span<const SceneInstance> tail,
                                 std::span<const DynamicBlock> blocks, TileChange& change,
                                 std::string* error) {
  const u32 mesh_count = flora_meshes_.size();
  tail_demand_.assign(mesh_count, 0u);
  tail_free_.assign(mesh_count, 0u);
  tail_old_.assign(mesh_count, 0u);
  tail_touched_flag_.assign(mesh_count, u8{0});
  tail_touched_.clear();
  const auto touch = [this](u32 m) {
    if (tail_touched_flag_[m] != 0) return;
    tail_touched_flag_[m] = 1;
    tail_touched_.push_back(m);
  };
  // The free records now, by mesh, and which are old enough for another mesh of their class.
  for (u32 c = 0; c < 33; ++c) {
    for (u32 t = tail_old_head_[c]; t != ~0u; t = tail_trees_[t].old_next) {
      const TailTree& tree = tail_trees_[t];
      touch(tree.mesh);
      ++tail_free_[tree.mesh];
      if (flora_frames_ >= tree.released + u64{k_joint_slots} + 1) ++tail_old_[tree.mesh];
    }
  }
  // The records this commit gives back: every tree of a live block no request keeps as it is —
  // gone from the tail, rewritten, or withheld below — since its slots are written or freed. A kept
  // block a compaction moves gives back and takes the same meshes, which changes nothing here.
  const std::span<const TileBlock> live = layout_.blocks();
  tail_kept_.assign(static_cast<u32>(live.size()), u8{0});
  for (u32 b = 0; b < blocks.size(); ++b) {
    if (!requests_[b].same) continue;
    const TileBlock* held = layout_.find(blocks[b].key);
    if (held != nullptr) tail_kept_[static_cast<u32>(held - live.data())] = 1;
  }
  for (u32 i = 0; i < live.size(); ++i) {
    if (tail_kept_[i] != 0) continue;
    const TileBlock& held = live[i];
    for (u32 slot = held.inst_begin;
         slot < held.inst_begin + held.instances && slot < tail_tree_of_slot_.size(); ++slot) {
      const u32 t = tail_tree_of_slot_[slot];
      if (t == ~0u) continue;
      touch(tail_trees_[t].mesh);
      ++tail_free_[tail_trees_[t].mesh];
    }
  }
  // The changed blocks, each with its record-taking trees, in admission order: the block the eye
  // stands in, then by ring, then by how far its instances' footprint is from the eye, then the
  // caller's order. Before any frame has named the eye, by ring and the caller's order.
  tail_candidates_.clear();
  tail_block_meshes_.clear();
  tail_block_first_.clear();
  for (u32 b = 0; b < blocks.size(); ++b) {
    if (requests_[b].same) continue;
    TailCandidate candidate;
    candidate.block = b;
    candidate.ring = blocks[b].ring;
    f64 lo_x = 0.0, hi_x = 0.0, lo_z = 0.0, hi_z = 0.0;
    bool first = true;
    const u32 begin = tail_block_meshes_.size();
    for (const SceneInstance& source : block_instances(tail, blocks[b])) {
      const f64 x = source.origin.x + static_cast<f64>(source.transform.position.x);
      const f64 z = source.origin.z + static_cast<f64>(source.transform.position.z);
      lo_x = first || x < lo_x ? x : lo_x;
      hi_x = first || x > hi_x ? x : hi_x;
      lo_z = first || z < lo_z ? z : lo_z;
      hi_z = first || z > hi_z ? z : hi_z;
      first = false;
      if (source.mesh < mesh_count && tail_sways(source.mesh) && !flora_meshes_[source.mesh].field)
        tail_block_meshes_.push_back(source.mesh);
    }
    candidate.trees = tail_block_meshes_.size() - begin;
    if (candidate.trees == 0) continue;  // takes no record: admitted whatever the budget
    tail_block_first_.push_back(begin);
    if (tail_eye_known_) {
      const f64 dx = std::max({lo_x - tail_eye_.x, 0.0, tail_eye_.x - hi_x});
      const f64 dz = std::max({lo_z - tail_eye_.z, 0.0, tail_eye_.z - hi_z});
      candidate.distance = std::sqrt(dx * dx + dz * dz);
      candidate.eye = candidate.distance == 0.0;
    }
    tail_candidates_.push_back(candidate);
  }
  // `tail_block_first_` follows the candidates as made; the sort orders their indices.
  Vector<u32>& order = tail_order_;
  order.resize(tail_candidates_.size());
  for (u32 i = 0; i < order.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [this](u32 a, u32 b) {
    const TailCandidate& x = tail_candidates_[a];
    const TailCandidate& y = tail_candidates_[b];
    if (x.eye != y.eye) return x.eye;
    if (x.ring != y.ring) return x.ring < y.ring;
    if (x.distance != y.distance) return x.distance < y.distance;
    return x.block < y.block;
  });
  tail_admit_.assign(static_cast<u32>(blocks.size()), u8{1});
  bool withholding = false;
  bool admitted_trees = false;
  u32 withheld_tiles = 0;
  u32 withheld_instances = 0;
  for (const u32 i : order) {
    const TailCandidate& candidate = tail_candidates_[i];
    const u32 begin = tail_block_first_[i];
    if (!withholding) {
      for (u32 k = 0; k < candidate.trees; ++k) {
        const u32 m = tail_block_meshes_[begin + k];
        touch(m);
        ++tail_demand_[m];
      }
      u64 parts = 0;
      const u64 made = tail_new_records(&parts);
      const u64 room = tail_room_bytes(made, parts);
      if (made == 0 || room <= tail_budget_bytes_) {
        admitted_trees = true;
        continue;
      }
      for (u32 k = 0; k < candidate.trees; ++k)
        --tail_demand_[tail_block_meshes_[begin + k]];
      if (!admitted_trees) {
        // **The nearest changed block alone passes the budget**: the one refusal left, before
        // anything changes, with a sentence naming the bytes. The device lives either way.
        ++flora_stats_.tail_refused;
        if (error != nullptr) {
          *error =
              std::string(
                  "the streamed trees' room would grow past renderer.flora.tail_budget_mib (") +
              std::to_string(tail_budget_bytes_) +
              " bytes) with the nearest changed tile "
              "alone: its " +
              std::to_string(candidate.trees) + " swaying trees would take " +
              std::to_string(made) + " new records and the room to " + std::to_string(room) +
              " bytes; the commit is refused and its tiles are not placed";
        }
        return false;
      }
      withholding = true;
    }
    tail_admit_[candidate.block] = 0;
    ++withheld_tiles;
    withheld_instances += blocks[candidate.block].count;
  }
  u64 parts = 0;
  const u64 made = tail_new_records(&parts);
  flora_stats_.tail_bound_bytes = tail_room_bytes(made, parts);
  tail_bound_records_ = u64{tail_trees_.size()} + made;
  change.withheld_tiles = withheld_tiles;
  change.withheld_instances = withheld_instances;
  // Said once per change of what is withheld, as the pair budget's withholds are: never an error.
  if (withheld_tiles != flora_stats_.tail_withheld_tiles ||
      withheld_instances != flora_stats_.tail_withheld_instances) {
    ENGINE_LOG_INFO(log_renderer, "streamed tiles withheld by the trees' budget",
                    log::field("withheld_tiles", withheld_tiles),
                    log::field("withheld_instances", withheld_instances),
                    log::field("budget_bytes", tail_budget_bytes_),
                    log::field("room_bytes", flora_stats_.tail_bound_bytes));
  }
  flora_stats_.tail_withheld_tiles = withheld_tiles;
  flora_stats_.tail_withheld_instances = withheld_instances;
  if (withheld_tiles == 0) return true;
  ++flora_stats_.tail_withholding_commits;
  // The admitted blocks, and their requests, in the caller's order.
  admitted_.clear();
  u32 w = 0;
  for (u32 b = 0; b < blocks.size(); ++b) {
    if (tail_admit_[b] == 0) continue;
    requests_[w++] = requests_[b];
    admitted_.push_back(blocks[b]);
  }
  requests_.resize(w);
  withheld_any_ = true;
  return true;
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
  bool own = !blocks.empty();  // every block names its own instances: no tail is handed over
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
    own = own && (block.instances != nullptr || block.count == 0);
  }
  if (next != tail.size() && !(own && tail.empty())) {
    if (error != nullptr) {
      *error = "the tail's blocks cover " + std::to_string(next) + " of its " +
               std::to_string(tail.size()) + " instances";
    }
    return false;
  }
  i64 mark = time::monotonic_ns();
  const auto lap = [&mark](i64& into) {
    const i64 now = time::monotonic_ns();
    into += now - mark;
    mark = now;
  };
  requests_.resize(static_cast<u32>(blocks.size()));
  for (u32 b = 0; b < blocks.size(); ++b) {
    const DynamicBlock& block = blocks[b];
    const std::span<const SceneInstance> run = block_instances(tail, block);
    TileLayout::Request& request = requests_[b];
    request = TileLayout::Request{};
    request.key = block.key;
    request.first = block.first;
    request.instances = block.count;
    request.version = block.version;
    // **A block the caller says is unchanged costs nothing** (W81): the same key, count and
    // version the layout holds is the same block, and none of its instances is read. Every block a
    // caller names no version for is compared instance by instance, as before; a slot's key names
    // only a mesh the scene has, so a block that matches needs no other check and its pairs are
    // what it held.
    const TileBlock* held = layout_.find(block.key);
    if (held != nullptr && held->instances == block.count) {
      bool same = block.version != 0 && held->version == block.version;
      if (!same) {
        same = true;
        change.compared += block.count;
        for (u32 k = 0; k < block.count && same; ++k) {
          const SceneInstance& source = run[k];
          same = source.joints == 0 && tail_keys_[held->inst_begin + k] == key_of(source);
        }
      }
      if (same) {
        request.same = true;
        request.pairs = held->pairs;
        continue;
      }
    }
    u64 pairs = 0;
    for (u32 k = 0; k < block.count; ++k) {
      const u32 i = block.first + k;
      const SceneInstance& source = run[k];
      gfx::InstanceDesc probe;
      // The placement's own checks: a mesh the scene has not is refused with its sentence — and
      // a mesh of the room that is not resident, which no tile may name before it is admitted.
      if (source.mesh >= data_->parts.size() && !mesh_resident(source.mesh)) {
        if (tail_mesh(source.mesh)) {
          if (error != nullptr) {
            *error = "tail instance " + std::to_string(i) + " names mesh " +
                     std::to_string(source.mesh) + ", a mesh slot with no mesh admitted";
          }
          return false;
        }
        if (!make_instance(*data_, source, 0, probe, error)) return false;
      }
      // **A deformed tail instance** — skinned, or of a mesh with morph channels, which deforms
      // whatever the flags say — is the next stage (renderer.md, "The pool and the chain over
      // the tail"): the deform table is laid out once, for the load's instances, so a tail
      // instance has no entry to point at, and drawn rigid it would stand at its bind pose.
      // Refused alone, by name, before the layout is touched.
      const geometry::ClusterMeshPart& part = part_of(source.mesh);
      if (source.joints != 0 || part.morph_channel_count != 0) {
        if (error != nullptr) {
          *error =
              "tail instance " + std::to_string(i) + " (mesh " + std::to_string(source.mesh) +
              (source.joints != 0 ? ", " + std::to_string(source.joints) + " joints) is skinned"
                                  : ", " + std::to_string(part.morph_channel_count) +
                                        " morph channels) is morphed") +
              ": an instance added after the load is drawn rigid, and a deformed one needs "
              "an entry in the deform table the scene lays out once";
        }
        return false;
      }
      pairs += part.cluster_count;
    }
    request.pairs = pairs > k_max_pairs ? k_max_pairs + 1u : static_cast<u32>(pairs);
  }
  // **A commit past the tail's budget withholds its farthest tiles** (renderer.md; R95, R137): the
  // records the changed blocks' swaying trees would make — counted once, a record a tree gives back
  // in this commit taken first by the next tree of its mesh — against
  // `renderer.flora.tail_budget_mib`, before anything changes, the nearest blocks first; a block
  // that would pass it is withheld with every farther one, and only a commit whose nearest changed
  // block alone passes it is refused. Grass takes no record and costs it nothing.
  // The first fill — nothing held before this change — stamps its blocks 0 (G136): a scene that
  // opens on a meadow draws it whole from its first frame. Stamps of blocks the caller no longer
  // names are dropped once they outnumber the held ones.
  first_fill_ = layout_.block_count() == 0;
  if (block_stamps_.size() > 2 * blocks.size() + 256) {
    HashMap<u64, BlockStamp> kept;
    for (const DynamicBlock& block : blocks) {
      const auto found = block_stamps_.find(block.key);
      if (found != block_stamps_.end()) kept[block.key] = (*found).second;
    }
    block_stamps_.swap(kept);
  }
  withheld_any_ = false;
  if (flora_tail_parts_) {
    if (!admit_tail_blocks(tail, blocks, change, error)) return false;
    if (withheld_any_) blocks = std::span<const DynamicBlock>(admitted_.data(), admitted_.size());
  }
  lap(change.compare_ns);
  if (!layout_.apply(std::span<const TileLayout::Request>(requests_.data(), requests_.size()),
                     compact_pct_, compact, pair_limit_, writes_, change.layout, error)) {
    return false;
  }
  lap(change.layout_ns);
  // Committed: the slots the layout wrote, then the buffers grown to hold them.
  if (!write_slots(tail, blocks, std::span<const SlotWrite>(writes_.data(), writes_.size()),
                   change.slots_written, error) ||
      !fit_tables(change, retired, error)) {
    return false;
  }
  lap(change.write_ns);
  if (change.layout.compacted) ++compactions_;
  if (change.slots_written > 0 || change.grew) tables_changed_ = true;
  // The drop order follows the tables (renderer.md, "The ray tracing chain's memory").
  if (ray_tracing_ && (change.slots_written > 0 || change.grew)) lay_out_rt_order();
  // The policy counts followed the slots as they were written, and the ones trimmed off the end
  // (`policy_count`, W81): until 2026-10-31 every change counted the whole instance table again,
  // twice when a tree swayed in.
  ENGINE_ASSERT(policy_counts_match(), "the policy counts kept slot by slot are the recount's");
  // The bound counted the records the commit makes; it may never have counted fewer (R137).
  ENGINE_ASSERT(
      !flora_tail_parts_ || tail_trees_.size() <= tail_bound_records_,
      "the tail's budget counted every record the commit made");  // The poser's chunks follow the
                                                                  // trees, cut again in place
                                                                  // (renderer.md, "Streamed trees
                                                                  // sway").
  if (flora_recut_) {
    flora_recut_ = false;
    flora_poser_.recut(
        std::span<const FloraInstance>(flora_instances_.data(), flora_instances_.size()),
        std::span<const FloraMesh>(flora_meshes_.data(), flora_meshes_.size()));
    flora_counts(false);
  }
  lap(change.after_ns);
  return true;
}

bool GpuScene::write_slots(std::span<const SceneInstance> tail,
                           std::span<const DynamicBlock> blocks, std::span<const SlotWrite> writes,
                           u32& written, std::string* error) {
  written = 0;
  const u32 end = layout_.instance_end();
  // A tree past the layout's new end gives its record back before its slot goes.
  if (flora_tail_) {
    for (u32 slot = end; slot < instance_table_.size(); ++slot)
      release_tail_tree(slot);
    if (tail_tree_of_slot_.size() < end) tail_tree_of_slot_.resize(end, ~0u);
  }
  for (u32 slot = end; slot < instance_table_.size(); ++slot) {
    uncount_static(slot);
    policy_count(instance_table_[slot], false);
  }
  // **Every tree the change moves gives its record back before any takes one** (R137), so a tree
  // that leaves one tile and a tree of its mesh that arrives in another share a record whatever
  // order the layout wrote their slots in, and the tail's budget can count the commit's records
  // once (`tail_new_records`). Until 2026-11-07 each slot gave back and took in turn.
  if (flora_tail_) {
    for (const SlotWrite& write : writes) {
      for (u32 k = 0; k < write.inst_count; ++k)
        release_tail_tree(write.inst_begin + k);
    }
  }
  const u32 had = instance_table_.size();
  instance_table_.resize(end);
  tail_keys_.resize(end);
  // The policy counts follow the table slot by slot (W81): a slot made here counts as what it holds
  // until it is written below, as `policy_counts` would count it.
  for (u32 slot = had; slot < end; ++slot)
    policy_count(instance_table_[slot], true);
  for (const SlotWrite& write : writes) {
    if (write.inst_count == 0) continue;
    u32 first_pair = write.pair_begin;
    const DynamicBlock* block = write.source < blocks.size() ? &blocks[write.source] : nullptr;
    for (u32 k = 0; k < write.inst_count; ++k) {
      const u32 slot = write.inst_begin + k;
      // Whatever tree the slot held gives its record back; the instance it now holds takes one
      // if its mesh sways in the tail (renderer.md, "Streamed trees sway").
      if (flora_tail_) release_tail_tree(slot);
      uncount_static(slot);
      policy_count(instance_table_[slot], false);
      if (block != nullptr && k < write.live) {
        const SceneInstance& source = block_instances(tail, *block)[k];
        gfx::InstanceDesc& desc = instance_table_[slot];
        // A mesh of the room is placed as it was built: no fit (renderer.md, "Meshes that come
        // and go"). Checked resident before the layout was touched.
        if (source.mesh >= data_->parts.size() && mesh_resident(source.mesh)) {
          place_instance(source, Mat4::identity(), first_pair, desc);
        } else if (!make_instance(*data_, source, first_pair, desc, error)) {
          return false;
        }
        // A patch's hand-off role carries the tick its tile's instances came at (G136; renderer.md,
        // "The hand-off by distance"), so a tile that arrives or moves inward fades its blades in.
        if (source.handoff != 0) {
          gfx::set_instance_handoff(desc, desc.flags & gfx::k_instance_handoff_mask, desc.band,
                                    block_stamp(*block));
        }
        if (source.mesh < mesh_policy_.size()) desc.flags |= mesh_policy_[source.mesh];
        desc.material_base =
            source.mesh < mesh_material_base_.size() ? mesh_material_base_[source.mesh] : 0u;
        // A rigid form's instance references its mesh's structure, built once (renderer.md,
        // "Static structures for rigid forms").
        // Its rank in the drop order: the ring its tile is drawn in, past the prefix's 0.
        if (slot_rank_.size() <= slot) slot_rank_.resize(slot + 1, 0u);
        slot_rank_[slot] = block->ring + 1;
        if (static_build_ && static_mesh(source.mesh)) {
          desc.flags |= gfx::k_instance_static_rt;
          ++static_instances_live_;
          static_pairs_live_ += part_of(source.mesh).cluster_count;
        }
        tail_keys_[slot] = key_of(source);
        first_pair += part_of(source.mesh).cluster_count;
        if (flora_tail_ && tail_sways(source.mesh)) acquire_tail_tree(slot, source);
      } else {
        // Past the live instances: a null instance at the pairs' end, so a pair of the slack lands
        // on it and the slots stay sorted by `first_pair`.
        instance_table_[slot] = null_instance(first_pair);
        tail_keys_[slot] = InstanceKey{};
      }
      policy_count(instance_table_[slot], true);
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
    if (stride > pair_limit_) stride = pair_limit_;
    retire(visible, retired);
    retire(shadow_args, retired);
    retire(pair_entries, retired);
    retire(fade, retired);
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
    // The tail's trees' per-slot arrays follow the slots, so tiles that come and go inside the
    // capacity never allocate.
    if (flora_tail_) {
      tail_tree_of_slot_.reserve(instance_capacity_);
      flora_instances_.reserve(load_flora_ + instance_capacity_);
    }
  }
  // The pool's per-entry slots follow the stride and the chain's tables the stride and the
  // instance capacity; both are read by the frames in flight by address, so they are made again
  // with the device idle (`relayout`, which the caller runs before the next frame). Rare: the
  // stride and the capacity grow half as much again each time.
  if (sets_again && (deform_ || ray_tracing_)) relayout_pending_ = true;
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
  out.rt_copies.clear();
  out.pair_copies.clear();
  out.pair_source = gfx::BufferResource{};
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
    // The terrain levels' slots' pairs, written once into a set that is written whole.
    if (ring_pairs_.buffer.valid()) {
      out.pair_source = ring_pairs_;
      out.pair_copies.push_back(
          gfx::BufferCopy{0, u64{ring_pair_begin_} * 2u * sizeof(u32),
                          u64{static_pairs_ - ring_pair_begin_} * 2u * sizeof(u32)});
    }
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
      expand += tail_mesh(instance_table_[i].mesh) ? 1u : 0u;
  }
  if (slot_count == 0) {
    runs.clear();
    return true;
  }
  const u64 instance_bytes = u64{slot_count} * sizeof(gfx::InstanceDesc);
  // With the chain, the same slots' top-level records after the list (renderer.md, "Instances that
  // come and go"): a block's arrival, rewrite or departure writes its own records and no other.
  const u64 records_at = (instance_bytes + u64{expand} * sizeof(u32) + 63) / 64 * 64;
  const u64 record_bytes = ray_tracing_ && rt_instances.buffer.valid()
                               ? u64{slot_count} * gfx::k_instance_record_bytes
                               : 0;
  const u64 bytes = records_at + record_bytes;
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
  u64 record_at = records_at;
  u32 listed = 0;
  u64 pairs = 0;
  for (const InstanceRange& run : runs) {
    const u64 run_bytes = u64{run.end - run.begin} * sizeof(gfx::InstanceDesc);
    std::memcpy(base + at, instance_table_.data() + run.begin, static_cast<usize>(run_bytes));
    out.copies.push_back(
        gfx::BufferCopy{at, u64{run.begin} * sizeof(gfx::InstanceDesc), run_bytes});
    at += run_bytes;
    if (record_bytes > 0) {
      const u64 run_records = u64{run.end - run.begin} * gfx::k_instance_record_bytes;
      for (u32 i = run.begin; i < run.end; ++i) {
        const gfx::TlasInstance record = tlas_record(i);
        gfx::write_instances(std::span<const gfx::TlasInstance>(&record, 1),
                             base + record_at + u64{i - run.begin} * gfx::k_instance_record_bytes);
      }
      out.rt_copies.push_back(
          gfx::BufferCopy{record_at, u64{run.begin} * gfx::k_instance_record_bytes, run_records});
      record_at += run_records;
    }
    for (u32 i = run.begin; i < run.end; ++i) {
      const u32 mesh = instance_table_[i].mesh;
      if (!tail_mesh(mesh)) continue;
      list[listed++] = i;
      pairs += part_of(mesh).cluster_count;
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

namespace {

// A frame slot's record staging: half as much again as asked for, in 64 KiB steps, and never less
// than 64 KiB, which is 680 instance records — the forest's 208 animals moving every frame use
// 20 KiB of it.
u64 record_staging_size(u64 bytes) noexcept {
  const u64 want = bytes + bytes / 2;
  return std::max<u64>(64 * 1024, (want + 65535) / 65536 * 65536);
}

// Sorted, without repeats, in place: a list that only grows to what it held before allocates
// nothing.
void sort_unique(Vector<u32>& list) {
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());
}

}  // namespace

bool GpuScene::reserve_record_staging(u32 frames_in_flight, std::string* error) {
  // A dynamic scene's instance records ride its table sets; only its ring slots' mesh records come
  // through here. A scene read whole can move any of its instances.
  if (device_ == nullptr || (dynamic_ && ring_slots_.empty())) return true;
  // The most a frame of ring swaps writes: every slot's mesh and instance records, off and on.
  u32 all_slots = 0;
  for (const RingSlots& ring : ring_slots_)
    all_slots += ring.slots.size();
  const u64 size = record_staging_size(
      u64{2} * all_slots * (sizeof(gfx::MeshDesc) + sizeof(gfx::InstanceDesc)) + 16);
  if (record_staging_.size() < frames_in_flight) record_staging_.resize(frames_in_flight);
  for (gfx::BufferResource& staging : record_staging_) {
    if (staging.size >= size) continue;
    gfx::destroy_buffer(*device_, staging);
    if (!gfx::create_buffer(*device_, size, gfx::BufferUsage::TransferSrc, true, staging, error))
      return false;
    // Touched once, here, as the terrain's ring is: a mapped page first written inside a frame is a
    // fault the frame's thread pays for (renderer.md, "What a frame waits for").
    std::memset(staging.mapped, 0, static_cast<usize>(size));
  }
  return true;
}

bool GpuScene::prepare_records(u32 slot, RecordUpdate& out, std::string* error) {
  out.staging = gfx::BufferResource{};
  out.instances.clear();
  out.meshes.clear();
  out.bytes = 0;
  if (!instances.buffer.valid()) pending_instance_writes_.clear();
  if (!meshes.buffer.valid()) pending_mesh_writes_.clear();
  if (pending_instance_writes_.empty() && pending_mesh_writes_.empty()) return true;
  // Each record once, with the state it ends in: a slot moved twice since the last frame, or turned
  // off and on, is one record.
  sort_unique(pending_instance_writes_);
  sort_unique(pending_mesh_writes_);
  // A frame has at most a region per record, so the region lists are kept at the record lists'
  // capacity — which for a ring's slots is reserved at load for every slot off and on — and a frame
  // with more runs than any before it allocates nothing (the frame loop test's second flight over
  // the endless desert found the one allocation a list grown on demand made).
  out.instances.reserve(pending_instance_writes_.capacity());
  out.meshes.reserve(pending_mesh_writes_.capacity());
  const u64 instance_bytes = u64{pending_instance_writes_.size()} * sizeof(gfx::InstanceDesc);
  const u64 mesh_at = (instance_bytes + 15) & ~u64{15};
  const u64 bytes = mesh_at + u64{pending_mesh_writes_.size()} * sizeof(gfx::MeshDesc);
  if (slot >= record_staging_.size()) record_staging_.resize(slot + 1);
  gfx::BufferResource& staging = record_staging_[slot];
  if (staging.size < bytes) {
    // The slot's last frame is done, so its staging can go now (as a dynamic scene's table staging
    // does, `prepare_tables`). A frame that writes more records than any before it pays for this
    // once; a steady stream of moves never does.
    gfx::destroy_buffer(*device_, staging);
    if (!gfx::create_buffer(*device_, record_staging_size(bytes), gfx::BufferUsage::TransferSrc,
                            true, staging, error)) {
      return false;
    }
  }
  auto* base = static_cast<u8*>(staging.mapped);
  // One copy region per run of consecutive records, so a move of every instance is one region
  // rather than one per instance (a copy is encoded region by region on the frame's thread:
  // docs/experiments/frame-thread-spikes-2026-10-04.md).
  const auto stage = [&](const Vector<u32>& list, u64 at, u64 size, const void* from,
                         Vector<gfx::BufferCopy>& copies) {
    for (u32 k = 0; k < list.size(); ++k) {
      std::memcpy(base + at + u64{k} * size, static_cast<const u8*>(from) + u64{list[k]} * size,
                  static_cast<usize>(size));
      if (k > 0 && list[k] == list[k - 1] + 1) {
        copies.back().size += size;
      } else {
        copies.push_back(gfx::BufferCopy{at + u64{k} * size, u64{list[k]} * size, size});
      }
    }
  };
  stage(pending_instance_writes_, 0, sizeof(gfx::InstanceDesc), instance_table_.data(),
        out.instances);
  stage(pending_mesh_writes_, mesh_at, sizeof(gfx::MeshDesc), mesh_descs_.data(), out.meshes);
  record_uploads_ += pending_instance_writes_.size() + pending_mesh_writes_.size();
  record_upload_bytes_ += bytes;
  pending_instance_writes_.clear();
  pending_mesh_writes_.clear();
  out.staging = staging;
  out.bytes = bytes;
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
        if (!tail_mesh(desc.mesh) || desc.first_pair != pair) {
          return fail("block " + std::to_string(block.key) + "'s slot " + std::to_string(k) +
                      " is not its instance");
        }
        pair += part_of(desc.mesh).cluster_count;
      } else if (desc.mesh != null_mesh_ || desc.first_pair != block.pair_begin + block.pairs) {
        return fail("block " + std::to_string(block.key) + "'s slack is not null at its end");
      }
    }
    if (pair != block.pair_begin + block.pairs) return fail("a block's pairs are not its count");
    live += block.instances;
  }
  u32 named = 0;
  for (u32 i = static_instances_; i < instance_count_; ++i)
    named += tail_mesh(instance_table_[i].mesh) ? 1u : 0u;
  if (named != live) return fail("a slot outside every block names an instance");
  return true;
}

bool GpuScene::upload_geometry(const ResolvedSettings& resolved, std::string* error) {
  ENGINE_STARTUP_STAGE("upload geometry");
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
  // **A rigid form is drawn at its frontier** (renderer.md, "Static structures for rigid forms"):
  // with static structures, the cut the cull selects for such a form's clusters is the frontier
  // at every distance — the frontier's parent error is made terminal, so it never coarsens, and
  // every other cluster's own error terminal, so it never draws — which is what makes the form a
  // fixed cut its structures can be built once for. Only the GPU's copy; the scene's stays the
  // form `extract_lod_form` cut.
  const geometry::ClusterLodDesc* lod_data = lod.lod.data();
  Vector<geometry::ClusterLodDesc> fixed_lods;
  if (static_forms_) {
    fixed_lods.resize(lod.lod.size());
    std::memcpy(fixed_lods.data(), lod.lod.data(),
                u64{lod.lod.size()} * sizeof(geometry::ClusterLodDesc));
    for (u32 m = 0; m < static_of_mesh_.size(); ++m) {
      if (static_of_mesh_[m] == ~0u) continue;
      const geometry::ClusterMeshPart& part = data_->parts[m];
      for (u32 i = 0; i < part.cluster_count; ++i) {
        geometry::ClusterLodDesc& desc = fixed_lods[part.first_cluster + i];
        if (desc.level == 0) {
          desc.parent_error = geometry::k_lod_terminal_error;
        } else {
          desc.own_error = geometry::k_lod_terminal_error;
        }
      }
    }
    lod_data = fixed_lods.data();
  }
  if (!upload_room(device, lod.mesh.clusters.data(), scene_clusters * sizeof(geometry::ClusterDesc),
                   u64{cluster_count_} * sizeof(geometry::ClusterDesc), cluster_usage, clusters,
                   error) ||
      !upload_room(device, lod_data, scene_clusters * sizeof(geometry::ClusterLodDesc),
                   u64{cluster_count_} * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                   error)) {
    return false;
  }
  // Everything below this line is a *payload* stream: under streaming it is a page pool of
  // fixed-size slots that `create_streaming` already allocated, and nothing is uploaded here. A
  // ring vertex v's three u16 are at byte 6 v like the scene's, and the last one is read as two
  // words, so the arena's end keeps four bytes of room.
  const bool ring_room = vertex_capacity_ > scene_vertex_count_;  // rings' arenas, or the mesh room
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
    // **The trees' motion, per mesh** (renderer.md, "The flora stage"): the contract as the stage
    // reads it, the sway's bound on how far a vertex leaves its rest position, and the rest bounds
    // the push's test starts from. A mesh whose table breaks the contract stays rigid, said once.
    flora_meshes_.clear();
    flora_bounds_.clear();
    if (resolved.flora_motion) {
      flora_meshes_.resize(data_->parts.size());
      flora_bounds_.resize(data_->parts.size(), Vec4{});
      for (u32 m = 0; m < data_->parts.size() && m < data_->sources.size(); ++m) {
        const SourceMesh& source = data_->sources[m];
        if (!source.has_motion()) continue;
        // A contract that does not move (the cover's mats, `FloraMotionKind::none`) is drawn rigid
        // and is no tree refused: no record, no warning.
        if (const geometry::FloraSpeciesInfo* info = geometry::flora_species_info(source.species);
            info != nullptr && info->motion == geometry::FloraMotionKind::none) {
          continue;
        }
        const geometry::ClusterMeshPart& part = data_->parts[m];
        const u32 end =
            m + 1 < data_->parts.size() ? data_->parts[m + 1].first_vertex : total_vertices;
        const u32 mesh_vertices = end - part.first_vertex;
        if (mesh_vertices != source.motion_vertices.size() ||
            !flora_mesh(std::span<const geometry::ClusterFileMotionPart>(
                            source.motion_parts.data(), source.motion_parts.size()),
                        std::span<const geometry::ClusterFileStructuralPart>(
                            source.structural_parts.data(), source.structural_parts.size()),
                        source.species, flora_meshes_[m],
                        std::span<const u32>(source.motion_part_ids.data(),
                                             source.motion_part_ids.size()))) {
          flora_meshes_[m] = FloraMesh{};
          // Named so the next reader need not guess (roadmap W78): which mesh, from where, its
          // vertices against its motion records, its parts, its species, and which test failed.
          const bool counts = mesh_vertices != source.motion_vertices.size();
          const char* why = counts ? "its motion records do not cover its vertices"
                            : geometry::flora_species_info(source.species) == nullptr
                                ? "its species is not in the flora catalogue"
                            : source.motion_parts.size() > 65536u
                                ? "more than 65536 motion parts"
                                : "a motion part's parent follows it";
          ENGINE_LOG_WARN(log_renderer, "a tree drawn rigid", log::field("mesh", m),
                          log::field("name", m < data_->mesh_info.size() ? data_->mesh_info[m].name
                                                                         : std::string()),
                          log::field("path", source.path), log::field("vertices", mesh_vertices),
                          log::field("motion_vertices", source.motion_vertices.size()),
                          log::field("parts", source.motion_parts.size()),
                          log::field("species", source.species), log::field("reason", why));
          continue;
        }
        Vec3 lo = lod.mesh.vertices[part.first_vertex];
        Vec3 hi = lo;
        for (u32 v = part.first_vertex; v < end; ++v) {
          lo = min(lo, lod.mesh.vertices[v]);
          hi = max(hi, lod.mesh.vertices[v]);
        }
        flora_bounds_[m] = Vec4{(lo + hi) * 0.5f, length(hi - lo) * 0.5f};
      }
    }
    flora_instances_.clear();
    flora_parts_ = 0;
    flora_padding_.assign(flora_meshes_.size(), -1.0f);  // computed for a mesh once
    // **A streamed world's trees sway too** (renderer.md, "Streamed trees sway"): when a mesh the
    // tail may name carries the motion contract, the tail's trees get records of their own past
    // the load's, made and given back as their tiles come and go.
    flora_tail_ = false;
    flora_tail_parts_ = false;
    if (dynamic_ && resolved.flora_motion && flora_tail_tunable()) {
      for (u32 m = 0; m < flora_meshes_.size(); ++m) {
        if (!tail_sways(m)) continue;
        flora_tail_ = true;
        if (!flora_meshes_[m].field) flora_tail_parts_ = true;
      }
    }
    // **The field kind** (renderer.md, "The field kind"; R95): every mesh posed as a field gets one
    // deform record, made here whether or not the load places it, which every instance of it — the
    // load's and every tile's — points at: the record is a function of the mesh alone (its first
    // vertex, its run of the scene's blade table, its compliance), and the phase a blade sways in
    // is the GPU's, from the instance's placement. So the tail's grass takes no record, no part of
    // the pose table and no push state: what a tile of a meadow costs the room is nothing.
    field_deform_.assign(flora_meshes_.size(), ~0u);
    flora_field_ = false;
    flora_stats_.field_meshes = 0;
    {
      Vector<gfx::FloraFieldPart> blades;
      Vector<u32> first_blade(flora_meshes_.size(), 0u);
      for (u32 m = 0; m < flora_meshes_.size(); ++m) {
        const FloraMesh& mesh = flora_meshes_[m];
        if (mesh.part_count() == 0 || !mesh.field) continue;
        first_blade[m] = blades.size();
        for (const geometry::ClusterFileMotionPart& part : mesh.parts)
          blades.push_back(gfx::FloraFieldPart{part.pivot, part.length});
        flora_field_ = true;
      }
      if (flora_field_) {
        if (!gfx::upload_buffer(device, blades.data(),
                                u64{blades.size()} * sizeof(gfx::FloraFieldPart), k_address,
                                flora_field_parts_, error)) {
          return false;
        }
        for (u32 m = 0; m < flora_meshes_.size(); ++m) {
          const FloraMesh& mesh = flora_meshes_[m];
          if (mesh.part_count() == 0 || !mesh.field) continue;
          gfx::DeformDesc desc{};
          desc.stages = gfx::k_deform_stage_flora | gfx::k_deform_flora_field;
          desc.first_vertex = data_->parts[m].first_vertex;
          desc.joint_count = mesh.part_count();
          desc.joints =
              flora_field_parts_.address + u64{first_blade[m]} * sizeof(gfx::FloraFieldPart);
          desc.first_channel = std::bit_cast<u32>(mesh.field_compliance);
          field_deform_[m] = deform_descs_.size();
          deform_descs_.push_back(desc);
          deform_instance_.push_back(0u);  // shared: no instance owns it, and no stage asks
          deform_mesh_clusters_.push_back(data_->parts[m].cluster_count);
          ++flora_stats_.field_meshes;
        }
      }
    }
    // **The water bodies the pool lifts** (S100; water.md, "The mesh"): every body with a depth map
    // and a wind (or an open one with a tide) gets its height function — a `WaterSurface` and its
    // arrival field — and a deform record of the water stage; a body with neither stays rigid, the
    // instruction-for-instruction path it always drew on.
    water_surface_of_.assign(instance_count_, ~0u);
    water_deforms_.clear();
    {
      Vector<gfx::WaterSurface> surfaces;
      for (u32 i = 0; i < instance_count_ && i < data_->instances.size(); ++i) {
        const u32 m = data_->instances[i].mesh;
        if (m >= data_->mesh_info.size() || !data_->mesh_info[m].water.has_value()) continue;
        const SceneWater& water = *data_->mesh_info[m].water;
        if (water.depth.size < 2 || water.channel) continue;
        const WaterSwashNumbers n = water_swash_numbers(water);
        if (!(n.run_up > 0.0) && !(n.tide > 0.0f)) continue;
        gfx::WaterSurface surface;
        Vector<f32> field;
        water_surface_of(water, n, surface, field);
        gfx::BufferResource buffer;
        if (!gfx::upload_buffer(device, field.data(), u64{field.size()} * sizeof(f32), k_address,
                                buffer, error)) {
          return false;
        }
        surface.field = buffer.address;
        water_fields_.push_back(buffer);
        water_surface_of_[i] = surfaces.size();
        // The spheres take the lift's reach: the bank's highest ground (a parked vertex), the
        // waves, the bore and the tide.
        f32 waves = surface.height_m * 2.0f + surface.run_up_m + surface.tide_m;
        for (const gfx::WaterWave& wave : water.waves)
          waves += wave.amplitude;
        waves += surface.amplitude * 2.1f;
        const f32 padding = water.depth.bank_max + waves + 0.5f;
        if (padding > instance_table_[i].bounds_padding)
          instance_table_[i].bounds_padding = padding;
        surfaces.push_back(surface);
        ENGINE_LOG_INFO(
            log_renderer, "water lifted", log::field("body", water.body.name),
            log::field("height_m", surface.height_m), log::field("period_s", surface.period_s),
            log::field("run_up_m", surface.run_up_m), log::field("field_texels", water.depth.size),
            log::field("padding_m", padding));
      }
      if (!surfaces.empty() && !gfx::upload_buffer(device, surfaces.data(),
                                                   u64{surfaces.size()} * sizeof(gfx::WaterSurface),
                                                   k_address, water_surfaces_, error)) {
        return false;
      }
    }
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
      const u32 mesh_index = instance_table_[i].mesh;
      const bool flora_level = !ring_slot && !terrain_level && instance_joints == 0 &&
                               mesh_index < flora_meshes_.size() &&
                               flora_meshes_[mesh_index].part_count() > 0;
      const bool water_level = !ring_slot && !terrain_level && !flora_level &&
                               instance_joints == 0 && i < water_surface_of_.size() &&
                               water_surface_of_[i] != ~0u;
      if (instance_joints == 0 && part.morph_channel_count == 0 && !resolved.settings.deform &&
          !terrain_level && !flora_level && !water_level)
        continue;
      const u32 next = ring_slot ? part.first_vertex + part.cluster_count * 64u
                       : instance_table_[i].mesh + 1 < data_->parts.size()
                           ? data_->parts[instance_table_[i].mesh + 1].first_vertex
                           : total_vertices;
      if (flora_level && flora_meshes_[mesh_index].field) {
        // Grass: its mesh's shared record, its spheres padded by the sway's bound as a tree's are.
        gfx::InstanceDesc& placed = instance_table_[i];
        const f32 padding = flora_padding_of(mesh_index);
        if (padding > placed.bounds_padding) placed.bounds_padding = padding;
        placed.deform = field_deform_[mesh_index];
        whole_mesh_vertices += next - part.first_vertex;
        flora_stats_.max_padding =
            std::max(flora_stats_.max_padding, placed.bounds_padding * placed.scale_max);
        continue;
      }
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
      } else if (flora_level) {
        // A tree runs the flora stage and nothing else: its record names its run of the frame's
        // part-pose table, and its spheres take the sway's bound.
        const FloraMesh& tree = flora_meshes_[mesh_index];
        desc.stages = gfx::k_deform_stage_flora;
        desc.first_part = flora_parts_;
        desc.joint_count = tree.part_count();
        gfx::InstanceDesc& placed = instance_table_[i];
        const f32 padding = flora_padding_of(mesh_index);
        if (padding > placed.bounds_padding) placed.bounds_padding = padding;
        const u32 key = i < data_->motion_keys.size() ? data_->motion_keys[i] : i;
        flora_instances_.push_back(flora_instance(i, mesh_index, flora_parts_, key));
        flora_parts_ += tree.part_count();
        flora_stats_.max_padding =
            std::max(flora_stats_.max_padding, placed.bounds_padding * placed.scale_max);
      } else if (water_level) {
        // A water body runs the water stage and nothing else: its record names its surface, and a
        // frame writes its clock and its tide's phase into its own copy (scene_renderer.cpp).
        desc.stages = gfx::k_deform_stage_water;
        desc.joints =
            water_surfaces_.address + u64{water_surface_of_[i]} * sizeof(gfx::WaterSurface);
        water_deforms_.push_back(deform_descs_.size());
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
    // Each view's runs and each shadow cascade's cut deform their own copies of the pairs they
    // hold, so the clamp that keeps a small scene from ever overflowing allows the whole meshes
    // once per view and once per cascade. Within one view a pair is drawn, a shadow caster or
    // neither — never two of them — so the caster run shares its view's allowance. Until
    // 2026-10-18 the views were left out (one allowance for every view), which a surround's
    // second and third view could overflow on a scene small enough to promise it never would.
    // A tail that may carry trees takes the budget unclamped: no load can bound what its tiles will
    // bring, as no load can bound its index budget (renderer.md, "Streamed trees sway").
    deform_pool_vertices_ = pool_budget_vertices(
        resolved,
        flora_tail_ ? ~u64{0} : whole_mesh_vertices * (u64{view_count_} + u64{cascade_count_}));
    // The load's records, and room past them for the tail's trees (a free list per mesh, grown
    // between frames by `relayout`).
    load_deforms_ = deform_descs_.size();
    load_flora_ = flora_instances_.size();
    load_flora_parts_ = flora_parts_;
    // A tail whose swaying meshes are all grass reserves nothing: its instances take no record.
    deform_capacity_ = load_deforms_ + (flora_tail_parts_ ? k_tail_deform_reserve : 0u);
    flora_part_capacity_ = flora_parts_ + (flora_tail_parts_ ? k_tail_part_reserve : 0u);
    {
      // **The budget follows the device** (R137): the heaps' size, as the chain's budget reads it.
      gfx::MemoryBudget memory;
      (void)device.memory_budget(memory);
      const FloraTailBudget budget = resolve_flora_tail_budget(
          static_cast<u32>(flora_tail_budget_mib.get()), memory.device_local_bytes);
      tail_budget_bytes_ = u64{budget.mib} * 1024 * 1024;
      flora_stats_.tail_budget_bytes = tail_budget_bytes_;
      flora_stats_.tail_budget_source = budget.device ? "device" : "tunable";
    }
    if (flora_tail_) {
      deform_descs_.reserve(deform_capacity_);
      deform_instance_.reserve(deform_capacity_);
      deform_mesh_clusters_.reserve(deform_capacity_);
      tail_trees_.reserve(deform_capacity_ - load_deforms_);
      tail_tree_free_.assign(flora_meshes_.size(), ~0u);
      std::fill(std::begin(tail_old_head_), std::end(tail_old_head_), ~0u);
      std::fill(std::begin(tail_old_tail_), std::end(tail_old_tail_), ~0u);
      tail_tree_of_slot_.assign(instance_capacity_, ~0u);
      flora_instances_.reserve(load_flora_ + instance_capacity_);
    }
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
        gfx::create_buffer(device, deform_block_bytes(), k_address, false, deform_blocks, error) &&
        gfx::create_buffer(
            device, u64{gfx::k_draw_args_bytes} * (k_visible_runs * view_count_ + cascade_count_),
            k_args, false, deform_args, error);
    // The per-frame side of skinning: `k_joint_slots` regions of bone matrices and the same
    // number of copies of the deform table, both host-visible and persistently mapped, so a tick
    // is one memcpy of the span plus one rewrite of a table of 24-byte records. Every region starts
    // as the static table, so a frame only ever rewrites the two words that change and a slot that
    // has never been written is still a valid rest pose.
    // A lifted water body's clock is per frame too (S100), so its scene keeps the copies as a
    // skinned one does.
    ok = ok && make_deform_tables(skinned_ || !water_deforms_.empty(), error);
    if (ok && skinned_) {
      const u64 joint_region = joint_bytes();
      ok = gfx::create_buffer(device, joint_region * k_joint_slots, k_address, true, joints, error);
      if (ok) std::memset(joints.mapped, 0, joint_region * k_joint_slots);
    }
    if (ok && !lod.mesh.morph_channels.empty()) ok = create_morph(resolved, error);
    if (ok && resolved.terrain_levels) ok = create_terrain(resolved, error);
    if (ok && (!flora_instances_.empty() || flora_tail_ || flora_field_)) ok = create_flora(error);
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
  static_cache_pending_.assign(deform_descs_.size(), u8{0});
  static_cache_rebuild_.assign(deform_descs_.size(), u8{0});
  morph_base_padding_.assign(deform_descs_.size(), 0.0f);
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
    morph_base_padding_[d] = instance.bounds_padding;
    instance.bounds_padding += padding;
  }
  // `set_static_weights` above marked every entry before the vectors were sized: mark them now.
  for (u32 d = 0; d < deform_descs_.size(); ++d)
    static_cache_pending_[d] = 1;
  // Re-upload the table now that the caches and the weights are on it. A morphed scene needs the
  // **per-frame** table even when nothing is skinned, because the pose weights live in a per-slot
  // region and the record has to point at this slot's. A skinned scene already has one; this is
  // the case where morphs alone force it.
  if (!make_deform_tables(true, error)) return false;
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
  for (u8& pending : static_cache_pending_)
    pending = 1;
  static_cache_dirty_ = true;
}

u32 GpuScene::instance_morph_channels(u32 instance) const noexcept {
  if (morph_channel_count_ == 0 || instance >= instance_table_.size()) return 0;
  const u32 d = instance_table_[instance].deform;
  // The weights' regions hold the load's entries (`morph_weight_floats`); a tail's have none.
  return d < deform_descs_.size() && d < load_deforms_ && deform_instance_[d] == instance
             ? deform_descs_[d].channel_count
             : 0u;
}

bool GpuScene::set_instance_static_weights(u32 instance, std::span<const f32> weights,
                                           std::string* error) {
  const u32 channels = instance_morph_channels(instance);
  if (channels == 0 || morph_weights.mapped == nullptr) {
    if (error != nullptr)
      *error = "instance " + std::to_string(instance) + " has no morph channels";
    return false;
  }
  if (weights.size() > channels) {
    if (error != nullptr) {
      *error = std::to_string(weights.size()) + " weights for instance " +
               std::to_string(instance) + "'s " + std::to_string(channels) + " morph channels";
    }
    return false;
  }
  const u32 d = instance_table_[instance].deform;
  // Every slot, as `set_static_weights` writes them: a slot the frame loop has not reached yet
  // must already hold the new shape. Only this entry's static half; every other instance keeps its.
  for (u32 slot = 0; slot < k_joint_slots; ++slot) {
    f32* statics = morph_weight_slot(slot) + u64{d} * 2 * morph_channel_count_;
    for (u32 c = 0; c < channels; ++c)
      statics[c] = c < weights.size() ? weights[c] : 0.0f;
  }
  // The cull's padding from the new static weights, priced as the load priced it: |static| + 1 (the
  // pose half at its range) times each channel's reach, on the instance's padding before any morph.
  const geometry::ClusterMesh& mesh = data_->lod.mesh;
  const gfx::DeformDesc& desc = deform_descs_[d];
  f32 padding = 0.0f;
  for (u32 c = 0; c < channels; ++c) {
    const f32 weight = c < weights.size() ? weights[c] : 0.0f;
    const f32 magnitude = weight < 0.0f ? -weight : weight;
    padding += (magnitude + 1.0f) * mesh.morph_channels[desc.first_channel + c].max_displacement;
  }
  if (d < morph_base_padding_.size()) {
    instance_table_[instance].bounds_padding = morph_base_padding_[d] + padding;
    mark_instance_written(instance);
  }
  if (d < static_cache_pending_.size()) static_cache_pending_[d] = 1;
  static_cache_dirty_ = true;
  return true;
}

u32 GpuScene::take_static_cache_rebuilds() noexcept {
  u32 rebuilds = 0;
  for (u32 d = 0; d < static_cache_rebuild_.size(); ++d) {
    const bool due = d < static_cache_pending_.size() && static_cache_pending_[d] != 0 &&
                     d < deform_descs_.size() && deform_descs_[d].cache != 0;
    static_cache_rebuild_[d] = due ? u8{1} : u8{0};
    if (due) ++rebuilds;
  }
  for (u8& pending : static_cache_pending_)
    pending = 0;
  static_cache_dirty_ = false;
  return rebuilds;
}

// **The trees' device side** (renderer.md, "The flora stage"): the scene's motion vertex stream —
// each tree mesh's records at its run of the merged vertex streams, zero for every other mesh,
// which no rigid instance reads — and the part-pose table, a region per frame slot like the joint
// matrices, host-visible and persistently mapped, so a frame's poses are written in place. Every
// region starts at rest. The ground the wind is asked of is the sand detail's when the scene has
// one, and one made here for a terrain that has none.
bool GpuScene::create_flora(std::string* error) {
  ENGINE_STARTUP_STAGE("flora motion");
  const gfx::Device& device = *device_;
  const u32 total_vertices = data_->lod.mesh.vertices.size();
  Vector<geometry::ClusterFileMotionVertex> stream(total_vertices,
                                                   geometry::ClusterFileMotionVertex{});
  for (u32 m = 0; m < flora_meshes_.size(); ++m) {
    if (flora_meshes_[m].part_count() == 0) continue;
    const SourceMesh& source = data_->sources[m];
    const u32 first = data_->parts[m].first_vertex;
    for (u32 v = 0; v < source.motion_vertices.size(); ++v)
      stream[first + v] = source.motion_vertices[v];
  }
  if (!gfx::upload_buffer(device, stream.data(),
                          u64{stream.size()} * sizeof(geometry::ClusterFileMotionVertex), k_address,
                          flora_motion_, error) ||
      !make_flora_poses(error)) {
    return false;
  }
  flora_state_.clear();
  flora_state_.reserve(flora_part_capacity_);
  flora_state_.resize(flora_parts_, FloraPartState{});
  if (!deform_normals.buffer.valid() &&
      !gfx::create_buffer(device, u64{deform_pool_vertices_} * sizeof(u32), k_address, false,
                          deform_normals, error)) {
    return false;
  }
  if (data_->terrain.enabled) flora_ground_ = std::make_unique<TerrainSampler>(data_->terrain);
  // The poser's chunks and, for a scene with enough parts, its pool (renderer.md, "Who poses the
  // trees"): the tunable read once, here.
  flora_poser_.prepare(
      std::span<const FloraInstance>(flora_instances_.data(), flora_instances_.size()),
      std::span<const FloraMesh>(flora_meshes_.data(), flora_meshes_.size()),
      flora_workers_tunable(), k_flora_chunk_parts, flora_tail_);
  flora_counts();
  ENGINE_LOG_INFO(
      log_renderer, "trees sway", log::field("instances", flora_stats_.instances),
      log::field("parts", flora_parts_), log::field("chunks", flora_poser_.chunk_count()),
      log::field("workers", flora_poser_.worker_count()),
      log::field("motion_bytes", u64{stream.size()} * sizeof(geometry::ClusterFileMotionVertex)),
      log::field("table_bytes", flora_poses_.size), log::field("tail", flora_tail_),
      log::field("max_padding_m", flora_stats_.max_padding));
  return true;
}

// The field kind's instances and the caster policy's, over the instance table: when the instances
// change, never per frame (a meadow's 126,309 instances are a fraction of a millisecond).
void GpuScene::policy_counts() noexcept {
  untraced_pairs_live_ = 0;
  u32 field = 0;
  u32 nearest = 0;
  u32 rest = 0;
  u32 untraced = 0;
  const bool fields = flora_field_;
  for (const gfx::InstanceDesc& placed : instance_table_) {
    if (fields && placed.mesh < field_deform_.size() && field_deform_[placed.mesh] != ~0u &&
        placed.deform == field_deform_[placed.mesh]) {
      ++field;
    }
    if (placed.flags == 0) continue;
    if ((placed.flags & gfx::k_instance_cascade_mask) != 0) ++nearest;
    if ((placed.flags & gfx::k_instance_shadow_rest) != 0) ++rest;
    if ((placed.flags & gfx::k_instance_untraced) != 0) {
      ++untraced;
      if (placed.mesh < data_->parts.size() && (placed.flags & gfx::k_instance_static_rt) == 0)
        untraced_pairs_live_ += data_->parts[placed.mesh].cluster_count;
    }
  }
  flora_stats_.field_instances = field;
  flora_stats_.casters_nearest = nearest;
  flora_stats_.casters_rest = rest;
  flora_stats_.untraced = untraced;
  count_rt_cache_instances();
}

void GpuScene::policy_count(const gfx::InstanceDesc& placed, bool add) noexcept {
  const auto step = [add](auto& counter, auto by) {
    if (add) {
      counter += by;
    } else {
      counter -= by;
    }
  };
  if (flora_field_ && placed.mesh < field_deform_.size() && field_deform_[placed.mesh] != ~0u &&
      placed.deform == field_deform_[placed.mesh]) {
    step(flora_stats_.field_instances, 1u);
  }
  if (placed.flags != 0) {
    if ((placed.flags & gfx::k_instance_cascade_mask) != 0) step(flora_stats_.casters_nearest, 1u);
    if ((placed.flags & gfx::k_instance_shadow_rest) != 0) step(flora_stats_.casters_rest, 1u);
    if ((placed.flags & gfx::k_instance_untraced) != 0) {
      step(flora_stats_.untraced, 1u);
      if (placed.mesh < data_->parts.size() && (placed.flags & gfx::k_instance_static_rt) == 0)
        step(untraced_pairs_live_, u64{data_->parts[placed.mesh].cluster_count});
    }
  }
  if (rt_cache_slots_ != 0 && rt_instance_cached(placed)) {
    step(rt_cache_instances_, 1u);
    step(rt_cache_pairs_, u64{data_->parts[placed.mesh].cluster_count});
  }
}

bool GpuScene::policy_counts_match() const noexcept {
  u32 field = 0;
  u32 nearest = 0;
  u32 rest = 0;
  u32 untraced = 0;
  u64 untraced_pairs = 0;
  u32 cached = 0;
  u64 cached_pairs = 0;
  for (const gfx::InstanceDesc& placed : instance_table_) {
    if (flora_field_ && placed.mesh < field_deform_.size() && field_deform_[placed.mesh] != ~0u &&
        placed.deform == field_deform_[placed.mesh]) {
      ++field;
    }
    if ((placed.flags & gfx::k_instance_cascade_mask) != 0) ++nearest;
    if ((placed.flags & gfx::k_instance_shadow_rest) != 0) ++rest;
    if ((placed.flags & gfx::k_instance_untraced) != 0) {
      ++untraced;
      if (placed.mesh < data_->parts.size() && (placed.flags & gfx::k_instance_static_rt) == 0)
        untraced_pairs += data_->parts[placed.mesh].cluster_count;
    }
    if (rt_cache_slots_ != 0 && rt_instance_cached(placed)) {
      ++cached;
      cached_pairs += data_->parts[placed.mesh].cluster_count;
    }
  }
  return field == flora_stats_.field_instances && nearest == flora_stats_.casters_nearest &&
         rest == flora_stats_.casters_rest && untraced == flora_stats_.untraced &&
         untraced_pairs == untraced_pairs_live_ && cached == rt_cache_instances_ &&
         cached_pairs == rt_cache_pairs_;
}

void GpuScene::flora_counts(bool policy) noexcept {
  if (policy) policy_counts();
  flora_stats_.instances = flora_instances_.size();
  flora_stats_.tail_instances = flora_instances_.size() - load_flora_;
  flora_stats_.parts = flora_parts_;
  flora_stats_.chunks = flora_poser_.chunk_count();
  flora_stats_.workers = flora_poser_.worker_count();
  flora_stats_.tail_records = tail_trees_.size();
  // What the tail's room costs: its records in the table and every per-frame copy, and its parts'
  // poses in every slot's region and their push state.
  const u64 tail_parts = u64{flora_part_capacity_ - load_flora_parts_};
  flora_stats_.tail_bytes =
      u64{deform_capacity_ - load_deforms_} * sizeof(gfx::DeformDesc) *
          (deform_frames.buffer.valid() ? u64{k_joint_slots} + 1 : u64{1}) +
      tail_parts * (u64{k_joint_slots} * sizeof(gfx::FloraPartPose) + sizeof(FloraPartState));
}

// **The pose table**: `k_joint_slots` regions of `flora_part_capacity_` poses, host-visible and
// persistently mapped, every part at rest (axis.w 0) until a frame poses it.
bool GpuScene::make_flora_poses(std::string* error) {
  gfx::destroy_buffer(*device_, flora_poses_);
  // A region is the field block (gfx::FloraFieldFrame, the field kind's frame) and then the poses,
  // so the address a frame hands the pool pass is the region's poses and the block is just before
  // it: a tree's `first_part` is what it always was.
  const u64 table_bytes = u64{k_joint_slots} * flora_region_bytes();
  if (!gfx::create_buffer(*device_, table_bytes, k_address, true, flora_poses_, error))
    return false;
  std::memset(flora_poses_.mapped, 0, table_bytes);
  flora_stats_.pose_table_bytes = table_bytes;
  return true;
}

// The deform table and, when `frames`, its per-frame copies, `deform_capacity_` entries a region:
// written once and device-local for a scene whose deform records are the load's alone, as always;
// host-visible and persistently mapped for one whose tail takes records of its own, so a tree's
// record is written between frames into an entry no frame in flight reads (renderer.md, "Streamed
// trees sway").
void GpuScene::put_deform_record(u32 index, const gfx::DeformDesc& desc) noexcept {
  if (index >= deform_capacity_) return;
  if (deform_table.mapped != nullptr)
    static_cast<gfx::DeformDesc*>(deform_table.mapped)[index] = desc;
  // A scene whose table has per-frame copies for its water (S100) and no skin to rewrite them every
  // frame keeps them in step here, so a tree the tail brings in sways in every slot.
  if (!skinned_ && deform_frames.mapped != nullptr) {
    for (u32 slot = 0; slot < k_joint_slots; ++slot)
      deform_frame(slot)[index] = desc;
  }
}

bool GpuScene::make_deform_tables(bool frames, std::string* error) {
  const gfx::Device& device = *device_;
  const u64 used = u64{deform_descs_.size()} * sizeof(gfx::DeformDesc);
  gfx::destroy_buffer(device, deform_table);
  if (flora_tail_) {
    if (!gfx::create_buffer(device, u64{deform_capacity_} * sizeof(gfx::DeformDesc), k_address,
                            true, deform_table, error)) {
      return false;
    }
    if (used > 0) std::memcpy(deform_table.mapped, deform_descs_.data(), used);
  } else if (used == 0) {
    // No record yet — a scene whose only trees are in a tail drawn rigid: one entry nobody reads,
    // for the address every frame passes.
    if (!gfx::create_buffer(device, sizeof(gfx::DeformDesc), k_address, false, deform_table,
                            error)) {
      return false;
    }
  } else if (!gfx::upload_buffer(device, deform_descs_.data(), used, k_storage, deform_table,
                                 error)) {
    return false;
  }
  if (!frames) {
    // **Copies that stay are written too** (S100, 2026-11-09): a scene that keeps per-frame copies
    // with no skin to rewrite them whole every frame — one with a lifted water body, whose frames
    // write only the water's words — reads every record from them. The tail's growth makes the
    // table again without making the copies when only the part-pose table outgrew its room (a
    // record made then was in neither until now), and on the forest that left a swaying tree's
    // record uninitialised in every copy: the device lost in the first frames. The table and
    // its copies hold the same records whenever the table is written, so no path can write one.
    if (deform_frames.mapped != nullptr && used > 0) {
      const u64 room = u64{deform_capacity_} * sizeof(gfx::DeformDesc);
      ENGINE_ASSERT(deform_frames.size >= room * k_joint_slots, "the copies hold the capacity");
      for (u32 slot = 0; slot < k_joint_slots; ++slot)
        std::memcpy(deform_frame(slot), deform_descs_.data(), used < room ? used : room);
    }
    return true;
  }
  gfx::destroy_buffer(device, deform_frames);
  if (!gfx::create_buffer(device, u64{deform_capacity_} * sizeof(gfx::DeformDesc) * k_joint_slots,
                          k_address, true, deform_frames, error)) {
    return false;
  }
  for (u32 slot = 0; slot < k_joint_slots; ++slot) {
    if (used > 0) std::memcpy(deform_frame(slot), deform_descs_.data(), used);
  }
  return true;
}

u32 GpuScene::block_stamp(const DynamicBlock& block) {
  // The tick a block's instances came at, kept by its key while the caller's version of them
  // stands, so a block the layout only moves (a compaction, a split) keeps its age. The first fill
  // is 0: present from the start, never faded.
  if (block.version != 0) {
    const auto found = block_stamps_.find(block.key);
    if (found != block_stamps_.end() && (*found).second.version == block.version)
      return (*found).second.stamp;
  }
  u32 stamp = 0;
  if (!first_fill_) {
    stamp = static_cast<u32>(flora_frames_ + 1) & gfx::k_instance_stamp_mask;
    if (stamp == 0) stamp = 1;
  }
  block_stamps_[block.key] = BlockStamp{block.version, stamp};
  return stamp;
}

u64 GpuScene::flora_frame(u32 slot, const FrameFlora& frame, u64 frame_index, f64 world_time_s,
                          f64 sky_time_s, const WorldEye& eye) noexcept {
  ++flora_frames_;  // what a given-back record's age is counted in
  // Where the eye stands, for the next commit's nearest-first admission past the tail's budget.
  set_tail_eye(to_world(eye));
  if ((flora_instances_.empty() && !flora_field_) || flora_poses_.mapped == nullptr ||
      slot >= k_joint_slots) {
    flora_moving_ = false;  // a tail whose trees have all left: nothing sways, nothing is written
    return 0;
  }
  u8* region = static_cast<u8*>(flora_poses_.mapped) + u64{slot} * flora_region_bytes();
  const u64 poses_address =
      flora_poses_.address + u64{slot} * flora_region_bytes() + gfx::k_flora_field_frame_bytes;
  // The wind in the ground plane: the frame's, the ground's record at the sky's hour (the weather
  // follows the day, clocks.h), or the tunables'.
  Vec2 to{1.0f, 0.0f};
  f32 strength = 0.0f;
  const char* source = "frame";
  if (frame.wind_set) {
    to = frame.wind_to;
    strength = frame.wind_strength;
  } else {
    const TerrainSampler* ground = ground_ != nullptr ? ground_.get() : flora_ground_.get();
    f64 moved = 0.0;
    f32 ground_strength = 0.0f;
    const f64 at = sky_time_s;
    if (ground != nullptr && ground->transport(at, moved, ground_strength)) {
      to = ground->wind(at);
      strength = ground_strength;
      source = "ground";
    } else {
      const FloraWindDefault fallback = flora_wind_default();
      to = fallback.to;
      strength = fallback.strength;
      source = "tunables";
    }
  }
  // Said once, on a tree's first frame and whenever the source changes, so "why do the palms not
  // move" has an answer in the log.
  if (source != flora_wind_source_) {
    flora_wind_source_ = source;
    ENGINE_LOG_INFO(log_renderer, "the trees' wind", log::field("source", source),
                    log::field("strength", strength), log::field("to_x", to.x),
                    log::field("to_z", to.y));
  }
  const f32 to_length = length(to);
  to = to_length > 0.0f ? to * (1.0f / to_length) : Vec2{1.0f, 0.0f};
  flora_stats_.wind_strength = strength;
  flora_stats_.wind_to = to;
  // **The field block** (renderer.md, "The field kind"): the grass's wind, gust field, flutter
  // clock and walker for the pool pass, written in place at the head of this slot's region. Not a
  // pose: nothing on the CPU knows a blade.
  bool field_moving = false;
  if (flora_field_) {
    gfx::FloraFieldFrame block;
    flora_field_frame(to, strength, world_time_s, eye, frame, block);
    // The hand-off's clock (G136): what the pool pass counts a patch's load-in age from.
    const u32 tick = handoff_tick();
    std::memcpy(&block.push.w, &tick, sizeof(tick));
    std::memcpy(region, &block, sizeof(block));
    field_moving = block.wind.z > 0.0f || block.walker.w > 0.0f;
  }
  if (flora_instances_.empty()) {
    flora_moving_ = field_moving;
    flora_stats_.pushed_parts = 0;
    return poses_address;
  }
  // The sway's clock is the world's (a host at 60 frames a second with the world at real time hands
  // `frame_index / 60`, the number this was before the clocks were split). Every instance is posed
  // by the poser, on its pool when the scene has enough parts to pay for one (flora_poser.h).
  FloraPoseFrame pose_frame;
  pose_frame.wind_to = to;
  pose_frame.wind_strength = strength;
  pose_frame.time_s = world_time_s;
  pose_frame.frame_index = frame_index;
  pose_frame.flora = &frame;
  FloraPoseInputs inputs;
  inputs.instances = std::span<FloraInstance>(flora_instances_.data(), flora_instances_.size());
  inputs.meshes = std::span<const FloraMesh>(flora_meshes_.data(), flora_meshes_.size());
  inputs.bounds = std::span<const Vec4>(flora_bounds_.data(), flora_bounds_.size());
  inputs.placed =
      std::span<const gfx::InstanceDesc>(instance_table_.data(), instance_table_.size());
  inputs.state = std::span<FloraPartState>(flora_state_.data(), flora_state_.size());
  inputs.region = reinterpret_cast<gfx::FloraPartPose*>(region + gfx::k_flora_field_frame_bytes);
  const i64 pose_start = time::monotonic_ns();
  const FloraPoseResult posed = flora_poser_.pose(inputs, pose_frame);
  const i64 pose_ns = time::monotonic_ns() - pose_start;
  flora_stats_.pose_ns_sum += static_cast<u64>(pose_ns);
  flora_stats_.pose_ns_max = std::max(flora_stats_.pose_ns_max, static_cast<u64>(pose_ns));
  ++flora_stats_.posed_frames;
  flora_stats_.pushed_parts = posed.pushed_parts;
  flora_moving_ = posed.moving || field_moving;
  return poses_address;
}
// ---- terrain levels (renderer.md, "The dunes in time-lapse")
// -------------------------------------
//
// The device side of a moving terrain, and nothing about when it moves: `TerrainMotion` decides
// that, evaluates the fields off the frame, and hands them over through `terrain_upload` and
// `terrain_show`. Level 0 is the scene's own grid, drawn by the terrain mesh's instance.
bool GpuScene::create_terrain(const ResolvedSettings& resolved, std::string* error) {
  ENGINE_STARTUP_STAGE("terrain levels");
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
  // The scene's grid (level 0) is placed on its float lattice in its mesh space, which the cull
  // pass's hole test and the pool pass take for the world: its instance is the identity at cell
  // zero, and anything else is refused rather than drawn in the wrong place. The grid is the scene
  // terrain's own, kilometres across round the world's origin. Every other level is the world's
  // lattice drawn as tiles placed at their corners (`terrain_chunk_show`; gfx::TerrainLevelDesc).
  gfx::InstanceDesc identity{};
  gfx::set_instance_transform(identity, Mat4::identity());
  const gfx::InstanceDesc& placed = instance_table_[instance];
  if (std::memcmp(placed.rows, identity.rows, sizeof(identity.rows)) != 0 ||
      !(placed.cell == identity.cell)) {
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
      // A world-lattice level's tiles are placed at their corners, and the pool pass finds their
      // points in whole millimetres (gfx::TerrainLevelDesc::spacing_mm).
      if (level.lattice.spacing_mm <= 0 || level.lattice.spacing_mm > 0x7fffffff) {
        if (error != nullptr)
          *error = "a terrain level's lattice is not a whole number of millimetres a point";
        return false;
      }
      level.spacing_mm = static_cast<i32>(level.lattice.spacing_mm);
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
  out.images.clear();
  // A recorded frame: the textures let go long enough ago for every frame that could have sampled
  // them to be done go now (gpu_scene_textures.cpp).
  if (!leaving_textures_.empty()) texture_frame();
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
  // The copies staged since the last frame — the rings' chunks, and the meshes admitted into the
  // mesh room (gpu_scene_meshes.cpp), which a scene with no terrain levels has too.
  // Swapped and cleared rather than moved from, so both lists keep what they grew to and a frame
  // of chunk uploads allocates nothing once they have (the long-flight test counts it).
  const auto hand_over_geometry = [&] {
    std::swap(out.geometry, pending_geometry_);
    pending_geometry_.clear();
    std::swap(out.images, pending_images_);  // the textures admitted since (`acquire_texture`)
    pending_images_.clear();
    for (const gfx::BufferResource& staging : pending_staging_)
      out.retire.push_back(staging);
    pending_staging_.clear();
    out.geometry_bytes = pending_geometry_bytes_;
    pending_geometry_bytes_ = 0;
    if (staging_ring_.mapped != nullptr) ring_slot_end_[region] = ring_head_;
  };
  if (terrain_.empty() || terrain_table_.mapped == nullptr) {
    hand_over_geometry();
    return;
  }
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
    desc.spacing_mm = l.spacing_mm;
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
  // The rings' chunk uploads since the last frame (the records of the slots they turn on or off are
  // `prepare_records`', which every frame runs).
  hand_over_geometry();
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
  // Every level's chunk carries its UVs from its corner (renderer.md, "The ground's tiles are
  // placed at their corners"): a world tile's, a far level's and, since 2026-10-07, a ring's. Its
  // slot's instance is flagged when it stands at the corner, and the frame's resolve is given the
  // grid's UV frame to put the corner in.
  terrain_uv_ = TerrainUvFrame{};
  if (!terrain_uv_frame(data_->terrain, terrain_uv_, error)) return false;
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
            "visibility id names (" +
            std::to_string(u64{pair_count_} + slot_clusters) + "); lower terrain.rings.*";
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
  pending_instance_writes_.reserve(2 * all_slots);
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
  slot.corner_x_mm = chunk.corner_x_mm;
  slot.corner_z_mm = chunk.corner_z_mm;
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
    // **Turned on, the slot's instance stands at its chunk's corner** (renderer.md, "The ground's
    // tiles are placed at their corners"), written in the same frame as the record that names the
    // chunk's clusters — and flagged, since its UVs are from that corner too, so the material puts
    // them back in the grid's frame. Turned off, it stays where it was: a slot with no clusters
    // draws nothing.
    if (slot.on) {
      const u32 instance = ring.first_instance + s;
      gfx::set_terrain_corner(instance_table_[instance], slot.corner_x_mm, slot.corner_z_mm);
      instance_table_[instance].flags |= gfx::k_instance_uv_from_corner;
      mark_instance_written(instance);
    }
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
  // The terrain's mask level, whole, before the table that names its address (renderer.md,
  // "Ground materials by place"): the page table and every tile the provider built.
  gfx::TerrainRecord terrain_mask_record;
  terrain_mask_bytes_ = 0;
  terrain_mask_tiles_ = 0;
  if (data_->terrain.enabled && data_->terrain.masks.has_value() &&
      data_->terrain.materials.has_value()) {
    TerrainMaskUpload upload;
    if (!prepare_terrain_masks(data_->terrain, upload, error)) return false;
    if (!upload.words.empty()) {
      if (!gfx::upload_buffer(*device_, upload.words.data(), u64{upload.words.size()} * sizeof(u32),
                              k_storage, terrain_masks, error))
        return false;
      terrain_mask_record = upload.record;
      terrain_mask_record.mask = terrain_masks.address;
      terrain_mask_bytes_ = u64{upload.words.size()} * sizeof(u32);
      terrain_mask_tiles_ = upload.tiles;
      ENGINE_LOG_INFO(log_renderer, "terrain masks", log::field("tiles", upload.tiles),
                      log::field("bytes", terrain_mask_bytes_),
                      log::field("palette", data_->terrain.materials->layers.size()));
    }
  }
  // The scene's formation (W85; renderer.md, "Formation bands"): one block the terrain's record
  // names by address, uploaded only when a layer asks to be banded by it, so a scene without one
  // holds nothing more and uploads the record it did.
  if (data_->terrain.enabled && data_->terrain.formation.has_value() &&
      data_->terrain.materials.has_value()) {
    bool banded = false;
    for (const auto& layer : data_->terrain.materials->layers)
      banded = banded || layer.formation;
    if (banded) {
      gfx::FormationParams block;
      if (!gfx::pack_formation(*data_->terrain.formation, block, error)) return false;
      if (!gfx::upload_buffer(*device_, &block, sizeof(block), k_storage, terrain_formation, error))
        return false;
      terrain_mask_record.formation = terrain_formation.address;
    }
  }
  ENGINE_STARTUP_STAGE("upload materials");
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  if (!gfx::create_sampler(device, gfx::Filter::Linear, sampler_, error)) return false;
  const u32 sampler_slot = bindless_.add_sampler(sampler_);
  // The materials' samplers are made as their slots ask (`material_sampler_slot`, with
  // `fill_material` in gpu_scene_textures.cpp, which an admitted mesh's materials go through too).
  load_textures_.clear();
  texture_bytes_ = 0;
  textures_built_ = 0;
  textures_decoded_ = 0;
  texture_references_ = 0;
  textures_shared_ = 0;
  texture_bytes_saved_ = 0;
  normal_variance_textures_ = 0;
  normal_variance_bytes_ = 0;
  const bool normal_filter = normal_filter_tunable();  // R97, read once per upload
  share_textures_ = resolved.settings.share_textures;
  transform_conflicts_ = 0;
  Vector<gfx::ResolveMaterial> material_table;
  // Per entry of the table, 1 where it is a material a cluster can name and 0 where it is a record
  // that only its material reads — a water body's optics, spectrum and plunges, the terrain's
  // record — whose word at `flags`' offset is a float or a tile index, not flags (R141: a terrain
  // record's negative first tile row read as every flag from bit 3 up, water and the alpha mask
  // among them). The scan below that asks what the table holds reads only the materials.
  Vector<u8> table_material;
  Vector<u32> cluster_material(cluster_count_);
  // The coverage errors (geometry.md, "Two errors"), only where a mesh carries them: empty, or a
  // word per cluster with `k_lod_no_coverage` for every cluster of a mesh with none.
  Vector<u32> coverage_words;
  Vector<u32> mesh_material_base(data_->parts.size(), 0u);
  // The water bodies' optics entries in the volume tier, and their meshes (water.md).
  struct WaterOpticsEntry {
    u32 optics = 0;
    u32 mesh = 0;
  };
  Vector<WaterOpticsEntry> water_optics;
  // A water body's depth map (scene.h, `SceneWaterDepth`; water.md, "The depth map") as the image
  // the water branch samples: R8G8B8A8 UNORM, red the 16-bit depth's high byte and green its low
  // (the RHI's formats have no 16-bit UNORM single channel, and a 32-bit float is not filterable
  // on every device; RGBA8 is, and the depth is linear in red and green), one level, through the
  // scene's linear sampler — the shader clamps the UV inside the map, so its address mode is
  // never asked.
  auto upload_water_depth = [&](const SceneWaterDepth& depth, gfx::WaterDepthFrame& frame) {
    const u32 texel_count = depth.size * depth.size;
    Vector<u8> texels(texel_count * 4);
    for (u32 t = 0; t < texel_count; ++t) {
      texels[t * 4 + 0] = static_cast<u8>(depth.texels[t] >> 8);
      texels[t * 4 + 1] = static_cast<u8>(depth.texels[t] & 0xffu);
      texels[t * 4 + 2] = 0;
      texels[t * 4 + 3] = 255;
    }
    gfx::ImageResource image;
    gfx::ImageViewHandle view;
    if (!gfx::upload_image_2d(device, depth.size, depth.size, gfx::Format::R8G8B8A8Unorm,
                              texels.data(), texels.size(), image, error) ||
        !gfx::create_image_view(device, image, view, error)) {
      if (image.image.valid()) gfx::destroy_image(device, image);
      return false;
    }
    texture_bytes_ += image.bytes;
    ENGINE_LOG_INFO(log_renderer, "water depth map", log::field("size", depth.size),
                    log::field("texel_m", depth.texel_m), log::field("bytes", image.bytes));
    textures_.push_back(image);
    texture_views_.push_back(view);
    frame.texture = bindless_.add_sampled_image(view, gfx::ImageLayout::ShaderReadOnly);
    frame.sampler = sampler_slot;
    frame.size = depth.size;
    frame.origin_m = depth.origin;
    frame.texel_m = depth.texel_m;
    frame.depth_max = depth.depth_max;
    frame.bank_max = depth.bank_max;
    return true;
  };
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
    table_material.assign(3, u8{1});
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
    TexturePrefetch prefetch(*data_);  // T73: the `.tex` reads ahead of the uploads, on a pool
    for (u32 m = 0; m < data_->sources.size(); ++m) {
      const SourceMesh& source_mesh = data_->sources[m];
      const assets::MeshData& mesh_data = source_mesh.data;
      if (m == data_->terrain_mesh && data_->terrain.materials.has_value() &&
          u64{mesh_data.materials.size()} < u64{data_->terrain.materials->layers.size()} + 1u) {
        if (error != nullptr)
          *error = "terrain.materials: the cached ground has an incomplete layer table";
        return false;
      }
      mesh_material_base[m] = material_table.size();
      Vector<BuiltImage> built;
      u32 sampled_images = 0;
      std::string why;
      bool use_built = find_built_textures(source_mesh, scene_textures, normal_filter, built,
                                           sampled_images, why, &prefetch);
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
      Vector<SceneTexture> image_made(mesh_data.images.size());  // its mean, for R131
      bool variance_upload_failed = false;
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
          image_made[index] = *held;
          return held;
        };
        if (use_built) {
          made.content = built[index].key;
          made.stored = built[index].stored;
          made.normal_variance = built[index].normal_variance;
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
          if (ok) made.has_mean = built_texture_mean(t, made.mean);
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
        if (made.normal_variance) {
          // Scene creation never frees a descriptor. Publish the auxiliary image immediately
          // after its normal, and check that contract rather than silently sampling another map.
          const texture::TextureData& t = built[index].data;
          Vector<gfx::ImageLevelData> levels;
          levels.reserve(t.levels.size());
          for (u32 l = 0; l < t.levels.size(); ++l) {
            const auto bytes = t.normal_variance_level_bytes(l);
            levels.push_back(gfx::ImageLevelData{bytes.data(), bytes.size()});
          }
          gfx::ImageResource variance;
          gfx::ImageViewHandle variance_view;
          if (!gfx::upload_image_2d_levels(
                  device, t.width, t.height, gfx::Format::Bc4Unorm,
                  std::span<const gfx::ImageLevelData>(levels.data(), levels.size()), variance,
                  &image_error) ||
              !gfx::create_image_view(device, variance, variance_view, &image_error)) {
            if (variance.image.valid()) gfx::destroy_image(device, variance);
            variance_upload_failed = true;
            if (error != nullptr) *error = "normal variance upload: " + image_error;
            return gfx::k_no_texture;
          }
          textures_.push_back(variance);
          texture_views_.push_back(variance_view);
          texture_bytes_ += variance.bytes;
          made.bytes += variance.bytes;
          ++normal_variance_textures_;
          normal_variance_bytes_ += variance.bytes;
          const u32 slot =
              bindless_.add_sampled_image(variance_view, gfx::ImageLayout::ShaderReadOnly);
          if (made.slot == gfx::k_no_texture || slot == gfx::k_no_texture ||
              slot != made.slot + 1u) {
            variance_upload_failed = true;
            if (error != nullptr) *error = "normal variance needs an adjacent sampled-image slot";
            return gfx::k_no_texture;
          }
        }
        scene_textures.add(made);
        image_made[index] = made;
        // A slice: a thread that owns a window keeps its messages flowing between two uploads
        // (profiling::progress_pump; apps.md, "The loading presentation"). Nothing when no one
        // pumps, which is every caller but engine-view's window.
        profiling::progress_pump();
        return image_slot[index];
      };
      u32 source_material_index = 0;
      for (const assets::Material& source : mesh_data.materials) {
        // Each slot's texture, in the order an image shared by two slots keeps the format of (base
        // colour first, as it always was; colour goes up sRGB and data UNORM), and what a material
        // needs to know of it. The record itself — factors, the five slots, the built flags, a
        // sampler per textured slot, the UV transform, the alpha mask — is `fill_material`'s
        // (gpu_scene_textures.cpp), which a mesh admitted after the load goes through too, so the
        // two cannot drift (texture.md, "Textures that arrive after the load").
        const auto info_of = [&](i32 image, gfx::Format format) {
          TextureInfo info;
          info.slot = texture_slot_of(image, format);
          if (image >= 0 && static_cast<u32>(image) < image_made.size()) {
            const u32 index = static_cast<u32>(image);
            info.has_mean = image_made[index].has_mean;
            info.mean = image_made[index].mean;
            if (use_built && index < built.size()) {
              info.normal_rg = built[index].stored == texture::TextureFormat::bc5;
              info.normal_variance = built[index].normal_variance;
            }
          }
          return info;
        };
        const TextureInfo tex[geometry::k_material_slots] = {
            info_of(source.base_color_image, gfx::Format::R8G8B8A8Srgb),
            info_of(source.metallic_roughness_image, gfx::Format::R8G8B8A8Unorm),
            info_of(source.normal_image, gfx::Format::R8G8B8A8Unorm),
            info_of(source.occlusion_image, gfx::Format::R8G8B8A8Unorm),
            info_of(source.emissive_image, gfx::Format::R8G8B8A8Srgb)};
        if (variance_upload_failed) return false;
        gfx::ResolveMaterial material;
        if (!fill_material(source, use_built, tex, material, error)) return false;
        // The terrain's material draws the sand's detail when its scene asks for it, weighted by
        // the sand share its metallic-roughness map carries in alpha (renderer.md, "The sand close
        // up"). The rings' slots draw with this material too.
        if (m == data_->terrain_mesh && data_->terrain.enabled && data_->terrain.has_detail)
          material.flags |= gfx::k_material_ground_detail;
        // A water body's mesh (scene.h, `SceneWater`; docs/subsystems/water.md): its one material
        // is the water class, the body's shading numbers packed into slots an untextured material
        // never reads, so every reader but the resolve's water branch draws its shallow colour.
        const bool water = m < data_->mesh_info.size() && data_->mesh_info[m].water.has_value();
        if (water) gfx::set_water_material(material, data_->mesh_info[m].water->shading);
        // An open body (water.md, "To the horizon"): its shore by depth, and the sea past it.
        if (water && data_->mesh_info[m].water->body.open)
          material.flags |= gfx::k_material_water_open;
        // A channel (water.md, "Channels"; S96): its UV.x is the flow phase.
        if (water && data_->mesh_info[m].water->channel)
          material.flags |= gfx::k_material_water_channel;
        // **Stochastic sampling** (R131; renderer.md, "Stochastic mesh materials"): a material
        // whose set asked for it (`ClusterFileMaterialSampling::flags`) reads its maps at three
        // tiles of the hex lattice, blended about its means — the factors times the built
        // textures' mean texels. Built textures only, since the means come from their small mips
        // and the tiles from their gradients; and never an alpha-tested, water or terrain
        // material, whose `uv_offset.z` is theirs (`set_stochastic` refuses). A refusal draws the
        // material plain, and says so.
        const bool wants_stochastic =
            source_material_index < source_mesh.material_sampling_flags.size() &&
            (source_mesh.material_sampling_flags[source_material_index] &
             geometry::k_material_sampling_stochastic) != 0;
        if (wants_stochastic) {
          const char* refused = use_built && m == data_->terrain_mesh
                                    ? "a terrain material is sampled by its own layers"
                                    : fill_stochastic(material, source, use_built, tex);
          if (refused != nullptr) {
            ENGINE_LOG_WARN(log_renderer, "stochastic sampling refused; the material draws plain",
                            log::field("mesh", m), log::field("material", source_material_index),
                            log::field("reason", refused));
          }
        }
        // An open sea's waves from the spectrum (water.md, "The spectrum"): the table in the five
        // entries after the material's next one, which is then always there (empty at the low
        // tier with no map).
        const bool spectrum =
            water && data_->mesh_info[m].water->waves.size() == gfx::k_water_waves;
        gfx::ResolveMaterial wave_entries[gfx::k_water_wave_entries];
        if (spectrum) {
          gfx::WaterWave waves[gfx::k_water_waves];
          for (u32 w = 0; w < gfx::k_water_waves; ++w)
            waves[w] = data_->mesh_info[m].water->waves[w];
          gfx::set_water_spectrum(material, wave_entries, waves);
        }
        // The volume tier (`RenderSettings::water_quality` high; water.md, "Translucent by
        // volume"): the body's optics in the entry right after its material, which no cluster
        // names (the water mesh has the one material). Its ground — the terrain's material and
        // the body's place in that material's UV frame — is filled in below, once the terrain's
        // material has its place in the table. The low tier packs nothing more: the table is the
        // one it always was.
        gfx::ResolveMaterial optics;
        const bool volume = water && resolved.settings.water_quality == WaterQuality::High;
        if (volume) {
          const scene::WaterBody& body = data_->mesh_info[m].water->body;
          gfx::WaterOptics o;
          if (body.absorption.has_value()) o.absorption = *body.absorption;
          if (body.scattering.has_value()) o.scattering = *body.scattering;
          o.transmission = body.transmission;
          o.turbidity = body.turbidity;
          gfx::set_water_optics(material, optics, o);
          water_optics.push_back(WaterOpticsEntry{material_table.size() + 1, m});
        }
        // The body's depth map, at both tiers (water.md, "The depth map"): in the same entry as
        // the optics where there are optics, in an entry of its own at the low tier. A body with
        // no map packs nothing, and the branch reads the mesh's vertex depths.
        if (water && data_->mesh_info[m].water->depth.size >= 2) {
          gfx::WaterDepthFrame frame;
          if (!upload_water_depth(data_->mesh_info[m].water->depth, frame)) return false;
          gfx::set_water_depth_map(material, optics, frame);
          // The shore where the level meets the ground, and its foam (water.md, "The shore";
          // S97): every body with a map, the foam's reach in the same entry.
          gfx::set_water_shore(material, optics, data_->mesh_info[m].water->body.foam_m);
        }
        bool record_entry = false;  // the terrain's record: an entry no cluster names
        if (m == data_->terrain_mesh && data_->terrain.materials.has_value()) {
          // The native terrain's entries (renderer.md, "Native authored terrain materials",
          // "Ground materials by place"): the root, an ordinary material that names its record by
          // one word; the layers (the palette, with masks); then the record, which no cluster
          // names.
          const auto& terrain_materials = *data_->terrain.materials;
          const u32 layers = static_cast<u32>(terrain_materials.layers.size());
          if (source_material_index == 0) {
            if (source.alpha_mode == assets::k_alpha_mask) {
              if (error != nullptr)
                *error = "terrain.materials: the terrain's own material cannot be alpha-tested";
              return false;
            }
            // The sand's detail draws on a layered terrain only through a wind-sand layer, weighted
            // by its share (R99; renderer.md, "Sand and snow"); otherwise the paths stay apart.
            if (!terrain_materials_wind_sand(terrain_materials))
              material.flags &= ~gfx::k_material_ground_detail;
            gfx::set_terrain_root(material, mesh_material_base[m] + layers + 1);
          } else if (source_material_index <= layers) {
            const auto& layer = terrain_materials.layers[source_material_index - 1];
            material.flags &= ~(gfx::k_material_uv_transform | gfx::k_material_ground_detail);
            if (layer.projection == scene::TerrainProjection::Map)
              material.flags |= gfx::k_material_terrain_map;
            gfx::TerrainLayerFields fields = terrain_layer_response(layer);
            fields.mean_colour =
                Vec3{material.emissive.x, material.emissive.y, material.emissive.z};
            fields.mean_roughness = material.emissive.w;  // the container's transport of the means
            // A layer with height detail reads it from its map's B through the metallic factor,
            // which a terrain layer never uses as metallic; the factor's rgb carries the
            // response's numbers (R99), which `set_terrain_layer` packs below.
            material.emissive = Vec4{0, 0, 0, layer.height.empty() ? 0.0f : 1.0f};
            fields.weight = layer.weight;
            fields.porosity = layer.porosity;
            fields.pitch_mm = std::round(layer.tile_m * 1000.0f);
            // A stochastic layer (R96): its largest turn, and its height map's mean, which the
            // content build carried in the layer's alpha cutoff (terrain_materials.cpp).
            if (layer.sampling == scene::TerrainSampling::Stochastic &&
                layer.projection == scene::TerrainProjection::Triplanar) {
              material.flags |= gfx::k_material_terrain_stochastic;
              fields.rotation = layer.rotation_deg * (k_pi / 180.0f);
              fields.mean_height = layer.height.empty() ? 0.0f : source.alpha_cutoff;
            }
            gfx::set_terrain_layer(material, fields);
            if (!layer.height.empty()) material.flags |= gfx::k_material_terrain_height;
            if (layer.formation) material.flags |= gfx::k_material_terrain_formation;
          } else if (source_material_index == layers + 1) {
            gfx::TerrainRecord record = terrain_mask_record;
            record.first_layer = mesh_material_base[m] + 1;
            record.layer_count = layers;
            record.detail_near_m = terrain_materials.detail_near_m;
            record.detail_far_m = terrain_materials.detail_far_m;
            TerrainUvFrame frame;
            if (!terrain_uv_frame(data_->terrain, frame, error)) return false;
            record.map_x0_mm = static_cast<i32>(frame.x0_mm);
            record.map_z0_mm = static_cast<i32>(frame.z0_mm);
            record.map_per_mm = static_cast<f32>(1.0 / static_cast<f64>(frame.size_mm));
            material = gfx::pack_terrain_record(record);
            record_entry = true;
          }
        }
        // A body's plunges (water.md, "The plunge"; S98): the falls' feet in the entry after the
        // spectrum's, or after the next one, which is then always there.
        gfx::ResolveMaterial plunge_entry;
        const bool plunges = water && !data_->mesh_info[m].water->plunges.empty() &&
                             gfx::set_water_plunges(
                                 material, plunge_entry, data_->mesh_info[m].water->plunges.data(),
                                 static_cast<u32>(data_->mesh_info[m].water->plunges.size())) > 0;
        ++source_material_index;
        material_table.push_back(material);
        table_material.push_back(record_entry ? u8{0} : u8{1});
        if (volume || spectrum || plunges ||
            (material.flags & gfx::k_material_water_depth_map) != 0) {
          material_table.push_back(optics);
          table_material.push_back(u8{0});
        }
        if (spectrum) {
          for (const gfx::ResolveMaterial& entry : wave_entries) {
            material_table.push_back(entry);
            table_material.push_back(u8{0});
          }
        }
        if (plunges) {
          material_table.push_back(plunge_entry);
          table_material.push_back(u8{0});
        }
      }
      const u32 local_count = material_table.size() - mesh_material_base[m];
      gfx::ResolveMaterial plain;
      plain.albedo = Vec4{0.8f, 0.8f, 0.8f, 0.6f};
      material_table.push_back(plain);  // the default for a primitive that names no material
      table_material.push_back(u8{1});
      const geometry::ClusterMeshPart& part = data_->parts[m];
      for (u32 i = 0; i < part.cluster_count; ++i) {
        const i32 material = source_mesh.part_material[source_mesh.part_of_cluster[i]];
        cluster_material[part.first_cluster + i] =
            material >= 0 && static_cast<u32>(material) < local_count ? static_cast<u32>(material)
                                                                      : local_count;
      }
      if (source_mesh.lod_coverage.size() == part.cluster_count) {
        if (coverage_words.empty())
          coverage_words.assign(cluster_count_, geometry::k_lod_no_coverage);
        for (u32 i = 0; i < part.cluster_count; ++i)
          coverage_words[part.first_cluster + i] = source_mesh.lod_coverage[i];
      }
    }
    // The load's built textures by key, for a scene whose tiles hand meshes: a mesh that arrives
    // naming one of them shares it and uploads nothing (gpu_scene_textures.cpp). Pinned: the load's
    // textures are the scene's for its life.
    if (mesh_room_.slots != 0) {
      for (const SceneTexture& held : scene_textures.all()) {
        if (held.form != k_form_built) continue;
        TextureInfo info;
        info.slot = held.slot;
        info.normal_rg = held.stored == texture::TextureFormat::bc5;
        info.normal_variance = held.normal_variance;
        info.has_mean = held.has_mean;
        info.mean = held.mean;
        load_textures_.try_emplace(held.content, info);
      }
    }
  }
  // The pairs of bindless slots the textures of meshes admitted later take, reserved now while the
  // set has freed nothing and its next slots are consecutive (gpu_scene_textures.cpp).
  if (!lay_out_texture_room(error)) return false;
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
  // The ground under each body in the volume tier: the terrain's one material, and the body's
  // centre — its instance's place, cell and local in f64 — in the UV frame that material's maps
  // are baked over (`terrain_uv_frame`: the grid's corner and side, millimetres), with the UV a
  // metre, so the resolve reads the bottom at centre + body-local metres and never a world
  // position. No terrain: `k_water_no_ground`, and the bottom is the body's shallow colour.
  if (!water_optics.empty() && data_->terrain.enabled &&
      data_->terrain_mesh < mesh_material_base.size()) {
    TerrainUvFrame frame;
    if (terrain_uv_frame(data_->terrain, frame, nullptr)) {
      const f64 per_mm = 1.0 / static_cast<f64>(frame.size_mm);
      for (const WaterOpticsEntry& entry : water_optics) {
        for (const gfx::InstanceDesc& instance : data_->instances) {
          if (instance.mesh != entry.mesh) continue;
          const f64 x_mm = (static_cast<f64>(instance.cell.x) * k_world_cell_m +
                            static_cast<f64>(instance.rows[0].w)) *
                           1000.0;
          const f64 z_mm = (static_cast<f64>(instance.cell.z) * k_world_cell_m +
                            static_cast<f64>(instance.rows[2].w)) *
                           1000.0;
          gfx::ResolveMaterial& optics = material_table[entry.optics];
          optics.sampler = mesh_material_base[data_->terrain_mesh];
          optics.uv_transform =
              Vec4{static_cast<f32>((x_mm - static_cast<f64>(frame.x0_mm)) * per_mm),
                   static_cast<f32>((z_mm - static_cast<f64>(frame.z0_mm)) * per_mm),
                   static_cast<f32>(1000.0 * per_mm), optics.uv_transform.w};
          break;
        }
      }
    }
  }
  // The mesh room's material arena, after the scene's own entries (gpu_scene_meshes.cpp): the
  // default material until a mesh that arrives with a tile brings its own into a run of it.
  if (mesh_room_.slots != 0) {
    mesh_material_base_arena_ = material_table.size();
    for (u32 k = 0; k < mesh_room_.materials; ++k) {
      material_table.push_back(gfx::ResolveMaterial{});
      table_material.push_back(u8{1});
    }
    free_mesh_materials_.clear();
    free_mesh_materials_.push_back(Range{mesh_material_base_arena_, mesh_room_.materials});
  }
  material_count_ = material_table.size();
  has_water_ = false;
  has_terrain_materials_ = false;
  has_masked_ = false;
  u32 stochastic = 0;
  ENGINE_ASSERT(table_material.size() == material_table.size(), "one mark per table entry");
  for (u32 e = 0; e < material_table.size(); ++e) {
    // Only the materials: a record's word at `flags`' offset is not flags (R141, above).
    if (e < table_material.size() && table_material[e] == 0) continue;
    const gfx::ResolveMaterial& material = material_table[e];
    if ((material.flags & (gfx::k_material_stochastic | gfx::k_material_water |
                           gfx::k_material_terrain_layers | gfx::k_material_alpha_mask)) ==
        gfx::k_material_stochastic)
      ++stochastic;
    if ((material.flags & gfx::k_material_water) != 0) has_water_ = true;
    if ((material.flags & gfx::k_material_terrain_layers) != 0) has_terrain_materials_ = true;
    if ((material.flags & gfx::k_material_alpha_mask) != 0) has_masked_ = true;
  }
  // A generator said its tiles may hand an alpha-tested mesh (`tile_meshes_masked`): the masked
  // pipelines are chosen from this flag when the scene's renderer is made, before any tile is.
  if (mesh_room_.slots != 0 && data_->tile_meshes_masked) has_masked_ = true;
  // The resolve's stochastic variant is the one only such a scene draws with (scene_renderer.cpp,
  // `resolve_module`).
  has_stochastic_materials_ = stochastic != 0;
  if (stochastic != 0)
    ENGINE_LOG_INFO(log_renderer, "stochastic materials", log::field("materials", stochastic));
  // The open sea (water.md, "To the horizon"): the instance of the scene's open body and its water
  // material — the mesh's one material, at the mesh's base — which the resolve's water modules draw
  // the plane past its outline by. The water generator lets a scene have one.
  water_open_instance_ = gfx::k_no_water_open;
  water_open_material_ = 0;
  for (u32 m = 0; m < data_->mesh_info.size() && m < mesh_material_base.size(); ++m) {
    if (!data_->mesh_info[m].water.has_value() || !data_->mesh_info[m].water->body.open) continue;
    for (u32 i = 0; i < data_->instances.size(); ++i) {
      if (data_->instances[i].mesh != m) continue;
      water_open_instance_ = i;
      water_open_material_ = mesh_material_base[m];
      break;
    }
    break;
  }
  if (!upload_water_shores(error)) return false;
  collect_water_reflect_bodies();  // R129: the reflection's planes
  collect_water_refract_bodies();  // S86: what is under the water
  // The opaque shadow proxy's list (R109): every alpha-tested cluster and its material, for the
  // bake to measure its coverage. Only a scene read whole: a streamed one's clusters live in a page
  // pool whose slots change under any table made now.
  coverage_list_.clear();
  shadow_coverage_clusters_ = 0;
  // The sweep's proxy rules (R115 stage 2, `renderer.shadow.sweep`) read the table too.
  if (resolved.shadow_coverage_table && has_masked_ && !streamed_) {
    for (u32 m = 0; m < data_->parts.size(); ++m) {
      const geometry::ClusterMeshPart& part = data_->parts[m];
      for (u32 i = 0; i < part.cluster_count; ++i) {
        const u32 c = part.first_cluster + i;
        if (c >= cluster_count_) break;
        const u32 material = cluster_material[c] + mesh_material_base[m];
        if (material < material_table.size() &&
            (material_table[material].flags & gfx::k_material_alpha_mask) != 0) {
          coverage_list_.push_back(c);
          coverage_list_.push_back(material);
        }
      }
    }
  }
  // The opacity micromaps' list (R120): the same clusters as the proxy's, each with its triangle
  // count, for `bake_micromaps`. Only a scene read whole, for the proxy's reason.
  micromap_list_.clear();
  micromap_triangles_.clear();
  micromap_level_ = resolved.micromap_level;
  micromap_budget_ = resolved.micromap_budget_bytes;
  micromap_texels_ = resolved.micromap_texels;
  if (resolved.shadow_micromaps && ray_tracing_ && has_masked_ && !streamed_) {
    for (u32 m = 0; m < data_->parts.size(); ++m) {
      const geometry::ClusterMeshPart& part = data_->parts[m];
      for (u32 i = 0; i < part.cluster_count; ++i) {
        const u32 c = part.first_cluster + i;
        if (c >= cluster_count_) break;
        const u32 material = cluster_material[c] + mesh_material_base[m];
        if (material < material_table.size() &&
            (material_table[material].flags & gfx::k_material_alpha_mask) != 0) {
          micromap_list_.push_back(c);
          micromap_list_.push_back(material);
          micromap_triangles_.push_back(data_->lod.mesh.clusters[c].triangle_count);
        }
      }
    }
  }
  // A rigid form's frontier, drawn at every distance (upload_geometry): its coverage errors follow
  // the geometric ones — the frontier's parent half and every other cluster's own half read +inf,
  // so the foliage threshold cannot move the fixed cut either. And which frontier clusters are
  // alpha-tested, since their static structures are built once and a masked one non-opaque.
  if (static_forms_) {
    static_masked_.assign(static_cluster_ids_.size(), u8{0});
    for (u32 m = 0; m < static_of_mesh_.size(); ++m) {
      const u32 s = static_of_mesh_[m];
      if (s == ~0u) continue;
      for (u32 k = static_first_[s]; k < static_first_[s] + static_counts_[s]; ++k) {
        const u32 material = cluster_material[static_cluster_ids_[k]] + mesh_material_base[m];
        if (material < material_table.size() &&
            (material_table[material].flags & gfx::k_material_alpha_mask) != 0) {
          static_masked_[k] = 1u;
        }
      }
    }
    if (!coverage_words.empty()) {
      for (u32 m = 0; m < static_of_mesh_.size(); ++m) {
        if (static_of_mesh_[m] == ~0u) continue;
        const geometry::ClusterMeshPart& part = data_->parts[m];
        for (u32 i = 0; i < part.cluster_count; ++i) {
          const u32 c = part.first_cluster + i;
          u32& word = coverage_words[c];
          word = data_->lod.lod[c].level == 0 ? (word & 0xffffu) | 0x7C000000u
                                              : (word & 0xffff0000u) | 0x7C00u;
        }
      }
    }
  }
  // A dynamic scene's instance table is a set per frame in flight, made by `create` once the mesh
  // table (and its null mesh) exists.
  has_coverage_ = !coverage_words.empty();
  if (has_coverage_ &&
      !gfx::upload_buffer(device, coverage_words.data(), u64{cluster_count_} * sizeof(u32),
                          k_storage, lod_coverage, error)) {
    return false;
  }
  return gfx::upload_buffer(device, material_table.data(),
                            u64{material_count_} * sizeof(gfx::ResolveMaterial), k_storage,
                            materials, error) &&
         gfx::upload_buffer(device, cluster_material.data(), u64{cluster_count_} * sizeof(u32),
                            k_storage, cluster_materials, error) &&
         (dynamic_ || gfx::upload_buffer(device, instance_table_.data(),
                                         u64{instance_count_} * sizeof(gfx::InstanceDesc),
                                         k_storage, instances, error));
}

// **The opaque shadow proxy's bake** (alpha_coverage.slang; renderer.md, "The opaque shadow proxy";
// R109): one dispatch, once, over the alpha-tested clusters `upload_materials` listed, into a
// zeroed table of a byte per cluster. It waits for the device, as every upload of a scene's
// creation does, and keeps nothing but the table: the list and the pipeline go with it.
bool GpuScene::bake_shadow_coverage(gfx::ShaderLibrary& shaders, std::string* error) {
  if (shadow_coverage.address != 0 || coverage_list_.empty()) return true;
  ENGINE_STARTUP_STAGE("shadow coverage");
  const gfx::Device& device = *device_;
  const u32 count = coverage_list_.size() / 2;
  const gfx::Shader* shader = shaders.get("alpha_coverage", error);
  if (shader == nullptr) return false;
  const gfx::DescriptorSetLayoutHandle set_layout = bindless_.layout();
  const std::span<const gfx::DescriptorSetLayoutHandle> layouts(&set_layout, 1);
  gfx::ComputePipeline pipeline;
  if (!gfx::create_compute_pipeline(device, shader->module, "alpha_coverage_main", layouts,
                                    sizeof(AlphaCoverageParams), pipeline, error)) {
    return false;
  }
  const Vector<u32> zeros((cluster_count_ + 3) / 4, 0u);
  gfx::BufferResource list;
  bool ok = gfx::upload_buffer(device, coverage_list_.data(),
                               u64{coverage_list_.size()} * sizeof(u32), k_storage, list, error) &&
            gfx::upload_buffer(device, zeros.data(), u64{zeros.size()} * sizeof(u32), k_storage,
                               shadow_coverage, error);
  if (ok) {
    AlphaCoverageParams params;
    params.clusters = clusters.address;
    params.triangles = triangles.address;
    params.quantized = quantized.address;
    params.attributes = attributes.address;
    params.materials = materials.address;
    params.list = list.address;
    params.out = shadow_coverage.address;
    params.count = count;
    const u32 groups_x = count < k_alpha_coverage_row ? count : k_alpha_coverage_row;
    const u32 groups_y = (count + k_alpha_coverage_row - 1) / k_alpha_coverage_row;
    const gfx::BindlessSet& bindless = bindless_;
    ok = gfx::submit_immediate(
        device,
        [&](gfx::CommandList cb) {
          cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
          bindless.bind(cb, gfx::BindPoint::Compute);
          cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(params), &params);
          cb.dispatch(groups_x, groups_y, 1);
        },
        error);
  }
  gfx::destroy_buffer(device, list);
  gfx::destroy_compute_pipeline(device, pipeline);
  if (!ok) {
    gfx::destroy_buffer(device, shadow_coverage);
    return false;
  }
  shadow_coverage_clusters_ = cluster_count_;
  ENGINE_LOG_INFO(log_renderer, "shadow coverage baked", log::field("masked_clusters", count),
                  log::field("table_bytes", u64{zeros.size()} * sizeof(u32)));
  coverage_list_ = Vector<u32>{};
  return true;
}

// **The opacity micromaps' bake** (gfx/opacity_micromap.h; renderer.md, "Opacity micromaps";
// R120): once, over the alpha-tested clusters `upload_materials` listed, blocking as every upload
// of a scene's creation is. Then the rigid forms' static structures, built at create before the
// shaders existed, are built again naming the micromaps, before any frame references them.
bool GpuScene::bake_micromaps(gfx::ShaderLibrary& shaders, std::string* error) {
  if (micromaps_.address != 0 || micromap_list_.empty()) return true;
  ENGINE_STARTUP_STAGE("opacity micromaps");
  const gfx::Device& device = *device_;
  gfx::MicromapBakeInput in;
  in.clusters = clusters.address;
  in.triangles = triangles.address;
  in.attributes = attributes.address;
  in.materials = materials.address;
  in.items = std::span<const u32>(micromap_list_.data(), micromap_list_.size());
  in.triangle_counts = std::span<const u32>(micromap_triangles_.data(), micromap_triangles_.size());
  in.cluster_count = cluster_count_;
  u32 limit_2 = 0;
  u32 limit_4 = 0;
  gfx::opacity_micromap_limits(device, limit_2, limit_4);
  in.max_level = std::min(micromap_level_, limit_4);
  in.budget_bytes = micromap_budget_;
  in.target_texels = micromap_texels_;
  if (!gfx::bake_opacity_micromaps(device, shaders, bindless_, in, micromaps_, error)) {
    return false;
  }
  micromap_list_ = Vector<u32>{};
  micromap_triangles_ = Vector<u32>{};
  const gfx::MicromapStats& s = micromaps_.stats;
  u32 entries = 0;
  // Per level, `level:triangles wanting it`, `level:triangles/stored entries`, and
  // `level:unknown/micro-triangles` (R120 stage 3's histogram), as text for the log line.
  std::string wanted;
  std::string stored;
  std::string unknown;
  for (u32 l = 0; l <= gfx::k_micromap_max_level; ++l) {
    entries += s.entries_per_level[l];
    if (s.wanted_per_level[l] != 0)
      wanted += std::to_string(l) + ":" + std::to_string(s.wanted_per_level[l]) + " ";
    if (s.entries_per_level[l] != 0)
      stored += std::to_string(l) + ":" + std::to_string(s.entries_per_level[l]) + "/" +
                std::to_string(s.unique_per_level[l]) + " ";
    if (s.micro_per_level[l] != 0)
      unknown += std::to_string(l) + ":" + std::to_string(s.unknown_per_level[l]) + "/" +
                 std::to_string(s.micro_per_level[l]) + " ";
  }
  ENGINE_LOG_INFO(
      log_renderer, "opacity micromaps baked", log::field("triangles", s.triangles),
      log::field("with_data", entries), log::field("unique_entries", s.unique_entries),
      log::field("dedup_mismatches", s.dedup_mismatches),
      log::field("fully_opaque", s.special_opaque),
      log::field("fully_transparent", s.special_transparent),
      log::field("fully_unknown", s.special_unknown), log::field("micro_unknown", s.micro_unknown),
      log::field("micro_unknown_filter", s.micro_unknown_filter),
      log::field("micro_known", s.micro_opaque + s.micro_transparent),
      log::field("level_cap", s.level_cap), log::field("levels_dropped", s.levels_dropped),
      log::field("wanted", std::string_view{wanted}),
      log::field("stored", std::string_view{stored}),
      log::field("unknown", std::string_view{unknown}), log::field("data_bytes", s.data_bytes),
      log::field("resident_bytes", micromap_bytes()), log::field("bake_ms", s.bake_ms));
  if (micromaps_.address != 0 && static_build_ && static_clusters_ > 0) {
    gfx::destroy_cluster_set(device, static_clas_);
    gfx::destroy_cluster_blas_set(device, static_blas_);
    if (!build_static_forms(error)) return false;
  }
  return true;
}

bool GpuScene::create_working_set(const ResolvedSettings& resolved, std::string* error) {
  ENGINE_STARTUP_STAGE("working set");
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
  bool ok =
      gfx::create_buffer(device,
                         visible_run_bytes_ * (k_visible_runs * view_count_ + cascade_count_),
                         k_readable, false, visible, error) &&
      (cascade_count_ == 0 ||
       gfx::create_buffer(device, u64{gfx::k_draw_args_bytes} * cascade_count_, k_args, false,
                          shadow_args, error)) &&
      gfx::create_buffer(device, u64{pair_stride_} * view_count_ * sizeof(u32), k_address, false,
                         pair_entries, error) &&
      // The cross-fade's tables (renderer.md, "The cross-fade"): a u32x2 per pair for every
      // view and one the cascades share, only in a scene with coverage errors.
      (!has_coverage_ || gfx::create_buffer(device, u64{pair_stride_} * (view_count_ + 1u) * 8u,
                                            k_address, false, fade, error)) &&
      gfx::create_buffer(device, u64{gfx::k_draw_args_bytes} * view_count_, k_args, false, sw_args,
                         error);
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
  ENGINE_STARTUP_STAGE("ray tracing");
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
  //
  // This half is the scene's — the 8-bit indices, the record count, the templates — and is made
  // once; the half laid out by the instances and the pairs is `lay_out_ray_tracing`'s, which a
  // dynamic scene makes again when its tail outgrows it (`relayout`).
  Vector<u8> packed;
  // Under streaming the 8-bit indices are one more page-pool stream, filled a page at a time
  // beside the float positions, because a CLAS record addresses them at the same slot-relative
  // `triangle_offset` the rasterizers read.
  if (!streamed_) {
    gfx::pack_cluster_indices(
        std::span<const u32>(lod.mesh.triangles.data(), lod.mesh.triangles.size()), packed);
  }
  gfx::ClusterSetLimits limits;
  limits.max_triangles_per_cluster = triangles_per_cluster_;
  limits.max_vertices_per_cluster = geometry::ClusterLodOptions{}.max_vertices;
  // A template is built once, opaque, and an instantiation cannot clear the flag, so a scene with
  // an alpha-tested material builds records instead (renderer.md, "Alpha-tested geometry").
  const bool templates = resolved.settings.rt_templates && !has_masked_;
  limits.instantiate = templates;
  // Every set, the cache's slots and the static forms are sized and built allowing micromaps when
  // the scene will bake them (R120), since the bake comes after this, with the renderer's shaders.
  limits.opacity_micromaps = !micromap_list_.empty();
  rt_limits_ = limits;
  constexpr gfx::BufferUsage k_record_usage = k_address | gfx::BufferUsage::AccelerationBuildInput;
  bool ok =
      (streamed_ ||
       upload_room(device, packed.data(), packed.size(),
                   triangle_capacity_ > scene_triangle_count_ ? triangle_capacity_ * 3 + 4 : 0,
                   gfx::k_build_input_usage, indices8, error)) &&
      gfx::create_buffer(device, sizeof(u32) * gfx::k_cluster_record_count_words,
                         k_record_usage | gfx::BufferUsage::TransferSrc, false, record_count,
                         error) &&
      // Each chunk's count of the frame's cluster set (R152; clas_chunks.slang).
      gfx::create_buffer(device, sizeof(u32) * k_rt_chunks_max, k_record_usage, false,
                         clas_chunk_counts, error);
  // One template per cluster of the scene, built from the rest pose, with cluster id and base
  // geometry index zero so an instantiate record's offsets are the visible entry outright. Built
  // once, **packed to the size each one is** rather than to the driver's worst case (about 2.2 KB
  // reserved against 504–532 bytes used on the desert overlook), and instantiated from the pool
  // every frame.
  if (ok && templates) {
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
  // The rigid forms' structures, once (renderer.md, "Static structures for rigid forms").
  if (ok) ok = build_static_forms(error);
  // The rigid meshes' cluster structures, built once each as frames first draw them (renderer.md,
  // "The resident cluster structure cache").
  if (ok) ok = create_rt_cache(error);
  if (!ok) return false;
  return lay_out_ray_tracing(error);
}

// **Static structures for rigid forms** (renderer.md, "Static structures for rigid forms"). A
// streamed world's forms past the first ring are meshes of their own (`SceneData::form_source`),
// and a rigid one (`form_rigid`) is drawn by no deformation. Its frontier — the clusters
// `extract_lod_form` left at level 0, one on every path of the form's DAG — is a complete cut, and
// drawn at every distance (upload_geometry's patch) it is the form's only cut: so its structures
// are a function of the mesh alone and are built here, once, instead of once per instance per
// frame.
void GpuScene::plan_static_forms() {
  static_forms_ = false;
  static_build_ = false;
  static_of_mesh_.clear();
  static_first_.clear();
  static_counts_.clear();
  static_cluster_ids_.clear();
  static_masked_.clear();
  static_count_ = 0;
  static_clusters_ = 0;
  if (!ray_tracing_ || !dynamic_ || !resolved_.static_forms || streamed_) return;
  const SceneData& data = *data_;
  static_of_mesh_.assign(data.parts.size(), ~0u);
  for (u32 m = 0; m < data.parts.size(); ++m) {
    const bool form = m < data.form_source.size() && data.form_source[m] != m;
    const bool rigid = m < data.form_rigid.size() && data.form_rigid[m] != 0;
    if (!form || !rigid) continue;
    const geometry::ClusterMeshPart& part = data.parts[m];
    const u32 first = static_cluster_ids_.size();
    for (u32 i = 0; i < part.cluster_count; ++i) {
      const u32 c = part.first_cluster + i;
      if (data.lod.lod[c].level == 0) static_cluster_ids_.push_back(c);
    }
    const u32 count = static_cluster_ids_.size() - first;
    if (count == 0) continue;
    static_of_mesh_[m] = static_count_++;
    static_first_.push_back(first);
    static_counts_.push_back(count);
  }
  static_clusters_ = static_cluster_ids_.size();
  static_forms_ = static_count_ > 0;
  // The fixed cut without the structures built once: the per-frame build of the same cut, which
  // the tests and the measurement compare the static structures against.
  static_build_ = static_forms_ && rt_static_build_tunable();
  if (!static_forms_) static_of_mesh_.clear();
}

bool GpuScene::build_static_forms(std::string* error) {
  static_bytes_ = 0;
  if (!static_build_) return true;
  ENGINE_STARTUP_STAGE("static ray tracing structures");
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  const u32 mesh_count = data_->parts.size();
  u32 most_clusters = 0;
  u32 most_per_form = 0;
  for (u32 m = 0; m < mesh_count; ++m) {
    const u32 s = static_of_mesh_[m];
    if (s == ~0u) continue;
    most_clusters = std::max(most_clusters, data_->parts[m].cluster_count);
    most_per_form = std::max(most_per_form, static_counts_[s]);
  }
  // One structure per frontier cluster, its cluster id and base geometry index the cluster's index
  // within its mesh — what a hit on a flagged instance decodes (scene.slang, `hit_pair`) — opaque
  // but where its material is alpha-tested, as the frame's records are written.
  Vector<gfx::ClusterBuildInput> inputs;
  inputs.reserve(static_clusters_);
  for (u32 m = 0; m < mesh_count; ++m) {
    const u32 s = static_of_mesh_[m];
    if (s == ~0u) continue;
    const geometry::ClusterMeshPart& part = data_->parts[m];
    for (u32 k = static_first_[s]; k < static_first_[s] + static_counts_[s]; ++k) {
      const u32 c = static_cluster_ids_[k];
      const geometry::ClusterDesc& desc = lod.mesh.clusters[c];
      gfx::ClusterBuildInput in;
      in.cluster_id = c - part.first_cluster;
      in.triangle_count = desc.triangle_count;
      in.vertex_count = desc.vertex_count;
      in.vertices = vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
      in.indices = indices8.address + u64{desc.triangle_offset} * 3;
      in.opaque = !(k < static_masked_.size() && static_masked_[k] != 0);
      // A masked frontier cluster names its micromap (R120): a static form never fades (its LOD is
      // the fixed frontier), so nothing a ray loop decides by the pixel is taken from it.
      if (!in.opaque && micromaps_.address != 0 && c < micromaps_.cluster_first_host.size() &&
          micromaps_.cluster_first_host[c] != gfx::k_micromap_none) {
        in.omm_array = micromaps_.address;
        in.omm_indices =
            micromaps_.indices.address + u64{micromaps_.cluster_first_host[c]} * sizeof(i32);
      }
      inputs.push_back(in);
    }
  }
  gfx::ClusterSetLimits limits = rt_limits_;
  limits.max_clusters = static_clusters_;
  limits.max_geometry_index = most_clusters > 0 ? most_clusters - 1 : 0;
  limits.instantiate = false;
  constexpr gfx::BufferUsage k_record_usage = k_address | gfx::BufferUsage::AccelerationBuildInput;
  gfx::BufferResource clas_records;
  gfx::BufferResource blas_records_once;
  gfx::BufferResource scratch;
  // One bottom-level record per **mesh**, not per static form: a mesh with none builds an empty
  // structure, whose address the build writes as zero, so `static_blas_.addresses` is the per-mesh
  // table `tlas_references.slang` reads with no copy in between.
  bool ok = gfx::create_buffer(device, gfx::k_cluster_build_record_bytes * static_clusters_,
                               k_record_usage, true, clas_records, error) &&
            gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * mesh_count,
                               k_record_usage, true, blas_records_once, error);
  // **At the size they are** (R143; renderer.md, "Static structures for rigid forms"): the forms'
  // cluster structures are built once and kept, so they are sized first and packed
  // (`gfx::create_packed_cluster_set`) instead of each being reserved at the driver's most for a
  // cluster, as the frame's own set has to be. What they would have reserved is kept for the log
  // and the statistics (`static_reserved_bytes()`).
  static_reserved_bytes_ = 0;
  if (ok) {
    gfx::ClusterBuildSizes reserved;
    if (gfx::cluster_set_build_sizes(device, limits, reserved)) {
      static_reserved_bytes_ = reserved.data_bytes;
    }
    gfx::write_cluster_build_records(
        std::span<const gfx::ClusterBuildInput>(inputs.data(), inputs.size()), clas_records.mapped);
    ok =
        gfx::create_packed_cluster_set(device, limits, clas_records.address, static_clas_, error) &&
        gfx::create_cluster_blas_set(device, mesh_count, static_clusters_, most_per_form,
                                     static_blas_, error, limits.opacity_micromaps);
  }
  if (ok) ok = gfx::create_scratch(device, static_blas_.build_scratch_bytes, scratch, error);
  if (ok) {
    auto* bytes = static_cast<u8*>(blas_records_once.mapped);
    for (u32 m = 0; m < mesh_count; ++m) {
      const u32 s = static_of_mesh_[m];
      const u32 count = s == ~0u ? 0u : static_counts_[s];
      const u32 stride = sizeof(u64);
      const u64 references =
          s == ~0u ? 0u : static_clas_.addresses.address + u64{static_first_[s]} * sizeof(u64);
      u8* record = bytes + u64{m} * gfx::k_cluster_blas_record_bytes;
      std::memcpy(record, &count, sizeof(count));
      std::memcpy(record + 4, &stride, sizeof(stride));
      std::memcpy(record + 8, &references, sizeof(references));
    }
    ok = gfx::submit_immediate(
        device,
        [&](gfx::CommandList cb) {
          gfx::build_cluster_blas_set(cb, static_blas_, blas_records_once.address, 0, scratch);
          gfx::acceleration_build_barrier(cb, gfx::PipelineStage::ComputeShader,
                                          gfx::MemoryAccess::AccelerationStructureRead);
        },
        error);
  }
  gfx::destroy_buffer(device, clas_records);
  gfx::destroy_buffer(device, blas_records_once);
  gfx::destroy_buffer(device, scratch);
  if (!ok) {
    for (gfx::BufferResource& buffer : rt_order_buffers_)
      gfx::destroy_buffer(device, buffer);
    rt_order_buffers_.clear();
    rt_order_written_.clear();
    rt_order_.clear();
    slot_rank_.clear();
    gfx::destroy_cluster_set(device, static_clas_);
    gfx::destroy_cluster_blas_set(device, static_blas_);
    return false;
  }
  static_bytes_ = static_clas_.data.size + static_clas_.addresses.size + static_clas_.sizes.size +
                  static_blas_.data.size + static_blas_.addresses.size;
  ENGINE_LOG_INFO(log_renderer, "static ray tracing structures built",
                  log::field("forms", static_count_), log::field("clusters", static_clusters_),
                  log::field("bytes", static_bytes_),
                  log::field("cluster_bytes", static_clas_.data.size),
                  log::field("cluster_bytes_unpacked", static_reserved_bytes_));
  return true;
}

// **The resident cluster structure cache** (renderer.md, "The resident cluster structure cache";
// R101; gfx::ClusterCacheTables). Every mesh of the scene's own but a static form, up to
// `gfx::k_cluster_cache_max_clusters` clusters, gets three keys a cluster; the slots are the
// driver's most for one cluster each, as many as the cached clusters twice over (the drawn and the
// caster forms) up to the ceiling — `renderer.rt.cache_mib`, or with it 0 a sixty-fourth of the
// device (R114, `resolve_rt_cache_mib`) — made here once and never grown: a cache that fills
// evicts, which is a cost the frame pays in builds and never a reallocation inside a flight, and
// what a frame cannot place in it the frame builds in its own set (`cache_assign_main`). Slot 0 is
// built now, with no active triangle, for a reference neither had room for.
bool GpuScene::create_rt_cache(std::string* error) {
  rt_cache_base_.clear();
  rt_cache_slots_ = rt_cache_meshes_ = rt_cache_clusters_ = 0;
  rt_cache_bytes_ = rt_cache_stride_ = rt_cache_scratch_bytes_ = 0;
  rt_cache_ceiling_mib_ = 0;
  if (!ray_tracing_ || rt_limits_.instantiate || streamed_ || !rt_cache_tunable()) return true;
  const gfx::Device& device = *device_;
  gfx::ClusterAsProperties props;
  if (!gfx::cluster_as_properties(device, props) || props.max_geometry_index < 0xffffffu) {
    return true;  // a cached structure's name needs all 24 bits (gfx::k_cluster_cache_geometry)
  }
  const SceneData& data = *data_;
  // **Only a mesh an instance could be served from** (R106). A scene read whole has every instance
  // it will ever have, so a mesh none of whose instances is rigid and traced — the erg's one
  // terrain mesh, 741,526 clusters and the whole 256 MiB ceiling of slots, every instance of it
  // deformed by the terrain stage — gets no keys and no slots; with no such mesh there is no cache.
  // A scene whose instances come and go keeps every mesh: its tail may place any of them rigid.
  Vector<u8> served;
  if (!dynamic_) {
    served.assign(data.parts.size(), u8{0});
    for (const gfx::InstanceDesc& instance : instance_table_) {
      const bool rigid =
          instance.deform == gfx::k_invalid_deform &&
          (instance.flags & (gfx::k_instance_static_rt | gfx::k_instance_untraced)) == 0;
      if (rigid && instance.mesh < served.size()) served[instance.mesh] = 1;
    }
  }
  rt_cache_base_.assign(data.parts.size(), ~0u);
  u64 cached = 0;
  for (u32 m = 0; m < data.parts.size(); ++m) {
    if (static_build_ && static_mesh(m)) continue;  // built once already, by its frontier
    if (!dynamic_ && served[m] == 0) continue;      // no instance of it can be served
    const u32 count = data.parts[m].cluster_count;
    if (count == 0 || count > gfx::k_cluster_cache_max_clusters) continue;
    rt_cache_base_[m] = static_cast<u32>(cached);
    cached += count;
    ++rt_cache_meshes_;
  }
  if (cached == 0 || cached * gfx::k_cluster_cache_forms > 0xfff00000u) {
    rt_cache_base_.clear();
    rt_cache_meshes_ = 0;
    return true;
  }
  gfx::ClusterSetLimits limits = rt_limits_;
  limits.instantiate = false;
  limits.max_geometry_index = 0xffffffu;
  limits.max_clusters = 1;
  gfx::ClusterExplicitSizes one;
  if (!gfx::cluster_explicit_sizes(device, limits, one)) {
    if (error != nullptr) *error = "the driver gave no size for one cached cluster structure";
    return false;
  }
  // **The ceiling follows the device** (R114): the heaps' size, as the chain's budget reads it, so
  // a card gives the same cache whatever else is running on it.
  gfx::MemoryBudget memory;
  (void)device.memory_budget(memory);
  const RtBudget cache_ceiling =
      resolve_rt_cache_mib(rt_cache_mib_tunable(), memory.device_local_bytes);
  rt_cache_ceiling_mib_ = cache_ceiling.mib;
  rt_cache_ceiling_source_ = cache_ceiling.source;
  const u64 ceiling = u64{cache_ceiling.mib} * 1024 * 1024 / one.structure_bytes;
  // Under bit 31 of a key (`k_cache_frame`, clas_records.slang), which marks a structure the frame
  // built in its own set.
  const u64 slot_ceiling = rt_cache_slots_tunable() != 0 ? rt_cache_slots_tunable() : 0x7fffffffu;
  const u64 slot_total = std::min<u64>({2 * cached + 1, ceiling, slot_ceiling});
  if (slot_total < 2) {
    rt_cache_base_.clear();
    rt_cache_meshes_ = 0;
    return true;
  }
  limits.max_clusters = static_cast<u32>(slot_total);
  gfx::ClusterExplicitSizes sizes;
  if (!gfx::cluster_explicit_sizes(device, limits, sizes)) {
    if (error != nullptr) {
      *error = "the driver gave no sizes for a cluster cache of " + std::to_string(slot_total);
    }
    return false;
  }
  rt_cache_limits = limits;
  rt_cache_alignment = sizes.alignment;
  rt_cache_stride_ = sizes.structure_bytes;
  rt_cache_scratch_bytes_ = sizes.scratch_bytes;
  const u32 slot_count = static_cast<u32>(slot_total);
  const u64 keys = cached * gfx::k_cluster_cache_forms;
  RtCacheBuffers& c = rt_cache_buffers;
  constexpr gfx::BufferUsage k_record_usage = k_address | gfx::BufferUsage::AccelerationBuildInput;
  constexpr gfx::BufferUsage k_structures = gfx::BufferUsage::AccelerationStorage |
                                            gfx::BufferUsage::ShaderDeviceAddress |
                                            gfx::BufferUsage::Storage;
  // Every key empty, every slot free but slot 0, held by a mark no key is.
  Vector<u32> empty_keys(static_cast<u32>(keys), gfx::k_cluster_cache_empty);
  Vector<u32> owners(slot_count, gfx::k_cluster_cache_empty);
  owners[0] = gfx::k_cluster_cache_claim;
  Vector<u32> zeros(std::max(slot_count, gfx::k_cluster_cache_scratch_words), 0u);
  // The args block starts every dispatch empty (`ranges_count_cached_main` writes it again every
  // frame, before anything reads it).
  u32 empty_args[gfx::k_cluster_cache_args_words] = {};
  for (u32 w = 0; w + 2 < gfx::k_cluster_cache_args_pending; w += 3)
    empty_args[w + 1] = empty_args[w + 2] = 1u;
  bool ok =
      gfx::upload_buffer(device, rt_cache_base_.data(), u64{rt_cache_base_.size()} * sizeof(u32),
                         k_address, c.bases, error) &&
      gfx::upload_buffer(device, empty_keys.data(), keys * sizeof(u32), k_address, c.keys, error) &&
      gfx::upload_buffer(device, owners.data(), u64{slot_count} * sizeof(u32), k_address, c.owners,
                         error) &&
      gfx::upload_buffer(device, zeros.data(), u64{slot_count} * sizeof(u32), k_address, c.stamps,
                         error) &&
      gfx::create_buffer(device, u64{slot_count} * sizeof(u32), k_address, false, c.free_list,
                         error) &&
      // The misses' list is the references' length (`resize_ray_tracing`): a frame can miss more
      // keys than the cache has slots (R114), never more than it references.
      gfx::create_buffer(device, u64{slot_count} * gfx::k_cluster_build_record_bytes,
                         k_record_usage, false, c.miss_records, error) &&
      gfx::create_buffer(device, u64{slot_count} * sizeof(u64), k_record_usage, false,
                         c.miss_destinations, error) &&
      // Zeroed: the slots occupied, its last word, carries from frame to frame and starts at none.
      gfx::upload_buffer(device, zeros.data(),
                         u64{gfx::k_cluster_cache_scratch_words} * sizeof(u32), k_address,
                         c.scratch, error) &&
      gfx::upload_buffer(device, empty_args, sizeof(empty_args),
                         k_address | gfx::BufferUsage::Indirect, c.args, error) &&
      gfx::create_buffer(device, u64{slot_count} * rt_cache_stride_ + one.alignment, k_structures,
                         false, c.slots, error) &&
      // One copy a frame slot (R130, `rt_cache_tables_address`).
      gfx::create_buffer(device, sizeof(gfx::ClusterCacheTables) * k_rt_cache_table_copies,
                         k_address, true, c.tables, error);
  // Slot 0: one triangle whose every vertex's x is NaN, which the device treats as inactive — a
  // structure nothing can hit, for a reference whose key had no slot.
  if (ok) {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 positions[9] = {nan, 0.0f, 0.0f, nan, 1.0f, 0.0f, nan, 0.0f, 1.0f};
    const u8 indices[4] = {0, 1, 2, 0};
    const u64 alignment = one.alignment != 0 ? one.alignment : 1;
    const u64 slot_base = (c.slots.address + alignment - 1) / alignment * alignment;
    gfx::BufferResource vertices_once, indices_once, record_once, destination_once, scratch_once;
    gfx::ClusterSetLimits single = limits;
    single.max_clusters = 1;
    ok = gfx::upload_buffer(device, positions, sizeof(positions), gfx::k_build_input_usage,
                            vertices_once, error) &&
         gfx::upload_buffer(device, indices, sizeof(indices), gfx::k_build_input_usage,
                            indices_once, error) &&
         gfx::create_buffer(device, gfx::k_cluster_build_record_bytes, k_record_usage, true,
                            record_once, error) &&
         gfx::create_buffer(device, sizeof(u64), k_record_usage, true, destination_once, error) &&
         gfx::create_scratch(device, one.scratch_bytes, scratch_once, error);
    if (ok) {
      gfx::ClusterBuildInput in;
      in.cluster_id = gfx::k_cluster_cache_geometry;
      in.triangle_count = 1;
      in.vertex_count = 3;
      in.vertices = vertices_once.address;
      in.indices = indices_once.address;
      in.opaque = true;
      gfx::write_cluster_build_records(std::span<const gfx::ClusterBuildInput>(&in, 1),
                                       record_once.mapped);
      std::memcpy(destination_once.mapped, &slot_base, sizeof(slot_base));
      ok = gfx::submit_immediate(
          device,
          [&](gfx::CommandList cb) {
            gfx::build_cluster_set_explicit(cb, single, record_once.address, 0,
                                            destination_once.address, scratch_once, one.alignment);
            gfx::acceleration_build_barrier(
                cb, gfx::PipelineStage::AllCommands,
                gfx::MemoryAccess::MemoryRead | gfx::MemoryAccess::MemoryWrite);
          },
          error);
    }
    for (gfx::BufferResource* b :
         {&vertices_once, &indices_once, &record_once, &destination_once, &scratch_once})
      gfx::destroy_buffer(device, *b);
  }
  if (!ok) {
    destroy_rt_cache();
    return false;
  }
  rt_cache_slots_ = slot_count;
  rt_cache_clusters_ = static_cast<u32>(cached);
  rt_cache_bytes_ = c.bases.size + c.keys.size + c.owners.size + c.stamps.size + c.free_list.size +
                    c.miss_records.size + c.miss_destinations.size + c.scratch.size + c.args.size +
                    c.slots.size + c.tables.size;
  write_rt_cache_tables();
  count_rt_cache_instances();
  ENGINE_LOG_INFO(log_renderer, "cluster structure cache ready",
                  log::field("meshes", rt_cache_meshes_), log::field("clusters", cached),
                  log::field("slots", slot_count), log::field("slot_bytes", rt_cache_stride_),
                  log::field("bytes", rt_cache_bytes_),
                  log::field("ceiling_mib", rt_cache_ceiling_mib_),
                  log::field("ceiling_source", rt_budget_source_name(rt_cache_ceiling_source_)),
                  log::field("device_local_mib", memory.device_local_bytes >> 20));
  return true;
}

u64 GpuScene::rt_cache_tables_address(u32 slot) noexcept {
  const RtCacheBuffers& c = rt_cache_buffers;
  if (c.tables.mapped == nullptr) return c.tables.address;
  ENGINE_ASSERT(slot < k_rt_cache_table_copies, "more frames in flight than the table's copies");
  const u32 copy = slot % k_rt_cache_table_copies;
  write_rt_cache_tables(copy);
  return c.tables.address + u64{copy} * sizeof(gfx::ClusterCacheTables);
}

void GpuScene::write_rt_cache_tables(u32 copy) noexcept {
  const RtCacheBuffers& c = rt_cache_buffers;
  if (c.tables.mapped == nullptr) return;
  gfx::ClusterCacheTables tables;
  tables.bases = c.bases.address;
  tables.keys = c.keys.address;
  tables.owners = c.owners.address;
  tables.stamps = c.stamps.address;
  tables.free_list = c.free_list.address;
  tables.misses = c.misses.address;
  tables.miss_records = c.miss_records.address;
  tables.miss_destinations = c.miss_destinations.address;
  tables.references = c.references.address;
  tables.reference_keys = c.reference_keys.address;
  tables.scratch = c.scratch.address;
  tables.args = c.args.address;
  const u64 alignment = rt_cache_alignment != 0 ? rt_cache_alignment : 1;
  tables.slot_base = (c.slots.address + alignment - 1) / alignment * alignment;
  tables.slot_stride = static_cast<u32>(rt_cache_stride_);
  tables.slot_count = rt_cache_slots_ != 0 ? rt_cache_slots_ : rt_cache_limits.max_clusters;
  tables.mesh_count = rt_cache_base_.size();
  tables.reference_capacity = c.references.address != 0 ? rt_reference_capacity_ : 0u;
  for (u32 i = 0; i < k_rt_cache_table_copies; ++i) {
    if (copy < k_rt_cache_table_copies && i != copy) continue;
    std::memcpy(static_cast<u8*>(c.tables.mapped) + u64{i} * sizeof(tables), &tables,
                sizeof(tables));
  }
}

void GpuScene::destroy_rt_cache() noexcept {
  if (device_ == nullptr) return;
  RtCacheBuffers& c = rt_cache_buffers;
  for (gfx::BufferResource* b : {&c.tables, &c.bases, &c.keys, &c.owners, &c.stamps, &c.free_list,
                                 &c.misses, &c.miss_records, &c.miss_destinations, &c.scratch,
                                 &c.args, &c.slots, &c.references, &c.reference_keys})
    gfx::destroy_buffer(*device_, *b);
  rt_cache_base_.clear();
  rt_cache_slots_ = rt_cache_meshes_ = rt_cache_clusters_ = rt_cache_instances_ = 0;
  rt_cache_pairs_ = 0;
  rt_cache_bytes_ = rt_cache_stride_ = rt_cache_scratch_bytes_ = 0;
  rt_cache_limits = gfx::ClusterSetLimits{};
  rt_cache_alignment = 0;
  rt_cache_ceiling_mib_ = 0;
  rt_reference_capacity_ = rt_reference_limit_ = 0;
  rt_bytes_per_reference_ = 0;
}

bool GpuScene::rt_instance_cached(const gfx::InstanceDesc& instance) const noexcept {
  if (rt_cache_slots_ == 0 || instance.deform != gfx::k_invalid_deform) return false;
  if ((instance.flags & (gfx::k_instance_static_rt | gfx::k_instance_untraced)) != 0) return false;
  return instance.mesh < rt_cache_base_.size() && rt_cache_base_[instance.mesh] != ~0u;
}

// **Whether a frame needs the cache at all** (`rt_cache_instances()`; R106): the instances it would
// serve, over the instance table, whenever the table's deform words or flags change — at the
// cache's creation and with `policy_counts`, which every change of the instances runs — and never
// per frame. A dynamic scene's holes name the null mesh, past `rt_cache_base_`, and count as none.
void GpuScene::count_rt_cache_instances() noexcept {
  rt_cache_instances_ = 0;
  rt_cache_pairs_ = 0;
  if (rt_cache_slots_ == 0) return;
  for (const gfx::InstanceDesc& instance : instance_table_) {
    if (!rt_instance_cached(instance)) continue;
    ++rt_cache_instances_;
    rt_cache_pairs_ += data_->parts[instance.mesh].cluster_count;
  }
}

// The cached instances' pairs in every view, from the count the instance table keeps as it changes
// (`policy_count`, W81): until 2026-10-31 a sum over the whole table, which every change of a
// streamed world's tail paid through the chain's statistics — 2.8 ms a commit on the forest's
// 180,000 slots (docs/experiments/forest-load-in-2026-10-31.md).
u64 GpuScene::rt_reference_estimate() const noexcept {
  if (rt_cache_slots_ == 0) return 0;
  return rt_cache_pairs_ * view_count_;
}

u64 GpuScene::rt_frame_estimate() const noexcept {
  // Untraced pairs are built only where the structures are the picture (R110): on the ray path
  // they are drawn entries like any other, and count; with a rasterized picture they never reach
  // the records. `untraced_pairs_live_` holds no static form's, which the static pairs count.
  const u64 untraced = rt_untraced_drawn_ ? 0u : untraced_pairs_live_;
  const u64 excluded = static_pairs_live_ + untraced + rt_reference_estimate() / view_count_;
  const u64 pairs = u64{pair_stride_} > excluded ? pair_stride_ - excluded : 0;
  return pairs * view_count_;
}

// A slot about to be written or dropped stops counting as a static form's instance
// (static_instances()).
void GpuScene::uncount_static(u32 slot) noexcept {
  if (!static_forms_ || slot >= instance_table_.size()) return;
  const gfx::InstanceDesc& desc = instance_table_[slot];
  if ((desc.flags & gfx::k_instance_static_rt) == 0) return;
  --static_instances_live_;
  static_pairs_live_ -= data_->parts[desc.mesh].cluster_count;
}

// The prefix, then every live block by its ring's rank and its key (`rt_drop_order`); a block's
// slack and the holes are null instances, which want nothing and are left out.
void GpuScene::lay_out_rt_order() {
  rt_order_blocks_.clear();
  for (const TileBlock& block : layout_.blocks()) {
    if (block.instances == 0) continue;
    RtDropBlock drop;
    drop.key = block.key;
    drop.rank = block.inst_begin < slot_rank_.size() ? slot_rank_[block.inst_begin] : 1u;
    drop.first_slot = block.inst_begin;
    drop.slots = block.instances;
    rt_order_blocks_.push_back(drop);
  }
  rt_drop_order(static_instances_,
                std::span<RtDropBlock>(rt_order_blocks_.data(), rt_order_blocks_.size()),
                rt_order_);
  ++rt_order_version_;
}

u64 GpuScene::rt_drop_order_address(u32 slot, std::string* error) {
  if (!ray_tracing_ || !dynamic_ || rt_order_.empty()) return 0;
  if (rt_order_buffers_.size() <= slot) {
    rt_order_buffers_.resize(slot + 1);
    rt_order_written_.resize(slot + 1, ~u64{0});
  }
  gfx::BufferResource& buffer = rt_order_buffers_[slot];
  if (rt_order_written_[slot] == rt_order_version_) return buffer.address;
  const u64 bytes = u64{rt_order_.size()} * sizeof(u32);
  if (buffer.size < bytes) {
    // This slot's own copy: its previous frame has finished, and no other frame reads it.
    gfx::destroy_buffer(*device_, buffer);
    if (!gfx::create_buffer(*device_, bytes + bytes / 2, k_address, true, buffer, error)) return 0;
  }
  std::memcpy(buffer.mapped, rt_order_.data(), bytes);
  rt_order_written_[slot] = rt_order_version_;
  return buffer.address;
}

void GpuScene::destroy_ray_tracing_layout() noexcept {
  if (device_ == nullptr) return;
  const gfx::Device& device = *device_;
  if (tlas_slot_ != gfx::BindlessSet::k_invalid_slot) {
    // Only ever with no frame in flight (create, destroy, `relayout`), so the slot is free at once.
    bindless_.release_acceleration_structure(tlas_slot_, 0);
    tlas_slot_ = gfx::BindlessSet::k_invalid_slot;
  }
  gfx::destroy_acceleration_structure(device, tlas);
  gfx::destroy_buffer(device, rt_instances);
  gfx::destroy_buffer(device, slots);
  gfx::destroy_buffer(device, instance_counts);
  gfx::destroy_buffer(device, blas_records);
  gfx::destroy_buffer(device, blas_dense);
  gfx::destroy_buffer(device, blas_slots);
  gfx::destroy_buffer(device, records);
  gfx::destroy_buffer(device, hit_entries);
  gfx::destroy_cluster_set(device, clas_set);
  destroy_rt_chunks();
  gfx::destroy_cluster_blas_set(device, blas_set);
  gfx::destroy_buffer(device, rt_scratch);
}

// The chain's half that is laid out by the instances and the pairs. For a scene read whole that is
// its instances and its pairs, as it always was. For one whose instances come and go it is the
// instance capacity and the stride (renderer.md, "Instances that come and go"): every slot has a
// top-level record from the start — a hole's inactive — and a tail's change copies its own slots'
// records with their instances (`prepare_tables`), so the chain is rewritten only when the tail
// outgrows the capacity or the stride (`relayout`).
bool GpuScene::lay_out_ray_tracing(std::string* error) {
  const gfx::Device& device = *device_;
  const u32 keep_capacity = rt_capacity_;
  const u32 keep_references = rt_reference_capacity_;
  destroy_ray_tracing_layout();
  rt_instance_slots_ = dynamic_ ? std::max(instance_capacity_, instance_count_) : instance_count_;
  rt_union_clusters_ = pair_stride_ * view_count_;
  gfx::ClusterSetLimits limits = rt_limits_;
  limits.max_clusters = rt_union_clusters_;
  // The emit pass names a dense record and writes its original visible entry to hit_entries.
  // Static forms produce no record, so they consume none of this index space; untraced instances
  // produce none in a frame whose picture is rasterized, and their drawn entries' on the ray path
  // (R110), where `rt_max_geometry_index` counts them (`PairAccount::frame_pairs(true)`).
  gfx::ClusterAsProperties cluster_props;
  if (!gfx::cluster_as_properties(device, cluster_props)) return false;
  const u32 device_max = cluster_props.max_geometry_index;
  limits.max_geometry_index = device_max;
  rt_limits_ = limits;
  // One instance's structure can reference at most its mesh's clusters in every view (a ring
  // slot's: the clusters it has room for). A tail may place any of the scene's meshes.
  rt_max_per_instance_ = 0;
  if (dynamic_) {
    for (const geometry::ClusterMeshPart& part : data_->parts)
      rt_max_per_instance_ = std::max(rt_max_per_instance_, part.cluster_count * view_count_);
    for (const geometry::ClusterMeshPart& part : slot_parts_)
      rt_max_per_instance_ = std::max(rt_max_per_instance_, part.cluster_count * view_count_);
    // Or a mesh that arrives with a tile, whatever is resident now: the most one may have.
    rt_max_per_instance_ = std::max(rt_max_per_instance_, mesh_room_.max_clusters * view_count_);
  } else {
    for (const gfx::InstanceDesc& instance : instance_table_) {
      rt_max_per_instance_ =
          std::max(rt_max_per_instance_, part_of(instance.mesh).cluster_count * view_count_);
    }
  }

  constexpr gfx::BufferUsage k_record_usage = k_address | gfx::BufferUsage::AccelerationBuildInput;
  // The top-level instance records, once: the instance as the custom index, and a bottom-level
  // address of zero and a transform, both of which the frame's `tlas_references` dispatch
  // overwrites — the address from `blas_set.addresses`, the transform from the instance's cell and
  // the frame's eye (ADR-0053) — before every top-level build. An instance whose structure a frame
  // did not build keeps a zero address, which the top-level build treats as an inactive instance.
  // The transform written here is the one at the world's origin, which no frame reads.
  Vector<gfx::TlasInstance> tlas_records;
  tlas_records.reserve(rt_instance_slots_);
  for (u32 i = 0; i < rt_instance_slots_; ++i)
    tlas_records.push_back(tlas_record(i));
  Vector<u8> tlas_bytes(u64{rt_instance_slots_} * gfx::k_instance_record_bytes, u8{0});
  gfx::write_instances(std::span<const gfx::TlasInstance>(tlas_records.data(), tlas_records.size()),
                       tlas_bytes.data());
  bool ok = gfx::create_buffer(device, u64{rt_union_clusters_} * sizeof(u32), k_address, false,
                               slots, error) &&
            // Three words an instance: its drawn clusters, its casters, then its dense record base;
            // then the ranges' scratch, eight more words an instance and eight a block of 1,024,
            // which a frame past its capacity lays the drop order out in and dispatches the passes
            // after the walk from (R106; `gfx::ClusterRecordParams::instance_counts`).
            gfx::create_buffer(
                device, gfx::cluster_instance_count_words(rt_instance_slots_) * sizeof(u32),
                k_address | gfx::BufferUsage::TransferDst | gfx::BufferUsage::Indirect, false,
                instance_counts, error) &&
            // A dynamic scene's set is built over every slot of the capacity, so the records past
            // the frame's instances are cleared to empty structures every frame.
            // A transfer source too, so a test can read the drop rule's result back (R128).
            gfx::create_buffer(
                device, gfx::k_cluster_blas_record_bytes * rt_instance_slots_,
                k_record_usage | gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc,
                false, blas_records, error) &&
            // The records of the instances that reference anything, which the bottom-level build
            // reads, and every instance's place among them (R143, `blas_compact_main`): 20 bytes a
            // slot, so that the build costs the instances a frame draws and not the slots.
            gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * rt_instance_slots_,
                               k_record_usage, false, blas_dense, error) &&
            gfx::create_buffer(device, sizeof(u32) * u64{rt_instance_slots_}, k_address, false,
                               blas_slots, error) &&
            gfx::create_tlas(device, rt_instance_slots_, gfx::k_build_fast_trace, tlas, error) &&
            gfx::upload_buffer(device, tlas_bytes.data(), tlas_bytes.size(),
                               gfx::k_build_input_usage | gfx::BufferUsage::TransferDst | k_storage,
                               rt_instances, error);
  if (ok) {
    tlas_slot_ = bindless_.add_acceleration_structure(tlas.handle);
    if (tlas_slot_ == gfx::BindlessSet::k_invalid_slot) {
      ok = false;
      if (error != nullptr) *error = "no bindless slot for the top-level structure";
    }
  }
  if (!ok) return false;
  rt_scene_bytes_ = indices8.size + record_count.size + slots.size + instance_counts.size +
                    blas_records.size + blas_dense.size + blas_slots.size + tlas.buffer.size +
                    rt_instances.size + clas_templates.data.size + clas_templates.addresses.size +
                    clas_templates.sizes.size + static_bytes_ + rt_cache_bytes_;

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
      !gfx::cluster_blas_set_build_sizes(device, rt_instance_slots_, k_probe,
                                         std::min(rt_max_per_instance_, k_probe), blas_sizes,
                                         limits.opacity_micromaps)) {
    if (error != nullptr) {
      *error = "the driver gave no cluster acceleration structure sizes for " +
               std::to_string(k_probe) + " clusters over " + std::to_string(rt_instance_slots_) +
               " bottom-level structures of at most " +
               std::to_string(std::min(rt_max_per_instance_, k_probe)) + " each";
    }
    return false;
  }
  const u64 record_bytes = limits.instantiate ? gfx::k_cluster_instantiate_record_bytes
                                              : gfx::k_cluster_build_record_bytes;
  // Per-instance storage is scene-sized. Charging its complete cost at a 65,536-cluster probe
  // as a per-cluster slope made grass slots lower the budget even though grass builds nothing.
  // Two driver queries at the same instance count cancel that fixed cost.
  const auto bytes = [&](const gfx::ClusterBuildSizes& set, const gfx::ClusterBuildSizes& blas,
                         u32 count) {
    return set.data_bytes + blas.data_bytes +
           std::max({set.scratch_bytes, blas.scratch_bytes, tlas.build_scratch_bytes}) +
           u64{count} * (record_bytes + sizeof(u64) + 2 * sizeof(u32));
  };
  const u64 probe_bytes = bytes(set_sizes, blas_sizes, k_probe);
  const gfx::ClusterBuildSizes blas_probe = blas_sizes;
  probe.max_clusters = 2 * k_probe;
  if (!gfx::cluster_set_build_sizes(device, probe, set_sizes) ||
      !gfx::cluster_blas_set_build_sizes(device, rt_instance_slots_, 2 * k_probe,
                                         std::min(rt_max_per_instance_, k_probe), blas_sizes,
                                         limits.opacity_micromaps)) {
    if (error != nullptr) *error = "the driver gave no sizes for the second frame-budget probe";
    return false;
  }
  const u64 twice_bytes = bytes(set_sizes, blas_sizes, 2 * k_probe);
  rt_bytes_per_cluster_ =
      twice_bytes > probe_bytes ? (twice_bytes - probe_bytes + k_probe - 1) / k_probe : 0;
  if (rt_bytes_per_cluster_ == 0) {
    if (error != nullptr) *error = "the driver gave no marginal frame storage cost";
    return false;
  }
  // **The budget follows the device unless somebody named one** (renderer.md, "The budget follows
  // the device"; R107): the setting, else the tunable, else a sixteenth of the device-local heaps,
  // 512 to 4,096 MiB. The heaps' size, not VK_EXT_memory_budget's share of it, so a card gives the
  // same capacity whatever else is running on it.
  // **And it follows the micromaps** (R120, batch 53; rt_capacity.h): a scene that allows them pays
  // more for every cluster slot, so the device's rule is scaled by the slope with the micromap flag
  // over the slope without it — the same two probes again, without the flag — and the chain holds
  // as many clusters with micromaps as it would without. A named budget is left as named.
  u64 plain_bytes_per_cluster = rt_bytes_per_cluster_;
  if (limits.opacity_micromaps) {
    gfx::ClusterSetLimits plain = limits;
    plain.opacity_micromaps = false;
    gfx::ClusterBuildSizes plain_set[2];
    gfx::ClusterBuildSizes plain_blas[2];
    bool sized = true;
    for (u32 i = 0; i < 2 && sized; ++i) {
      const u32 count = k_probe * (i + 1);
      plain.max_clusters = count;
      sized = gfx::cluster_set_build_sizes(device, plain, plain_set[i]) &&
              gfx::cluster_blas_set_build_sizes(device, rt_instance_slots_, count,
                                                std::min(rt_max_per_instance_, k_probe),
                                                plain_blas[i], false);
    }
    const u64 once = sized ? bytes(plain_set[0], plain_blas[0], k_probe) : 0;
    const u64 twice = sized ? bytes(plain_set[1], plain_blas[1], 2 * k_probe) : 0;
    // A driver that gives no plain sizes leaves the budget unscaled, which is the old rule.
    plain_bytes_per_cluster = twice > once ? (twice - once + k_probe - 1) / k_probe : 0;
  }
  rt_bytes_per_cluster_plain_ = plain_bytes_per_cluster;
  gfx::MemoryBudget memory;
  (void)device.memory_budget(memory);
  const RtBudget budget =
      resolve_rt_budget(resolved_.settings.rt_budget_mib, rt_budget_mib_tunable(),
                        memory.device_local_bytes, rt_bytes_per_cluster_, plain_bytes_per_cluster);
  const u32 budget_mib = budget.mib;
  rt_budget_mib_ = budget.mib;
  rt_budget_source_ = budget.source;
  const u64 budget_clusters = u64{budget_mib} * 1024 * 1024 / rt_bytes_per_cluster_;
  // With a cache the frame's own records are named under bit 23 (gfx::k_cluster_cache_geometry),
  // which a cached structure's name sets: 8,388,608 of them, past any budget's.
  const u64 index_room = rt_cache() ? u64{gfx::k_cluster_cache_geometry} : u64{device_max} + 1;
  rt_capacity_limit_ = static_cast<u32>(
      std::max<u64>(1, std::min({budget_clusters, u64{rt_union_clusters_}, index_room})));
  // **The references to the cache** (renderer.md, "The resident cluster structure cache"): eight
  // bytes of address, four of key, sixteen of the misses' list (R114: as long as the references),
  // and the bottom-level set's growth per reference, by the same two driver probes;
  // `renderer.rt.reference_mib` is their ceiling, as the budget is the capacity's.
  rt_reference_limit_ = 0;
  rt_bytes_per_reference_ = 0;
  if (rt_cache()) {
    const u64 before = blas_probe.data_bytes + blas_probe.scratch_bytes;
    const u64 after = blas_sizes.data_bytes + blas_sizes.scratch_bytes;
    const u64 blas_growth = after > before ? after - before : 0;
    rt_bytes_per_reference_ = (blas_growth + k_probe - 1) / k_probe + sizeof(u64) + sizeof(u32) +
                              gfx::k_cluster_cache_miss_bytes;
    rt_reference_limit_ = static_cast<u32>(std::max<u64>(
        1, std::min(u64{rt_reference_mib_tunable()} * 1024 * 1024 / rt_bytes_per_reference_,
                    u64{rt_union_clusters_})));
  }
  // The first capacity, before any frame has said what it builds: the whole union when that fits
  // (which is every scene the tests draw, and so every picture they pin), `k_initial_rt_clusters`
  // otherwise. The renderer grows or shrinks it from the frames' own demand; a relayout keeps the
  // capacity the frames had grown it to.
  // Since 2026-10-25 from what the scene knows (`first_rt_capacity`): every pair a frame could
  // build, less the static forms', in every view, clamped to the budget and the union, at least
  // `renderer.rt.initial_clusters` where the union is larger — so a scene past the budget starts at
  // it rather than growing over its first frames, and one inside it can never drop anything.
  const u32 first = keep_capacity != 0
                        ? keep_capacity
                        : first_rt_capacity(rt_capacity_limit_, rt_union_clusters_,
                                            rt_frame_estimate(), rt_initial_clusters_tunable());
  u32 first_references = keep_references;
  if (rt_cache() && first_references == 0) {
    first_references = first_rt_capacity(rt_reference_limit_, rt_union_clusters_,
                                         rt_reference_estimate(), rt_initial_clusters_tunable());
  }
  rt_reference_capacity_ = 0;
  if (!resize_ray_tracing(first, error, false, first_references)) return false;
  ENGINE_LOG_INFO(
      log_renderer, "ray tracing ready", log::field("pairs", pair_count_),
      log::field("stride", pair_stride_), log::field("views", view_count_),
      log::field("union_clusters", rt_union_clusters_), log::field("instances", instance_count_),
      log::field("instance_slots", rt_instance_slots_), log::field("capacity", rt_capacity_),
      log::field("capacity_limit", rt_capacity_limit_), log::field("budget_mib", budget_mib),
      log::field("budget_source", rt_budget_source_name(budget.source)),
      log::field("device_local_mib", memory.device_local_bytes >> 20),
      log::field("bytes_per_cluster", rt_bytes_per_cluster_),
      log::field("bytes_per_cluster_plain", plain_bytes_per_cluster),
      log::field("clas_bytes", rt_chunk_bytes()), log::field("clas_chunks", rt_chunk_count_),
      log::field("blas_bytes", blas_set.data.size), log::field("scratch_bytes", rt_scratch.size),
      log::field("rt_bytes", rt_bytes_),
      log::field("templates", limits.instantiate ? cluster_count_ : 0u),
      log::field("template_bytes", template_bytes_),
      log::field("template_storage", clas_templates.data.size));
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

// The shore's records (water.md, "The shore"; roadmap S97, S94): one a water body with a depth map
// and a `wet_m` or a swash, its instance, its water material, its swash — the wind's wave run up
// the map's foreshore slope by Stockdon et al., the period rounded onto the water's clock, the
// tide — and its bounds: the map's square, body-local, grown by the distance a 1:25 bank rises the
// larger of `wet_m` and the highest the swash and the tide reach, and a metre, for the resolve's
// water modules to find the terrain pixels just above its level by. A scene with none uploads
// nothing and the resolve reads nothing.
bool GpuScene::upload_water_shores(std::string* error) {
  if (water_shores_.size != 0) gfx::destroy_buffer(*device_, water_shores_);
  water_shores_ = gfx::BufferResource{};
  water_shore_count_ = 0;
  Vector<gfx::WaterShore> shores;
  Vector<gfx::WaterShore> banks;
  u32 bank_reference = ~0u;
  for (u32 i = 0; i < data_->instances.size(); ++i) {
    const u32 m = data_->instances[i].mesh;
    if (m >= data_->mesh_info.size() || !data_->mesh_info[m].water.has_value()) continue;
    const SceneWater& water = *data_->mesh_info[m].water;
    // A channel's banks (S99): its stretch records, moved into the first channel's instance's
    // frame (the tree has one), written after every body's as one tree.
    if (water.channel) {
      if (bank_reference == ~0u) bank_reference = i;
      const gfx::InstanceDesc& here = data_->instances[i];
      const gfx::InstanceDesc& ref = data_->instances[bank_reference];
      // Where each stands: its cell on the 64 m grid plus its translation column, in f64.
      const auto offset = [&](u32 r, i32 a, i32 b) {
        return static_cast<f64>(a - b) * k_world_cell_m +
               (static_cast<f64>(here.rows[r].w) - static_cast<f64>(ref.rows[r].w));
      };
      const f64 ox = offset(0, here.cell.x, ref.cell.x);
      const f64 oy = offset(1, here.cell.y, ref.cell.y);
      const f64 oz = offset(2, here.cell.z, ref.cell.z);
      const f32 dx = static_cast<f32>(ox), dy = static_cast<f32>(oy), dz = static_cast<f32>(oz);
      for (gfx::WaterShore bank : water.banks) {
        bank.stretch_a = Vec4{bank.stretch_a.x + dx, bank.stretch_a.y + dz, bank.stretch_a.z + dy,
                              bank.stretch_a.w};
        bank.stretch_b = Vec4{bank.stretch_b.x + dx, bank.stretch_b.y + dz, bank.stretch_b.z + dy,
                              bank.stretch_b.w};
        bank.lo = Vec2{bank.lo.x + dx, bank.lo.y + dz};
        bank.hi = Vec2{bank.hi.x + dx, bank.hi.y + dz};
        banks.push_back(bank);
      }
      continue;
    }
    if (water.depth.size < 2) continue;
    const scene::WaterBody& body = water.body;
    const WaterSwashNumbers numbers = water_swash_numbers(water);
    const f64 wind = numbers.wind;
    const f64 fetch_m = numbers.fetch_m;
    const f64 slope = numbers.slope;
    const gfx::SwashWave wave = numbers.wave;
    const f64 run_up = numbers.run_up;
    const f32 tide = numbers.tide;
    if (!(body.wet_m > 0.0f) && !(run_up > 0.0) && !(tide > 0.0f)) continue;
    // A lifted body's run-up is each stretch of shore's own (S102): the record carries the most
    // any stretch can have, for the bounds and the ground's band; a pixel finds its own.
    const bool lifted = i < water_surface_of_.size() && water_surface_of_[i] != ~0u;
    f64 highest = run_up;
    if (lifted && run_up > 0.0)
      highest = std::max(run_up, gfx::swash_highest_run_up(wave.height_m, wave.period_s,
                                                           static_cast<f64>(water.depth.bank_max)));
    const f32 reach = std::max(body.wet_m, static_cast<f32>(highest) * 1.25f + tide);
    const f32 margin = 25.0f * reach + 1.0f;
    const f32 half = 0.5f * water.depth.texel_m;
    const f32 span = static_cast<f32>(water.depth.size - 1) * water.depth.texel_m;
    gfx::WaterShore shore;
    shore.instance = i;
    shore.wet_m = body.wet_m;
    shore.terrain_mesh = data_->terrain.enabled ? data_->terrain_mesh : 0xffffffffu;
    shore.material = m < mesh_material_base_.size() ? mesh_material_base_[m] : 0xffffffffu;
    shore.run_up_m = static_cast<f32>(highest);
    shore.period_s = static_cast<f32>(gfx::swash_period(wave.period_s));
    shore.drain_s = body.drain_s;
    shore.slope = static_cast<f32>(slope);
    shore.tide_m = tide;
    shore.height_m = static_cast<f32>(wave.height_m);
    shore.foam_m = body.foam_m;
    shore.shore_m = body.shore_m;
    if (lifted)
      shore.surface =
          water_surfaces_.address + u64{water_surface_of_[i]} * sizeof(gfx::WaterSurface);
    ENGINE_LOG_INFO(log_renderer, "water swash", log::field("body", body.name),
                    log::field("wind_m_s", wind), log::field("fetch_m", fetch_m),
                    log::field("height_m", wave.height_m), log::field("period_s", wave.period_s),
                    log::field("slope", slope), log::field("run_up_m", run_up),
                    log::field("tide_m", tide));
    shore.lo = Vec2{water.depth.origin.x - half - margin, water.depth.origin.y - half - margin};
    shore.hi = Vec2{water.depth.origin.x + span + half + margin,
                    water.depth.origin.y + span + half + margin};
    shores.push_back(shore);
  }
  // The channels' banks as one tree after the bodies (gfx::build_bank_tree), so a ground pixel
  // tests about log2 of the stretches rather than every one.
  if (!banks.empty()) {
    gfx::build_bank_tree(banks, bank_reference,
                         data_->terrain.enabled ? data_->terrain_mesh : 0xffffffffu, shores);
    ENGINE_LOG_INFO(log_renderer, "water banks", log::field("stretches", banks.size()),
                    log::field("records", shores.size()));
  }
  if (shores.empty()) return true;
  if (!gfx::upload_buffer(*device_, shores.data(), u64{shores.size()} * sizeof(gfx::WaterShore),
                          k_storage, water_shores_, error))
    return false;
  water_shore_count_ = shores.size();
  return true;
}

// ---- the water's reflection (R129; water_reflect.h)
// ----------------------------------------------
//
// The level bodies a frame's mirrored caster volume may be fit to: every water body's instance but
// a channel's, whose surface slopes along its flow and has no one plane. A body's square is its
// depth map's (body-local), which the nearest-body rule measures the eye's distance to; a body with
// no map is measured to its instance's origin. Read once at load, with the materials.
void GpuScene::collect_water_reflect_bodies() {
  water_reflect_bodies_.clear();
  for (u32 i = 0; i < data_->instances.size(); ++i) {
    const u32 m = data_->instances[i].mesh;
    if (m >= data_->mesh_info.size() || !data_->mesh_info[m].water.has_value()) continue;
    const SceneWater& water = *data_->mesh_info[m].water;
    if (water.channel) continue;
    WaterReflectBody body;
    body.instance = i;
    body.open = water.body.open;
    if (water.depth.size >= 2) {
      const f32 half = 0.5f * static_cast<f32>(water.depth.size - 1) * water.depth.texel_m;
      body.center = Vec2{water.depth.origin.x + half, water.depth.origin.y + half};
      body.radius = half * 1.41421356f;
    }
    water_reflect_bodies_.push_back(body);
  }
}

bool GpuScene::water_reflect_level(const WorldEye& eye, f32& level) const noexcept {
  bool found = false;
  f32 nearest = 0.0f;
  for (const WaterReflectBody& body : water_reflect_bodies_) {
    if (body.instance >= instance_table_.size()) continue;
    // The instance stands at the level (water.md, "The mesh"), its mesh flat at y = 0.
    const Vec3 at = relative(gfx::instance_cell(instance_table_[body.instance]), eye);
    const Vec3 center{at.x + body.center.x, at.y, at.z + body.center.y};
    const f32 away = length(center) - body.radius;
    const f32 distance = body.open ? -1.0f : (away > 0.0f ? away : 0.0f);
    if (!found || distance < nearest) {
      found = true;
      nearest = distance;
      level = at.y;
    }
  }
  return found;
}

// ---- what is under the water (S86; water_refract.h)
// ---------------------------------------------------
//
// Per level body of `water_reflect_bodies_`, at the volume tier: its deepest bed (its depth map's
// own deepest texel; none without a map) and the reach its optics leave light through (ln 255
// extinction lengths of the clearest channel, the extinction `gfx::set_water_optics` packs).
void GpuScene::collect_water_refract_bodies() {
  water_refract_bodies_.clear();
  if (resolved_.settings.water_quality != WaterQuality::High) return;
  for (const WaterReflectBody& reflect : water_reflect_bodies_) {
    const SceneWater& water = *data_->mesh_info[data_->instances[reflect.instance].mesh].water;
    gfx::WaterOptics o;
    if (water.body.absorption.has_value()) o.absorption = *water.body.absorption;
    if (water.body.scattering.has_value()) o.scattering = *water.body.scattering;
    o.turbidity = water.body.turbidity;
    const Vec3 scattering = o.scattering + gfx::k_water_silt_scatter * o.turbidity;
    const Vec3 extinction = o.absorption + gfx::k_water_silt_absorb * o.turbidity + scattering;
    WaterRefractBody body;
    body.deepest = water.depth.size >= 2 ? water.depth.depth_max : 0.0f;
    body.path_m = water_refract_path_m(extinction);
    water_refract_bodies_.push_back(body);
  }
}

bool GpuScene::water_refract_body(const WorldEye& eye, f32& level, f32& floor,
                                  f32& path_m) const noexcept {
  if (water_refract_bodies_.empty() ||
      water_refract_bodies_.size() != water_reflect_bodies_.size()) {
    return false;
  }
  // The body `water_reflect_level` picks, by the same rule.
  bool found = false;
  f32 nearest = 0.0f;
  for (u32 b = 0; b < water_reflect_bodies_.size(); ++b) {
    const WaterReflectBody& body = water_reflect_bodies_[b];
    if (body.instance >= instance_table_.size()) continue;
    const Vec3 at = relative(gfx::instance_cell(instance_table_[body.instance]), eye);
    const Vec3 center{at.x + body.center.x, at.y, at.z + body.center.y};
    const f32 away = length(center) - body.radius;
    const f32 distance = body.open ? -1.0f : (away > 0.0f ? away : 0.0f);
    if (!found || distance < nearest) {
      found = true;
      nearest = distance;
      level = at.y;
      const WaterRefractBody& refract = water_refract_bodies_[b];
      path_m = refract.path_m;
      const f32 bed =
          refract.deepest > 0.0f ? refract.deepest + k_water_refract_bed_margin_m : path_m;
      floor = level - (bed < path_m ? bed : path_m);
    }
  }
  return found;
}

void GpuScene::destroy() noexcept {
  ground_.reset();
  ground_time_s_ = 0.0;
  ground_previous_s_ = 0.0;
  terrain_uv_ = TerrainUvFrame{};
  if (device_ == nullptr) return;
  const gfx::Device& device = *device_;
  // A dynamic scene's `instances` and `pair_table` name one of its table sets, which own them.
  gfx::destroy_opacity_micromaps(device, micromaps_);
  if (water_shores_.size != 0) gfx::destroy_buffer(device, water_shores_);
  water_shores_ = gfx::BufferResource{};
  water_shore_count_ = 0;
  if (water_surfaces_.size != 0) gfx::destroy_buffer(device, water_surfaces_);
  water_surfaces_ = gfx::BufferResource{};
  for (gfx::BufferResource& field : water_fields_)
    gfx::destroy_buffer(device, field);
  water_fields_.clear();
  water_surface_of_.clear();
  water_deforms_.clear();
  micromap_list_.clear();
  micromap_triangles_.clear();
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
  pair_limit_ = k_max_pairs;
  relayout_pending_ = false;
  ring_pair_begin_ = 0;
  rt_instance_slots_ = 0;
  gfx::destroy_buffer(device, ring_pairs_);
  gfx::destroy_buffer(device, meshes);
  gfx::destroy_cluster_templates(device, clas_templates);
  for (gfx::BufferResource& buffer : rt_order_buffers_)
    gfx::destroy_buffer(device, buffer);
  rt_order_buffers_.clear();
  rt_order_written_.clear();
  rt_order_.clear();
  slot_rank_.clear();
  gfx::destroy_cluster_set(device, static_clas_);
  gfx::destroy_cluster_blas_set(device, static_blas_);
  destroy_rt_cache();
  static_forms_ = static_build_ = false;
  static_of_mesh_.clear();
  static_count_ = static_clusters_ = static_instances_live_ = 0;
  static_bytes_ = static_pairs_live_ = 0;
  gfx::destroy_acceleration_structure(device, tlas);
  gfx::destroy_cluster_blas_set(device, blas_set);
  gfx::destroy_cluster_set(device, clas_set);
  destroy_rt_chunks();
  gfx::destroy_buffer(device, clas_chunk_counts);
  gfx::destroy_buffer(device, rt_scratch);
  gfx::destroy_buffer(device, rt_instances);
  gfx::destroy_buffer(device, blas_records);
  gfx::destroy_buffer(device, blas_dense);
  gfx::destroy_buffer(device, blas_slots);
  gfx::destroy_buffer(device, instance_counts);
  gfx::destroy_buffer(device, slots);
  gfx::destroy_buffer(device, record_count);
  gfx::destroy_buffer(device, records);
  gfx::destroy_buffer(device, hit_entries);
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
  pending_instance_writes_.clear();
  for (gfx::BufferResource& staging : record_staging_)
    gfx::destroy_buffer(device, staging);
  record_staging_.clear();
  record_uploads_ = record_upload_bytes_ = 0;
  ring_slots_.clear();
  slot_parts_.clear();
  mesh_room_ = MeshRoom{};
  mesh_use_ = MeshUse{};
  mesh_slots_.clear();
  free_mesh_clusters_.clear();
  free_mesh_materials_.clear();
  mesh_descs_.clear();
  rings_ = nullptr;
  scene_vertex_count_ = scene_triangle_count_ = vertex_capacity_ = triangle_capacity_ = 0;
  terrain_ring_bytes_ = terrain_chunk_uploads_ = terrain_chunk_upload_bytes_ = 0;
  gfx::destroy_buffer(device, deform_normals);
  gfx::destroy_buffer(device, flora_motion_);
  gfx::destroy_buffer(device, flora_poses_);
  gfx::destroy_buffer(device, flora_field_parts_);
  flora_meshes_.clear();
  flora_bounds_.clear();
  flora_instances_.clear();
  flora_state_.clear();
  flora_poser_.reset();
  flora_parts_ = 0;
  flora_ground_.reset();
  flora_stats_ = FloraStats{};
  flora_moving_ = false;
  flora_wind_source_ = nullptr;
  flora_padding_.clear();
  tail_trees_.clear();
  tail_tree_free_.clear();
  tail_tree_of_slot_.clear();
  flora_tail_ = flora_recut_ = flora_grow_ = false;
  flora_frames_ = 0;
  load_deforms_ = load_flora_ = load_flora_parts_ = deform_capacity_ = flora_part_capacity_ = 0;
  gfx::destroy_buffer(device, static_cache);
  gfx::destroy_buffer(device, joints);
  gfx::destroy_buffer(device, deform_args);
  gfx::destroy_buffer(device, deform_table);
  gfx::destroy_buffer(device, deform_alloc);
  gfx::destroy_buffer(device, deform_blocks);
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
  gfx::destroy_buffer(device, lod_coverage);
  gfx::destroy_buffer(device, shadow_coverage);
  coverage_list_.clear();
  shadow_coverage_clusters_ = 0;
  gfx::destroy_buffer(device, terrain_masks);
  gfx::destroy_buffer(device, terrain_formation);
  gfx::destroy_buffer(device, fade);
  gfx::destroy_buffer(device, materials);
  gfx::destroy_buffer(device, skin);
  gfx::destroy_buffer(device, attributes);
  gfx::destroy_buffer(device, lods);
  gfx::destroy_buffer(device, triangles);
  gfx::destroy_buffer(device, vertices);
  gfx::destroy_buffer(device, instances);
  gfx::destroy_buffer(device, quantized);
  gfx::destroy_buffer(device, clusters);
  destroy_texture_room();
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
  normal_variance_textures_ = 0;
  normal_variance_bytes_ = 0;
  bindless_.destroy();
  instance_table_.clear();
  deform_descs_.clear();
  deform_instance_.clear();
  deform_mesh_clusters_.clear();
  morph_channel_count_ = static_cached_instances_ = 0;
  static_cache_bytes_ = 0;
  static_cache_dirty_ = false;
  static_cache_pending_.clear();
  static_cache_rebuild_.clear();
  morph_base_padding_.clear();
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
  rt_bytes_per_cluster_plain_ = 0;
  rt_budget_mib_ = 0;
  rt_budget_source_ = RtBudgetSource::Device;
  rt_capacity_ = rt_capacity_limit_ = rt_union_clusters_ = rt_max_per_instance_ = 0;
  rt_limits_ = gfx::ClusterSetLimits{};
  vertex_index_capacity_ = 0;
  tlas_slot_ = gfx::BindlessSet::k_invalid_slot;
  ray_tracing_ = deform_ = skinned_ = streamed_ = rt_untraced_drawn_ = false;
  flora_field_ = flora_tail_parts_ = false;
  field_deform_.clear();
  mesh_policy_.clear();
  page_count_ = page_slots_ = slot_vertices_ = slot_triangles_ = max_requests_ = 0;
  upload_budget_bytes_ = 0;
  page_budget_bytes_ = stream_bytes_ = geometry_bytes_ = 0;
}

bool GpuScene::place_instances(std::span<const u32> indices,
                               std::span<const SceneInstance> placements, std::string* error) {
  if (device_ == nullptr || data_ == nullptr) {
    if (error != nullptr) *error = "the scene is not created";
    return false;
  }
  if (indices.size() != placements.size()) {
    if (error != nullptr) *error = "place_instances takes one placement per index";
    return false;
  }
  // Everything is checked before anything is written, so a refusal moves nothing.
  for (usize i = 0; i < indices.size(); ++i) {
    const u32 index = indices[i];
    if (index >= instance_count_ || index >= instance_table_.size()) {
      if (error != nullptr) *error = "instance " + std::to_string(index) + " is past the scene's";
      return false;
    }
    // A dynamic scene moves its load's own instances, the prefix; a tail slot is whatever block
    // the layout put there, and is placed by the tail it came in (`set_dynamic_instances`).
    if (dynamic_ && index >= static_instances_) {
      if (error != nullptr) {
        *error = "instance " + std::to_string(index) +
                 " is in the dynamic tail, which set_dynamic_instances places";
      }
      return false;
    }
    if (placements[i].mesh != instance_table_[index].mesh ||
        placements[i].mesh >= data_->mesh_fit.size()) {
      if (error != nullptr)
        *error = "instance " + std::to_string(index) + " is placed as a mesh it is not";
      return false;
    }
    const WorldPos at = instance_translation(placements[i], data_->mesh_fit[placements[i].mesh]);
    if (!world_cell_valid(at)) {
      if (error != nullptr) *error = "instance " + std::to_string(index) + " is off the world grid";
      return false;
    }
  }
  for (usize i = 0; i < indices.size(); ++i) {
    gfx::InstanceDesc& entry = instance_table_[indices[i]];
    const Mat4& fit = data_->mesh_fit[placements[i].mesh];
    // **A move changes the placement and nothing else**: `set_instance_placement` writes the flags
    // from the scale alone, so every other bit — the species' caster policy, the static-structure
    // bit — is kept across it and only the uniform-scale bit is the new placement's. Until
    // 2026-10-31 a moved instance lost them (an untraced one became traced, a static form was built
    // per frame); found by R111, whose tile hand-off moves instances.
    const u32 kept = entry.flags & ~gfx::k_instance_uniform_scale;
    gfx::set_instance_placement(entry, mat4_from_transform(placements[i].transform) * fit,
                                instance_translation(placements[i], fit));
    entry.flags |= kept;
  }
  // **A move rides the next frame** (renderer.md, "Moving an instance"). In a dynamic scene the
  // slot is marked pending for every table set, as a tail block's slots are, so the next frame
  // flips to a set no frame in flight reads and copies the moved record into it, with the chain's
  // top-level record of the slot, before anything reads either; the frames in flight keep reading
  // theirs. In a scene read whole the record joins the next frame's record upload
  // (`prepare_records`): copied into the one table in that frame's command stream, after a barrier
  // that waits for every frame before it on the queue, and before any of its passes reads the
  // table. Either way nothing is written to the device here and nothing waits. Nothing else holds
  // the placement: the deformed-vertex pool is in mesh space, and `tlas_references` writes the
  // chain's transforms from the frame's instance table.
  for (const u32 index : indices)
    mark_instance_written(index);
  return true;
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
