// A city block of authored meshes for the collision (W200; docs/subsystems/scene_collision.md,
// "A scene of authored meshes"): facades, each a wall with a grid of openings whose reveals are 0.1
// to 0.3 m deep and a sill under each, so the 5 cm cut keeps nearly every triangle, as an imported
// building's facade does. Shared by the case that walks onto it (authored_mesh_tests.cpp) and the
// bench row that builds its proxies (scene_collision_bench.cpp), so both measure the same shape.
// No device, no file: the meshes are made here and their DAGs by the geometry module.
#pragma once

#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>
#include <systems/renderer/scene.h>

#include <cmath>
#include <span>
#include <string>

namespace engine::facade_fixture {

// One facade's shape: `cols` x `rows` openings at `pitch` metres, each `opening` wide and tall,
// recessed `reveal` metres behind the wall's face, with a sill a box under it standing out 0.12 m.
// The wall stands in the x-y plane, its face toward +z, its foot at y = 0, centred in x.
struct Facade {
  u32 cols = 30;
  u32 rows = 55;
  f32 pitch = 0.35f;
  f32 opening = 0.2f;
  f32 margin = 0.3f;
  f32 reveal = 0.2f;
};

// The k-th facade of a block: each its own counts and depth, so no two are one mesh. About 50,000
// triangles each (30 x 55 openings), or with `small` about 2,000 (6 x 10), for a case that runs
// in a debug build.
inline Facade facade_for(u32 k, bool small = false) {
  Facade f;
  f.cols = (small ? 4 : 28) + k % 5;
  f.rows = (small ? 8 : 53) + (k / 5) % 5;
  f.reveal = 0.1f + 0.2f * static_cast<f32>(k % 7) / 6.0f;
  return f;
}

struct MeshSource {
  Vector<Vec3> positions;
  Vector<u32> indices;
};

namespace detail {

inline Vec3 cross3(Vec3 a, Vec3 b) {
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Two triangles over a quad's corners a, b, c, d (in order round it), wound to face `n` as the
// tree's meshes are (the collision tests' boxes): (b - a) x (c - a) points away from `n`, which is
// the side a walker is stopped on.
inline void quad(MeshSource& m, u32 a, u32 b, u32 c, u32 d, Vec3 n) {
  const Vec3 pa = m.positions[a];
  const Vec3 e1 = m.positions[b] - pa;
  const Vec3 e2 = m.positions[c] - pa;
  const Vec3 x = cross3(e1, e2);
  const bool flip = x.x * n.x + x.y * n.y + x.z * n.z > 0.0f;
  const u32 tri[6] = {a, b, c, a, c, d};
  for (u32 k = 0; k < 6; ++k)
    m.indices.push_back(tri[flip ? (k % 3 == 1 ? k + 1 : (k % 3 == 2 ? k - 1 : k)) : k]);
}

inline u32 vertex(MeshSource& m, Vec3 p) {
  m.positions.push_back(p);
  return m.positions.size() - 1;
}

// A face of its own: four new vertices and the quad over them. An imported mesh's vertices are
// split at every crease, because the import's weld keys on the normal (geometry's
// `weld_vertices`): a reveal, a pane and each face of a sill are separate vertices from the wall's
// face beside them, as an exporter writing a normal a face leaves them.
inline void face(MeshSource& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n) {
  const u32 va = vertex(m, a);
  const u32 vb = vertex(m, b);
  const u32 vc = vertex(m, c);
  const u32 vd = vertex(m, d);
  quad(m, va, vb, vc, vd, n);
}

// A closed box from lo to hi, faces out, each face its own four vertices.
inline void box(MeshSource& m, Vec3 lo, Vec3 hi) {
  Vec3 v[8];
  for (u32 k = 0; k < 8; ++k)
    v[k] = Vec3{(k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z};
  face(m, v[0], v[2], v[6], v[4], Vec3{-1, 0, 0});
  face(m, v[1], v[5], v[7], v[3], Vec3{1, 0, 0});
  face(m, v[0], v[4], v[5], v[1], Vec3{0, -1, 0});
  face(m, v[2], v[3], v[7], v[6], Vec3{0, 1, 0});
  face(m, v[0], v[1], v[3], v[2], Vec3{0, 0, -1});
  face(m, v[4], v[6], v[7], v[5], Vec3{0, 0, 1});
}

}  // namespace detail

// The facade's triangles: the wall's face as a grid of cells between the openings' edges, welded
// (one plane, one normal), and each opening's four reveals, its pane and its sill's six faces, each
// face its own four vertices (`detail::face`).
inline MeshSource facade_mesh(const Facade& f) {
  using detail::quad;
  using detail::vertex;
  MeshSource m;
  const auto lines = [&](u32 n) {
    Vector<f32> at;
    at.push_back(0.0f);
    for (u32 c = 0; c < n; ++c) {
      at.push_back(f.margin + static_cast<f32>(c) * f.pitch);
      at.push_back(f.margin + static_cast<f32>(c) * f.pitch + f.opening);
    }
    at.push_back(2.0f * f.margin + static_cast<f32>(n) * f.pitch - (f.pitch - f.opening));
    return at;
  };
  const Vector<f32> xs = lines(f.cols);
  Vector<f32> ys = lines(f.rows);
  const f32 half = 0.5f * xs[xs.size() - 1];
  const u32 nx = xs.size();
  const u32 ny = ys.size();
  for (u32 j = 0; j < ny; ++j)
    for (u32 i = 0; i < nx; ++i)
      (void)vertex(m, Vec3{xs[i] - half, ys[j], 0.0f});
  const auto at = [nx](u32 i, u32 j) { return j * nx + i; };
  const Vec3 front{0.0f, 0.0f, 1.0f};
  for (u32 j = 0; j + 1 < ny; ++j) {
    for (u32 i = 0; i + 1 < nx; ++i) {
      const bool hole = (i & 1u) == 1u && (j & 1u) == 1u;
      if (!hole) {
        quad(m, at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1), front);
        continue;
      }
      using detail::face;
      const f32 x0 = xs[i] - half, x1 = xs[i + 1] - half, y0 = ys[j], y1 = ys[j + 1];
      const f32 z = -f.reveal;
      const Vec3 f0{x0, y0, 0.0f}, f1{x1, y0, 0.0f}, f2{x1, y1, 0.0f}, f3{x0, y1, 0.0f};
      const Vec3 b0{x0, y0, z}, b1{x1, y0, z}, b2{x1, y1, z}, b3{x0, y1, z};
      face(m, f0, f1, b1, b0, Vec3{0, 1, 0});   // the bottom reveal
      face(m, f1, f2, b2, b1, Vec3{-1, 0, 0});  // the right
      face(m, f2, f3, b3, b2, Vec3{0, -1, 0});  // the top
      face(m, f3, f0, b0, b3, Vec3{1, 0, 0});   // the left
      face(m, b0, b1, b2, b3, front);           // the pane
      detail::box(m, Vec3{x0 - 0.04f, y0 - 0.05f, 0.0f}, Vec3{x1 + 0.04f, y0, 0.12f});  // the sill
    }
  }
  return m;
}

// `meshes` facades, each placed `scales` times at a scale of its own (so every placement is its own
// proxy: `meshes * scales` of them), in a grid of 8 a row at 12 m in x and rows 12 m apart in z
// from (6, 6), facing +z: the block's 64 at the default fill the 3 x 3 tiles of 32 m round
// (48, 48), whose street (z = 48) runs between the fourth row and the fifth.
inline bool facade_scene(u32 meshes, u32 scales, bool small, renderer::SceneData& scene,
                         std::string& error) {
  Vector<geometry::ClusterLodMesh> lods;
  for (u32 k = 0; k < meshes; ++k) {
    const MeshSource m = facade_mesh(facade_for(k, small));
    geometry::ClusterLodMesh lod;
    if (!geometry::build_cluster_lod(std::span<const Vec3>(m.positions.data(), m.positions.size()),
                                     std::span<const u32>(m.indices.data(), m.indices.size()),
                                     geometry::ClusterLodOptions{}, lod, &error))
      return false;
    lods.push_back(std::move(lod));
  }
  if (!geometry::merge_cluster_meshes(
          std::span<const geometry::ClusterLodMesh>(lods.data(), lods.size()), scene.lod,
          scene.parts, &error))
    return false;
  scene.mesh_fit.resize(scene.parts.size(), Mat4::identity());
  scene.terrain_mesh = ~0u;
  u32 first = 0;
  for (u32 n = 0; n < meshes * scales; ++n) {
    renderer::SceneInstance i;
    i.mesh = n % meshes;
    const f32 s = 0.9f + 0.2f * static_cast<f32>(n / meshes) / static_cast<f32>(scales);
    i.transform.scale = Vec3{s, s, s};
    i.transform.position =
        Vec3{6.0f + 12.0f * static_cast<f32>(n % 8), 0.0f, 6.0f + 12.0f * static_cast<f32>(n / 8)};
    gfx::InstanceDesc desc;
    if (!renderer::make_instance(scene, i, first, desc, &error)) return false;
    first += scene.parts[i.mesh].cluster_count;
    scene.instances.push_back(desc);
  }
  scene.pair_count = first;
  return true;
}

}  // namespace engine::facade_fixture
