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
// The by-kind lookup table's width. It is the header's `k_cluster_section_kinds` and not a second
// copy of "the highest kind plus one": adding a kind and forgetting this line reads one past the
// end of a stack array, which is what it cost once.
constexpr u32 k_kind_count = k_cluster_section_kinds;

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

// Appends `text` to the string table and returns its byte offset.
u32 add_string(Vector<u8>& strings, std::string_view text) {
  const u32 at = strings.size();
  strings.append(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
  strings.push_back(u8{0});
  return at;
}

// Serializes `data` into `out` and returns the content hash the header carries.
u64 encode(const ClusterFileData& data, std::string& out) {
  // The string table: every image path NUL-terminated, back to back, with its byte offset.
  Vector<u8> strings;
  Vector<u32> path_offsets;
  path_offsets.reserve(data.image_paths.size());
  for (const std::string& path : data.image_paths)
    path_offsets.push_back(add_string(strings, path));

  // The images the source embedded: their encoded bytes back to back, one record apiece saying
  // where they landed, and their media types in the same string table. Two images with identical
  // bytes share one copy — an exporter packs occlusion, roughness, and metallic into one texture
  // and names it from three slots — and the hash is confirmed with a comparison, because a
  // 64-bit collision that quietly swapped two textures would be invisible in the picture.
  Vector<ClusterFileImage> image_records;
  Vector<u8> image_bytes;
  Vector<u32> mime_offsets;       // into `strings`, one per distinct media type
  Vector<std::string> mime_seen;  // parallel to `mime_offsets`, for the lookup
  image_records.reserve(data.images.size());
  for (const ClusterImage& image : data.images) {
    ClusterFileImage record;
    if (!image.bytes.empty()) {
      const u8* from = image.bytes.data();
      const usize size = image.bytes.size();
      record.hash = hash_bytes(from, size);
      record.bytes = image.bytes.size();
      bool shared = false;
      for (const ClusterFileImage& earlier : image_records) {
        if (earlier.bytes != record.bytes || earlier.hash != record.hash) continue;
        if (std::memcmp(image_bytes.data() + earlier.offset, from, size) != 0) continue;
        record.offset = earlier.offset;
        shared = true;
        break;
      }
      if (!shared) {
        record.offset = image_bytes.size();
        image_bytes.append(std::span<const u8>(from, size));
      }
    }
    if (!image.mime_type.empty()) {
      u32 at = ~u32{0};
      for (u32 i = 0; i < mime_seen.size(); ++i) {
        if (mime_seen[i] == image.mime_type) at = mime_offsets[i];
      }
      if (at == ~u32{0}) {
        at = add_string(strings, image.mime_type);
        mime_offsets.push_back(at);
        mime_seen.push_back(image.mime_type);
      }
      record.mime = at + 1;  // one-based: zero is "the source did not say"
    }
    image_records.push_back(record);
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
  payloads.reserve(24);
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
  const u32 page_scalars[1] = {data.pages.page_bytes_target};
  add_section(payloads, ClusterSection::PageScalars, static_cast<u32>(sizeof(u32)), 1u,
              page_scalars);
  // The skin binding stream and the width of the palette it indexes; empty and zero for an
  // unskinned mesh, which is what every container built before skinning existed reads as.
  add_section(payloads, ClusterSection::Skin, static_cast<u32>(sizeof(SkinBinding)),
              mesh.skin.size(), mesh.skin.data());
  const u32 skin_scalars[1] = {mesh.skin_joint_count};
  add_section(payloads, ClusterSection::SkinScalars, static_cast<u32>(sizeof(u32)), 1u,
              skin_scalars);
  // The embedded images. Both sections are written even when there are none, for the same reason
  // the page and skin sections are: a container's section list says what the format is, not what
  // this mesh happened to have.
  add_section(payloads, ClusterSection::Images, static_cast<u32>(sizeof(ClusterFileImage)),
              image_records.size(), image_records.data());
  add_section(payloads, ClusterSection::ImageBytes, 1u, image_bytes.size(), image_bytes.data());
  // The morph stream. The two delta arrays go in one section back to back, because they are the
  // same element and the same length and one section record is cheaper than a second kind that
  // would only ever be present or absent with the first.
  add_section(payloads, ClusterSection::MorphChannels, static_cast<u32>(sizeof(MorphChannel)),
              mesh.morph_channels.size(), mesh.morph_channels.data());
  Vector<u8> morph_names;
  for (const std::string& name : mesh.morph_names) {
    for (const char c : name)
      morph_names.push_back(static_cast<u8>(c));
    morph_names.push_back(0);
  }
  add_section(payloads, ClusterSection::MorphNames, 1u, morph_names.size(), morph_names.data());
  add_section(payloads, ClusterSection::MorphClusterSlices, static_cast<u32>(sizeof(u32)),
              mesh.morph_cluster_slices.size(), mesh.morph_cluster_slices.data());
  add_section(payloads, ClusterSection::MorphSlices, static_cast<u32>(sizeof(MorphSlice)),
              mesh.morph_slices.size(), mesh.morph_slices.data());
  add_section(payloads, ClusterSection::MorphIndices, 1u, mesh.morph_indices.size(),
              mesh.morph_indices.data());
  Vector<i16> morph_deltas;
  morph_deltas.reserve(mesh.morph_deltas.size() + mesh.morph_normal_deltas.size());
  morph_deltas.append(std::span<const i16>(mesh.morph_deltas.data(), mesh.morph_deltas.size()));
  const bool morph_normals = !mesh.morph_normal_deltas.empty();
  if (morph_normals) {
    morph_deltas.append(
        std::span<const i16>(mesh.morph_normal_deltas.data(), mesh.morph_normal_deltas.size()));
  }
  add_section(payloads, ClusterSection::MorphDeltas, static_cast<u32>(sizeof(i16)),
              morph_deltas.size(), morph_deltas.data());
  const u32 morph_scalars[2] = {mesh.morph_delta_count, morph_normals ? 1u : 0u};
  add_section(payloads, ClusterSection::MorphScalars, static_cast<u32>(sizeof(u32)), 2u,
              morph_scalars);

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

// One NUL-terminated string out of the `Strings` payload. `what` names the caller in the message,
// since an image path and a media type fail for the same two reasons and read differently.
bool read_string(std::span<const u8> bytes, const ClusterFileSection& strings, u64 at,
                 const char* what, std::string& out, std::string* error) {
  const u64 size = strings.element_count;
  if (at >= size) {
    return fail(error, std::string("cluster file ") + what + " offset " + std::to_string(at) +
                           " is outside the " + std::to_string(size) + "-byte string table");
  }
  const u8* blob = bytes.data() + strings.offset;
  const void* nul = std::memchr(blob + at, 0, static_cast<usize>(size - at));
  if (nul == nullptr) return fail(error, "cluster file string table is not NUL-terminated");
  out.assign(reinterpret_cast<const char*>(blob + at),
             static_cast<usize>(static_cast<const u8*>(nul) - (blob + at)));
  return true;
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
    case ClusterSection::PageScalars: return "page_scalars";
    case ClusterSection::Skin: return "skin";
    case ClusterSection::SkinScalars: return "skin_scalars";
    case ClusterSection::Images: return "images";
    case ClusterSection::ImageBytes: return "image_bytes";
    case ClusterSection::MorphChannels: return "morph_channels";
    case ClusterSection::MorphNames: return "morph_names";
    case ClusterSection::MorphClusterSlices: return "morph_cluster_slices";
    case ClusterSection::MorphSlices: return "morph_slices";
    case ClusterSection::MorphIndices: return "morph_indices";
    case ClusterSection::MorphDeltas: return "morph_deltas";
    case ClusterSection::MorphScalars: return "morph_scalars";
  }
  return "unknown";
}

u64 cluster_file_hash(const ClusterFileData& data) {
  std::string bytes;
  return encode(data, bytes);
}

ClusterImageSummary summarize_cluster_images(const ClusterFileData& data) {
  ClusterImageSummary out;
  out.count =
      data.image_paths.size() > data.images.size() ? data.image_paths.size() : data.images.size();
  // The same dedup the writer does, so what a build reports and what a reader reports of the file
  // it wrote are the same numbers: identical bytes hash the same on either side.
  Vector<u64> hashes;
  Vector<u32> sizes;
  Vector<const u8*> blobs;
  for (const ClusterImage& image : data.images) {
    if (image.bytes.empty()) continue;
    ++out.embedded;
    const u64 hash = hash_bytes(image.bytes.data(), image.bytes.size());
    bool shared = false;
    for (u32 i = 0; i < hashes.size(); ++i) {
      if (hashes[i] != hash || sizes[i] != image.bytes.size()) continue;
      if (std::memcmp(blobs[i], image.bytes.data(), image.bytes.size()) != 0) continue;
      shared = true;
      break;
    }
    if (shared) {
      ++out.deduplicated;
      continue;
    }
    hashes.push_back(hash);
    sizes.push_back(image.bytes.size());
    blobs.push_back(image.bytes.data());
    ++out.distinct;
    out.bytes += image.bytes.size();
  }
  return out;
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

  // The skin bindings, and the palette width they index. A file with neither section, or one
  // with an empty stream, is an unskinned mesh; the two travel together, so a stream with no
  // width is refused rather than loaded with a palette nothing can be checked against.
  if (const ClusterFileSection* skin = found[static_cast<u32>(ClusterSection::Skin)];
      skin != nullptr && skin->element_count != 0) {
    if (skin->element_size != sizeof(SkinBinding)) {
      return fail(error, "cluster file section skin has " + std::to_string(skin->element_size) +
                             "-byte elements, expected " + std::to_string(sizeof(SkinBinding)));
    }
    copy_section(bytes, *skin, result.mesh.mesh.skin);
    const ClusterFileSection* width = found[static_cast<u32>(ClusterSection::SkinScalars)];
    if (width == nullptr || width->element_count == 0)
      return fail(error, "cluster file has skin bindings but no joint count");
    if (width->element_size != sizeof(u32)) {
      return fail(error, "cluster file section skin_scalars has " +
                             std::to_string(width->element_size) + "-byte elements, expected 4");
    }
    std::memcpy(&result.mesh.mesh.skin_joint_count, bytes.data() + width->offset, sizeof(u32));
  }

  // The morph stream. A file with no `morph_channels` section, or with an empty one, is a mesh
  // with no channels and every other morph section is ignored — which is exactly what every
  // container written before these kinds existed is. Past that, the sections travel together, so
  // a missing one is refused rather than loaded as half a stream.
  if (const ClusterFileSection* channels = found[static_cast<u32>(ClusterSection::MorphChannels)];
      channels != nullptr && channels->element_count != 0) {
    if (channels->element_size != sizeof(MorphChannel)) {
      return fail(error, "cluster file section morph_channels has " +
                             std::to_string(channels->element_size) + "-byte elements, expected " +
                             std::to_string(sizeof(MorphChannel)));
    }
    copy_section(bytes, *channels, result.mesh.mesh.morph_channels);
    const ClusterFileSection* names = found[static_cast<u32>(ClusterSection::MorphNames)];
    const ClusterFileSection* directory =
        found[static_cast<u32>(ClusterSection::MorphClusterSlices)];
    const ClusterFileSection* slices = found[static_cast<u32>(ClusterSection::MorphSlices)];
    const ClusterFileSection* indices = found[static_cast<u32>(ClusterSection::MorphIndices)];
    const ClusterFileSection* deltas = found[static_cast<u32>(ClusterSection::MorphDeltas)];
    const ClusterFileSection* scalars_section =
        found[static_cast<u32>(ClusterSection::MorphScalars)];
    if (directory == nullptr || slices == nullptr || indices == nullptr || deltas == nullptr ||
        scalars_section == nullptr || scalars_section->element_count < 2) {
      return fail(error, "cluster file has morph channels but not the rest of the stream");
    }
    if (directory->element_size != sizeof(u32) || slices->element_size != sizeof(MorphSlice) ||
        indices->element_size != 1 || deltas->element_size != sizeof(i16) ||
        scalars_section->element_size != sizeof(u32)) {
      return fail(error, "a morph section's element size is not what the format says");
    }
    u32 morph_scalars[2] = {0, 0};
    std::memcpy(morph_scalars, bytes.data() + scalars_section->offset, sizeof(morph_scalars));
    result.mesh.mesh.morph_delta_count = morph_scalars[0];
    copy_section(bytes, *directory, result.mesh.mesh.morph_cluster_slices);
    copy_section(bytes, *slices, result.mesh.mesh.morph_slices);
    copy_section(bytes, *indices, result.mesh.mesh.morph_indices);
    Vector<i16> packed;
    copy_section(bytes, *deltas, packed);
    const bool normals = morph_scalars[1] != 0;
    const u32 half = normals ? packed.size() / 2 : packed.size();
    if (normals && (packed.size() & 1u) != 0)
      return fail(error, "cluster file morph deltas claim normals but hold an odd count");
    result.mesh.mesh.morph_deltas.append(std::span<const i16>(packed.data(), half));
    if (normals) {
      result.mesh.mesh.morph_normal_deltas.append(
          std::span<const i16>(packed.data() + half, packed.size() - half));
    }
    if (names != nullptr && names->element_count != 0 && names->element_size == 1) {
      const char* text = reinterpret_cast<const char*>(bytes.data() + names->offset);
      u64 at = 0;
      while (at < names->element_count &&
             result.mesh.mesh.morph_names.size() < result.mesh.mesh.morph_channels.size()) {
        const void* nul = std::memchr(text + at, 0, static_cast<usize>(names->element_count - at));
        if (nul == nullptr) break;
        const usize length = static_cast<usize>(static_cast<const char*>(nul) - (text + at));
        result.mesh.mesh.morph_names.push_back(std::string(text + at, length));
        at += length + 1;
      }
    }
    // A file that carried fewer names than channels (an older writer, or a truncated table) gets
    // empty ones rather than a short array, because everything downstream indexes names by channel.
    while (result.mesh.mesh.morph_names.size() < result.mesh.mesh.morph_channels.size())
      result.mesh.mesh.morph_names.push_back(std::string());
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
    result.image_paths.reserve(offsets.size());
    for (const u32 at : offsets) {
      std::string path;
      if (!read_string(bytes, *strings, at, "image path", path, error)) return false;
      result.image_paths.push_back(std::move(path));
    }
  }

  // The images the source embedded, which is what makes a container drawable on its own. The
  // records are parallel to the paths, so a file that disagrees with itself about how many images
  // it has is refused rather than half-read; a file with no records at all is one written before
  // the sections existed, and it draws what it drew then.
  if (const ClusterFileSection* records = found[static_cast<u32>(ClusterSection::Images)];
      records != nullptr && records->element_count != 0) {
    if (records->element_size != sizeof(ClusterFileImage)) {
      return fail(error, "cluster file section images has " +
                             std::to_string(records->element_size) + "-byte elements, expected " +
                             std::to_string(sizeof(ClusterFileImage)));
    }
    if (records->element_count != result.image_paths.size()) {
      return fail(error, "cluster file has " + std::to_string(records->element_count) +
                             " image records for " + std::to_string(result.image_paths.size()) +
                             " image paths");
    }
    Vector<ClusterFileImage> image_records;
    copy_section(bytes, *records, image_records);
    const ClusterFileSection* blob = found[static_cast<u32>(ClusterSection::ImageBytes)];
    const u64 blob_size = blob != nullptr && blob->element_size == 1 ? blob->element_count : 0;
    result.images.resize(image_records.size());
    for (u32 i = 0; i < image_records.size(); ++i) {
      const ClusterFileImage& record = image_records[i];
      if (record.bytes != 0) {
        if (record.offset > blob_size || record.bytes > blob_size - record.offset) {
          return fail(error, "cluster file image " + std::to_string(i) + " names " +
                                 std::to_string(record.bytes) + " bytes at " +
                                 std::to_string(record.offset) + ", outside the " +
                                 std::to_string(blob_size) + "-byte image payload");
        }
        result.images[i].bytes.resize(record.bytes);
        std::memcpy(result.images[i].bytes.data(), bytes.data() + blob->offset + record.offset,
                    static_cast<usize>(record.bytes));
      }
      // The media type is one-based, so a record from a build that did not fill it reads empty.
      if (record.mime != 0) {
        if (strings == nullptr || strings->element_size != 1)
          return fail(error, "cluster file has an image media type but no string table");
        if (!read_string(bytes, *strings, record.mime - 1, "image media type",
                         result.images[i].mime_type, error)) {
          return false;
        }
      }
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
  if (!mesh.skin.empty() && mesh.skin.size() != mesh.vertices.size()) {
    return fail(error, "cluster file has " + std::to_string(mesh.skin.size()) +
                           " skin bindings for " + std::to_string(mesh.vertices.size()) +
                           " vertices");
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
    // The byte target the layout was given. A file that does not record it falls back to the
    // largest page that was not flagged oversized, which is inside the target by construction
    // and is the best a reader can do — but it reads as a full page for a mesh that fits in one,
    // which is why the number is written down.
    result.pages.page_bytes_target = 0;
    for (const ClusterPageDesc& page : result.pages.pages) {
      if ((page.flags & k_page_oversized) == 0 && page.bytes > result.pages.page_bytes_target)
        result.pages.page_bytes_target = page.bytes;
    }
    if (const ClusterFileSection* scalars_section =
            found[static_cast<u32>(ClusterSection::PageScalars)];
        scalars_section != nullptr && scalars_section->element_count != 0) {
      if (scalars_section->element_size != sizeof(u32)) {
        return fail(error, "cluster file section page_scalars has " +
                               std::to_string(scalars_section->element_size) +
                               "-byte elements, expected 4");
      }
      u32 target = 0;
      std::memcpy(&target, bytes.data() + scalars_section->offset, sizeof(u32));
      if (target != 0) result.pages.page_bytes_target = target;
    }
    std::string why;
    if (!rebuild_cluster_page_index(result.mesh, result.pages, &why))
      return fail(error, "cluster file page table: " + why);
  }

  out = std::move(result);
  return true;
}

// ---- the container as a random-access file -----------------------------------------------------

namespace {

// What travels with a streamed scene and is never itself paged. The image payload is the one
// large member and is here for the reason the header states: a container is a complete answer to
// "draw this mesh", and textures are uploaded whole.
constexpr ClusterSection k_resident_kinds[] = {
    ClusterSection::Clusters,
    ClusterSection::Lod,
    ClusterSection::LevelClusterCounts,
    ClusterSection::Scalars,
    ClusterSection::Pages,
    ClusterSection::PageChildren,
    ClusterSection::PageScalars,
    ClusterSection::SkinScalars,
    ClusterSection::ClusterMaterial,
    ClusterSection::Materials,
    ClusterSection::ImagePaths,
    ClusterSection::Strings,
    ClusterSection::Images,
    ClusterSection::ImageBytes,
    ClusterSection::SourcePath,
    ClusterSection::SourceHash,
    // The whole morph stream is resident, deltas included, and that is a decision rather than an
    // oversight: the renderer **refuses to stream a morphed mesh** and says so
    // ([renderer](renderer.md)), so a streamed scene never carries one and a resident read of one
    // that does is the honest answer rather than half a stream. Paging it needs the slice
    // directory's indices patched per page the way `ClusterDesc`'s vertex offsets are, which is
    // the same work and belongs with it.
    ClusterSection::MorphChannels,
    ClusterSection::MorphNames,
    ClusterSection::MorphClusterSlices,
    ClusterSection::MorphSlices,
    ClusterSection::MorphIndices,
    ClusterSection::MorphDeltas,
    ClusterSection::MorphScalars,
};

// The per-page streams, which a resident read leaves on disk. They are still *listed*, with zero
// elements, in the container the resident read hands the decoder — because "present and empty"
// and "absent" mean different things to it: an absent `Quantized` section means "requantize this
// mesh", which on an empty mesh would throw the 16-bit grid away, and an absent `Vertices`
// section is a file it refuses outright.
constexpr ClusterSection k_paged_kinds[] = {
    ClusterSection::Vertices,     ClusterSection::Attributes, ClusterSection::Triangles,
    ClusterSection::VertexSource, ClusterSection::Quantized,  ClusterSection::Skin,
};

bool read_exact(const io::FileHandle& file, u64 offset, void* dst, u64 bytes, const char* what,
                std::string* error) {
  u64 read = 0;
  const io::Status status = file.read_at(offset, dst, bytes, read);
  if (status != io::Status::Ok) {
    return fail(error, std::string("cannot read the cluster file's ") + what + ": " +
                           io::status_name(status));
  }
  if (read != bytes) {
    return fail(error, std::string("cluster file is truncated: its ") + what + " wanted " +
                           std::to_string(bytes) + " bytes at " + std::to_string(offset) +
                           " and the file gave " + std::to_string(read));
  }
  return true;
}

}  // namespace

void ClusterFileReader::close() noexcept {
  file_.close();
  content_hash_ = 0;
  sections_.clear();
  for (u32& index : by_kind_)
    index = 0;
}

u64 ClusterFileReader::element_count(ClusterSection kind) const noexcept {
  const u32 index = static_cast<u32>(kind);
  if (index >= k_cluster_section_kinds || by_kind_[index] == 0) return 0;
  return sections_[by_kind_[index] - 1].element_count;
}

bool ClusterFileReader::range(ClusterSection kind, u32 element_size, u64 first, u64 count,
                              u64& offset, u64& bytes) const noexcept {
  offset = 0;
  bytes = 0;
  const u32 index = static_cast<u32>(kind);
  if (index >= k_cluster_section_kinds || by_kind_[index] == 0) return false;
  const ClusterFileSection& section = sections_[by_kind_[index] - 1];
  if (section.element_size != element_size) return false;
  if (first > section.element_count || count > section.element_count - first) return false;
  offset = section.offset + first * element_size;
  bytes = count * element_size;
  return true;
}

bool ClusterFileReader::open(std::string_view path, ClusterFileData* resident, std::string* error) {
  close();
  if (resident != nullptr) *resident = ClusterFileData{};
  const io::Status status = file_.open(path);
  if (status != io::Status::Ok) {
    close();
    return fail(error,
                "cannot open cluster file '" + std::string(path) + "': " + io::status_name(status));
  }

  ClusterFileHeader header;
  if (!read_exact(file_, 0, &header, k_header_bytes, "header", error)) {
    close();
    return false;
  }
  if (std::memcmp(header.magic, "CLST", 4) != 0) {
    close();
    return fail(error, "not a cluster file: the magic is not \"CLST\"");
  }
  if (header.version != k_cluster_file_version) {
    close();
    return fail(error, "unsupported cluster file version " + std::to_string(header.version) +
                           ": this build reads version " + std::to_string(k_cluster_file_version));
  }
  if (header.total_bytes < k_header_bytes || file_.size() < header.total_bytes) {
    close();
    return fail(error, "cluster file is truncated: " + std::to_string(file_.size()) +
                           " bytes, the header says " + std::to_string(header.total_bytes));
  }
  const u64 table_end = k_header_bytes + k_record_bytes * u64{header.section_count};
  if (table_end > header.total_bytes) {
    close();
    return fail(error, "cluster file is truncated: the table of " +
                           std::to_string(header.section_count) + " sections does not fit");
  }
  sections_.resize(header.section_count);
  if (header.section_count != 0 &&
      !read_exact(file_, k_header_bytes, sections_.data(), table_end - k_header_bytes,
                  "section table", error)) {
    close();
    return false;
  }
  // Structure before contents, exactly as a full read checks it, so that nothing `range` reports
  // can fall outside the file. What is *not* checked is the payload hash; see the header.
  for (const ClusterFileSection& section : sections_) {
    if (section.element_count > 0xffffffffull) {
      close();
      return fail(error, "cluster file section " + section_label(section.kind) + " holds " +
                             std::to_string(section.element_count) +
                             " elements, more than this build reads");
    }
    const u64 span_bytes = u64{section.element_size} * section.element_count;
    if (section.offset < k_header_bytes || section.offset > header.total_bytes ||
        span_bytes > header.total_bytes - section.offset) {
      close();
      return fail(error, "cluster file section " + section_label(section.kind) +
                             " extends past the end of the file");
    }
  }
  content_hash_ = header.content_hash;
  for (u32 i = 0; i < sections_.size(); ++i) {
    const u32 kind = sections_[i].kind;
    if (kind == 0 || kind >= k_cluster_section_kinds) continue;  // an unknown kind is skipped
    if (by_kind_[kind] == 0) by_kind_[kind] = i + 1;
  }
  if (resident == nullptr) return true;

  // The resident sections, decoded by **rewriting them as a container of their own** and handing
  // that to `read_cluster_file_memory`. Copying the decode instead would be a second reader of
  // one format — two sets of checks, two sets of messages, and a drift nobody would notice until
  // a container loaded one way and not the other — so the only thing written twice here is the
  // twelve lines of layout that `encode` already knows how to do.
  constexpr u64 k_max_emitted = sizeof(k_resident_kinds) / sizeof(k_resident_kinds[0]) +
                                sizeof(k_paged_kinds) / sizeof(k_paged_kinds[0]);
  Vector<ClusterFileSection> emitted;
  u64 at = k_header_bytes + k_record_bytes * k_max_emitted;
  for (const ClusterSection kind : k_resident_kinds) {
    const u32 index = static_cast<u32>(kind);
    if (by_kind_[index] == 0) continue;
    ClusterFileSection record = sections_[by_kind_[index] - 1];
    at = align_up(at);
    record.offset = at;
    at += u64{record.element_size} * record.element_count;
    emitted.push_back(record);
  }
  for (const ClusterSection kind : k_paged_kinds) {
    const u32 index = static_cast<u32>(kind);
    if (by_kind_[index] == 0) continue;
    ClusterFileSection record = sections_[by_kind_[index] - 1];
    record.element_count = 0;
    at = align_up(at);
    record.offset = at;
    emitted.push_back(record);
  }
  // The table is sized for every kind and filled with the ones this file has, so the payloads
  // start where the loop above assumed; the unused records are trimmed by rewriting the count.
  std::string bytes;
  bytes.assign(static_cast<usize>(at), '\0');
  auto* raw = reinterpret_cast<u8*>(bytes.data());
  u32 out_index = 0;
  for (const ClusterFileSection& record : emitted) {
    std::memcpy(raw + k_header_bytes + k_record_bytes * out_index, &record, k_record_bytes);
    ++out_index;
    if (record.element_count == 0) continue;
    const u32 index = static_cast<u32>(record.kind);
    const ClusterFileSection& source = sections_[by_kind_[index] - 1];
    if (!read_exact(file_, source.offset, raw + record.offset,
                    u64{record.element_size} * record.element_count,
                    cluster_section_name(record.kind), error)) {
      close();
      return false;
    }
  }
  ClusterFileHeader out_header;
  out_header.section_count = emitted.size();
  out_header.total_bytes = at;
  out_header.content_hash =
      hash_bytes(raw + k_header_bytes, static_cast<usize>(at - k_header_bytes));
  std::memcpy(raw, &out_header, k_header_bytes);
  if (!read_cluster_file_memory(std::span<const u8>(raw, bytes.size()), *resident, error)) {
    close();
    return false;
  }
  return true;
}

u64 cluster_cache_key(u64 source_hash, const ClusterLodOptions& options, bool weld,
                      u32 page_bytes) noexcept {
  u64 key = hash_combine(source_hash, k_cluster_cache_version);
  key = hash_combine(key, options.max_triangles);
  key = hash_combine(key, options.max_vertices);
  const u64 flags = (options.ray_tracing ? 1ull : 0ull) | (options.normal_cones ? 2ull : 0ull) |
                    (weld ? 4ull : 0ull) | (static_cast<u64>(options.uv_seams) << 3) |
                    (static_cast<u64>(options.normal_seams) << 5) |
                    (static_cast<u64>(options.skin_seams) << 7) |
                    (static_cast<u64>(options.morph_seams) << 9);
  key = hash_combine(key, flags);
  // The attribute weights change what the simplifier keeps, so two weights are two meshes. The
  // bits of the float are the identity, not its value, so a weight that reads the same reads the
  // same everywhere.
  u32 weight_bits[2] = {};
  std::memcpy(&weight_bits[0], &options.normal_weight, sizeof(f32));
  std::memcpy(&weight_bits[1], &options.uv_weight, sizeof(f32));
  key = hash_combine(key, (u64{weight_bits[0]} << 32) | u64{weight_bits[1]});
  return hash_combine(key, page_bytes);
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
