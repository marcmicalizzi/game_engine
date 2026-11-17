// Which level of each texture a frame's samples asked for (texture_feedback.h; texture.md, "Levels
// resident by what the frame samples"; roadmap R172).

#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <renderer_log.h>
#include <shaders/texture_feedback.spv.h>
#include <texture_feedback.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace engine::renderer {

namespace {

tunables::String feedback_path{
    "renderer.textures.feedback", "",
    "A file to write, per frame, which level of each texture the frame's samples asked for, and a "
    "closing line per texture (texture.md, \"Levels resident by what the frame samples\"). A "
    "measurement: empty, the default, makes nothing; set, the frame draws through the frame passes' "
    "linear image",
    1024};

constexpr u32 k_workgroup = 8;  // texture_feedback.slang's
constexpr f32 k_offset = 32.0f;  // the shader's code: (lambda + 32) * 256
constexpr f32 k_steps = 256.0f;
constexpr u32 k_never = 0xffffffffu;

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
  u32 width = 0;
  u32 height = 0;
  u32 slot_count = 0;
  u32 pad = 0;
  gfx::FrameEye eye;
};
static_assert(sizeof(TextureFeedbackParams) == 208, "texture_feedback.slang's block");

// The finest level a code reads first: floor(lambda), never under 0.
i32 level_of(u32 code) noexcept {
  const f32 lambda = static_cast<f32>(code) / k_steps - k_offset;
  return lambda <= 0.0f ? 0 : static_cast<i32>(std::floor(lambda));
}

void append(std::string& out, const char* format, ...) noexcept {
  char line[512];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (n > 0) out.append(line, static_cast<usize>(std::min(n, static_cast<int>(sizeof(line) - 1))));
}

}  // namespace

std::string TextureFeedback::path_wanted() { return feedback_path.get(); }

bool TextureFeedback::create(const gfx::Device& device, GpuScene& scene,
                             gfx::ShaderLibrary& shaders, u32 frames_in_flight,
                             const std::string& path, std::string* error) {
  device_ = &device;
  scene_ = &scene;
  path_ = path;
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
  slot_count_ = scene.bindless().capacity().sampled_images;
  const u64 table_bytes = u64{slot_count_} * sizeof(u32);
  if (!gfx::create_buffer(device, table_bytes,
                          gfx::BufferUsage::Storage | gfx::BufferUsage::TransferSrc |
                              gfx::BufferUsage::TransferDst |
                              gfx::BufferUsage::ShaderDeviceAddress,
                          false, table_, error)) {
    return false;
  }
  readback_.resize(frames_in_flight);
  params_.resize(frames_in_flight);
  pending_.resize(frames_in_flight, u8{0});
  pending_frame_.resize(frames_in_flight, 0);
  pending_time_.resize(frames_in_flight, 0.0);
  for (u32 s = 0; s < frames_in_flight; ++s) {
    if (!gfx::create_buffer(device, table_bytes, gfx::BufferUsage::TransferDst, true,
                            readback_[s], error) ||
        !gfx::create_buffer(device, sizeof(TextureFeedbackParams) * k_max_views,
                            gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress,
                            true, params_[s], error)) {
      return false;
    }
  }
  // About 200 bytes a frame: a flight of 50,000 frames before the reserve is spent.
  lines_.reserve(16u << 20);
  refresh_textures();
  ENGINE_LOG_INFO(log_renderer, "texture feedback", log::field("path", path_),
                  log::field("textures", static_cast<u32>(textures_.size())),
                  log::field("device_bytes", device_bytes()));
  return true;
}

u64 TextureFeedback::device_bytes() const noexcept {
  u64 bytes = table_.size;
  for (const gfx::BufferResource& b : readback_)
    bytes += b.size;
  for (const gfx::BufferResource& b : params_)
    bytes += b.size;
  return bytes;
}

void TextureFeedback::refresh_textures() {
  Vector<GpuScene::TextureRecord> records;
  scene_->texture_records(records);
  Vector<Texture> next;
  next.reserve(records.size());
  for (const GpuScene::TextureRecord& r : records) {
    Texture t;
    // The one this slot held before, if the same image is still there: its history goes on.
    const i32 before = r.slot < by_slot_.size() ? by_slot_[r.slot] : -1;
    if (before >= 0 && textures_[static_cast<u32>(before)].record.width == r.width &&
        textures_[static_cast<u32>(before)].record.levels == r.levels &&
        textures_[static_cast<u32>(before)].record.bytes == r.bytes) {
      t = textures_[static_cast<u32>(before)];
    } else {
      for (f64& at : t.asked_at)
        at = -1.0e30;
      for (i32& h : t.held)
        h = -1;
    }
    t.record = r;
    // Each level's share of the device bytes by its 4x4 blocks (one for a level under four texels
    // a side): what a block format takes, and the allocator's rounding spread pro rata.
    u64 blocks[k_max_levels] = {};
    u64 total = 0;
    const u32 levels = std::min(r.levels, k_max_levels);
    for (u32 l = 0; l < levels; ++l) {
      const u32 w = std::max(1u, r.width >> l);
      const u32 h = std::max(1u, r.height >> l);
      blocks[l] = u64{(w + 3) / 4} * ((h + 3) / 4);
      total += blocks[l];
    }
    for (u32 l = 0; l < levels; ++l)
      t.level_bytes[l] = total == 0 ? 0 : r.bytes * blocks[l] / total;
    next.push_back(t);
  }
  textures_ = std::move(next);
  by_slot_.assign(slot_count_, -1);
  for (u32 i = 0; i < textures_.size(); ++i) {
    if (textures_[i].record.slot < slot_count_) by_slot_[textures_[i].record.slot] = static_cast<i32>(i);
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
  const f64 now = pending_time_[slot];
  const u32* codes = static_cast<const u32*>(readback_[slot].mapped);
  // A tile's mesh may have brought or taken textures since the list was made.
  if (scene_->texture_records_serial() != records_serial_) {
    refresh_textures();
    records_serial_ = scene_->texture_records_serial();
  }
  u64 resident = 0;
  u64 asked_now = 0;    // the levels from this frame's finest down, over the textures it asked
  u32 asked_textures = 0;
  u32 changed = 0;      // textures whose finest asked level is not the last frame's
  u64 held[k_holds] = {};
  u64 brought[k_holds] = {};  // bytes each hold would have had to bring in for this frame
  u32 brought_textures[k_holds] = {};
  for (Texture& t : textures_) {
    resident += t.record.bytes;
    const u32 code = t.record.slot < slot_count_ ? codes[t.record.slot] : k_never;
    const i32 level =
        code == k_never ? -1 : std::min(level_of(code), static_cast<i32>(t.record.levels) - 1);
    if (level != t.finest_last) ++changed;
    t.finest_last = level;
    if (level >= 0) {
      ++asked_textures;
      asked_now += bytes_from(t, level);
      if (t.finest_ever < 0 || level < t.finest_ever) t.finest_ever = level;
      for (u32 l = static_cast<u32>(level); l < k_max_levels; ++l)
        t.asked_at[l] = now;
    }
    for (u32 h = 0; h < k_holds; ++h) {
      // The finest level asked within the hold, which the hold keeps; the coarsest level is kept
      // whatever happens (a texture is never absent).
      i32 keep = static_cast<i32>(t.record.levels) - 1;
      for (u32 l = 0; l < std::min(t.record.levels, k_max_levels); ++l) {
        if (now - t.asked_at[l] <= k_hold_s[h]) {
          keep = static_cast<i32>(l);
          break;
        }
      }
      if (t.held[h] >= 0 && keep < t.held[h]) {
        brought[h] += bytes_from(t, keep) - bytes_from(t, t.held[h]);
        ++brought_textures[h];
      }
      t.held[h] = keep;
      held[h] += bytes_from(t, keep);
    }
  }
  ++frames_;
  append(lines_,
         "{\"frame\":%llu,\"t\":%.4f,\"textures\":%u,\"asked\":%u,\"changed\":%u,"
         "\"resident\":%llu,\"asked_bytes\":%llu,"
         "\"held\":[%llu,%llu,%llu],\"brought\":[%llu,%llu,%llu],\"brought_textures\":[%u,%u,%u]}\n",
         static_cast<unsigned long long>(pending_frame_[slot]), now,
         static_cast<u32>(textures_.size()), asked_textures, changed,
         static_cast<unsigned long long>(resident), static_cast<unsigned long long>(asked_now),
         static_cast<unsigned long long>(held[0]), static_cast<unsigned long long>(held[1]),
         static_cast<unsigned long long>(held[2]), static_cast<unsigned long long>(brought[0]),
         static_cast<unsigned long long>(brought[1]), static_cast<unsigned long long>(brought[2]),
         brought_textures[0], brought_textures[1], brought_textures[2]);
}

void TextureFeedback::destroy() noexcept {
  if (device_ == nullptr) return;
  // The device is idle: every slot's copy has landed.
  for (u32 s = 0; s < pending_.size(); ++s) {
    // Oldest first: the slot after the last recorded one is the oldest still pending.
    fold(s);
  }
  u64 resident = 0;
  u64 ever = 0;
  for (const Texture& t : textures_) {
    resident += t.record.bytes;
    ever += bytes_from(t, t.finest_ever);
    append(lines_,
           "{\"texture\":%u,\"format\":%u,\"width\":%u,\"height\":%u,\"levels\":%u,\"bytes\":%llu,"
           "\"admitted\":%s,\"finest_ever\":%d,\"ever_bytes\":%llu}\n",
           t.record.slot, static_cast<u32>(t.record.format), t.record.width, t.record.height,
           t.record.levels, static_cast<unsigned long long>(t.record.bytes),
           t.record.admitted ? "true" : "false", t.finest_ever,
           static_cast<unsigned long long>(bytes_from(t, t.finest_ever)));
  }
  append(lines_,
         "{\"summary\":true,\"frames\":%llu,\"textures\":%u,\"resident\":%llu,\"ever_bytes\":%llu,"
         "\"never_asked_share\":%.4f}\n",
         static_cast<unsigned long long>(frames_), static_cast<u32>(textures_.size()),
         static_cast<unsigned long long>(resident), static_cast<unsigned long long>(ever),
         resident == 0 ? 0.0 : 1.0 - static_cast<f64>(ever) / static_cast<f64>(resident));
  if (!path_.empty() && io::write_file(path_, lines_) != io::Status::Ok) {
    ENGINE_LOG_WARN(log_renderer, "texture feedback not written", log::field("path", path_));
  }
  gfx::destroy_compute_pipeline(*device_, pipeline_);
  gfx::destroy_buffer(*device_, table_);
  for (gfx::BufferResource& b : readback_)
    gfx::destroy_buffer(*device_, b);
  for (gfx::BufferResource& b : params_)
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
  pending_frame_[slot] = context.frame_index;
  pending_time_[slot] = time_s_;

  const GpuScene& scene = *scene_;
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
    p.slot_count = slot_count_;
    p.eye = context.gpu_eye;
    blocks[v] = p;
    view_width_[v] = view.source_width;
    view_height_[v] = view.source_height;
  }
  // Without the scene's pair table a pixel's pair names no instance this pass can read: nothing is
  // dispatched, and the frame counts as asking for nothing.
  const bool dispatch = scene.pair_table.address != 0;
  gfx::RenderGraph& graph = *context.graph;
  // Last frame's copy read the table; this frame clears it first.
  const gfx::RgBuffer table = graph.import_buffer("texture feedback", table_,
                                                  gfx::PipelineStage::AllTransfer,
                                                  gfx::MemoryAccess::TransferRead);
  const gfx::RgBuffer target = graph.import_buffer("texture feedback copy", readback_[slot]);
  const gfx::RgBuffer visibility = context.visibility;
  const gfx::BufferHandle table_buffer = table_.buffer;
  const gfx::BufferHandle target_buffer = readback_[slot].buffer;
  const u64 table_bytes = u64{slot_count_} * sizeof(u32);
  graph.add_pass(
      "texture feedback clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.write(table, gfx::Access::TransferWrite); },
      [table_buffer, table_bytes](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.fill_buffer(table_buffer, 0, table_bytes, k_never);
      });
  if (dispatch) {
    graph.add_pass(
        "texture feedback", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.read(visibility, gfx::Access::ComputeRead);
          b.write(table, gfx::Access::ComputeReadWrite);
        },
        [this, slot, timer = context.timer, bindless = context.bindless](gfx::CommandList cb,
                                                                         gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, pipeline_.pipeline);
          bindless->bind(cb, gfx::BindPoint::Compute);
          timer->begin(cb, "texture feedback");
          for (u32 v = 0; v < view_count_; ++v) {
            const u64 block = params_[slot].address + u64{v} * sizeof(TextureFeedbackParams);
            cb.push_constants(pipeline_.layout, gfx::ShaderStage::Compute, 0, sizeof(u64), &block);
            cb.dispatch((view_width_[v] + k_workgroup - 1) / k_workgroup,
                        (view_height_[v] + k_workgroup - 1) / k_workgroup, 1);
          }
          timer->end(cb);
        });
  }
  graph.add_pass(
      "texture feedback readback", gfx::PassKind::Transfer,
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
