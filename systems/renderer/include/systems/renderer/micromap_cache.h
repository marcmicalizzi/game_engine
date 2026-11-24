#pragma once

// **The opacity micromaps' bake as a derived node, stored by what it is made of** (roadmap R207,
// R208; docs/subsystems/renderer.md, "The bake is read back"). A scene read whole bakes a micromap
// for every alpha-tested triangle at load (`GpuScene::bake_micromaps`, gfx/opacity_micromap.h):
// about 7 s on the forest staging. R207 stored the whole scene's bake under one key; R208 stores it
// **a part at a time** — a part is one mesh of the scene with its masked clusters — because in a
// streamed world what a load holds follows where it starts, and a key over the whole held set
// baked the whole scene again at every new starting place.
//
// **What a part is keyed by** (`micromap_part_key`): the *generation* — `k_micromap_cache_version`,
// the device as the driver states it (vendor, device, driver version, `pipelineCacheUUID`; the
// bake's bytes are one device's answer, opacity_micromap.h says why) and the bake shader's SPIR-V
// hash — then the level cap the bake used, the texel target, and per masked cluster of the part in
// order: its index within the part, its triangle count, its material as the bake reads it
// (`micromap_material_digest`: the texture's content and the sampler's description in place of
// slot numbers) and its triangles' UV words digested on the device
// (`gfx::micromap_item_digests`). Nothing about the load — which slot, which place in the list,
// which other meshes — is in it, so a part is found by any load that holds the same mesh.
//
// **Where they are kept**: `<ddc>/micromaps/<generation>/<pack>.omm`. A *pack* is what one load
// stored: the parts that load had to bake, in one file, their entries deduplicated across the pack
// as the bake deduplicated them (most sharing is between meshes: a forest's trees cut their cards
// from the same atlas regions, so a file a part would store those entries again per mesh, and open
// a file per mesh at every load). A load reads the index of every pack of its generation, reads the
// packs that hold its parts, bakes the rest in one dispatch, stores them as a new pack, and builds
// one micromap from all of it (`assemble_micromaps`), entries shared by their exact bytes.
//
// **A stored pack is never trusted without its keys**: its header records its generation and each
// part's key, and a content hash over the header and every byte of its sections, checked when the
// pack is read; every index and triangle record is held to the pack's own counts before anything
// is copied or uploaded, and a stored part is used only when its triangle count is the scene part's
// (a 64-bit key that collides over other content is a miss, never a read past the part). A pack
// that fails is not used and its parts are baked; it is removed at that load's store only when its
// bytes were read and refused for what they say (a pack that did not open or changed under the read
// is left), and never when this load stored it.
//
// **What bounds the folder** (`prune_micromap_store`, at every store): the generations kept are the
// current one and the most recently used other (`k_micromap_generations_kept`: a driver rolled
// back, or a second card), by a `used` file each keyed load rewrites; every other generation's
// packs are removed. R207's whole-scene files are removed. Within a generation the packs grow with
// the distinct parts the machine has loaded under it, as `<ddc>/clusters` grows with the meshes
// built (row R211). A temporary file a store that died left behind, in the current generation, the
// kept other or the root, is removed by the next store once it is a minute old (a younger one may
// be another process's store under way).

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <domain/gfx/opacity_micromap.h>
#include <domain/gfx/visibility_resolve.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::renderer {

// Bumped when an entry made before would be wrong: the file's layout, what the key covers, or the
// bake's rule (the shader's bytes are in the generation already, so a change to the shader alone
// does not need it). The bake's host-side rules — `gfx::bake_opacity_micromaps`' sort, dedup,
// special indices — are **not** in the key: a change there bumps this. 2: R208's packs of parts.
inline constexpr u32 k_micromap_cache_version = 2;
inline constexpr u32 k_micromap_file_magic = 0x4d4d4f45u;  // "EOMM", little-endian
// The generations a store keeps: the current one and the most recently used other.
inline constexpr u32 k_micromap_generations_kept = 2;
// A temporary file younger than this may be another process's store under way.
inline constexpr i64 k_micromap_temp_age_ms = 60'000;
// The bytes of one part's record in a pack's header: key, triangle count, the wanted histogram.
inline constexpr u32 k_micromap_part_record_bytes = 8 + 4 + 4 * (gfx::k_micromap_max_level + 1);
// A pack's header before its part records: the prefix (magic, version, generation, content hash,
// the header's length) and the counts (parts, triangles, entries, data bytes).
inline constexpr u32 k_micromap_pack_fixed_bytes = 28 + 20;

// The device, the bake shader and the version: what makes one machine's stored bakes its own.
u64 micromap_generation(const u8 device[28], u64 shader) noexcept;
// "<ddc_root>/micromaps/<generation as 16 lower-case hex digits>".
std::string micromap_generation_dir(std::string_view ddc_root, u64 generation);

// One masked material as the bake reads it: the record's words with every texture and sampler
// slot replaced by what is in it — the albedo texture's content identity (its `.tex` build key or
// its encoded bytes' hash, its form, the level it was uploaded from) and the sampler's packed
// description — so two loads that put the same texture in another slot key alike, and another
// texture in the same slot keys apart.
struct MicromapMaterialWords {
  gfx::ResolveMaterial record;
  u64 albedo_identity = 0;   // 0: no albedo texture
  u64 sampler_identity = 0;  // 0: the albedo is not sampled
};
u64 micromap_material_digest(const MicromapMaterialWords& material) noexcept;

// What a part is keyed by (the header's comment says why each is there). Per masked cluster of
// the part, in the part's order: its index within the part, its triangle count, its material's
// digest and its UV words' digest.
struct MicromapPartKeyInput {
  u64 generation = 0;
  u32 level_cap = 0;
  f32 target_texels = 0.0f;
  std::span<const u32> locals;
  std::span<const u32> triangle_counts;
  std::span<const u64> materials;
  std::span<const u64> digests;
};
u64 micromap_part_key(const MicromapPartKeyInput& in) noexcept;

// **Whether a scene's micromaps were baked or read back** (`GpuScene::micromap_cache`, the load
// summary's `load.micromaps`): `parts` the scene's masked parts, `read` and `baked` how many of
// them were read from a stored pack and how many baked, `packs_read` the packs they came from,
// `stored` whether this load wrote a pack (`stored_bytes`, `path`), `generation` the store's
// generation (0 when the scene's bake is not keyed: no derived-data root, below the floor, or a
// material whose texture's content is not known, said in `unkeyed`), `refused` why the first pack
// that was there was not used (`packs_refused` how many), `swept`, `pruned_packs` and
// `pruned_generations` what the store removed, `ms` the whole stage and `key_ms` its key's share.
struct MicromapCacheReport {
  u32 parts = 0;
  u32 baked = 0;
  u32 read = 0;
  u32 packs = 0;
  u32 packs_read = 0;
  u32 packs_refused = 0;
  bool stored = false;
  u64 stored_bytes = 0;
  u64 read_bytes = 0;
  u64 generation = 0;
  u32 levels_dropped = 0;
  u32 swept = 0;
  u32 pruned_packs = 0;
  u32 pruned_generations = 0;
  f64 ms = 0.0;
  f64 key_ms = 0.0;
  std::string path;
  std::string unkeyed;
  std::string refused;
  // `renderer.rt.micromap_capture` (a diagnostic for the tests, off by default): the micromap's
  // input as it was built, keyed or not — per masked triangle in the scene's order its index
  // (special or an entry), the entries' records and the states — so a case can compare two loads
  // triangle by triangle through each one's own index.
  Vector<i32> captured_indices;
  Vector<gfx::MicromapTriangle> captured_triangles;
  Vector<u32> captured_data;
};

// What `GpuScene::upload_materials` leaves for the bake: its parameters (the level cap, the budget
// and the texel target the settings resolved to, the store's floor), per masked cluster the part
// it belongs to and its index there, each material's digest (0 for one no masked cluster names),
// why there is no key, and the derived-data root the bake is stored under ("" stores nothing).
struct MicromapKeyState {
  u32 level = 0;
  u64 budget_bytes = 0;
  f32 texels = 0.0f;
  u64 store_min = 0;  // `renderer.rt.micromap_store_min`: masked triangles below which it bakes
  Vector<u32> parts;  // per masked cluster: {part, index within the part}
  Vector<u64> material_digests;
  std::string unkeyed;
  std::string root;
  MicromapCacheReport report;
};
// Fills `state.material_digests` and `state.root` from the scene's masked clusters (`list`:
// {cluster, material} pairs), its material table and what each bindless slot holds — `slots` a
// texture slot's content identity, `sampler_slots[i]` made from the description `sampler_keys[i]` —
// or says in `state.unkeyed` why the bake cannot be keyed (no root, or a slot whose content is not
// known).
void micromap_key_materials(MicromapKeyState& state, std::span<const u32> list,
                            std::span<const gfx::ResolveMaterial> table,
                            const HashMap<u32, u64>& slots, std::span<const u32> sampler_slots,
                            std::span<const u32> sampler_keys, std::string_view root);

// ---- a pack: the parts one load stored ------------------------------------------------------

struct MicromapPackPart {
  u64 key = 0;
  u32 triangle_count = 0;
  u32 wanted[gfx::k_micromap_max_level + 1] = {};
};
// A pack's sections as written or read: per part in order its triangles' indices (special or an
// entry of the pack), the pack's entries' triangle records and their states.
struct MicromapPackView {
  std::span<const MicromapPackPart> parts;
  std::span<const i32> indices;
  std::span<const gfx::MicromapTriangle> triangles;
  std::span<const u32> data;
};
// "<generation dir>/<hash of the parts' keys as 16 hex digits>.omm".
std::string micromap_pack_path(std::string_view ddc_root, u64 generation,
                               std::span<const MicromapPackPart> parts);
// The file's bytes before its sections: the prefix, the counts and the part records.
// Little-endian, every field written by itself (no struct is copied whole).
std::string micromap_pack_header(u64 generation, const MicromapPackView& pack);
// Writes the header and the sections to a file of its own beside `path` and renames it over
// `path`, so a reader sees a whole file or none. False with the reason; a failed write leaves
// nothing.
bool write_micromap_pack(const std::string& path, u64 generation, const MicromapPackView& pack,
                         std::string* error = nullptr);

// A pack's header as read: its parts and counts, or why it was refused (another magic, version or
// generation, a header or a length its counts do not add up to). No section is read. `damaged` is
// set only when the bytes it read were refused for what they say; a pack that did not open or did
// not read is a miss for this load and is left alone (R208-L7: another process may be replacing it
// or a scanner holding it).
struct MicromapPackIndex {
  std::string path;
  u64 file_bytes = 0;
  u32 header_bytes = 0;
  u64 content_hash = 0;
  u32 triangle_count = 0;
  u32 entry_count = 0;
  u64 data_bytes = 0;
  Vector<MicromapPackPart> parts;
  std::string refused;
  bool damaged = false;
};
bool read_micromap_pack_index(const std::string& path, u64 generation, MicromapPackIndex& out);
// Its sections, read whole into memory and checked: the content hash over the header and every
// byte, every index a special index or one of its entries, every record a 4-state entry inside its
// states. False with the refusal; nothing is used from a pack that fails. `*damaged` is set as the
// index's is: true only when every byte was read and a content check refused them, false when the
// file did not open, changed length under the read, or a read failed.
struct MicromapPackData {
  Vector<MicromapPackPart> parts;
  Vector<u32> first_triangle;  // per part, its first index in `indices`
  Vector<i32> indices;
  Vector<gfx::MicromapTriangle> triangles;
  Vector<u32> data;
  MicromapPackView view() const;
};
bool read_micromap_pack(const MicromapPackIndex& index, MicromapPackData& out, std::string* refused,
                        bool* damaged = nullptr);

// ---- one micromap from stored and baked parts ------------------------------------------------

// Where a run of the scene's masked triangles comes from: `count` of them from `first` of source
// `source`'s indices, and the run's histogram of the levels the texel target wanted.
struct MicromapRun {
  u32 source = 0;
  u32 first = 0;
  u32 count = 0;
  const u32* wanted = nullptr;  // k_micromap_max_level + 1 counts, or null
};
// **The assembly**: the runs in the scene's order, each triangle's entry shared with any earlier
// entry of the same level and the same states word for word (a 64-bit hash proposes, the words
// decide; `dedup_mismatches` counts proposals the words refused), entries numbered in the order
// the scene first names them — so the micromap is a function of the triangles' states alone, the
// same whether its parts were baked by this load or read. `entry_source`/`entry_index` say where
// each entry's states are; `stats` the scene's measures (the micro-triangles counted from the
// states; `micro_unknown_filter` is the bake's alone and stays 0 here), and `total_bytes` the
// figure the budget is held to (states, indices and records, as the bake's). False, with nothing
// assembled, when a run names a source there is not or reaches past its source's indices (R208-M2:
// the caller matches a stored part's triangle count to the scene's before it makes a run, and this
// is the bound that holds whatever a key says).
struct MicromapAssembly {
  Vector<i32> indices;
  Vector<gfx::MicromapTriangle> triangles;
  Vector<u32> entry_source;
  Vector<u32> entry_index;
  gfx::MicromapStats stats;
  u64 total_bytes = 0;
};
bool assemble_micromaps(std::span<const MicromapPackView> sources,
                        std::span<const MicromapRun> runs, MicromapAssembly& out);
// Builds the scene's micromap from an assembly: the per-cluster table of `items` (the bake's own
// layout), the indices, the records and the states copied from the sources into the build's input.
bool restore_assembled_micromaps(const gfx::Device& device, u32 cluster_count,
                                 std::span<const u32> items, std::span<const u32> triangle_counts,
                                 std::span<const MicromapPackView> sources,
                                 const MicromapAssembly& assembly, gfx::OpacityMicromapSet& out,
                                 std::string* error);

// ---- the folder ---------------------------------------------------------------------------------

// What a store removed: temporary files a store that died left (`swept`: a minute old, in the
// current generation, the kept other and at the root, where R207's stores named theirs), packs that
// failed their read (`damaged`, given), every pack of a generation past the kept ones and R207's
// whole-scene files (`packs`), and the generations' folders emptied (`generations`: removed only
// when empty, never recursively, so a file another process put there after the listing stays).
struct MicromapPruneReport {
  u32 swept = 0;
  u32 packs = 0;
  u32 generations = 0;
};
void prune_micromap_store(std::string_view ddc_root, u64 generation,
                          std::span<const std::string> damaged, i64 now_unix_ms,
                          MicromapPruneReport& out);
// Rewrites `<generation dir>/used`, which the pruning orders generations by.
void touch_micromap_generation(std::string_view ddc_root, u64 generation);

}  // namespace engine::renderer
