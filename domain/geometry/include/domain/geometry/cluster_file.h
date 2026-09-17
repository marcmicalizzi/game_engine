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
  u32 pad[4] = {0, 0, 0, 0};  // room for quantization origin and scale, and for page counts
};

// A material as the GPU wants it: 64 bytes, one cache line's half, indexed per cluster. The
// image slots index ClusterFileData::image_paths, or are -1. The padding is zero today and is
// where emissive, alpha cutoff, and the remaining texture slots go.
struct ClusterFileMaterial {
  Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
  f32 metallic = 1.0f;
  f32 roughness = 1.0f;
  i32 base_color_image = -1;
  i32 normal_image = -1;
  u32 pad[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

static_assert(sizeof(ClusterFileHeader) == 32, "the cluster file header is 32 bytes on the wire");
static_assert(sizeof(ClusterFileSection) == 24, "a cluster file section record is 24 bytes");
static_assert(sizeof(ClusterFileScalars) == 32, "the cluster file scalars section is 32 bytes");
static_assert(sizeof(ClusterFileMaterial) == 64, "ClusterFileMaterial is a 64-byte GPU record");

// Everything one file holds. `cluster_material` is empty or one entry per cluster; the image
// paths are as the source named them (relative to the source file), and an image the source
// embedded has an empty path.
struct ClusterFileData {
  ClusterLodMesh mesh;
  Vector<u32> cluster_material;
  Vector<ClusterFileMaterial> materials;
  Vector<std::string> image_paths;
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

}  // namespace engine::geometry
