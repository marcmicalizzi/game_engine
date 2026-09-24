#pragma once

// The tissue container, `.tissue`, and the interchange it is imported from
// (docs/subsystems/tissue.md; schemas/tissue.schema).
//
// **The interchange** is what the authoring side writes without the engine: one JSON file, a
// `TissueDefinition` (schemas/tissue.schema), and one little-endian binary file per block its
// `blocks` table lists, each with its kind, element count, byte size and SHA-256. A Blender script
// writes it with `json.dump` and `ndarray.astype('<f4').tofile`, and `import_interchange` checks
// every size and every hash before anything is interpreted.
//
// **The container** is the same content as one file the engine loads, laid out like `.clusters`:
//
//   header     32 bytes: magic "TISS", version, flags, section_count, total_bytes, content_hash
//   sections   section_count records of { kind, element_size, element_count, offset, name, 0 }
//   payloads   at 16-byte aligned offsets from the start of the file, in the writer's order
//
// A section is one fixed-size element repeated: kind 1 is the definition's canonical JSON, kind 2
// the block names, and every other kind is one block, its kind the block's `BlockKind`. **The
// kinds are append-only** — never renumbered, never reused — and a reader **skips a kind it does
// not know**, with a warning naming it: a region whose element kind this build lacks falls back
// to what it can read and the validator says so, rather than refusing the file (plan 07 §7.11's
// note on element kinds). The definition is read the same way, unknown fields warned about and
// ignored. Removing a kind readers need, or changing an element's layout, is a version bump.
//
// `content_hash` (core/hash `hash_bytes`) covers every byte after the header, as in `.clusters`;
// each block's SHA-256 travels in the definition's block table and is checked again on load, so a
// container carries the author's own hashes from the interchange to the validator.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <cstring>
#include <schemas/tissue.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::tissue {

inline constexpr u32 k_tissue_file_version = 1;
inline constexpr u32 k_tissue_file_alignment = 16;
// The definition's `format` string this build writes and reads.
inline constexpr const char* k_tissue_format = "engine.tissue.v0";

// The file header, memcpy'd in and out. 32 bytes.
struct TissueFileHeader {
  char magic[4] = {'T', 'I', 'S', 'S'};
  u32 version = k_tissue_file_version;
  u32 flags = 0;
  u32 section_count = 0;
  u64 total_bytes = 0;
  u64 content_hash = 0;  // of every byte after the header
};

// One section record. 32 bytes.
struct TissueFileSection {
  u32 kind = 0;  // a BlockKind value
  u32 element_size = 0;
  u64 element_count = 0;
  u64 offset = 0;  // from the start of the file, a multiple of k_tissue_file_alignment
  u32 name = 0;    // one-based byte offset into the Names section; 0 for an unnamed section
  u32 reserved = 0;
};

static_assert(sizeof(TissueFileHeader) == 32, "the tissue file header is 32 bytes on the wire");
static_assert(sizeof(TissueFileSection) == 32, "a tissue file section record is 32 bytes");

// The element size of a kind this build knows (f32 x3 is 12, u32 x4 is 16), or 0 for one it does
// not — which is how a reader tells a kind to skip.
u32 block_element_size(BlockKind kind) noexcept;
// The kind's schema name ("RegionNodes"), or "unknown".
const char* block_kind_name(u32 kind) noexcept;

// One block: its name, kind and element count as the definition's table says, and its bytes.
struct TissueBlock {
  std::string name;
  BlockKind kind = BlockKind::RegionNodes;
  u64 count = 0;
  Vector<u8> bytes;
};

// A definition and its blocks, as the importer produces and the container holds them. `warnings`
// is what a read had to skip or ignore: unknown section kinds, unknown definition fields, blocks
// the table lists that no section holds. Nothing in it stops a read; the validators report it.
struct TissueFile {
  TissueDefinition definition;
  Vector<TissueBlock> blocks;  // the definition's `blocks` order
  Vector<std::string> warnings;

  const TissueBlock* find(std::string_view name) const noexcept;
};

// ---- the container ------------------------------------------------------------------------------

// The container's bytes: the definition (with each block's `file` cleared and its count, size and
// hash as the block holds them) as canonical JSON, the names, then one section per block.
bool encode_tissue_file(const TissueFile& file, Vector<u8>& out, std::string* error = nullptr);
// Writes it through io::write_file_atomic. Parent directories are not created.
bool write_tissue_file(std::string_view path, const TissueFile& file, std::string* error = nullptr);
// Reads a container: refuses a wrong magic or version, a section past the end, a content hash or a
// block hash that does not match, and a definition that is missing or not a TissueDefinition;
// skips, with a warning, a section of a kind it does not know. `out` is replaced.
bool read_tissue_file(std::string_view path, TissueFile& out, std::string* error = nullptr);
bool read_tissue_file_memory(std::span<const u8> bytes, TissueFile& out,
                             std::string* error = nullptr);

// What `engine-content tissue info` prints: the header and every section, known or not.
struct TissueFileSectionInfo {
  u32 kind = 0;
  std::string kind_name;
  std::string name;
  u32 element_size = 0;
  u64 element_count = 0;
  u64 offset = 0;
  bool known = false;
};
struct TissueFileInfo {
  TissueFileHeader header;
  Vector<TissueFileSectionInfo> sections;
};
bool read_tissue_file_info(std::span<const u8> bytes, TissueFileInfo& out,
                           std::string* error = nullptr);

// ---- the interchange
// ------------------------------------------------------------------------------

// Reads the interchange JSON at `json_path` and every block file its table names (relative to the
// JSON's directory), checking each block's kind is one this build knows, its byte size is its count
// times the kind's element size, the file is exactly that long, and its SHA-256 is the table's.
// Refuses a duplicate block name, a `format` other than `k_tissue_format`, and anything the schema
// cannot read; unknown JSON fields are refused here (an interchange is written for this format),
// where a container read only warns.
bool import_interchange(std::string_view json_path, TissueFile& out, std::string* error = nullptr);

// Writes a TissueFile as an interchange: `<directory>/<stem>.json` and one `<stem>.<block>.bin`
// per block, the table's `file`, `count`, `bytes` and `sha256` filled from the blocks. For tests
// and for handing a container back to an authoring tool. The directory must exist.
bool export_interchange(const TissueFile& file, std::string_view directory, std::string_view stem,
                        std::string* error = nullptr);

// Fills a block's table entry (`count`, `bytes`, `sha256`) from its bytes, and adds the entry to
// the definition's table if it is not there: the one way a writer keeps the table and the bytes in
// step.
void seal_block(TissueFile& file, const TissueBlock& block);

// Appends a block holding `data`, `per_element` values of T to an element (3 for a u32 x3 kind
// held as u32), and seals its table entry. For writers: the synthetic fixture, the tests, a tool.
template <class T>
void add_block(TissueFile& file, std::string name, BlockKind kind, std::span<const T> data,
               u32 per_element = 1) {
  TissueBlock block;
  block.name = std::move(name);
  block.kind = kind;
  block.count = data.size() / per_element;
  block.bytes.resize(static_cast<u32>(data.size() * sizeof(T)));
  if (!data.empty()) std::memcpy(block.bytes.data(), data.data(), block.bytes.size());
  seal_block(file, block);
  file.blocks.push_back(std::move(block));
}

// The topology hash the observation contract carries: SHA-256 over the face indices widened to
// little-endian u64, face by face and corner by corner (schemas/tissue.schema, `topology_sha256`).
std::string topology_sha256(std::span<const u32> indices);

}  // namespace engine::tissue
