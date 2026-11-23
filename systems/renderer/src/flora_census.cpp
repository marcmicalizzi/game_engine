// What a swaying tree's motion is a frame, for the chain's census (R206; renderer.md, "What a gale
// builds"; flythrough.h, `VisibleCensus::motion`). A measurement, never read by a frame: with
// `renderer.rt.census_motion` off (the default) it costs one tunable read a frame and holds
// nothing.
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/flora_motion.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/gpu_scene.h>

#include <algorithm>
#include <cmath>

namespace engine::renderer {
namespace {

tunables::Bool census_motion{"renderer.rt.census_motion", false,
                             "Measure every swaying tree's motion a frame (its parts' tips against "
                             "the last frame's pose) and its crown's distance, for engine-view "
                             "--census's histogram of the deforming entries (R206)"};

// A part's tip as the pose puts it: the bend at its whole weight, then its ancestors' frame.
Vec3 posed_tip(const gfx::FloraPartPose& pose, Vec3 tip) noexcept {
  Vec3 p;
  Vec3 n;
  gfx::flora_bend(pose, 1.0f, tip, Vec3{0.0f, 1.0f, 0.0f}, p, n);
  return p;
}

}  // namespace

// The last frame's poses are the region of the slot before this one, which no frame writes again
// for `k_joint_slots - 1` frames, so both are on the host's mapping as the pool pass reads them.
void GpuScene::measure_motion(u32 slot, const WorldEye& eye) {
  if (!census_motion.get()) {
    motion_.clear();
    return;
  }
  if (flora_poses_.mapped == nullptr || k_joint_slots < 2 || slot >= k_joint_slots) return;
  motion_.assign(instance_table_.size(), Vec2{0.0f, 0.0f});
  // A tree is measured only where the slot held the same tree, posed, the frame before: a tree a
  // tile has just brought (or a record handed on to another) would read another's last pose.
  if (motion_last_.size() < instance_table_.size()) motion_last_.resize(instance_table_.size(), 0);
  const u32 before = (slot + k_joint_slots - 1) % k_joint_slots;
  const u8* base = static_cast<const u8*>(flora_poses_.mapped) + gfx::k_flora_field_frame_bytes;
  const auto* now =
      reinterpret_cast<const gfx::FloraPartPose*>(base + u64{slot} * flora_region_bytes());
  const auto* last =
      reinterpret_cast<const gfx::FloraPartPose*>(base + u64{before} * flora_region_bytes());
  for (const FloraInstance& tree : flora_instances_) {
    if (tree.instance >= instance_table_.size() || tree.mesh >= flora_meshes_.size()) continue;
    const u64 key = (tree.last_tick << 32) | tree.first_part;
    const bool held = motion_last_[tree.instance] == key - (u64{1} << 32);
    motion_last_[tree.instance] = key;
    if (!held) continue;
    const FloraMesh& mesh = flora_meshes_[tree.mesh];
    f32 most = 0.0f;
    for (u32 p = 0; p < mesh.part_count(); ++p) {
      const geometry::ClusterFileMotionPart& part = mesh.parts[p];
      const Vec3 tip = part.pivot + part.direction * part.length;
      const Vec3 a = posed_tip(now[tree.first_part + p], tip);
      const Vec3 b = posed_tip(last[tree.first_part + p], tip);
      most = std::max(most, length(a - b));
    }
    const gfx::InstanceDesc& placed = instance_table_[tree.instance];
    // The crown: the mesh's rest bounds' centre carried by the instance's 3x4 into the eye's frame.
    const Vec4 bounds = tree.mesh < flora_bounds_.size() ? flora_bounds_[tree.mesh] : Vec4{};
    const Vec3 centre{bounds.x, bounds.y, bounds.z};
    const Vec3 from_eye = relative(gfx::instance_cell(placed), eye) +
                          Vec3{dot(placed.rows[0].xyz(), centre), dot(placed.rows[1].xyz(), centre),
                               dot(placed.rows[2].xyz(), centre)};
    motion_[tree.instance] = Vec2{most * placed.scale_max, std::max(length(from_eye), 1e-3f)};
  }
}

}  // namespace engine::renderer
