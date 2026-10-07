// The GPU-resident scene, mirrored once for every shader that reads it (docs/plan/04-renderer.md
// §4.2). No bindings and no entry points: the cull pass, the three rasterizers, the resolve, and
// the cluster acceleration structure records all include this file, so the structs the CPU pins
// with size tables are written down here exactly once instead of in each shader. A shader that
// includes it names it in the `DEPENDS` of its `engine_shaders()`, the way brdf.slang is named.
//
// A scene is instances of meshes. Every mesh lives in one set of global buffers with the offsets
// baked into each ClusterDesc; a MeshDesc says which clusters a mesh owns and which 16-bit
// position grid it is on, and an InstanceDesc places one mesh in the world. The unit of work is
// the **pair** (instance, cluster): instance i owns its mesh's clusters starting at
// `first_pair`, a prefix sum over the instances in order. The visible list holds one
// `uint2 {instance, cluster}` per surviving pair, and the visibility buffer's id is
// `pair << 8 | triangle` — the scene's pair, not the list entry, so that a depth tie goes to the
// same surface whatever order the frame drew in (`pair_of`, and docs/subsystems/gfx.md, "The tie
// rule").

#ifndef ENGINE_SCENE_SLANG
#define ENGINE_SCENE_SLANG

// Mirrors geometry::ClusterDesc (48 bytes).
struct ClusterDesc {
  uint vertex_offset;
  uint triangle_offset;
  uint vertex_count;
  uint triangle_count;
  float3 center;
  float radius;
  float3 cone_apex;  // normal cone for backface culling (cull pass only)
  uint cone;         // axis snorm8 x, y, z in bytes 0..2, cutoff snorm8 in byte 3
};

// Mirrors geometry::VertexAttributes (8 bytes).
struct VertexAttributes {
  uint normal_oct;
  uint uv_half2;
};

// Mirrors gfx::MeshDesc (64 bytes): one per mesh of the scene. The last four addresses are
// where a deformed instance's positions come from instead of the grid.
struct MeshDesc {
  float4 quant;        // xyz grid origin, w grid step: this mesh's own grid
  uint64_t quantized;  // uint16_t[3 * vertex_count] of the whole scene, padded to an even count
  uint first_cluster;  // this mesh's range of the global cluster array
  uint cluster_count;
  uint64_t deform_pool;   // float[3 * pool_vertices]: the frame's deformed-vertex pool, or 0
  uint64_t deform_slots;  // uint[] per visible entry: its pool vertex base, or k_no_pool_slot
  uint64_t templates;     // uint64_t[]: a cluster template address per global cluster index, or 0
  uint64_t skin;          // SkinBinding[]: eight bytes per scene-wide vertex, or 0
};

// Mirrors gfx::InstanceDesc (96 bytes): one per instance, in order of first_pair. Where it is, is a
// WorldCell — `cell` on the world's 64 m grid and the offset into it, which is the 3x4's
// translation column — and a frame never reads it but through `instance_point` (below), which
// measures it from the frame's eye (ADR-0053, gfx.md "The instance's layout").
struct InstanceDesc {
  float4 rows[3];  // mesh space to the cell's frame: (linear row r, local[r])
  int3 cell;
  uint mesh;
  uint material_base;  // added to the cluster's material index in the resolve
  uint first_pair;
  float scale_max;  // largest axis scale: radii and LOD errors multiply by it
  uint flags;       // bit 0: uniform scale; bit 1: a terrain tile's UVs from its corner
  uint deform;      // entry of the DeformDesc table; k_invalid_deform: rigid
  // Mesh-space slack the cull pass adds to every sphere of this instance, so a deformed instance
  // whose vertices leave their rest positions is still culled conservatively. Zero for a rigid
  // one, and a float plus zero is exact, so the rigid arithmetic is unchanged.
  float bounds_padding;
  uint terrain;  // 1 + this instance's terrain level in the frame's TerrainLevelDesc table, or 0
  uint pad;
};
// InstanceDesc::flags bit 1 (gfx::k_instance_uv_from_corner; bit 0, k_instance_uniform_scale, is
// below with the cull's constants): a terrain tile whose UVs are measured from its corner, which
// the material's lookup puts back in the scene grid's frame (`terrain_uv_offset`).
static const uint k_instance_uv_from_corner = 2u;

// ---- the frame's origin (ADR-0053; docs/subsystems/gfx.md, "The frame's origin") ----------------
//
// **No shader holds or computes an absolute world position.** A frame has one origin, its eye, and
// every position a pass works with is measured from it. What a pass is given of the eye is this:
// its cell, its offset into the cell, and what that offset's rounding to a float left over.

// Mirrors gfx::FrameEye (48 bytes): engine::WorldEye in three 16-byte rows. All zero is the world's
// origin, and an instance is then where it is stored, to the bit.
struct FrameEye {
  int3 cell;
  uint pad0;
  float3 local;
  float pad1;
  float3 residual;
  float pad2;
};

// **engine::relative_axis (core/math/world.h), operation for operation**: where a stored position
// is from the frame's eye along one axis. The cells' difference times the pitch, which is exact (to
// 2^24 cells), then the two offsets, in the order that keeps every intermediate the size of the
// result — `(cells - eye) + local` where the point's cell is above the eye's, which near the eye is
// an eye at the top of its cell and a point at the bottom of the next, and `(cells + local) - eye`
// otherwise — and the eye's residual last. `precise` is SPIR-V's NoContraction on every operation,
// so the driver neither fuses nor reorders it: the world-eye test holds this function to the CPU's
// to the bit, and a scene moved by whole cells hands it the same operands and gets the same bits.
float relative_axis(int cell, float local, int eye_cell, float eye_local, float eye_residual) {
  precise const float cells = float(cell - eye_cell) * 64.0;
  precise const float from_local =
      cells > 0.0 ? (cells - eye_local) + local : (cells + local) - eye_local;
  precise const float result = from_local - eye_residual;
  return result;
}

// The instance's translation from the frame's eye: engine::relative(WorldCell, WorldEye).
float3 instance_from_eye(InstanceDesc instance, FrameEye eye) {
  return float3(
      relative_axis(instance.cell.x, instance.rows[0].w, eye.cell.x, eye.local.x, eye.residual.x),
      relative_axis(instance.cell.y, instance.rows[1].w, eye.cell.y, eye.local.y, eye.residual.y),
      relative_axis(instance.cell.z, instance.rows[2].w, eye.cell.z, eye.local.z, eye.residual.z));
}

// A mesh-space direction through the instance's linear part.
float3 instance_direction(InstanceDesc instance, float3 v) {
  return float3(dot(instance.rows[0].xyz, v), dot(instance.rows[1].xyz, v),
                dot(instance.rows[2].xyz, v));
}

// **Every reader moves a mesh-space point into the frame through this**: the linear part, then the
// instance's translation from the eye (`from_eye`, which a reader computes once per instance and
// hands every vertex). The point's distance from the eye is what the sum rounds at, so a vertex by
// the eye is as fine as a float is there, wherever in the world the eye is.
float3 instance_point(InstanceDesc instance, float3 from_eye, float3 p) {
  return instance_direction(instance, p) + from_eye;
}

// A point of the instance in its **cell's** frame — the 3x4 alone, the cell's corner at the origin.
// What a test against something stored in the same frame takes (the scene grid's hole: the grid is
// at cell zero, so its cell's frame is its mesh space).
float3 instance_cell_point(InstanceDesc instance, float3 p) {
  return float3(dot(instance.rows[0], float4(p, 1.0)), dot(instance.rows[1], float4(p, 1.0)),
                dot(instance.rows[2], float4(p, 1.0)));
}

// The visibility buffer's extent as gfx::draw_extent packs it.
uint extent_width(uint extent) { return extent & 0xffffu; }
uint extent_height(uint extent) { return extent >> 16; }

// The frame's eye a block names: a FrameEye* or 0, which is the world's origin.
FrameEye load_eye(uint64_t address) {
  if (address == 0) {
    FrameEye origin;
    origin.cell = int3(0, 0, 0);
    origin.pad0 = 0;
    origin.local = float3(0.0, 0.0, 0.0);
    origin.pad1 = 0.0;
    origin.residual = float3(0.0, 0.0, 0.0);
    origin.pad2 = 0.0;
    return origin;
  }
  return ((FrameEye*)address)[0];
}

// Mirrors gfx::TerrainField (24 bytes): one evaluated height field on a window of a terrain
// level's lattice, sample (i, j) at lattice point (i0 + i, j0 + j), rows of x in order of z.
struct TerrainField {
  uint64_t heights;  // float[nx * nz], metres
  int i0;
  int j0;
  uint nx;
  uint nz;
};

// Mirrors gfx::TerrainLevelDesc (96 bytes): a terrain level as this frame draws it — its lattice,
// the two fields it blends, the padding its spheres take, and the square the next finer level
// draws instead (deform.slang's terrain stage; cluster_cull.slang drops a cluster wholly inside
// `hole`). Two kinds (gfx::TerrainLevelDesc): the scene's grid (`spacing_mm` 0), in its mesh
// space, which is the world, with `origin`, `spacing` and `hole` in metres; and a world-lattice
// level (`spacing_mm` > 0), whose tiles are instances at their corners and whose vertices the pool
// pass writes relative to them (`terrain_corner_mm`).
struct TerrainLevelDesc {
  TerrainField a;
  TerrainField b;
  float2 origin;  // x, z of lattice point (0, 0): the scene's grid's
  float spacing;
  float blend;    // 0 draws a, 1 draws b
  float padding;  // replaces InstanceDesc::bounds_padding for the level's clusters
  float skirt;    // a skirt vertex (its rest normal points down) hangs this far under the field
  // k_terrain_level_named: a vertex whose rest normal is horizontal is drawn from the level it
  // names (a world tile's border along a coarser tile; gfx::TerrainLevelDesc::flags).
  uint flags;
  int spacing_mm;  // a world-lattice level's millimetres between points; 0 for the scene's grid
  float4 hole;     // x0, z0, x1, z1, the scene's grid's; none when x1 <= x0
};
static const uint k_terrain_level_named = 1u;

// **A terrain tile's corner in whole millimetres from the world's origin** (gfx::terrain_corner_mm,
// operation for operation): a world-lattice level's tile is an instance at its corner, whose cell
// and local (`gfx::set_terrain_corner`) are whole millimetres; the local times 1000 in float32 is
// within a hundredth of a millimetre of them, so rounding gives them back exactly.
int64_t terrain_corner_axis_mm(int cell, float local) {
  return int64_t(cell) * 64000 + int64_t(round(local * 1000.0));
}

// **Where a tile's corner stands in the scene grid's UV frame** (gfx::terrain_uv_offset, operation
// for operation; gfx::k_instance_uv_from_corner): a world tile's UVs are measured from its corner,
// so the material's lookup adds this to the interpolated UV — the corner less the frame's corner,
// integer millimetres, over the frame's side. Zero for any other instance.
float2 terrain_uv_offset(InstanceDesc instance, int x0_mm, int z0_mm, uint size_mm) {
  if ((instance.flags & k_instance_uv_from_corner) == 0) return float2(0.0, 0.0);
  const int64_t x = terrain_corner_axis_mm(instance.cell.x, instance.rows[0].w) - int64_t(x0_mm);
  const int64_t z = terrain_corner_axis_mm(instance.cell.z, instance.rows[2].w) - int64_t(z0_mm);
  const float size = float(size_mm);
  return float2(float(x) / size, float(z) / size);
}

// a / b rounded down, for b > 0 (SPIR-V's integer division truncates towards zero).
int64_t terrain_floor_div(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return a - q * b < 0 ? q - 1 : q;
}

// The lattice index along one axis of a point `rest_m` metres from a tile's corner at `corner_mm`,
// on the world's lattice at `spacing_mm`: the corner's whole spacings in integers, and what is left
// of it plus the rest position in float32, which is the size of a tile and not of the world.
int terrain_lattice_index(int64_t corner_mm, float rest_m, int64_t spacing_mm) {
  const int64_t q = terrain_floor_div(corner_mm, spacing_mm);
  const float r = float(corner_mm - q * spacing_mm);
  return int(q + int64_t(round((r + rest_m * 1000.0) / float(spacing_mm))));
}

// Lattice point `index` at `spacing_mm`, in metres from a tile's corner at `corner_mm`: exact in
// millimetres, rounded to float32 once at the size of the tile.
float terrain_from_corner(int index, int64_t spacing_mm, int64_t corner_mm) {
  return float(int64_t(index) * spacing_mm - corner_mm) / 1000.0;
}

// Mirrors gfx::DeformDesc (48 bytes): one per deformed instance, read by the pool pass alone.
// `stages` is a **mask** of the chain's stages plus the procedural kind, not an exclusive mode.
// `joints` and `weights` are per instance and `MeshDesc::skin` is per mesh, which is the split a
// crowd needs: one binding stream, one array of bone matrices and one of weights per character.
// Where in the pool the instance writes is **not** here — the pool is suballocated per frame per
// visible entry, so that address is `MeshDesc::deform_slots[entry]`.
struct DeformDesc {
  uint stages;         // k_deform_stage_* | the procedural kind in k_deform_kind_mask
  uint joint_count;    // the bone-matrix array's length; 0 leaves a skinned instance at rest
  uint first_channel;  // this mesh's first entry of the scene's morph channel array
  uint channel_count;  // the width of `weights`' two halves
  uint first_vertex;   // this mesh's first scene-wide vertex: what indexes `cache`
  uint pad;
  uint64_t joints;    // JointMatrix[joint_count], or 0
  uint64_t weights;   // float[2 * channel_count]: static weights then pose weights
  uint64_t cache;     // float3 per mesh vertex: the static stage's kept result, or 0
};

// Mirrors geometry::MorphChannel (16 bytes).
struct MorphChannel {
  float position_scale;
  float normal_scale;    // 0: this channel carries no normal deltas
  float max_displacement;
  float default_weight;
};

// Mirrors geometry::MorphSlice (12 bytes): one channel's run of deltas inside one cluster.
struct MorphSlice {
  uint channel;
  uint first_delta;
  uint delta_count;
};

// Mirrors gfx::DeformCacheVertex (16 bytes): what the static shape stage keeps between frames,
// one per vertex of the instance's mesh. The normal is in it because a cache of positions alone
// would leave the stage's morph scan running every frame for the normals, which is the cost the
// cache exists to remove.
struct DeformCacheVertex {
  float3 position;
  uint normal;  // octahedral, geometry::encode_normal_oct's packing
};

// Mirrors geometry::SkinBinding (8 bytes): four u8 joint indices then four u8 weights summing to
// 255. Read as two 32-bit words, because a byte array through a device address would be four
// loads where the hardware wants one.
struct SkinBinding {
  uint packed_joints;
  uint packed_weights;
};

// Mirrors anim::JointMatrix (48 bytes): the affine 3x4 of model * inverse_bind, three float4
// rows, so skinning a position is three dot products with float4(p, 1) and the fourth row that
// is always (0, 0, 0, 1) is never stored or loaded.
struct JointMatrix {
  float4 rows[3];
};

// ---- geometry streaming (docs/plan/04-renderer.md §4.3 step 3 and §4.9) -----------------------
//
// A scene's clusters are laid out in fixed-size **pages**, and a viewer may hold only some of
// them. Three tables travel with the scene and stay resident whatever the budget, because the
// cull pass tests every pair of every frame and has to be able to say "not that one" about a
// cluster whose payload is not here: the page table itself, the page each cluster is in, and the
// clusters each cluster refines into. They are 12 bytes a cluster plus 48 bytes a page against
// the ~1.5 KB a cluster the page holds, which is the ratio that makes the split worth making.

// Mirrors geometry::ClusterPageDesc (48 bytes). The cull pass reads none of it today — the
// drawing rule needs only `page_of_cluster` and the residency word — but the record is the
// streaming contract the way ClusterDesc is the geometry one, so it is written down here once,
// beside the tables that index it, rather than in whichever pass first needs `level_min`.
struct ClusterPageDesc {
  uint first_cluster;
  uint cluster_count;
  uint first_vertex;
  uint vertex_count;
  uint first_triangle;
  uint triangle_count;
  uint level_min;
  uint level_max;
  uint first_child_page;
  uint child_page_count;
  uint bytes;
  uint flags;
};

static const uint k_page_root = 1u;      // nothing coarser can replace it: pinned, never evicted
static const uint k_page_oversized = 2u;  // one group did not fit in a page of its own

// Mirrors geometry::ClusterChildren (two uint): the run of clusters one cluster refines into,
// which is the group it was simplified from and which the page builder keeps contiguous. A
// level-0 cluster is original geometry and has `cluster_count` zero.
struct ClusterChildren {
  uint first_cluster;
  uint cluster_count;
};

// Mirrors geometry::PageRequest (12 bytes): what one missing page is worth to the cluster that
// could not refine into it. The CPU heap reads these words without a conversion.
struct PageRequest {
  uint page;
  float screen_px;  // the projected diameter of the requesting cluster's sphere, in pixels
  float distance;   // observer to the near side of that sphere
};

static const uint k_instance_uniform_scale = 1u;
static const uint k_invalid_deform = 0xffffffffu;
// gfx::k_no_pool_slot: this visible entry has no block in the frame's pool, so every reader takes
// its rest pose. That is the *only* overflow behaviour there is — never garbage, never a crash.
static const uint k_no_pool_slot = 0xffffffffu;
static const uint k_deform_identity = 0u;
static const uint k_deform_wave = 1u;
static const uint k_deform_lattice = 2u;
static const uint k_deform_kind_mask = 3u;
// The chain's stages (gfx::k_deform_stage_*), run in this order and never in another.
static const uint k_deform_stage_static = 0x100u;
static const uint k_deform_stage_pose = 0x200u;
static const uint k_deform_stage_skin = 0x400u;
static const uint k_deform_stage_procedural = 0x800u;
static const uint k_deform_stage_rebuild_cache = 0x1000u;
static const uint k_deform_stage_terrain = 0x2000u;

// The rest pose: three u16 at 3 * vertex of the mesh's grid, read as the 32-bit words that
// contain them. The stream is padded to an even u16 count on the CPU, so the last word read is
// always inside the buffer. `vertex` is the scene-wide vertex index (vertex_offset + local).
float3 load_quantized_position(MeshDesc mesh, uint vertex) {
  uint* words = (uint*)mesh.quantized;
  const uint i = vertex * 3;
  const uint w0 = words[i >> 1];
  const uint w1 = words[(i + 1) >> 1];
  const uint w2 = words[(i + 2) >> 1];
  const uint x = (i & 1) != 0 ? (w0 >> 16) : (w0 & 0xffff);
  const uint y = ((i + 1) & 1) != 0 ? (w1 >> 16) : (w1 & 0xffff);
  const uint z = ((i + 2) & 1) != 0 ? (w2 >> 16) : (w2 & 0xffff);
  return mesh.quant.xyz + float3(float(x), float(y), float(z)) * mesh.quant.w;
}

// Where this frame put the deformed vertices of visible entry `entry`, as a **pool vertex
// index**, or k_no_pool_slot when the instance is rigid, the frame has no pool, or the entry did
// not fit the budget. Resolved once per cluster by every reader — a workgroup, a draw instance,
// a resolve pixel's triangle — and then handed to `load_position` for each of its vertices.
//
// `entry` is the index into the **whole** visible list (the visibility id's high bits), not into
// one run of it, because the table is one array over the frame's three runs and every view.
uint deform_base(MeshDesc mesh, InstanceDesc instance, uint entry) {
  if (instance.deform == k_invalid_deform || mesh.deform_slots == 0) return k_no_pool_slot;
  uint* slots = (uint*)mesh.deform_slots;
  return slots[entry];
}

// True when this instance's positions could come from the pool. It says nothing about whether
// this frame found room for a particular cluster; `deform_base` answers that.
bool is_deformed(MeshDesc mesh, InstanceDesc instance) {
  return instance.deform != k_invalid_deform && mesh.deform_slots != 0;
}

// **Every** position read of a scene goes through this (docs/plan/04-renderer.md §4.3). `base` is
// what `deform_base` answered for the entry being drawn and `first` is the cluster's
// `vertex_offset`, so the pool slot is the vertex's position *within its cluster* — which is what
// a per-frame suballocation makes it. `k_no_pool_slot` is the rest pose off the mesh's 16-bit
// grid, and it covers three cases with one branch: a rigid instance, a frame with no pool, and a
// deformed cluster the pool budget had no room for.
float3 load_position(MeshDesc mesh, uint base, uint first, uint local) {
  if (base != k_no_pool_slot) {
    float* pool = (float*)mesh.deform_pool;
    const uint slot = (base + local) * 3;
    return float3(pool[slot], pool[slot + 1], pool[slot + 2]);
  }
  return load_quantized_position(mesh, first + local);
}

// The octahedral normal packing, the inverse of geometry::encode_normal_oct / decode_normal_oct.
// It is here rather than in the two files that want it because the deform chain **writes** the
// packing the resolve **reads**, and a pair of encoders in two files is a pair that can drift.
// `material.slang` keeps its own decoder for the rest attribute stream, deliberately untouched:
// that one is on the path of every existing picture.
float3 decode_oct_normal(uint packed) {
  const float x = float(int(packed << 16) >> 16) / 32767.0;
  const float y = float(int(packed) >> 16) / 32767.0;
  float3 n = float3(x, y, 1.0 - abs(x) - abs(y));
  if (n.z < 0.0) {
    n.xy = float2((1.0 - abs(y)) * (x < 0.0 ? -1.0 : 1.0), (1.0 - abs(x)) * (y < 0.0 ? -1.0 : 1.0));
  }
  return normalize(n);
}

uint encode_oct_normal(float3 n) {
  const float l1 = abs(n.x) + abs(n.y) + abs(n.z);
  float2 p = l1 > 0.0 ? n.xy / l1 : float2(0.0, 0.0);
  if (n.z < 0.0) {
    p = float2((1.0 - abs(p.y)) * (p.x < 0.0 ? -1.0 : 1.0),
               (1.0 - abs(p.x)) * (p.y < 0.0 ? -1.0 : 1.0));
  }
  const int x = int(round(clamp(p.x, -1.0, 1.0) * 32767.0));
  const int y = int(round(clamp(p.y, -1.0, 1.0) * 32767.0));
  return (uint(x) & 0xffffu) | ((uint(y) & 0xffffu) << 16);
}

// The deformed shading normal of pool vertex `base + local`, or the rest one when this frame
// carries no normal pool or this entry has no block. `normal_pool` is `ResolveParams::normal_pool`
// or `DeformParams::normal_pool`; both are the same array.
float3 load_pool_normal(uint64_t normal_pool, uint base, uint local, float3 rest) {
  if (normal_pool == 0 || base == k_no_pool_slot) return rest;
  uint* packed = (uint*)normal_pool;
  return decode_oct_normal(packed[base + local]);
}

// The vertex path's indexed draw of one hardware run: word offsets into gfx::VertexDrawHeader (64
// bytes). Its records (gfx::VertexDrawRecord, a uint4 per pair slot) and its index array are
// buffers of their own. cluster_cull.slang allocates into it, vertex_expand.slang writes the
// indices, and cluster_vertex_indexed.slang draws from it; the offsets are written down once, here.
static const uint k_vertex_draw_index_count = 0;  // VkDrawIndexedIndirectCommand: words 0..4
static const uint k_vertex_draw_cursor = 5;       // triangles allocated, fitting or not
static const uint k_vertex_draw_fit_end = 6;      // atomic min of the overflowing survivors' starts
static const uint k_vertex_draw_capacity = 7;     // triangles the index array holds
static const uint k_vertex_draw_fallback_instances = 9;  // of the fallback's draw, words 8..11
static const uint k_vertex_draw_expand_groups = 12;      // vertex_expand's dispatch, words 12..14
static const uint k_vertex_draw_overflow = 15;           // survivors the fallback draws

// Entry `index` of the visible list. A null list is the identity scene the direct draws use:
// instance 0 and the cluster in index order, which is what a single-mesh draw without culling
// wants, and there entry i is pair i too (`pair_of`, below).
uint2 visible_entry(uint64_t visible, uint index) {
  if (visible == 0) return uint2(0, index);
  uint2* entries = (uint2*)visible;
  return entries[index];
}

// The scene's pair index of `cluster` drawn by `instance`: the inverse of the cull pass's
// `first_cluster + (pair - first_pair)`. It is what the visibility id carries in its high 24 bits,
// so that the id is a function of the scene alone — the same for every run of the visible list,
// every occlusion pass, every rasterizer and every frame — and a depth tie, which the 64-bit atomic
// max settles on the id, goes to the larger (pair, triangle) whatever order the pairs were drawn
// in. A null visible list draws instance 0's clusters in index order, and there the pair *is* the
// cluster, because the first instance's pairs start at 0 and the first mesh's clusters do too.
uint pair_of(InstanceDesc instance, MeshDesc mesh, uint cluster) {
  return instance.first_pair + (cluster - mesh.first_cluster);
}

// The visible entry a pixel's pair was drawn as this frame, which is where everything kept per
// entry is found — a deformed instance's block of the pool above all; the {instance, cluster} is
// the scene's pair table's to give, in one load instead of two. `pair_entries` is this view's
// table, written by the cull pass for every pair it drew; 0, with a null visible list, means the
// entry is the pair.
uint entry_of_pair(uint64_t pair_entries, uint pair) {
  if (pair_entries == 0) return pair;
  uint* entries = (uint*)pair_entries;
  return entries[pair];
}

// The instance that owns pair `pair`: the last one whose first_pair is at or below it. The
// instances are in order, so this is a plain binary search over the prefix sum.
uint instance_of_pair(InstanceDesc* instances, uint instance_count, uint pair) {
  uint lo = 0;
  uint hi = instance_count - 1;
  while (lo < hi) {
    const uint mid = (lo + hi + 1) >> 1;
    if (instances[mid].first_pair <= pair) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

// A direction in the instance's frame, for normals. The cofactor matrix of the upper-left 3x3
// (its columns are the cross products of the linear part's columns) is the adjugate transpose,
// which is the inverse transpose scaled by the determinant: the same direction for any positive
// scale and the correctly flipped one for a mirror, so normalizing afterwards makes the division
// unnecessary and the uniform-scale case needs no special path.
float3 transform_normal(InstanceDesc instance, float3 n) {
  const float3 c0 = float3(instance.rows[0].x, instance.rows[1].x, instance.rows[2].x);
  const float3 c1 = float3(instance.rows[0].y, instance.rows[1].y, instance.rows[2].y);
  const float3 c2 = float3(instance.rows[0].z, instance.rows[1].z, instance.rows[2].z);
  return normalize(cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z);
}

#endif  // ENGINE_SCENE_SLANG
