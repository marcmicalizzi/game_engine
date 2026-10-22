// The date palm's solver (docs/subsystems/flora.md): shape, ranges, budget and determinism.
#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/palm.h>
#include <domain/flora/trees.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>

using namespace engine;

namespace {

flora::Palm default_palm() { return flora::Palm{}; }

assets::MeshData solve(const flora::Palm& p, flora::PalmReport* report = nullptr) {
  assets::MeshData mesh;
  std::string error;
  const bool ok = flora::solve_palm(p, mesh, report, &error);
  REQUIRE_MESSAGE(ok, error);
  return mesh;
}

// The primitive a material index draws, or null.
const assets::Primitive* primitive_of(const assets::MeshData& mesh, i32 material) {
  for (const assets::Primitive& p : mesh.primitives)
    if (p.material == material) return &p;
  return nullptr;
}

f64 triangle_area(const assets::MeshData& mesh, u32 t) {
  const Vec3 a = mesh.positions[mesh.indices[t * 3 + 0]];
  const Vec3 b = mesh.positions[mesh.indices[t * 3 + 1]];
  const Vec3 c = mesh.positions[mesh.indices[t * 3 + 2]];
  const Vec3 n = cross(b - a, c - a);
  return 0.5 * std::sqrt(static_cast<f64>(dot(n, n)));
}

// Every stream the solver fills, hashed in order: the mesh's bytes, whatever the platform.
u64 mesh_hash(const assets::MeshData& mesh) {
  u64 h = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(mesh.normals.data(), mesh.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2)));
  h = hash_combine(h, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
  for (const assets::Primitive& p : mesh.primitives) {
    h = hash_combine(h, p.first_index);
    h = hash_combine(h, p.index_count);
    h = hash_combine(h, static_cast<u64>(static_cast<u32>(p.material)));
  }
  return h;
}

// Every validity property a solved palm has, whatever its parameters.
void check_mesh(u32 budget, const assets::MeshData& mesh) {
  REQUIRE(!mesh.positions.empty());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  REQUIRE(mesh.uvs.size() == mesh.positions.size());
  REQUIRE(mesh.indices.size() % 3 == 0);
  CHECK(mesh.indices.size() / 3 <= budget);
  u32 covered = 0;
  for (const assets::Primitive& prim : mesh.primitives) {
    CHECK(prim.first_index == covered);
    covered += prim.index_count;
    REQUIRE(prim.material >= 0);
    REQUIRE(static_cast<u32>(prim.material) < mesh.materials.size());
  }
  CHECK(covered == mesh.indices.size());
  for (const Vec3& v : mesh.positions) {
    REQUIRE(std::isfinite(v.x));
    REQUIRE(std::isfinite(v.y));
    REQUIRE(std::isfinite(v.z));
  }
  u32 degenerate = 0;
  f64 smallest = 1e30;
  for (u32 t = 0; t < mesh.indices.size() / 3; ++t) {
    const u32 a = mesh.indices[t * 3];
    const u32 b = mesh.indices[t * 3 + 1];
    const u32 c = mesh.indices[t * 3 + 2];
    REQUIRE(a < mesh.positions.size());
    REQUIRE(b < mesh.positions.size());
    REQUIRE(c < mesh.positions.size());
    const f64 area = triangle_area(mesh, t);
    smallest = std::min(smallest, area);
    if (a == b || b == c || a == c || !(area > 1e-10)) ++degenerate;
  }
  CHECK_MESSAGE(degenerate == 0, degenerate << " degenerate triangles; smallest area " << smallest);
}

void check_valid(const flora::Palm& p, const assets::MeshData& mesh) {
  check_mesh(p.triangle_budget, mesh);
}

// The motion contract a solver emitted, against its mesh (flora.md, "Motion attributes"): one
// record per vertex, ids dense with the trunk 0 and parents first, and every part's weights an
// arc-length parameter of it — 0 at the root, 1 at the tip, and the fraction of the way along at
// every station between. That last is checked without knowing how the solver orders its vertices:
// the vertices of a part are binned by weight, and the polyline through the bins' centroids, from
// the pivot, must reach each bin at the share of its length the bin's weight says.
void check_motion(const assets::MeshData& mesh, const flora::Motion& motion, u32 expected_parts,
                  bool along = true) {
  REQUIRE(motion.vertex_part.size() == mesh.positions.size());
  REQUIRE(motion.vertex_weight.size() == mesh.positions.size());
  REQUIRE(motion.parts.size() == expected_parts);
  CHECK(motion.parts[0].parent == geometry::k_motion_no_parent);
  for (u32 p = 1; p < motion.parts.size(); ++p) {
    CAPTURE(p);
    CHECK(motion.parts[p].parent < p);
    CHECK(motion.parts[p].length > 0.0f);
    const Vec3 d = motion.parts[p].direction;
    CHECK(std::abs(dot(d, d) - 1.0f) < 1e-4f);
  }
  constexpr u32 k_bins = 12;
  struct Bin {
    f64 sum[3] = {0.0, 0.0, 0.0};
    f64 weight = 0.0;
    u32 count = 0;
  };
  Vector<Bin> bins(motion.parts.size() * k_bins);
  Vector<f32> lo(motion.parts.size(), 2.0f);
  Vector<f32> hi(motion.parts.size(), -1.0f);
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    const u32 part = motion.vertex_part[v];
    REQUIRE(part < motion.parts.size());
    const f32 w = motion.vertex_weight[v];
    REQUIRE(w >= 0.0f);
    REQUIRE(w <= 1.0f);
    lo[part] = std::min(lo[part], w);
    hi[part] = std::max(hi[part], w);
    u32 b = static_cast<u32>(w * static_cast<f32>(k_bins));
    if (b >= k_bins) b = k_bins - 1;
    Bin& bin = bins[part * k_bins + b];
    bin.sum[0] += static_cast<f64>(mesh.positions[v].x);
    bin.sum[1] += static_cast<f64>(mesh.positions[v].y);
    bin.sum[2] += static_cast<f64>(mesh.positions[v].z);
    bin.weight += static_cast<f64>(w);
    ++bin.count;
  }
  // How far a part's vertices stand off its axis — the tube's radius, a leaflet's or a card's
  // length — is how far a bin's centroid may be pulled off the axis: the tolerance scales with it.
  Vector<f64> spread(motion.parts.size(), 0.0);
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    const u32 part = motion.vertex_part[v];
    u32 b = static_cast<u32>(motion.vertex_weight[v] * static_cast<f32>(k_bins));
    if (b >= k_bins) b = k_bins - 1;
    const Bin& bin = bins[part * k_bins + b];
    const f64 dx = static_cast<f64>(mesh.positions[v].x) - bin.sum[0] / bin.count;
    const f64 dy = static_cast<f64>(mesh.positions[v].y) - bin.sum[1] / bin.count;
    const f64 dz = static_cast<f64>(mesh.positions[v].z) - bin.sum[2] / bin.count;
    spread[part] = std::max(spread[part], std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  u32 off = 0;
  for (u32 p = 0; p < motion.parts.size(); ++p) {
    CAPTURE(p);
    const f64 tolerance = 0.12 + spread[p] / static_cast<f64>(motion.parts[p].length);
    // A part whose cards stand out further than a quarter of its length (a short top branch of
    // a conifer under 0.4 m needle cards) has centroids that zigzag across it: only its ends say
    // anything there.
    const bool measurable = spread[p] <= 0.25 * static_cast<f64>(motion.parts[p].length);
    CHECK(lo[p] <= 0.01f);  // every part reaches its root...
    CHECK(hi[p] >= 0.99f);  // ...and its tip: the ids are dense, none is empty
    const flora::MotionPart& part = motion.parts[p];
    f64 at[3] = {static_cast<f64>(part.pivot.x), static_cast<f64>(part.pivot.y),
                 static_cast<f64>(part.pivot.z)};
    f64 travelled = 0.0;
    for (u32 b = 0; b < k_bins; ++b) {
      const Bin& bin = bins[p * k_bins + b];
      if (bin.count == 0) continue;
      const f64 c[3] = {bin.sum[0] / bin.count, bin.sum[1] / bin.count, bin.sum[2] / bin.count};
      travelled += std::sqrt((c[0] - at[0]) * (c[0] - at[0]) + (c[1] - at[1]) * (c[1] - at[1]) +
                             (c[2] - at[2]) * (c[2] - at[2]));
      at[0] = c[0];
      at[1] = c[1];
      at[2] = c[2];
      const f64 share = travelled / static_cast<f64>(part.length);
      if (along && measurable && std::abs(share - bin.weight / bin.count) > tolerance) {
        if (off < 4) {
          MESSAGE("part " << p << " bin " << b << ": at " << share << " of its length "
                          << part.length << ", weight " << bin.weight / bin.count << ", spread "
                          << spread[p]);
        }
        ++off;
      }
    }
  }
  CHECK_MESSAGE(off == 0, off << " part bins are not where their weight says along the part");
}

}  // namespace

TEST_CASE("flora: the palm emits the motion contract — dense parts, weights root to tip") {
  const flora::Palm p = default_palm();
  assets::MeshData mesh;
  flora::Motion motion;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_palm(p, mesh, nullptr, &error, &motion), error);
  check_motion(mesh, motion, 1 + p.frond_count + p.dead_skirt_fronds);
  // The trunk's pivot is its foot and its direction the spine's; every frond hangs from it.
  CHECK(std::abs(motion.parts[0].pivot.y) < 1e-6f);
  CHECK(motion.parts[0].direction.y > 0.95f);
  for (u32 k = 1; k < motion.parts.size(); ++k)
    CHECK(motion.parts[k].parent == flora::k_trunk_part);
  // Every rung of the ladder names the same parts: a part is never dropped or merged.
  for (const u32 budget : {20000u, 2000u}) {
    flora::Palm q = p;
    q.triangle_budget = budget;
    assets::MeshData small;
    flora::Motion small_motion;
    REQUIRE_MESSAGE(flora::solve_palm(q, small, nullptr, &error, &small_motion), error);
    CAPTURE(budget);
    // On the bottom rung a frond is a handful of vertices and a leaflet the size of a bin, so the
    // centroids say little there; the ids, the parents and the end weights still hold.
    check_motion(small, small_motion, static_cast<u32>(motion.parts.size()), budget > 2000u);
    for (u32 k = 0; k < motion.parts.size(); ++k) {
      CHECK(small_motion.parts[k].pivot.x == motion.parts[k].pivot.x);
      CHECK(small_motion.parts[k].length == motion.parts[k].length);
    }
  }
  // Without the out-parameter the mesh is the same bytes: the contract rides beside it.
  CHECK(mesh_hash(solve(p)) == mesh_hash(mesh));
}

TEST_CASE("flora: the default palm is valid, holds its budget and is the height asked") {
  flora::PalmReport report;
  const flora::Palm p = default_palm();
  const assets::MeshData mesh = solve(p, &report);
  check_valid(p, mesh);
  MESSAGE("default palm: " << report.triangles << " triangles (trunk " << report.trunk_triangles
                           << ", crown " << report.crown_triangles << ", skirt "
                           << report.skirt_triangles << "), " << report.vertices << " vertices, "
                           << report.rings << " rings, " << report.steps << " budget steps");
  CHECK(report.triangles == mesh.indices.size() / 3);
  CHECK(report.vertices == mesh.positions.size());
  CHECK(report.steps == 0);  // the default fits its own budget at full detail
  // The trunk is material 0, one-sided; the apex is the highest point of the trunk, at height_m.
  REQUIRE(mesh.materials.size() >= 1);
  CHECK_FALSE(mesh.materials[0].double_sided);
  const assets::Primitive* trunk = primitive_of(mesh, 0);
  REQUIRE(trunk != nullptr);
  f32 top = -1e30f;
  f32 bottom = 1e30f;
  for (u32 i = 0; i < trunk->index_count; ++i) {
    const Vec3 v = mesh.positions[mesh.indices[trunk->first_index + i]];
    top = std::max(top, v.y);
    bottom = std::min(bottom, v.y);
  }
  // The wall's last row stands at the height asked; the cap rises a third of a radius above it.
  const f32 cap = 0.35f * p.trunk_top_radius_m;
  // The base ring is square to the leaning spine, so its low side is in the ground by the radius
  // times the lean's sine and its high side above it by as much.
  const f32 dip = p.trunk_base_radius_m * std::sin(p.lean_deg * 3.14159265f / 180.0f);
  CHECK(bottom >= -1.01f * dip);
  CHECK(bottom <= 0.0f);
  CHECK(std::abs(top - cap - p.height_m) <= 0.01f * p.height_m);
}

TEST_CASE("flora: the trunk's radii are the radii asked, and every wall face looks outwards") {
  // Standing straight, the spine is the y axis, and a wall vertex's distance from it is its radius
  // there: the widest in the bottom ring and in the top ring is a boot's lip, where the radius is
  // the one asked.
  flora::Palm p = default_palm();
  p.lean_deg = 0.0f;
  flora::PalmReport report;
  const assets::MeshData mesh = solve(p, &report);
  const assets::Primitive* trunk = primitive_of(mesh, 0);
  REQUIRE(trunk != nullptr);
  const auto widest = [&](f32 y0, f32 y1) {
    f64 r = 0.0;
    for (u32 i = 0; i < trunk->index_count; ++i) {
      const Vec3 v = mesh.positions[mesh.indices[trunk->first_index + i]];
      if (v.y >= y0 && v.y <= y1)
        r = std::max(r, std::hypot(static_cast<f64>(v.x), static_cast<f64>(v.z)));
    }
    return r;
  };
  const f64 base = widest(0.0f, p.ring_spacing_m);
  // The top radius is the stem's where the leaf sheaths begin (the lowest dead frond's base); above
  // it the sheaths swell the stem by 40% by the apex (flora.md, "The crown").
  const f32 sheath = report.sheath_arc_m;
  REQUIRE(sheath > 0.5f * p.height_m);
  REQUIRE(sheath < p.height_m);
  const f64 top = widest(sheath - p.ring_spacing_m, sheath);
  const f64 apex = widest(p.height_m - 0.5f * p.ring_spacing_m, p.height_m + 0.001f);
  CHECK(std::abs(base - static_cast<f64>(p.trunk_base_radius_m)) <=
        0.01 * static_cast<f64>(p.trunk_base_radius_m));
  CHECK(std::abs(top - static_cast<f64>(p.trunk_top_radius_m)) <=
        0.01 * static_cast<f64>(p.trunk_top_radius_m));
  CHECK(std::abs(apex - 1.4 * static_cast<f64>(p.trunk_top_radius_m)) <=
        0.02 * static_cast<f64>(p.trunk_top_radius_m));

  // Every face of the trunk agrees with its corners' normals, and those point away from the
  // spine: the wall's outward, the cap's up. A face wound the wrong way round would be dropped by
  // its cluster's cone seen from outside.
  // Which way is out, for the faces as a whole: the volume they enclose with the base's centre
  // (the open bottom's disc holds that point, so it adds nothing) is positive for outward faces
  // and close to the tapered column's own.
  u32 inward = 0;
  f64 volume = 0.0;
  for (u32 t = trunk->first_index / 3; t < (trunk->first_index + trunk->index_count) / 3; ++t) {
    const Vec3 a = mesh.positions[mesh.indices[t * 3]];
    const Vec3 b = mesh.positions[mesh.indices[t * 3 + 1]];
    const Vec3 c = mesh.positions[mesh.indices[t * 3 + 2]];
    const Vec3 n = cross(b - a, c - a);
    const Vec3 vn = mesh.normals[mesh.indices[t * 3]] + mesh.normals[mesh.indices[t * 3 + 1]] +
                    mesh.normals[mesh.indices[t * 3 + 2]];
    if (dot(n, vn) <= 0.0f) ++inward;
    volume += static_cast<f64>(dot(a, cross(b, c))) / 6.0;
  }
  CHECK(inward == 0);
  const f64 r0 = static_cast<f64>(p.trunk_base_radius_m);
  const f64 r1 = static_cast<f64>(p.trunk_top_radius_m);
  const f64 column =
      3.14159265358979 * static_cast<f64>(p.height_m) * (r0 * r0 + r0 * r1 + r1 * r1) / 3.0;
  CHECK(volume > 0.85 * column);
  CHECK(volume < 1.05 * column);
}

TEST_CASE("flora: each frond is one connected, consistently wound surface above the trunk") {
  const flora::Palm p = default_palm();
  const assets::MeshData mesh = solve(p);
  REQUIRE(mesh.materials.size() == 3);
  CHECK(mesh.materials[1].double_sided);
  CHECK(mesh.materials[2].double_sided);
  CHECK(mesh.materials[1].base_color.y > mesh.materials[1].base_color.x);  // green
  CHECK(mesh.materials[2].base_color.x > mesh.materials[2].base_color.z);  // tan
  for (const i32 material : {1, 2}) {
    CAPTURE(material);
    const assets::Primitive* part = primitive_of(mesh, material);
    REQUIRE(part != nullptr);
    // Connected components over shared vertices: one per frond, since a frond's leaflets stand on
    // its rachis's own edge vertices. A loose leaflet would be a component of its own, and the LOD
    // builder could never collapse it away.
    Vector<u32> parent(mesh.positions.size());
    for (u32 v = 0; v < parent.size(); ++v)
      parent[v] = v;
    const auto find = [&](u32 v) {
      while (parent[v] != v)
        v = parent[v] = parent[parent[v]];
      return v;
    };
    u32 inconsistent = 0;
    for (u32 i = part->first_index; i < part->first_index + part->index_count; i += 3) {
      const u32 a = mesh.indices[i];
      const u32 b = mesh.indices[i + 1];
      const u32 c = mesh.indices[i + 2];
      parent[find(a)] = find(b);
      parent[find(b)] = find(c);
      const Vec3 n =
          cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
      if (dot(n, mesh.normals[a] + mesh.normals[b] + mesh.normals[c]) <= 0.0f) ++inconsistent;
    }
    Vector<u32> roots;
    for (u32 i = part->first_index; i < part->first_index + part->index_count; ++i) {
      const u32 root = find(mesh.indices[i]);
      if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
    }
    CHECK(roots.size() == (material == 1 ? p.frond_count : p.dead_skirt_fronds));
    // A face whose winding disagrees with its corners' area-weighted normals is a fold or a flip;
    // a few where a leaflet's base turns against the rachis are tolerated, not a whole side.
    CHECK(inconsistent * 100 <= part->index_count / 3);
  }
  // The crown stands at the apex: its highest point above the trunk's, and every dead frond's
  // highest point below the apex.
  const assets::Primitive* crown = primitive_of(mesh, 1);
  f32 crown_top = -1e30f;
  for (u32 i = crown->first_index; i < crown->first_index + crown->index_count; ++i)
    crown_top = std::max(crown_top, mesh.positions[mesh.indices[i]].y);
  CHECK(crown_top > p.height_m + 0.5f);
  const assets::Primitive* skirt = primitive_of(mesh, 2);
  f32 skirt_top = -1e30f;
  for (u32 i = skirt->first_index; i < skirt->first_index + skirt->index_count; ++i)
    skirt_top = std::max(skirt_top, mesh.positions[mesh.indices[i]].y);
  CHECK(skirt_top < p.height_m);
  // No skirt asked, no skirt primitive.
  flora::Palm bare = p;
  bare.dead_skirt_fronds = 0;
  const assets::MeshData bare_mesh = solve(bare);
  CHECK(primitive_of(bare_mesh, 2) == nullptr);
}

// The crown is one leaf spiral by age (flora.md, "The crown"): a frond's base is lower the older it
// is, by one internode a frond, each turned the golden angle from the last; the youngest stands
// near vertical and the oldest at the cone's edge; the trunk's top is a collar of persistent bases
// below the dead fronds, out of the trunk and pointing up.
TEST_CASE("flora: the fronds leave the stem on one spiral by age, above a collar of old bases") {
  flora::Palm p = default_palm();
  p.lean_deg = 0.0f;  // a vertical spine, so a pivot's height is its arc
  assets::MeshData mesh;
  flora::Motion motion;
  flora::PalmReport report;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_palm(p, mesh, &report, &error, &motion), error);
  const u32 fronds = p.frond_count + p.dead_skirt_fronds;
  REQUIRE(motion.parts.size() == 1 + fronds);
  const f32 internode = report.internode_m;
  CHECK(internode > 0.0f);
  // One internode between green fronds; the dead fronds hang from a deeper band below them, still
  // one leaf after another down the stem.
  for (u32 k = 1; k < fronds; ++k) {
    CAPTURE(k);
    const f32 drop = motion.parts[k].pivot.y - motion.parts[k + 1].pivot.y;
    if (k < p.frond_count) {
      CHECK(std::abs(drop - internode) < 0.25f * internode);
    } else {
      CHECK(drop > 0.5f * internode);
    }
  }
  // The youngest frond stands near vertical and the oldest green one at the cone's edge.
  const auto from_vertical = [](const Vec3& d) {
    return std::acos(std::clamp(d.y, -1.0f, 1.0f)) * 180.0f / 3.14159265f;
  };
  CHECK(from_vertical(motion.parts[1].direction) < 0.25f * p.frond_spread_deg);
  CHECK(from_vertical(motion.parts[p.frond_count].direction) > 0.85f * p.frond_spread_deg);
  // Younger means steeper, frond by frond, apart from the seed's few degrees.
  for (u32 k = 1; k + 3 < p.frond_count; ++k)
    CHECK(from_vertical(motion.parts[k].direction) < from_vertical(motion.parts[k + 3].direction));
  // The collar: below the lowest dead frond, out of the stem and pointing up.
  CHECK(report.collar_bases > 20);
  CHECK(report.collar_triangles == (fronds + report.collar_bases) * 12);  // a boot per leaf
  const assets::Primitive* trunk = primitive_of(mesh, 0);
  REQUIRE(trunk != nullptr);
  REQUIRE(mesh.primitives.size() == 4);
  const assets::Primitive& collar = mesh.primitives[1];
  CHECK(collar.material == 0);
  f32 lowest = 1e30f;
  f32 widest = 0.0f;
  for (u32 i = collar.first_index; i < collar.first_index + collar.index_count; ++i) {
    const Vec3 v = mesh.positions[mesh.indices[i]];
    lowest = std::min(lowest, v.y);
    widest = std::max(widest, std::hypot(v.x, v.z));
  }
  CHECK(lowest > 0.6f * p.height_m);
  CHECK(lowest < report.sheath_arc_m);
  CHECK(widest > 1.5f * p.trunk_top_radius_m);  // the bases stand out of the stem
  // A frond leaves the stem where its base is on the spiral: inside the swollen sheath.
  for (u32 k = 1; k <= fronds; ++k) {
    const Vec3 pivot = motion.parts[k].pivot;
    CHECK(std::hypot(pivot.x, pivot.z) < 1.4f * p.trunk_top_radius_m);
  }
}

TEST_CASE("flora: a smaller budget walks down the ladder and holds") {
  for (const u32 budget : {40000u, 20000u, 8000u, 2000u}) {
    flora::Palm p = default_palm();
    p.triangle_budget = budget;
    flora::PalmReport report;
    const assets::MeshData mesh = solve(p, &report);
    CAPTURE(budget);
    check_valid(p, mesh);
    CHECK(report.steps > 0);
    CHECK(report.triangles <= budget);
    // The fronds are kept whatever the budget: the crown's outline is what a palm is.
    const assets::Primitive* crown = primitive_of(mesh, 1);
    REQUIRE(crown != nullptr);
    MESSAGE(budget << ": " << report.triangles << " triangles after " << report.steps
                   << " steps; leaflets " << report.leaflets_per_side << " of "
                   << report.leaflet_segments << " segments, " << report.trunk_segments
                   << " segments x " << report.rows_per_ring << " rows a ring");
  }
}

TEST_CASE("flora: one seed is one palm, and another seed is another") {
  const flora::Palm p = default_palm();
  const u64 a = mesh_hash(solve(p));
  const u64 b = mesh_hash(solve(p));
  CHECK(a == b);
  flora::Palm q = p;
  q.seed = 2;
  CHECK(mesh_hash(solve(q)) != a);
}

TEST_CASE("flora: every parameter at the ends of its range builds a valid palm in its budget") {
  struct Case {
    const char* name;
    void (*set)(flora::Palm&, bool high);
  };
  const Case cases[] = {
      {"height_m", [](flora::Palm& p, bool h) { p.height_m = h ? 30.0f : 1.0f; }},
      {"trunk_base_radius_m",
       [](flora::Palm& p, bool h) {
         p.trunk_base_radius_m = h ? 1.0f : 0.05f;
         p.trunk_top_radius_m = h ? 0.18f : 0.03f;
         p.ring_depth_m = h ? 0.014f : 0.0075f;
       }},
      {"trunk_top_radius_m",
       [](flora::Palm& p, bool h) { p.trunk_top_radius_m = h ? p.trunk_base_radius_m : 0.06f; }},
      {"lean_deg", [](flora::Palm& p, bool h) { p.lean_deg = h ? 40.0f : 0.0f; }},
      {"curve", [](flora::Palm& p, bool h) { p.curve = h ? 1.0f : 0.0f; }},
      {"ring_spacing_m", [](flora::Palm& p, bool h) { p.ring_spacing_m = h ? 1.0f : 0.04f; }},
      {"ring_depth_m", [](flora::Palm& p, bool h) { p.ring_depth_m = h ? 0.045f : 0.0f; }},
      {"frond_count", [](flora::Palm& p, bool h) { p.frond_count = h ? 64u : 1u; }},
      {"frond_length_m", [](flora::Palm& p, bool h) { p.frond_length_m = h ? 8.0f : 0.5f; }},
      {"frond_droop", [](flora::Palm& p, bool h) { p.frond_droop = h ? 1.0f : 0.0f; }},
      {"frond_spread_deg", [](flora::Palm& p, bool h) { p.frond_spread_deg = h ? 120.0f : 5.0f; }},
      {"leaflet_count", [](flora::Palm& p, bool h) { p.leaflet_count = h ? 160u : 2u; }},
      {"leaflet_width_m", [](flora::Palm& p, bool h) { p.leaflet_width_m = h ? 0.12f : 0.005f; }},
      {"leaflet_angle_deg",
       [](flora::Palm& p, bool h) { p.leaflet_angle_deg = h ? 85.0f : 10.0f; }},
      {"dead_skirt_fronds", [](flora::Palm& p, bool h) { p.dead_skirt_fronds = h ? 64u : 0u; }},
      {"triangle_budget", [](flora::Palm& p, bool h) { p.triangle_budget = h ? 2000000u : 2000u; }},
  };
  for (const Case& c : cases) {
    for (const bool high : {false, true}) {
      flora::Palm p = default_palm();
      c.set(p, high);
      CAPTURE(c.name);
      CAPTURE(high);
      std::string error;
      REQUIRE_MESSAGE(flora::palm_in_range(p, error), error);
      flora::PalmReport report;
      const assets::MeshData mesh = solve(p, &report);
      check_valid(p, mesh);
    }
  }
  // Everything at its most expensive at once, on the smallest budget: the ladder's bottom rung.
  flora::Palm worst = default_palm();
  worst.height_m = 30.0f;
  worst.ring_spacing_m = 0.04f;
  worst.frond_count = 64;
  worst.leaflet_count = 160;
  worst.dead_skirt_fronds = 64;
  worst.triangle_budget = 2000;
  flora::PalmReport report;
  const assets::MeshData mesh = solve(worst, &report);
  check_valid(worst, mesh);
  MESSAGE("worst case on 2,000 triangles: " << report.triangles << " after " << report.steps
                                            << " steps");
}

TEST_CASE("flora: a parameter out of range is refused with its name") {
  flora::Palm p = default_palm();
  p.height_m = 0.5f;
  std::string error;
  CHECK_FALSE(flora::palm_in_range(p, error));
  CHECK(error.find("height_m") != std::string::npos);
  p = default_palm();
  p.leaflet_angle_deg = std::nanf("");
  CHECK_FALSE(flora::palm_in_range(p, error));
  CHECK(error.find("leaflet_angle_deg") != std::string::npos);
  assets::MeshData mesh;
  CHECK_FALSE(flora::solve_palm(p, mesh, nullptr, &error));
}

// The default palm's container, as `engine-content flora` and the renderer build it: three
// primitives with their own cone settings, the identity, the generator section; and its content
// hash pinned beside the mesh's, since the cluster build is the other half of what a palm's bytes
// are and is held to the same rule (geometry.md, "The same bytes from every toolchain").
TEST_CASE(
    "flora: the default palm's container has its parts, its identity and its committed bytes") {
  flora::FloraFile file;
  geometry::ClusterFileData data;
  flora::PalmReport report;
  std::string error;
  const geometry::ClusterLodOptions lod;
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  REQUIRE_MESSAGE(flora::build_flora_container(file, lod, page_bytes, data, &report, error), error);
  CHECK(data.mesh.leaf_triangle_count == report.triangles);
  REQUIRE(data.materials.size() == 3);
  CHECK(data.cluster_material.size() == data.mesh.mesh.clusters.size());
  CHECK_FALSE(data.pages.pages.empty());
  CHECK(data.mesh.level_cluster_counts.size() > 3);
  // The trunk's clusters carry cones; a frond's never do (k_cone_none is a cutoff of 1).
  u32 coned[3] = {0, 0, 0};
  for (u32 c = 0; c < data.mesh.mesh.clusters.size(); ++c) {
    const geometry::NormalCone cone = geometry::decode_cone(data.mesh.mesh.clusters[c].cone);
    if (cone.cutoff < 1.0f) ++coned[data.cluster_material[c]];
  }
  CHECK(coned[0] > 0);
  CHECK(coned[1] == 0);
  CHECK(coned[2] == 0);
  CHECK(data.source_hash == flora::flora_source_hash(file));
  CHECK(data.build_key == flora::flora_build_key(data.source_hash, lod, page_bytes));
  CHECK(data.generator == "flora");
  CHECK(data.generator_kind == "date_palm");
  CHECK(data.generator_params == flora::canonical_flora_json(file));
  MESSAGE("default palm: " << data.mesh.mesh.clusters.size() << " clusters, "
                           << data.mesh.level_cluster_counts.size() << " levels, "
                           << data.pages.pages.size() << " pages");

  // Written and read back, it is the same container, generator section and all.
  const u64 hash = geometry::cluster_file_hash(data);
  char hex[32];
  std::snprintf(hex, sizeof(hex), "0x%016llxull", static_cast<unsigned long long>(hash));
  // With the motion contract appended (sections 35 and 36); without it, the next test holds it to
  // the bytes it was before them.
  // 0x5ab6c0536295b350 at k_flora_version 2; the palm's UVs in metres moved it (version 3), and
  // 0x178ae32e3572bd96 was version 3's: the version is in the build key the container records, so
  // version 4 (the trees' impostor chain, R91) moved it with no byte of the palm's mesh moving,
  // and 0x1fc747f1b4eb758b was version 4's, until version 5 (the megaflora's habits, G79).
  constexpr u64 k_golden = 0xf5eeb4a2203dcbbdull;
  CHECK_MESSAGE(hash == k_golden, "the default palm's container hashes to " << hex);

  // The canonical form is the parameters, defaults filled in, whatever the file spelled.
  flora::FloraFile spelled;
  REQUIRE_MESSAGE(flora::read_flora_file("{ \"palm\" : { \"seed\" : 1 } }", spelled, error), error);
  CHECK(flora::canonical_flora_json(spelled) == flora::canonical_flora_json(file));
  CHECK(flora::flora_source_hash(spelled) == flora::flora_source_hash(file));
  CHECK_FALSE(flora::read_flora_file("{\"species\":\"oak\"}", spelled, error));
  CHECK(error.find("oak") != std::string::npos);
  CHECK_FALSE(flora::read_flora_file("{\"palm\":{\"frond_count\":0}}", spelled, error));
  CHECK(error.find("frond_count") != std::string::npos);
}

namespace {

// A container's section records and payloads as written: { kind, element size, count, bytes }.
struct RawSection {
  u32 kind = 0;
  u32 element_size = 0;
  u64 count = 0;
  std::string bytes;
};
Vector<RawSection> raw_sections(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  REQUIRE(file.size() >= sizeof(geometry::ClusterFileHeader));
  geometry::ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  Vector<RawSection> out;
  for (u32 i = 0; i < header.section_count; ++i) {
    geometry::ClusterFileSection record;
    std::memcpy(&record, file.data() + sizeof(header) + i * sizeof(record), sizeof(record));
    RawSection s;
    s.kind = record.kind;
    s.element_size = record.element_size;
    s.count = record.element_count;
    s.bytes = file.substr(static_cast<usize>(record.offset),
                          static_cast<usize>(record.element_size * record.element_count));
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace

// The motion contract in the container (sections 35 and 36): the table round-trips, every cluster
// vertex's record is its source vertex's, and the sections are **appended** — the container with
// them cleared is the container that was, every other section the same bytes.
TEST_CASE("flora: the motion table round-trips through the container, appended to the rest") {
  flora::FloraFile file;
  geometry::ClusterFileData data;
  std::string error;
  const geometry::ClusterLodOptions lod;
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  REQUIRE_MESSAGE(flora::build_flora_container(file, lod, page_bytes, data, nullptr, error), error);
  assets::MeshData mesh;
  flora::Motion motion;
  REQUIRE(flora::solve_palm(file.palm, mesh, nullptr, &error, &motion));
  REQUIRE(data.motion_parts.size() == motion.parts.size());
  REQUIRE(data.motion_vertices.size() == data.mesh.mesh.vertices.size());
  for (u32 v = 0; v < data.mesh.mesh.vertices.size(); ++v) {
    // Each primitive was built over the whole vertex array; the merge shifts its source indices.
    const u32 source = data.mesh.mesh.vertex_source[v] % static_cast<u32>(mesh.positions.size());
    REQUIRE(source < mesh.positions.size());
    CHECK(data.mesh.mesh.vertices[v].x == mesh.positions[source].x);
    CHECK(data.motion_vertices[v].part == motion.vertex_part[source]);
    CHECK(data.motion_vertices[v].weight ==
          flora::encode_bend_weight(motion.vertex_weight[source]));
  }
  CHECK(flora::encode_bend_weight(0.0f) == 0);
  CHECK(flora::encode_bend_weight(1.0f) == 65535);
  CHECK(flora::decode_bend_weight(65535) == 1.0f);

  engine::test::TempDir tmp("flora");
  const std::string with = tmp.file("with.clusters");
  const std::string without = tmp.file("without.clusters");
  REQUIRE_MESSAGE(geometry::write_cluster_file(with, data, &error), error);
  geometry::ClusterFileData back;
  REQUIRE_MESSAGE(geometry::read_cluster_file(with, back, &error), error);
  REQUIRE(back.motion_parts.size() == data.motion_parts.size());
  CHECK(std::memcmp(back.motion_parts.data(), data.motion_parts.data(),
                    data.motion_parts.size() * sizeof(geometry::ClusterFileMotionPart)) == 0);
  REQUIRE(back.motion_vertices.size() == data.motion_vertices.size());
  CHECK(std::memcmp(back.motion_vertices.data(), data.motion_vertices.data(),
                    data.motion_vertices.size() * sizeof(geometry::ClusterFileMotionVertex)) == 0);

  geometry::ClusterFileData rigid = data;
  rigid.motion_parts.clear();
  rigid.motion_vertices.clear();
  REQUIRE_MESSAGE(geometry::write_cluster_file(without, rigid, &error), error);
  const Vector<RawSection> a = raw_sections(with);
  const Vector<RawSection> b = raw_sections(without);
  REQUIRE(a.size() == b.size() + 2);
  for (u32 i = 0; i < b.size(); ++i) {
    CAPTURE(b[i].kind);
    CHECK(a[i].kind == b[i].kind);
    CHECK(a[i].element_size == b[i].element_size);
    CHECK(a[i].count == b[i].count);
    CHECK(a[i].bytes == b[i].bytes);
  }
  CHECK(a[b.size()].kind == static_cast<u32>(geometry::ClusterSection::MotionParts));
  CHECK(a[b.size() + 1].kind == static_cast<u32>(geometry::ClusterSection::MotionVertices));
  CHECK(std::string(geometry::cluster_section_name(a[b.size()].kind)) == "motion_parts");
  // The container with the contract cleared, pinned on its own: it was 0xab870afd905f4acd, the
  // container from before the contract existed, until the crown's spiral moved the mesh
  // (k_flora_version 2), and 0xeb471d9e96454476 until the UVs went to metres (3), so a change that
  // moves only the contract is told apart from one that moves the mesh; 0x31af079d27301ce7 until
  // k_flora_version 4 moved the build key it records (R91; the palm's mesh did not move), and
  // 0x4cbe7ab61460f083 until version 5 did the same (G79, the megaflora's habits).
  constexpr u64 k_rigid_golden = 0x1cbf4f684b7db8dfull;
  const u64 rigid_hash = geometry::cluster_file_hash(rigid);
  char hex[32];
  std::snprintf(hex, sizeof(hex), "0x%016llxull", static_cast<unsigned long long>(rigid_hash));
  CHECK_MESSAGE(rigid_hash == k_rigid_golden, "the rigid palm's container hashes to " << hex);
}

// The default palm's mesh, pinned (flora.md, "The same palm from every toolchain"): the solver's
// arithmetic is IEEE f64 with contraction off and the deterministic sines, so this number is the
// same on MSVC, GCC and Clang at either baseline. A change to the solver that moves it bumps
// `k_flora_version` and the number in the same commit.
TEST_CASE("flora: the default palm is the committed bytes on every toolchain") {
  const u64 got = mesh_hash(solve(default_palm()));
  // 0x86f10c7cf1c598e7 at version 2; the UVs in metres (3) moved only the UVs.
  constexpr u64 k_golden = 0x297fc148e1d646fcull;
  char hex[32];
  std::snprintf(hex, sizeof(hex), "0x%016llxull", static_cast<unsigned long long>(got));
  CHECK_MESSAGE(got == k_golden, "the default palm's mesh hashes to " << hex);
}

// ---- the branching species (flora.md, "The broadleaf", "The conifer") -------------------------

namespace {

// Every property a branching species' mesh has: valid, in budget, two primitives (bark one-sided,
// leaves two-sided), and the motion contract with every part reaching its root and its tip.
void check_tree(const assets::MeshData& mesh, const flora::Motion& motion,
                const flora::TreeReport& r, u32 budget, bool along) {
  check_mesh(budget, mesh);
  REQUIRE(mesh.materials.size() == 2);
  CHECK_FALSE(mesh.materials[0].double_sided);
  CHECK(mesh.materials[1].double_sided);
  REQUIRE(mesh.primitives.size() == 2);
  CHECK(r.triangles == mesh.indices.size() / 3);
  CHECK(r.wood_triangles + r.leaf_triangles == r.triangles);
  CHECK(r.parts == motion.parts.size());
  check_motion(mesh, motion, r.parts, along);
}

u64 tree_hash(const assets::MeshData& mesh, const flora::Motion& motion) {
  u64 h = mesh_hash(mesh);
  h = hash_combine(h, hash_bytes(motion.parts.data(),
                                 motion.parts.size() * sizeof(geometry::ClusterFileMotionPart)));
  h = hash_combine(h,
                   hash_bytes(motion.vertex_part.data(), motion.vertex_part.size() * sizeof(u32)));
  return hash_combine(
      h, hash_bytes(motion.vertex_weight.data(), motion.vertex_weight.size() * sizeof(f32)));
}

template <typename Params, typename Solve>
void check_species(const std::string& name, const Params& params, Solve solve, u32 expected_parts,
                   u64 golden) {
  CAPTURE(name);
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE_MESSAGE(solve(params, mesh, &report, &error, &motion), error);
  MESSAGE(name << ": " << report.triangles << " triangles (bark " << report.wood_triangles
               << ", leaves " << report.leaf_triangles << "), " << report.parts << " parts, "
               << report.sprays << " sprays of " << report.cards_per_side << " cards a side, "
               << report.steps << " budget steps");
  CHECK(report.steps == 0);  // the defaults fit their own budget at full detail
  CHECK(report.parts == expected_parts);
  check_tree(mesh, motion, report, params.triangle_budget, true);
  // The leaves: each spray one connected surface, so the LOD builder can collapse it.
  const assets::Primitive& leaves = mesh.primitives[1];
  Vector<u32> parent(mesh.positions.size());
  for (u32 v = 0; v < parent.size(); ++v)
    parent[v] = v;
  const auto find = [&](u32 v) {
    while (parent[v] != v)
      v = parent[v] = parent[parent[v]];
    return v;
  };
  for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; i += 3) {
    parent[find(mesh.indices[i])] = find(mesh.indices[i + 1]);
    parent[find(mesh.indices[i + 1])] = find(mesh.indices[i + 2]);
  }
  Vector<u32> roots;
  for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; ++i) {
    const u32 root = find(mesh.indices[i]);
    if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
  }
  CHECK(roots.size() == report.sprays);
  // Every rung of the ladder holds and names the same parts.
  for (const u32 budget : {params.triangle_budget / 2, params.triangle_budget / 5, 8000u}) {
    CAPTURE(budget);
    Params q = params;
    q.triangle_budget = budget;
    assets::MeshData small;
    flora::Motion small_motion;
    flora::TreeReport small_report;
    REQUIRE_MESSAGE(solve(q, small, &small_report, &error, &small_motion), error);
    MESSAGE(name << " on " << budget << ": " << small_report.triangles << " triangles after "
                 << small_report.steps << " steps, " << small_report.cards_per_side
                 << " cards a side");
    check_tree(small, small_motion, small_report, budget, false);
    REQUIRE(small_motion.parts.size() == motion.parts.size());
    for (u32 k = 0; k < motion.parts.size(); ++k) {
      CHECK(small_motion.parts[k].pivot.x == motion.parts[k].pivot.x);
      CHECK(small_motion.parts[k].length == motion.parts[k].length);
      CHECK(small_motion.parts[k].parent == motion.parts[k].parent);
    }
  }
  // One seed one tree, the same bytes on every toolchain; another seed another tree.
  assets::MeshData again;
  flora::Motion again_motion;
  REQUIRE(solve(params, again, nullptr, &error, &again_motion));
  const u64 got = tree_hash(again, again_motion);
  CHECK(got == tree_hash(mesh, motion));
  char hex[32];
  std::snprintf(hex, sizeof(hex), "0x%016llxull", static_cast<unsigned long long>(got));
  CHECK_MESSAGE(got == golden, name << " hashes to " << hex);
  Params other = params;
  other.seed = 2;
  assets::MeshData other_mesh;
  flora::Motion other_motion;
  REQUIRE(solve(other, other_mesh, nullptr, &error, &other_motion));
  CHECK(tree_hash(other_mesh, other_motion) != got);
}

}  // namespace

TEST_CASE("flora: the broadleaf grows trunk, limbs, branches and leafy twigs, in its budget") {
  const flora::Broadleaf p;
  // Parts: the trunk, the limbs and the recursion's shoots of the part class and up — 230 on the
  // default (flora.md, "The broadleaf"); every finer shoot and every twig rides one of them.
  Vector<flora::CrownShoot> crown;
  REQUIRE(flora::broadleaf_crown(p, crown));
  u32 parts = 0;
  for (const flora::CrownShoot& s : crown)
    parts += (s.flags & flora::k_crown_shoot_part) != 0u ? 1u : 0u;
  CHECK(parts == 230u);
  // 0x80af7e3dd0bf4804 until the crown became the allometric recursion (G72).
  check_species("broadleaf", p, flora::solve_broadleaf, parts, 0x1830133b376b6e08ull);
}

TEST_CASE("flora: the conifer grows whorls of needled branches thinning upward, in its budget") {
  const flora::Conifer p;
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_conifer(p, mesh, &report, &error, &motion), error);
  // Parts: the trunk and every branch; the whorls thin upward, so the top has fewer.
  // 0xd849a6672e26415a until the laterals, the interwhorl branches and the branches' fates (G80).
  check_species("conifer", p, flora::solve_conifer, report.parts, 0x2a62d51b98e2dd20ull);
  CHECK(report.parts < 1 + p.whorls * p.branches_per_whorl + (p.whorls - 1) * p.interwhorl);
  CHECK(report.parts > 1 + p.whorls * 2);
  // A cone: the lowest branches reach furthest, the highest least.
  f32 low_reach = 0.0f;
  f32 high_reach = 0.0f;
  for (u32 k = 1; k < motion.parts.size(); ++k) {
    const f32 y = motion.parts[k].pivot.y;
    if (y < 0.4f * p.height_m) low_reach = std::max(low_reach, motion.parts[k].length);
    if (y > 0.8f * p.height_m) high_reach = std::max(high_reach, motion.parts[k].length);
  }
  CHECK(low_reach > 2.0f * high_reach);
}

namespace {

struct ConiferBuild {
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
};

ConiferBuild build_conifer(const flora::Conifer& p) {
  ConiferBuild b;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_conifer(p, b.mesh, &b.report, &error, &b.motion), error);
  return b;
}

// Per part, the smallest bend weight of its needle vertices (2 for a part with none).
Vector<f32> lowest_needle_weight(const ConiferBuild& b) {
  Vector<f32> lowest(b.motion.parts.size(), 2.0f);
  const assets::Primitive& leaves = b.mesh.primitives[1];
  for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; ++i) {
    const u32 v = b.mesh.indices[i];
    const u32 part = b.motion.vertex_part[v];
    lowest[part] = std::min(lowest[part], b.motion.vertex_weight[v]);
  }
  return lowest;
}

}  // namespace

// G80 (flora.md, "The conifer"; experiments/conifer-shoots.md): the needles are on the outer
// years of the live branches — a long branch's inner part is bare — and the dead branches of the
// self-pruned zone carry none.
TEST_CASE("flora: the conifer's needles sit on its branches' outer years, none on the dead") {
  flora::Conifer p;
  p.crown_radius_m = 6.0f;  // branches long enough to have a bare inner part
  const ConiferBuild b = build_conifer(p);
  const Vector<f32> lowest = lowest_needle_weight(b);
  const f32 needled = p.needle_years * p.shoot_growth_m;
  u32 bare = 0;
  u32 long_live = 0;
  for (u32 k = 1; k < b.motion.parts.size(); ++k) {
    CAPTURE(k);
    if (lowest[k] > 1.5f) {
      ++bare;
      continue;
    }
    // Laterals reach in at most two needled reaches from the tip; the branch's own needles one.
    const f32 length = b.motion.parts[k].length;
    if (length > 2.5f * needled) {
      ++long_live;
      CHECK(lowest[k] >= (length - 2.0f * needled) / length - 1e-3f);
    }
  }
  MESSAGE("conifer: " << bare << " dead branches of " << b.motion.parts.size() - 1 << ", "
                      << long_live << " live branches long enough to be bare inside, "
                      << b.report.leaf_cards << " cards, " << b.report.leaf_area_m2
                      << " m2 of them");
  CHECK(bare > 0u);
  CHECK(long_live > 0u);
  // With no dead zone and nothing missing, every branch is needled.
  p.dead_share = 0.0f;
  p.missing_branches = 0.0f;
  p.sparse_whorls = 0.0f;
  const ConiferBuild live = build_conifer(p);
  const Vector<f32> live_lowest = lowest_needle_weight(live);
  for (u32 k = 1; k < live.motion.parts.size(); ++k) {
    CAPTURE(k);
    CHECK(live_lowest[k] < 1.5f);
  }
  // The leaf cards and their area are reported.
  CHECK(b.report.leaf_cards > 0u);
  CHECK(b.report.leaf_area_m2 > 0.0f);
  CHECK(b.report.shoots > b.report.parts);
}

// G80: a seeded share of the branches is missing — some leave a broken stub — drawn on a stream
// of its own, so every branch that stays stands exactly where it stood.
TEST_CASE("flora: the conifer's missing branches move none that stay") {
  flora::Conifer full;
  full.missing_branches = 0.0f;
  full.sparse_whorls = 0.0f;
  flora::Conifer gappy = full;
  gappy.missing_branches = 0.35f;
  gappy.sparse_whorls = 0.3f;
  const ConiferBuild a = build_conifer(full);
  const ConiferBuild b = build_conifer(gappy);
  CHECK(b.motion.parts.size() < a.motion.parts.size());
  u32 stubs = 0;
  u32 next = 1;  // parts come in the same order, so a walk finds every survivor in turn
  for (u32 k = 1; k < b.motion.parts.size(); ++k) {
    CAPTURE(k);
    const flora::MotionPart& part = b.motion.parts[k];
    while (next < a.motion.parts.size() && !(a.motion.parts[next].pivot.x == part.pivot.x &&
                                             a.motion.parts[next].pivot.y == part.pivot.y &&
                                             a.motion.parts[next].pivot.z == part.pivot.z))
      ++next;
    REQUIRE(next < a.motion.parts.size());
    const flora::MotionPart& was = a.motion.parts[next];
    CHECK(part.direction.x == was.direction.x);
    CHECK(part.direction.y == was.direction.y);
    CHECK(part.direction.z == was.direction.z);
    if (part.length < 0.2f * was.length) ++stubs;
    ++next;
  }
  MESSAGE("conifer: " << a.motion.parts.size() - b.motion.parts.size() << " branches gone, "
                      << stubs << " broken stubs");
  CHECK(stubs > 0u);
  // The other draws did not move either: the trunk is the same bits.
  REQUIRE(a.mesh.positions.size() > 0);
  CHECK(a.mesh.positions[0].x == b.mesh.positions[0].x);
  CHECK(a.mesh.positions[0].y == b.mesh.positions[0].y);
}

// G80: a comb spruce's laterals hang from their branches as curtains; a plate spruce's lie level.
TEST_CASE("flora: the conifer's laterals hang by its pendulous habit") {
  flora::Conifer plate;
  plate.pendulous = 0.0f;
  flora::Conifer comb = plate;
  comb.pendulous = 1.0f;
  const ConiferBuild a = build_conifer(plate);
  const ConiferBuild b = build_conifer(comb);
  REQUIRE(a.motion.parts.size() == b.motion.parts.size());
  const auto mean_drop = [](const ConiferBuild& t) {
    const assets::Primitive& leaves = t.mesh.primitives[1];
    f64 drop = 0.0;
    u32 n = 0;
    for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; ++i) {
      const u32 v = t.mesh.indices[i];
      const u32 part = t.motion.vertex_part[v];
      if (part == 0) continue;
      drop +=
          static_cast<f64>(t.motion.parts[part].pivot.y) - static_cast<f64>(t.mesh.positions[v].y);
      ++n;
    }
    return drop / static_cast<f64>(n);
  };
  const f64 plate_drop = mean_drop(a);
  const f64 comb_drop = mean_drop(b);
  MESSAGE("needles below their branch's base: plate " << plate_drop << " m, comb " << comb_drop
                                                      << " m");
  CHECK(comb_drop > plate_drop + 0.1);
}

// G80: a recipe written for version 1 of the conifer — no laterals, no fates — still reads, and
// solves in its budget with the new fields at their defaults.
TEST_CASE("flora: a version 1 conifer recipe still reads") {
  flora::FloraFile file;
  std::string error;
  REQUIRE_MESSAGE(
      flora::read_flora_file(
          R"({"species":"conifer","conifer":{"seed":3,"height_m":22.0,"needle_cards":24,)"
          R"("needle_length_m":0.42,"triangle_budget":50000}})",
          file, error),
      error);
  CHECK(file.conifer.needle_cards == 24u);
  CHECK(file.conifer.interwhorl == flora::Conifer{}.interwhorl);
  assets::MeshData mesh;
  flora::TreeReport report;
  REQUIRE_MESSAGE(flora::solve_conifer(file.conifer, mesh, &report, &error), error);
  CHECK(report.triangles <= 50000u);
}

TEST_CASE("flora: the branching species' ranges, ends and refusals") {
  std::string error;
  flora::Broadleaf b;
  b.limbs = 0;
  CHECK_FALSE(flora::broadleaf_in_range(b, error));
  CHECK(error.find("limbs") != std::string::npos);
  flora::Conifer c;
  c.whorls = 61;
  CHECK_FALSE(flora::conifer_in_range(c, error));
  CHECK(error.find("whorls") != std::string::npos);
  c = flora::Conifer{};
  c.interwhorl = 7;
  CHECK_FALSE(flora::conifer_in_range(c, error));
  CHECK(error.find("engine.flora.Conifer.interwhorl") != std::string::npos);
  c = flora::Conifer{};
  c.pendulous = 1.5f;
  CHECK_FALSE(flora::conifer_in_range(c, error));
  CHECK(error.find("engine.flora.Conifer.pendulous") != std::string::npos);
  c = flora::Conifer{};
  c.dead_share = 0.95f;
  CHECK_FALSE(flora::conifer_in_range(c, error));
  CHECK(error.find("engine.flora.Conifer.dead_share") != std::string::npos);
  // Everything at its most expensive at once, on the smallest budget: the bottom rung holds.
  flora::Broadleaf wb;
  wb.limbs = 12;
  wb.branches = 8;
  wb.twigs = 10;
  wb.leaves = 40;
  wb.triangle_budget = 8000;
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  REQUIRE_MESSAGE(flora::solve_broadleaf(wb, mesh, &report, &error, &motion), error);
  check_tree(mesh, motion, report, wb.triangle_budget, false);
  MESSAGE("broadleaf at its most expensive on 8,000: " << report.triangles << " after "
                                                       << report.steps << " steps");
  flora::Conifer wc;
  wc.whorls = 60;
  wc.branches_per_whorl = 10;
  wc.thinning = 0.0f;
  wc.needle_cards = 60;
  wc.height_m = 60.0f;
  wc.interwhorl = 6;
  wc.laterals_per_m = 20.0f;
  wc.lateral_length_m = 2.5f;
  wc.dead_share = 0.0f;
  wc.missing_branches = 0.0f;
  wc.sparse_whorls = 0.0f;
  wc.triangle_budget = 8000;
  REQUIRE_MESSAGE(flora::solve_conifer(wc, mesh, &report, &error, &motion), error);
  check_tree(mesh, motion, report, wc.triangle_budget, false);
  MESSAGE("conifer at its most expensive on 8,000: " << report.triangles << " after "
                                                     << report.steps << " steps");
  // The smallest trees build too.
  flora::Broadleaf sb;
  sb.height_m = 3.0f;
  sb.limbs = 1;
  sb.branches = 1;
  sb.twigs = 1;
  sb.leaves = 1;
  REQUIRE_MESSAGE(flora::solve_broadleaf(sb, mesh, &report, &error, &motion), error);
  check_tree(mesh, motion, report, sb.triangle_budget, false);
  flora::Conifer sc;
  sc.height_m = 1.0f;
  sc.whorls = 2;
  sc.branches_per_whorl = 2;
  REQUIRE_MESSAGE(flora::solve_conifer(sc, mesh, &report, &error, &motion), error);
  check_tree(mesh, motion, report, sc.triangle_budget, false);
}

// A flora file names its species, and the container is built from that species' parameters alone:
// the canonical form carries nothing of the species not chosen, so the palm's key is the same
// whatever the broadleaf's defaults are.
TEST_CASE("flora: a file's species picks the solver, and its key carries that species alone") {
  flora::FloraFile file;
  std::string error;
  REQUIRE_MESSAGE(
      flora::read_flora_file("{\"species\":\"broadleaf\",\"broadleaf\":{\"seed\":3}}", file, error),
      error);
  const std::string canonical = flora::canonical_flora_json(file);
  CHECK(canonical.find("\"broadleaf\"") != std::string::npos);
  CHECK(canonical.find("\"palm\"") == std::string::npos);
  CHECK(canonical.find("\"conifer\"") == std::string::npos);
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(flora::build_flora_container(file, geometry::ClusterLodOptions{},
                                               geometry::ClusterPagesOptions{}.page_bytes, data,
                                               nullptr, error),
                  error);
  CHECK(data.generator_kind == "broadleaf");
  // The species' two, and one a level of its impostor chain (flora.md, "The impostor chain").
  CHECK_FALSE(data.impostors.empty());
  CHECK(data.materials.size() == 2 + data.impostors.size());
  CHECK(data.motion_vertices.size() == data.mesh.mesh.vertices.size());
  CHECK_FALSE(data.motion_parts.empty());
  flora::FloraFile palm;
  REQUIRE(flora::read_flora_file("{}", palm, error));
  CHECK(flora::canonical_flora_json(palm).find("broadleaf") == std::string::npos);
  CHECK_FALSE(
      flora::read_flora_file("{\"species\":\"conifer\",\"conifer\":{\"whorls\":1}}", file, error));
  CHECK(error.find("whorls") != std::string::npos);
}

namespace {

// A pack of the test's own, in memory: every map of every role a path under `dir`.
materials::Pack stand_in_pack(std::initializer_list<const char*> roles, f32 tile_m) {
  std::string text = R"({"format":"engine.materials.pack/1","name":"stand-in","version":"1",)"
                     R"("roles":{)";
  bool first = true;
  for (const char* role : roles) {
    if (!first) text += ",";
    first = false;
    text += "\"" + std::string(role) + "\":{\"tile_m\":" + std::to_string(tile_m) +
            R"(,"normal_convention":"opengl","maps":{)";
    const char* maps[] = {"base_color", "normal", "roughness", "metallic", "height"};
    for (u32 i = 0; i < 5; ++i) {
      if (i > 0) text += ",";
      text += "\"" + std::string(maps[i]) + "\":{\"file\":\"" + role + "/" + maps[i] + ".png\"}";
    }
    text += "}}";
  }
  text += "}}";
  materials::Pack pack;
  std::string error;
  REQUIRE_MESSAGE(materials::parse_pack(text, "C:/stand-in/materials", pack, error), error);
  return pack;
}

f32 slot_scale(const geometry::ClusterFileData& data, u32 material, u32 slot) {
  geometry::TextureSlotSampling slots[geometry::k_material_slots];
  f32 occlusion = 0.0f;
  geometry::decode_material_sampling(data.material_sampling[material], slots, occlusion);
  return slots[slot].transform.scale.x;
}

const std::string& image_of(const geometry::ClusterFileData& data, i32 index) {
  REQUIRE(index >= 0);
  REQUIRE(static_cast<u32>(index) < data.image_paths.size());
  return data.image_paths[static_cast<u32>(index)];
}

}  // namespace

// The stand-in set (flora.md, "Materials"): a recipe that names images for the bark, a pack that
// has the frond's role, and nothing for the skirt — the three ways a part gets its material.
TEST_CASE("flora: each part takes the recipe's set, else the pack's role, else its flat colour") {
  flora::FloraFile file;
  std::string error;
  REQUIRE_MESSAGE(
      flora::read_flora_file(
          R"({"materials":{"bark":{"base_color":"sets/bark_c.png","normal":"sets/bark_n.png",)"
          R"("roughness":"sets/bark_r.png","tile_m":0.5,"normal_scale":0.4}}})",
          file, error),
      error);
  const materials::Pack pack = stand_in_pack({"palm/bark", "palm/frond"}, 0.25f);
  const geometry::ClusterLodOptions lod;
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(flora::build_flora_container(file, lod, page_bytes, data, nullptr, error, &pack),
                  error);
  REQUIRE(data.materials.size() == 3);
  // The bark: the recipe's images, as it spelled them (relative to the flora file), over the
  // pack's.
  CHECK(image_of(data, data.materials[0].base_color_image) == "sets/bark_c.png");
  CHECK(image_of(data, data.materials[0].normal_image) == "sets/bark_n.png");
  CHECK(image_of(data, geometry::decode_optional_image(
                           data.materials[0].metallic_roughness_image)) == "sets/bark_r.png");
  CHECK(data.materials[0].metallic == 0.0f);
  CHECK(data.materials[0].roughness == 1.0f);
  CHECK(data.materials[0].normal_scale == doctest::Approx(0.4f));
  CHECK(slot_scale(data, 0, geometry::k_slot_base_color) == doctest::Approx(2.0f));  // 1 / 0.5 m
  // The frond: the pack's, absolute, at the pack's repeat.
  CHECK(image_of(data, data.materials[1].base_color_image) ==
        "C:/stand-in/materials/palm/frond/base_color.png");
  CHECK(slot_scale(data, 1, geometry::k_slot_normal) == doctest::Approx(4.0f));  // 1 / 0.25 m
  // The skirt: no set anywhere, so the flat colour it always had.
  CHECK(data.materials[2].base_color_image == -1);
  CHECK(data.materials[2].normal_image == -1);
  CHECK(data.materials[2].base_color.x < 1.0f);
  // Five images, each with a texture record saying how it is built (section 32).
  CHECK(data.image_paths.size() == 6);
  CHECK(data.textures.size() == data.image_paths.size());
  // The pack is in the key, and a different pack is a different key; the source hash is the
  // file's alone, so a scene that names it by hash still finds it.
  CHECK(data.build_key == flora::flora_build_key(data.source_hash, lod, page_bytes, &pack));
  CHECK(data.build_key != flora::flora_build_key(data.source_hash, lod, page_bytes));
  CHECK(data.source_hash == flora::flora_source_hash(file));

  // Written and read back, the images and the transforms survive.
  engine::test::TempDir tmp("flora_materials");
  const std::string path = tmp.file("palm.clusters");
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, data, &error), error);
  geometry::ClusterFileData back;
  REQUIRE_MESSAGE(geometry::read_cluster_file(path, back, &error), error);
  CHECK(back.image_paths.size() == data.image_paths.size());
  CHECK(slot_scale(back, 1, geometry::k_slot_normal) == doctest::Approx(4.0f));

  // The broadleaf's leaf is a card: its image whole, no transform; its bark in metres.
  flora::FloraFile tree;
  REQUIRE(flora::read_flora_file(R"({"species":"broadleaf"})", tree, error));
  const materials::Pack tree_pack = stand_in_pack({"broadleaf/bark", "broadleaf/leaf"}, 0.5f);
  geometry::ClusterFileData oak;
  REQUIRE_MESSAGE(
      flora::build_flora_container(tree, lod, page_bytes, oak, nullptr, error, &tree_pack), error);
  CHECK(slot_scale(oak, 0, geometry::k_slot_base_color) == doctest::Approx(2.0f));
  CHECK(slot_scale(oak, 1, geometry::k_slot_base_color) == doctest::Approx(1.0f));
  CHECK(flora::flora_role("conifer", 1) == "conifer/needles");
  CHECK(flora::flora_role("date_palm", 2) == "palm/skirt");
  CHECK(flora::flora_role("birch", 0) == "birch/bark");

  // A set's numbers are checked when the file is read.
  CHECK_FALSE(flora::read_flora_file(R"({"materials":{"leaf":{"tile_m":0.01}}})", file, error));
  CHECK(error.find("materials.leaf.tile_m") != std::string::npos);
}

TEST_CASE("flora: with no block and no pack the palm's container names no image") {
  flora::FloraFile file;
  geometry::ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(flora::build_flora_container(file, geometry::ClusterLodOptions{},
                                               geometry::ClusterPagesOptions{}.page_bytes, data,
                                               nullptr, error),
                  error);
  CHECK(data.image_paths.empty());
  CHECK(data.textures.empty());
  for (const geometry::ClusterFileMaterial& m : data.materials) {
    CHECK(m.base_color_image == -1);
    CHECK(m.normal_image == -1);
  }
}
