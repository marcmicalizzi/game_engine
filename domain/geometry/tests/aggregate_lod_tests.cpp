// Aggregate geometry in the LOD DAG (geometry.md, "Loose islands: removed whole, the rest grown";
// roadmap T147): a mesh of parts that share no vertex -- a crown of leaf cards, a pile of stones --
// cannot be coarsened by collapsing edges, only by removing parts, and a level's error has to say
// what it removed.
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/stress_mesh.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

// A mesh of islands: `island_of_vertex` names each source vertex's part, `extent` each part's
// size (twice the farthest corner from the mean of its corners).
struct Islands {
  Vector<Vec3> positions;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  Vector<u32> island_of_vertex;
  Vector<f64> extent;
  f64 half = 0.0;  // the fixture lies inside [-half, half]^3
};

u64 mix(u64 x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

// A float in [0, 1) that is the same on every toolchain: 24 bits of the hash over 2^24.
f64 unit(u64 seed, u64 k) { return static_cast<f64>(mix(seed * 0x100000001b3ull + k) >> 40) / 16777216.0; }

struct V3 {
  f64 x = 0.0, y = 0.0, z = 0.0;
};
V3 sub(V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 cross(V3 a, V3 b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
f64 norm(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V3 scaled(V3 a, f64 s) { return V3{a.x * s, a.y * s, a.z * s}; }
V3 of(Vec3 p) { return V3{static_cast<f64>(p.x), static_cast<f64>(p.y), static_cast<f64>(p.z)}; }

// A direction from the hash, by rejection inside the unit ball (no trigonometry: a square root is
// the only library call, and it is correctly rounded).
V3 direction(u64 seed, u64& k) {
  for (;;) {
    const V3 v{unit(seed, k) * 2.0 - 1.0, unit(seed, k + 1) * 2.0 - 1.0,
               unit(seed, k + 2) * 2.0 - 1.0};
    k += 3;
    const f64 n = norm(v);
    if (n > 0.2 && n <= 1.0) return scaled(v, 1.0 / n);
  }
}

void close_island(Islands& out, u32 first_vertex) {
  const u32 island = out.extent.size();
  V3 mean;
  const u32 count = out.positions.size() - first_vertex;
  for (u32 v = first_vertex; v < out.positions.size(); ++v) {
    const V3 p = of(out.positions[v]);
    mean = V3{mean.x + p.x / count, mean.y + p.y / count, mean.z + p.z / count};
    out.island_of_vertex.push_back(island);
  }
  f64 r = 0.0;
  for (u32 v = first_vertex; v < out.positions.size(); ++v)
    r = std::max(r, norm(sub(of(out.positions[v]), mean)));
  out.extent.push_back(2.0 * r);
}

// A crown: `cards` square cards of 4 to 8 cm, each two triangles over four vertices of its own
// mapping the whole of one image, placed and turned by the hash inside a ball of `radius`.
void make_crown(u32 cards, f64 radius, u64 seed, Islands& out) {
  out = Islands{};
  out.half = radius + 0.1;
  u64 k = 0;
  for (u32 c = 0; c < cards; ++c) {
    V3 centre;
    for (;;) {
      centre = V3{unit(seed, k) * 2.0 - 1.0, unit(seed, k + 1) * 2.0 - 1.0,
                  unit(seed, k + 2) * 2.0 - 1.0};
      k += 3;
      if (norm(centre) <= 1.0) break;
    }
    centre = scaled(centre, radius);
    const V3 u = direction(seed, k);
    V3 v;
    for (;;) {
      v = cross(u, direction(seed, k));
      if (norm(v) > 0.2) break;
    }
    v = scaled(v, 1.0 / norm(v));
    const f64 half = 0.02 + 0.02 * unit(seed, k++);
    const u32 first = out.positions.size();
    const f64 su[4] = {-1.0, 1.0, 1.0, -1.0};
    const f64 sv[4] = {-1.0, -1.0, 1.0, 1.0};
    for (u32 q = 0; q < 4; ++q) {
      out.positions.push_back(Vec3{static_cast<f32>(centre.x + half * (su[q] * u.x + sv[q] * v.x)),
                                   static_cast<f32>(centre.y + half * (su[q] * u.y + sv[q] * v.y)),
                                   static_cast<f32>(centre.z + half * (su[q] * u.z + sv[q] * v.z))});
      out.uvs.push_back(Vec2{su[q] > 0.0 ? 1.0f : 0.0f, sv[q] > 0.0 ? 1.0f : 0.0f});
    }
    for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u})
      out.indices.push_back(first + i);
    close_island(out, first);
  }
}

// A pile: `stones` closed octahedra of 6 to 14 cm (eight triangles over six vertices each), opaque
// parts that share nothing, inside a ball of `radius`.
void make_pile(u32 stones, f64 radius, u64 seed, Islands& out) {
  out = Islands{};
  out.half = radius + 0.2;
  u64 k = 0;
  for (u32 s = 0; s < stones; ++s) {
    V3 centre;
    for (;;) {
      centre = V3{unit(seed, k) * 2.0 - 1.0, unit(seed, k + 1) * 2.0 - 1.0,
                  unit(seed, k + 2) * 2.0 - 1.0};
      k += 3;
      if (norm(centre) <= 1.0) break;
    }
    centre = scaled(centre, radius);
    const f64 r = 0.03 + 0.04 * unit(seed, k++);
    const u32 first = out.positions.size();
    const f64 axis[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (u32 q = 0; q < 6; ++q) {
      out.positions.push_back(Vec3{static_cast<f32>(centre.x + r * axis[q][0]),
                                   static_cast<f32>(centre.y + r * axis[q][1]),
                                   static_cast<f32>(centre.z + r * axis[q][2])});
      out.uvs.push_back(Vec2{static_cast<f32>(q) / 8.0f, 0.5f});
    }
    // Counter-clockwise seen from outside.
    const u32 faces[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                             {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (const auto& f : faces)
      for (const u32 i : f)
        out.indices.push_back(first + i);
    close_island(out, first);
  }
}

// What one cut draws, against the fixture.
struct CutMeasure {
  u32 triangles = 0;
  f64 area = 0.0;
  u32 islands_lost = 0;        // parts with no triangle of positive area left
  f64 largest_lost = 0.0;      // the largest such part's extent
  u32 covered[3] = {0, 0, 0};  // pixels of a 256 x 256 orthographic silhouette along x, y and z
};

constexpr u32 k_bitmap = 256;

void fill(Vector<u8>& bitmap, f64 ax, f64 ay, f64 bx, f64 by, f64 cx, f64 cy) {
  const f64 lo_x = std::min(ax, std::min(bx, cx)), hi_x = std::max(ax, std::max(bx, cx));
  const f64 lo_y = std::min(ay, std::min(by, cy)), hi_y = std::max(ay, std::max(by, cy));
  const f64 top = static_cast<f64>(k_bitmap) - 1.0;
  const u32 x0 = static_cast<u32>(std::clamp(std::floor(lo_x), 0.0, top));
  const u32 x1 = static_cast<u32>(std::clamp(std::floor(hi_x), 0.0, top));
  const u32 y0 = static_cast<u32>(std::clamp(std::floor(lo_y), 0.0, top));
  const u32 y1 = static_cast<u32>(std::clamp(std::floor(hi_y), 0.0, top));
  for (u32 y = y0; y <= y1; ++y) {
    for (u32 x = x0; x <= x1; ++x) {
      const f64 px = static_cast<f64>(x) + 0.5, py = static_cast<f64>(y) + 0.5;
      const f64 e0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
      const f64 e1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
      const f64 e2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
      if ((e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0) || (e0 <= 0.0 && e1 <= 0.0 && e2 <= 0.0))
        bitmap[y * k_bitmap + x] = 1;
    }
  }
}

CutMeasure measure(const Islands& islands, const ClusterLodMesh& lod, std::span<const u32> cut) {
  CutMeasure out;
  Vector<u8> alive(islands.extent.size(), u8{0});
  Vector<u8> bitmap[3];
  for (auto& b : bitmap)
    b.resize(k_bitmap * k_bitmap, u8{0});
  const f64 to_px = static_cast<f64>(k_bitmap) / (2.0 * islands.half);
  for (const u32 c : cut) {
    const ClusterDesc& desc = lod.mesh.clusters[c];
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const u32 packed = lod.mesh.triangles[desc.triangle_offset + t];
      V3 p[3];
      u32 island = 0;
      for (u32 k = 0; k < 3; ++k) {
        const u32 v = desc.vertex_offset + ClusterMesh::unpack(packed, k);
        p[k] = of(lod.mesh.vertices[v]);
        island = islands.island_of_vertex[lod.mesh.vertex_source[v]];
      }
      const f64 area = 0.5 * norm(cross(sub(p[1], p[0]), sub(p[2], p[0])));
      ++out.triangles;
      out.area += area;
      if (area > 0.0) alive[island] = 1;
      const f64 q[3][3] = {{(p[0].x + islands.half) * to_px, (p[0].y + islands.half) * to_px,
                            (p[0].z + islands.half) * to_px},
                           {(p[1].x + islands.half) * to_px, (p[1].y + islands.half) * to_px,
                            (p[1].z + islands.half) * to_px},
                           {(p[2].x + islands.half) * to_px, (p[2].y + islands.half) * to_px,
                            (p[2].z + islands.half) * to_px}};
      fill(bitmap[0], q[0][1], q[0][2], q[1][1], q[1][2], q[2][1], q[2][2]);
      fill(bitmap[1], q[0][0], q[0][2], q[1][0], q[1][2], q[2][0], q[2][2]);
      fill(bitmap[2], q[0][0], q[0][1], q[1][0], q[1][1], q[2][0], q[2][1]);
    }
  }
  for (u32 i = 0; i < alive.size(); ++i) {
    if (alive[i] != 0) continue;
    ++out.islands_lost;
    out.largest_lost = std::max(out.largest_lost, islands.extent[i]);
  }
  for (u32 k = 0; k < 3; ++k)
    for (const u8 b : bitmap[k])
      out.covered[k] += b;
  return out;
}

f64 silhouette(const CutMeasure& cut, const CutMeasure& finest) {
  f64 worst = 1e300;
  for (u32 k = 0; k < 3; ++k)
    worst = std::min(worst, static_cast<f64>(cut.covered[k]) / static_cast<f64>(finest.covered[k]));
  return worst;
}

// `loose_islands` false is the builder before the rule, byte for byte: the defect's own build.
bool build(const Islands& islands, ClusterLodMesh& lod, bool loose_islands = true) {
  AttributeSource attributes;
  attributes.uvs = std::span<const Vec2>(islands.uvs.data(), islands.uvs.size());
  std::string error;
  ClusterLodOptions options;
  options.normal_cones = false;  // two-sided cards
  options.loose_islands = loose_islands;
  const bool ok =
      build_cluster_lod(islands.positions, islands.indices, options, lod, &error, attributes);
  if (!ok) MESSAGE(error);
  return ok;
}

// The finite errors a mesh's transitions carry, ascending and distinct: the raw thresholds at
// which its cut changes.
Vector<f32> transition_errors(const ClusterLodMesh& lod) {
  Vector<f32> errors;
  for (const ClusterLodDesc& d : lod.lod)
    if (d.parent_error < k_lod_terminal_error) errors.push_back(d.parent_error);
  std::sort(errors.begin(), errors.end());
  Vector<f32> distinct;
  for (const f32 e : errors)
    if (distinct.empty() || distinct.back() != e) distinct.push_back(e);
  return distinct;
}

// 1080p at a 60 degree field of view: cot(30 deg) * 540.
constexpr f32 k_proj_1080p = 935.3074f;

struct Honesty {
  f64 worst_overstep = 0.0;    // the largest (part lost) / (the cut's own error), over every cut
  f64 least_area = 1.0;        // the least drawn area over level 0's, over every cut
  f64 least_silhouette = 1.0;  // the least silhouette coverage over level 0's
};

// Every cut the mesh has, by raw threshold: what it lost against what its error admits to.
Honesty sweep(const Islands& islands, const ClusterLodMesh& lod, const CutMeasure& finest) {
  Honesty out;
  Vector<u32> cut;
  for (const f32 e : transition_errors(lod)) {
    cut.clear();
    select_lod_raw(lod, e, cut);
    const CutMeasure m = measure(islands, lod, cut);
    if (m.islands_lost != 0)
      out.worst_overstep = std::max(out.worst_overstep, m.largest_lost / static_cast<f64>(e));
    out.least_area = std::min(out.least_area, m.area / finest.area);
    out.least_silhouette = std::min(out.least_silhouette, silhouette(m, finest));
  }
  return out;
}

void report(const char* name, const Islands& islands, const ClusterLodMesh& lod,
            const CutMeasure& finest) {
  char line[256];
  std::snprintf(line, sizeof line, "%s: %u parts, %u triangles, %u clusters in %u levels", name,
                islands.extent.size(), finest.triangles, lod.mesh.clusters.size(),
                lod.level_cluster_counts.size());
  MESSAGE(line);
  std::snprintf(line, sizeof line,
                "  build: %u groups, %u regular, %u by islands (%u removed, %u grown), %u sloppy, "
                "%u errors raised to a removed part's extent, %u vertices added",
                lod.aggregate.groups, lod.aggregate.regular, lod.aggregate.by_islands,
                lod.aggregate.islands_removed, lod.aggregate.islands_grown, lod.aggregate.sloppy,
                lod.aggregate.errors_raised, lod.aggregate.vertices_added);
  MESSAGE(line);
  Vector<u32> cut;
  for (const f32 distance : {3.0f, 10.0f, 30.0f, 100.0f, 300.0f}) {
    LodView view;
    view.camera = Vec3{0.0f, 0.0f, distance};
    view.proj_scale = k_proj_1080p;
    view.threshold_px = 1.0f;
    cut.clear();
    select_lod(lod, view, cut);
    const CutMeasure m = measure(islands, lod, cut);
    std::snprintf(line, sizeof line,
                  "  %5.0f m: %6u triangles, area %5.1f %%, silhouette %5.1f %%, %4u parts gone, "
                  "the largest %.1f px across",
                  static_cast<f64>(distance), m.triangles, 100.0 * m.area / finest.area,
                  100.0 * silhouette(m, finest), m.islands_lost,
                  m.largest_lost * static_cast<f64>(k_proj_1080p) / static_cast<f64>(distance));
    MESSAGE(line);
  }
}

}  // namespace

TEST_CASE("aggregate lod: a crown of loose cards keeps its covered area at every cut") {
  Islands crown;
  make_crown(2400, 1.5, 7, crown);
  ClusterLodMesh lod;
  REQUIRE(build(crown, lod));
  std::string error;
  CHECK_MESSAGE(validate_cluster_lod(lod, crown.indices, &error), error);

  Vector<u32> leaves;
  select_lod_raw(lod, 0.0f, leaves);
  const CutMeasure finest = measure(crown, lod, leaves);
  REQUIRE(finest.triangles == crown.indices.size() / 3);
  REQUIRE(finest.islands_lost == 0);
  report("crown", crown, lod, finest);

  const Honesty h = sweep(crown, lod, finest);
  char line[200];
  std::snprintf(line, sizeof line,
                "crown, every cut: a lost card at most %.2f of the cut's error; area at least "
                "%.1f %%, silhouette at least %.1f %% of the finest",
                h.worst_overstep, 100.0 * h.least_area, 100.0 * h.least_silhouette);
  MESSAGE(line);
  // A cut's error is at least the extent of every card it no longer draws: drawn at one pixel of
  // error, nothing wider than a pixel is missing.
  CHECK(h.worst_overstep <= 1.0 + 1e-5);
  // A removed card's area goes to a kept neighbour, so the drawn area holds: exactly but for the
  // groups that end their chain, and a pair's float rounding. The silhouette is the union's, and
  // fewer, larger cards overlap as often as many small ones of the same area (a Boolean model's
  // covered share depends on the area alone), so it holds to the noise of the placement.
  CHECK(h.least_area >= 0.95);
  CHECK(h.least_silhouette >= 0.85);
  // And the DAG still coarsens: the coarsest cut is a small part of the crown.
  Vector<u32> root;
  select_lod_raw(lod, transition_errors(lod).back(), root);
  CHECK(measure(crown, lod, root).triangles * 8 <= finest.triangles);

  // The defect, so the fixture is known to see it. The builder before the rule never reaches the
  // sloppy fallback on these cards: the regular simplifier collapses each card's two triangles to
  // one and then to none, reports about half the card's extent for it, and gives the area to
  // nothing, so past the distance at which a card is two pixels wide the crown is 18 triangles.
  ClusterLodMesh before;
  REQUIRE(build(crown, before, false));
  report("crown, before the rule", crown, before, finest);
  const Honesty b = sweep(crown, before, finest);
  std::snprintf(line, sizeof line,
                "crown before the rule, every cut: a lost card up to %.1f times the cut's error; "
                "area down to %.1f %%, silhouette down to %.1f %% of the finest",
                b.worst_overstep, 100.0 * b.least_area, 100.0 * b.least_silhouette);
  MESSAGE(line);
  CHECK(b.worst_overstep > 1.5);
  CHECK(b.least_area < 0.05);
  CHECK(b.least_silhouette < 0.05);
  CHECK(before.aggregate.regular == before.aggregate.groups);
  CHECK(before.aggregate.sloppy == 0);
  CHECK(before.aggregate.by_islands == 0);
  CHECK(before.aggregate.vertices_added == 0);
}

TEST_CASE("aggregate lod: a connected surface is built as it was") {
  // The shredded-atlas torus: 9,216 triangles in 288 UV islands whose borders are doubled
  // vertices, so it is a mesh of many islands to its atlas and one connected surface to the
  // builder, which joins triangles by position. The rule has nothing to say about it.
  ShreddedAtlasOptions shape;
  shape.segments = 96;
  shape.rings = 48;
  shape.island_side = 4;
  ShreddedAtlasMesh torus;
  build_shredded_atlas_torus(shape, torus);
  AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(torus.normals.data(), torus.normals.size());
  attributes.uvs = std::span<const Vec2>(torus.uvs.data(), torus.uvs.size());
  ClusterLodOptions options;
  ClusterLodMesh with, without;
  std::string error;
  REQUIRE_MESSAGE(
      build_cluster_lod(torus.positions, torus.indices, options, with, &error, attributes), error);
  options.loose_islands = false;
  REQUIRE_MESSAGE(
      build_cluster_lod(torus.positions, torus.indices, options, without, &error, attributes),
      error);
  const auto equal = [](const auto& x, const auto& y) {
    return x.size() == y.size() &&
           (x.size() == 0 || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
  };
  CHECK(equal(with.mesh.clusters, without.mesh.clusters));
  CHECK(equal(with.lod, without.lod));
  CHECK(equal(with.mesh.vertices, without.mesh.vertices));
  CHECK(equal(with.mesh.vertex_source, without.mesh.vertex_source));
  CHECK(equal(with.mesh.triangles, without.mesh.triangles));
  CHECK(equal(with.mesh.quantized, without.mesh.quantized));
  CHECK(with.aggregate.groups == without.aggregate.groups);
  CHECK(with.aggregate.by_islands == 0);
  CHECK(with.aggregate.islands_restored == 0);
  CHECK(with.aggregate.errors_raised == 0);
  CHECK(with.aggregate.vertices_added == 0);
}

TEST_CASE("aggregate lod: a pile of separate stones loses none wider than its error") {
  Islands pile;
  make_pile(1200, 2.0, 11, pile);
  ClusterLodMesh lod;
  REQUIRE(build(pile, lod));
  std::string error;
  CHECK_MESSAGE(validate_cluster_lod(lod, pile.indices, &error), error);

  Vector<u32> leaves;
  select_lod_raw(lod, 0.0f, leaves);
  const CutMeasure finest = measure(pile, lod, leaves);
  REQUIRE(finest.triangles == pile.indices.size() / 3);
  report("pile", pile, lod, finest);

  const Honesty h = sweep(pile, lod, finest);
  char line[200];
  std::snprintf(line, sizeof line,
                "pile, every cut: a lost stone at most %.2f of the cut's error; area at least "
                "%.1f %%, silhouette at least %.1f %% of the finest",
                h.worst_overstep, 100.0 * h.least_area, 100.0 * h.least_silhouette);
  MESSAGE(line);
  CHECK(h.worst_overstep <= 1.0 + 1e-5);
  CHECK(h.least_silhouette >= 0.85);
}
