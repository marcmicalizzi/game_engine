// The .clusters container: a DAG plus its materials written, read back, and compared array by
// array (including the eight words of the material record that used to be padding, and a
// material that leaves them zero as an older file does); the forward-compatibility rule
// (unknown sections are skipped); and the five ways a file can be broken, each with its own
// message and an untouched output.
#include <core/hash/hash.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

// The same heightfield the LOD tests use, so the DAG has several levels and real error bounds.
void make_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.clear();
  indices.clear();
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / (n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
}

template <class T>
bool same_bytes(const Vector<T>& a, const Vector<T>& b) {
  if (a.size() != b.size()) return false;
  if (a.empty()) return true;
  return std::memcmp(a.data(), b.data(), static_cast<usize>(a.size()) * sizeof(T)) == 0;
}

// Stand-in bytes for an embedded image: this module carries encoded files without looking at
// them, so what matters here is that they are a run of bytes that is not all the same and is long
// enough that a memcmp is doing work. `foundation/image` is where real PNGs are made.
Vector<u8> fake_png() {
  Vector<u8> bytes;
  bytes.reserve(1024);
  const u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  for (const u8 b : signature)
    bytes.push_back(b);
  for (u32 i = 8; i < 1024; ++i)
    bytes.push_back(static_cast<u8>((i * 37u + (i >> 3)) & 0xffu));
  return bytes;
}

// A DAG over a small terrain, wrapped with two materials, three images (one named by a path, two
// embedded with the same bytes), and a material index per cluster. `indices` is what
// validate_cluster_lod checks the result against.
void make_fixture(ClusterFileData& data, Vector<u32>& indices) {
  Vector<Vec3> positions;
  make_terrain(65, 10.0f, positions, indices);
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(positions, indices, ClusterLodOptions{}, data.mesh, &error),
                  error);
  // Laid out in streaming pages, as `engine-content build` writes it: the clusters are renumbered
  // and the two page sections travel with them.
  ClusterPagesOptions page_options;
  page_options.page_bytes = 64 * 1024;
  REQUIRE_MESSAGE(build_cluster_pages(data.mesh, page_options, data.pages, &error), error);
  data.materials.resize(2);
  data.materials[0].base_color = Vec4{0.82f, 0.71f, 0.49f, 1.0f};
  data.materials[0].metallic = 0.0f;
  data.materials[0].roughness = 0.85f;
  data.materials[0].base_color_image = 0;
  // The eight words that used to be padding: the three one-based image slots, the emissive
  // factor, the normal scale, and the packed alpha word.
  data.materials[0].normal_image = 1;
  data.materials[0].metallic_roughness_image = encode_optional_image(1);
  data.materials[0].occlusion_image = encode_optional_image(1);
  data.materials[0].emissive_image = encode_optional_image(0);
  data.materials[0].emissive = Vec3{0.05f, 0.02f, 0.0f};
  data.materials[0].normal_scale = 1.5f;
  data.materials[0].alpha = encode_alpha_word(1 /* mask */, true, 0.25f);
  data.materials[1].base_color = Vec4{0.18f, 0.32f, 0.91f, 1.0f};
  data.materials[1].metallic = 1.0f;
  data.materials[1].roughness = 0.2f;
  data.materials[1].alpha = 0;  // what a file written before the word existed reads
  data.image_paths.push_back("textures/sand_basecolor.png");
  data.image_paths.push_back("");  // an embedded image keeps its slot with an empty path
  data.image_paths.push_back("");  // and so does a second one with the same bytes
  // The images themselves: the first is a file beside the source and carries nothing, the second
  // is embedded, and the third is the same bytes again — which is what an exporter that packs
  // occlusion, roughness, and metallic into one texture and names it twice produces.
  data.images.resize(3);
  data.images[1].mime_type = "image/png";
  data.images[1].bytes = fake_png();
  data.images[2].mime_type = "image/png";
  data.images[2].bytes = fake_png();
  data.source_path = "content/samples/Terrain/terrain.gltf";
  data.source_hash = 0x0123'4567'89ab'cdefull;
  data.build_key =
      cluster_cache_key(data.source_hash, ClusterLodOptions{}, true, page_options.page_bytes);
  data.cluster_material.reserve(data.mesh.mesh.clusters.size());
  for (u32 i = 0; i < data.mesh.mesh.clusters.size(); ++i)
    data.cluster_material.push_back(i % 2);
}

void check_equal(const ClusterFileData& read, const ClusterFileData& written) {
  CHECK(same_bytes(read.mesh.mesh.clusters, written.mesh.mesh.clusters));
  CHECK(same_bytes(read.mesh.lod, written.mesh.lod));
  CHECK(same_bytes(read.mesh.mesh.vertices, written.mesh.mesh.vertices));
  CHECK(same_bytes(read.mesh.mesh.attributes, written.mesh.mesh.attributes));
  CHECK(same_bytes(read.mesh.mesh.triangles, written.mesh.mesh.triangles));
  CHECK(same_bytes(read.mesh.mesh.vertex_source, written.mesh.mesh.vertex_source));
  CHECK(same_bytes(read.mesh.mesh.skin, written.mesh.mesh.skin));
  CHECK(read.mesh.mesh.skin_joint_count == written.mesh.mesh.skin_joint_count);
  CHECK(same_bytes(read.mesh.level_cluster_counts, written.mesh.level_cluster_counts));
  CHECK(same_bytes(read.cluster_material, written.cluster_material));
  CHECK(same_bytes(read.materials, written.materials));
  CHECK(same_bytes(read.pages.pages, written.pages.pages));
  CHECK(same_bytes(read.pages.child_pages, written.pages.child_pages));
  CHECK(read.pages.page_bytes_target == written.pages.page_bytes_target);
  CHECK(read.mesh.group_count == written.mesh.group_count);
  CHECK(read.mesh.leaf_triangle_count == written.mesh.leaf_triangle_count);
  CHECK(read.mesh.mesh.source_vertex_count == written.mesh.mesh.source_vertex_count);
  CHECK(read.mesh.mesh.source_triangle_count == written.mesh.mesh.source_triangle_count);
  REQUIRE(read.image_paths.size() == written.image_paths.size());
  for (u32 i = 0; i < read.image_paths.size(); ++i)
    CHECK(read.image_paths[i] == written.image_paths[i]);
  REQUIRE(read.images.size() == written.images.size());
  for (u32 i = 0; i < read.images.size(); ++i) {
    CHECK(read.images[i].mime_type == written.images[i].mime_type);
    CHECK(same_bytes(read.images[i].bytes, written.images[i].bytes));
  }
  CHECK(read.source_path == written.source_path);
  CHECK(read.source_hash == written.source_hash);
  CHECK(read.build_key == written.build_key);
}

std::span<const u8> view(const std::string& file) {
  return std::span<const u8>(reinterpret_cast<const u8*>(file.data()), file.size());
}

// The file offset of the table record for `kind`, and the record itself; 0 when there is none.
usize find_section(const std::string& file, ClusterSection kind, ClusterFileSection& out) {
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(ClusterFileHeader) + sizeof(ClusterFileSection) * i;
    ClusterFileSection record;
    std::memcpy(&record, file.data() + at, sizeof(record));
    if (record.kind == static_cast<u32>(kind)) {
      out = record;
      return at;
    }
  }
  return 0;
}

void patch(std::string& file, usize at, const void* data, usize size) {
  std::memcpy(file.data() + at, data, size);
}

// Recomputes the content hash after a deliberate edit, so a test can change the structure
// without the hash check hiding what it wanted to exercise.
void rehash(std::string& file) {
  const u64 content = hash_bytes(file.data() + 32, file.size() - 32);
  patch(file, 24, &content, sizeof(content));
}

// A copy of `file` with one section of an unknown kind inserted: 48 bytes go between the table
// and the first payload (24 for the record and 24 of padding), so every payload offset shifts
// by a multiple of the 16-byte alignment, and the unknown payload is appended at the end.
std::string with_unknown_section(const std::string& file) {
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  const u64 table_end =
      sizeof(ClusterFileHeader) + sizeof(ClusterFileSection) * header.section_count;
  Vector<ClusterFileSection> sections(header.section_count);
  u64 first_payload = header.total_bytes;
  for (u32 i = 0; i < header.section_count; ++i) {
    std::memcpy(&sections[i],
                file.data() + sizeof(ClusterFileHeader) + sizeof(ClusterFileSection) * i,
                sizeof(ClusterFileSection));
    if (sections[i].offset < first_payload) first_payload = sections[i].offset;
  }
  // Payloads move to the first 16-byte boundary after the table grown by one record.
  const u64 shift = (table_end + sizeof(ClusterFileSection) + 15) / 16 * 16 - first_payload;

  std::string out;
  out.append(file, 0, sizeof(ClusterFileHeader));
  for (ClusterFileSection section : sections) {
    section.offset += shift;
    out.append(reinterpret_cast<const char*>(&section), sizeof(section));
  }
  ClusterFileSection unknown;
  unknown.kind = 4242;  // a kind from a build that does not exist yet
  unknown.element_size = 4;
  unknown.element_count = 4;
  const usize unknown_record_at = out.size();
  out.append(reinterpret_cast<const char*>(&unknown), sizeof(unknown));
  out.append(static_cast<usize>(first_payload + shift) - out.size(), '\0');
  out.append(file, static_cast<usize>(first_payload),
             static_cast<usize>(header.total_bytes - first_payload));
  while (out.size() % 16 != 0)
    out.push_back('\0');
  unknown.offset = out.size();
  out.append(16, static_cast<char>(0xab));
  patch(out, unknown_record_at, &unknown, sizeof(unknown));

  header.section_count += 1;
  header.total_bytes = out.size();
  std::memcpy(out.data(), &header, sizeof(header));
  rehash(out);
  return out;
}

}  // namespace

TEST_CASE("cluster file: a DAG with materials survives a round trip array by array") {
  const test::TempDir tmp("engine_cluster_file");
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  const std::string path = tmp.file("terrain.clusters");

  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file(path, read, &error), error);
  check_equal(read, data);
  CHECK_MESSAGE(validate_cluster_lod(read.mesh, indices, &error), error);
  CHECK(read.mesh.leaf_triangle_count == 8192);
  CHECK(read.mesh.level_cluster_counts.size() >= 3);

  // The record's second half, which was padding: the slots come back one-based and decode to
  // the -1 convention, the packed alpha word unpacks, and a zero word is opaque with no
  // image anywhere, which is exactly what a file written before these words existed reads.
  CHECK(decode_optional_image(read.materials[0].metallic_roughness_image) == 1);
  CHECK(decode_optional_image(read.materials[0].occlusion_image) == 1);
  CHECK(decode_optional_image(read.materials[0].emissive_image) == 0);
  CHECK(length(read.materials[0].emissive - Vec3{0.05f, 0.02f, 0.0f}) < 1e-6f);
  CHECK(std::fabs(read.materials[0].normal_scale - 1.5f) < 1e-6f);
  CHECK(alpha_word_mode(read.materials[0].alpha) == 1);
  CHECK(alpha_word_double_sided(read.materials[0].alpha));
  CHECK(std::fabs(alpha_word_cutoff(read.materials[0].alpha) - 0.25f) < 1e-4f);
  CHECK(decode_optional_image(read.materials[1].metallic_roughness_image) == -1);
  CHECK(decode_optional_image(read.materials[1].occlusion_image) == -1);
  CHECK(decode_optional_image(read.materials[1].emissive_image) == -1);
  CHECK(alpha_word_mode(read.materials[1].alpha) == 0);
  CHECK_FALSE(alpha_word_double_sided(read.materials[1].alpha));
  CHECK(alpha_word_cutoff(read.materials[1].alpha) == 0.0f);

  // The bytes on disk say what the header promised, and the hash is the one computed in memory.
  std::string file;
  REQUIRE(io::read_file(path, file) == io::Status::Ok);
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  CHECK(std::memcmp(header.magic, "CLST", 4) == 0);
  CHECK(header.version == k_cluster_file_version);
  CHECK(header.flags == 0);
  CHECK(header.section_count == 22);
  CHECK(header.total_bytes == file.size());
  CHECK(header.content_hash == cluster_file_hash(data));
  for (u32 i = 0; i < header.section_count; ++i) {
    ClusterFileSection section;
    std::memcpy(&section, file.data() + sizeof(header) + sizeof(section) * i, sizeof(section));
    CHECK(section.offset % k_cluster_file_alignment == 0);
    CHECK(section.offset + section.element_size * section.element_count <= header.total_bytes);
    CHECK(std::string(cluster_section_name(section.kind)) != "unknown");
  }
  MESSAGE("terrain.clusters: " << file.size() << " bytes, " << data.mesh.mesh.clusters.size()
                               << " clusters, " << data.mesh.level_cluster_counts.size()
                               << " levels");

  // Reading the same bytes from memory gives the same thing as reading the file.
  ClusterFileData from_memory;
  REQUIRE_MESSAGE(read_cluster_file_memory(view(file), from_memory, &error), error);
  check_equal(from_memory, read);
}

TEST_CASE("cluster file: the page table round-trips, and a file without one reads empty") {
  const test::TempDir tmp("engine_cluster_file");
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  REQUIRE(data.pages.pages.size() > 1);
  const std::string path = tmp.file("pages.clusters");
  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);

  // The descriptors and the flat child-page list come back as they were written, and the two
  // per-cluster tables the file does not store are rebuilt from them.
  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file(path, read, &error), error);
  check_equal(read, data);
  CHECK(read.pages.page_of_cluster.size() == read.mesh.mesh.clusters.size());
  CHECK(read.pages.children.size() == read.mesh.mesh.clusters.size());
  // The byte target is written down rather than guessed at: the largest page is only a lower
  // bound on it, and for a mesh that fits in one page it is that page's own size.
  CHECK(read.pages.page_bytes_target == 64 * 1024);
  CHECK(same_bytes(read.pages.page_of_cluster, data.pages.page_of_cluster));
  CHECK(same_bytes(read.pages.children, data.pages.children));
  CHECK_MESSAGE(validate_cluster_pages(read.mesh, read.pages, &error), error);
  CHECK_MESSAGE(validate_cluster_lod(read.mesh, indices, &error), error);
  // The container's clusters are in page order, so the coarsest level is first rather than last.
  CHECK(read.mesh.lod[0].level + 1 == read.mesh.level_cluster_counts.size());
  MESSAGE("pages.clusters: " << data.pages.pages.size() << " pages over "
                             << data.mesh.mesh.clusters.size() << " clusters, "
                             << data.pages.child_pages.size() << " child page entries");

  // Both sections are written even when there is no page table, so the section list does not
  // depend on how the container was built, and a mesh with no pages reads back with none.
  ClusterFileData unpaged = data;
  unpaged.pages = ClusterPages{};
  const std::string unpaged_path = tmp.file("unpaged.clusters");
  REQUIRE_MESSAGE(write_cluster_file(unpaged_path, unpaged, &error), error);
  ClusterFileData read_unpaged;
  REQUIRE_MESSAGE(read_cluster_file(unpaged_path, read_unpaged, &error), error);
  CHECK(read_unpaged.pages.pages.empty());
  CHECK(read_unpaged.pages.page_of_cluster.empty());
  check_equal(read_unpaged, unpaged);

  // A file from a build that did not know about pages: the kind becomes one this build does not
  // know, and everything else still loads.
  std::string file;
  REQUIRE(io::read_file(path, file) == io::Status::Ok);
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  usize pages_at = 0;
  ClusterFileSection section{};
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(header) + sizeof(section) * i;
    ClusterFileSection record;
    std::memcpy(&record, file.data() + at, sizeof(record));
    if (record.kind == static_cast<u32>(ClusterSection::Pages)) {
      pages_at = at;
      section = record;
    }
  }
  REQUIRE(pages_at != 0);
  CHECK(section.element_size == sizeof(ClusterPageDesc));
  CHECK(section.element_count == data.pages.pages.size());
  CHECK(std::string(cluster_section_name(section.kind)) == "pages");
  std::string older = file;
  ClusterFileSection renamed = section;
  renamed.kind = 31339;
  patch(older, pages_at, &renamed, sizeof(renamed));
  rehash(older);
  ClusterFileData without;
  REQUIRE_MESSAGE(read_cluster_file_memory(view(older), without, &error), error);
  CHECK(without.pages.pages.empty());
  check_equal(without, unpaged);
}

TEST_CASE("cluster file: a reader gives the resident tables now and the page streams by range") {
  const test::TempDir tmp("engine_cluster_file");
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  REQUIRE(data.pages.pages.size() > 1);
  const std::string path = tmp.file("ranged.clusters");
  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  ClusterFileData whole;
  REQUIRE_MESSAGE(read_cluster_file(path, whole, &error), error);

  ClusterFileReader reader;
  ClusterFileData resident;
  REQUIRE_MESSAGE(reader.open(path, &resident, &error), error);
  CHECK(reader.valid());
  CHECK(reader.path() == path);
  CHECK(reader.file().size() > 0);

  // **The resident read is the full read minus the paged streams**, and the only way to keep two
  // readers of one format from drifting is to say so as an assertion rather than in a comment.
  CHECK(same_bytes(resident.mesh.mesh.clusters, whole.mesh.mesh.clusters));
  CHECK(same_bytes(resident.mesh.lod, whole.mesh.lod));
  CHECK(same_bytes(resident.mesh.level_cluster_counts, whole.mesh.level_cluster_counts));
  CHECK(resident.mesh.group_count == whole.mesh.group_count);
  CHECK(resident.mesh.leaf_triangle_count == whole.mesh.leaf_triangle_count);
  CHECK(resident.mesh.mesh.source_vertex_count == whole.mesh.mesh.source_vertex_count);
  CHECK(resident.mesh.mesh.source_triangle_count == whole.mesh.mesh.source_triangle_count);
  // The 16-bit grid comes off the scalars and not off a requantization of an empty mesh, which is
  // why the paged sections are listed with zero elements rather than left out.
  CHECK(resident.mesh.mesh.quant_scale == whole.mesh.mesh.quant_scale);
  CHECK(resident.mesh.mesh.quant_origin.x == whole.mesh.mesh.quant_origin.x);
  CHECK(same_bytes(resident.pages.pages, whole.pages.pages));
  CHECK(same_bytes(resident.pages.child_pages, whole.pages.child_pages));
  CHECK(same_bytes(resident.pages.page_of_cluster, whole.pages.page_of_cluster));
  CHECK(same_bytes(resident.pages.children, whole.pages.children));
  CHECK(resident.pages.page_bytes_target == whole.pages.page_bytes_target);
  CHECK(same_bytes(resident.cluster_material, whole.cluster_material));
  CHECK(same_bytes(resident.materials, whole.materials));
  REQUIRE(resident.image_paths.size() == whole.image_paths.size());
  for (u32 i = 0; i < resident.image_paths.size(); ++i)
    CHECK(resident.image_paths[i] == whole.image_paths[i]);
  REQUIRE(resident.images.size() == whole.images.size());
  for (u32 i = 0; i < resident.images.size(); ++i) {
    CHECK(resident.images[i].mime_type == whole.images[i].mime_type);
    CHECK(same_bytes(resident.images[i].bytes, whole.images[i].bytes));
  }
  CHECK(resident.source_path == whole.source_path);
  CHECK(resident.source_hash == whole.source_hash);
  CHECK(resident.build_key == whole.build_key);
  CHECK(reader.header_hash() == cluster_file_hash(whole));

  // And the paged streams are **not** in it: that is the whole point.
  CHECK(resident.mesh.mesh.vertices.empty());
  CHECK(resident.mesh.mesh.quantized.empty());
  CHECK(resident.mesh.mesh.attributes.empty());
  CHECK(resident.mesh.mesh.triangles.empty());
  CHECK(resident.mesh.mesh.vertex_source.empty());
  CHECK(reader.element_count(ClusterSection::Quantized) == whole.mesh.mesh.quantized.size());
  CHECK(reader.element_count(ClusterSection::Triangles) == whole.mesh.mesh.triangles.size());

  // Every page's slice of every paged stream is one contiguous range, and reading it gives what
  // the full read has at the same indices. This is the property the whole file-backed page source
  // rests on — `build_cluster_pages` reordered the streams so that it holds.
  Vector<u8> buffer;
  for (u32 p = 0; p < resident.pages.pages.size(); ++p) {
    const ClusterPageDesc& page = resident.pages.pages[p];
    u64 offset = 0;
    u64 bytes = 0;
    REQUIRE(reader.range(ClusterSection::Quantized, sizeof(u16), u64{page.first_vertex} * 3,
                         u64{page.vertex_count} * 3, offset, bytes));
    CHECK(bytes == u64{page.vertex_count} * 3 * sizeof(u16));
    buffer.resize(static_cast<u32>(bytes));
    u64 read = 0;
    REQUIRE(reader.file().read_at(offset, buffer.data(), bytes, read) == io::Status::Ok);
    REQUIRE(read == bytes);
    CHECK(std::memcmp(buffer.data(), whole.mesh.mesh.quantized.data() + u64{page.first_vertex} * 3,
                      static_cast<usize>(bytes)) == 0);

    REQUIRE(reader.range(ClusterSection::Attributes, sizeof(VertexAttributes), page.first_vertex,
                         page.vertex_count, offset, bytes));
    buffer.resize(static_cast<u32>(bytes));
    REQUIRE(reader.file().read_at(offset, buffer.data(), bytes, read) == io::Status::Ok);
    REQUIRE(read == bytes);
    CHECK(std::memcmp(buffer.data(), whole.mesh.mesh.attributes.data() + page.first_vertex,
                      static_cast<usize>(bytes)) == 0);

    REQUIRE(reader.range(ClusterSection::Triangles, sizeof(u32), page.first_triangle,
                         page.triangle_count, offset, bytes));
    buffer.resize(static_cast<u32>(bytes));
    REQUIRE(reader.file().read_at(offset, buffer.data(), bytes, read) == io::Status::Ok);
    REQUIRE(read == bytes);
    CHECK(std::memcmp(buffer.data(), whole.mesh.mesh.triangles.data() + page.first_triangle,
                      static_cast<usize>(bytes)) == 0);

    REQUIRE(reader.range(ClusterSection::Vertices, sizeof(Vec3), page.first_vertex,
                         page.vertex_count, offset, bytes));
    buffer.resize(static_cast<u32>(bytes));
    REQUIRE(reader.file().read_at(offset, buffer.data(), bytes, read) == io::Status::Ok);
    REQUIRE(read == bytes);
    CHECK(std::memcmp(buffer.data(), whole.mesh.mesh.vertices.data() + page.first_vertex,
                      static_cast<usize>(bytes)) == 0);
  }

  // A range that asks for the wrong element size, a run past the end, and a kind the container
  // does not carry are all answered with false and zeros rather than a fault, so a caller may ask
  // about a section an older container has none of.
  u64 offset = 0;
  u64 bytes = 1;
  CHECK_FALSE(reader.range(ClusterSection::Triangles, sizeof(u16), 0, 1, offset, bytes));
  CHECK(bytes == 0);
  CHECK_FALSE(reader.range(ClusterSection::Triangles, sizeof(u32), whole.mesh.mesh.triangles.size(),
                           1, offset, bytes));
  CHECK(reader.range(ClusterSection::Triangles, sizeof(u32), 0, 0, offset, bytes));
  CHECK(bytes == 0);
  CHECK(reader.element_count(static_cast<ClusterSection>(31339)) == 0);
  CHECK_FALSE(reader.range(static_cast<ClusterSection>(31339), 4, 0, 1, offset, bytes));

  // Header and table only: no resident decode at all, and the ranges still answer.
  ClusterFileReader table_only;
  REQUIRE_MESSAGE(table_only.open(path, nullptr, &error), error);
  CHECK(table_only.range(ClusterSection::Triangles, sizeof(u32), 0, 1, offset, bytes));
  CHECK(bytes == sizeof(u32));
  table_only.close();
  CHECK_FALSE(table_only.valid());
  CHECK_FALSE(table_only.range(ClusterSection::Triangles, sizeof(u32), 0, 1, offset, bytes));

  // The structural checks are the full read's, so a broken container is refused here too and by
  // the same sentence; the payload hash is the one thing this reader does not look at.
  ClusterFileReader missing;
  CHECK_FALSE(missing.open(tmp.file("nothing.clusters"), &resident, &error));
  CHECK(error.find("cannot open") != std::string::npos);
  const std::string wrong = tmp.file("wrong.clusters");
  REQUIRE(io::write_file(wrong, "NOPE and then some padding to get past the header size") ==
          io::Status::Ok);
  CHECK_FALSE(missing.open(wrong, &resident, &error));
  CHECK(error.find("magic") != std::string::npos);
  CHECK(resident.mesh.mesh.clusters.empty());
}

TEST_CASE("cluster file: the skin bindings round-trip, and a file without them reads unskinned") {
  const test::TempDir tmp("cluster_file_skin");
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  // Bind the fixture's vertices to two joints. The mesh is already laid out in pages, so this
  // stands in for a skinned container exactly as `engine-content build` would write one.
  ClusterMesh& geo = data.mesh.mesh;
  geo.skin.clear();
  geo.skin.reserve(geo.vertices.size());
  for (u32 v = 0; v < geo.vertices.size(); ++v) {
    const u32 joints[4] = {0, 1, 0, 0};
    const f32 t = (geo.vertices[v].x + 10.0f) / 20.0f;
    const f32 weights[4] = {1.0f - t, t, 0.0f, 0.0f};
    geo.skin.push_back(make_skin_binding(joints, weights));
  }
  geo.skin_joint_count = 2;

  const std::string path = tmp.file("skinned.clusters");
  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file(path, read, &error), error);
  check_equal(read, data);
  REQUIRE(read.mesh.mesh.skin.size() == read.mesh.mesh.vertices.size());
  CHECK(read.mesh.mesh.skin_joint_count == 2);
  CHECK_MESSAGE(validate_cluster_lod(read.mesh, indices, &error), error);

  // A container written before skinning existed: the kind becomes one this build does not know,
  // and what comes back is the same mesh with no bindings rather than a failure.
  std::string file;
  REQUIRE(io::read_file(path, file) == io::Status::Ok);
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  usize skin_at = 0;
  ClusterFileSection section{};
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(header) + sizeof(section) * i;
    ClusterFileSection record;
    std::memcpy(&record, file.data() + at, sizeof(record));
    if (record.kind == static_cast<u32>(ClusterSection::Skin)) {
      skin_at = at;
      section = record;
    }
  }
  REQUIRE(skin_at != 0);
  CHECK(section.element_size == sizeof(SkinBinding));
  CHECK(section.element_count == geo.skin.size());
  CHECK(std::string(cluster_section_name(section.kind)) == "skin");
  std::string older = file;
  ClusterFileSection renamed = section;
  renamed.kind = 31341;
  patch(older, skin_at, &renamed, sizeof(renamed));
  rehash(older);
  ClusterFileData without;
  REQUIRE_MESSAGE(read_cluster_file_memory(view(older), without, &error), error);
  CHECK(without.mesh.mesh.skin.empty());
  CHECK(without.mesh.mesh.skin_joint_count == 0);
  CHECK(same_bytes(without.mesh.mesh.vertices, data.mesh.mesh.vertices));

  // Bindings with no joint count is the one combination that is refused: a palette nothing can
  // be checked against is worse than no palette at all.
  std::string orphaned = file;
  usize width_at = 0;
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(header) + sizeof(section) * i;
    ClusterFileSection record;
    std::memcpy(&record, file.data() + at, sizeof(record));
    if (record.kind == static_cast<u32>(ClusterSection::SkinScalars)) width_at = at;
  }
  REQUIRE(width_at != 0);
  ClusterFileSection dropped;
  std::memcpy(&dropped, file.data() + width_at, sizeof(dropped));
  dropped.kind = 31342;
  patch(orphaned, width_at, &dropped, sizeof(dropped));
  rehash(orphaned);
  ClusterFileData refused;
  CHECK_FALSE(read_cluster_file_memory(view(orphaned), refused, &error));
  CHECK(error.find("joint count") != std::string::npos);
}

TEST_CASE("cluster file: an embedded image travels whole, once per distinct blob") {
  const test::TempDir tmp("cluster_file_images");
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  const Vector<u8> png = fake_png();
  const std::string path = tmp.file("images.clusters");
  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);

  // Three image slots: one named by a path, two carrying the same bytes. Both embedded slots read
  // back byte for byte with their media type, and the file holds one copy of the bytes.
  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file(path, read, &error), error);
  check_equal(read, data);
  REQUIRE(read.images.size() == 3);
  CHECK(read.images[0].bytes.empty());
  CHECK(read.images[0].mime_type.empty());
  CHECK(same_bytes(read.images[1].bytes, png));
  CHECK(same_bytes(read.images[2].bytes, png));
  CHECK(read.images[1].mime_type == "image/png");
  CHECK(read.images[2].mime_type == "image/png");

  const ClusterImageSummary summary = summarize_cluster_images(data);
  CHECK(summary.count == 3);
  CHECK(summary.embedded == 2);
  CHECK(summary.distinct == 1);
  CHECK(summary.deduplicated == 1);
  CHECK(summary.bytes == png.size());
  // What a reader reports of a container is what the build that wrote it reported, which is the
  // property `engine-content info` rests on.
  const ClusterImageSummary reread = summarize_cluster_images(read);
  CHECK(reread.count == summary.count);
  CHECK(reread.embedded == summary.embedded);
  CHECK(reread.distinct == summary.distinct);
  CHECK(reread.deduplicated == summary.deduplicated);
  CHECK(reread.bytes == summary.bytes);

  std::string file;
  REQUIRE(io::read_file(path, file) == io::Status::Ok);
  ClusterFileSection payload{};
  REQUIRE(find_section(file, ClusterSection::ImageBytes, payload) != 0);
  CHECK(payload.element_size == 1);
  CHECK(payload.element_count == png.size());  // one copy, not two
  ClusterFileSection records{};
  const usize records_at = find_section(file, ClusterSection::Images, records);
  REQUIRE(records_at != 0);
  CHECK(records.element_size == sizeof(ClusterFileImage));
  CHECK(records.element_count == 3);
  // The two duplicates point at the same range, and the hash is the content hash of the bytes,
  // which is what a texture derivative in the cache will be addressed by.
  ClusterFileImage first;
  ClusterFileImage second;
  std::memcpy(&first, file.data() + records.offset + sizeof(ClusterFileImage), sizeof(first));
  std::memcpy(&second, file.data() + records.offset + 2 * sizeof(ClusterFileImage), sizeof(second));
  CHECK(first.offset == second.offset);
  CHECK(first.bytes == png.size());
  CHECK(first.hash == hash_bytes(png.data(), png.size()));

  // A container from a build that carried no image bytes: the kind becomes one this build does
  // not know, and the mesh comes back with its paths and no bytes — which is exactly the older
  // container, and exactly what it drew.
  std::string older = file;
  ClusterFileSection renamed = records;
  renamed.kind = 31343;
  patch(older, records_at, &renamed, sizeof(renamed));
  rehash(older);
  ClusterFileData without;
  REQUIRE_MESSAGE(read_cluster_file_memory(view(older), without, &error), error);
  CHECK(without.images.empty());
  REQUIRE(without.image_paths.size() == 3);
  CHECK(without.image_paths[0] == data.image_paths[0]);
  CHECK(same_bytes(without.mesh.mesh.vertices, data.mesh.mesh.vertices));

  // A file that disagrees with itself about how many images it has is refused rather than
  // half-read: a record array shorter than the path array would silently drop a texture.
  std::string short_records = file;
  ClusterFileSection fewer = records;
  fewer.element_count = 2;
  patch(short_records, records_at, &fewer, sizeof(fewer));
  rehash(short_records);
  ClusterFileData mismatched;
  CHECK_FALSE(read_cluster_file_memory(view(short_records), mismatched, &error));
  CHECK(error.find("image records") != std::string::npos);

  // And a record whose bytes run past the payload is named as such rather than read out of it.
  std::string overrun = file;
  ClusterFileImage broken = first;
  broken.bytes = 0xffffffffu;
  patch(overrun, static_cast<usize>(records.offset) + sizeof(ClusterFileImage), &broken,
        sizeof(broken));
  rehash(overrun);
  ClusterFileData outside;
  CHECK_FALSE(read_cluster_file_memory(view(overrun), outside, &error));
  CHECK(error.find("image payload") != std::string::npos);
}

TEST_CASE("cluster file: a section of an unknown kind is skipped") {
  const test::TempDir tmp("engine_cluster_file");
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  const std::string path = tmp.file("forward.clusters");
  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  std::string file;
  REQUIRE(io::read_file(path, file) == io::Status::Ok);

  const std::string newer = with_unknown_section(file);
  CHECK(newer.size() > file.size());
  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file_memory(view(newer), read, &error), error);
  check_equal(read, data);
}

TEST_CASE("cluster file: the source path travels with the mesh, and its absence is not an error") {
  const test::TempDir tmp("engine_cluster_file");
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);

  // The path is a section of its own, so it is there and it is what was written.
  const std::string path = tmp.file("source.clusters");
  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  std::string file;
  REQUIRE(io::read_file(path, file) == io::Status::Ok);
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  usize source_at = 0;
  ClusterFileSection source{};
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(header) + sizeof(source) * i;
    ClusterFileSection section;
    std::memcpy(&section, file.data() + at, sizeof(section));
    if (section.kind == static_cast<u32>(ClusterSection::SourcePath)) {
      source_at = at;
      source = section;
    }
  }
  REQUIRE(source_at != 0);
  CHECK(source.element_size == 1);
  CHECK(source.element_count == data.source_path.size() + 1);  // the NUL travels too
  CHECK(std::string(cluster_section_name(source.kind)) == "source_path");
  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file(path, read, &error), error);
  CHECK(read.source_path == "content/samples/Terrain/terrain.gltf");

  // A file from a build that did not write the section: the kind becomes one this build does
  // not know, which is exactly what an older file looks like. Everything else survives and the
  // path comes back empty rather than as a failure.
  std::string older = file;
  ClusterFileSection renamed = source;
  renamed.kind = 31337;
  patch(older, source_at, &renamed, sizeof(renamed));
  rehash(older);
  ClusterFileData without;
  REQUIRE_MESSAGE(read_cluster_file_memory(view(older), without, &error), error);
  CHECK(without.source_path.empty());
  ClusterFileData expected = data;
  expected.source_path.clear();
  check_equal(without, expected);

  // A mesh built from bytes with no file behind them writes an empty path and reads one back.
  ClusterFileData anonymous;
  Vector<u32> anonymous_indices;
  make_fixture(anonymous, anonymous_indices);
  anonymous.source_path.clear();
  const std::string anonymous_path = tmp.file("anonymous.clusters");
  REQUIRE_MESSAGE(write_cluster_file(anonymous_path, anonymous, &error), error);
  ClusterFileData anonymous_read;
  REQUIRE_MESSAGE(read_cluster_file(anonymous_path, anonymous_read, &error), error);
  CHECK(anonymous_read.source_path.empty());
  check_equal(anonymous_read, anonymous);
}

TEST_CASE("cluster file: the source identity reads back without decoding the mesh") {
  const test::TempDir tmp("engine_cluster_file");
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  const std::string path = tmp.file("identity.clusters");
  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);

  // Two u64 in one section, the source hash first and the build key second.
  std::string file;
  REQUIRE(io::read_file(path, file) == io::Status::Ok);
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  usize identity_at = 0;
  ClusterFileSection identity{};
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(header) + sizeof(identity) * i;
    ClusterFileSection section;
    std::memcpy(&section, file.data() + at, sizeof(section));
    if (section.kind == static_cast<u32>(ClusterSection::SourceHash)) {
      identity_at = at;
      identity = section;
    }
  }
  REQUIRE(identity_at != 0);
  CHECK(identity.element_size == 8);
  CHECK(identity.element_count == 2);
  CHECK(std::string(cluster_section_name(identity.kind)) == "source_hash");

  // The cheap reader agrees with the full one and costs no mesh.
  u64 source_hash = 0;
  u64 build_key = 0;
  REQUIRE_MESSAGE(read_cluster_file_identity(path, source_hash, build_key, &error), error);
  CHECK(source_hash == data.source_hash);
  CHECK(build_key == data.build_key);
  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file(path, read, &error), error);
  CHECK(read.source_hash == source_hash);
  CHECK(read.build_key == build_key);

  // A file from before the section existed: the kind becomes one this build does not know, and
  // the identity reads as zero rather than as a failure, which is what makes a build rebuild it.
  std::string older = file;
  ClusterFileSection renamed = identity;
  renamed.kind = 31338;
  patch(older, identity_at, &renamed, sizeof(renamed));
  rehash(older);
  const std::string older_path = tmp.file("older.clusters");
  REQUIRE(io::write_file(older_path, older) == io::Status::Ok);
  REQUIRE_MESSAGE(read_cluster_file_identity(older_path, source_hash, build_key, &error), error);
  CHECK(source_hash == 0);
  CHECK(build_key == 0);
  ClusterFileData without;
  REQUIRE_MESSAGE(read_cluster_file(older_path, without, &error), error);
  CHECK(without.source_hash == 0);
  CHECK(without.build_key == 0);
  ClusterFileData expected = data;
  expected.source_hash = 0;
  expected.build_key = 0;
  check_equal(without, expected);

  // A file that is not a container, and one that is not there, are failures with zeros left.
  CHECK_FALSE(read_cluster_file_identity(tmp.file("missing.clusters"), source_hash, build_key));
  CHECK(source_hash == 0);
  std::string corrupt = file;
  corrupt[0] = 'X';
  const std::string corrupt_path = tmp.file("corrupt.clusters");
  REQUIRE(io::write_file(corrupt_path, corrupt) == io::Status::Ok);
  std::string message;
  CHECK_FALSE(read_cluster_file_identity(corrupt_path, source_hash, build_key, &message));
  CHECK_MESSAGE(message.find("magic") != std::string::npos, message);
}

TEST_CASE("cluster file: the cache key answers to everything that went into the build") {
  const ClusterLodOptions options;
  constexpr u32 k_page_bytes = 128 * 1024;
  const u64 key = cluster_cache_key(0x1234'5678'9abc'def0ull, options, true, k_page_bytes);
  CHECK(key != 0);
  CHECK(cluster_cache_key(0x1234'5678'9abc'def0ull, options, true, k_page_bytes) ==
        key);  // deterministic

  // Every input moves it: a different source, either per-cluster limit, the cone and ray
  // tracing switches, and welding.
  CHECK(cluster_cache_key(0x1234'5678'9abc'def1ull, options, true, k_page_bytes) != key);
  CHECK(cluster_cache_key(0x1234'5678'9abc'def0ull, options, false, k_page_bytes) != key);
  ClusterLodOptions other = options;
  other.max_triangles = options.max_triangles - 1;
  CHECK(cluster_cache_key(0x1234'5678'9abc'def0ull, other, true, k_page_bytes) != key);
  other = options;
  other.max_vertices = options.max_vertices - 1;
  CHECK(cluster_cache_key(0x1234'5678'9abc'def0ull, other, true, k_page_bytes) != key);
  other = options;
  other.normal_cones = !options.normal_cones;
  CHECK(cluster_cache_key(0x1234'5678'9abc'def0ull, other, true, k_page_bytes) != key);
  other = options;
  other.ray_tracing = !options.ray_tracing;
  CHECK(cluster_cache_key(0x1234'5678'9abc'def0ull, other, true, k_page_bytes) != key);
  // And the page target, because paging renumbers the clusters: two page sizes are two
  // containers, and "no pages" is a third.
  CHECK(cluster_cache_key(0x1234'5678'9abc'def0ull, options, true, k_page_bytes / 2) != key);
  CHECK(cluster_cache_key(0x1234'5678'9abc'def0ull, options, true, 0) != key);

  // The path is the root, "clusters", and sixteen lower-case hex digits of the key.
  CHECK(cluster_cache_path("D:/repo/ddc", 0x0123456789abcdefull) ==
        "D:/repo/ddc/clusters/0123456789abcdef.clusters");
  CHECK(cluster_cache_path("ddc", 0) == "ddc/clusters/0000000000000000.clusters");
  CHECK(cluster_cache_path("ddc", ~u64{0}) == "ddc/clusters/ffffffffffffffff.clusters");
}

TEST_CASE("cluster file: the derived-data root is the directory above that holds AGENTS.md") {
  const test::TempDir tmp("engine_cluster_file");
  const std::string deep = tmp.file("build/msvc-debug/bin");
  std::filesystem::create_directories(std::filesystem::path(deep));
  CHECK(find_ddc_root(deep).empty());  // nothing above it says this is a repository

  REQUIRE(io::write_file(tmp.file("AGENTS.md"), "# marker\n") == io::Status::Ok);
  CHECK(find_ddc_root(deep) == tmp.file("ddc"));
  CHECK(find_ddc_root(tmp.path()) == tmp.file("ddc"));  // the root itself counts
}

TEST_CASE("cluster file: broken files fail with distinct messages and an empty result") {
  const test::TempDir tmp("engine_cluster_file");
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  const std::string path = tmp.file("broken.clusters");
  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  std::string good;
  REQUIRE(io::read_file(path, good) == io::Status::Ok);

  Vector<std::string> messages;
  auto refuses = [&](const std::string& file, const char* what) {
    ClusterFileData read;
    std::string message;
    CHECK_FALSE(read_cluster_file_memory(view(file), read, &message));
    CHECK_MESSAGE(message.find(what) != std::string::npos, message);
    CHECK(read.mesh.mesh.clusters.empty());
    CHECK(read.mesh.mesh.vertices.empty());
    CHECK(read.materials.empty());
    CHECK(read.cluster_material.empty());
    CHECK(read.image_paths.empty());
    CHECK(read.source_path.empty());
    messages.push_back(message);
  };

  refuses(good.substr(0, good.size() / 2), "truncated");
  refuses(std::string(), "truncated");

  std::string bad_magic = good;
  bad_magic[0] = 'X';
  refuses(bad_magic, "magic");

  std::string bad_version = good;
  const u32 version = k_cluster_file_version + 1;
  patch(bad_version, 4, &version, sizeof(version));
  refuses(bad_version, "version");

  // The first section's payload is moved past the end. The table is covered by the content
  // hash, so this also pins the order of the checks: structure is reported before integrity.
  std::string past_end = good;
  ClusterFileHeader header;
  std::memcpy(&header, good.data(), sizeof(header));
  ClusterFileSection section;
  std::memcpy(&section, good.data() + sizeof(header), sizeof(section));
  section.offset = header.total_bytes - 16;
  patch(past_end, sizeof(header), &section, sizeof(section));
  refuses(past_end, "past the end");

  std::string corrupt = good;
  corrupt[corrupt.size() - 1] = static_cast<char>(corrupt[corrupt.size() - 1] ^ 0x5a);
  refuses(corrupt, "hash mismatch");

  // A required section renamed to an unknown kind is missing, not silently empty.
  std::string no_scalars = good;
  ClusterFileSection scalars;
  usize scalars_at = 0;
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(header) + sizeof(scalars) * i;
    std::memcpy(&scalars, good.data() + at, sizeof(scalars));
    if (scalars.kind == static_cast<u32>(ClusterSection::Scalars)) scalars_at = at;
  }
  REQUIRE(scalars_at != 0);
  std::memcpy(&scalars, good.data() + scalars_at, sizeof(scalars));
  scalars.kind = 7777;
  patch(no_scalars, scalars_at, &scalars, sizeof(scalars));
  rehash(no_scalars);
  refuses(no_scalars, "has no scalars");

  // Every failure says something different, which is the point of reporting them separately.
  for (u32 i = 0; i < messages.size(); ++i) {
    for (u32 j = i + 1; j < messages.size(); ++j) {
      if (i == 0 && j == 1) continue;  // both truncations, the same sentence with other numbers
      CHECK_MESSAGE(messages[i] != messages[j], messages[i]);
    }
  }
}

TEST_CASE("cluster file: a missing file and an unwritable directory report the reason") {
  const test::TempDir tmp("engine_cluster_file");
  ClusterFileData read;
  std::string error;
  CHECK_FALSE(read_cluster_file(tmp.file("nothing.clusters"), read, &error));
  CHECK(error.find("cannot read") != std::string::npos);
  CHECK(read.mesh.mesh.clusters.empty());

  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  error.clear();
  CHECK_FALSE(write_cluster_file(tmp.file("no/such/dir/out.clusters"), data, &error));
  CHECK(error.find("cannot write") != std::string::npos);
}
