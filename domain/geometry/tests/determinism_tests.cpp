// The content build's output is a function of its input on every toolchain (ADR-0035, geometry.md
// "The same bytes from every toolchain"). A fixture whose every coordinate is an exact binary
// fraction, made with integer arithmetic and no C library call, goes through what the content
// build does to a mesh — the vertex ids, the weld, the LOD builder with the UVs, a seam and a morph
// channel, the cone refit, the quantization, the pages, the container — and the hash of every
// section of the file it writes is compared with a table committed here. The table was taken on
// MSVC and agrees on GCC 13 and Clang 18 at x86-64-v3 and GCC 14 at x86-64-v2
// (docs/experiments/content-build-determinism.md).
//
// A failure here means one of two things, and the message cannot tell them apart:
//   - the build changed on purpose (a builder, a default, the format): bump
//     `k_cluster_cache_version` and replace the table with the one the failure prints;
//   - one toolchain computes something another does not: a target that took back the tree's
//     -ffp-contract=off (cmake/EngineFpContraction.cmake), a C library call in a decision (see the
//     cone refit), an unstable sort or a hash map's order reaching the output. **That is a bug,
//     not a table to replace**: the table is right on MSVC, where nothing contracts, and on every
//     v2 build, where nothing can.
#include <core/hash/hash.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

// A 65 x 65 heightfield in two UV islands that meet at column 32, where the vertices are doubled
// (same position, different UV), and one morph channel over a disc of the left island. Heights are
// integer bumps over 2^18 and positions are quarter units, so every float is exact and nothing that
// makes the fixture depends on a compiler: that is the input half of "a function of its input".
struct Fixture {
  Vector<Vec3> positions;
  Vector<Vec2> uvs;
  Vector<Vec3> normals;  // empty: the builder computes them, which is arithmetic worth pinning
  Vector<u32> indices;
  Vector<MorphChannelSource> morph;
  Vector<u32> ids;
};

constexpr u32 k_grid = 65;
constexpr u32 k_seam = 32;

i64 height_of(i64 u, i64 v) {
  struct Bump {
    i64 cx, cz, r, a;
  };
  constexpr Bump bumps[] = {{-12, -6, 20, 3}, {10, 8, 16, -2}, {3, -16, 12, 4}, {-6, 14, 14, 2}};
  i64 h = 0;
  for (const Bump& b : bumps) {
    const i64 d2 = (u - b.cx) * (u - b.cx) + (v - b.cz) * (v - b.cz);
    const i64 r2 = b.r * b.r;
    if (d2 < r2) h += b.a * (r2 - d2) * (r2 - d2);
  }
  return h + (u * 7 + v * 13) % 5;  // a little roughness, so no group simplifies for free
}

Fixture make_fixture() {
  Fixture f;
  constexpr i64 half = (k_grid - 1) / 2;
  // Island 0 holds columns 0..k_seam, island 1 columns k_seam..k_grid-1; vertex (island, x, z).
  auto first_column = [](u32 island) { return island == 0 ? 0u : k_seam; };
  auto columns = [](u32 island) { return island == 0 ? k_seam + 1 : k_grid - k_seam; };
  u32 base[2] = {0, 0};
  for (u32 island = 0; island < 2; ++island) {
    base[island] = f.positions.size();
    for (u32 z = 0; z < k_grid; ++z) {
      for (u32 c = 0; c < columns(island); ++c) {
        const u32 x = first_column(island) + c;
        const i64 u = static_cast<i64>(x) - half;
        const i64 v = static_cast<i64>(z) - half;
        f.positions.push_back(Vec3{static_cast<f32>(u) * 0.25f,
                                   static_cast<f32>(height_of(u, v)) / 262144.0f,
                                   static_cast<f32>(v) * 0.25f});
        // Island 0 is u in [0, 0.25], island 1 u in [0.5, 0.75]: the seam's two copies of a
        // vertex are a quarter of the atlas apart.
        f.uvs.push_back(Vec2{static_cast<f32>(island) * 0.5f + static_cast<f32>(c) / 128.0f,
                             static_cast<f32>(z) / 64.0f});
      }
    }
  }
  auto vertex = [&](u32 x, u32 z, u32 island) {
    return base[island] + z * columns(island) + (x - first_column(island));
  };
  for (u32 z = 0; z + 1 < k_grid; ++z) {
    for (u32 x = 0; x + 1 < k_grid; ++x) {
      const u32 island = x < k_seam ? 0u : 1u;
      const u32 a = vertex(x, z, island);
      const u32 b = vertex(x, z + 1, island);
      const u32 c = vertex(x + 1, z, island);
      const u32 d = vertex(x + 1, z + 1, island);
      f.indices.push_back(a);
      f.indices.push_back(b);
      f.indices.push_back(c);
      f.indices.push_back(c);
      f.indices.push_back(b);
      f.indices.push_back(d);
    }
  }
  // One channel: a bulge of exact deltas over a disc of the left island.
  MorphChannelSource channel;
  channel.name = "bulge";
  for (u32 z = 8; z <= 24; ++z) {
    for (u32 x = 8; x <= 24; ++x) {
      const i64 d2 = (static_cast<i64>(x) - 16) * (static_cast<i64>(x) - 16) +
                     (static_cast<i64>(z) - 16) * (static_cast<i64>(z) - 16);
      if (d2 >= 64) continue;
      channel.vertices.push_back(vertex(x, z, 0));
      channel.position_deltas.push_back(Vec3{0.0f, static_cast<f32>(64 - d2) / 1024.0f, 0.0f});
    }
  }
  f.morph.push_back(std::move(channel));
  return f;
}

struct SectionHash {
  u32 kind;
  u64 hash;
};

// Every section of the file at `path`, as the table lists them: its kind and the hash of its
// payload. False when the file cannot be read or its table runs past the end.
bool section_hashes(const std::string& path, u64& content_hash, Vector<SectionHash>& out) {
  std::string file;
  if (io::read_file(path, file) != io::Status::Ok || file.size() < sizeof(ClusterFileHeader))
    return false;
  ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  content_hash = header.content_hash;
  out.clear();
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(header) + sizeof(ClusterFileSection) * i;
    if (at + sizeof(ClusterFileSection) > file.size()) return false;
    ClusterFileSection section;
    std::memcpy(&section, file.data() + at, sizeof(section));
    const u64 bytes = u64{section.element_size} * section.element_count;
    if (section.offset > file.size() || bytes > file.size() - section.offset) return false;
    out.push_back(SectionHash{section.kind, hash_bytes(file.data() + section.offset, bytes)});
  }
  return true;
}

// Builds the fixture as the content build would, writes it, and compares every section with
// `golden`. On a mismatch it prints the table this build made, in the form the source wants.
void check_against(const char* name, bool ray_tracing, u64 golden_content,
                   std::span<const SectionHash> golden) {
  Fixture f = make_fixture();
  // The content build's order: ids from a position weld of the source, then the weld itself with
  // every stream the mesh owns, then the builder.
  const u32 source_vertices = f.positions.size();
  // Two copies of each of the 65 seam vertices share a position, so there is one id fewer a row.
  CHECK(position_weld_ids(f.positions, f.ids) == source_vertices - k_grid);
  // Nothing merges — the seam's copies differ in UV — so the weld only renumbers by first use.
  CHECK(weld_vertices(f.positions, f.normals, f.uvs,
                      std::span<u32>(f.indices.data(), f.indices.size()), nullptr, &f.morph,
                      &f.ids) == source_vertices);
  AttributeSource attributes;
  attributes.uvs = std::span<const Vec2>(f.uvs.data(), f.uvs.size());
  attributes.morph = std::span<const MorphChannelSource>(f.morph.data(), f.morph.size());
  attributes.vertex_ids = std::span<const u32>(f.ids.data(), f.ids.size());
  attributes.vertex_id_source = VertexIdSource::position_weld;

  ClusterLodOptions options;
  options.ray_tracing = ray_tracing;
  ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(f.positions, f.indices, options, data.mesh, &error, attributes),
                  error);
  REQUIRE_MESSAGE(validate_cluster_lod(data.mesh, f.indices, &error), error);
  REQUIRE_MESSAGE(build_cluster_pages(data.mesh, ClusterPagesOptions{}, data.pages, &error), error);
  data.materials.push_back(ClusterFileMaterial{});
  data.cluster_material.resize(data.mesh.mesh.clusters.size(), 0u);
  data.source_path = "determinism-fixture.gltf";
  data.source_hash = 0x1234;
  data.build_key = 0x5678;

  const test::TempDir tmp("geometry_determinism");
  const std::string path = tmp.file("fixture.clusters");
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  u64 content = 0;
  Vector<SectionHash> got;
  REQUIRE(section_hashes(path, content, got));
  CHECK(content == cluster_file_hash(data));

  const std::string what = std::string(name) + ": " +
                           std::to_string(data.mesh.mesh.clusters.size()) + " clusters in " +
                           std::to_string(data.mesh.level_cluster_counts.size()) + " levels, " +
                           std::to_string(data.pages.pages.size()) + " pages";
  MESSAGE(what);
  bool same = content == golden_content && got.size() == golden.size();
  for (u32 i = 0; same && i < got.size(); ++i)
    same = got[i].kind == golden[i].kind && got[i].hash == golden[i].hash;
  if (!same) {
    // The table this build made, ready to paste — after reading the header comment of this file.
    std::string table =
        "this build's table:\n  constexpr u64 k_" + std::string(name) + "_content = 0x";
    char word[64];
    std::snprintf(word, sizeof(word), "%016llxull;\n", static_cast<unsigned long long>(content));
    table += word;
    for (const SectionHash& s : got) {
      const SectionHash* want = nullptr;
      for (const SectionHash& g : golden)
        if (g.kind == s.kind) want = &g;
      std::snprintf(word, sizeof(word), "    {%u, 0x%016llxull},", s.kind,
                    static_cast<unsigned long long>(s.hash));
      table += word;
      table += "  // ";
      table += cluster_section_name(s.kind);
      if (want == nullptr || want->hash != s.hash) table += "  <-- differs";
      table += '\n';
    }
    FAIL_CHECK(what << " does not match the committed hashes; " << table);
  }
}

// Taken on MSVC 14.51 (msvc-debug and msvc-release agree), 2026-09-24, at cache version 13; the
// section list grew by kind 32 (`textures`, empty here) at version 14, which moved the two content
// hashes — they cover the section table — and no section's own hash. 180 clusters in 8 levels and
// 2 pages; the ray-tracing build 266 in 8 and 3. The empty sections (no images, no skin, no
// textures) all hash to 0x9ca066f1a4ab2eea, which is `hash_bytes` of nothing.
constexpr u64 k_raster_content = 0xbf1fdadb89fa1be5ull;
constexpr SectionHash k_raster_sections[] = {
    {1, 0x5c01f613aafe7769ull},   // clusters
    {2, 0x098fa5888c192cfbull},   // lod
    {3, 0x4f88b1f616e96b2aull},   // vertices
    {4, 0x549e8bc83195223eull},   // attributes
    {5, 0x0511ccb0265e0c7full},   // triangles
    {6, 0x59219d8def5a67e4ull},   // vertex_source
    {7, 0x24e2a33c09402911ull},   // level_cluster_counts
    {8, 0xc62527f0f5484550ull},   // cluster_material
    {9, 0x7b807a2ae7f596bbull},   // materials
    {10, 0x9ca066f1a4ab2eeaull},  // image_paths
    {11, 0x9ca066f1a4ab2eeaull},  // strings
    {12, 0xe8fd214c8a793176ull},  // scalars
    {13, 0x8d4c63b865508f1bull},  // quantized
    {14, 0x400e45916d2cca6eull},  // source_path
    {15, 0xfc56d5a0519a868eull},  // source_hash
    {16, 0x75e702c14077ee60ull},  // pages
    {17, 0x43350b1e0c2c7469ull},  // page_children
    {18, 0x1d46f3ca5f069ef3ull},  // page_scalars
    {19, 0x9ca066f1a4ab2eeaull},  // skin
    {20, 0x1e3a02855f6b4e6bull},  // skin_scalars
    {21, 0x9ca066f1a4ab2eeaull},  // images
    {22, 0x9ca066f1a4ab2eeaull},  // image_bytes
    {23, 0x7ff6f55ce7109ccaull},  // morph_channels
    {24, 0xcb3b1bec74dc0f55ull},  // morph_names
    {25, 0x4128961e3a656e56ull},  // morph_cluster_slices
    {26, 0x1f1466614dc48f47ull},  // morph_slices
    {27, 0xa15e2dff30751269ull},  // morph_indices
    {28, 0x1d8ccfd45e30b98cull},  // morph_deltas
    {29, 0xaec380763763bf80ull},  // morph_scalars
    {30, 0x37c459dab3378470ull},  // vertex_ids
    {31, 0x89af6b25f28e6045ull},  // vertex_id_scalars
    {32, 0x9ca066f1a4ab2eeaull},  // textures: none, since the fixture has no images
};
constexpr u64 k_ray_tracing_content = 0xc86611875c8af106ull;
constexpr SectionHash k_ray_tracing_sections[] = {
    {1, 0xfcbbc6820607644cull},   // clusters
    {2, 0x9f1ca027f3b30a57ull},   // lod
    {3, 0x4e3ea230de41f305ull},   // vertices
    {4, 0x27d9edd010e748faull},   // attributes
    {5, 0x21d5807b0137f350ull},   // triangles
    {6, 0xf48fb90b39ec96d9ull},   // vertex_source
    {7, 0xba27f7262e0c0732ull},   // level_cluster_counts
    {8, 0x0ee58acfbed08e08ull},   // cluster_material
    {9, 0x7b807a2ae7f596bbull},   // materials
    {10, 0x9ca066f1a4ab2eeaull},  // image_paths
    {11, 0x9ca066f1a4ab2eeaull},  // strings
    {12, 0x8480eadc1eea49ddull},  // scalars
    {13, 0x6afd7890ac5571d5ull},  // quantized
    {14, 0x400e45916d2cca6eull},  // source_path
    {15, 0xfc56d5a0519a868eull},  // source_hash
    {16, 0x4787cbd01a2fb505ull},  // pages
    {17, 0x54b77d97acdf5536ull},  // page_children
    {18, 0x1d46f3ca5f069ef3ull},  // page_scalars
    {19, 0x9ca066f1a4ab2eeaull},  // skin
    {20, 0x1e3a02855f6b4e6bull},  // skin_scalars
    {21, 0x9ca066f1a4ab2eeaull},  // images
    {22, 0x9ca066f1a4ab2eeaull},  // image_bytes
    {23, 0x7ff6f55ce7109ccaull},  // morph_channels
    {24, 0xcb3b1bec74dc0f55ull},  // morph_names
    {25, 0xa371563b6d631f45ull},  // morph_cluster_slices
    {26, 0x766233c43cc9a426ull},  // morph_slices
    {27, 0xfa1d73b07a6524d7ull},  // morph_indices
    {28, 0xd3de75d12202556eull},  // morph_deltas
    {29, 0xfafe6ffb3a6a47f8ull},  // morph_scalars
    {30, 0x2b3195264c33db68ull},  // vertex_ids
    {31, 0x89af6b25f28e6045ull},  // vertex_id_scalars
    {32, 0x9ca066f1a4ab2eeaull},  // textures: none, since the fixture has no images
};

}  // namespace

TEST_CASE("determinism: the margin constants are the cosine and sine of the margin") {
  // Correctly rounded values of cos and sin at the double k_cone_margin widens to. A C library may
  // be an ulp off either way, which is exactly why the refit does not call one; this only checks
  // that the constants and the margin still belong together.
  const f64 m = static_cast<f64>(k_cone_margin);
  CHECK(std::fabs(k_cone_margin_cos - std::cos(m)) <= 0x1p-53);  // one ulp in [0.5, 1)
  CHECK(std::fabs(k_cone_margin_sin - std::sin(m)) <= 0x1p-62);  // one ulp in [2^-10, 2^-9)
}

TEST_CASE("determinism: the content build writes the committed bytes (raster clusters)") {
  check_against("raster", false, k_raster_content,
                std::span<const SectionHash>(k_raster_sections, std::size(k_raster_sections)));
}

TEST_CASE("determinism: the content build writes the committed bytes (ray-tracing clusters)") {
  check_against(
      "ray_tracing", true, k_ray_tracing_content,
      std::span<const SectionHash>(k_ray_tracing_sections, std::size(k_ray_tracing_sections)));
}
