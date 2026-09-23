// The weld both builders of a cluster container run (`assets::weld_vertices`): every per-vertex
// stream an imported mesh owns goes through it, so each cluster vertex of the built mesh carries
// the morph deltas and the skin binding of the source vertices it stands for. The rule itself —
// what may merge — is domain/geometry's and tested there (tests/morph_tests.cpp); this file checks
// that the one function both builders call hands the weld everything, which is exactly what went
// wrong: until cluster cache version 11 neither builder passed the morph channels, and on any mesh
// whose weld renumbered a vertex the deltas landed on the wrong vertices or were dropped
// (docs/subsystems/geometry.md, "Morph channels").

#include <domain/assets/gltf.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/uv_repair.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::assets;

namespace {

// Where the weld sent each source vertex, read off the index buffer: the weld rewrites every
// index in place, so slot i named source vertex `before[i]` and now names `after[i]`. A vertex no
// index names was dropped and keeps ~0u.
Vector<u32> welded_of(const Vector<u32>& before, const Vector<u32>& after, u32 source_vertices) {
  Vector<u32> out(source_vertices, ~0u);
  for (u32 i = 0; i < before.size(); ++i)
    out[before[i]] = after[i];
  return out;
}

// A channel's delta at one source vertex; absent is zero, which is what a stored stream means by
// not listing a vertex.
bool delta_at(const geometry::MorphChannelSource& channel, u32 vertex, Vec3& position,
              Vec3& normal) {
  position = Vec3{};
  normal = Vec3{};
  const auto at = std::lower_bound(channel.vertices.begin(), channel.vertices.end(), vertex);
  if (at == channel.vertices.end() || *at != vertex) return false;
  const u32 i = static_cast<u32>(at - channel.vertices.begin());
  position = channel.position_deltas[i];
  if (channel.normal_deltas.size() == channel.vertices.size()) normal = channel.normal_deltas[i];
  return true;
}

// The source channel's own quantization step, largest component over 32767, as the stream sets
// it (geometry.md, "The quantization").
f32 step_of(const Vector<Vec3>& deltas) {
  f32 largest = 0.0f;
  for (const Vec3& d : deltas)
    largest = std::max({largest, std::fabs(d.x), std::fabs(d.y), std::fabs(d.z)});
  return largest / 32767.0f;
}

// Every (cluster vertex, source vertex it stands for, channel) of a set of per-primitive DAGs,
// compared with what the source said about that source vertex. The source is the mesh **before**
// the weld, so the comparison is "what the built mesh carries" against "what the file said", with
// nothing of the weld's own bookkeeping in between.
struct Placement {
  u32 copies = 0;         // (cluster vertex, source vertex) pairs looked at
  u32 deltas = 0;         // of those times channels, the ones where the source has a delta
  u32 wrong = 0;          // stored delta further from the source's than the stream's precision
  u32 lost = 0;           // of `wrong`: a source delta with nothing stored at all
  u32 wrong_sources = 0;  // source vertices at least one of whose copies is wrong
  u32 bindings_wrong = 0;
  f32 worst = 0.0f;    // largest |stored - source| position delta
  f32 largest = 0.0f;  // largest source position delta, to read `worst` against
};

Placement check_placement(const MeshData& source, const MeshData& welded,
                          std::span<const geometry::ClusterLodMesh> parts) {
  Placement out;
  const u32 source_vertices = source.positions.size();
  const Vector<u32> map = welded_of(source.indices, welded.indices, source_vertices);
  // welded vertex -> the source vertices it stands for, as a CSR.
  const u32 welded_vertices = welded.positions.size();
  Vector<u32> first(welded_vertices + 1, 0u);
  for (u32 v = 0; v < source_vertices; ++v) {
    if (map[v] != ~0u) ++first[map[v] + 1];
  }
  for (u32 w = 0; w < welded_vertices; ++w)
    first[w + 1] += first[w];
  Vector<u32> sources(first[welded_vertices]);
  {
    Vector<u32> cursor = first;
    for (u32 v = 0; v < source_vertices; ++v) {
      if (map[v] != ~0u) sources[cursor[map[v]]++] = v;
    }
  }
  // A merged pair agreed on its deltas quantized at the source channel's step, and the stored
  // step can only be finer (the weld drops vertices, never adds them), so a copy is within one
  // and a half source steps per axis of every source vertex it stands for.
  Vector<f32> position_tolerance;
  Vector<f32> normal_tolerance;
  for (const geometry::MorphChannelSource& channel : source.morph) {
    position_tolerance.push_back(1.5f * step_of(channel.position_deltas) * 1.7321f + 1.0e-9f);
    normal_tolerance.push_back(1.5f * step_of(channel.normal_deltas) * 1.7321f + 1.0e-9f);
    for (const Vec3& d : channel.position_deltas)
      out.largest = std::max(out.largest, length(d));
  }
  Vector<u8> source_wrong(source_vertices, u8{0});
  for (const geometry::ClusterLodMesh& part : parts) {
    const geometry::ClusterMesh& mesh = part.mesh;
    for (u32 c = 0; c < mesh.clusters.size(); ++c) {
      const geometry::ClusterDesc& cluster = mesh.clusters[c];
      for (u32 local = 0; local < cluster.vertex_count; ++local) {
        const u32 vertex = cluster.vertex_offset + local;
        const u32 w = mesh.vertex_source[vertex];
        for (u32 s = first[w]; s < first[w + 1]; ++s) {
          const u32 o = sources[s];
          ++out.copies;
          bool wrong = false;
          if (!source.skin_bindings.empty()) {
            if (mesh.skin.size() != mesh.vertices.size() ||
                std::memcmp(&mesh.skin[vertex], &source.skin_bindings[o],
                            sizeof(geometry::SkinBinding)) != 0) {
              ++out.bindings_wrong;
              wrong = true;
            }
          }
          for (u32 k = 0; k < source.morph.size(); ++k) {
            Vec3 want_p;
            Vec3 want_n;
            const bool present = delta_at(source.morph[k], o, want_p, want_n);
            Vec3 got_p;
            Vec3 got_n;
            const bool stored = geometry::morph_delta_at(mesh, c, k, local, got_p, got_n);
            if (present) ++out.deltas;
            const f32 error = length(got_p - want_p);
            out.worst = std::max(out.worst, error);
            if (error > position_tolerance[k] || length(got_n - want_n) > normal_tolerance[k]) {
              ++out.wrong;
              if (present && !stored) ++out.lost;
              wrong = true;
            }
          }
          if (wrong && source_wrong[o] == 0u) {
            source_wrong[o] = 1u;
            ++out.wrong_sources;
          }
        }
      }
    }
  }
  return out;
}

// One DAG per primitive over the welded mesh, exactly as both builders build it.
bool build_parts(const MeshData& welded, Vector<geometry::ClusterLodMesh>& parts,
                 std::string& error) {
  const geometry::AttributeSource attributes = attribute_source(welded);
  for (const Primitive& primitive : welded.primitives) {
    if (primitive.index_count < 3) continue;
    const std::span<const u32> range(welded.indices.data() + primitive.first_index,
                                     primitive.index_count);
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(welded.positions, range, geometry::ClusterLodOptions{}, part,
                                     &error, attributes)) {
      return false;
    }
    parts.push_back(std::move(part));
  }
  return true;
}

// A strip of four unit quads along x (x = 0..4, y = 0..1, facing +z), written as a triangle soup
// the way an unindexing exporter writes one, in two primitives of two quads each, after one vertex
// nothing references. Every copy of a grid point agrees on position, normal, UV and skin binding;
// the two channels decide what the weld may do with them:
//
//   lift  moves the x = 1 and x = 2 columns up, identically at every copy, with normal deltas.
//         The copies agree on everything, so the weld must merge them.
//   tear  pulls the x = 2 column apart, -x on primitive 0's copies and +x on primitive 1's, with
//         no normal deltas. The copies disagree *only* here, so the weld must keep them apart:
//         a morph seam, which merged would open a hole the moment the channel plays.
//
// The unreferenced vertex carries a `lift` delta of its own, so dropping it moves every index
// after it: a weld that is not handed the channels leaves no delta on its own vertex.
MeshData soup_strip() {
  MeshData m;
  auto add = [&](Vec3 p) {
    m.positions.push_back(p);
    m.normals.push_back(Vec3{0.0f, 0.0f, 1.0f});
    m.uvs.push_back(Vec2{p.x * 0.25f, p.y});
    const u32 w = static_cast<u32>(std::lround(p.x * 60.0f));  // 0..240 of 255 on joint 1
    geometry::SkinBinding binding;
    binding.joints[0] = 0;
    binding.joints[1] = 1;
    binding.weights[0] = static_cast<u8>(255u - std::min(w, 255u));
    binding.weights[1] = static_cast<u8>(std::min(w, 255u));
    m.skin_bindings.push_back(binding);
  };
  add(Vec3{9.0f, 9.0f, 0.0f});  // referenced by nothing
  for (u32 p = 0; p < 2; ++p) {
    const u32 first = m.indices.size();
    for (u32 q = 2 * p; q < 2 * p + 2; ++q) {
      const f32 x = static_cast<f32>(q);
      const Vec3 corners[6] = {Vec3{x, 0, 0}, Vec3{x + 1.0f, 0, 0}, Vec3{x + 1.0f, 1, 0},
                               Vec3{x, 0, 0}, Vec3{x + 1.0f, 1, 0}, Vec3{x, 1, 0}};
      for (const Vec3& c : corners) {
        m.indices.push_back(m.positions.size());
        add(c);
      }
    }
    m.primitives.push_back(Primitive{first, m.indices.size() - first, static_cast<i32>(p), 0});
  }
  m.materials.resize(2);
  Skin skin;
  for (i32 joint = 0; joint < 2; ++joint) {
    skin.joints.push_back(joint);
    skin.inverse_bind.push_back(Mat4::identity());
  }
  m.skins.push_back(skin);

  geometry::MorphChannelSource lift;
  lift.name = "lift";
  geometry::MorphChannelSource tear;
  tear.name = "tear";
  // Primitive 0's corners are vertices 1..12 and primitive 1's 13..24.
  for (u32 v = 0; v < m.positions.size(); ++v) {
    const f32 x = m.positions[v].x;
    if (v == 0 || x == 1.0f || x == 2.0f) {
      lift.vertices.push_back(v);
      lift.position_deltas.push_back(Vec3{0.0f, 0.0f, 0.5f});
      lift.normal_deltas.push_back(Vec3{0.2f, 0.0f, -0.02f});
    }
    if (v != 0 && x == 2.0f) {
      tear.vertices.push_back(v);
      tear.position_deltas.push_back(Vec3{v <= 12 ? -0.3f : 0.3f, 0.0f, 0.0f});
    }
  }
  m.morph.push_back(std::move(lift));
  m.morph.push_back(std::move(tear));
  return m;
}

}  // namespace

TEST_CASE("assets weld: every morph delta and skin binding stays on the vertex it belongs to") {
  const MeshData source = soup_strip();
  REQUIRE(source.positions.size() == 25);
  MeshData welded = source;
  const u32 vertices = weld_vertices(welded);

  // The property first, so a failure says how far off it is before anything structural stops it.
  Vector<geometry::ClusterLodMesh> parts;
  std::string error;
  REQUIRE_MESSAGE(build_parts(welded, parts, error), error);
  const Placement placement = check_placement(
      source, welded, std::span<const geometry::ClusterLodMesh>(parts.data(), parts.size()));
  MESSAGE("soup strip: " << source.positions.size() << " source vertices welded to " << vertices
                         << "; " << placement.copies << " cluster copies, " << placement.deltas
                         << " source deltas over them, " << placement.wrong << " wrong ("
                         << placement.lost << " lost) on " << placement.wrong_sources
                         << " source vertices, worst " << placement.worst << " against a largest "
                         << placement.largest);
  CHECK(placement.copies == 24);  // every corner of the soup, each through its own cluster vertex
  CHECK(placement.deltas == 18);  // 12 corners on `lift` (x = 1 and 2) and 6 on `tear` (x = 2)
  CHECK(placement.wrong == 0);
  CHECK(placement.bindings_wrong == 0);

  // Ten grid points; the x = 2 column's two points are two vertices each (primitive 0's copies
  // and primitive 1's disagree about `tear`), everything else one; the unreferenced vertex is gone.
  CHECK(vertices == 12);
  u32 at_seam = 0;
  for (const Vec3& p : welded.positions)
    at_seam += p.x == 2.0f ? 1u : 0u;
  CHECK(at_seam == 4);

  // The channels were renumbered with the vertices: `lift` names the one vertex at each x = 1
  // point and both at each x = 2 point, `tear` the four seam vertices, and every one of them is a
  // vertex of the welded mesh at the right place.
  REQUIRE(welded.morph.size() == 2);
  CHECK(welded.morph[0].vertices.size() == 6);
  CHECK(welded.morph[1].vertices.size() == 4);
  for (const geometry::MorphChannelSource& channel : welded.morph) {
    for (const u32 v : channel.vertices) {
      CHECK(v < welded.positions.size());
      if (v < welded.positions.size())
        CHECK((welded.positions[v].x == 1.0f || welded.positions[v].x == 2.0f));
    }
  }
}

// The same property on the four Khronos files tools/fetch-samples.ps1 fetches, through the same
// steps both builders take — the load, the UV repair, the weld, one DAG per primitive — when they
// are present (CI does not fetch them; a test bundle made with -WithSamples carries them). Before
// the builders handed the weld the channels, MorphStressTest put its deltas on the wrong vertices
// (docs/subsystems/geometry.md, "Morph channels", has the numbers).
TEST_CASE("assets weld: the Khronos morph samples keep every delta on its own vertex") {
  const char* k_samples[] = {"AnimatedMorphCube/AnimatedMorphCube.glb",
                             "MorphPrimitivesTest/MorphPrimitivesTest.glb",
                             "MorphStressTest/MorphStressTest.glb", "SimpleMorph/SimpleMorph.gltf"};
  for (const char* relative : k_samples) {
    const std::string path =
        test::data_path(std::string(ENGINE_SOURCE_DIR "/content/samples/") + relative,
                        std::string("content/samples/") + relative);
    if (!test::path_exists(path)) {
      MESSAGE("not fetched (tools/fetch-samples.ps1): " << path);
      continue;
    }
    MeshData source;
    std::string error;
    REQUIRE_MESSAGE(load_gltf(path, source, &error), error);
    REQUIRE_MESSAGE(!source.morph.empty(), relative);
    geometry::UvRepairReport repair;
    REQUIRE_MESSAGE(repair_uv_degenerate_triangles(source, repair, &error), error);
    MeshData welded = source;
    const u32 vertices = weld_vertices(welded);
    Vector<geometry::ClusterLodMesh> parts;
    REQUIRE_MESSAGE(build_parts(welded, parts, error), error);
    const Placement placement = check_placement(
        source, welded, std::span<const geometry::ClusterLodMesh>(parts.data(), parts.size()));
    MESSAGE(std::string(relative) << ": " << source.positions.size()
                                  << " source vertices welded to " << vertices << "; "
                                  << placement.copies << " cluster copies, " << placement.deltas
                                  << " source deltas over them, " << placement.wrong << " wrong ("
                                  << placement.lost << " lost) on " << placement.wrong_sources
                                  << " source vertices, worst " << placement.worst
                                  << " against a largest " << placement.largest);
    CHECK(placement.deltas > 0);
    CHECK(placement.wrong == 0);
  }
}
