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

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/gfx/cluster_cull.h>
#include <systems/renderer/terrain.h>

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
};

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

// What a scene file said about one mesh besides its path: what to call it, which bytes it must
// be, where it came from, and how to fit it. Parallel to `SceneDesc::meshes`, or empty.
struct SceneMeshInfo {
  std::string name;
  u64 hash = 0;                  // `assets::source_mesh_hash` the file must have; 0 is unchecked
  const char* origin = "scene";  // "scene", "overlay" (a manifest's replacement), or "terrain"
  MeshFit fit;
};

// What to load: mesh files and instances of them, or one of the procedural scenes above.
struct SceneDesc {
  Vector<std::string> meshes;       // glTF, GLB, or .clusters; one empty path = procedural
  Vector<SceneInstance> instances;  // empty: one identity instance of every mesh
  u32 grid_instances = 0;           // n: n x n copies of mesh 0, varied in rotation and scale
  Procedural procedural = Procedural::heightfield;  // which mesh an empty path builds
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
  // ---- what a scene file adds (schema `engine.scene.Scene`) ------------------------------------
  // Per mesh: a name, the hash its bytes must have, and a fit. Empty, or parallel to `meshes`.
  Vector<SceneMeshInfo> mesh_info;
  // The procedural terrain. When `enabled`, an empty mesh path builds it rather than the classic
  // heightfield, and it is cached in the derived-data root under a key over its fields.
  TerrainDesc terrain;
  std::string name;   // the scene file's own name
  u64 file_hash = 0;  // of the scene file's bytes; 0 for a scene no file described
};

// What `read_scene_file` may be told besides the path.
struct SceneFileOptions {
  // A landmark overlay manifest (`engine.scene.Overlay`): a local file mapping content hashes to
  // paths. A mesh whose `overlay` hash the manifest names is drawn from the manifest's file
  // instead, which the load then hashes and refuses if the bytes disagree. Empty: no overlay.
  std::string overlay;
};

// Reads a scene file (schema `engine.scene.Scene`): meshes with an optional name, content hash,
// overlay hash and fit; instances with a translation, rotation, yaw, scale and an optional height
// above the terrain; seeded scatters of instances; and an optional terrain. The oldest spelling —
// `{"meshes":[{"path":"..."}],"instances":[{"mesh":0,"translation":[x,y,z],"rotation":[x,y,z,w],
// "scale":[x,y,z]}]}` — is a valid scene of the same type and reads exactly as it always did.
// Mesh paths resolve against the file's own directory, so a scene file is movable as a unit; an
// empty instance list gives one identity instance of every mesh. The terrain, when present, is
// the last mesh and has one identity instance, after the file's own.
bool read_scene_file(const std::string& path, SceneDesc& out, std::string& error);
bool read_scene_file(const std::string& path, const SceneFileOptions& options, SceneDesc& out,
                     std::string& error);

// A 64-bit content hash as the scene format writes it: 16 lower-case hex digits.
std::string hash_hex(u64 hash);
bool parse_hash_hex(std::string_view text, u64& out) noexcept;

// One mesh of the scene as it arrived: the materials and images it named, and which primitive —
// and so which material — each of its clusters came from. The DAG itself has been moved into
// the scene's merged mesh by then.
struct SourceMesh {
  assets::MeshData data;        // materials and images; the positions are spent by now
  Vector<u32> part_of_cluster;  // the part index of every cluster, mesh-local
  Vector<i32> part_material;    // the material of every part, -1 for none
  std::string image_dir;        // what the relative image paths are relative to
  u32 primitives = 0;           // glTF primitives merged into it; 0 from a container
  const char* cache = "none";   // "file", "hit", "miss", or "none"
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
  // or the ones this load wrote into the cache entry it made. Empty — every image decoded as
  // before — for a glTF loaded with no cache and for a container that records none.
  Vector<geometry::ClusterFileTexture> textures;
  // The derived-data root those textures are looked up under; empty when the load reads no cache
  // (`SceneDesc::cache` off), and then the built textures are not used either.
  std::string texture_ddc;
};

// A loaded scene: everything the GPU upload reads, and everything a summary reports.
struct SceneData {
  geometry::ClusterLodMesh lod;             // every mesh of the scene, merged
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
  Vec3 center{};              // the scene's bounding sphere: what the camera frames
  f32 radius = 10.0f;         // and what every distance, bias, and light reach scales with
  u32 pair_count = 0;         // the prefix sum the cull dispatch covers
  bool heightfield = false;   // the procedural scene: procedural materials and fixed bounds
  u32 mesh_primitives = 0;    // of the first mesh, as the summary reports it
  const char* mesh_cache = "none";
  i64 build_ns = 0;  // import, weld, cluster, page, and merge
  // Carried from the `SceneDesc`, for a host that has only the loaded scene: the terrain answers
  // a camera path's heights above the ground, and the rest names what a measurement measured.
  TerrainDesc terrain;
  Vector<SceneMeshInfo> mesh_info;
  std::string name;
  u64 file_hash = 0;

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
