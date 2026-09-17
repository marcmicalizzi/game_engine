#pragma once

// The binary cluster container, `.clusters` (docs/plan/07-content-pipeline.md §7.3,
// docs/plan/04-renderer.md §4.3): one `ClusterLodMesh` plus the per-cluster material map, the
// materials themselves, and the paths of the images they name. It is what `engine-content
// build` writes into the derived-data cache and what the renderer loads instead of importing
// glTF and clustering at startup, and it is the first step toward fixed-size streaming pages.
//
// Layout, little-endian, no pointers, every payload an array of one fixed-size element that the
// reader and the writer copy with memcpy:
//
//   header     32 bytes: magic "CLST", version, flags, section_count, total_bytes, content_hash
//   sections   section_count records of { kind, element_size, element_count, offset }
//   payloads   at 16-byte aligned offsets from the start of the file, writer's order
//
// The section kinds are fixed and append-only: a new kind takes the next free value, an old one
// is never renumbered or reused, and a reader **skips a kind it does not know**, so a file
// written by a newer build still loads as long as the sections this build requires are there.
// Removing a kind that readers require, or changing an element's layout, is a version bump.
//
// `content_hash` (core/hash `hash_bytes`) covers every byte after the header, so it pins the
// section table as well as the payloads. `cluster_file_hash` computes the same value from a
// `ClusterFileData` in memory: that is the identity a content-addressed cache stores the built
// mesh under, and the reader checks it on load.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::geometry {

inline constexpr u32 k_cluster_file_version = 1;
inline constexpr u32 k_cluster_file_alignment = 16;  // every payload offset is a multiple

// Fixed, append-only section kinds. Unknown values are skipped on read.
enum class ClusterSection : u32 {
  Clusters = 1,            // ClusterDesc, one per cluster
  Lod = 2,                 // ClusterLodDesc, parallel to Clusters
  Vertices = 3,            // Vec3, cluster-ordered positions
  Attributes = 4,          // VertexAttributes, parallel to Vertices
  Triangles = 5,           // u32, one packed triangle each
  VertexSource = 6,        // u32, source vertex index, parallel to Vertices
  LevelClusterCounts = 7,  // u32, clusters per DAG level
  ClusterMaterial = 8,     // u32, material index, one per cluster
  Materials = 9,           // ClusterFileMaterial
  ImagePaths = 10,         // u32, byte offsets into Strings, one per image
  Strings = 11,            // u8, NUL-terminated strings back to back
  Scalars = 12,            // exactly one ClusterFileScalars
  // 13 is reserved for per-cluster quantized positions and their origin and scale, which the
  // writer emits once ClusterMesh carries them.
  Quantized = 13,
  SourcePath = 14,  // u8, one NUL-terminated string: the source mesh this was built from
};

// Names the kinds this build knows, "unknown" for anything else; for diagnostics and for
// `engine-content info`.
const char* cluster_section_name(u32 kind) noexcept;

// The file header, memcpy'd in and out. 32 bytes.
struct ClusterFileHeader {
  char magic[4] = {'C', 'L', 'S', 'T'};
  u32 version = k_cluster_file_version;
  u32 flags = 0;
  u32 section_count = 0;
  u64 total_bytes = 0;
  u64 content_hash = 0;  // of every byte after the header
};

// One section record. 24 bytes.
struct ClusterFileSection {
  u32 kind = 0;  // a ClusterSection value
  u32 element_size = 0;
  u64 element_count = 0;
  u64 offset = 0;  // from the start of the file, a multiple of k_cluster_file_alignment
};

// The counts that are not arrays, as one single-element section so the table stays uniform.
struct ClusterFileScalars {
  u32 group_count = 0;
  u32 leaf_triangle_count = 0;
  u32 source_vertex_count = 0;
  u32 source_triangle_count = 0;
  u32 pad[4] = {0, 0, 0, 0};  // the quantization origin and step, as float bits
};

// The alpha word, `ClusterFileMaterial::alpha` (what was pad[7]): the glTF alpha mode in bits
// 0..1 (0 opaque, 1 mask, 2 blend, matching assets::k_alpha_*), "double sided" in bit 2, and the
// alpha cutoff as a unorm16 in bits 16..31 — a cutoff is compared against an 8-bit alpha
// channel, so 1/65535 of precision is more than the comparison can see, and packing it here is
// what keeps the record at 64 bytes. A zero word is opaque, single sided, cutoff 0, which is
// what a file written before the word existed reads.
u32 encode_alpha_word(u8 alpha_mode, bool double_sided, f32 alpha_cutoff) noexcept;
u8 alpha_word_mode(u32 word) noexcept;
bool alpha_word_double_sided(u32 word) noexcept;
f32 alpha_word_cutoff(u32 word) noexcept;

// The image slots that live in the record's padding are **one-based** — 0 is "no image", and
// slot n names image n - 1 — because a file written before they existed has zeros there and
// zero has to mean none. The two original slots keep their -1 convention; these convert.
inline u32 encode_optional_image(i32 index) noexcept {
  return index < 0 ? 0u : static_cast<u32>(index) + 1u;
}
inline i32 decode_optional_image(u32 slot) noexcept {
  return slot == 0 ? -1 : static_cast<i32>(slot - 1u);
}

// A material as the GPU wants it: 64 bytes, one cache line's half, indexed per cluster.
//
// The first 32 bytes are the original record and never move. The second 32 were `u32 pad[8]`
// and are now named, one field per word, so that **every one of them reads as "none" or "off"
// when it is zero** — which is exactly what an older file, whose padding was zero, gives:
//
//   pad[0]  metallic_roughness_image  one-based slot, 0 = none
//   pad[1]  occlusion_image           one-based slot, 0 = none
//   pad[2]  emissive_image            one-based slot, 0 = none
//   pad[3]  emissive.x                f32 bits
//   pad[4]  emissive.y
//   pad[5]  emissive.z
//   pad[6]  normal_scale              f32 bits; 0 leaves the normal map with nothing to do
//   pad[7]  alpha                     see encode_alpha_word above
//
// `base_color_image` and `normal_image` index ClusterFileData::image_paths, or are -1.
struct ClusterFileMaterial {
  Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
  f32 metallic = 1.0f;
  f32 roughness = 1.0f;
  i32 base_color_image = -1;
  i32 normal_image = -1;
  u32 metallic_roughness_image = 0;  // glTF packing: G roughness, B metallic
  u32 occlusion_image = 0;           // R ambient occlusion
  u32 emissive_image = 0;
  Vec3 emissive{0.0f, 0.0f, 0.0f};
  f32 normal_scale = 1.0f;
  u32 alpha = 0;
};

static_assert(sizeof(ClusterFileHeader) == 32, "the cluster file header is 32 bytes on the wire");
static_assert(sizeof(ClusterFileSection) == 24, "a cluster file section record is 24 bytes");
static_assert(sizeof(ClusterFileScalars) == 32, "the cluster file scalars section is 32 bytes");
static_assert(sizeof(ClusterFileMaterial) == 64, "ClusterFileMaterial is a 64-byte GPU record");

// Everything one file holds. `cluster_material` is empty or one entry per cluster; the image
// paths are as the source named them (relative to the source file), and an image the source
// embedded has an empty path.
//
// `source_path` is the mesh this was built from, exactly as it was given to the builder, so a
// reader resolves those relative image paths against its directory. It is empty in a file
// written before the section existed and in one built from bytes with no file behind them; a
// reader with nothing to fall back on resolves the image paths against the container's own
// directory instead.
struct ClusterFileData {
  ClusterLodMesh mesh;
  Vector<u32> cluster_material;
  Vector<ClusterFileMaterial> materials;
  Vector<std::string> image_paths;
  std::string source_path;
};

// Writes the file through io::write_file_atomic, so a reader sees the old bytes or the new ones
// and never a mix. Parent directories are not created. Returns false and fills `error`.
bool write_cluster_file(std::string_view path, const ClusterFileData& data,
                        std::string* error = nullptr);

// Reads a file written by `write_cluster_file`. `out` is replaced, and left empty on failure:
// an unreadable file, a truncated one, the wrong magic or version, a section that runs past the
// end, a content hash that does not match, a missing required section, or inconsistent counts.
bool read_cluster_file(std::string_view path, ClusterFileData& out, std::string* error = nullptr);

// The same from bytes already in memory (a mapping, a cache entry, a download).
bool read_cluster_file_memory(std::span<const u8> bytes, ClusterFileData& out,
                              std::string* error = nullptr);

// The content hash the writer stores in the header, computed without writing anything.
u64 cluster_file_hash(const ClusterFileData& data);

// ---- the derived-data cache (docs/plan/07-content-pipeline.md §7.3) -------------------------
//
// A mesh built from a source file is stored under a hash of everything that went into it, so a
// second build finds the answer instead of repeating the work, and a changed source, a changed
// build option, or a changed builder misses. The rules live here because `engine-content` and
// `engine-view` must agree on them byte for byte: what one writes, the other finds.

// Bumped whenever the builder or the container changes in a way that makes an old cache entry
// wrong. It is not the file format version: a cache miss is cheap, a wrong mesh is not.
inline constexpr u32 k_cluster_cache_version = 2;  // 2: the material record's padding is filled

// The cache key: the source's content hash (`assets::source_mesh_hash`) mixed with the build
// options and the version above.
u64 cluster_cache_key(u64 source_hash, const ClusterLodOptions& options, bool weld) noexcept;

// "<ddc_root>/clusters/<hash as 16 lower-case hex digits>.clusters".
std::string cluster_cache_path(std::string_view ddc_root, u64 hash);

// The repository's derived-data root: walks up from `start` (an executable's directory, say)
// to the first directory that holds an `AGENTS.md` and returns "<that>/ddc". Empty when there
// is no such directory, which is how a caller knows to ask for one explicitly.
std::string find_ddc_root(std::string_view start);

}  // namespace engine::geometry
