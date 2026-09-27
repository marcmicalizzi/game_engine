#pragma once

// Island City as a placement generator, "city" (docs/subsystems/city.md, "Where it attaches";
// docs/subsystems/scene_gen.md; ADR-0044, ADR-0046): a scene's `placements` entry naming "city"
// with a plan reference draws the plan's proxies — the twelve box meshes instanced per element of
// the plan and its buildings (fragment.h) — through the scene-generator registry, by a scene reader
// and a world ring that link nothing of this capability. Until it existed the city reached the
// renderer only as a fragment `engine-content city fragment` wrote to disk.
//
// The entry's parameters are an `engine.city.CityEntry`: the plan (a directory holding plan.json,
// or the file), one tile or one district or neither, the detail, and where the proxy meshes are.
// `open` reads the plan and writes the proxy meshes where they are missing; `expand` is **the
// fragment writer's content in memory** — a tile's proxies (`tile_proxies`), a district's buildings
// and ground, or every lot's buildings and every street and park — one placement per proxy, placed
// exactly as the fragment places its instance; `meshes` is the twelve boxes; and `tile` is the
// per-tile query (`tile_proxies`), the plan's content restricted to the tile at the detail the
// city's LOD policy gives the tile's ring (`stage_for_distance` at the ring's inner radius), so a
// streamed world streams its buildings' proxies. A streamed entry's plan must be on the world's
// tile grid. Every placement's tag is 0 and every tile's kind 1: the city draws one kind of thing.
//
// Registered from this capability's own source (`scene_generator.cpp`); the module is
// `WHOLE_ARCHIVE`, which keeps the registrar.

namespace engine::city {

// The name the generator registered under.
inline constexpr const char* k_placement_generator = "city";

}  // namespace engine::city
