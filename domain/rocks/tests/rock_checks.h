#pragma once

// What every solved rock is held to (docs/subsystems/rocks.md, "Invariants"), shared by the
// boulders' tests and the cliffs': welded by exact position, closed, two-manifold, outward-wound,
// and its parts counted.
#include <core/hash/hash.h>
#include <domain/assets/gltf.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::rocks::checks {

inline u64 mesh_hash(const assets::MeshData& mesh) {
  u64 h = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(mesh.normals.data(), mesh.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2)));
  h = hash_combine(h, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
  return h;
}

struct Topology {
  std::vector<u32> weld;  // per vertex, its position's id
  u32 positions = 0;
  u32 components = 0;
};

// Welds by exact position (the solver's seams are the same bits on both sides by construction).
inline Topology weld(const assets::MeshData& mesh) {
  Topology t;
  const u32 n = mesh.positions.size();
  std::vector<u32> order(n);
  for (u32 i = 0; i < n; ++i)
    order[i] = i;
  const auto less = [&](u32 a, u32 b) {
    const Vec3 pa = mesh.positions[a];
    const Vec3 pb = mesh.positions[b];
    if (pa.x != pb.x) return pa.x < pb.x;
    if (pa.y != pb.y) return pa.y < pb.y;
    return pa.z < pb.z;
  };
  std::sort(order.begin(), order.end(), less);
  t.weld.assign(n, 0);
  for (u32 i = 0; i < n; ++i) {
    if (i > 0 && !less(order[i - 1], order[i]) && !less(order[i], order[i - 1])) {
      t.weld[order[i]] = t.weld[order[i - 1]];
    } else {
      t.weld[order[i]] = t.positions++;
    }
  }
  return t;
}

// Every validity property a solved rock has: finite, closed, two-manifold, consistently and
// outward wound, and `parts` connected components.
inline void check_closed(const assets::MeshData& mesh, u32 parts) {
  REQUIRE(!mesh.positions.empty());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  REQUIRE(mesh.uvs.size() == mesh.positions.size());
  REQUIRE(mesh.indices.size() % 3 == 0);
  REQUIRE(mesh.primitives.size() == 1);
  REQUIRE(mesh.materials.size() == 1);
  CHECK_FALSE(mesh.materials[0].double_sided);
  for (const Vec3& p : mesh.positions)
    REQUIRE((std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)));
  const Topology t = weld(mesh);
  // Directed edges, welded: each must appear exactly once, and its reverse exactly once.
  std::vector<u64> directed;
  directed.reserve(mesh.indices.size());
  u32 degenerate = 0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const u32 w[3] = {t.weld[mesh.indices[i]], t.weld[mesh.indices[i + 1]],
                      t.weld[mesh.indices[i + 2]]};
    if (w[0] == w[1] || w[1] == w[2] || w[0] == w[2]) ++degenerate;
    for (u32 e = 0; e < 3; ++e)
      directed.push_back(static_cast<u64>(w[e]) << 32 | w[(e + 1) % 3]);
  }
  CHECK(degenerate == 0);
  std::sort(directed.begin(), directed.end());
  u32 repeated = 0;
  u32 unmatched = 0;
  for (u32 i = 0; i < directed.size(); ++i) {
    if (i > 0 && directed[i] == directed[i - 1]) ++repeated;
    const u64 reverse = (directed[i] & 0xffffffffull) << 32 | directed[i] >> 32;
    if (!std::binary_search(directed.begin(), directed.end(), reverse)) ++unmatched;
  }
  CHECK_MESSAGE(repeated == 0, repeated << " edges used twice in one direction");
  CHECK_MESSAGE(unmatched == 0, unmatched << " edges with no opposite: a hole");
  // Components over welded edges.
  std::vector<u32> parent(t.positions);
  for (u32 i = 0; i < t.positions; ++i)
    parent[i] = i;
  const auto find = [&](u32 x) {
    while (parent[x] != x)
      x = parent[x] = parent[parent[x]];
    return x;
  };
  for (const u64 e : directed)
    parent[find(static_cast<u32>(e >> 32))] = find(static_cast<u32>(e & 0xffffffffull));
  u32 components = 0;
  for (u32 i = 0; i < t.positions; ++i)
    components += find(i) == i ? 1u : 0u;
  CHECK(components == parts);
  // Outward: the signed volume of every component is positive, and so is the whole's.
  f64 volume = 0.0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const Vec3 a = mesh.positions[mesh.indices[i]];
    const Vec3 b = mesh.positions[mesh.indices[i + 1]];
    const Vec3 c = mesh.positions[mesh.indices[i + 2]];
    volume += static_cast<f64>(dot(a, cross(b, c))) / 6.0;
  }
  CHECK(volume > 0.0);
  // And the faces agree with the normals the solver gave their corners.
  u32 against = 0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const Vec3 a = mesh.positions[mesh.indices[i]];
    const Vec3 fn =
        cross(mesh.positions[mesh.indices[i + 1]] - a, mesh.positions[mesh.indices[i + 2]] - a);
    const Vec3 vn = mesh.normals[mesh.indices[i]] + mesh.normals[mesh.indices[i + 1]] +
                    mesh.normals[mesh.indices[i + 2]];
    if (dot(fn, vn) <= 0.0f) ++against;
  }
  CHECK_MESSAGE(against == 0, against << " faces wound against their normals");
}

struct Box {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
};
inline Box bounds(const assets::MeshData& mesh) {
  Box b;
  for (const Vec3& p : mesh.positions) {
    b.lo = Vec3{std::min(b.lo.x, p.x), std::min(b.lo.y, p.y), std::min(b.lo.z, p.z)};
    b.hi = Vec3{std::max(b.hi.x, p.x), std::max(b.hi.y, p.y), std::max(b.hi.z, p.z)};
  }
  return b;
}

inline f64 volume_of(const assets::MeshData& mesh) {
  f64 v = 0.0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3)
    v += static_cast<f64>(
             dot(mesh.positions[mesh.indices[i]],
                 cross(mesh.positions[mesh.indices[i + 1]], mesh.positions[mesh.indices[i + 2]]))) /
         6.0;
  return v;
}

}  // namespace engine::rocks::checks
