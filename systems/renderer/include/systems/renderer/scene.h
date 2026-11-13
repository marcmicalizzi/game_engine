#pragma once

// The scene, before the GPU (docs/subsystems/renderer.md). `SceneDesc` is what a caller asks
// for — mesh files and instances of them — and `SceneData` is what loading one produces: one
// merged cluster LOD DAG, one `geometry::ClusterMeshPart` per mesh, the materials and images
// each mesh named, the instance table in GPU shape, and the bounding sphere the camera frames.
//
// Loading is where the derived-data cache lives (AGENTS.md, [geometry](geometry.md)): a
// `.clusters` container named outright is read as it is, a glTF is looked up in
// `<ddc>/clusters/<key>.clusters` and built into it on a miss, and the container either app
// writes is the one both read. The cost of a load is the import and the clustering, which is
// why engine-host keeps a `SceneData` across calls and rebuilds only the GPU side when the
// settings change.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/gfx/water_spectrum.h>
#include <domain/texture/texture_file.h>
#include <systems/renderer/far_forest.h>
#include <systems/renderer/fire.h>
#include <systems/renderer/terrain.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::renderer {

// The optional `"animation"` block of a scene file's instance, **carried and never interpreted**.
//
// Playing a clip belongs to `systems/animation`, which this module must not depend on — a
// renderer that knew about clips would put the tick inside the frame, and the whole point of the
// contract is that it is outside it. But a scene file is one file, and making the host parse it a
// second time to find the animation blocks would be two parsers over one grammar. So the renderer
// reads the block into these four fields and hands them to whoever asked for the scene; only the
// app resolves `clip` against an `animation::Library`.
struct SceneAnimation {
  std::string clip;   // the clip's name; empty means the skin's first clip
  f32 speed = 1.0f;   // playback rate
  f32 phase = 0.0f;   // seconds added to the starting playhead, so a crowd is not in lockstep
  bool play = false;  // the block was present at all
};

// One instance of a scene as a file, a grid, or a caller describes it.
//
// **A skinned instance is one that names a joint count.** `joints` is how many bone matrices the
// caller will hand this instance every frame (`FrameDesc::joints`, `InstanceJoints`), and it is
// what makes the instance read the deformed-vertex pool in skinning mode; zero is a rigid
// instance and is what every caller before skinning got. `bounds_padding` is the caller's bound,
// in the mesh's own space, on how far skinning can move a vertex from its rest position — see
// `gfx::InstanceDesc::bounds_padding` and docs/subsystems/renderer.md, "Skinned instances", for
// why the renderer takes a number rather than computing one (it has no clips and no skeleton).
struct SceneInstance {
  u32 mesh = 0;
  Transform3 transform;
  u32 joints = 0;             // bone matrices this instance is skinned by; 0 is rigid
  f32 bounds_padding = 0.0f;  // mesh-space slack on every sphere the cull pass tests it by
  // Braced, like every other member's default above it: a member of class type with no
  // initializer of its own makes every aggregate initialization that stops short of it a
  // -Wmissing-field-initializers error on Clang, and `SceneInstance{mesh, transform}` is the
  // spelling three call sites use.
  SceneAnimation animation{};  // the scene file's block, for the app; the renderer never reads it
  // **Where `transform` is measured from** (ADR-0053): the instance stands at
  // `origin + transform.position`, added in f64, so an instance can be placed anywhere in the world
  // while its rotation, scale and float32 offset stay local. **A producer with a world position
  // puts it here, whole, and leaves `transform.position` zero**: the scene file's reader does (its
  // `translation` is a `worldpos`, Instance version 3), and so does a placement generator's
  // placement (`expand_placements`, from `scene_gen::Placement::position`; 2026-10-06), and a
  // scene file's scatter (its centre a `worldpos` and each instance's offset from it added in f64,
  // Scatter version 2; 2026-10-07), so an instance reaches its cell without a float on the way.
  // Zero - the world's origin, the frame every producer used until world positions were f64 - only
  // for what still hands over a float32 world position in `transform.position`: `grid_instances`,
  // a few metres round the origin by construction.
  WorldPos origin{};
  // **Where a swaying instance's phase comes from** (renderer.md, "Streamed trees sway"): the
  // number its seed is hashed from, `key * 0x9e3779b9 + 0x632be5ab`. `k_motion_key_load` is its
  // place among the loaded instances, which is what every instance of a scene read whole has
  // always used; a streamed read gives an instance the place the whole read would have given it,
  // so the two draw the same sway (a kept landmark's by `load_scene`, a streamed tree's from its
  // `authored` id through `StreamedPlacements::motion_key_base`). A tail instance with neither
  // hashes where it stands, in millimetres: a function of the instance, never of when it came.
  u32 motion_key = ~0u;
  // **The hand-off role** (G136; renderer.md, "The hand-off by distance"):
  // `scene_gen::k_handoff_near` or `k_handoff_far` for a patch placed as both its forms, which the
  // renderer gives the band `renderer.flora.sward_near_m`/`sward_fade_m` say; 0, everything else.
  u8 handoff = 0;
};

inline constexpr u32 k_motion_key_load = ~0u;

// The world position `source` stands at once a fit's translation has gone through its rotation and
// scale: `origin + transform.position + (R S)(fit's translation)`, in f64. What `load_scene` and
// `make_instance` place an instance at.
WorldPos instance_translation(const SceneInstance& source, const Mat4& fit) noexcept;

// Where one instance's bone matrices are in the frame's one contiguous span. Parallel to the
// scene's instances, and the whole of the renderer's side of the animation contract: the caller
// hands over one span and one of these per instance, and the renderer does the rest
// (docs/subsystems/animation.md, "The renderer contract").
struct InstanceJoints {
  u32 first = 0;  // index into FrameDesc::joints
  u32 count = 0;  // 0: this instance has no matrices this tick and draws its rest pose
};

// Which mesh an empty path builds — the scenes the engine can render with **no content at all**,
// which is what the renderer's own tests and a bring-up on a new machine start from. `heightfield`
// is the dunes-and-ridges grid engine-view has always drawn with no content at all.
// `shredded_atlas` is the other kind of scene the engine needs to be able to draw from nothing:
// `geometry::build_shredded_atlas_torus` with its probe texture, a mesh whose UV atlas is thousands
// of small islands, which is what an AI mesh generator and a photogrammetry scan produce and what
// broke LOD simplification until 2026-09-19 ([geometry](geometry.md), "The shredded atlas"). It is
// a procedural scene rather than a committed asset so that the corpus can guard the defect on a
// fresh clone and without a third-party model.
enum class Procedural : u8 { heightfield, shredded_atlas };

// How a mesh is scaled and placed in its own space before any instance transform
// (`engine.scene.Fit`). A fitted mesh occupies the same volume whichever file stands in for it —
// which is what lets a Khronos placeholder and the owner's 2-million-triangle landmark share one
// set of placements and one camera path, and the occlusion a path was laid out around hold for
// both.
struct MeshFit {
  f32 height = 0.0f;   // scale so the bounding box is this tall; 0 is no height fit
  f32 extent = 0.0f;   // or so its longest side is this long, when `height` is 0
  bool ground = true;  // bottom centre of the box to the origin; false centres the box there
  bool active() const noexcept { return height > 0.0f || extent > 0.0f; }
};

// **A water body's mesh** (docs/subsystems/water.md; renderer.md, "Materials"): the scene file's
// block as it gave it, for a host that wades in it (engine-view's walker asks the water capability
// for the depth at its feet from these), and the shading numbers the reader resolved from it — the
// colours with their defaults and the waves' direction, the ground's wind where the block follows
// it — which the GPU scene packs into the mesh's one material with `gfx::k_material_water`
// (`gfx::set_water_material`). A water mesh is not a collision surface: the walker stands on the
// ground under it (scene_collision leaves out every mesh that has this).
//
// `depth` is the body's **depth map** (docs/subsystems/water.md, "The depth map"), read from the
// file beside the mesh (`<stem>.depth` for `<stem>.glb`; the water capability's
// `depth_map_bytes`, whose layout the reader mirrors): the depth on a square grid over the body,
// texel (i, j) at body-local (origin.x + i texel_m, origin.y + j texel_m) and
// `texels[j * size + i] / 65535 * (depth_max + bank_max) - bank_max` metres deep — signed since
// version 2 (S94): the bank above the level is a negative depth, down to `-bank_max`; a version-1
// file reads with `bank_max` 0. The GPU scene uploads it for the water
// branch, which shades the bottom from it rather than from the mesh's vertex depths. `size` 0: no
// map beside the mesh, and the branch reads the vertex depths as it did before the map.
struct SceneWaterDepth {
  u32 size = 0;
  Vec2 origin{};
  f32 texel_m = 0.0f;
  f32 depth_max = 0.0f;
  f32 bank_max = 0.0f;
  // What the body's mesh resolves (the file's version 3; `water::DepthMap`): its vertices' spacing
  // along a ring and far from the outline, its rings' spacing at the outline, and the spacing's
  // growth per metre in from it. A file older than version 3: four texels, four texels and 0.
  f32 mesh_far_m = 0.0f;
  f32 mesh_near_m = 0.0f;
  f32 mesh_grade = 0.0f;
  Vector<u16> texels;
};
struct SceneWater {
  scene::WaterBody body;
  gfx::WaterShading shading;
  SceneWaterDepth depth{};
  // The wind the body's waves were made at, m/s: an open body's is the ground's where the body
  // follows it, else its own `wind_m_s`; a closed body's is its own `wind_m_s` (S94, S100).
  f32 wind_m_s = 0.0f;
  // The fetch that wind's sea is grown over, metres (S100 stage B; water.md, "The spectrum"): an
  // open body's `fetch_km`; a closed body's the longest reach of its own water along its wind
  // (`gfx::water_fetch_field` over its depth map), no longer than `fetch_km`.
  f32 fetch_m = 0.0f;
  // The wind's waves from the spectrum (gfx::water_spectrum; water.md, "The spectrum"), made at
  // load from the wind and the fetch: `gfx::k_water_waves` of them for every body with a wind, or
  // none, and then the material's four sines.
  Vector<gfx::WaterWave> waves;
  // A channel of flowing water's (`engine.scene.WaterChannel`; water.md, "Channels"; S96): the
  // mesh's UV.x is the flow phase, and the GPU scene sets `gfx::k_material_water_channel` on its
  // material. `body` then holds the channel's name and optics under a body's names, and no shape.
  bool channel = false;
  // A body's **plunges** (water.md, "The plunge"; S98): the feet of the scene's channels' falls
  // that land in it — inside its depth map, within a metre of its level — body-local,
  // the highest falls first, at most `gfx::k_water_plunges` (the reader drops the rest and says
  // so). The GPU scene packs them after the body's material (`gfx::set_water_plunges`).
  Vector<gfx::WaterPlunge> plunges;
  // A channel's **banks** (water.md, "Channels"; S99): one `gfx::WaterShore` stretch record a
  // stretch, local to the channel's instance, its fringe by the bank's material and its spray band
  // by the stretch's regime (`channel_bank_records`). The GPU scene sets each record's instance and
  // terrain mesh and uploads them beside the bodies' shores. Empty for a body.
  Vector<gfx::WaterShore> banks;
};

// What a scene file said about one mesh besides its path: what to call it, which bytes it must
// be, where it came from, and how to fit it. Parallel to `SceneDesc::meshes`, or empty.
struct SceneMeshInfo {
  std::string name;
  u64 hash = 0;  // `assets::source_mesh_hash` the file must have; 0 is unchecked
  // "scene", "overlay" (a manifest's replacement), "prop", "terrain", or "water"
  const char* origin = "scene";
  MeshFit fit;
  // A water body's (`SceneWater`): set for a mesh the scene's `water` block made, and only then.
  std::optional<SceneWater> water{};
};

// A scene streamed tile by tile (schema `engine.scene.WorldRings`), **carried and never
// interpreted**, like `SceneAnimation`: the tile ring is the world capability's
// (docs/subsystems/world.md), which the renderer must not depend on, and the scene file is one file
// read by one parser. What the renderer does with it is only what its reader does differently —
// the placement entries become `StreamedPlacements` instead of instances — and what
// `SceneData::dynamic` says: that instances will come and go between frames
// (`GpuScene::set_dynamic_instances`).
struct WorldDesc {
  bool enabled = false;
  f32 tile_size = 32.0f;
  u32 ring_count = 0;  // 0: the capability's defaults
  f32 radius[7] = {};  // tiles, innermost first
  u8 ruins[7] = {};    // per ring: 0 blocks, 1 sections, 2 walls (`engine.scene.RingRuins`)
  f32 hysteresis = 0.15f;
  u32 simulated = 2;  // rings, innermost first, the simulation runs in
  // The terrain drawn from the world's tiles (`engine.scene.WorldRings.ground`; renderer.md, "The
  // ground from the world's tiles"), and each ring's cells a side (0: the rule). A scene asking for
  // it with no streamed placement entry is not dynamic: its instances do not come and go.
  bool ground = false;
  u32 ground_cells[7] = {};
};

// One placement entry of a streamed scene (a `ruins` entry, or one of `placements`), as the reader
// left it: the meshes its generator holds resident are in the scene (so the renderer has them
// before a tile asks), and none of its instances are. Everything the world's placements consumer
// needs to open the generator again and ask it for a tile exactly as the whole read would have
// expanded that tile (docs/subsystems/scene_gen.md) — the generator's name, the entry's
// parameters, what its paths resolve against — and which scene mesh each of the generator's
// resident meshes is.
struct StreamedPlacements {
  std::string generator;  // the placement generator's registered name
  JsonValue params;       // the entry's parameters, as the file gave them
  std::string dir;        // the scene file's directory
  std::string where;      // "scene.json: ruins 0", for a sentence
  // The generator's resident meshes (`scene_gen::PlacementGeneratorDesc::meshes`), in its order, as
  // scene meshes: a tile's placement of mesh `m` is an instance of scene mesh `meshes[m]`. A
  // placement of mesh `meshes.size() + k` names the k-th mesh its tile handed
  // (`scene_gen::Placements::meshes`), which is resident while a tile held names it and not before
  // (world.md, "A tile's meshes: residency by reference").
  Vector<u32> meshes;
  // The generator's `tile_meshes`: its tiles hand meshes, so the scene makes room for them.
  bool tile_meshes = false;
  bool tile_meshes_masked = false;  // and they may be alpha-tested (`tile_meshes_masked`)
  // The derived-data root the reader opened the entry with (`scene_gen::host_derived_root`, T119):
  // the host's, else the scene's own. Every consumer that opens the entry again (the world's
  // placements consumer, the collision) hands this one, so its plans and recipes are the whole
  // read's, never a root found from where the process was started.
  std::string ddc;
  // For an entry whose placement ids name the whole read's instances (the `authored` entry's: its
  // place in the file's own list plus one), the loaded index of id 1: a placement of id `n` is
  // given `motion_key = motion_key_base + n - 1`, the key the whole read gives it. `~0u`: none.
  u32 motion_key_base = ~0u;
  // Finite entries can size the consumer's per-tile storage at load. The authored reader knows
  // the exact count in every tile; a distance form changes the mesh, never this count. Empty for
  // generators without a finite bound (their tile storage is pooled at its high-water mark).
  struct TileInstances {
    i32 x = 0;
    i32 z = 0;
    u32 count = 0;
  };
  Vector<TileInstances> tile_instances;
};

// A prop of a scene file (`engine.scene.Prop`): a mesh the load places nowhere, kept for a game
// module to place instances of between frames (docs/subsystems/apps.md, "The game contract"). The
// renderer's rule for instances that come and go is that the meshes stay and only the instances
// change, so a module's objects have to be meshes the scene loaded; this is how a scene says which.
// `mesh` is the scene mesh index the prop's file became.
struct SceneProp {
  std::string name;
  u32 mesh = 0;
  // The prop's `fire` (fire.h): every placed instance of it carries this fire, its position from
  // the instance's origin. Absent: none.
  std::optional<FireDesc> fire{};
};

// **A streamed world's own instances, a tile at a time** (renderer.md, "Authored instances per
// tile"; docs/subsystems/authored.md). When a world streams and the scene-generator registry has
// the placement generator "authored", `load_scene` hands the file's own instances over as that
// generator's entry — every rigid one whose mesh reaches no farther than `max_reach_m` from its
// origin — and gives each ring past the first a **distance form** of each such mesh: what it can
// draw from the nearest a camera can be to an origin on a tile of that ring
// (`geometry::extract_lod_form`), at this projection and these thresholds. The rest stay in the
// prefix: a skinned or morphed mesh, an instance with an animation block, a mesh reaching farther
// (the landmarks), and everything when the scene is paged.
// The world capability's default rings (`world::RingParams`; schema `engine.scene.WorldRings`:
// 1.5 tiles, 8 and 24), which a scene whose `world` block names none streams with. Mirrored here
// because the renderer may not depend on the capability; the world's placement tests hold the two
// equal.
inline constexpr u32 k_default_world_ring_count = 3;
inline constexpr f32 k_default_world_ring_radius[7] = {1.5f, 8.0f, 24.0f, 0.0f, 0.0f, 0.0f, 0.0f};
// The projection a form is cut for when the host names none: the default camera's (55 degrees) at
// 1080p, cot(27.5 degrees) * 540.
inline constexpr f32 k_default_form_proj_scale = 1037.33f;
struct AuthoredStreaming {
  bool enabled = true;
  // The projection the forms are cut for (`LodView::proj_scale`: cot(fov_y / 2) * height / 2). 0
  // is the default camera's at 1080p. A view with a larger one draws a form's frontier where the
  // full mesh would draw finer (geometry.md, "Distance forms").
  f32 proj_scale = 0.0f;
  f32 threshold_px = 1.0f;  // `RenderSettings::lod_threshold`
  f32 foliage_px = geometry::k_default_foliage_px;
  f32 fade = 0.1f;         // `RenderSettings::lod_fade`
  f32 max_reach_m = 0.0f;  // 0: the world's tile edge
  // **Which forms sway** (renderer.md, "Streamed trees sway"): a ring's form of a mesh with the
  // motion contract sways when the nearest an origin on that ring can be is under this many
  // metres, and is cut for the sway's padded spheres, so the cull's choice over it is the full
  // mesh's swaying; past it the form is cut and drawn rigid. Negative: `renderer.flora.sway_m`;
  // 0: every form rigid (`authored_rules` sets 0 when the trees do not sway at all).
  f32 sway_m = -1.0f;
  // **The instances a host moves — a game's actors — which stay in the prefix** (renderer.md,
  // "An actor is never streamed"), by their index in the whole read (`SceneDesc::instances`), in
  // any order. A game moves, plays and asks after an actor every tick, so its entity, its joints
  // and its row of the instance table must exist from the first frame to the last; a streamed
  // placement exists only while the ring holds its tile. The declarations are the scene file's and
  // are known before the load (engine-view's `keep_game_actors`).
  Vector<u32> keep;
  // **The far forest** (renderer.md, "The far forest"; far_forest.h): the streamed instances'
  // coarsest forms merged per world tile into prefix instances the renderer draws wherever the
  // ring's tail does not. Off for a host that does not ask (`RenderSettings::far_forest`, which
  // `authored_rules` copies here; engine-view's `--far-forest`, on by default).
  bool far_forest = false;
};

// The rules a host's session cuts its forms for: its projection (the views' height and vertical
// field of view; a surround's views are each as tall as the target) and its LOD thresholds, the
// foliage threshold and the fade resolved from their tunables as `resolve_settings` resolves them.
struct RenderSettings;
void authored_rules(const RenderSettings& settings, u32 height, f32 fov_y,
                    AuthoredStreaming& out) noexcept;

// What `load_scene` did with a streamed world's own instances, for a host's summary.
struct AuthoredStats {
  u32 streamed = 0;  // handed to the "authored" entry
  // Kept in the prefix: a host's actor (`AuthoredStreaming::keep`), deformed (skinned or morphed
  // mesh, or an animation block), reaching farther than `max_reach_m`, or every one because the
  // scene is paged. An actor is counted as an actor whatever else it is.
  u32 kept_actors = 0;
  u32 kept_deformed = 0;
  u32 kept_reach = 0;
  u32 kept_paged = 0;
  u32 forms = 0;          // distance-form meshes made
  u32 form_clusters = 0;  // their clusters, all rings
  // Their motion parts before and after the pruning to the parts their vertices use (R95).
  u64 form_parts_before = 0;
  u64 form_parts_after = 0;
  // The pairs the streamed instances would hold whole, innermost form; and the largest one tile
  // holds in it, which is the least a tail must have room for (`SceneData::tail_floor_pairs`).
  u64 whole_pairs = 0;
};

// What to load: mesh files and instances of them, or one of the procedural scenes above.
// A mesh handed as its recipe (`scene_gen::PlacementMesh::recipe`; scene_gen.md, "A tile's
// meshes"): the bytes the mesh generator's file at `path` would hold. The load reads these instead
// of the file, so a solver's output is never written to be read back.
struct MeshRecipe {
  std::string path;
  std::string bytes;
};

struct SceneDesc {
  Vector<std::string> meshes;  // glTF, GLB, or .clusters; one empty path = procedural
  // The recipes of the meshes above that were handed and not written (`MeshRecipe`), by path. A
  // whole read's placement entries fill it; empty for every scene whose meshes are all files.
  Vector<MeshRecipe> recipes;
  Vector<SceneInstance> instances;  // empty: one identity instance of every mesh
  u32 grid_instances = 0;           // n: n x n copies of mesh 0, varied in rotation and scale
  Procedural procedural = Procedural::heightfield;  // which mesh an empty path builds
  // A still authored terrain drawn from tile levels needs only a small material/UV carrier,
  // independent of its heightfield's extent and resolution (ADR-0057).
  bool terrain_tiles = false;
  // What the LOD builder is told about every mesh of this scene: the per-cluster limits, the
  // attribute weights, and the seam rules (docs/subsystems/geometry.md, "What the simplifier is
  // given, and why"). It is **part of the derived-data cache key**, so a scene built with
  // non-default options addresses its own container and `engine-content build` with the matching
  // flags writes exactly that one — which is the property that makes a before-and-after
  // comparison a flag rather than two builds of the tree, and the reason this is one field for
  // every mesh instead of a special case for the procedural fixture.
  geometry::ClusterLodOptions lod;
  u32 heightfield_grid = 257;  // vertices a side, when a mesh path is empty
  std::string ddc;             // derived-data root; empty and `cache` looks one up
  bool cache = true;           // read and write <ddc>/clusters/<key>.clusters
  // The material pack a generated mesh's parts fall back to (docs/subsystems/materials.md): a
  // directory holding `pack.json`. Empty and the repository's own, `<repo>/content/materials`, is
  // looked up (`materials::default_pack_dir`) whatever `ddc` and `cache` say — except in a test or
  // a bench, whose main switches that lookup off for itself and every process it starts, so a test
  // that wants a pack names one (T103). `SceneLoadReport::materials` says which was used.
  std::string materials;
  // The longest bone-matrix span the caller will hand a frame, which is what the per-frame joint
  // buffer is sized by. It is the caller's number because it comes from the animation capability's
  // pose pool, which the renderer does not know about; a frame handing over more than this is
  // clamped with one warning rather than overrunning the buffer. Zero means nothing is skinned.
  u32 max_joints = 0;
  // The joint count each copy of a `grid_instances` grid gets, since the grid is expanded here
  // rather than by the caller. The padding is not here: it is a length in the mesh's own space,
  // which nobody knows until the mesh is loaded, so a host sets it afterwards and calls
  // `update_scene_bounds`.
  u32 grid_joints = 0;
  // Keep the streaming page table (04 §4.3 step 3). A glTF and a `.clusters` container are laid
  // out in pages either way — the layout is part of the derived-data cache key — but the table
  // itself is dropped unless someone is going to stream from it, and the procedural heightfield is
  // not paged at all unless this is set. It is here rather than in `RenderSettings` because it
  // changes what *loading* produces, and a `SceneData` outlives a settings change in engine-host.
  bool stream = false;
  // The page byte target, for the **procedural heightfield only**. A glTF's and a container's
  // layout is fixed at `geometry::ClusterPagesOptions{}.page_bytes`, because the target is part of
  // `geometry::cluster_cache_key` and two targets would be two different containers; the
  // heightfield has no cache entry, so a caller that wants many small pages — a test, or a
  // measurement of what a budget does — may say so. Zero is the default target.
  u32 page_bytes = 0;
  // The workers the load's meshes and textures run over (renderer.md, "What the load reads"): 0
  // one per performance CPU, as `engine-content --jobs 0`. The result does not depend on it — a
  // test loads a scene at 1 and at 0 and requires the same tables.
  u32 load_jobs = 0;
  // ---- what a scene file adds (schema `engine.scene.Scene`) ------------------------------------
  // Per mesh: a name, the hash its bytes must have, and a fit. Empty, or parallel to `meshes`.
  Vector<SceneMeshInfo> mesh_info;
  // The procedural terrain. When `enabled`, an empty mesh path builds it rather than the classic
  // heightfield, and it is cached in the derived-data root under a key over its fields.
  TerrainDesc terrain;
  std::string name;   // the scene file's own name
  u64 file_hash = 0;  // of the scene file's bytes; 0 for a scene no file described
  // The camera path the scene file names (`Scene.camera_path`), resolved against the file's own
  // directory; empty when it names none. Nothing here flies it: it is where a host that has no
  // path of its own starts a camera — engine-view's `--interactive` — read with `read_camera_path`
  // once the terrain it may stand on is known.
  std::string camera_path;
  // What the file's placement entries — its `ruins` and its `placements` — expanded into
  // `instances` (docs/subsystems/scene_gen.md): the things their generators made (buildings) and
  // the instances, for a host's summary. Zero for a scene with none.
  u32 placed_buildings = 0;
  u32 placed_instances = 0;
  // **The file's instances that do not stand on the ground** (T74; renderer.md, "Instances off the
  // ground"): those with `ground: false` whose base `y` is more than `k_off_ground_m` above
  // (`floating`) or below (`buried`) the terrain's surface at their x and z, counted at the read
  // with the largest offset of each (metres, 0 when none) and the first one's index. A count, not a
  // refusal: the author may mean it. Zero for a scene with no terrain.
  u32 instances_floating = 0;
  u32 instances_buried = 0;
  f64 floating_max_m = 0.0;
  f64 buried_max_m = 0.0;
  u32 floating_first = 0;
  u32 buried_first = 0;
  // A streamed world (`engine.scene.WorldRings`): enabled by the file's `world` block or by
  // `SceneFileOptions::world`, and then every placement entry is in `streamed` and none of its
  // instances in `instances`.
  WorldDesc world;
  Vector<StreamedPlacements> streamed;
  // The file's own `instances` and its `scatters`' expansions:
  // `instances[authored_first, authored_first + authored_count)`, the run a streamed world may hand
  // to the "authored" generator (`authored`).
  u32 authored_first = 0;
  u32 authored_count = 0;
  AuthoredStreaming authored;
  // The scene's sky (`engine.scene.Sky`; sky.h, ADR-0048): the entry as the file gave it, its
  // provider found in the registry when the file was read. Empty: the stand-in sky and sun, exactly
  // as before the field existed.
  std::optional<scene::Sky> sky;
  // The scene's participating medium (`engine.scene.Medium`, the volumetrics capability's type):
  // the block as the file gave it, read by nothing here. Null: no medium.
  JsonValue medium;
  // The file's `props` (`engine.scene.Prop`): meshes appended after the file's own, in the file's
  // order, with no instance of any of them. Empty for a scene with none.
  Vector<SceneProp> props;
  // The file's `fires` (`engine.scene.Fire`, fire.h), at their world positions. Empty: none.
  Vector<FireDesc> fires;
};

// What `read_scene_file` may be told besides the path.
struct SceneFileOptions {
  // A landmark overlay manifest (`engine.scene.Overlay`): a local file mapping content hashes to
  // paths. A mesh whose `overlay` hash the manifest names is drawn from the manifest's file
  // instead, which the load then hashes and refuses if the bytes disagree. Empty: no overlay.
  std::string overlay;
  // Stream the scene through a world's tile ring even when the file has no `world` block (the
  // block's defaults then): engine-view `--world`. The same scene read without it is today's
  // whole read, which is what E35 compares a streamed world against.
  bool world = false;
  bool terrain_tiles = false;  // the request's terrain layout, before building a scene grid
  // The game time the ground stands at, in place of the file's `Terrain.time`: engine-view
  // `--ground-time`, `render.load`'s `ground_time_s` (request.h). The terrain mesh is built at it,
  // anything placed on the ground stands on it there, and a sky's one clock starts from it. A
  // file with no terrain is refused. Unset: the file's own time.
  std::optional<f64> ground_time_s;
  // In a streamed world, hand the file's own instances to the "authored" generator a tile at a
  // time (engine-view `--world-instances tiles`, the default) or keep them in the scene's prefix
  // (`whole`): `SceneDesc::authored.enabled`.
  bool world_instances = true;
};

// Reads a scene file (schema `engine.scene.Scene`): meshes with an optional name, content hash,
// overlay hash and fit; instances with a translation, rotation, yaw, scale and an optional height
// above the terrain; seeded scatters of instances; placement entries — ruined buildings over the
// terrain's tiles (`ruins`) and any generator by name (`placements`) — expanded through the
// scene-generator registry (docs/subsystems/scene_gen.md): ground first, then each entry in the
// file's order (the `ruins` before the `placements`), standing on the terrain, its meshes after the
// file's own and each generator's after the ones before it, and a name this executable does not
// carry refused with a sentence; and an optional terrain drawn by the ground provider it names. The
// oldest spelling —
// `{"meshes":[{"path":"..."}],"instances":[{"mesh":0,"translation":[x,y,z],"rotation":[x,y,z,w],
// "scale":[x,y,z]}]}` — is a valid scene of the same type and reads exactly as it always did.
// Mesh paths resolve against the file's own directory, so a scene file is movable as a unit; an
// empty instance list gives one identity instance of every mesh. The terrain, when present, is
// the last mesh and has one identity instance, after the file's own.
bool read_scene_file(const std::string& path, SceneDesc& out, std::string& error);
bool read_scene_file(const std::string& path, const SceneFileOptions& options, SceneDesc& out,
                     std::string& error);

// The names of a scene file's `props`, in the file's order, and nothing else of the file read:
// one schema read of the file (the same grammar as `read_scene_file`'s), no generator opened and
// no mesh loaded. For a host that has to know a scene's props before it loads the scene —
// engine-view's game runner loads its module, whose `on_load` names props, before any device
// (apps.md, "--game"). `mesh` is left zero: the prop's index in the list is what a module gets.
bool read_scene_props(const std::string& path, Vector<SceneProp>& out, std::string& error);

// A 64-bit content hash as the scene format writes it: 16 lower-case hex digits.
std::string hash_hex(u64 hash);
bool parse_hash_hex(std::string_view text, u64& out) noexcept;

// One mesh of the scene as it arrived: the materials and images it named, and which primitive —
// and so which material — each of its clusters came from. The DAG itself has been moved into
// the scene's merged mesh by then.
struct SourceMesh {
  assets::MeshData data;  // materials and images; the positions are spent by now
  // The part index of every cluster of this mesh, in the order of its run of the merged cluster
  // array (`parts[m].first_cluster + i`): `load_scene` permutes it with the merge, which puts a
  // mesh's leaves first and so reorders every mesh that arrived in pages (renderer.md, "The
  // material map follows the merge").
  Vector<u32> part_of_cluster;
  // The coverage errors (geometry.md, "Two errors"; container section 39): empty, or one
  // `geometry::pack_lod_coverage` word per cluster, in the same order as `part_of_cluster` and
  // reordered with it. A generated tree with an impostor chain carries them.
  Vector<u32> lod_coverage;
  Vector<i32> part_material;  // the material of every part, -1 for none
  // Parallel to `data.materials`, or empty (every material plain): the container's
  // `ClusterFileMaterialSampling::flags` (`k_material_sampling_stochastic`, R131), which the upload
  // turns into `gfx::k_material_stochastic` (renderer.md, "Stochastic mesh materials"). A glTF has
  // no way to say it.
  Vector<u32> material_sampling_flags;
  std::string image_dir;  // what the relative image paths are relative to
  std::string path;       // the mesh as the scene named it (a form: its source's); for sentences
  u32 primitives = 0;     // glTF primitives merged into it; 0 from a container
  const char* cache = "none";  // "file", "hit", "miss", or "none"
  // The `.clusters` container this mesh's bytes are in: the file a `--mesh x.clusters` named, the
  // derived-data entry a glTF hit, or the one this load just wrote. Empty when there is none —
  // the procedural heightfield, or a glTF loaded with `--no-cache` — and that is exactly the
  // condition under which a streamed page has to come out of host memory instead of off disk
  // (`FilePageSource`, docs/subsystems/renderer.md).
  std::string container;
  // What these bytes are: `assets::source_mesh_hash` of the file for a glTF or GLB, the terrain's
  // `terrain_hash`, zero when nothing computed one (a container named outright, the classic
  // procedural scenes, a glTF loaded with no cache and no expected hash).
  u64 source_hash = 0;
  // Where each image's built texture is (`geometry::ClusterFileTexture`,
  // docs/subsystems/texture.md), parallel to `data.images` or empty: the container's own records,
  // or the ones this load wrote into the cache entry it made — with, after `load_scene`'s texture
  // pass, the key and source hash of every image named by path filled in from the load's one
  // identity of its file, so the upload finds a built texture by key and hashes no source
  // (renderer.md, "What the load reads"). Empty — every image decoded as
  // before — for a glTF loaded with no cache and for a container that records none.
  Vector<geometry::ClusterFileTexture> textures;
  // The derived-data root those textures are looked up under; empty when the load reads no cache
  // (`SceneDesc::cache` off), and then the built textures are not used either.
  std::string texture_ddc;
  // **A generated tree's motion contract** (flora.md, "Motion attributes"; renderer.md, "The flora
  // stage"): the container's sections 35 to 37 as `adopt_container` found them — the parts, one
  // record per vertex of this mesh's run of the merged vertex streams (`parts[m].first_vertex` on),
  // and the structural table — and the species the generator section names. All empty for every
  // mesh a generator did not make, which is what keeps them rigid.
  Vector<geometry::ClusterFileMotionPart> motion_parts;
  Vector<geometry::ClusterFileMotionVertex> motion_vertices;
  Vector<geometry::ClusterFileStructuralPart> structural_parts;
  // **A distance form's parts are the ones its vertices use** (renderer.md, "A form carries only
  // the parts it uses"; R95): `stream_authored` prunes a form's copy of the table to the parts its
  // motion records name and their ancestors, remapping the records, the parents and the structural
  // table's ids, and keeps here each kept part's id in its source, which the sway's phase is taken
  // from so the form sways as the full mesh does. Empty for a mesh that is its own source.
  Vector<u32> motion_part_ids;
  std::string species;
  bool has_motion() const noexcept { return !motion_parts.empty() && !motion_vertices.empty(); }
};

// What `load_scene` read and did to have the scene's meshes and their textures (renderer.md, "What
// the load reads"; roadmap W75): the numbers a test holds a cooked load to — no source image read —
// and engine-view's summary prints as `load`.
struct SceneLoadReport {
  u32 meshes = 0;   // loaded by the per-mesh pass
  u32 threads = 0;  // the job system's workers it ran over, plus the calling thread
  // The material pack directory the load resolved (`SceneDesc::materials`, or the default lookup):
  // empty when there was none, so a run says which pack it drew with (T103).
  std::string materials;
  // The file's `ground: false` instances more than a metre above (`floating`) or below (`buried`)
  // the ground they stand over (`SceneDesc::instances_floating`); only counted, never refused.
  u32 instances_floating = 0;
  u32 instances_buried = 0;
  // The texture pass: images the meshes' materials sample (`references`), the distinct built
  // textures they name (`textures`), of those the ones whose `.tex` was trusted from its header
  // and the ones built now, and the ones neither (the image could not be read or built; drawn
  // through the decode).
  u32 references = 0;
  u32 textures = 0;
  u32 trusted = 0;
  u32 built = 0;
  u32 unbuilt = 0;
  // Source images: answered by a material pack's index without a read, read and hashed (once
  // each), and every open of one this load made with the bytes it returned — zero for a scene
  // whose images are all embedded or a pack's and all built.
  u32 images_from_index = 0;
  u32 images_hashed = 0;
  u32 source_image_reads = 0;
  u64 source_image_bytes = 0;
  // The pass's wall milliseconds: the per-mesh loads, and the texture pass after them.
  f64 meshes_ms = 0.0;
  f64 textures_ms = 0.0;
  // Jobs the load's pool ran on the submitting thread because its queues were full
  // (`jobs::JobSystemStats::inline_runs`): zero, because the load hands the pool one runner per
  // thread and never one job per mesh (renderer.md, "What the load reads"; T73).
  u64 inline_jobs = 0;
};

// **What a mesh built after the load is built with** (R142; renderer.md, "Meshes that come and
// go"): the load's own LOD options, derived-data root and material pack, carried on the loaded
// scene so every host that holds one — the world's placements consumer, the collision — builds a
// tile's mesh into the cache entry the whole read would have, and finds what the other built.
struct PlacementMeshOptions {
  geometry::ClusterLodOptions lod;
  std::string ddc;
  bool cache = true;
  std::string materials;
  // **Its textures too** (R147; texture.md, "Textures that arrive after the load"): the build also
  // runs the load's texture pass for the one mesh — every image its materials sample gets its
  // `.tex` key, trusted from its header or built now by the content build's own step — and reads
  // the ones the scene does not hold, so the admission has their bytes without a read on the
  // frame's thread. Off for a caller that wants the geometry alone (the collision).
  bool textures = false;
  // Whether the scene holds the texture of this build key now, asked from the building thread
  // before a `.tex` is read: true skips the read. Advisory — a texture let go between the build
  // and the admission is read again by the admitter (`read_placement_texture`). Null reads all.
  bool (*texture_resident)(void* context, u64 key) = nullptr;
  void* texture_context = nullptr;
};

// A loaded scene: everything the GPU upload reads, and everything a summary reports.
struct SceneData {
  SceneLoadReport load;
  PlacementMeshOptions mesh_options;  // what a tile's mesh is built with, as the load built its own
  geometry::ClusterLodMesh lod;       // every mesh of the scene, merged
  Vector<geometry::ClusterMeshPart> parts;  // one per mesh: its clusters and its 16-bit grid
  // The scene's one page table, filled only under `SceneDesc::stream`: every mesh's pages, one
  // after another, with `parts[m].first_page`/`page_count` naming each mesh's run
  // (`geometry::merge_paged_cluster_meshes`). Empty means the scene is uploaded whole.
  geometry::ClusterPages pages;
  Vector<SourceMesh> sources;  // parallel to `parts`
  // One per instance, in order of `first_pair`, with `world`, `mesh`, `first_pair`,
  // `scale_max`, and the uniform-scale flag filled in. `material_base` and `deform` are the GPU
  // scene's to fill, because both depend on tables it lays out.
  Vector<gfx::InstanceDesc> instances;
  // Bone matrices per instance, parallel to `instances`, or empty when nothing is skinned. It is
  // the *capacity* an instance was declared with, not what a frame hands over; the GPU scene reads
  // it to decide which instances get a skinning `gfx::DeformDesc`.
  Vector<u32> instance_joints;
  u32 skinned_instances = 0;  // how many entries of `instance_joints` are non-zero
  u32 max_joints = 0;         // SceneDesc::max_joints, carried through for the GPU scene
  WorldPos center{};          // the scene's bounding sphere: what the camera frames
  f32 radius = 10.0f;         // and what every distance, bias, and light reach scales with
  u32 pair_count = 0;         // the prefix sum the cull dispatch covers
  bool heightfield = false;   // the procedural scene: procedural materials and fixed bounds
  u32 mesh_primitives = 0;    // of the first mesh, as the summary reports it
  const char* mesh_cache = "none";
  i64 build_ns = 0;  // import, weld, cluster, page, and merge
  // The ground's albedo, linear: the lower half of the resolve's hemisphere ambient is this ground
  // lit by the frame's sun and sky (`FrameLighting::ground`), so a surface turned towards the
  // ground is lit by it. `load_scene` makes it the terrain's sand when the scene has a terrain
  // (`terrain_sand_albedo`) and leaves the neutral grey otherwise.
  Vec3 ground_albedo{gfx::k_neutral_ground_albedo, gfx::k_neutral_ground_albedo,
                     gfx::k_neutral_ground_albedo};
  // Carried from the `SceneDesc`, for a host that has only the loaded scene: the terrain answers
  // a camera path's heights above the ground, and the rest names what a measurement measured.
  TerrainDesc terrain;
  // Which mesh the terrain built (the empty path that built it), `~0u` without one: what the GPU
  // scene makes a terrain level of when the terrain moves (renderer.md, "The dunes in time-lapse").
  u32 terrain_mesh = ~0u;
  Vector<SceneMeshInfo> mesh_info;
  std::string name;
  u64 file_hash = 0;
  // Each mesh's fit (`MeshFit`) as the matrix every instance of it is placed through, identity for
  // a mesh with none. Kept because an instance added after the load (`make_instance`) is placed the
  // same way, and the float positions the fit was measured from may be gone by then.
  Vector<Mat4> mesh_fit;
  // **Instances come and go between frames.** A streamed world's (`WorldDesc`): `instances` is the
  // fixed prefix the load produced and the GPU scene takes a tail of more after it
  // (`GpuScene::set_dynamic_instances`). Carried with the world description and the streamed
  // placement entries for the capability that streams them.
  bool dynamic = false;
  WorldDesc world;
  Vector<StreamedPlacements> streamed;
  // Some streamed entry's tiles hand meshes (`StreamedPlacements::tile_meshes`): the GPU scene
  // makes the room they arrive into (renderer.md, "Meshes that come and go").
  bool tile_meshes = false;
  // ... and one of them says those meshes may be alpha-tested
  // (`scene_gen::PlacementGeneratorDesc::tile_meshes_masked`): the scene draws through the masked
  // pipelines from its first frame, since they are chosen when it is made (R147).
  bool tile_meshes_masked = false;
  // A streamed world's own instances (`AuthoredStreaming`): what was streamed and what kept, and
  // the most pairs one tile of them holds in its innermost form — the least room a tail must have,
  // which `GpuScene::create` accounts with the prefix and the terrain rings' slots before anything
  // is made (renderer.md, "The pair accounting"). Zero for a scene that streams none.
  AuthoredStats authored;
  u32 tail_floor_pairs = 0;
  // Per mesh, the mesh it is a distance form of, or itself (empty: no forms).
  Vector<u32> form_source;
  // Per mesh, 1 for a distance form cut for a ring past `AuthoredStreaming::sway_m`, which a tail
  // instance draws rigid (empty: none).
  Vector<u8> form_rigid;
  // The far forest the load built (far_forest.h): one mesh and one prefix instance per world tile
  // with a far tree, which `SceneRenderer` hands off with the tail. Empty when the scene streams
  // none or the host did not ask (`AuthoredStreaming::far_forest`).
  FarForestData far_forest;
  // Per loaded instance, the key its sway's seed is hashed from (`SceneInstance::motion_key`, its
  // index when the source named none). Empty: every instance's index.
  Vector<u32> motion_keys;
  // **The whole read's index to the loaded instance** (renderer.md, "The whole read's index"): per
  // instance of the whole read (`SceneDesc::instances`), its index among `instances`, or
  // `k_not_loaded` for one handed to the world's tail. Built once, by a load that streamed; empty
  // for a scene read whole, where the two are the same number. Read through `loaded_instance` and
  // the inverse through `read_instance`, never by hand: a host's handles — a game's actors, a scene
  // file's indices — are the whole read's.
  Vector<u32> loaded_of_read;
  // Carried from the `SceneDesc`: the sky the renderer draws (sky.h), or the stand-in without one.
  std::optional<scene::Sky> sky;
  // Carried from the `SceneDesc`: the medium a frame pass draws (frame_pass.h;
  // systems/volumetrics), uninterpreted. Null: no medium.
  JsonValue medium;
  // Carried from the `SceneDesc`: the props a game module may place (`SceneProp`), each naming its
  // mesh among `parts`. The load placed none of them.
  Vector<SceneProp> props;
  // Carried from the `SceneDesc`: the scene's own fires (fire.h), lit by the renderer's `FireSet`.
  Vector<FireDesc> fires;

  u32 cluster_count() const noexcept { return lod.mesh.clusters.size(); }
  u32 leaf_count() const noexcept {
    return lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0];
  }
  // Whether any instance is skinned, which is what makes the GPU scene allocate the joint buffer
  // and upload the mesh's per-vertex binding stream.
  bool skinned() const noexcept { return skinned_instances > 0 && !lod.mesh.skin.empty(); }
  // A scene with morph channels is deformed whatever the flags say: the chain has to run for its
  // instances, exactly as it does for a skinned one.
  bool morphed() const noexcept { return !lod.mesh.morph_channels.empty(); }
  // Whether the scene can be streamed at all: a page table that covers its clusters.
  bool paged() const noexcept {
    return !pages.pages.empty() && pages.page_of_cluster.size() == cluster_count() &&
           pages.children.size() == cluster_count();
  }
};

// Loads every mesh of `desc`, merges them into one set of buffers, expands the instances, and
// computes the bounds. `error` says which mesh failed and why.
bool load_scene(const SceneDesc& desc, SceneData& out, std::string& error);

// **Between the whole read's instances and the loaded ones** (`SceneData::loaded_of_read`). A
// streamed world keeps a prefix of the scene's instances and hands the rest to the tail, so a
// loaded instance's index is not the file's; these are the one way across. `loaded_instance` is
// `k_not_loaded` for an instance in the tail or past the whole read; `read_instance` is the whole
// read's index of a loaded one (the load gives every kept instance that place as its
// `motion_key`), the index itself for a scene read whole; `read_instance_count` is how many the
// whole read has, the loaded ones and the tail's.
inline constexpr u32 k_not_loaded = ~0u;
u32 loaded_instance(const SceneData& scene, u32 read) noexcept;
u32 read_instance(const SceneData& scene, u32 loaded) noexcept;
u32 read_instance_count(const SceneData& scene) noexcept;

// One instance in GPU shape, placed as `load_scene` places the instances it expands: the mesh's
// fit, then the instance's transform, at `first_pair` in the prefix sum. For an instance added
// after the load. False, with `error`, for a mesh the scene has not or a skinned instance (an
// instance that comes and goes is rigid: its joints would need a deform entry the GPU scene lays
// out once).
bool make_instance(const SceneData& scene, const SceneInstance& source, u32 first_pair,
                   gfx::InstanceDesc& out, std::string* error = nullptr);
// The placement alone, through a fit the caller names: what `make_instance` does once it has
// checked the mesh, and what the GPU scene does for a mesh that arrived after the load (a tile's
// mesh, whose fit is the identity; renderer.md, "Meshes that come and go").
void place_instance(const SceneInstance& source, const Mat4& fit, u32 first_pair,
                    gfx::InstanceDesc& out);
// The record's hand-off role for `SceneInstance::handoff` and the band it carries (G136;
// `gfx::set_instance_handoff`, stamp 0): no role, and a zero band, for an instance with none.
void apply_handoff(gfx::InstanceDesc& out, u8 handoff, u32 band) noexcept;

// Where the prefix sum ends after `instances`, starting at `first_pair`: `first_pair` plus each
// instance's mesh's clusters. False, with `error`, for a mesh the scene has not or a sum past
// `max_pairs` (the visibility id's 2^24, `k_max_pairs`, unless a caller keeps a budget of its own).
// No GPU: what `GpuScene::set_dynamic_instances` checks before it touches one, and what a caller
// can check before it asks.
bool pairs_after(const SceneData& scene, u32 first_pair, std::span<const SceneInstance> instances,
                 u32 max_pairs, u32& end_pair, std::string* error = nullptr);

// ---- a tile's mesh (R142; renderer.md, "Meshes that come and go") -------------------------------
//
// **A mesh that arrives with its tile, built** — everything the GPU scene admits and the collision
// builds a proxy from, and nothing of the device: its own DAG with offsets from zero and its
// clusters in the order a whole read's merge leaves them (leaves first), its materials and the map
// from cluster to material, and its reach on the ground. The same bytes a scene read whole holds
// for the same recipe, since both come from one cache entry.
// One built texture of a tile's mesh (R147): the image of `source.data.images` it is, its `.tex`
// build key — what the scene shares a texture by — and its bytes as the file holds them, or none
// when the scene said it holds the key (`PlacementMeshOptions::texture_resident`).
struct PlacementTexture {
  u32 image = 0;
  u64 key = 0;
  texture::TextureData data;
  // What the device copies for it: every level of the chain, and of the variance chain, each at
  // a 16-byte offset. Zero for one with no bytes.
  u64 staged_bytes() const noexcept;
};

struct PlacementMeshData {
  geometry::ClusterLodMesh lod;
  geometry::ClusterMeshPart part;  // of `lod`: every cluster, from zero
  SourceMesh source;
  f32 reach = 0.0f;  // `ground_radius` of the mesh as built
  // With `PlacementMeshOptions::textures`: one per image a material samples, **all or none** as
  // the load's own rule is (texture.md, "In the renderer") — empty, with `textures_why`, when any
  // of them has no built texture (no cache, an image that cannot be read or built), and the mesh
  // then draws its materials' constants.
  Vector<PlacementTexture> textures;
  std::string textures_why;
  // Host bytes of the streams a device upload copies, for a caller's accounting.
  u64 payload_bytes() const noexcept;
};

// What a host keeps one resident mesh per: over the generator `path` names and the recipe's bytes
// for a mesh handed as its recipe (so two tiles, or two paths, handing one recipe share one mesh),
// over the path and its expected hash for a file.
u64 placement_mesh_identity(const scene_gen::PlacementMesh& mesh) noexcept;

// Builds or finds `mesh` exactly as the load does one of its own (`load_scene`'s per-mesh pass:
// the mesh generator registry for a recipe or a generator's file, the importer for a glTF, a
// container as it is; the derived-data cache under `options`). **No device and no scene**: it may
// run on any thread, and two threads asking for one recipe write the same bytes to the same cache
// entry. False, with a sentence naming the mesh, for a generator this executable does not carry, a
// recipe its generator refuses, or a mesh that deforms (skinned or morphed: a mesh that comes and
// goes is drawn rigid).
bool load_placement_mesh(const scene_gen::PlacementMesh& mesh, const PlacementMeshOptions& options,
                         PlacementMeshData& out, std::string& error);
// The `.tex` of `key` under `options`' derived-data root, read whole: what an admitter does for a
// texture the build skipped because the scene held it then and no longer does.
bool read_placement_texture(const PlacementMeshOptions& options, u64 key, texture::TextureData& out,
                            std::string* error = nullptr);

// **The ground before its cuts, for `scene_gen::Context::uncut_ground`** (scene_gen.md, "The
// formation and the ground before its cuts"): the terrain entry with its tile set's cuts removed,
// as a second provider made on the first ask and kept. `present()` only for a terrain that has
// cuts; every host that builds a `Context` hands `hook()` when it is. Thread-safe.
class UncutGround {
 public:
  explicit UncutGround(const TerrainDesc& terrain);
  ~UncutGround();
  ENGINE_NON_COPYABLE(UncutGround);
  bool present() const noexcept { return present_; }
  scene_gen::Context::UncutGround hook() noexcept;

 private:
  struct State;
  static bool get(void* context, scene_gen::Ground& out);
  bool present_ = false;
  std::unique_ptr<State> state_;
};

// Re-derives the scene's bounding sphere — what the camera frames and what every distance, bias
// and light reach scales with — from the instances as they stand. `load_scene` ends with it.
//
// It is public for one caller and one reason: a host that skins its instances only learns the
// mesh's own bounds *from* the load, and `gfx::InstanceDesc::bounds_padding` is a length in that
// same space, so it cannot be known before the mesh is. Such a host loads, computes the padding
// from `mesh_bounds` and its clips, writes it onto the instances, and calls this to put the
// camera back where the animated character actually is.
void update_scene_bounds(SceneData& out);

// The bounding sphere of a range of a mesh's clusters, in its own space; tighter than the box
// diagonal, which matters for elongated meshes.
void mesh_bounds(const geometry::ClusterLodMesh& lod, u32 first, u32 count, Vec3& center,
                 f32& radius);

// **How far a mesh reaches on the ground from its own origin**, metres at scale 1: the largest
// distance in x and z from the origin to any cluster sphere's edge of a range of its clusters. What
// a placement's footprint is — the circle a later placement entry keeps clear of (scene_gen.md,
// "Earlier placements"; ADR-0046 decision 4, as built) — measured the same way by the scene reader
// for a whole read and by the world's placements consumer for a streamed one.
f32 ground_radius(const geometry::ClusterLodMesh& lod, u32 first, u32 count) noexcept;

// The canonical vertex ids of mesh `mesh`'s cluster vertices, read-only: entry i is the id of
// `scene.lod.mesh.vertices[scene.parts[mesh].first_vertex + i]`, and
// `scene.parts[mesh].vertex_id_source` says what id space it is in. This is what a binding step
// looks a vertex up by — a file written beside the mesh names ids, never cluster indices, which the
// content build renumbers ([geometry](geometry.md), "Canonical vertex identity").
//
// **Host side only**: nothing uploads the ids, and they are not in a page's bytes, because no pass
// reads them yet. They are kept when a streamed scene releases its paged host streams, so the
// lookup does not depend on how the scene was loaded. Empty for a mesh that has no ids — the
// procedural scenes, a container built before cluster cache version 12 — and for an index past
// the scene's meshes.
std::span<const u32> mesh_vertex_ids(const SceneData& scene, u32 mesh) noexcept;

}  // namespace engine::renderer
