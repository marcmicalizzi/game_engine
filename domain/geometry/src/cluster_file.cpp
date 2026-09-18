#include <core/hash/hash.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <cstddef>
#include <cstring>
#include <utility>

namespace engine::geometry {

u32 encode_alpha_word(u8 alpha_mode, bool double_sided, f32 alpha_cutoff) noexcept {
  const f32 clamped = alpha_cutoff < 0.0f ? 0.0f : (alpha_cutoff > 1.0f ? 1.0f : alpha_cutoff);
  const u32 cutoff = static_cast<u32>(clamped * 65535.0f + 0.5f);
  return (alpha_mode & 3u) | (double_sided ? 4u : 0u) | (cutoff << 16);
}

u8 alpha_word_mode(u32 word) noexcept { return static_cast<u8>(word & 3u); }

bool alpha_word_double_sided(u32 word) noexcept { return (word & 4u) != 0u; }

f32 alpha_word_cutoff(u32 word) noexcept { return static_cast<f32>(word >> 16) / 65535.0f; }

namespace {

constexpr u64 k_header_bytes = sizeof(ClusterFileHeader);
constexpr u64 k_record_bytes = sizeof(ClusterFileSection);
constexpr u32 k_kind_count = static_cast<u32>(ClusterSection::PageChildren) + 1;

constexpr u64 align_up(u64 value) noexcept {
  const u64 a = k_cluster_file_alignment;
  return (value + a - 1) / a * a;
}

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// One section on its way out: the record as it will be written plus the bytes it points at,
// which live in the caller's frame for as long as `encode` runs.
struct Payload {
  ClusterFileSection record;
  const void* data = nullptr;
};

void add_section(Vector<Payload>& out, ClusterSection kind, u32 element_size, u64 element_count,
                 const void* data) {
  Payload payload;
  payload.record.kind = static_cast<u32>(kind);
  payload.record.element_size = element_size;
  payload.record.element_count = element_count;
  payload.data = data;
  out.push_back(payload);
}

// Serializes `data` into `out` and returns the content hash the header carries.
u64 encode(const ClusterFileData& data, std::string& out) {
  // The string table: every image path NUL-terminated, back to back, with its byte offset.
  Vector<u8> strings;
  Vector<u32> path_offsets;
  path_offsets.reserve(data.image_paths.size());
  for (const std::string& path : data.image_paths) {
    path_offsets.push_back(strings.size());
    strings.append(std::span<const u8>(reinterpret_cast<const u8*>(path.data()), path.size()));
    strings.push_back(u8{0});
  }

  ClusterFileScalars scalars;
  scalars.group_count = data.mesh.group_count;
  scalars.leaf_triangle_count = data.mesh.leaf_triangle_count;
  scalars.source_vertex_count = data.mesh.mesh.source_vertex_count;
  scalars.source_triangle_count = data.mesh.mesh.source_triangle_count;
  // The 16-bit grid: origin in pad[0..2], step in pad[3], as float bits.
  std::memcpy(&scalars.pad[0], &data.mesh.mesh.quant_origin.x, sizeof(f32));
  std::memcpy(&scalars.pad[1], &data.mesh.mesh.quant_origin.y, sizeof(f32));
  std::memcpy(&scalars.pad[2], &data.mesh.mesh.quant_origin.z, sizeof(f32));
  std::memcpy(&scalars.pad[3], &data.mesh.mesh.quant_scale, sizeof(f32));

  const ClusterMesh& mesh = data.mesh.mesh;
  Vector<Payload> payloads;
  payloads.reserve(16);
  add_section(payloads, ClusterSection::Clusters, static_cast<u32>(sizeof(ClusterDesc)),
              mesh.clusters.size(), mesh.clusters.data());
  add_section(payloads, ClusterSection::Lod, static_cast<u32>(sizeof(ClusterLodDesc)),
              data.mesh.lod.size(), data.mesh.lod.data());
  add_section(payloads, ClusterSection::Vertices, static_cast<u32>(sizeof(Vec3)),
              mesh.vertices.size(), mesh.vertices.data());
  add_section(payloads, ClusterSection::Attributes, static_cast<u32>(sizeof(VertexAttributes)),
              mesh.attributes.size(), mesh.attributes.data());
  add_section(payloads, ClusterSection::Triangles, static_cast<u32>(sizeof(u32)),
              mesh.triangles.size(), mesh.triangles.data());
  add_section(payloads, ClusterSection::VertexSource, static_cast<u32>(sizeof(u32)),
              mesh.vertex_source.size(), mesh.vertex_source.data());
  add_section(payloads, ClusterSection::LevelClusterCounts, static_cast<u32>(sizeof(u32)),
              data.mesh.level_cluster_counts.size(), data.mesh.level_cluster_counts.data());
  add_section(payloads, ClusterSection::ClusterMaterial, static_cast<u32>(sizeof(u32)),
              data.cluster_material.size(), data.cluster_material.data());
  add_section(payloads, ClusterSection::Materials, static_cast<u32>(sizeof(ClusterFileMaterial)),
              data.materials.size(), data.materials.data());
  add_section(payloads, ClusterSection::ImagePaths, static_cast<u32>(sizeof(u32)),
              path_offsets.size(), path_offsets.data());
  add_section(payloads, ClusterSection::Strings, 1u, strings.size(), strings.data());
  add_section(payloads, ClusterSection::Scalars, static_cast<u32>(sizeof(ClusterFileScalars)), 1u,
              &scalars);
  add_section(payloads, ClusterSection::Quantized, static_cast<u32>(sizeof(u16)),
              mesh.quantized.size(), mesh.quantized.data());
  // The source path, NUL included, so a reader gets a C string straight out of the mapping.
  add_section(payloads, ClusterSection::SourcePath, 1u, data.source_path.size() + 1,
              data.source_path.c_str());
  // What the source was and what was built from it, for an incremental build to compare.
  const u64 identity[2] = {data.source_hash, data.build_key};
  add_section(payloads, ClusterSection::SourceHash, static_cast<u32>(sizeof(u64)), 2u, identity);
  // The streaming page table. Both sections are written even when there is no table, so the
  // section list of a container does not depend on how it was built.
  add_section(payloads, ClusterSection::Pages, static_cast<u32>(sizeof(ClusterPageDesc)),
              data.pages.pages.size(), data.pages.pages.data());
  add_section(payloads, ClusterSection::PageChildren, static_cast<u32>(sizeof(u32)),
              data.pages.child_pages.size(), data.pages.child_pages.data());

  u64 offset = k_header_bytes + k_record_bytes * payloads.size();
  for (Payload& payload : payloads) {
    offset = align_up(offset);
    payload.record.offset = offset;
    offset += u64{payload.record.element_size} * payload.record.element_count;
  }
  const u64 total = offset;

  out.assign(static_cast<usize>(total), '\0');
  char* base = out.data();
  ClusterFileHeader header;
  header.section_count = payloads.size();
  header.total_bytes = total;
  std::memcpy(base, &header, sizeof(header));
  u64 record_at = k_header_bytes;
  for (const Payload& payload : payloads) {
    std::memcpy(base + record_at, &payload.record, sizeof(payload.record));
    record_at += k_record_bytes;
    const u64 span_bytes = u64{payload.record.element_size} * payload.record.element_count;
    if (span_bytes != 0)
      std::memcpy(base + payload.record.offset, payload.data, static_cast<usize>(span_bytes));
  }
  const u64 content_hash =
      hash_bytes(base + k_header_bytes, static_cast<usize>(total - k_header_bytes));
  std::memcpy(base + offsetof(ClusterFileHeader, content_hash), &content_hash,
              sizeof(content_hash));
  return content_hash;
}

template <class T>
void copy_section(std::span<const u8> bytes, const ClusterFileSection& section, Vector<T>& out) {
  const u32 count = static_cast<u32>(section.element_count);
  out.resize(count);
  if (count != 0)
    std::memcpy(out.data(), bytes.data() + section.offset, static_cast<usize>(count) * sizeof(T));
}

std::string section_label(u32 kind) {
  return std::string(cluster_section_name(kind)) + " (kind " + std::to_string(kind) + ")";
}

// Everything that can be checked without interpreting a payload: the header, the section table,
// that no section runs past the end, and the content hash. Both readers start here, so a file
// they disagree about does not exist.
bool check_container(std::span<const u8> bytes, ClusterFileHeader& header,
                     Vector<ClusterFileSection>& sections, std::string* error) {
  if (bytes.size() < k_header_bytes) {
    return fail(error, "cluster file is truncated: " + std::to_string(bytes.size()) +
                           " bytes, the 32-byte header does not fit");
  }
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (std::memcmp(header.magic, "CLST", 4) != 0)
    return fail(error, "not a cluster file: the magic is not \"CLST\"");
  if (header.version != k_cluster_file_version) {
    return fail(error, "unsupported cluster file version " + std::to_string(header.version) +
                           ": this build reads version " + std::to_string(k_cluster_file_version));
  }
  if (header.total_bytes < k_header_bytes || bytes.size() < header.total_bytes) {
    return fail(error, "cluster file is truncated: " + std::to_string(bytes.size()) +
                           " bytes, the header says " + std::to_string(header.total_bytes));
  }
  const u64 table_end = k_header_bytes + k_record_bytes * u64{header.section_count};
  if (table_end > header.total_bytes) {
    return fail(error, "cluster file is truncated: the table of " +
                           std::to_string(header.section_count) + " sections does not fit");
  }

  sections.resize(header.section_count);
  for (u32 i = 0; i < header.section_count; ++i) {
    std::memcpy(&sections[i], bytes.data() + k_header_bytes + k_record_bytes * i, k_record_bytes);
  }
  // Structure before contents: a section that runs past the end is named as such rather than
  // reported as a hash mismatch, and nothing below reads outside the file.
  for (const ClusterFileSection& section : sections) {
    if (section.element_count > 0xffffffffull) {
      return fail(error, "cluster file section " + section_label(section.kind) + " holds " +
                             std::to_string(section.element_count) +
                             " elements, more than this build reads");
    }
    const u64 span_bytes = u64{section.element_size} * section.element_count;
    if (section.offset < k_header_bytes || section.offset > header.total_bytes ||
        span_bytes > header.total_bytes - section.offset) {
      return fail(error, "cluster file section " + section_label(section.kind) +
                             " extends past the end of the file");
    }
  }
  const u64 content_hash = hash_bytes(bytes.data() + k_header_bytes,
                                      static_cast<usize>(header.total_bytes - k_header_bytes));
  if (content_hash != header.content_hash) {
    return fail(error, "cluster file content hash mismatch: the header says " +
                           std::to_string(header.content_hash) + ", the contents give " +
                           std::to_string(content_hash));
  }
  return true;
}

// The two identity words of a checked container, zero when it records none.
bool read_identity(std::span<const u8> bytes, std::span<const ClusterFileSection> sections,
                   u64& source_hash, u64& build_key, std::string* error) {
  source_hash = 0;
  build_key = 0;
  for (const ClusterFileSection& section : sections) {
    if (section.kind != static_cast<u32>(ClusterSection::SourceHash)) continue;
    if (section.element_count == 0) return true;
    if (section.element_size != sizeof(u64)) {
      return fail(error, "cluster file section source_hash has " +
                             std::to_string(section.element_size) + "-byte elements, expected 8");
    }
    std::memcpy(&source_hash, bytes.data() + section.offset, sizeof(u64));
    if (section.element_count >= 2)
      std::memcpy(&build_key, bytes.data() + section.offset + sizeof(u64), sizeof(u64));
    return true;
  }
  return true;
}

}  // namespace

const char* cluster_section_name(u32 kind) noexcept {
  switch (static_cast<ClusterSection>(kind)) {
    case ClusterSection::Clusters: return "clusters";
    case ClusterSection::Lod: return "lod";
    case ClusterSection::Vertices: return "vertices";
    case ClusterSection::Attributes: return "attributes";
    case ClusterSection::Triangles: return "triangles";
    case ClusterSection::VertexSource: return "vertex_source";
    case ClusterSection::LevelClusterCounts: return "level_cluster_counts";
    case ClusterSection::ClusterMaterial: return "cluster_material";
    case ClusterSection::Materials: return "materials";
    case ClusterSection::ImagePaths: return "image_paths";
    case ClusterSection::Strings: return "strings";
    case ClusterSection::Scalars: return "scalars";
    case ClusterSection::Quantized: return "quantized";
    case ClusterSection::SourcePath: return "source_path";
    case ClusterSection::SourceHash: return "source_hash";
    case ClusterSection::Pages: return "pages";
    case ClusterSection::PageChildren: return "page_children";
  }
  return "unknown";
}

u64 cluster_file_hash(const ClusterFileData& data) {
  std::string bytes;
  return encode(data, bytes);
}

bool write_cluster_file(std::string_view path, const ClusterFileData& data, std::string* error) {
  std::string bytes;
  encode(data, bytes);
  const io::Status status = io::write_file_atomic(path, bytes);
  if (status != io::Status::Ok) {
    return fail(
        error, "cannot write cluster file '" + std::string(path) + "': " + io::status_name(status));
  }
  return true;
}

bool read_cluster_file(std::string_view path, ClusterFileData& out, std::string* error) {
  out = ClusterFileData{};
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok) {
    return fail(error,
                "cannot read cluster file '" + std::string(path) + "': " + io::status_name(status));
  }
  return read_cluster_file_memory(
      std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()), out, error);
}

bool read_cluster_file_identity(std::string_view path, u64& source_hash, u64& build_key,
                                std::string* error) {
  source_hash = 0;
  build_key = 0;
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok) {
    return fail(error,
                "cannot read cluster file '" + std::string(path) + "': " + io::status_name(status));
  }
  const std::span<const u8> view(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  ClusterFileHeader header;
  Vector<ClusterFileSection> sections;
  if (!check_container(view, header, sections, error)) return false;
  return read_identity(view, std::span<const ClusterFileSection>(sections.data(), sections.size()),
                       source_hash, build_key, error);
}

bool read_cluster_file_memory(std::span<const u8> bytes, ClusterFileData& out, std::string* error) {
  out = ClusterFileData{};
  ClusterFileHeader header;
  Vector<ClusterFileSection> sections;
  if (!check_container(bytes, header, sections, error)) return false;

  // Unknown kinds are skipped here: that is the forward-compatibility guarantee.
  const ClusterFileSection* found[k_kind_count] = {};
  for (const ClusterFileSection& section : sections) {
    if (section.kind == 0 || section.kind >= k_kind_count) continue;
    if (found[section.kind] == nullptr) found[section.kind] = &section;
  }

  ClusterFileData result;
  const ClusterFileSection* section = nullptr;
  bool ok = true;
  auto require = [&](ClusterSection kind, usize element_size) -> bool {
    const u32 index = static_cast<u32>(kind);
    section = found[index];
    if (section == nullptr) {
      ok = fail(error, "cluster file has no " + section_label(index) + " section");
      return false;
    }
    if (section->element_size != element_size) {
      ok = fail(error, "cluster file section " + section_label(index) + " has " +
                           std::to_string(section->element_size) + "-byte elements, expected " +
                           std::to_string(element_size));
      return false;
    }
    return true;
  };

  if (!require(ClusterSection::Clusters, sizeof(ClusterDesc))) return ok;
  copy_section(bytes, *section, result.mesh.mesh.clusters);
  if (!require(ClusterSection::Lod, sizeof(ClusterLodDesc))) return ok;
  copy_section(bytes, *section, result.mesh.lod);
  if (!require(ClusterSection::Vertices, sizeof(Vec3))) return ok;
  copy_section(bytes, *section, result.mesh.mesh.vertices);
  if (!require(ClusterSection::Attributes, sizeof(VertexAttributes))) return ok;
  copy_section(bytes, *section, result.mesh.mesh.attributes);
  if (!require(ClusterSection::Triangles, sizeof(u32))) return ok;
  copy_section(bytes, *section, result.mesh.mesh.triangles);
  if (!require(ClusterSection::VertexSource, sizeof(u32))) return ok;
  copy_section(bytes, *section, result.mesh.mesh.vertex_source);
  if (!require(ClusterSection::LevelClusterCounts, sizeof(u32))) return ok;
  copy_section(bytes, *section, result.mesh.level_cluster_counts);
  if (!require(ClusterSection::Scalars, sizeof(ClusterFileScalars))) return ok;
  if (section->element_count != 1) {
    return fail(error, "cluster file scalars section holds " +
                           std::to_string(section->element_count) + " records, expected one");
  }
  ClusterFileScalars scalars;
  std::memcpy(&scalars, bytes.data() + section->offset, sizeof(scalars));
  result.mesh.group_count = scalars.group_count;
  result.mesh.leaf_triangle_count = scalars.leaf_triangle_count;
  result.mesh.mesh.source_vertex_count = scalars.source_vertex_count;
  result.mesh.mesh.source_triangle_count = scalars.source_triangle_count;
  // The quantized stream and its grid; a file from before the section existed is requantized.
  if (const ClusterFileSection* quantized = found[static_cast<u32>(ClusterSection::Quantized)];
      quantized != nullptr) {
    if (quantized->element_size != sizeof(u16)) {
      return fail(error, "cluster file section quantized has " +
                             std::to_string(quantized->element_size) +
                             "-byte elements, expected 2");
    }
    copy_section(bytes, *quantized, result.mesh.mesh.quantized);
    std::memcpy(&result.mesh.mesh.quant_origin.x, &scalars.pad[0], sizeof(f32));
    std::memcpy(&result.mesh.mesh.quant_origin.y, &scalars.pad[1], sizeof(f32));
    std::memcpy(&result.mesh.mesh.quant_origin.z, &scalars.pad[2], sizeof(f32));
    std::memcpy(&result.mesh.mesh.quant_scale, &scalars.pad[3], sizeof(f32));
  } else {
    quantize_positions(result.mesh.mesh);
  }

  // The source path; a file from before the section existed leaves it empty, and so does a
  // build that had no file behind its bytes.
  if (const ClusterFileSection* source = found[static_cast<u32>(ClusterSection::SourcePath)];
      source != nullptr && source->element_count != 0) {
    if (source->element_size != 1) {
      return fail(error, "cluster file section source_path has " +
                             std::to_string(source->element_size) + "-byte elements, expected 1");
    }
    const char* text = reinterpret_cast<const char*>(bytes.data() + source->offset);
    const void* nul = std::memchr(text, 0, static_cast<usize>(source->element_count));
    if (nul == nullptr) return fail(error, "cluster file source path is not NUL-terminated");
    result.source_path.assign(text, static_cast<usize>(static_cast<const char*>(nul) - text));
  }

  // The source's identity; zero when the file records none, which reads as "rebuild it".
  if (!read_identity(bytes, std::span<const ClusterFileSection>(sections.data(), sections.size()),
                     result.source_hash, result.build_key, error)) {
    return false;
  }

  // The material map, the materials, and the image paths are optional: a mesh may carry none.
  if (const ClusterFileSection* materials = found[static_cast<u32>(ClusterSection::Materials)];
      materials != nullptr) {
    if (materials->element_size != sizeof(ClusterFileMaterial)) {
      return fail(error, "cluster file section materials has " +
                             std::to_string(materials->element_size) +
                             "-byte elements, expected 64");
    }
    copy_section(bytes, *materials, result.materials);
  }
  if (const ClusterFileSection* map = found[static_cast<u32>(ClusterSection::ClusterMaterial)];
      map != nullptr) {
    if (map->element_size != sizeof(u32)) {
      return fail(error, "cluster file section cluster_material has " +
                             std::to_string(map->element_size) + "-byte elements, expected 4");
    }
    copy_section(bytes, *map, result.cluster_material);
  }
  const ClusterFileSection* paths = found[static_cast<u32>(ClusterSection::ImagePaths)];
  const ClusterFileSection* strings = found[static_cast<u32>(ClusterSection::Strings)];
  if (paths != nullptr && paths->element_count != 0) {
    if (paths->element_size != sizeof(u32))
      return fail(error, "cluster file section image_paths does not hold 4-byte offsets");
    if (strings == nullptr || strings->element_size != 1)
      return fail(error, "cluster file has image paths but no string table");
    Vector<u32> offsets;
    copy_section(bytes, *paths, offsets);
    const u8* blob = bytes.data() + strings->offset;
    const u64 blob_size = strings->element_count;
    result.image_paths.reserve(offsets.size());
    for (const u32 at : offsets) {
      if (at >= blob_size) {
        return fail(error, "cluster file image path offset " + std::to_string(at) +
                               " is outside the " + std::to_string(blob_size) +
                               "-byte string table");
      }
      const void* nul = std::memchr(blob + at, 0, static_cast<usize>(blob_size - at));
      if (nul == nullptr) return fail(error, "cluster file string table is not NUL-terminated");
      const usize length = static_cast<usize>(static_cast<const u8*>(nul) - (blob + at));
      result.image_paths.push_back(std::string(reinterpret_cast<const char*>(blob + at), length));
    }
  }

  // Counts that the rest of the engine indexes by, checked once here so no caller has to.
  const ClusterMesh& mesh = result.mesh.mesh;
  if (result.mesh.lod.size() != mesh.clusters.size()) {
    return fail(error, "cluster file has " + std::to_string(mesh.clusters.size()) +
                           " clusters but " + std::to_string(result.mesh.lod.size()) +
                           " LOD descriptors");
  }
  if (mesh.vertex_source.size() != mesh.vertices.size() ||
      mesh.attributes.size() != mesh.vertices.size()) {
    return fail(error, "cluster file vertex streams differ in length: " +
                           std::to_string(mesh.vertices.size()) + " positions, " +
                           std::to_string(mesh.attributes.size()) + " attributes, " +
                           std::to_string(mesh.vertex_source.size()) + " sources");
  }
  u64 level_total = 0;
  for (const u32 count : result.mesh.level_cluster_counts)
    level_total += count;
  if (level_total != mesh.clusters.size()) {
    return fail(error, "cluster file level counts add up to " + std::to_string(level_total) +
                           ", not to the " + std::to_string(mesh.clusters.size()) + " clusters");
  }
  if (!result.cluster_material.empty()) {
    if (result.cluster_material.size() != mesh.clusters.size()) {
      return fail(error, "cluster file has " + std::to_string(result.cluster_material.size()) +
                             " material indices for " + std::to_string(mesh.clusters.size()) +
                             " clusters");
    }
    for (const u32 material : result.cluster_material) {
      if (material >= result.materials.size()) {
        return fail(error, "cluster file cluster material " + std::to_string(material) +
                               " is outside the " + std::to_string(result.materials.size()) +
                               " materials");
      }
    }
  }
  const i32 image_count = static_cast<i32>(result.image_paths.size());
  for (const ClusterFileMaterial& material : result.materials) {
    const i32 slots[5] = {material.base_color_image, material.normal_image,
                          decode_optional_image(material.metallic_roughness_image),
                          decode_optional_image(material.occlusion_image),
                          decode_optional_image(material.emissive_image)};
    for (const i32 slot : slots) {
      if (slot >= image_count) {
        return fail(error, "cluster file material names an image outside the " +
                               std::to_string(image_count) + " image paths");
      }
    }
  }

  // The streaming page table, once the mesh it indexes is known to be consistent. Only the page
  // descriptors and the flat child-page list are stored; the per-cluster page and child tables
  // are rebuilt from them and from the DAG, which is what keeps eight bytes a cluster off disk.
  // A file with no page table (an older build, or one built with pages switched off) reads with
  // an empty one rather than as a failure: the caller builds it if it wants it.
  if (const ClusterFileSection* table = found[static_cast<u32>(ClusterSection::Pages)];
      table != nullptr && table->element_count != 0) {
    if (table->element_size != sizeof(ClusterPageDesc)) {
      return fail(error, "cluster file section pages has " + std::to_string(table->element_size) +
                             "-byte elements, expected " + std::to_string(sizeof(ClusterPageDesc)));
    }
    copy_section(bytes, *table, result.pages.pages);
    if (const ClusterFileSection* kids = found[static_cast<u32>(ClusterSection::PageChildren)];
        kids != nullptr) {
      if (kids->element_size != sizeof(u32)) {
        return fail(error, "cluster file section page_children has " +
                               std::to_string(kids->element_size) + "-byte elements, expected 4");
      }
      copy_section(bytes, *kids, result.pages.child_pages);
    }
    // The byte target is a build knob, not something the renderer reads, and the scalars record
    // is full, so it is not stored: the largest page that was *not* flagged oversized is inside
    // the target by construction, and every oversized page is over it, which is exactly what the
    // validator needs and the closest a reader can get to the number the builder was given.
    result.pages.page_bytes_target = 0;
    for (const ClusterPageDesc& page : result.pages.pages) {
      if ((page.flags & k_page_oversized) == 0 && page.bytes > result.pages.page_bytes_target)
        result.pages.page_bytes_target = page.bytes;
    }
    std::string why;
    if (!rebuild_cluster_page_index(result.mesh, result.pages, &why))
      return fail(error, "cluster file page table: " + why);
  }

  out = std::move(result);
  return true;
}

u64 cluster_cache_key(u64 source_hash, const ClusterLodOptions& options, bool weld) noexcept {
  u64 key = hash_combine(source_hash, k_cluster_cache_version);
  key = hash_combine(key, options.max_triangles);
  key = hash_combine(key, options.max_vertices);
  const u64 flags = (options.ray_tracing ? 1ull : 0ull) | (options.normal_cones ? 2ull : 0ull) |
                    (weld ? 4ull : 0ull);
  return hash_combine(key, flags);
}

std::string cluster_cache_path(std::string_view ddc_root, u64 hash) {
  constexpr char k_digits[] = "0123456789abcdef";
  char name[17];
  for (u32 i = 0; i < 16; ++i)
    name[i] = k_digits[(hash >> ((15 - i) * 4)) & 0xfull];
  name[16] = '\0';
  return io::join_path(io::join_path(ddc_root, "clusters"), std::string(name) + ".clusters");
}

std::string find_ddc_root(std::string_view start) {
  std::string dir = io::normalize_path(start);
  // A repository is nowhere near this deep; the bound keeps a path helper that stops shortening
  // from spinning.
  for (u32 step = 0; step < 64; ++step) {
    if (dir.empty() || dir == ".") break;
    if (io::exists(io::join_path(dir, "AGENTS.md"))) return io::join_path(dir, "ddc");
    std::string parent(io::parent_path(dir));
    if (parent.empty() || parent == dir) break;
    dir = std::move(parent);
  }
  return std::string();
}

}  // namespace engine::geometry
