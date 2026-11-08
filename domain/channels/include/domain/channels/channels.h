#pragma once

// The channels capability (ADR-0027; docs/subsystems/channels.md, roadmap W86): the drainage of a
// region's ground, the channels it carries, and what dresses them.
//
// **What it is.** A placement generator, "channels", that routes the flow over a lattice of the
// ground it is handed (the pits filled, D8 directions, contributing area, all in integers so every
// toolchain agrees), begins a channel where contributing area times slope crosses Montgomery and
// Dietrich's threshold, follows the links downstream, gives each point a bankfull width and depth
// from its area by the downstream hydraulic geometry, low-passes the line off the lattice and bends
// it by Leopold and Wolman's meander rule where the slope allows, and places the bed's gravel skin,
// the bars of finer sediment and the cut-soil banks along it as closed meshes the mesh generator
// "channel" builds through the derived-data cache. The ground already holds the trough (the erosion
// model cut it), so the generator **dresses** it and never cuts it.
//
// **What it is not.** It moves no water and edits no ground. The water a channel carries is drawn
// by the water capability from the description this writes (`water_channels_json`,
// `engine.scene.WaterChannel`), and the subcommand `engine-content channels` writes it from the
// same network the generator places (`plan_channels` is the one function both call).
//
// **Determinism.** A function of the layer, the world seed and the ground's answers at the
// lattice's whole millimetres: heights are taken to the micrometre as integers, the fill and the
// routing order are total orders over (height, index), and the job system only fills the height
// lattice, each row by itself, so the network is the same bytes at any thread count and any tile
// order. A recipe's file is its canonical JSON, keyed by its hash, so a container is the same bytes
// from every toolchain.
//
// Checklist (ADR-0027 decision 2), and where each item is:
//   [x] schema types      schemas/channels.schema, engine_schema_library
//   [ ] scheduler entry   not needed: nothing ticks; the network is decided when a scene is read
//   [ ] render passes     not needed: the meshes are scene instances like any placement's
//   [x] derived data      the mesh generator "channel" (`.channel.json` recipes, the cache)
//   [ ] protocol methods  not needed
//   [ ] tunables          not needed: every number is the layer's
//   [x] LOD policy        the cluster LOD DAG of each piece; nothing per tier
//   [x] determinism       above
//   [x] zero cost unused  a scene with no `channels` entry never calls it (the registry)
//   [x] docs, tests, size table

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/world.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/scene_gen/exclusion.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/channels.h>
#include <string>

namespace engine::jobs {
class JobSystem;
}
namespace engine::materials {
struct Pack;
}

namespace engine::channels {

// Bumped when a change makes an old network or recipe wrong: it is in every recipe's hash.
// 2 (W89): the bed is the cut's flat bed and no bank piece is placed.
// 3 (W90): pieces by reach (boulder steps, nothing on bedrock and colluvial), mouths, confluences.
inline constexpr u32 k_channels_version = 3;

// A point of a channel's line, downstream order: where it is (y the ground under it, metres), its
// bankfull width and depth, the bed's fall per metre there, and the contributing area.
struct ChannelPoint {
  WorldPos at{};
  f32 width_m = 0;
  f32 depth_m = 0;
  f32 slope = 0;
  f32 area_m2 = 0;
};

// A link: the stretch of channel between a head or a junction and the next junction or the region's
// edge. `cells` are lattice indices downstream, the first the link's start and the last the
// junction (shared with the link below) or the outlet; `line` is the resampled, smoothed, meandered
// line.
struct ChannelLink {
  Vector<u32> cells;
  Vector<ChannelPoint> line;
  f64 length_m = 0;   // of `line`
  f64 slope = 0;      // the link's mean fall per metre along its cells, from the ground
  f64 meander = 0;    // the share of the meander amplitude the slope allowed, 0 to 1
  bool head = false;  // the link begins at a channel head
  u64 seed = 0;
  // **W90** (channels.md, "Reaches", "Mouths", "Confluences"), one per point of line:
  Vector<u8> reach;        // the reach's ChannelReach code
  Vector<f32> step_depth;  // how far a step-pool's pool lowers the cut's bed below its line, m
  Vector<f64> bed;         // the cut's bed, metres: the thalweg's, the steps', the fan's, the grade's
  // The steps' crests (indices into line) and their heights: where the boulders stand.
  Vector<u32> step_at;
  Vector<f32> step_height;
  // The points the water and the pieces run to: line.size(), less for a tributary whose last
  // points lie within the trunk's banks.
  u32 keep = 0;
  // The link this one joins as a tributary (its index), or ~0u.
  u32 trunk = ~0u;
  // Ends at a water body's shoreline (the last point), whose level is mouth_level; begins at
  // one (a lake's outlet).
  bool mouth = false;
  bool outlet = false;
  f64 mouth_level = 0;
};

// What the water bodies did to a network (W90): the links that end at a shoreline, the links
// dropped whole (begun under a body and never out of it), the outlets split off, and the points
// taken out from under the water.
struct ChannelWaterCounts {
  u32 mouths = 0;
  u32 dropped = 0;
  u32 outlets = 0;
  u32 confluences = 0;
  u64 points_removed = 0;
};

// The network over a layer's region: the lattice, its routing, and the links.
struct ChannelNetwork {
  u32 width = 0;  // lattice points along x
  u32 depth = 0;  // along z
  i64 x_mm = 0;   // point (0, 0), world millimetres
  i64 z_mm = 0;
  i64 spacing_mm = 0;
  Vector<i64> ground_um;      // the ground, micrometres
  Vector<i64> filled_um;      // the ground with its pits filled (Barnes et al. 2014, with epsilon)
  Vector<u32> receiver;       // D8 steepest descent over `filled_um`; ~0u at the region's edge
  Vector<u32> area_cells;     // contributing area, in cells (the cell itself counted)
  Vector<u8> channel;         // 1 where a channel runs
  Vector<u32> heads;          // channel heads, ascending index
  Vector<ChannelLink> links;  // by their first cell, ascending
  f64 length_m = 0;           // every link's line, summed
  ChannelWaterCounts water;   // W90
  f64 area_m2() const noexcept {
    const f64 s = static_cast<f64>(spacing_mm) / 1000.0;
    return static_cast<f64>(width - 1) * s * static_cast<f64>(depth - 1) * s;
  }
};

// The layer's numbers in range, and its region aligned to its spacing. False with a sentence.
bool layer_in_range(const ChannelLayer& layer, std::string& error);

// **The one function** the generator and `engine-content channels` both call: the network over the
// layer's region standing on `ground`. `jobs` (nullable) samples the lattice a row a job; the
// result does not depend on it. `seed` is mixed into every link's (the world seed for the
// generator). Refuses an absent or non-finite ground and an exceeded budget; `out` clears on error.
// `water` (nullable; W90): the scene's water bodies, read through `Exclusion::excluded_mm`'s water
// kind — a link ends at the first shoreline it meets (a mouth), a link begun under a body is
// dropped, and what leaves a body again is an outlet of its own; null, or with no bodies, nothing
// is cut.
bool plan_channels(const ChannelLayer& layer, const scene_gen::Ground& ground, u64 seed,
                   jobs::JobSystem* jobs, ChannelNetwork& out, std::string& error,
                   const scene_gen::Exclusion* water = nullptr);

// The reach of a fall per metre by the layer's bounds (Montgomery & Buffington 1997); `colluvial`
// picks colluvial over bedrock above `slope_steep`.
ChannelReach reach_of(const ChannelLayer& layer, f64 slope, bool colluvial);

// **The description the water draws** (`engine.scene.WaterChannel` version 1, declared by the water
// capability): one object per link whose last point drains at least `min_area_m2`, in link order,
// each `{name, points, width_m, depth_m, slope, seed}` — `points` the line as `worldpos`, y the
// water's surface at `stage` of the bankfull depth over the bed, `depth_m` that water's depth, the
// bed below the surface by it — as a JSON array. `cut` (W89): the bed is the cut's
// (`terrain_cuts_json`) and the stage a share of its thalweg depth, so the surface lies inside the
// banks the cut makes; without it the surface stands on the ground as W86 laid it.
JsonValue water_channels_json(const ChannelLayer& layer, const ChannelNetwork& network,
                              f64 min_area_m2, f32 stage, bool cut = false);

// **The cuts** (`engine.scene.TerrainCut`, the `tiles` block's `cuts`; channels.md, "The cut";
// W89): one object per link of the network (every link, the rills' too), in link order, each
// `{name, points, width_m, depth_m, bank_slope}` — `points` the line with y the top of the bank
// (the ground under the line), `width_m` the bankfull width, `depth_m` the bed below the top: the
// section's thalweg (`bank_slope`'s trapezoid of the hydraulic geometry's area), the bed held
// down the line, never more than two thalwegs under the ground at it — as a JSON array.
JsonValue terrain_cuts_json(const ChannelLayer& layer, const ChannelNetwork& network);

// One placed piece (a stretch of a link up to `piece_m` long, one part of it): where its frame
// stands (x and z whole millimetres, y the ground there), its world tile, its recipe and a stable
// id within the entry.
struct PlacedPiece {
  WorldPos origin{};
  scene_gen::TileCoord tile{};
  ChannelPiece recipe;
  u32 id = 0;
};
// Every piece of the network, in link order, then piece, then part (bed, bars, banks). Each piece's
// heights are the ground's at its vertices, which is why it takes the ground again.
bool place_pieces(const ChannelLayer& layer, const ChannelNetwork& network,
                  const scene_gen::Ground& ground, f32 tile_size, Vector<PlacedPiece>& out,
                  std::string& error);

// A recipe's numbers in range: slab sizes, finite heights, top above bottom.
bool piece_in_range(const ChannelPiece& piece, std::string& error);
// The closed, welded mesh of a recipe: each slab's top, bottom and four walls; one material.
bool solve_piece(const ChannelPiece& piece, assets::MeshData& out, std::string& error);
// Over the recipe's canonical JSON and the version: what its `.channel.json` file is named and
// keyed by.
u64 piece_source_hash(const ChannelPiece& piece);
// The canonical JSON a recipe is written as.
std::string piece_json(const ChannelPiece& piece);
bool build_piece_container(const ChannelPiece& piece, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                           const materials::Pack* pack = nullptr);

}  // namespace engine::channels
