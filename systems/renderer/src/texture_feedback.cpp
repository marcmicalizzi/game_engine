// Which level of each texture a frame's samples asked for (texture_feedback.h; texture.md, "Levels
// resident by what the frame samples"; roadmap R172).

#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <renderer_log.h>
#include <shaders/texture_feedback.spv.h>
#include <texture_feedback.h>

namespace engine::renderer {

namespace {

tunables::String feedback_path{
    "renderer.textures.feedback", "",
    "A file to write, per frame, which level of each texture the frame's samples asked for, and a "
    "closing line per texture (texture.md, \"Levels resident by what the frame samples\"). A "
    "measurement: empty, the default, makes nothing; set, the frame draws through the frame "
    "passes' "
    "linear image",
    1024};

constexpr u32 k_workgroup = 8;   // texture_feedback.slang's
constexpr f32 k_offset = 32.0f;  // the shader's code: (lambda + 32) * 256
constexpr f32 k_steps = 256.0f;
constexpr u32 k_never = 0xffffffffu;
constexpr u32 k_softer_words = 4;  // pixels 1, 2, 3+ levels softer; pixels counted

// Mirrors texture_feedback.slang's `TextureFeedbackParams`.
struct TextureFeedbackParams {
  Mat4 view_proj;
  u64 visibility = 0;
  u64 clusters = 0;
  u64 mesh = 0;
  u64 triangles = 0;
  u64 instances = 0;
  u64 attributes = 0;
  u64 materials = 0;
  u64 cluster_materials = 0;
  u64 pairs = 0;
  u64 out = 0;
  u64 tops = 0;
  u64 softer = 0;
  u32 width = 0;
  u32 height = 0;
  u32 slot_count = 0;
  u32 slot_base = 0;
  u32 stride = 1;
  u32 phase_x = 0;
  u32 phase_y = 0;
  u32 pad = 0;
  gfx::FrameEye eye;
};
static_assert(sizeof(TextureFeedbackParams) == 240, "texture_feedback.slang's block");

// The whole chain's finest level a code reads first: floor of the resident image's lambda plus the
// slot's resident top, never under 0; -1 where no pixel asked.
i32 chain_level(u32 code, u32 top) noexcept {
  if (code == k_never) return -1;
  const f32 lambda = static_cast<f32>(code) / k_steps - k_offset;
  return std::max(0, static_cast<i32>(std::floor(lambda)) + static_cast<i32>(top));
}

// The format is checked against its arguments where the compiler can (GCC and Clang refuse a
// non-literal format otherwise: the first leg's Linux run, 2026-11-16).
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void append(std::string& out, const char* format, ...) noexcept {
  char line[768];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (n > 0) out.append(line, static_cast<usize>(std::min(n, static_cast<int>(sizeof(line) - 1))));
}

}  // namespace

std::string TextureFeedback::path_wanted() { return feedback_path.get(); }

void TextureFeedback::phase(u64 frame, u32& x, u32& y) noexcept {
  // A 4x4 ordered-dither order: consecutive frames read pixels far apart in the tile, and sixteen
  // frames read each pixel once.
  static constexpr u8 k_order[16] = {0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12};
  const u32 at = k_order[frame % 16];
  x = at % k_stride;
  y = at / k_stride;
}

bool TextureFeedback::create_common(const gfx::Device& device, GpuScene& scene,
                                    gfx::ShaderLibrary& shaders, u32 frames_in_flight,
                                    u32 table_words, std::string* error) {
  device_ = &device;
  scene_ = &scene;
  table_words_ = table_words;
  shaders.add_embedded("texture_feedback", shaders::k_texture_feedback_spirv,
                       shaders::k_texture_feedback_spirv_size);
  const gfx::Shader* shader = shaders.get("texture_feedback", error);
  if (shader == nullptr) return false;
  const gfx::DescriptorSetLayoutHandle set_layout = scene.bindless().layout();
  const std::span<const gfx::DescriptorSetLayoutHandle> sets(&set_layout, 1);
  if (!gfx::create_compute_pipeline(device, shader->module, "texture_feedback_main", sets,
                                    sizeof(u64), pipeline_, error)) {
    return false;
  }
  const u64 table_bytes = u64{table_words} * sizeof(u32);
  if (!gfx::create_buffer(device, table_bytes,
                          gfx::BufferUsage::Storage | gfx::BufferUsage::TransferSrc |
                              gfx::BufferUsage::TransferDst | gfx::BufferUsage::ShaderDeviceAddress,
                          false, table_, error)) {
    return false;
  }
  readback_.resize(frames_in_flight);
  params_.resize(frames_in_flight);
  pending_.resize(frames_in_flight, u8{0});
  pending_frame_.resize(frames_in_flight, 0);
  pending_time_.resize(frames_in_flight, 0.0);
  for (u32 s = 0; s < frames_in_flight; ++s) {
    if (!gfx::create_buffer(device, table_bytes, gfx::BufferUsage::TransferDst, true, readback_[s],
                            error) ||
        !gfx::create_buffer(device, 2 * sizeof(TextureFeedbackParams) * k_max_views,
                            gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, true,
                            params_[s], error)) {
      return false;
    }
  }
  return true;
}

bool TextureFeedback::create(const gfx::Device& device, GpuScene& scene,
                             gfx::ShaderLibrary& shaders, u32 frames_in_flight,
                             const std::string& path, std::string* error) {
  path_ = path;
  slot_base_ = 0;
  slot_count_ = scene.bindless().capacity().sampled_images;
  if (!create_common(device, scene, shaders, frames_in_flight, 2 * slot_count_ + k_softer_words,
                     error)) {
    return false;
  }
  tops_.resize(frames_in_flight);
  for (u32 s = 0; s < frames_in_flight; ++s) {
    if (!gfx::create_buffer(device, u64{slot_count_} * sizeof(u32),
                            gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, true,
                            tops_[s], error)) {
      return false;
    }
  }
  // About 300 bytes a frame: a flight of 50,000 frames before the reserve is spent.
  lines_.reserve(16u << 20);
  refresh_textures();
  ENGINE_LOG_INFO(log_renderer, "texture feedback", log::field("path", path_),
                  log::field("textures", static_cast<u32>(textures_.size())),
                  log::field("device_bytes", device_bytes()));
  return true;
}

bool TextureFeedback::create_residency(const gfx::Device& device, GpuScene& scene,
                                       gfx::ShaderLibrary& shaders, u32 frames_in_flight,
                                       std::string* error) {
  residency_ = true;
  if (!scene.texture_residency_run(slot_base_, slot_count_) || slot_count_ == 0) {
    if (error != nullptr) *error = "texture residency feedback: no managed run";
    return false;
  }
  if (!create_common(device, scene, shaders, frames_in_flight, slot_count_, error)) return false;
  ENGINE_LOG_INFO(log_renderer, "texture residency feedback", log::field("slots", slot_count_),
                  log::field("device_bytes", device_bytes()));
  return true;
}

u64 TextureFeedback::device_bytes() const noexcept {
  u64 bytes = table_.size;
  for (const gfx::BufferResource& b : readback_)
    bytes += b.size;
  for (const gfx::BufferResource& b : params_)
    bytes += b.size;
  for (const gfx::BufferResource& b : tops_)
    bytes += b.size;
  return bytes;
}

void TextureFeedback::refresh_textures() {
  Vector<GpuScene::TextureRecord> records;
  scene_->texture_records(records);
  Vector<Texture> next;
  next.reserve(records.size());
  for (u32 i = 0; i < records.size(); ++i) {
    const GpuScene::TextureRecord& r = records[i];
    Texture t;
    // The one this record was before: the load's records keep their order and their index (a
    // promotion or a trim moves a record's slot, never its place), a tile's is found by its slot.
    i32 before = -1;
    if (!r.admitted) {
      if (i < textures_.size() && !textures_[i].record.admitted) before = static_cast<i32>(i);
    } else if (r.slot < by_slot_.size()) {
      before = by_slot_[r.slot];
    }
    if (before >= 0 && textures_[static_cast<u32>(before)].record.width == r.width &&
        textures_[static_cast<u32>(before)].record.levels == r.levels) {
      t = textures_[static_cast<u32>(before)];
    } else {
      for (f64& at : t.asked_at)
        at = -1.0e30;
      for (f64& at : t.sparse_asked_at)
        at = -1.0e30;
      for (i32& h : t.held)
        h = -1;
    }
    t.record = r;
    // Each level's share of the whole chain's device bytes by its 4x4 blocks (one for a level
    // under four texels a side): what a block format takes, the allocator's rounding spread pro
    // rata, and for a texture held from a top the resident levels' bytes scaled up to the chain.
    u64 blocks[k_max_levels] = {};
    u64 total = 0;
    u64 resident_blocks = 0;
    const u32 levels = std::min(r.levels, k_max_levels);
    for (u32 l = 0; l < levels; ++l) {
      const u32 w = std::max(1u, r.width >> l);
      const u32 h = std::max(1u, r.height >> l);
      blocks[l] = u64{(w + 3) / 4} * ((h + 3) / 4);
      total += blocks[l];
      if (l >= r.top) resident_blocks += blocks[l];
    }
    const u64 chain_bytes = resident_blocks == 0 ? 0 : r.bytes * total / resident_blocks;
    for (u32 l = 0; l < levels; ++l)
      t.level_bytes[l] = total == 0 ? 0 : chain_bytes * blocks[l] / total;
    next.push_back(t);
  }
  textures_ = std::move(next);
  by_slot_.assign(slot_count_, -1);
  for (u32 i = 0; i < textures_.size(); ++i) {
    if (textures_[i].record.slot < slot_count_)
      by_slot_[textures_[i].record.slot] = static_cast<i32>(i);
  }
}

u64 TextureFeedback::bytes_from(const Texture& t, i32 level) const noexcept {
  if (level < 0) return 0;
  u64 bytes = 0;
  for (u32 l = static_cast<u32>(level); l < std::min(t.record.levels, k_max_levels); ++l)
    bytes += t.level_bytes[l];
  return bytes;
}

void TextureFeedback::fold(u32 slot) {
  if (!pending_[slot]) return;
  pending_[slot] = false;
  const u32* codes = static_cast<const u32*>(readback_[slot].mapped);
  if (residency_) {
    scene_->texture_residency_fold(codes, slot_count_, pending_frame_[slot]);
    return;
  }
  const f64 now = pending_time_[slot];
  const u32* sparse = codes + slot_count_;
  const u32* softer = codes + 2 * slot_count_;
  // A tile's mesh may have brought or taken textures, or the residency moved one, since the list
  // was made.
  if (scene_->texture_records_serial() != records_serial_) {
    refresh_textures();
    records_serial_ = scene_->texture_records_serial();
  }
  u64 resident = 0;
  u64 asked_now = 0;  // the levels from this frame's finest down, over the textures it asked
  u32 asked_textures = 0;
  u32 changed = 0;  // textures whose finest asked level is not the last frame's
  u64 held[k_holds] = {};
  u64 brought[k_holds] = {};  // bytes each hold would have had to bring in for this frame
  u32 brought_textures[k_holds] = {};
  // The sparse pattern against every pixel: textures every pixel asked and it did not, textures
  // it saw coarser than every pixel did, and an 8 s hold kept by it: its bytes, and the textures
  // whose kept level is coarser than the every-pixel hold's (a trim it drove would release a level
  // a pixel asked within the hold).
  u32 sparse_asked = 0;
  u32 sparse_missed = 0;
  u32 sparse_coarser = 0;
  u64 sparse_held = 0;
  u32 sparse_hold_coarser = 0;
  const auto keep_of = [](const f64* asked_at, u32 levels, f64 now_s, f64 hold) {
    i32 keep = static_cast<i32>(levels) - 1;
    for (u32 l = 0; l < std::min(levels, k_max_levels); ++l) {
      if (now_s - asked_at[l] <= hold) {
        keep = static_cast<i32>(l);
        break;
      }
    }
    return keep;
  };
  for (Texture& t : textures_) {
    resident += t.record.bytes;
    const bool in = t.record.slot < slot_count_;
    const u32 top = t.record.top;
    const i32 last_level = static_cast<i32>(t.record.levels) - 1;
    const i32 level = std::min(chain_level(in ? codes[t.record.slot] : k_never, top), last_level);
    const i32 sparse_level =
        std::min(chain_level(in ? sparse[t.record.slot] : k_never, top), last_level);
    if (level != t.finest_last) ++changed;
    t.finest_last = level;
    if (level >= 0) {
      ++asked_textures;
      asked_now += bytes_from(t, level);
      if (t.finest_ever < 0 || level < t.finest_ever) t.finest_ever = level;
      for (u32 l = static_cast<u32>(level); l < k_max_levels; ++l)
        t.asked_at[l] = now;
    }
    if (sparse_level >= 0) {
      ++sparse_asked;
      if (t.sparse_ever < 0 || sparse_level < t.sparse_ever) t.sparse_ever = sparse_level;
      for (u32 l = static_cast<u32>(sparse_level); l < k_max_levels; ++l)
        t.sparse_asked_at[l] = now;
      if (level >= 0 && sparse_level > level) ++sparse_coarser;
    } else if (level >= 0) {
      ++sparse_missed;
    }
    for (u32 h = 0; h < k_holds; ++h) {
      // The finest level asked within the hold, which the hold keeps; the coarsest level is kept
      // whatever happens (a texture is never absent).
      const i32 keep = keep_of(t.asked_at, t.record.levels, now, k_hold_s[h]);
      if (t.held[h] >= 0 && keep < t.held[h]) {
        brought[h] += bytes_from(t, keep) - bytes_from(t, t.held[h]);
        ++brought_textures[h];
      }
      t.held[h] = keep;
      held[h] += bytes_from(t, keep);
    }
    const i32 sparse_keep = keep_of(t.sparse_asked_at, t.record.levels, now, k_hold_s[k_holds - 1]);
    sparse_held += bytes_from(t, sparse_keep);
    if (sparse_keep > t.held[k_holds - 1]) ++sparse_hold_coarser;
  }
  ++frames_;
  append(lines_,
         "{\"frame\":%llu,\"t\":%.4f,\"textures\":%u,\"asked\":%u,\"changed\":%u,"
         "\"resident\":%llu,\"asked_bytes\":%llu,"
         "\"held\":[%llu,%llu,%llu],\"brought\":[%llu,%llu,%llu],\"brought_textures\":[%u,%u,%u],"
         "\"sparse_asked\":%u,\"sparse_missed\":%u,\"sparse_coarser\":%u,\"sparse_held8\":%llu,"
         "\"sparse_hold_coarser\":%u,\"softer\":[%u,%u,%u],\"pixels\":%u}\n",
         static_cast<unsigned long long>(pending_frame_[slot]), now,
         static_cast<u32>(textures_.size()), asked_textures, changed,
         static_cast<unsigned long long>(resident), static_cast<unsigned long long>(asked_now),
         static_cast<unsigned long long>(held[0]), static_cast<unsigned long long>(held[1]),
         static_cast<unsigned long long>(held[2]), static_cast<unsigned long long>(brought[0]),
         static_cast<unsigned long long>(brought[1]), static_cast<unsigned long long>(brought[2]),
         brought_textures[0], brought_textures[1], brought_textures[2], sparse_asked, sparse_missed,
         sparse_coarser, static_cast<unsigned long long>(sparse_held), sparse_hold_coarser,
         softer[0], softer[1], softer[2], softer[3]);
}

void TextureFeedback::destroy() noexcept {
  if (device_ == nullptr) return;
  // The device is idle: every slot's copy has landed.
  for (u32 s = 0; s < pending_.size(); ++s) {
    fold(s);
  }
  if (!residency_) {
    u64 resident = 0;
    u64 ever = 0;
    u64 sparse_ever = 0;
    for (const Texture& t : textures_) {
      resident += t.record.bytes;
      ever += bytes_from(t, t.finest_ever);
      sparse_ever += bytes_from(t, t.sparse_ever);
      append(lines_,
             "{\"texture\":%u,\"format\":%u,\"width\":%u,\"height\":%u,\"levels\":%u,"
             "\"bytes\":%llu,\"top\":%u,\"admitted\":%s,\"finest_ever\":%d,\"ever_bytes\":%llu,"
             "\"sparse_ever\":%d}\n",
             t.record.slot, static_cast<u32>(t.record.format), t.record.width, t.record.height,
             t.record.levels, static_cast<unsigned long long>(t.record.bytes), t.record.top,
             t.record.admitted ? "true" : "false", t.finest_ever,
             static_cast<unsigned long long>(bytes_from(t, t.finest_ever)), t.sparse_ever);
    }
    append(
        lines_,
        "{\"summary\":true,\"frames\":%llu,\"textures\":%u,\"resident\":%llu,\"ever_bytes\":%llu,"
        "\"sparse_ever_bytes\":%llu,\"never_asked_share\":%.4f}\n",
        static_cast<unsigned long long>(frames_), static_cast<u32>(textures_.size()),
        static_cast<unsigned long long>(resident), static_cast<unsigned long long>(ever),
        static_cast<unsigned long long>(sparse_ever),
        resident == 0 ? 0.0 : 1.0 - static_cast<f64>(ever) / static_cast<f64>(resident));
    if (!path_.empty() && io::write_file(path_, lines_) != io::Status::Ok) {
      ENGINE_LOG_WARN(log_renderer, "texture feedback not written", log::field("path", path_));
    }
  }
  gfx::destroy_compute_pipeline(*device_, pipeline_);
  gfx::destroy_buffer(*device_, table_);
  for (gfx::BufferResource& b : readback_)
    gfx::destroy_buffer(*device_, b);
  for (gfx::BufferResource& b : params_)
    gfx::destroy_buffer(*device_, b);
  for (gfx::BufferResource& b : tops_)
    gfx::destroy_buffer(*device_, b);
  *this = TextureFeedback{};
}

bool TextureFeedback::record_thunk(const FramePassContext& context, void* user,
                                   std::string* error) {
  return static_cast<TextureFeedback*>(user)->record(context, error);
}

bool TextureFeedback::record(const FramePassContext& context, std::string*) {
  const u32 slot = context.slot;
  // This slot's last copy is done (the slot is recorded again only once its frame is): fold it
  // before the copy below overwrites it.
  fold(slot);
  // The frames' own clock: the host's step, or a 60 Hz frame where the host steps none (a
  // flythrough's fixed frames), so a hold in seconds means what it says at 60 frames a second.
  time_s_ += context.dt_s > 0.0 ? context.dt_s : 1.0 / 60.0;
  pending_[slot] = true;
  // The residency's fold reads which pair held which texture by its own count of recorded frames.
  pending_frame_[slot] = residency_ ? scene_->texture_residency_frame() : context.frame_index;
  pending_time_[slot] = time_s_;

  const GpuScene& scene = *scene_;
  u32 phase_x = 0;
  u32 phase_y = 0;
  phase(context.frame_index, phase_x, phase_y);
  // The measurement's tops: each slot's resident top, so a pixel's level is the whole chain's.
  u64 tops = 0;
  if (!residency_ && slot < tops_.size()) {
    auto* words = static_cast<u32*>(tops_[slot].mapped);
    std::memset(words, 0, u64{slot_count_} * sizeof(u32));
    for (const Texture& t : textures_)
      if (t.record.slot < slot_count_) words[t.record.slot] = t.record.top;
    tops = tops_[slot].address;
  }
  auto* blocks = static_cast<TextureFeedbackParams*>(params_[slot].mapped);
  view_count_ = std::min(context.view_count, k_max_views);
  for (u32 v = 0; v < view_count_; ++v) {
    const FramePassView& view = context.views[v];
    TextureFeedbackParams p{};
    p.view_proj = view.view_proj;
    p.visibility = view.visibility;
    p.clusters = scene.clusters.address;
    p.mesh = scene.meshes.address;
    p.triangles = scene.triangles.address;
    p.instances = scene.instances.address;
    p.attributes = scene.attributes.address;
    p.materials = scene.materials.address;
    p.cluster_materials = scene.cluster_materials.address;
    p.pairs = scene.pair_table.address;
    p.out = table_.address;
    p.width = view.source_width;
    p.height = view.source_height;
    p.slot_base = slot_base_;
    p.slot_count = slot_count_;
    p.eye = context.gpu_eye;
    if (residency_) {
      p.stride = k_stride;
      p.phase_x = phase_x;
      p.phase_y = phase_y;
      blocks[v] = p;
    } else {
      // Every pixel, with the softer count; then the sparse pattern into the second table.
      p.tops = tops;
      p.softer = table_.address + u64{2} * slot_count_ * sizeof(u32);
      blocks[2 * v] = p;
      p.tops = 0;
      p.softer = 0;
      p.out = table_.address + u64{slot_count_} * sizeof(u32);
      p.stride = k_stride;
      p.phase_x = phase_x;
      p.phase_y = phase_y;
      blocks[2 * v + 1] = p;
    }
    view_width_[v] = view.source_width;
    view_height_[v] = view.source_height;
  }
  // Without the scene's pair table a pixel's pair names no instance this pass can read: nothing is
  // dispatched, and the frame counts as asking for nothing.
  const bool dispatch = scene.pair_table.address != 0;
  gfx::RenderGraph& graph = *context.graph;
  // Last frame's copy read the table; this frame clears it first.
  const gfx::RgBuffer table =
      graph.import_buffer(residency_ ? "texture residency" : "texture feedback", table_,
                          gfx::PipelineStage::AllTransfer, gfx::MemoryAccess::TransferRead);
  const gfx::RgBuffer target = graph.import_buffer(
      residency_ ? "texture residency copy" : "texture feedback copy", readback_[slot]);
  const gfx::RgBuffer visibility = context.visibility;
  const gfx::BufferHandle table_buffer = table_.buffer;
  const gfx::BufferHandle target_buffer = readback_[slot].buffer;
  const u64 table_bytes = u64{table_words_} * sizeof(u32);
  const u64 codes_bytes = residency_ ? table_bytes : u64{2} * slot_count_ * sizeof(u32);
  graph.add_pass(
      residency_ ? "texture residency clear" : "texture feedback clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.write(table, gfx::Access::TransferWrite); },
      [table_buffer, table_bytes, codes_bytes](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.fill_buffer(table_buffer, 0, codes_bytes, k_never);
        if (table_bytes > codes_bytes)
          cb.fill_buffer(table_buffer, codes_bytes, table_bytes - codes_bytes, 0u);
      });
  if (dispatch) {
    graph.add_pass(
        residency_ ? "texture residency feedback" : "texture feedback", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.read(visibility, gfx::Access::ComputeRead);
          b.write(table, gfx::Access::ComputeReadWrite);
        },
        [this, slot, timer = context.timer, bindless = context.bindless](gfx::CommandList cb,
                                                                         gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, pipeline_.pipeline);
          bindless->bind(cb, gfx::BindPoint::Compute);
          timer->begin(cb, residency_ ? "texture residency" : "texture feedback");
          const u32 per_view = residency_ ? 1u : 2u;
          for (u32 v = 0; v < view_count_; ++v) {
            for (u32 k = 0; k < per_view; ++k) {
              const u32 stride = residency_ || k == 1 ? k_stride : 1u;
              const u64 block =
                  params_[slot].address + u64{per_view * v + k} * sizeof(TextureFeedbackParams);
              cb.push_constants(pipeline_.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                                &block);
              const u32 w = (view_width_[v] + stride - 1) / stride;
              const u32 h = (view_height_[v] + stride - 1) / stride;
              cb.dispatch((w + k_workgroup - 1) / k_workgroup, (h + k_workgroup - 1) / k_workgroup,
                          1);
            }
          }
          timer->end(cb);
        });
  }
  graph.add_pass(
      residency_ ? "texture residency readback" : "texture feedback readback",
      gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(table, gfx::Access::TransferRead);
        b.write(target, gfx::Access::TransferWrite);
      },
      [table_buffer, target_buffer, table_bytes](gfx::CommandList cb, gfx::RenderGraph&) {
        const gfx::BufferCopy copy{0, 0, table_bytes};
        cb.copy_buffer(table_buffer, target_buffer, copy);
      });
  return true;
}

}  // namespace engine::renderer
