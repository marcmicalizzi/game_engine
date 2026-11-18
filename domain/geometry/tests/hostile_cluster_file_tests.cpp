// Hostile `.clusters` containers (T187, from T13's reading, docs/experiments/fuzzing-design.md): a
// client names one to `render.load`, and the content hash is not cryptographic, so a crafted file
// carries a valid one. Each case is a container a build writes, edited in one field and rehashed,
// by a function that returns its bytes so that the fuzz harness can lift it as a seed. Each was
// accepted (or asserted in a debug build) before the reader checked a cluster's ranges.

#include <core/hash/hash.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace hostile_clusters {

// A 9 x 9 heightfield's DAG with one material, written by the engine's own writer and read back
// as bytes: the valid container every case below edits. `dir` is the caller's scratch directory.
std::string valid_container(const std::string& dir) {
  Vector<Vec3> positions;
  Vector<u32> indices;
  const u32 n = 9;
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = static_cast<f32>(x);
      const f32 fz = static_cast<f32>(z);
      positions.push_back(Vec3{fx, 0.3f * std::sin(fx) * std::cos(fz), fz});
    }
  }
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x;
      for (const u32 v : {a, a + n, a + 1, a + 1, a + n, a + n + 1})
        indices.push_back(v);
    }
  }
  ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(positions, indices, ClusterLodOptions{}, data.mesh, &error),
                  error);
  data.materials.resize(1);
  for (u32 i = 0; i < data.mesh.mesh.clusters.size(); ++i)
    data.cluster_material.push_back(0);
  const std::string path = io::join_path(dir, "valid.clusters");
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  std::string bytes;
  REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
  return bytes;
}

// The table record of `kind`'s section: its offset in the file, 0 when there is none.
usize find_section(const std::string& file, ClusterSection kind, ClusterFileSection& out) {
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(ClusterFileHeader) + sizeof(ClusterFileSection) * i;
    std::memcpy(&out, file.data() + at, sizeof(out));
    if (out.kind == static_cast<u32>(kind)) return at;
  }
  return 0;
}

// The content hash recomputed after an edit, as the format's own check computes it, so the edit is
// what the reader meets rather than a hash mismatch.
void rehash(std::string& file) {
  const u64 content = hash_bytes(file.data() + 32, file.size() - 32);
  std::memcpy(file.data() + 24, &content, sizeof(content));
}

// Cluster 0's descriptor, edited by `edit`.
template <class Edit>
std::string with_first_cluster(std::string file, Edit edit) {
  ClusterFileSection clusters;
  REQUIRE(find_section(file, ClusterSection::Clusters, clusters) != 0);
  ClusterDesc desc;
  std::memcpy(&desc, file.data() + clusters.offset, sizeof(desc));
  edit(desc, file);
  std::memcpy(file.data() + clusters.offset, &desc, sizeof(desc));
  rehash(file);
  return file;
}

// Cluster 0 names vertices from the end of the stream on: every read of its vertices is past it.
// The quantized section is renamed to a kind no build knows, as a file from before that section
// reads, so the reader requantizes and the cone refit indexes the cluster's triangles' vertices.
std::string vertex_range_past_stream(const std::string& dir) {
  std::string file = with_first_cluster(valid_container(dir), [](ClusterDesc& c, std::string& f) {
    ClusterFileSection vertices;
    REQUIRE(find_section(f, ClusterSection::Vertices, vertices) != 0);
    c.vertex_offset = static_cast<u32>(vertices.element_count);
  });
  ClusterFileSection quantized;
  const usize at = find_section(file, ClusterSection::Quantized, quantized);
  REQUIRE(at != 0);
  quantized.kind = 4242;
  std::memcpy(file.data() + at, &quantized, sizeof(quantized));
  rehash(file);
  return file;
}

// Cluster 0 names triangles from the end of the stream on.
std::string triangle_range_past_stream(const std::string& dir) {
  return with_first_cluster(valid_container(dir), [](ClusterDesc& c, std::string& f) {
    ClusterFileSection triangles;
    REQUIRE(find_section(f, ClusterSection::Triangles, triangles) != 0);
    c.triangle_offset = static_cast<u32>(triangles.element_count);
  });
}

// Cluster 0's first triangle names local vertex 255, which the cluster does not hold.
std::string triangle_local_outside_cluster(const std::string& dir) {
  return with_first_cluster(valid_container(dir), [](ClusterDesc& c, std::string& f) {
    REQUIRE(c.vertex_count < 255u);
    ClusterFileSection triangles;
    REQUIRE(find_section(f, ClusterSection::Triangles, triangles) != 0);
    f[static_cast<usize>(triangles.offset) + usize{c.triangle_offset} * 4] =
        static_cast<char>(0xff);
  });
}

}  // namespace hostile_clusters

namespace {

bool read_bytes(const std::string& bytes, ClusterFileData& out, std::string& error) {
  return read_cluster_file_memory(
      std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()), out, &error);
}

}  // namespace

TEST_CASE("cluster file hostile: a cluster's ranges are checked against its streams at load") {
  const test::TempDir tmp("engine_geometry_hostile_clusters");
  ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(read_bytes(hostile_clusters::valid_container(tmp.path()), data, error), error);
  CHECK_FALSE(data.mesh.mesh.clusters.empty());

  SUBCASE("vertices past the stream, on the requantizing path") {
    CHECK_FALSE(read_bytes(hostile_clusters::vertex_range_past_stream(tmp.path()), data, error));
    CHECK(error.find("names vertices") != std::string::npos);
  }
  SUBCASE("triangles past the stream") {
    CHECK_FALSE(read_bytes(hostile_clusters::triangle_range_past_stream(tmp.path()), data, error));
    CHECK(error.find("names triangles") != std::string::npos);
  }
  SUBCASE("a triangle naming a vertex outside its cluster") {
    CHECK_FALSE(
        read_bytes(hostile_clusters::triangle_local_outside_cluster(tmp.path()), data, error));
    CHECK(error.find("outside the cluster") != std::string::npos);
  }
  SUBCASE("the paged reader, whose resident decode carries no vertex stream") {
    const std::string path = io::join_path(tmp.path(), "hostile.clusters");
    REQUIRE(io::write_file(path, hostile_clusters::vertex_range_past_stream(tmp.path())) ==
            io::Status::Ok);
    ClusterFileReader reader;
    CHECK_FALSE(reader.open(path, &data, &error));
    CHECK(error.find("names vertices") != std::string::npos);
  }
  CHECK(data.mesh.mesh.clusters.empty());
}
