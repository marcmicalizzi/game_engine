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
// 4 (W91): steps on step-pool reaches alone, cascade clusters, knickpoints, the line in its trough.
// 5 (W93): the section follows the bends (the bed toward the outer bank, the bars the ground's own
//    point bars with a skin on them), the flood bed.
// 6 (W93, continued): no bed slab and no bar skin; the boulders alone are placed.
// 7 (W93, continued): the kinks low-passed out of every line, a steep line's wander, the pool
//    downstream of its bend's apex, the steep reaches' section, boulders at a step's banks.
inline constexpr u32 k_channels_version = 7;

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
  // The contributing area at the link's last cell, in lattice cells: what orders the junctions,
  // kept on the link so a plan read from its file (which holds no lattice) orders them the same.
  u32 area_end = 0;
  // **W90** (channels.md, "Reaches", "Mouths", "Confluences"), one per point of `line`:
  Vector<u8> reach;        // the reach's `ChannelReach` code
  Vector<f32> step_depth;  // how far a step-pool's pool lowers the cut's bed below its line, m
  Vector<f64> bed;  // the cut's bed, metres: the thalweg's, the steps', the fan's, the grade's
  // The steps' crests (indices into `line`) and their heights: where the boulders stand.
  Vector<u32> step_at;
  Vector<f32> step_height;
  // **W91** (channels.md, "Reaches"): a cascade's boulder clusters (indices into `line`) and their
  // clasts' size, metres; 1 at each point of a knickpoint, where the cut's bed follows the ground
  // and the water falls; the knickpoints; the stretches still at 45 degrees or steeper away from a
  // knickpoint after the chute's limit; the points of `line` the trough test drew back.
  Vector<u32> cluster_at;
  Vector<f32> cluster_size;
  Vector<u8> knick;
  u32 knickpoints = 0;
  f64 lip_convexity = 0;  // the knickpoints' second differences at their lips, summed
  u32 steep_left = 0;
  u32 trough_pulled = 0;
  // W93 (channels.md, "A line across a slope"): the points moved across the line so that neither
  // bank's ground stands more than half a depth under the line's.
  u32 bank_moved = 0;
  // **W93** (channels.md, "The section", "The flood bed"), one per point of `line`: the section's
  // asymmetry, -1 to 1 — positive where the line bends left (toward (-dz, dx)), so the point bar
  // is on the left and the cut bank and the thalweg on the right; 0 at a riffle, and on a steep
  // reach an irregular wander of the thalweg between the banks; how much wider the cut is than
  // the hydraulic geometry's width, as a share of it (`swell`: up to half `riffle_widening` wider
  // at a riffle and narrower at a pool's apex, half `step_widening` narrower at a step's crest and
  // wider over its pool, 0 where a reach has neither); the flood bed's reach from the line each
  // side, metres (0: none); and the entrenchment ratio measured there (the flood-prone width over
  // the bankfull width; 0 where it was not measured).
  Vector<f32> asym;
  Vector<f32> swell;
  Vector<f32> flood_left;
  Vector<f32> flood_right;
  Vector<f32> entrenchment;
  // The points the water and the pieces run to: `line.size()`, less for a tributary whose last
  // points lie within the trunk's banks.
  u32 keep = 0;
  // The link this one joins as a tributary (its index), or ~0u.
  u32 trunk = ~0u;
  // Ends at a water body's shoreline (the last point), whose level is `mouth_level`; begins at
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

// **The plan** (channels.md, "One plan"; W93, the proof of concept's solver needs 2.1 item 9): the
// network of a layer over the ground **before its cuts**, made once and kept as a derived file,
// `<derived-data root>/channels/plan-<key>.channel-plan`. Everything that needs a channel's line
// reads this one object: `terrain_cuts_json` (the cut), `water_channels_json` (the water's
// description, and through it the exclusion's corridor) and `place_pieces` (what the generator
// still places). `network` holds the links, the heads and the counts; the lattice's own arrays
// (heights, fill, receivers, areas) are in it when the plan was made by this call and empty when
// it was read, since no reader of a plan asks them.
//
// **The key** is what the plan was made from: the layer (every field but `plan`), the world's
// seed, `k_channels_version`, the ground's heights at every point of the layer's lattice to the
// micrometre and whether a water body covers each. A layer that names no plan is keyed that way
// from the ground it is handed, which must then be the ground before any cut; a hit reads the
// file and plans nothing. **A layer on a cut ground names its plan** (`ChannelLayer.plan`, the
// key `engine-content channels` printed, 16 hexadecimal digits): the cut ground's heights are not
// what the plan was made from, so the key cannot be made again from them, and the generator
// never plans over a ground its own cuts lowered. A named plan that is not in the directory, or
// that records another layer or seed than the entry's, is refused with the command that makes it.
struct ChannelPlan {
  u64 key = 0;
  u64 layer_hash = 0;  // `plan_layer_hash` of the layer it was made from
  u64 seed = 0;        // the world's seed it was made with
  ChannelNetwork network;
  bool planned = false;  // made by this call (a miss); false: read from its file
};
// Over the layer's canonical JSON with `plan` cleared, and `k_channels_version`.
u64 plan_layer_hash(const ChannelLayer& layer);
// "<dir>/plan-<key as 16 lower-case hex digits>.channel-plan".
std::string plan_path(std::string_view dir, u64 key);
std::string plan_key_text(u64 key);
// Where the capability's derived files live: `<ddc>/channels`, `ddc` the host's root when it named
// one, else the repository's found from `scene_dir`, else `<scene_dir>/ddc` — the rule the
// generator's recipes always had. Made when it is not there. False with a sentence.
bool derived_dir(std::string_view ddc, std::string_view scene_dir, std::string& out,
                 std::string& error);
// **The one way to a plan.** `context` is what a generator is handed: its ground, the world's
// seed, the job system (asked only on a miss) and the exclusion whose water bodies end a link at a
// shoreline. With `layer.plan` empty: keys the plan from the ground, reads it on a hit, plans and
// writes it on a miss. With `layer.plan` a key: reads that file and plans nothing, whatever the
// ground. `dir` is `derived_dir`'s.
bool ensure_plan(const ChannelLayer& layer, const scene_gen::Context& context, std::string_view dir,
                 ChannelPlan& out, std::string& error);
// The file's bytes for a plan, and a plan from them: the same bytes from every toolchain (integers
// and IEEE bits, little-endian), closed by a hash of the whole. `read_plan` refuses a file that is
// short, of another version, or whose arrays disagree with one another.
std::string plan_bytes(const ChannelPlan& plan);
bool read_plan(std::string_view bytes, ChannelPlan& out, std::string& error);
// How many times this process has run the planner (`plan_channels`): what a test holds a cached
// load to.
u64 plans_made() noexcept;

// The reach of a fall per metre by the layer's bounds (Montgomery & Buffington 1997); `colluvial`
// picks colluvial over bedrock above `slope_steep`.
ChannelReach reach_of(const ChannelLayer& layer, f64 slope, bool colluvial);

// **The description the water draws** (`engine.scene.WaterChannel` version 1, declared by the water
// capability): one object per link whose last point drains at least `min_area_m2`, in link order,
// each `{name, points, width_m, depth_m, slope, seed}` — `points` the line as `worldpos`, y the
// water's surface at `stage` of the bankfull depth over the bed, `depth_m` that water's depth, the
// bed below the surface by it — as a JSON array. `cut` (W89): the bed is the cut's
// (`terrain_cuts_json`) and the stage a share of its thalweg depth, so the surface lies inside the
// banks the cut makes; without it the surface stands on the ground as W86 laid it. `ground`
// (nullable; W91, channels.md "Invariants"): the ribbon is fitted to it — an edge over lower
// ground is drawn in to where the ground meets the surface, the point moved and its width
// narrowed to the two edges, or where the bed itself stands over the downhill ground the surface
// lowered under it — and each block carries `fit`, one code a point (0 as planned, 1 narrowed, 2
// lowered); `water` (nullable) the bodies whose points are left as they are, and which **cover** a
// ribbon's ends: a run of points at either end of a ribbon lying inside a body's outline with its
// surface under the body's level (or under an open body's level anywhere) is trimmed off, since the
// body's own water stands over it (W91 continuation, channels.md "Mouths"); a ribbon left with
// fewer than two points is not written. `trim` (nullable) counts what that took. A layer whose
// `bank_width_widths` or `bank_width_m` is not the default writes both on every block (T78), which
// is how the placements' exclusion reads the bank's reach; at the defaults neither is written.
struct RibbonTrim {
  u64 points = 0;      // points trimmed off the ribbons' ends
  u64 blocks = 0;      // ribbons that lost any
  u64 dropped = 0;     // ribbons left with fewer than two points, so not written
  u64 mouth_fans = 0;  // mouths whose trailing (fan) points were trimmed
  // Each trimmed point (x, the water's surface there, z), for the caller to name the body.
  Vector<WorldPos> at;
};
JsonValue water_channels_json(const ChannelLayer& layer, const ChannelNetwork& network,
                              f64 min_area_m2, f32 stage, bool cut = false,
                              const scene_gen::Ground* ground = nullptr,
                              const scene_gen::Exclusion* water = nullptr,
                              RibbonTrim* trim = nullptr);

// **The cuts** (`engine.scene.TerrainCut`, the `tiles` block's `cuts`; channels.md, "The cut";
// W89): one object per link of the network (every link, the rills' too), in link order, each
// `{name, points, width_m, depth_m, bank_slope}` — `points` the line with y the top of the bank
// (the ground under the line), `width_m` the bankfull width, `depth_m` the bed below the top: the
// section's thalweg (`bank_slope`'s trapezoid of the hydraulic geometry's area), the bed held
// down the line, never more than two thalwegs under the ground at it — as a JSON array.
JsonValue terrain_cuts_json(const ChannelLayer& layer, const ChannelNetwork& network);

// **The bed's material** (channels.md, "The flood bed"; W93): each stretch of bed — the cut between
// its bank tops and its flood bed — as an `engine.scene.TerrainMaskRegion` naming the layer's
// `bed_material`, in link order: what the terrain's `masks.regions` take so the bed is drawn with
// the ground's own material and not a mesh laid on it. Empty for a layer that names no material.
JsonValue bed_regions_json(const ChannelLayer& layer, const ChannelNetwork& network);

// **What the water's description draws, measured** (W91; channels.md, "Against the proof of
// concept"): over `WaterChannel` blocks as `water_channels_json` writes them, against the ground
// (the ground before any cut: a cut only lowers it, so a ribbon edge over it is over the cut ground
// too). A stretch is a fall as the water draws one (water.md, "Channels": its surface drops at
// least its horizontal run, 45 degrees); a fall is a run of such stretches. A ribbon edge is a
// point's surface carried `k_ribbon_half_widths` of its width across the line each way, across the
// mean of its two stretches' headings as the water's mesher lays a section; a point under a water
// body (`water`, nullable) is left out, since the body stands over it.
inline constexpr f64 k_ribbon_half_widths = .75;  // half the width and the water's 0.25 past it
struct WaterStats {
  u64 points = 0;
  u64 stretches = 0;
  u64 stretches_by_reach[7] = {};
  u64 fall_stretches = 0;
  u64 fall_stretches_by_reach[7] = {};  // by the reach of the stretch's upstream point
  u64 falls = 0;                        // runs of fall stretches
  f64 highest_fall_m = 0;               // the largest single stretch's drop
  u64 edges = 0;                        // ribbon edges measured (two a dry point)
  u64 edges_above_005 = 0;              // standing more than 5 cm over the ground beside them
  u64 edges_above_03 = 0;               // more than 0.3 m
  f64 edge_max_m = 0;                   // the most, and where
  WorldPos edge_max_at{};
  std::string edge_max_block;
  // Within `border_m` of the region's edge, where the routing's outlet is the region's border and
  // a line may run along it: counted apart, with the blocks they belong to.
  u64 border_edges_above_005 = 0;
  f64 border_edge_max_m = 0;
  Vector<std::string> border_blocks;
  // `depth_m` entries that are not positive, which the water capability refuses (W91
  // continuation: v36's link 690 wrote five at its fan), and the first block holding one.
  u64 depths_not_positive = 0;
  std::string depth_block;
};
// `region` (nullable): min x, min z, max x, max z of the layer's region.
WaterStats measure_water(const JsonValue& blocks, const scene_gen::Ground& ground,
                         const scene_gen::Exclusion* water = nullptr, const f64* region = nullptr,
                         f64 border_m = 0);

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
