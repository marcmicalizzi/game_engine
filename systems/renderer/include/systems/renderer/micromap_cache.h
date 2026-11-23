#pragma once

// **The opacity micromaps' bake as a derived node** (roadmap R207; docs/subsystems/renderer.md,
// "The bake is read back"). A scene read whole bakes a micromap for every alpha-tested triangle at
// load (`GpuScene::bake_micromaps`, gfx/opacity_micromap.h): about 7 s on the forest staging, every
// load, until this. Now the bake's sections — the per-cluster table, the per-triangle indices, the
// stored entries' triangle records and their states — are written to
// `<ddc>/micromaps/<key>.omm` and a later load with the same key builds the micromap from them with
// no bake dispatch.
//
// **The key covers the device.** The bake's bytes are one device's answer, not the engine's
// (opacity_micromap.h says why: contraction, the divide, the centre sample's filter, the
// subdivision limit), so the key names the device as the driver states it — vendor, device,
// driver version, `pipelineCacheUUID`, which is what the pipeline cache's header holds — beside
// everything the bake read: the bake shader's SPIR-V, the level cap, the device's limit, the
// budget and the texel target, the masked clusters and their triangle counts, the masked
// materials (each texture slot named by the texture's content and each sampler slot by its
// description, never by a slot number), and the masked triangles' UV words digested on the device
// by an integer-only pass. `k_micromap_cache_version` retires every entry a change to the format,
// the key or the bake's rules makes wrong. Nothing about the machine but the device, and no
// timestamp, decides.
//
// **A stored bake is never trusted without its key**: the file records the key it was made for,
// and a read compares it before a section is read; the file also records a hash of its header and
// every byte of its sections, which the last section's read checks before the micromap is built,
// so a file torn, truncated or written over by anything else is baked again, never drawn.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <domain/gfx/opacity_micromap.h>
#include <domain/gfx/visibility_resolve.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::renderer {

// Bumped when an entry made before would be wrong: the file's layout, what the key covers, or the
// bake's rule (the shader's bytes are in the key already, so a change to the shader alone does not
// need it). The bake's host-side rules — `gfx::bake_opacity_micromaps`' sort, dedup, budget level
// drop, special indices and per-cluster layout — are **not** in the key: a change there bumps this.
inline constexpr u32 k_micromap_cache_version = 1;
inline constexpr u32 k_micromap_file_magic = 0x4d4d4f45u;  // "EOMM", little-endian

// What a stored bake is keyed by (the header's comment says why each is there).
struct MicromapCacheKeyInput {
  u8 device[28] = {};  // gfx::micromap_device_identity
  u64 shader = 0;      // the bake shader's SPIR-V hash (gfx::Shader::hash)
  u32 level_cap = 0;   // the cap asked for, after the device's limit
  u32 device_limit = 0;
  u64 budget_bytes = 0;
  f32 target_texels = 0.0f;
  u32 cluster_count = 0;
  std::span<const u32> items;  // {cluster, material} per masked cluster, in the bake's order
  std::span<const u32> triangle_counts;
  u64 materials = 0;     // the masked materials' digest (`micromap_materials_digest`)
  u64 geometry[2] = {};  // gfx::micromap_geometry_digest
};
u64 micromap_cache_key(const MicromapCacheKeyInput& in) noexcept;

// One masked material as the bake reads it: the record's words with every texture and sampler
// slot replaced by what is in it — the albedo texture's content identity (its `.tex` build key or
// its encoded bytes' hash, its form, the level it was uploaded from) and the sampler's packed
// description — so two loads that put the same texture in another slot key alike, and another
// texture in the same slot keys apart. False (and the scene's bake is not stored) when a slot's
// content is not known.
struct MicromapMaterialWords {
  gfx::ResolveMaterial record;
  u64 albedo_identity = 0;   // 0: no albedo texture
  u64 sampler_identity = 0;  // 0: the albedo is not sampled
};
u64 micromap_materials_digest(std::span<const MicromapMaterialWords> materials) noexcept;

// "<ddc_root>/micromaps/<key as 16 lower-case hex digits>.omm".
std::string micromap_cache_path(std::string_view ddc_root, u64 key);

// The file's bytes before its sections: magic, version, key, the content hash, the header's own
// length, the four section sizes and the bake's measures. Little-endian, every field written by
// itself (no struct is copied whole, so no padding byte is ever written).
std::string micromap_file_header(u64 key, const gfx::MicromapSections& sections);
// Writes the header and the sections to a file of its own beside `path` and renames it over
// `path`, so a reader sees a whole file or none. False with the reason; a failed write leaves
// nothing.
bool write_micromap_file(const std::string& path, u64 key, const gfx::MicromapSections& sections,
                         std::string* error = nullptr);

// What a read found. `refused` names why a file that was there was not used (another key, another
// version, a torn file); empty when it was used or was not there.
struct MicromapFileRead {
  bool found = false;
  bool used = false;
  std::string refused;
  u64 file_bytes = 0;
};
// **Whether a scene's bake ran or was read back** (`GpuScene::micromap_cache`, the load summary's
// `load.micromaps`): `baked` and `read` count this scene's bakes and reads (0 or 1 each), `stored`
// whether a bake was written for the next load, `key` the derived-data key (0 when the scene's
// bake is not keyed: no derived-data root, or a masked material whose texture's content is not
// known, said in `unkeyed`), `refused` why a stored bake that was there was not used, `ms` the
// whole stage on the CPU's clock and `key_ms` its key's share (the digest pass and the hash).
struct MicromapCacheReport {
  u32 baked = 0;
  u32 read = 0;
  bool stored = false;
  u64 key = 0;
  u64 file_bytes = 0;
  f64 ms = 0.0;
  f64 key_ms = 0.0;
  u64 materials = 0;     // the key's material and geometry digests, for a log that asks why two
  u64 geometry[2] = {};  // loads keyed apart
  std::string path;
  std::string unkeyed;
  std::string refused;
};
// What `GpuScene::upload_materials` leaves for the bake: its parameters (the level cap, the budget
// and the texel target the settings resolved to), the masked materials' digest, why there is none,
// and the derived-data root the bake is stored under ("" stores nothing).
struct MicromapKeyState {
  u32 level = 0;
  u64 budget_bytes = 0;
  f32 texels = 0.0f;
  u64 materials = 0;
  std::string unkeyed;
  std::string root;
  MicromapCacheReport report;
};
// Fills `state.materials` and `state.root` from the scene's masked clusters (`list`: {cluster,
// material} pairs), its material table and what each bindless slot holds — `slots` a texture
// slot's content identity, `sampler_slots[i]` made from the description `sampler_keys[i]` — or says
// in `state.unkeyed` why the bake cannot be keyed (no root, or a slot whose content is not known).
void micromap_key_materials(MicromapKeyState& state, std::span<const u32> list,
                            std::span<const gfx::ResolveMaterial> table,
                            const HashMap<u32, u64>& slots, std::span<const u32> sampler_slots,
                            std::span<const u32> sampler_keys, std::string_view root);

// The scene a stored bake is read for, as the bake would read it (`gfx::MicromapBakeInput`'s
// `cluster_count`, `items` and `triangle_counts`).
struct MicromapSceneShape {
  u32 cluster_count = 0;
  std::span<const u32> items;  // {cluster, material} per masked cluster
  std::span<const u32> triangle_counts;
};
// Opens `path`, checks it is a stored bake for `key`, and restores the micromap from it
// (gfx::restore_opacity_micromaps), each section read straight into the memory it is uploaded
// from through a 4 MiB bounce buffer that the content hash is taken over on the way. `out` is
// untouched unless it is used.
//
// **The file bounds nothing; the scene does** (R207-M2): before anything is allocated, every
// section's size is held to the file's length (and their sum to u64), the cluster count to the
// scene's, the masked triangles to the scene's total and the stored entries to at most that; and
// before the restore uploads them, the per-cluster table is held word for word to the layout the
// scene's bake makes (`gfx::micromap_cluster_first`) and every index to a special index or an
// entry the file stores (`gfx::micromap_index_valid`), read once through the bounce buffer — the
// GPU reads both unchecked. Each refusal is named; the content hash is no bound, only a check
// against damage, and it is taken last.
MicromapFileRead read_micromap_file(const gfx::Device& device, const std::string& path, u64 key,
                                    const MicromapSceneShape& scene,
                                    gfx::OpacityMicromapSet& out);

}  // namespace engine::renderer
