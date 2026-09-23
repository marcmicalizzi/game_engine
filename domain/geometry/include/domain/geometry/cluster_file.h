#pragma once

// The binary cluster container, `.clusters` (docs/plan/07-content-pipeline.md §7.3,
// docs/plan/04-renderer.md §4.3): one `ClusterLodMesh` plus the per-cluster material map, the
// materials themselves, the paths of the images they name, and the encoded bytes of the images
// the source embedded rather than named. It is what `engine-content build` writes into the
// derived-data cache and what the renderer loads instead of importing glTF and clustering at
// startup, and it is the first step toward fixed-size streaming pages.
//
// **A container is self-sufficient for drawing, or says exactly what else it needs**: an image
// with a path names a file beside the source mesh, and every other image travels inside. That is
// the property the renderer rests on — what it draws from a container has to be what it draws
// from the source — and it is checked end to end rather than assumed (docs/subsystems/apps.md).
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

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <foundation/io/vfs.h>

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
  // u64, two of them: the identity of what this container was built from. [0] is the source
  // mesh's content hash (`assets::source_mesh_hash`), [1] the build key over that hash and the
  // build options (`cluster_cache_key`). Both are zero in a file written before the section
  // existed and in one built from bytes with no source behind them, which is how an incremental
  // build knows to rebuild the entry rather than trust it.
  SourceHash = 15,
  // The streaming page table (`cluster_pages.h`): `ClusterPageDesc` per page, and the flat list
  // of child pages the descriptors index. Only these two are stored — `page_of_cluster` and
  // `children` are derived from the page ranges and the DAG by `rebuild_cluster_page_index` on
  // read, which costs one sort over the clusters and saves eight bytes a cluster on disk. A file
  // without them (an older build, or one built with `--page-bytes 0`) reads with an empty page
  // table, and a caller that wants one calls `build_cluster_pages`.
  Pages = 16,
  PageChildren = 17,
  // u32: [0] is the byte target the layout was given. It is not derivable from the table — the
  // largest page is a lower bound and nothing more, and for a mesh that fits in one page it is
  // the page's own size, which would report a fill of 1.0 whatever the target was. A file
  // without the section falls back to that lower bound.
  PageScalars = 18,
  // The optional per-vertex skin bindings of a skinned mesh (`geometry::SkinBinding`, eight
  // bytes, parallel to `Vertices`) and, because the mesh's own palette width is not derivable
  // from them, one `u32` saying how many joints they index. They are two sections rather than a
  // field in `Scalars` because that record's eight words are all spoken for (the four counts and
  // the four floats of the 16-bit grid), and growing it would change the layout of an element
  // every existing container carries. Both are written even when the mesh is unskinned — empty
  // and zero — so a container's section list does not depend on what it holds, and a file
  // without either reads as an unskinned mesh rather than as a failure.
  Skin = 19,
  SkinScalars = 20,
  // The images the source **embedded**, carried in full so that a container draws the picture its
  // source draws with nothing beside it. 21 is one `ClusterFileImage` per image, parallel to
  // `ImagePaths`, naming a range of 22, which holds the encoded files — PNG or JPEG bytes exactly
  // as they were found, never decoded — back to back and **deduplicated by content hash**, since
  // an exporter that packs occlusion, roughness, and metallic into one texture names the same
  // bytes from several slots. An image that came from a file of its own keeps its path and
  // carries no bytes; both sections are written even when there are none, so a container's
  // section list does not depend on what it holds, and a file without them reads with no bytes at
  // all, which is what every container built before them is.
  Images = 21,
  ImageBytes = 22,
  // The morph stream (`cluster.h`, "morph channels"). Six sections, because the stream is six
  // arrays of five different element sizes and the format's rule is one fixed-size element per
  // section — the alternative, one blob with offsets inside it, would put a second layout inside
  // the one the section table already describes.
  //
  //   23 MorphChannels       `MorphChannel`, 16 bytes, one per channel
  //   24 MorphNames          u8, the channels' names as NUL-terminated strings back to back,
  //                          in channel order. A separate section rather than offsets into
  //                          `Strings` so that a build which carries no images still writes its
  //                          channel names, and so that nothing has to renumber when either grows.
  //   25 MorphClusterSlices  u32, `clusters + 1` of them: the CSR into 26
  //   26 MorphSlices         `MorphSlice`, 12 bytes
  //   27 MorphIndices        u8, the cluster-local vertex of each delta, padded to a multiple of 4
  //   28 MorphDeltas         i16, three per delta then three more per delta for the normals when
  //                          the mesh has them, so this one section holds both arrays back to
  //                          back: the second half is present exactly when its element count is
  //                          twice the first's, which `MorphScalars` says.
  //   29 MorphScalars        u32, two of them: the delta count, and 1 when normal deltas follow
  //                          the position deltas in 28.
  //
  // All seven are written even for a mesh with no channels — empty, and zero — so a container's
  // section list says what the format is and not what this mesh happened to have, and a file from
  // before they existed reads as a mesh with no morph channels rather than as a failure.
  MorphChannels = 23,
  MorphNames = 24,
  MorphClusterSlices = 25,
  MorphSlices = 26,
  MorphIndices = 27,
  MorphDeltas = 28,
  MorphScalars = 29,
};

// One past the highest kind this build knows, which is how wide a by-kind table has to be.
inline constexpr u32 k_cluster_section_kinds = static_cast<u32>(ClusterSection::MorphScalars) + 1;

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

// Where one image's encoded bytes are, as the `Images` section records it. 24 bytes.
//
// `hash` is `core/hash`'s `hash_bytes` over those bytes. It is the dedup key the writer matches
// on (and then confirms with a comparison, because a 64-bit collision that swapped two textures
// would be invisible), and it is deliberately the *content* hash of the encoded file rather than
// an index: when the derived-data cache grows texture derivatives of its own (plan 07 §7.3), the
// entry for this image is addressed by exactly this number, and a container already says what to
// ask for.
//
// A zero `bytes` is an image the container does not carry — an external file, named by
// `image_paths[i]` — which is also what an all-zero record means, so a reader needs no flag.
struct ClusterFileImage {
  u64 hash = 0;
  u64 offset = 0;  // into the ImageBytes payload
  u32 bytes = 0;   // 0: no bytes here; the image is the file image_paths[i] names
  // A **one-based** byte offset into `Strings` of a NUL-terminated media type ("image/png"), 0
  // meaning the source did not say — one-based for the same reason the record's image slots are:
  // zero is what a file written before the field existed carries, so zero has to mean none.
  u32 mime = 0;
};

static_assert(sizeof(ClusterFileHeader) == 32, "the cluster file header is 32 bytes on the wire");
static_assert(sizeof(ClusterFileSection) == 24, "a cluster file section record is 24 bytes");
static_assert(sizeof(ClusterFileScalars) == 32, "the cluster file scalars section is 32 bytes");
static_assert(sizeof(ClusterFileMaterial) == 64, "ClusterFileMaterial is a 64-byte GPU record");
static_assert(sizeof(ClusterFileImage) == 24, "a cluster file image record is 24 bytes");

// One image's bytes as a caller holds them: the encoded file (PNG or JPEG, undecoded) and the
// media type the source stated, if it stated one. `bytes` is empty for an image that lives in a
// file of its own, which `ClusterFileData::image_paths` names.
struct ClusterImage {
  std::string mime_type;
  Vector<u8> bytes;
};

// Everything one file holds. `cluster_material` is empty or one entry per cluster; the image
// paths are as the source named them (relative to the source file), and an image the source
// embedded has an empty path and its bytes in `images` instead.
//
// `images` is empty or parallel to `image_paths`: entry *i* carries image *i*'s encoded bytes
// when the source embedded them, and is empty when the image is the file `image_paths[i]` names.
// **That is what makes a container self-sufficient for drawing**: a GLB keeps its textures inside
// itself, so a container that recorded only their (empty) paths drew untextured, and the same
// mesh looked different depending on whether it came from the source or from the cache. It is
// empty for every container written before the sections existed, which draws exactly as it did.
//
// `source_path` is the mesh this was built from, exactly as it was given to the builder, so a
// reader resolves those relative image paths against its directory. It is empty in a file
// written before the section existed and in one built from bytes with no file behind them; a
// reader with nothing to fall back on resolves the image paths against the container's own
// directory instead.
//
// `source_hash` and `build_key` are that source's identity: the hash of its bytes
// (`assets::source_mesh_hash`) and `cluster_cache_key` over it and the build options. They are
// what an incremental build compares against before deciding a container is up to date, and
// they are zero when nothing recorded them.
struct ClusterFileData {
  ClusterLodMesh mesh;
  // The streaming page table over `mesh`, empty in a container built without one. When it is
  // there, the mesh's clusters are in page order — coarse to fine, group by group — rather than
  // in the builders' leaves-first order.
  ClusterPages pages;
  Vector<u32> cluster_material;
  Vector<ClusterFileMaterial> materials;
  Vector<std::string> image_paths;
  Vector<ClusterImage> images;  // empty, or parallel to image_paths
  std::string source_path;
  u64 source_hash = 0;
  u64 build_key = 0;
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

// What the images of a container come to, for whoever reports on one. The dedup is the writer's,
// so these are the numbers the file has (or would have): `distinct` blobs take `bytes`, and
// `deduplicated` image slots are served by an earlier slot's bytes. Reading a container back and
// summarizing it gives what building it gave, because identical bytes hash the same either way.
struct ClusterImageSummary {
  u32 count = 0;         // images the container names at all
  u32 embedded = 0;      // of those, how many carry their bytes here
  u32 distinct = 0;      // distinct blobs among them
  u32 deduplicated = 0;  // embedded - distinct
  u64 bytes = 0;         // what the distinct blobs take
};
ClusterImageSummary summarize_cluster_images(const ClusterFileData& data);

// ---- the container as a random-access file (docs/plan/04-renderer.md §4.9) --------------------
//
// `read_cluster_file` answers "give me this mesh" and costs the whole file in host memory.
// Streaming needs the other half of that question: the small **always-resident** tables now, and
// the big per-page streams left on disk to be fetched a page at a time. This is that reader.
//
// What makes it possible is the format itself: a section is an array of one fixed-size element at
// a 16-byte aligned offset, and `build_cluster_pages` has already reordered the vertex,
// attribute, quantized and triangle streams so that **a page's slice of each is one contiguous
// run** ("Pages and streaming" in docs/subsystems/geometry.md). So a page's payload is three or
// four ranged reads of one file and not a gather, which is why `range()` is all a streamer needs
// on top of `io::FileHandle`.
//
// **What `open` does not check, and why.** The header, the section table, and every section's
// extent are validated exactly as a full read validates them, so nothing `range()` reports can
// fall outside the file. The header's **content hash is not** checked, because it covers every
// byte after the header and checking it means reading the file — which is the cost this reader
// exists to avoid. `header_hash()` hands the stored value to a caller that wants to compare it
// against a full read it made for other reasons, which is what the renderer does: its load reads
// the container once, hash and all, and opens this beside it to go on fetching pages after the
// merged streams have been released.
//
// Filling `resident` reads only the resident sections, by range. One of them is not small: the
// embedded image payload is the whole of a GLB's textures, and it is here because a container is
// a complete answer to "draw this mesh" and the textures are uploaded whole. That stops being
// true when the derived-data cache grows texture derivatives ([07 §7.3]).
class ClusterFileReader {
 public:
  ClusterFileReader() noexcept = default;
  ~ClusterFileReader() = default;
  ENGINE_NON_COPYABLE(ClusterFileReader);
  ClusterFileReader(ClusterFileReader&&) noexcept = default;
  ClusterFileReader& operator=(ClusterFileReader&&) noexcept = default;

  // Opens the container and reads its header and section table. `resident`, when given, is
  // replaced with everything **but** the paged streams: the cluster and LOD descriptors, the
  // level counts, the scalars and the 16-bit grid, the page table, the per-cluster material map,
  // the materials, the image paths, records and strings, the source path and the source identity.
  // `mesh.vertices`, `mesh.quantized`, `mesh.attributes`, `mesh.triangles`, `mesh.vertex_source`
  // and `mesh.skin` come back **empty** however long they are in the file; `range()` is how to
  // reach them. Returns false with `error` and leaves the reader closed.
  bool open(std::string_view path, ClusterFileData* resident = nullptr,
            std::string* error = nullptr);
  void close() noexcept;
  bool valid() const noexcept { return file_.valid(); }

  const io::FileHandle& file() const noexcept { return file_; }
  const std::string& path() const noexcept { return file_.path(); }
  u64 header_hash() const noexcept { return content_hash_; }
  // How many elements of `kind` the container holds; zero for a kind it does not carry, which is
  // what an older container's page table or image records are.
  u64 element_count(ClusterSection kind) const noexcept;

  // The byte range of elements [first, first + count) of `kind`, for `io::FileHandle::read_at`.
  // False — leaving the outputs zero — when the container has no such section, when its elements
  // are not `element_size` bytes, or when the run runs past the section's end, so a caller may
  // ask about a section an older container does not carry and get an answer rather than a fault.
  // A `count` of zero is true with zero bytes: an empty run is a read a caller can skip.
  bool range(ClusterSection kind, u32 element_size, u64 first, u64 count, u64& offset,
             u64& bytes) const noexcept;

 private:
  io::FileHandle file_;
  u64 content_hash_ = 0;
  Vector<ClusterFileSection> sections_;        // as the file lists them, validated
  u32 by_kind_[k_cluster_section_kinds] = {};  // index into `sections_` plus one; 0 is absent
};

// Just the `SourceHash` section of a container, without rebuilding the mesh from it: the header,
// the section table, and the content hash are checked exactly as a full read checks them, and
// then the two words are taken out. Both are zero for a container that recorded no identity, so
// an incremental build treats such a file as stale rather than as a hit. Returns false, with
// `error`, when the file cannot be read or is not a container.
bool read_cluster_file_identity(std::string_view path, u64& source_hash, u64& build_key,
                                std::string* error = nullptr);

// ---- the derived-data cache (docs/plan/07-content-pipeline.md §7.3) -------------------------
//
// A mesh built from a source file is stored under a hash of everything that went into it, so a
// second build finds the answer instead of repeating the work, and a changed source, a changed
// build option, or a changed builder misses. The rules live here because `engine-content` and
// `engine-view` must agree on them byte for byte: what one writes, the other finds.

// Bumped whenever the builder or the container changes in a way that makes an old cache entry
// wrong. It is not the file format version: a cache miss is cheap, a wrong mesh is not.
// 3: the clusters of a cached container are laid out in streaming pages, which renumbers them.
// 4: a skinned source now carries a per-vertex `SkinBinding` stream, and the weld key includes
//    it, so an entry built before this is both missing the stream and welded differently.
// 5: 4 was the format's half of that and not the builders'. `weld_vertices` took the bindings as
//    an optional fourth stream, but neither `engine-content build` nor `engine-view --mesh`
//    passed them, so every container built at 4 from a skinned source was welded *without* the
//    binding in the key — two coincident vertices with different weights silently became one,
//    which is the defect that stream exists to prevent. Both builders pass it now, so an entry
//    built at 4 is wrong for a skinned mesh and merely re-keyed for a rigid one.
// 6: a container carries the bytes of the images its source embedded (sections 21 and 22). An
//    entry built at 5 from a GLB has none of them, so it draws that mesh untextured while the
//    glTF beside it draws it textured — the same mesh, two pictures, depending on whether the
//    cache was warm. Such an entry is not merely missing a section: it is the wrong answer.
// 7: the LOD simplifier is given the normals and the UVs as weighted attributes and is told
//    where the atlas seams are (`ClusterLodOptions::normal_weight`/`uv_weight`/`uv_seams`,
//    geometry.md "What the simplifier is given, and why"). Every entry built at 6 or earlier was
//    simplified in meshoptimizer's *permissive* mode with no attribute metric and no seam tags,
//    so on a fragmented atlas its coarse levels interpolate the texture across unrelated islands.
//    Such an entry is not missing anything the reader can add: its triangles are the wrong ones.
// 8: a source's morph targets become a per-vertex channel stream (sections 23..29), the weld key
//    includes a vertex's morph deltas, and `ClusterLodOptions::morph_seams` tags the pairs the
//    weld kept apart so permissive simplification does not merge them back. An entry built at 7
//    from a source with morph targets carries none of the stream *and* was welded without the
//    deltas in the key, which merged two coincident vertices that move differently — the same
//    class of defect version 5 fixed for skin weights.
// 9: normal cones are refit to the triangles as floats **and on the 16-bit grid** with
//    `k_cone_margin` of slack (geometry.md, "Normal cones"). An entry built at 8 carries
//    meshoptimizer's cutoff and apex, tight around its own float normals: a grid triangle the
//    rasterizers draw can sit outside that cone, and on a GCC or clang build at x86-64-v3 a
//    degenerate triangle's FMA residue may have widened it. Its cones are the wrong ones.
// 10: both builders repair UV-degenerate triangles before the weld (`uv_repair.h`, geometry.md
//    "UV-degenerate triangles: the repair"): a triangle whose UVs enclose no area is refolded into
//    the neighbouring island, or dropped when it is also thinner than a grid step. An entry built
//    at 9 from a source with such a triangle keeps an atlas island of zero texels that no LOD
//    level can sample, and its vertex and index streams differ from what a build makes now. A
//    source with none builds the same bytes as before and merely moves to a new cache path.
// 11: 8 was, like 4, the format's half and not the builders'. `weld_vertices` took the morph
//    channels as an optional stream, but neither `engine-content build` nor `engine-view --mesh`
//    passed them, so the weld renumbered every vertex while the channels kept the old numbers:
//    on any source whose weld moved a vertex — dropping an unreferenced one, merging a duplicate,
//    or only renumbering by first use, which is most indexed files — the deltas landed on
//    whichever vertex took the old number, and a delta whose old number was past the welded count
//    was dropped. MorphStressTest had its deltas wrong on 1,124 of its 1,528 vertices. Both
//    builders now call `assets::weld_vertices`, which hands the weld every stream `MeshData` owns,
//    so an entry built at 10 from a source with morph targets is the wrong mesh; a source without
//    any builds the same bytes and moves to a new cache path.
inline constexpr u32 k_cluster_cache_version = 11;

// The cache key: the source's content hash (`assets::source_mesh_hash`) mixed with the build
// options and the version above. `page_bytes` is the streaming page target the container was
// laid out with, 0 for a container with no page table — it is here because paging renumbers the
// clusters, so two page sizes are two different containers and an entry built with one is not
// the answer to a question that asked for the other.
u64 cluster_cache_key(u64 source_hash, const ClusterLodOptions& options, bool weld,
                      u32 page_bytes) noexcept;

// "<ddc_root>/clusters/<hash as 16 lower-case hex digits>.clusters".
std::string cluster_cache_path(std::string_view ddc_root, u64 hash);

// The repository's derived-data root: walks up from `start` (an executable's directory, say)
// to the first directory that holds an `AGENTS.md` and returns "<that>/ddc". Empty when there
// is no such directory, which is how a caller knows to ask for one explicitly.
std::string find_ddc_root(std::string_view start);

}  // namespace engine::geometry
