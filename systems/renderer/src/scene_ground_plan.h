#pragma once

// The scene reader's placement entries, and what they do to the ground and to each other before
// and between their expansions (roadmap R144; docs/subsystems/scene_gen.md, "What an entry plans
// for the ground" and "The shapes a tile occupies"). Beside `scene.cpp` and not in it, because that
// file is where every feature of a scene's read meets.

#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <core/json/json_value.h>
#include <domain/content_build/content_build.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>

#include <optional>
#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::renderer {

// One placement entry of a scene file, as the reader walks them: the `ruins` entries, then
// `placements`, in the file's order.
struct PlacementRef {
  std::string generator;
  JsonValue params;
  std::string where;
  // In a streamed world, whether the world streams it (`PlacementEntry::stream`); a `ruins` entry
  // always is.
  bool stream = true;
};
Vector<PlacementRef> placement_entries(const scene::Scene& file, const std::string& path);

// The job system's performance pool, made the first time a generator asks for it: a read of a few
// tiles never starts one, and a read of many shares one, as the load's meshes and textures share
// one.
struct LazyPool {
  std::optional<jobs::JobSystem> pool;
  static jobs::JobSystem* get(void* context) {
    auto* self = static_cast<LazyPool*>(context);
    if (!self->pool.has_value()) self->pool.emplace(content_build::job_config(0));
    return &*self->pool;
  }
};

// **Before the ground is made**: builds what every placement reads of the scene besides the ground
// — the exclusion (G135; unbuilt when `renderer.placement.exclusion` is false) and the site (G144)
// — and then asks each entry whose generator plans for the ground (`plan_ground`) for its edits and
// its openings, in the scene's order, on the ground as it was before any edit. The edits are
// appended to the terrain's tile set's (`out.terrain.tiles->edits`) and the openings to its holes
// (compiled into `out.terrain.holes`, and into the exclusion), after the scene's own, so the ground
// the caller makes next — and everything that reads the description after it — is the edited one.
// A scene none of whose generators plans pays a lookup an entry. `world`: the read is a streamed
// world's. False with a sentence.
bool plan_scene_ground(const scene::Scene& file, const std::string& path, const std::string& dir,
                       std::span<const PlacementRef> entries, bool world, SceneDesc& out,
                       std::string& error);

// **Between two entries of a whole read**: the shapes the entry handed (`Placements::shapes`) go
// into the scene's exclusion, so `Context::excluded_mm` answers by them for every entry after it —
// and for the world's placements consumer and the collision, which are handed the same object —
// and into `earlier`, after the placements' discs. False with the exclusion's sentence for a shape
// it refuses.
bool hand_shapes_on(const scene_gen::FootprintList& shapes, const std::string& where,
                    SceneDesc& out, scene_gen::FootprintList& earlier, std::string& error);

}  // namespace engine::renderer
