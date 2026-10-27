#pragma once

#include <core/base/types.h>
#include <core/json/json_value.h>

#include <string>

namespace engine::renderer {

struct SceneData;
struct ResolvedSettings;
class TerrainLevelSet;

// One account for the immutable prefix, terrain reservations and the tail's admission.
// Visibility counts every pair; the frame's structures exclude untraced and static instances.
struct PairAccount {
  u64 instances = 0;
  u64 prefix_pairs = 0;
  u64 slot_pairs = 0;
  u64 tail_floor = 0;
  u64 limit = 1u << 24;
  const char* named = "the visibility id names";
  u64 untraced_pairs = 0;
  u64 static_pairs = 0;

  u64 fixed_pairs() const noexcept { return prefix_pairs + slot_pairs; }
  u64 minimum_pairs() const noexcept { return fixed_pairs() + tail_floor; }
  u64 frame_pairs() const noexcept { return minimum_pairs() - untraced_pairs - static_pairs; }
};

// Uses the very capacities lay_out_rings consumes, including every far level's slots.
PairAccount account_pairs(const SceneData& scene, const ResolvedSettings& settings,
                          const TerrainLevelSet* rings = nullptr) noexcept;
bool pairs_fit(const PairAccount& account, std::string* error) noexcept;
// The same size class that TileLayout reserves, used before a tile is admitted.
u32 reserved_tile_pairs(u32 pairs) noexcept;
u32 mesh_caster_policy(const SceneData& scene, u32 mesh) noexcept;

// Cold reporting only: the admission account and, before upload, the terrain layout's slots.
// A GPU scene fills the live tail and uploaded slot details from its existing tables.
JsonValue pair_account_summary(const PairAccount& account, TerrainLevelSet* rings = nullptr);

}  // namespace engine::renderer
