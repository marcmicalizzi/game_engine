#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/tissue/sha256.h>
#include <domain/tissue/tissue_file.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace engine::tissue {

namespace {

constexpr u64 k_header_bytes = sizeof(TissueFileHeader);
constexpr u64 k_section_bytes = sizeof(TissueFileSection);

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

u64 align_up(u64 value) noexcept {
  const u64 a = k_tissue_file_alignment;
  return (value + a - 1) / a * a;
}

// The enumerator names of BlockKind this build knows, as the schema spells them in JSON.
bool known_kind_name(std::string_view name) {
  for (u32 kind = 0; kind < 64; ++kind) {
    if (block_element_size(static_cast<BlockKind>(kind)) == 0) continue;
    if (name == block_kind_name(kind)) return true;
  }
  return false;
}

// Reads a TissueDefinition from JSON. Strict for the interchange; for a container, blocks of a kind
// this build does not know are dropped from the table and unknown fields ignored, each with a
// warning, so that a newer writer's file still loads.
bool definition_from_json(std::string_view text, bool strict, TissueDefinition& out,
                          Vector<std::string>* warnings, std::string* error) {
  JsonValue json;
  const JsonParseResult parsed = parse_json(text, json);
  if (!parsed.ok)
    return fail(error, std::string("the definition is not JSON: ") + parsed.message + " at line " +
                           std::to_string(parsed.line) + ", column " +
                           std::to_string(parsed.column));
  if (!json.is_object()) return fail(error, "the definition is not a JSON object");
  if (!strict) {
    if (JsonValue* blocks = json.find("blocks"); blocks != nullptr && blocks->is_array()) {
      JsonValue::Array kept;
      for (usize i = 0; i < blocks->size(); ++i) {
        JsonValue& entry = (*blocks)[i];
        const JsonValue* kind = entry.is_object() ? entry.find("kind") : nullptr;
        std::string_view name;
        u64 number = 0;
        bool known = true;
        if (kind != nullptr && kind->get_string(name)) known = known_kind_name(name);
        if (kind != nullptr && kind->get_u64(number))
          known = number < 64 && block_element_size(static_cast<BlockKind>(number)) != 0;
        if (known) {
          kept.push_back(std::move(entry));
        } else if (warnings != nullptr) {
          const JsonValue* block_name = entry.is_object() ? entry.find("name") : nullptr;
          std::string_view block;
          if (block_name != nullptr) block_name->get_string(block);
          warnings->push_back("block '" + std::string(block) +
                              "' is of a kind this build does not know; it is skipped");
        }
      }
      *blocks = JsonValue(std::move(kept));
    }
  }
  schema::ReadContext ctx;
  ctx.options.ignore_unknown_fields = false;
  TissueDefinition definition;
  if (schema::from_json(definition, json, ctx)) {
    out = std::move(definition);
    return true;
  }
  bool only_unknown = true;
  for (const schema::Diagnostic& d : ctx.diagnostics)
    only_unknown = only_unknown && d.message == "unknown field";
  if (strict || !only_unknown) {
    const schema::Diagnostic& first = ctx.diagnostics.front();
    return fail(error,
                "the definition is not a TissueDefinition: " + first.path + ": " + first.message);
  }
  for (const schema::Diagnostic& d : ctx.diagnostics)
    if (warnings != nullptr)
      warnings->push_back("definition field " + d.path +
                          " is unknown to this build; it is ignored");
  schema::ReadContext tolerant;
  tolerant.options.ignore_unknown_fields = true;
  definition = TissueDefinition{};
  if (!schema::from_json(definition, json, tolerant))
    return fail(error,
                "the definition is not a TissueDefinition: " + tolerant.diagnostics.front().path +
                    ": " + tolerant.diagnostics.front().message);
  out = std::move(definition);
  return true;
}

Block* table_entry(TissueDefinition& definition, std::string_view name) {
  for (Block& b : definition.blocks)
    if (b.name == name) return &b;
  return nullptr;
}

std::string safe_file_name(std::string_view name) {
  std::string out;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
    out.push_back(ok ? c : '_');
  }
  return out;
}

}  // namespace

u32 block_element_size(BlockKind kind) noexcept {
  switch (kind) {
    case BlockKind::Definition:
    case BlockKind::Names: return 1;
    case BlockKind::RegionNodes:
    case BlockKind::FrameVertices:
    case BlockKind::SurfaceVertices:
    case BlockKind::ObservedPositions:
    case BlockKind::BaseOutsidePositions:
    case BlockKind::FootpointBarycentrics:
    case BlockKind::AuthoredNormals:
    case BlockKind::StateNodes:
    case BlockKind::ExpectedVisible: return 12;
    case BlockKind::Tetrahedra:
    case BlockKind::BaseQuads: return 16;
    case BlockKind::NodeSet:
    case BlockKind::CanonicalIds:
    case BlockKind::FootpointTriangles: return 4;
    case BlockKind::PhaseFraction:
    case BlockKind::MembraneStiffness:
    case BlockKind::CableStiffness:
    case BlockKind::FrameCover:
    case BlockKind::IdValues:
    case BlockKind::FootpointOffsets: return 4;
    case BlockKind::MembraneTriangles:
    case BlockKind::SheetFaces:
    case BlockKind::ShellStitch:
    case BlockKind::FrameTriangles:
    case BlockKind::SurfaceTriangles:
    case BlockKind::BaseTriangles: return 12;
    case BlockKind::CableEdges: return 8;
    case BlockKind::FrameCoverProvenance: return 1;
    case BlockKind::QuadraticTetrahedra: return 40;
  }
  return 0;
}

const char* block_kind_name(u32 kind) noexcept {
  switch (static_cast<BlockKind>(kind)) {
    case BlockKind::Definition: return "Definition";
    case BlockKind::Names: return "Names";
    case BlockKind::RegionNodes: return "RegionNodes";
    case BlockKind::Tetrahedra: return "Tetrahedra";
    case BlockKind::NodeSet: return "NodeSet";
    case BlockKind::PhaseFraction: return "PhaseFraction";
    case BlockKind::MembraneTriangles: return "MembraneTriangles";
    case BlockKind::MembraneStiffness: return "MembraneStiffness";
    case BlockKind::CableEdges: return "CableEdges";
    case BlockKind::CableStiffness: return "CableStiffness";
    case BlockKind::SheetFaces: return "SheetFaces";
    case BlockKind::ShellStitch: return "ShellStitch";
    case BlockKind::FrameVertices: return "FrameVertices";
    case BlockKind::FrameTriangles: return "FrameTriangles";
    case BlockKind::FrameCover: return "FrameCover";
    case BlockKind::FrameCoverProvenance: return "FrameCoverProvenance";
    case BlockKind::SurfaceVertices: return "SurfaceVertices";
    case BlockKind::SurfaceTriangles: return "SurfaceTriangles";
    case BlockKind::BaseTriangles: return "BaseTriangles";
    case BlockKind::BaseQuads: return "BaseQuads";
    case BlockKind::ObservedPositions: return "ObservedPositions";
    case BlockKind::BaseOutsidePositions: return "BaseOutsidePositions";
    case BlockKind::CanonicalIds: return "CanonicalIds";
    case BlockKind::IdValues: return "IdValues";
    case BlockKind::FootpointTriangles: return "FootpointTriangles";
    case BlockKind::FootpointBarycentrics: return "FootpointBarycentrics";
    case BlockKind::FootpointOffsets: return "FootpointOffsets";
    case BlockKind::AuthoredNormals: return "AuthoredNormals";
    case BlockKind::StateNodes: return "StateNodes";
    case BlockKind::ExpectedVisible: return "ExpectedVisible";
    case BlockKind::QuadraticTetrahedra: return "QuadraticTetrahedra";
  }
  return "unknown";
}

const TissueBlock* TissueFile::find(std::string_view name) const noexcept {
  for (const TissueBlock& b : blocks)
    if (b.name == name) return &b;
  return nullptr;
}

void seal_block(TissueFile& file, const TissueBlock& block) {
  Block* entry = table_entry(file.definition, block.name);
  if (entry == nullptr) {
    file.definition.blocks.push_back(Block{});
    entry = &file.definition.blocks.back();
    entry->name = block.name;
  }
  entry->kind = block.kind;
  entry->count = block.count;
  entry->bytes = block.bytes.size();
  entry->sha256 = sha256_hex(block.bytes);
}

std::string topology_sha256(std::span<const u32> indices) {
  Sha256 hasher;
  u8 word[8];
  for (const u32 index : indices) {
    const u64 wide = index;
    for (u32 b = 0; b < 8; ++b)
      word[b] = static_cast<u8>(wide >> (8u * b));
    hasher.update(std::span<const u8>(word, 8));
  }
  return to_hex(hasher.finish());
}

// ---- the container
// --------------------------------------------------------------------------------

bool encode_tissue_file(const TissueFile& file, Vector<u8>& out, std::string* error) {
  out.clear();
  TissueDefinition definition = file.definition;
  for (Block& entry : definition.blocks) {
    const TissueBlock* block = file.find(entry.name);
    if (block == nullptr)
      return fail(error, "the block table lists '" + entry.name + "', which holds no bytes");
    const u32 element = block_element_size(block->kind);
    if (element == 0 || static_cast<u32>(block->kind) < 10)
      return fail(error, "block '" + entry.name + "' is of a kind this build cannot write");
    if (block->bytes.size() != block->count * element)
      return fail(error, "block '" + entry.name + "' holds " + std::to_string(block->bytes.size()) +
                             " bytes for " + std::to_string(block->count) + " elements of " +
                             std::to_string(element));
    entry.kind = block->kind;
    entry.file.clear();
    entry.count = block->count;
    entry.bytes = block->bytes.size();
    entry.sha256 = sha256_hex(block->bytes);
  }
  for (const TissueBlock& block : file.blocks)
    if (table_entry(definition, block.name) == nullptr)
      return fail(error, "block '" + block.name + "' is not in the definition's table");

  std::string json;
  if (!write_json(schema::to_json(definition), json, JsonWriteOptions{.pretty = false}))
    return fail(error, "the definition holds a number JSON cannot represent");

  // Names, one-based offsets into them.
  Vector<u8> names;
  Vector<u32> name_offset;
  for (const TissueBlock& block : file.blocks) {
    name_offset.push_back(static_cast<u32>(names.size()) + 1u);
    for (const char c : block.name)
      names.push_back(static_cast<u8>(c));
    names.push_back(0);
  }

  struct Payload {
    TissueFileSection record;
    const u8* data;
    u64 size;
  };
  Vector<Payload> payloads;
  payloads.push_back(
      Payload{TissueFileSection{static_cast<u32>(BlockKind::Definition), 1, json.size(), 0, 0, 0},
              reinterpret_cast<const u8*>(json.data()), json.size()});
  payloads.push_back(
      Payload{TissueFileSection{static_cast<u32>(BlockKind::Names), 1, names.size(), 0, 0, 0},
              names.data(), names.size()});
  for (u32 i = 0; i < file.blocks.size(); ++i) {
    const TissueBlock& block = file.blocks[i];
    payloads.push_back(
        Payload{TissueFileSection{static_cast<u32>(block.kind), block_element_size(block.kind),
                                  block.count, 0, name_offset[i], 0},
                block.bytes.data(), block.bytes.size()});
  }

  u64 at = align_up(k_header_bytes + payloads.size() * k_section_bytes);
  for (Payload& p : payloads) {
    p.record.offset = at;
    at = align_up(at + p.size);
  }
  const u64 total = at;
  out.assign(static_cast<u32>(total), u8{0});
  TissueFileHeader header;
  header.section_count = static_cast<u32>(payloads.size());
  header.total_bytes = total;
  for (u32 i = 0; i < payloads.size(); ++i) {
    std::memcpy(out.data() + k_header_bytes + i * k_section_bytes, &payloads[i].record,
                sizeof(TissueFileSection));
    if (payloads[i].size > 0)
      std::memcpy(out.data() + payloads[i].record.offset, payloads[i].data, payloads[i].size);
  }
  header.content_hash = hash_bytes(out.data() + k_header_bytes, total - k_header_bytes);
  std::memcpy(out.data(), &header, sizeof(header));
  return true;
}

bool write_tissue_file(std::string_view path, const TissueFile& file, std::string* error) {
  Vector<u8> bytes;
  if (!encode_tissue_file(file, bytes, error)) return false;
  const io::Status status = io::write_file_atomic(
      path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  if (status != io::Status::Ok)
    return fail(error,
                std::string("cannot write ") + std::string(path) + ": " + io::status_name(status));
  return true;
}

bool read_tissue_file_info(std::span<const u8> bytes, TissueFileInfo& out, std::string* error) {
  out = TissueFileInfo{};
  if (bytes.size() < k_header_bytes) return fail(error, "the file is shorter than its header");
  std::memcpy(&out.header, bytes.data(), sizeof(TissueFileHeader));
  if (std::memcmp(out.header.magic, "TISS", 4) != 0) return fail(error, "not a tissue container");
  if (out.header.version != k_tissue_file_version)
    return fail(error, "container version " + std::to_string(out.header.version) +
                           " is not the version " + std::to_string(k_tissue_file_version) +
                           " this build reads");
  if (out.header.total_bytes != bytes.size())
    return fail(error, "the file is " + std::to_string(bytes.size()) +
                           " bytes and its header says " + std::to_string(out.header.total_bytes));
  const u64 table_end = k_header_bytes + u64{out.header.section_count} * k_section_bytes;
  if (table_end > bytes.size()) return fail(error, "the section table runs past the end");
  if (hash_bytes(bytes.data() + k_header_bytes, bytes.size() - k_header_bytes) !=
      out.header.content_hash)
    return fail(error, "the content hash does not match: the file is damaged");
  // The names, if there are any, to label the sections.
  std::span<const u8> names;
  Vector<TissueFileSection> records(out.header.section_count);
  for (u32 i = 0; i < out.header.section_count; ++i) {
    std::memcpy(&records[i], bytes.data() + k_header_bytes + i * k_section_bytes,
                sizeof(TissueFileSection));
    const TissueFileSection& s = records[i];
    const u64 size = s.element_count * s.element_size;
    if (s.element_size != 0 && s.element_count > (bytes.size() / s.element_size))
      return fail(error, "section " + std::to_string(i) + " runs past the end of the file");
    if (s.offset % k_tissue_file_alignment != 0 || s.offset < table_end ||
        s.offset + size > bytes.size())
      return fail(error, "section " + std::to_string(i) + " lies outside the file");
    if (s.kind == static_cast<u32>(BlockKind::Names) && s.element_size == 1)
      names = bytes.subspan(s.offset, size);
  }
  for (const TissueFileSection& s : records) {
    TissueFileSectionInfo info;
    info.kind = s.kind;
    info.kind_name = block_kind_name(s.kind);
    info.element_size = s.element_size;
    info.element_count = s.element_count;
    info.offset = s.offset;
    const u32 known_size = block_element_size(static_cast<BlockKind>(s.kind));
    info.known = known_size != 0 && known_size == s.element_size;
    if (s.name > 0 && s.name <= names.size()) {
      for (usize c = s.name - 1; c < names.size() && names[c] != 0; ++c)
        info.name.push_back(static_cast<char>(names[c]));
    }
    out.sections.push_back(std::move(info));
  }
  return true;
}

bool read_tissue_file_memory(std::span<const u8> bytes, TissueFile& out, std::string* error) {
  out = TissueFile{};
  TissueFileInfo info;
  if (!read_tissue_file_info(bytes, info, error)) return false;
  const TissueFileSectionInfo* definition = nullptr;
  for (const TissueFileSectionInfo& s : info.sections)
    if (s.kind == static_cast<u32>(BlockKind::Definition) && s.element_size == 1) definition = &s;
  if (definition == nullptr) return fail(error, "the container holds no definition");
  const std::string_view text(reinterpret_cast<const char*>(bytes.data() + definition->offset),
                              static_cast<usize>(definition->element_count));
  TissueFile file;
  if (!definition_from_json(text, false, file.definition, &file.warnings, error)) return false;
  if (file.definition.format != k_tissue_format)
    return fail(error, "the definition's format is '" + file.definition.format + "', not '" +
                           k_tissue_format + "'");

  for (const TissueFileSectionInfo& s : info.sections) {
    if (s.kind == static_cast<u32>(BlockKind::Definition) ||
        s.kind == static_cast<u32>(BlockKind::Names))
      continue;
    if (!s.known) {
      file.warnings.push_back("section '" + s.name + "' is of kind " + std::to_string(s.kind) +
                              ", which this build does not know; it is skipped");
      continue;
    }
    const Block* entry = table_entry(file.definition, s.name);
    if (entry == nullptr) {
      file.warnings.push_back("section '" + s.name +
                              "' is not in the definition's table; it is skipped");
      continue;
    }
    if (static_cast<u32>(entry->kind) != s.kind || entry->count != s.element_count)
      return fail(error,
                  "section '" + s.name + "' disagrees with its table entry on its kind or count");
    TissueBlock block;
    block.name = s.name;
    block.kind = entry->kind;
    block.count = s.element_count;
    const u64 size = s.element_count * s.element_size;
    block.bytes.assign(bytes.data() + s.offset, bytes.data() + s.offset + size);
    if (sha256_hex(block.bytes) != entry->sha256)
      return fail(error, "block '" + s.name + "' does not match the SHA-256 its table records");
    file.blocks.push_back(std::move(block));
  }
  // The table's order, and a warning for anything it lists that no section held.
  Vector<TissueBlock> ordered;
  Vector<Block> table;
  for (const Block& entry : file.definition.blocks) {
    TissueBlock* found = nullptr;
    for (TissueBlock& b : file.blocks)
      if (b.name == entry.name) found = &b;
    if (found == nullptr) {
      file.warnings.push_back("block '" + entry.name +
                              "' is in the table but in no section; what names it falls back");
      continue;
    }
    ordered.push_back(std::move(*found));
    table.push_back(entry);
  }
  file.blocks = std::move(ordered);
  file.definition.blocks = std::move(table);
  out = std::move(file);
  return true;
}

bool read_tissue_file(std::string_view path, TissueFile& out, std::string* error) {
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok)
    return fail(error,
                std::string("cannot read ") + std::string(path) + ": " + io::status_name(status));
  return read_tissue_file_memory(
      std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()), out, error);
}

// ---- the interchange
// --------------------------------------------------------------------------------

bool import_interchange(std::string_view json_path, TissueFile& out, std::string* error) {
  out = TissueFile{};
  std::string text;
  const io::Status status = io::read_file(json_path, text);
  if (status != io::Status::Ok)
    return fail(error, std::string("cannot read ") + std::string(json_path) + ": " +
                           io::status_name(status));
  TissueFile file;
  if (!definition_from_json(text, true, file.definition, nullptr, error)) return false;
  if (file.definition.format != k_tissue_format)
    return fail(error, "the definition's format is '" + file.definition.format + "', not '" +
                           k_tissue_format + "'");
  const std::string directory(io::parent_path(json_path));
  for (const Block& entry : file.definition.blocks) {
    if (entry.name.empty()) return fail(error, "a block has no name");
    for (const TissueBlock& b : file.blocks)
      if (b.name == entry.name) return fail(error, "two blocks are named '" + entry.name + "'");
    const u32 element = block_element_size(entry.kind);
    if (element == 0 || static_cast<u32>(entry.kind) < 10)
      return fail(error, "block '" + entry.name + "' is of a kind an interchange cannot carry");
    if (entry.bytes != entry.count * element)
      return fail(error, "block '" + entry.name + "' says " + std::to_string(entry.bytes) +
                             " bytes for " + std::to_string(entry.count) + " elements of " +
                             std::to_string(element) + " bytes");
    if (entry.file.empty()) return fail(error, "block '" + entry.name + "' names no file");
    std::string bytes;
    const std::string path = io::join_path(directory, entry.file);
    const io::Status read = io::read_file(path, bytes);
    if (read != io::Status::Ok)
      return fail(error,
                  "block '" + entry.name + "': cannot read " + path + ": " + io::status_name(read));
    if (bytes.size() != entry.bytes)
      return fail(error, "block '" + entry.name + "' is " + std::to_string(bytes.size()) +
                             " bytes on disk and its table says " + std::to_string(entry.bytes));
    TissueBlock block;
    block.name = entry.name;
    block.kind = entry.kind;
    block.count = entry.count;
    block.bytes.assign(reinterpret_cast<const u8*>(bytes.data()),
                       reinterpret_cast<const u8*>(bytes.data()) + bytes.size());
    const std::string hash = sha256_hex(block.bytes);
    if (hash != entry.sha256)
      return fail(error, "block '" + entry.name + "' has SHA-256 " + hash + " and its table says " +
                             entry.sha256);
    file.blocks.push_back(std::move(block));
  }
  out = std::move(file);
  return true;
}

bool export_interchange(const TissueFile& file, std::string_view directory, std::string_view stem,
                        std::string* error) {
  TissueFile copy;
  copy.definition = file.definition;
  copy.blocks = file.blocks;
  for (const TissueBlock& block : copy.blocks) {
    seal_block(copy, block);
    Block* entry = table_entry(copy.definition, block.name);
    entry->file = std::string(stem) + "." + safe_file_name(block.name) + ".bin";
    const std::string path = io::join_path(directory, entry->file);
    const io::Status status = io::write_file_atomic(
        path,
        std::string_view(reinterpret_cast<const char*>(block.bytes.data()), block.bytes.size()));
    if (status != io::Status::Ok)
      return fail(error, "cannot write " + path + ": " + io::status_name(status));
  }
  std::string json;
  if (!write_json(schema::to_json(copy.definition), json, JsonWriteOptions{.pretty = true}))
    return fail(error, "the definition holds a number JSON cannot represent");
  json.push_back('\n');
  const std::string path = io::join_path(directory, std::string(stem) + ".json");
  const io::Status status = io::write_file_atomic(path, json);
  if (status != io::Status::Ok)
    return fail(error, "cannot write " + path + ": " + io::status_name(status));
  return true;
}

}  // namespace engine::tissue
