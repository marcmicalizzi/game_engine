#include <core/hash/hash.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <cstddef>
#include <cstring>
#include <utility>

namespace engine::geometry {
namespace {

constexpr u64 k_header_bytes = sizeof(ClusterFileHeader);
constexpr u64 k_record_bytes = sizeof(ClusterFileSection);
constexpr u32 k_kind_count = static_cast<u32>(ClusterSection::Quantized) + 1;

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
  payloads.reserve(12);
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

bool read_cluster_file_memory(std::span<const u8> bytes, ClusterFileData& out, std::string* error) {
  out = ClusterFileData{};
  if (bytes.size() < k_header_bytes) {
    return fail(error, "cluster file is truncated: " + std::to_string(bytes.size()) +
                           " bytes, the 32-byte header does not fit");
  }
  ClusterFileHeader header;
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

  Vector<ClusterFileSection> sections(header.section_count);
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
    if (material.base_color_image >= image_count || material.normal_image >= image_count) {
      return fail(error, "cluster file material names an image outside the " +
                             std::to_string(image_count) + " image paths");
    }
  }

  out = std::move(result);
  return true;
}

}  // namespace engine::geometry
