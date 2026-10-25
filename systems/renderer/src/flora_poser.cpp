#include <core/jobs/job_system.h>
#include <core/math/world.h>
#include <core/platform/topology.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/flora_poser.h>

#include <algorithm>

namespace engine::renderer {

namespace {

tunables::Int flora_workers{"renderer.flora.workers", 0, 0, 256,
                            "Threads posing the trees beside the frame's own, read at load; 0 is "
                            "automatic (one per performance CPU but the frame thread's), 1 or a "
                            "small scene poses every tree on the frame's thread"};

}  // namespace

u32 flora_workers_tunable() noexcept { return static_cast<u32>(flora_workers.get()); }

FloraPoseResult flora_pose_instances(const FloraPoseInputs& in, const FloraPoseFrame& frame,
                                     u32 begin, u32 end) noexcept {
  FloraPoseResult result;
  const Vec2 to = frame.wind_to;
  for (u32 i = begin; i < end; ++i) {
    FloraInstance& fi = in.instances[i];
    const FloraMesh& tree = in.meshes[fi.mesh];
    const gfx::InstanceDesc& placed = in.placed[fi.instance];
    auto to_mesh = [&](Vec3 v) {
      return Vec3{dot(fi.inverse[0], v), dot(fi.inverse[1], v), dot(fi.inverse[2], v)};
    };
    FloraWind wind;
    const Vec3 along = to_mesh(Vec3{to.x, 0.0f, to.y});
    const f32 along_length = length(along);
    wind.to = along_length > 0.0f ? along * (1.0f / along_length) : Vec3{1.0f, 0.0f, 0.0f};
    wind.strength = frame.wind_strength;
    FloraPush push;
    if (frame.flora != nullptr && frame.flora->walker) {
      const FrameFlora& walker = *frame.flora;
      // The walker's feet from the instance's own place, in f64 to the cell and float after: the
      // mesh frame's, never a world position in a float.
      const WorldPos origin{
          static_cast<f64>(placed.cell.x) * k_world_cell_m + static_cast<f64>(placed.rows[0].w),
          static_cast<f64>(placed.cell.y) * k_world_cell_m + static_cast<f64>(placed.rows[1].w),
          static_cast<f64>(placed.cell.z) * k_world_cell_m + static_cast<f64>(placed.rows[2].w)};
      const Vec3 feet = to_mesh(relative(walker.walker_feet, origin));
      push.radius = walker.walker_radius * fi.inverse_scale;
      push.height = walker.walker_height * fi.inverse_scale;
      push.feet = feet;
      // Only a tree the capsule can reach is tested part by part.
      const Vec4 bound = in.bounds[fi.mesh];
      const Vec3 centre{bound.x, bound.y, bound.z};
      const Vec3 mid = feet + Vec3{0.0f, push.height * 0.5f, 0.0f};
      push.active = length(mid - centre) < bound.w + push.height + push.radius;
    }
    const u32 ticks = fi.posed && frame.frame_index > fi.last_tick
                          ? static_cast<u32>(std::min<u64>(frame.frame_index - fi.last_tick,
                                                           k_flora_push_ease_ticks))
                          : 0u;
    const u32 parts = tree.part_count();
    const std::span<FloraPartState> state = in.state.subspan(fi.first_part, parts);
    gfx::FloraPartPose* out = in.region + fi.first_part;
    flora_part_poses(tree, wind, frame.time_s, fi.seed, push, ticks, state,
                     std::span<gfx::FloraPartPose>(out, parts));
    for (const FloraPartState& s : state)
      result.pushed_parts += s.ease > 0.0f ? 1u : 0u;
    for (u32 p = 0; p < parts && !result.moving; ++p)
      result.moving = out[p].axis.w != 0.0f;
    fi.last_tick = frame.frame_index;
    fi.posed = true;
  }
  return result;
}

FloraPoser::FloraPoser() noexcept = default;
FloraPoser::~FloraPoser() { reset(); }

void FloraPoser::reset() noexcept {
  jobs_.reset();
  chunks_.clear();
  workers_ = 0;
  grows_ = false;
}

u32 FloraPoser::worker_count() const noexcept {
  return jobs_ != nullptr ? jobs_->worker_count(jobs::Pool::Performance) : 0u;
}

void FloraPoser::prepare(std::span<const FloraInstance> instances,
                         std::span<const FloraMesh> meshes, u32 workers, u32 chunk_parts,
                         bool grows) {
  reset();
  if (workers == 0) {
    const u32 cpus = platform::topology().performance_cpus.count();
    workers = cpus > 1 ? cpus - 1 : 0;  // the frame's thread is the one left
  }
  workers_ = workers;
  chunk_parts_ = chunk_parts > 0 ? chunk_parts : 1u;
  grows_ = grows;
  // Room for the most chunks there can be, so a cut again after the instances change writes in
  // place.
  chunks_.reserve(k_flora_max_chunks + 1);
  recut(instances, meshes);
}

void FloraPoser::recut(std::span<const FloraInstance> instances,
                       std::span<const FloraMesh> meshes) {
  chunks_.clear();
  const u32 n = static_cast<u32>(instances.size());
  if (n == 0) return;
  u64 total = 0;
  for (const FloraInstance& fi : instances)
    total += meshes[fi.mesh].part_count();
  const u32 workers = workers_;
  // Four chunks a thread, so a thread that started late or was preempted is evened out by the
  // others; never fewer parts a chunk than pay for a wake-up; never more chunks than parallel_for
  // keeps in place, nor than instances (an instance is never split: its parts compose in order).
  const u64 threads = u64{workers} + 1;
  u64 count = std::min<u64>(k_flora_max_chunks, threads * 4);
  count = std::min<u64>(count, total / chunk_parts_);
  count = std::min<u64>(count, n);
  if (workers == 0 || count < 2) count = 1;
  // Cut at equal shares of the parts, so a grove of one large species and many small ones splits
  // by the work and not by the count.
  chunks_.push_back(0);
  u64 acc = 0;
  u64 c = 1;
  for (u32 i = 0; i < n && c < count; ++i) {
    acc += meshes[instances[i].mesh].part_count();
    if (acc * count >= total * c) {
      chunks_.push_back(i + 1);
      while (c < count && acc * count >= total * c)
        ++c;
    }
  }
  if (chunks_.back() != n) chunks_.push_back(n);
  const u32 chunks = chunks_.size() - 1;
  if (chunks < 2 || jobs_ != nullptr) return;
  jobs::JobSystemConfig config;
  // Never more threads than chunks to run beside the frame's own — unless the instances come and
  // go (a streamed world's trees), when the pool is made once, the first time it pays, for as many
  // chunks as there may later be.
  config.performance_workers = grows_ ? workers : std::min<u32>(workers, chunks - 1);
  config.efficiency_workers = 1;  // the class's pool is never used here; 0 would be its default
  // Unpinned, as the time-lapse's pool is: two pools pinned to the same CPUs would contend for them
  // by placement where the OS can balance them.
  config.pin_threads = false;
  jobs_ = std::make_unique<jobs::JobSystem>(config);
}

FloraPoseResult FloraPoser::pose(const FloraPoseInputs& in, const FloraPoseFrame& frame) noexcept {
  const u32 n = static_cast<u32>(in.instances.size());
  const u32 chunks = chunk_count();
  ENGINE_ASSERT(chunks == 0 || chunks_[chunks] == n, "FloraPoser::pose: not the prepared trees");
  if (jobs_ == nullptr || chunks < 2 || chunks_[chunks] != n)
    return flora_pose_instances(in, frame, 0, n);
  FloraPoseResult results[k_flora_max_chunks];
  // A chunk a job; at most k_flora_max_chunks of them, which parallel_for holds without allocating.
  jobs_->parallel_for(jobs::Pool::Performance, chunks, 1, [&](u32 begin, u32 end) {
    for (u32 c = begin; c < end; ++c)
      results[c] = flora_pose_instances(in, frame, chunks_[c], chunks_[c + 1]);
  });
  FloraPoseResult out;
  for (u32 c = 0; c < chunks; ++c) {
    out.pushed_parts += results[c].pushed_parts;
    out.moving = out.moving || results[c].moving;
  }
  return out;
}

}  // namespace engine::renderer
