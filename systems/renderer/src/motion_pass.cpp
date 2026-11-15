#include <motion_pass.h>

#include <cstring>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

// 4x4 in double, row then column: the reprojection is three matrices multiplied and one inverted,
// and in float the product of a view-projection with its own inverse is off by parts in 1e7 — a
// thousandth of a pixel of false motion at 3840 wide on a frame where nothing moved. Double and one
// rounding at the end leaves parts in 1e16.
struct M4 {
  f64 m[4][4] = {};
};

M4 widen(const Mat4& a) noexcept {
  M4 out;
  for (u32 r = 0; r < 4; ++r)
    for (u32 c = 0; c < 4; ++c)
      out.m[r][c] = static_cast<f64>(a.at(r, c));
  return out;
}

Mat4 narrow(const M4& a) noexcept {
  Mat4 out;
  for (u32 r = 0; r < 4; ++r)
    for (u32 c = 0; c < 4; ++c)
      out.at(r, c) = static_cast<f32>(a.m[r][c]);
  return out;
}

M4 multiply(const M4& a, const M4& b) noexcept {
  M4 out;
  for (u32 r = 0; r < 4; ++r) {
    for (u32 c = 0; c < 4; ++c) {
      f64 sum = 0.0;
      for (u32 k = 0; k < 4; ++k)
        sum += a.m[r][k] * b.m[k][c];
      out.m[r][c] = sum;
    }
  }
  return out;
}

M4 translation(const DVec3& t) noexcept {
  M4 out;
  for (u32 k = 0; k < 4; ++k)
    out.m[k][k] = 1.0;
  out.m[0][3] = t.x;
  out.m[1][3] = t.y;
  out.m[2][3] = t.z;
  return out;
}

// Gauss-Jordan with partial pivoting. False for a singular matrix, which a view-projection with a
// positive near plane and a field of view never is.
bool invert(const M4& a, M4& out) noexcept {
  f64 w[4][8];
  for (u32 r = 0; r < 4; ++r) {
    for (u32 c = 0; c < 4; ++c) {
      w[r][c] = a.m[r][c];
      w[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  }
  for (u32 col = 0; col < 4; ++col) {
    u32 pivot = col;
    f64 best = w[col][col] < 0.0 ? -w[col][col] : w[col][col];
    for (u32 r = col + 1; r < 4; ++r) {
      const f64 v = w[r][col] < 0.0 ? -w[r][col] : w[r][col];
      if (v > best) {
        best = v;
        pivot = r;
      }
    }
    if (best == 0.0) return false;
    if (pivot != col) {
      for (u32 c = 0; c < 8; ++c) {
        const f64 t = w[col][c];
        w[col][c] = w[pivot][c];
        w[pivot][c] = t;
      }
    }
    const f64 inv = 1.0 / w[col][col];
    for (u32 c = 0; c < 8; ++c)
      w[col][c] *= inv;
    for (u32 r = 0; r < 4; ++r) {
      if (r == col) continue;
      const f64 f = w[r][col];
      if (f == 0.0) continue;
      for (u32 c = 0; c < 8; ++c)
        w[r][c] -= f * w[col][c];
    }
  }
  for (u32 r = 0; r < 4; ++r)
    for (u32 c = 0; c < 4; ++c)
      out.m[r][c] = w[r][c + 4];
  return true;
}

constexpr gfx::BufferUsage k_address =
    gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
// TRANSFER_SRC: a capture's `motion` channel reads the buffer back; TRANSFER_DST: the kept
// visibility buffer is this frame's every other frame, and the frame clears it with a fill.
constexpr gfx::BufferUsage k_screen =
    k_address | gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;

}  // namespace

bool MotionPass::create(const gfx::Device& device, const GpuScene& scene, const ViewSet& views,
                        u32 frames_in_flight, std::string* error) {
  params_.resize(frames_in_flight);
  moved_buffers_.resize(frames_in_flight);
  moved_.reserve(k_motion_moved_capacity);
  for (u32 slot = 0; slot < frames_in_flight; ++slot) {
    if (!gfx::create_buffer(device, sizeof(gfx::MotionParams) * k_max_views, k_address, true,
                            params_[slot], error) ||
        !gfx::create_buffer(device, sizeof(gfx::MovedInstance) * k_motion_moved_capacity,
                            k_address, true, moved_buffers_[slot], error)) {
      return false;
    }
  }
  // The second pool: this frame's blocks, written from last frame's inputs. Only where a pool is.
  if (scene.deform_pool.buffer.valid() &&
      !gfx::create_buffer(device, scene.deform_pool.size, k_address, false, prev_pool_, error)) {
    return false;
  }
  return resize(device, views, error);
}

bool MotionPass::resize(const gfx::Device& device, const ViewSet& views, std::string* error) {
  gfx::destroy_buffer(device, output_);
  gfx::destroy_buffer(device, prev_vis_);
  output_ = gfx::BufferResource{};
  prev_vis_ = gfx::BufferResource{};
  reset();
  const u64 pixels = views.source_pixels();
  return gfx::create_buffer(device, pixels * sizeof(u32), k_screen, false, output_, error) &&
         gfx::create_buffer(device, pixels * sizeof(u64), k_screen, false, prev_vis_, error);
}

void MotionPass::destroy(const gfx::Device& device) noexcept {
  gfx::destroy_buffer(device, output_);
  gfx::destroy_buffer(device, prev_vis_);
  gfx::destroy_buffer(device, prev_pool_);
  for (gfx::BufferResource& buffer : params_)
    gfx::destroy_buffer(device, buffer);
  for (gfx::BufferResource& buffer : moved_buffers_)
    gfx::destroy_buffer(device, buffer);
  *this = MotionPass{};
}

void MotionPass::reset() noexcept {
  last_ = Last{};
  phase_ = 0;
}

void MotionPass::frame_offset(f32& x, f32& y) const noexcept {
  sample_offset(static_cast<u32>(phase_ % k_sample_phases), x, y);
}

void MotionPass::swap_visibility(gfx::BufferResource& current) noexcept {
  // The two are the same size and usage (`resize`), so the frame's passes neither know nor care
  // which they were given; what was `current` holds the frame before this one.
  const gfx::BufferResource drawn = current;
  current = prev_vis_;
  prev_vis_ = drawn;
}

bool MotionPass::note_moves(const GpuScene& scene, std::span<const u32> indices) {
  bool fitted = true;
  for (const u32 index : indices) {
    const gfx::InstanceDesc* placed = scene.instance_record(index);
    if (placed == nullptr) continue;
    // Sorted by instance: the shader's search, and this one.
    u32 lo = 0;
    u32 hi = static_cast<u32>(moved_.size());
    while (lo < hi) {
      const u32 mid = (lo + hi) / 2;
      if (moved_[mid].instance < index) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo < moved_.size() && moved_[lo].instance == index) continue;  // its last frame is known
    if (moved_.size() >= k_motion_moved_capacity) {
      fitted = false;
      continue;
    }
    gfx::MovedInstance entry;
    entry.instance = index;
    entry.cell = placed->cell;
    entry.rows[0] = placed->rows[0];
    entry.rows[1] = placed->rows[1];
    entry.rows[2] = placed->rows[2];
    moved_.push_back(entry);
    for (u32 k = static_cast<u32>(moved_.size()) - 1; k > lo; --k) {
      const gfx::MovedInstance t = moved_[k];
      moved_[k] = moved_[k - 1];
      moved_[k - 1] = t;
    }
  }
  if (!fitted && !full_warned_) {
    full_warned_ = true;
    ENGINE_LOG_WARN(log_renderer, "the motion pass's moved table is full",
                    log::field("capacity", k_motion_moved_capacity));
  }
  return fitted;
}

void MotionPass::prepare(const Frame& frame, const ViewSet& views, const GpuScene& scene) {
  view_count_ = views.size();
  had_history_ = last_.valid;
  // The moved table as it stands is last frame's transforms: into this slot's copy, and then each
  // entry takes the transform this frame draws, which is last frame's to the next.
  const u32 moved = static_cast<u32>(moved_.size());
  if (moved > 0) {
    std::memcpy(moved_buffers_[frame.slot].mapped, moved_.data(),
                u64{moved} * sizeof(gfx::MovedInstance));
    for (gfx::MovedInstance& entry : moved_) {
      const gfx::InstanceDesc* placed = scene.instance_record(entry.instance);
      if (placed == nullptr) continue;
      entry.cell = placed->cell;
      entry.rows[0] = placed->rows[0];
      entry.rows[1] = placed->rows[1];
      entry.rows[2] = placed->rows[2];
    }
  }
  auto* blocks = static_cast<gfx::MotionParams*>(params_[frame.slot].mapped);
  for (u32 v = 0; v < view_count_; ++v) {
    const View& view = views[v];
    view_width_[v] = view.source_width;
    view_height_[v] = view.source_height;
    gfx::MotionParams p{};
    // Last frame's un-offset view-projection and eye; with no last frame, this frame's own, so
    // the vector is zero wherever nothing is deformed.
    const M4 before = widen(last_.valid ? last_.view_proj[v] : view.view_proj_center);
    const DVec3 moved_eye = last_.valid ? frame.eye - last_.eye : DVec3{};
    M4 inverse;
    if (!invert(widen(view.view_proj), inverse)) inverse = M4{};
    p.reproject = narrow(multiply(multiply(before, translation(moved_eye)), inverse));
    p.unproject = narrow(inverse);
    p.prev_view_proj = narrow(before);
    p.eye = frame.gpu_eye;
    p.prev_eye = last_.valid ? last_.gpu_eye : frame.gpu_eye;
    p.visibility = frame.visibility + frame.vis_offset[v] * sizeof(u64);
    p.prev_visibility = prev_vis_.address + frame.vis_offset[v] * sizeof(u64);
    p.pairs = frame.pairs;
    p.pair_entries = frame.pair_entries[v];
    p.visible = frame.visible;
    p.instances = frame.instances;
    p.meshes = frame.meshes;
    p.clusters = frame.clusters;
    p.triangles = frame.triangles;
    p.output = output_.address + frame.vis_offset[v] * sizeof(u32);
    p.moved = moved > 0 ? moved_buffers_[frame.slot].address : 0;
    p.prev_pool = prev_pool_.address;
    p.width = view.source_width;
    p.height = view.source_height;
    p.instance_count = frame.instance_count;
    p.moved_count = moved;
    p.offset_ndc_x = 2.0f * view.offset_x / static_cast<f32>(view.source_width);
    p.offset_ndc_y = -2.0f * view.offset_y / static_cast<f32>(view.source_height);
    blocks[v] = p;
  }
  // What the pool pass's second write reads: last frame's table, clock and part poses, where they
  // are still what last frame wrote — its frame slot was another (each slot's region is rewritten
  // by the frame that takes it), and the scene's tables did not change under them.
  prev_inputs_ = last_.valid && last_.slot != frame.slot && !frame.tables_changed;
  prev_deform_table_ = last_.deform_table;
  prev_deform_time_ = last_.deform_time;
  prev_flora_ = last_.flora;
  last_.valid = true;
  last_.slot = frame.slot;
  last_.eye = frame.eye;
  last_.gpu_eye = frame.gpu_eye;
  for (u32 v = 0; v < view_count_; ++v)
    last_.view_proj[v] = views[v].view_proj_center;
  last_.deform_table = frame.deform_table;
  last_.deform_time = frame.deform_time;
  last_.flora = frame.flora;
  ++phase_;
}

gfx::DeformParams MotionPass::previous(const gfx::DeformParams& current) const noexcept {
  gfx::DeformParams p = current;
  p.pool = prev_pool_.address;
  p.normal_pool = 0;  // positions only: nothing shades from last frame's normals
  // The deformer's clock is a number, so last frame's is good wherever there was a last frame; the
  // table and the part poses are regions of per-slot buffers, good only while last frame's slot
  // has not been rewritten.
  if (had_history_) p.time = prev_deform_time_;
  if (prev_inputs_) {
    p.deform = prev_deform_table_;
    p.flora = prev_flora_;
  }
  return p;
}

void MotionPass::record(gfx::CommandList cb, const gfx::ComputePipeline& pipeline,
                        gfx::GpuTimer& timer, u32 slot) const {
  cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
  timer.begin(cb, "motion");
  for (u32 v = 0; v < view_count_; ++v) {
    const u64 block = params_[slot].address + u64{v} * sizeof(gfx::MotionParams);
    cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64), &block);
    cb.dispatch((view_width_[v] + gfx::k_motion_workgroup - 1) / gfx::k_motion_workgroup,
                (view_height_[v] + gfx::k_motion_workgroup - 1) / gfx::k_motion_workgroup, 1);
  }
  timer.end(cb);
}

u64 MotionPass::device_bytes() const noexcept {
  u64 bytes = output_.size + prev_vis_.size + prev_pool_.size;
  for (const gfx::BufferResource& buffer : params_)
    bytes += buffer.size;
  for (const gfx::BufferResource& buffer : moved_buffers_)
    bytes += buffer.size;
  return bytes;
}

}  // namespace engine::renderer
