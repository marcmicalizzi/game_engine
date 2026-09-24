// The atlas step over a whole mesh (repack.h, docs/subsystems/atlas.md): the shredded-atlas torus
// (domain/geometry's stress fixture) re-charted into a handful of charts with its texture carried
// across, the same bytes at any thread count, the materials it leaves alone, and the per-vertex
// streams that have to follow a vertex into every copy of it.

#include <core/jobs/job_system.h>
#include <domain/assets/gltf.h>
#include <domain/atlas/repack.h>
#include <domain/geometry/stress_mesh.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::atlas;

namespace {

// Large enough that a quad of the fixture is several texels in either atlas: at 256 a quad is
// about four texels and the error is the two bilinear footprints at island borders, not the rebake.
constexpr u32 k_texture = 512;

// The shredded torus as an imported mesh: one primitive, one material whose base colour is the
// probe texture, embedded as a PNG the way a GLB carries it.
assets::MeshData shredded_mesh(u32& islands, u32& seam_vertices) {
  geometry::ShreddedAtlasOptions options;
  options.segments = 64;
  options.rings = 32;
  options.island_side = 4;
  geometry::ShreddedAtlasMesh torus;
  geometry::build_shredded_atlas_torus(options, torus);
  islands = torus.island_count;
  seam_vertices = torus.seam_vertices;

  assets::MeshData mesh;
  mesh.positions = torus.positions;
  mesh.normals = torus.normals;
  mesh.uvs = torus.uvs;
  mesh.indices = torus.indices;
  mesh.primitives.push_back(assets::Primitive{0, torus.indices.size(), 0, -1});
  assets::Material material;
  material.name = "probe";
  material.base_color_image = 0;
  mesh.materials.push_back(material);
  Vector<u8> rgba;
  geometry::build_atlas_probe_texture(k_texture, torus.atlas_cells, rgba);
  assets::ImageRef image;
  image.name = "probe";
  image.mime_type = "image/png";
  REQUIRE(image::encode_png(k_texture, k_texture, 4, std::span<const u8>(rgba.data(), rgba.size()),
                            image.bytes));
  mesh.images.push_back(std::move(image));
  return mesh;
}

// Vertices that share a position with another vertex that disagrees about the UV: what the LOD
// builder's seam rule protects, counted the way `engine-content stats` counts it.
u32 count_seam_vertices(const assets::MeshData& mesh) {
  struct Key {
    Vec3 p;
    Vec2 uv;
  };
  Vector<Key> keys;
  Vector<u8> referenced(mesh.positions.size(), u8{0});
  for (const u32 i : mesh.indices)
    referenced[i] = 1u;
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    if (referenced[v] != 0) keys.push_back(Key{mesh.positions[v], mesh.uvs[v]});
  }
  std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
    if (a.p.x != b.p.x) return a.p.x < b.p.x;
    if (a.p.y != b.p.y) return a.p.y < b.p.y;
    if (a.p.z != b.p.z) return a.p.z < b.p.z;
    if (a.uv.x != b.uv.x) return a.uv.x < b.uv.x;
    return a.uv.y < b.uv.y;
  });
  u32 seams = 0;
  for (u32 i = 0; i < keys.size();) {
    u32 j = i;
    bool differs = false;
    while (j < keys.size() && keys[j].p.x == keys[i].p.x && keys[j].p.y == keys[i].p.y &&
           keys[j].p.z == keys[i].p.z) {
      differs = differs || keys[j].uv.x != keys[i].uv.x || keys[j].uv.y != keys[i].uv.y;
      ++j;
    }
    if (differs) seams += j - i;
    i = j;
  }
  return seams;
}

template <class T>
bool same_bytes(const Vector<T>& a, const Vector<T>& b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

bool same_mesh(const assets::MeshData& a, const assets::MeshData& b) {
  if (!same_bytes(a.positions, b.positions) || !same_bytes(a.normals, b.normals) ||
      !same_bytes(a.uvs, b.uvs) || !same_bytes(a.indices, b.indices) ||
      !same_bytes(a.vertex_ids, b.vertex_ids) || a.images.size() != b.images.size() ||
      a.materials.size() != b.materials.size()) {
    return false;
  }
  for (u32 i = 0; i < a.images.size(); ++i) {
    if (!same_bytes(a.images[i].bytes, b.images[i].bytes) || a.images[i].uri != b.images[i].uri)
      return false;
  }
  for (u32 m = 0; m < a.materials.size(); ++m) {
    if (a.materials[m].base_color_image != b.materials[m].base_color_image ||
        a.materials[m].normal_image != b.materials[m].normal_image)
      return false;
  }
  return true;
}

}  // namespace

TEST_CASE(
    "atlas: a shredded atlas is re-charted into a few charts and its texture carried across") {
  u32 islands = 0;
  u32 seams_before = 0;
  assets::MeshData mesh = shredded_mesh(islands, seams_before);
  const assets::MeshData source = mesh;
  RepackReport report;
  std::string error;
  REQUIRE_MESSAGE(repack_atlas(mesh, "", RepackOptions{}, nullptr, report, &error), error);

  CHECK(report.materials_repacked == 1);
  REQUIRE(report.materials.size() == 1);
  REQUIRE(report.images.size() == 1);
  const RepackedMaterial& m = report.materials[0];
  const RebakedImage& im = report.images[0];
  MESSAGE("islands " << islands << " -> charts " << m.charts << "; seam vertices " << seams_before
                     << "; utilization " << m.utilization << " after " << m.pack_attempts
                     << " packings; rebake error mean " << im.error.mean << ", p99 " << im.error.p99
                     << ", max " << im.error.max << " over " << im.error.samples << " samples");
  CHECK(m.triangles == source.indices.size() / 3);
  CHECK(m.resolution == k_texture);
  CHECK(m.charts * 8 < islands);
  CHECK(m.unatlased_triangles == 0);
  // 4,096 triangles is over twice the default proxy, so the charts were cut on a proxy.
  CHECK(m.proxy_triangles > 0);
  CHECK(m.proxy_triangles <= RepackOptions{}.proxy_triangles + 64);
  CHECK(m.folded_triangles * 100 < m.triangles);  // a smooth torus: under 1% folds
  CHECK(report.materials_declined == 0);
  // A smooth surface leaves no chart under a texel (atlas.md rule 8).
  CHECK(m.crumb_charts == 0);
  CHECK(m.crumb_triangles == 0);

  // Every triangle is where it was, corner for corner; only its vertices' UVs changed.
  REQUIRE(mesh.indices.size() == source.indices.size());
  for (u32 i = 0; i < mesh.indices.size(); ++i) {
    const Vec3 a = mesh.positions[mesh.indices[i]];
    const Vec3 b = source.positions[source.indices[i]];
    REQUIRE((a.x == b.x && a.y == b.y && a.z == b.z));
    const Vec3 na = mesh.normals[mesh.indices[i]];
    const Vec3 nb = source.normals[source.indices[i]];
    REQUIRE((na.x == nb.x && na.y == nb.y && na.z == nb.z));
    const Vec2 uv = mesh.uvs[mesh.indices[i]];
    REQUIRE((uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f));
  }

  // No triangle is stretched across the atlas: the charts are packed side by side, so their
  // triangles' UV areas add up to less than the atlas (atlas.md rule 7: a face whose corners
  // xatlas put in two charts once covered several atlases' worth of area on a generated prop).
  f64 uv_area = 0.0;
  for (u32 t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const Vec2 a = mesh.uvs[mesh.indices[t]];
    const Vec2 b = mesh.uvs[mesh.indices[t + 1]];
    const Vec2 c = mesh.uvs[mesh.indices[t + 2]];
    const f64 ax = static_cast<f64>(a.x);
    const f64 ay = static_cast<f64>(a.y);
    uv_area += std::fabs((static_cast<f64>(b.x) - ax) * (static_cast<f64>(c.y) - ay) -
                         (static_cast<f64>(c.x) - ax) * (static_cast<f64>(b.y) - ay)) *
               0.5;
  }
  MESSAGE("summed UV area " << uv_area << " of the atlas");
  CHECK(uv_area <= 1.0);

  // The weld the content build runs next merges what the new atlas made one point.
  assets::weld_vertices(mesh);
  const u32 seams_after = count_seam_vertices(mesh);
  MESSAGE("seam vertices after the weld: " << seams_after << " of " << mesh.positions.size());
  // A torus cannot be flattened without cutting it, and at 4,096 triangles a chart border is a
  // large share of any chart; the ratio is the fixture's, not a promise (E10 has the real ones).
  CHECK(seams_after * 2 < seams_before);

  // The texture: one rebaked PNG in place of the probe, at the probe's size, named by the slot.
  REQUIRE(mesh.images.size() == 1);
  CHECK(mesh.materials[0].base_color_image == 0);
  CHECK(mesh.images[0].uri.empty());
  CHECK(mesh.images[0].mime_type == "image/png");
  image::Image decoded;
  REQUIRE(image::decode_image(
      std::span<const u8>(mesh.images[0].bytes.data(), mesh.images[0].bytes.size()), decoded, 4));
  CHECK(decoded.width == k_texture);
  CHECK(decoded.height == k_texture);
  CHECK(im.png_bytes == mesh.images[0].bytes.size());
  // The probe is a flat hue per island with a gentle gradient: the same point reads the same
  // colour through either atlas, to within the resampling.
  CHECK(im.error.samples > 1000);
  CHECK(im.error.mean < 1.0);
  CHECK(im.error.p99 < 4.0);
}

TEST_CASE("atlas: the repack is the same bytes on any number of threads") {
  u32 islands = 0;
  u32 seams = 0;
  const assets::MeshData source = shredded_mesh(islands, seams);
  assets::MeshData serial = source;
  RepackReport report;
  REQUIRE(repack_atlas(serial, "", RepackOptions{}, nullptr, report));
  for (const u32 workers : {1u, 6u}) {
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 1;
    config.pin_threads = false;
    jobs::JobSystem pool(config);
    assets::MeshData parallel = source;
    RepackReport parallel_report;
    REQUIRE(repack_atlas(parallel, "", RepackOptions{}, &pool, parallel_report));
    CHECK(same_mesh(serial, parallel));
    CHECK(parallel_report.charts == report.charts);
  }
}

TEST_CASE("atlas: charted directly, without a proxy, the texture is carried across as well") {
  u32 islands = 0;
  u32 seams_before = 0;
  assets::MeshData mesh = shredded_mesh(islands, seams_before);
  RepackOptions options;
  options.proxy_triangles = 0;  // xatlas cuts the full mesh itself
  RepackReport report;
  std::string error;
  REQUIRE_MESSAGE(repack_atlas(mesh, "", options, nullptr, report, &error), error);
  REQUIRE(report.materials.size() == 1);
  const RepackedMaterial& m = report.materials[0];
  MESSAGE("directly: " << m.charts << " charts, rebake error mean " << report.images[0].error.mean);
  CHECK(m.proxy_triangles == 0);
  CHECK(m.folded_triangles == 0);  // xatlas's own charts never fold
  CHECK(m.charts < islands);
  CHECK(report.images[0].error.mean < 1.0);
  // And the same bytes again, which is the direct path's determinism.
  assets::MeshData again = shredded_mesh(islands, seams_before);
  RepackReport again_report;
  REQUIRE(repack_atlas(again, "", options, nullptr, again_report));
  CHECK(same_mesh(mesh, again));
}

TEST_CASE("atlas: the options key moves with every option that changes the bytes") {
  const RepackOptions base;
  const u64 key = repack_options_key(base);
  CHECK(key == repack_options_key(RepackOptions{}));
  RepackOptions o = base;
  o.padding = 8;
  CHECK(repack_options_key(o) != key);
  o = base;
  o.max_chart_cost_milli = 4000;
  CHECK(repack_options_key(o) != key);
  o = base;
  o.proxy_triangles = 0;
  CHECK(repack_options_key(o) != key);
  o = base;
  o.normal_maps = NormalMapMode::resample;
  CHECK(repack_options_key(o) != key);
  o = base;
  o.max_resolution = 4096;
  CHECK(repack_options_key(o) != key);
  o = base;
  o.dilation = 4;
  CHECK(repack_options_key(o) != key);
  o = base;
  o.supersample = 2;
  CHECK(repack_options_key(o) != key);
  // Only the measurement, not the bytes.
  o = base;
  o.error_samples = 16;
  CHECK(repack_options_key(o) == key);
}

TEST_CASE("atlas: untextured and unreadable materials keep their UVs and images") {
  u32 islands = 0;
  u32 seams = 0;
  assets::MeshData untextured = shredded_mesh(islands, seams);
  untextured.materials[0].base_color_image = -1;
  const assets::MeshData before = untextured;
  RepackReport report;
  REQUIRE(repack_atlas(untextured, "", RepackOptions{}, nullptr, report));
  CHECK(report.materials_repacked == 0);
  CHECK(report.materials_untextured == 1);
  CHECK(same_mesh(untextured, before));

  assets::MeshData unreadable = shredded_mesh(islands, seams);
  unreadable.images[0].bytes.assign(64, u8{0x5a});
  const assets::MeshData unreadable_before = unreadable;
  REQUIRE(repack_atlas(unreadable, "", RepackOptions{}, nullptr, report));
  CHECK(report.materials_repacked == 0);
  CHECK(report.materials_unreadable == 1);
  REQUIRE(report.notes.size() == 1);
  CHECK(report.notes[0].find("could not be read") != std::string::npos);
  CHECK(same_mesh(unreadable, unreadable_before));
}

// A textured 6x6 grid of unit quads (72 triangles, one UV island) with `degenerate` triangles
// appended to its primitive, each naming one grid vertex twice: faces xatlas refuses, which is what
// the gate counts alongside folds.
assets::MeshData grid_with_degenerate(u32 degenerate) {
  assets::MeshData mesh;
  constexpr u32 k_side = 7;
  for (u32 y = 0; y < k_side; ++y) {
    for (u32 x = 0; x < k_side; ++x) {
      mesh.positions.push_back(Vec3{static_cast<f32>(x), static_cast<f32>(y), 0.0f});
      mesh.normals.push_back(Vec3{0.0f, 0.0f, 1.0f});
      mesh.uvs.push_back(
          Vec2{static_cast<f32>(x) / (k_side - 1), static_cast<f32>(y) / (k_side - 1)});
    }
  }
  for (u32 y = 0; y + 1 < k_side; ++y) {
    for (u32 x = 0; x + 1 < k_side; ++x) {
      const u32 a = y * k_side + x;
      for (const u32 i : {a, a + 1, a + k_side + 1, a, a + k_side + 1, a + k_side})
        mesh.indices.push_back(i);
    }
  }
  for (u32 d = 0; d < degenerate; ++d) {
    const u32 a = d % (k_side - 1);
    for (const u32 i : {a, a + 1, a})
      mesh.indices.push_back(i);
  }
  mesh.primitives.push_back(assets::Primitive{0, mesh.indices.size(), 0, -1});
  assets::Material material;
  material.base_color_image = 0;
  mesh.materials.push_back(material);
  Vector<u8> rgba(64 * 64 * 4, u8{120});
  assets::ImageRef image;
  REQUIRE(image::encode_png(64, 64, 4, std::span<const u8>(rgba.data(), rgba.size()), image.bytes));
  mesh.images.push_back(std::move(image));
  return mesh;
}

TEST_CASE("atlas: a material whose new charts would leave over 5% of it unusable keeps its own") {
  // 8 refused faces of 80 is 10%: declined, and the mesh is exactly as it was, with a note.
  assets::MeshData declined = grid_with_degenerate(8);
  const assets::MeshData before = declined;
  RepackReport report;
  std::string error;
  REQUIRE_MESSAGE(repack_atlas(declined, "", RepackOptions{}, nullptr, report, &error), error);
  CHECK(report.materials_repacked == 0);
  CHECK(report.materials_declined == 1);
  REQUIRE(report.notes.size() == 1);
  MESSAGE(report.notes[0]);
  CHECK(report.notes[0].find("keeps its own UVs and images") != std::string::npos);
  CHECK(same_mesh(declined, before));

  // 2 of 74 is under 3%: repacked, and the refused faces borrow their corners' UVs from the chart
  // the same points are in, so they open no island of their own and stretch across nothing.
  assets::MeshData taken = grid_with_degenerate(2);
  REQUIRE_MESSAGE(repack_atlas(taken, "", RepackOptions{}, nullptr, report, &error), error);
  CHECK(report.materials_repacked == 1);
  CHECK(report.materials_declined == 0);
  REQUIRE(report.materials.size() == 1);
  CHECK(report.materials[0].unatlased_triangles == 2);
  for (u32 t = 72; t < 74; ++t) {
    for (u32 k = 0; k < 3; ++k) {
      const u32 v = taken.indices[t * 3 + k];
      bool found = false;  // a grid triangle's vertex at the same point with the same UV
      for (u32 i = 0; i < 72 * 3 && !found; ++i) {
        const u32 w = taken.indices[i];
        const Vec3 p = taken.positions[w];
        const Vec3 q = taken.positions[v];
        found = p.x == q.x && p.y == q.y && p.z == q.z && taken.uvs[w].x == taken.uvs[v].x &&
                taken.uvs[w].y == taken.uvs[v].y;
      }
      CHECK(found);
    }
  }
}

TEST_CASE("atlas: supersampling bakes the atlas and its images at a multiple of the source side") {
  RepackOptions options;
  options.min_resolution = 32;  // the grid's texture is 64 texels a side
  for (const u32 factor : {1u, 2u, 4u}) {
    assets::MeshData mesh = grid_with_degenerate(0);
    options.supersample = factor;
    RepackReport report;
    std::string error;
    REQUIRE_MESSAGE(repack_atlas(mesh, "", options, nullptr, report, &error), error);
    REQUIRE(report.materials.size() == 1);
    REQUIRE(report.images.size() == 1);
    CHECK(report.materials[0].resolution == 64 * factor);
    CHECK(report.images[0].width == 64 * factor);
    CHECK(report.images[0].error.mean < 1.0);  // a flat grey source comes back flat grey
  }
  assets::MeshData mesh = grid_with_degenerate(0);
  const assets::MeshData before = mesh;
  options.supersample = 3;
  RepackReport report;
  std::string error;
  CHECK_FALSE(repack_atlas(mesh, "", options, nullptr, report, &error));
  CHECK_FALSE(error.empty());
  CHECK(same_mesh(mesh, before));
}

TEST_CASE("atlas: skin bindings, morph deltas and vertex ids follow a vertex into every copy") {
  // A 6x6 grid of unit quads with one vertex per position (so a position names its vertex), one
  // UV island, a skin binding and a morph delta that encode the vertex's own index.
  assets::MeshData mesh;
  constexpr u32 k_side = 7;
  geometry::MorphChannelSource channel;
  channel.name = "index";
  for (u32 y = 0; y < k_side; ++y) {
    for (u32 x = 0; x < k_side; ++x) {
      const u32 v = y * k_side + x;
      // A fold down the middle, so xatlas has a reason to cut.
      const f32 z = x > k_side / 2 ? static_cast<f32>(x - k_side / 2) : 0.0f;
      mesh.positions.push_back(Vec3{static_cast<f32>(x), static_cast<f32>(y), z});
      mesh.normals.push_back(Vec3{0.0f, 0.0f, 1.0f});
      mesh.uvs.push_back(
          Vec2{static_cast<f32>(x) / (k_side - 1), static_cast<f32>(y) / (k_side - 1)});
      geometry::SkinBinding skin;
      skin.joints[0] = static_cast<u8>(v);
      mesh.skin_bindings.push_back(skin);
      mesh.vertex_ids.push_back(1000u + v);
      if (v % 3 != 0) {  // a sparse channel: not every vertex has a delta
        channel.vertices.push_back(v);
        channel.position_deltas.push_back(Vec3{static_cast<f32>(v), 0.0f, 0.0f});
        channel.normal_deltas.push_back(Vec3{0.0f, static_cast<f32>(v), 0.0f});
      }
    }
  }
  mesh.morph.push_back(channel);
  for (u32 y = 0; y + 1 < k_side; ++y) {
    for (u32 x = 0; x + 1 < k_side; ++x) {
      const u32 a = y * k_side + x;
      for (const u32 i : {a, a + 1, a + k_side + 1, a, a + k_side + 1, a + k_side})
        mesh.indices.push_back(i);
    }
  }
  mesh.primitives.push_back(assets::Primitive{0, mesh.indices.size(), 0, -1});
  assets::Material material;
  material.base_color_image = 0;
  mesh.materials.push_back(material);
  Vector<u8> rgba(64 * 64 * 4, u8{200});
  assets::ImageRef image;
  REQUIRE(image::encode_png(64, 64, 4, std::span<const u8>(rgba.data(), rgba.size()), image.bytes));
  mesh.images.push_back(std::move(image));
  const u32 before = mesh.positions.size();

  RepackReport report;
  std::string error;
  REQUIRE_MESSAGE(repack_atlas(mesh, "", RepackOptions{}, nullptr, report, &error), error);
  REQUIRE(mesh.positions.size() > before);
  REQUIRE(mesh.skin_bindings.size() == mesh.positions.size());
  REQUIRE(mesh.vertex_ids.size() == mesh.positions.size());
  const geometry::MorphChannelSource& out = mesh.morph[0];
  REQUIRE(out.vertices.size() == out.position_deltas.size());
  REQUIRE(out.vertices.size() == out.normal_deltas.size());
  CHECK(std::is_sorted(out.vertices.begin(), out.vertices.end()));
  u32 copies_with_delta = 0;
  for (u32 k = before; k < mesh.positions.size(); ++k) {
    const Vec3 p = mesh.positions[k];
    const u32 v = static_cast<u32>(p.y) * k_side + static_cast<u32>(p.x);
    CHECK(mesh.skin_bindings[k].joints[0] == v);
    CHECK(mesh.vertex_ids[k] == 1000u + v);  // a copy is the same point of the base
    const auto it = std::lower_bound(out.vertices.begin(), out.vertices.end(), k);
    const bool has = it != out.vertices.end() && *it == k;
    CHECK(has == (v % 3 != 0));
    if (has) {
      const u32 e = static_cast<u32>(it - out.vertices.begin());
      CHECK(out.position_deltas[e].x == static_cast<f32>(v));
      CHECK(out.normal_deltas[e].y == static_cast<f32>(v));
      ++copies_with_delta;
    }
  }
  CHECK(copies_with_delta > 0);
}

TEST_CASE("atlas: a malformed mesh is refused and left as it was") {
  u32 islands = 0;
  u32 seams = 0;
  assets::MeshData mesh = shredded_mesh(islands, seams);
  mesh.indices.push_back(mesh.positions.size() + 5);
  mesh.indices.push_back(0);
  mesh.indices.push_back(1);
  const assets::MeshData before = mesh;
  RepackReport report;
  std::string error;
  CHECK_FALSE(repack_atlas(mesh, "", RepackOptions{}, nullptr, report, &error));
  CHECK_FALSE(error.empty());
  CHECK(same_mesh(mesh, before));
}
