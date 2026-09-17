// The .clusters container: a DAG plus its materials written, read back, and compared array by
// array; the forward-compatibility rule (unknown sections are skipped); and the five ways a
// file can be broken, each with its own message and an untouched output.
#include <core/hash/hash.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>

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

// A DAG over a small terrain, wrapped with two materials, one image path, and a material index
// per cluster. `indices` is what validate_cluster_lod checks the result against.
void make_fixture(ClusterFileData& data, Vector<u32>& indices) {
  Vector<Vec3> positions;
  make_terrain(65, 10.0f, positions, indices);
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(positions, indices, ClusterLodOptions{}, data.mesh, &error),
                  error);
  data.materials.resize(2);
  data.materials[0].base_color = Vec4{0.82f, 0.71f, 0.49f, 1.0f};
  data.materials[0].metallic = 0.0f;
  data.materials[0].roughness = 0.85f;
  data.materials[0].base_color_image = 0;
  data.materials[1].base_color = Vec4{0.18f, 0.32f, 0.91f, 1.0f};
  data.materials[1].metallic = 1.0f;
  data.materials[1].roughness = 0.2f;
  data.image_paths.push_back("textures/sand_basecolor.png");
  data.image_paths.push_back("");  // an embedded image keeps its slot with an empty path
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
  CHECK(same_bytes(read.mesh.level_cluster_counts, written.mesh.level_cluster_counts));
  CHECK(same_bytes(read.cluster_material, written.cluster_material));
  CHECK(same_bytes(read.materials, written.materials));
  CHECK(read.mesh.group_count == written.mesh.group_count);
  CHECK(read.mesh.leaf_triangle_count == written.mesh.leaf_triangle_count);
  CHECK(read.mesh.mesh.source_vertex_count == written.mesh.mesh.source_vertex_count);
  CHECK(read.mesh.mesh.source_triangle_count == written.mesh.mesh.source_triangle_count);
  REQUIRE(read.image_paths.size() == written.image_paths.size());
  for (u32 i = 0; i < read.image_paths.size(); ++i)
    CHECK(read.image_paths[i] == written.image_paths[i]);
}

struct TempDir {
  std::string path;
  TempDir() {
    const auto p = std::filesystem::temp_directory_path() / "engine_cluster_file_tests";
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    path = p.string();
    for (char& c : path) {
      if (c == '\\') c = '/';
    }
  }
  ~TempDir() { std::filesystem::remove_all(std::filesystem::path(path)); }
};

std::span<const u8> view(const std::string& file) {
  return std::span<const u8>(reinterpret_cast<const u8*>(file.data()), file.size());
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
  TempDir tmp;
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  const std::string path = tmp.path + "/terrain.clusters";

  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file(path, read, &error), error);
  check_equal(read, data);
  CHECK_MESSAGE(validate_cluster_lod(read.mesh, indices, &error), error);
  CHECK(read.mesh.leaf_triangle_count == 8192);
  CHECK(read.mesh.level_cluster_counts.size() >= 3);

  // The bytes on disk say what the header promised, and the hash is the one computed in memory.
  std::string file;
  REQUIRE(io::read_file(path, file) == io::Status::Ok);
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  CHECK(std::memcmp(header.magic, "CLST", 4) == 0);
  CHECK(header.version == k_cluster_file_version);
  CHECK(header.flags == 0);
  CHECK(header.section_count == 13);
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

TEST_CASE("cluster file: a section of an unknown kind is skipped") {
  TempDir tmp;
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  const std::string path = tmp.path + "/forward.clusters";
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

TEST_CASE("cluster file: broken files fail with distinct messages and an empty result") {
  TempDir tmp;
  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  const std::string path = tmp.path + "/broken.clusters";
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
  TempDir tmp;
  ClusterFileData read;
  std::string error;
  CHECK_FALSE(read_cluster_file(tmp.path + "/nothing.clusters", read, &error));
  CHECK(error.find("cannot read") != std::string::npos);
  CHECK(read.mesh.mesh.clusters.empty());

  ClusterFileData data;
  Vector<u32> indices;
  make_fixture(data, indices);
  error.clear();
  CHECK_FALSE(write_cluster_file(tmp.path + "/no/such/dir/out.clusters", data, &error));
  CHECK(error.find("cannot write") != std::string::npos);
}
