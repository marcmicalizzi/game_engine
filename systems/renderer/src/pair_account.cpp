#include <domain/geometry/flora_species.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/pair_account.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain_levels.h>
#include <systems/renderer/tile_layout.h>

namespace engine::renderer {
namespace {
tunables::Bool caster_policy{"renderer.flora.caster_policy", true,
                             "Use each flora species' traced and cascaded caster policy"};
}

u32 mesh_caster_policy(const SceneData& scene, u32 mesh) noexcept {
  if (!caster_policy.get() || mesh >= scene.sources.size()) return 0;
  const auto* info = geometry::flora_species_info(scene.sources[mesh].species);
  if (info == nullptr) return 0;
  u32 policy = 0;
  if (info->casters.cascades != geometry::k_flora_all_cascades) {
    const u32 nearest = info->casters.cascades < 6u ? info->casters.cascades : 6u;
    policy |= (nearest + 1u) << gfx::k_instance_cascade_shift;
  }
  if (info->casters.rest_pose) policy |= gfx::k_instance_shadow_rest;
  if (!info->casters.traced) policy |= gfx::k_instance_untraced;
  return policy;
}

u32 reserved_tile_pairs(u32 pairs) noexcept {
  return block_capacity(pairs, k_block_pair_granularity);
}

PairAccount account_pairs(const SceneData& scene, const ResolvedSettings& settings,
                          const TerrainLevelSet* rings) noexcept {
  PairAccount a;
  a.instances = scene.instances.size();
  a.prefix_pairs = scene.pair_count;
  a.tail_floor = scene.dynamic ? reserved_tile_pairs(scene.tail_floor_pairs) : 0;
  for (const gfx::InstanceDesc& instance : scene.instances) {
    if (instance.mesh >= scene.parts.size()) continue;
    const u64 pairs = scene.parts[instance.mesh].cluster_count;
    const u32 flags = instance.flags | mesh_caster_policy(scene, instance.mesh);
    if ((flags & gfx::k_instance_untraced) != 0)
      a.untraced_pairs += pairs;
    else if ((flags & gfx::k_instance_static_rt) != 0)
      a.static_pairs += pairs;
  }
  if (rings != nullptr && settings.terrain_levels && rings->valid() && !settings.stream) {
    for (u32 level = 1; level < rings->level_count(); ++level) {
      const auto c = rings->capacity(level);
      a.slot_pairs += u64{c.slots} * c.clusters_per_slot;
    }
  }
  return a;
}

bool pairs_fit(const PairAccount& a, std::string* error) noexcept {
  if (a.fixed_pairs() <= a.limit) return true;
  if (error != nullptr) {
    *error = "the scene's pairs do not fit: its " + std::to_string(a.instances) +
             " loaded instances hold " + std::to_string(a.prefix_pairs) +
             " pairs, the terrain rings' slots " + std::to_string(a.slot_pairs) +
             " and one tile of its streamed instances " + std::to_string(a.tail_floor) + ", " +
             std::to_string(a.minimum_pairs()) + " of the " + std::to_string(a.limit) + " " +
             a.named +
             "; stream the scene's instances (--world), place fewer, or lower terrain.rings.* "
             "and --terrain-far";
  }
  return false;
}
}  // namespace engine::renderer
