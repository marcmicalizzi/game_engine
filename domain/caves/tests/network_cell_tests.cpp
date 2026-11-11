// A cave network's cells (network.h; docs/subsystems/caves.md, "Cells"; roadmap G146): the
// network's surface is one closed surface cut into cells — closed but for the aprons' rims, two
// cells sharing every vertex of the ring they meet at, wound toward the air — a walker's capsule
// passes every edge the plan calls walkable, a cell is the same bytes every time (and pinned), its
// dressing hangs from rock and stands on a floor, and its container is built through the registry.
#include "network_test_support.h"

#include <core/hash/hash.h>
#include <domain/assets/mesh_generator.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

using namespace engine;
using namespace engine::caves;

namespace {

u64 mesh_hash(const assets::MeshData& m) {
  u64 h = hash_bytes(m.positions.data(), m.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(m.normals.data(), m.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(m.uvs.data(), m.uvs.size() * sizeof(Vec2)));
  return hash_combine(h, hash_bytes(m.indices.data(), m.indices.size() * sizeof(u32)));
}

Plan planned(Network n, const caves::test::Hill& hill, f32 voxel_m) {
  n.voxel_m = voxel_m;
  Plan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_network(n, caves::test::ground_of(hill), 32000, plan, error), error);
  return plan;
}

struct Solved {
  assets::MeshData mesh;
  CellReport report;
};

Solved solved(const Plan& plan, u32 cell, u32 part = 0) {
  Solved s;
  CellRecipe recipe;
  cell_recipe(plan, cell, part, recipe);
  std::string error;
  REQUIRE_MESSAGE(solve_cell(recipe, s.mesh, &s.report, error), error);
  return s;
}

// A vertex's place in the world, in 1/1024 m from the plan's anchor: exact, so two cells' copies
// of one vertex are one key.
struct Key {
  i64 x, y, z;
  bool operator<(const Key& o) const { return x != o.x ? x < o.x : (y != o.y ? y < o.y : z < o.z); }
};
Key key_of(const Plan& plan, const CellReport& report, Vec3 local) {
  const auto unit = [&](i64 origin_mm, i64 anchor_mm, f32 v) {
    return (origin_mm - anchor_mm) * 1024 / 1000 +
           static_cast<i64>(std::llround(static_cast<f64>(v) * 1024.0));
  };
  return Key{unit(report.origin[0], plan.anchor[0], local.x),
             unit(report.origin[1], plan.anchor[1], local.y),
             unit(report.origin[2], plan.anchor[2], local.z)};
}

// Ray (o, d) against triangle (a, b, c): the distance, or a negative number for a miss.
f64 ray_hit(const f64 o[3], const f64 d[3], Vec3 a, Vec3 b, Vec3 c) {
  const f64 e1[3] = {static_cast<f64>(b.x - a.x), static_cast<f64>(b.y - a.y),
                     static_cast<f64>(b.z - a.z)};
  const f64 e2[3] = {static_cast<f64>(c.x - a.x), static_cast<f64>(c.y - a.y),
                     static_cast<f64>(c.z - a.z)};
  const f64 p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                    d[0] * e2[1] - d[1] * e2[0]};
  const f64 det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::fabs(det) < 1e-12) return -1.0;
  const f64 t[3] = {o[0] - static_cast<f64>(a.x), o[1] - static_cast<f64>(a.y),
                    o[2] - static_cast<f64>(a.z)};
  const f64 u = (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]) / det;
  if (u < 0.0 || u > 1.0) return -1.0;
  const f64 q[3] = {t[1] * e1[2] - t[2] * e1[1], t[2] * e1[0] - t[0] * e1[2],
                    t[0] * e1[1] - t[1] * e1[0]};
  const f64 v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
  if (v < 0.0 || u + v > 1.0) return -1.0;
  return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
}

// The nearest hit of a ray given in the world's millimetres against a cell's mesh, metres.
f64 nearest_hit(const Solved& s, f64 ox, f64 oy, f64 oz, f64 dx, f64 dy, f64 dz) {
  const f64 o[3] = {ox - static_cast<f64>(s.report.origin[0]) / 1000.0,
                    oy - static_cast<f64>(s.report.origin[1]) / 1000.0,
                    oz - static_cast<f64>(s.report.origin[2]) / 1000.0};
  const f64 d[3] = {dx, dy, dz};
  f64 best = 1e30;
  for (u32 i = 0; i + 2 < s.mesh.indices.size(); i += 3) {
    const f64 t =
        ray_hit(o, d, s.mesh.positions[s.mesh.indices[i]], s.mesh.positions[s.mesh.indices[i + 1]],
                s.mesh.positions[s.mesh.indices[i + 2]]);
    if (t > 1e-6 && t < best) best = t;
  }
  return best;
}

}  // namespace

TEST_CASE("caves: the network is one closed surface, and two cells meet at a portal with no gap") {
  // Half-metre lattice: every cell of the network is solved here, and in a debug build the solve
  // is most of this case's seconds.
  const caves::test::Hill hill;
  // A smaller network than the other cases' (110 m of strike, 45 m of dip), because every cell
  // of it is solved here and in a debug build a cell is half a second.
  Network small = caves::test::hill_network();
  small.reach_strike_m = 110.0f;
  small.reach_dip_m = 45.0f;
  const Plan plan = planned(small, hill, 0.5f);
  const PlanField field(plan);
  std::map<Key, u32> ids;
  std::vector<Key> where;
  std::vector<u32> first_cell, second_cell;
  std::map<std::pair<u32, u32>, u32> sides;
  u64 triangles = 0, class_triangles[5] = {};
  u32 on_border = 0, wrong_way = 0, faces = 0;
  for (u32 cell = 0; cell < plan.cells.size(); ++cell) {
    if (plan.cells[cell].flooded) continue;
    const Solved s = solved(plan, cell);
    triangles += s.report.triangles;
    for (u32 k = 0; k < 5; ++k)
      class_triangles[k] += s.report.class_triangles[k];
    // The bounds hold the surface: nothing a cell owns is on its lattice's edge.
    const PlanCell& c = plan.cells[cell];
    const f32 step = 0.5f;
    for (const Vec3 p : s.mesh.positions) {
      if (p.x < step || p.y < step || p.z < step ||
          p.x > static_cast<f32>(c.hi[0] - c.lo[0] - 1) * step ||
          p.y > static_cast<f32>(c.hi[1] - c.lo[1] - 1) * step ||
          p.z > static_cast<f32>(c.hi[2] - c.lo[2] - 1) * step) {
        if (on_border < 4)
          MESSAGE("cell " << cell << (c.mouth ? " (mouth)" : "") << (c.hall ? " (hall)" : "")
                          << " has a vertex on its bounds' edge: " << p.x << ", " << p.y << ", "
                          << p.z << " of " << (c.hi[0] - c.lo[0]) * 0.5 << ", "
                          << (c.hi[1] - c.lo[1]) * 0.5 << ", " << (c.hi[2] - c.lo[2]) * 0.5);
        ++on_border;
      }
    }
    std::vector<u32> id(s.mesh.positions.size());
    for (u32 v = 0; v < s.mesh.positions.size(); ++v) {
      const Key k = key_of(plan, s.report, s.mesh.positions[v]);
      const auto found = ids.emplace(k, static_cast<u32>(ids.size()));
      id[v] = found.first->second;
      if (found.second) {
        where.push_back(k);
        first_cell.push_back(cell);
        second_cell.push_back(cell);
      } else if (first_cell[id[v]] != cell) {
        second_cell[id[v]] = cell;
      }
    }
    for (const assets::Primitive& primitive : s.mesh.primitives) {
      for (u32 i = 0; i + 2 < primitive.index_count; i += 3) {
        const u32* t = s.mesh.indices.data() + primitive.first_index + i;
        for (u32 k = 0; k < 3; ++k) {
          const u32 a = id[t[k]], b = id[t[(k + 1) % 3]];
          ++sides[{std::min(a, b), std::max(a, b)}];
        }
        // Wound toward the air: a step off the front of a wall or a floor is in the void, a step
        // behind it in the rock (a sample of the triangles; the apron faces the sky).
        if (primitive.material == 4 || (i / 3) % 53 != 0) continue;
        const Vec3 a = s.mesh.positions[t[0]], b = s.mesh.positions[t[1]],
                   c2 = s.mesh.positions[t[2]];
        const Vec3 n = normalize(cross(b - a, c2 - a));
        const Vec3 mid = (a + b + c2) * (1.0f / 3.0f);
        // At a mouth the air in front of a face is the sky's, which the plan's field does not
        // hold: those faces are the open-edge check's.
        const i64 mid_x = s.report.origin[0] + std::llround(static_cast<f64>(mid.x) * 1000.0);
        const i64 mid_z = s.report.origin[2] + std::llround(static_cast<f64>(mid.z) * 1000.0);
        if (static_cast<f64>(s.report.origin[1]) / 1000.0 + static_cast<f64>(mid.y) >
            caves::test::hill_height(&hill, mid_x, mid_z) - 1.0)
          continue;
        const auto at = [&](f32 off) {
          return field.void_at(
              s.report.origin[0] + std::llround(static_cast<f64>(mid.x + n.x * off) * 1000.0),
              s.report.origin[1] + std::llround(static_cast<f64>(mid.y + n.y * off) * 1000.0),
              s.report.origin[2] + std::llround(static_cast<f64>(mid.z + n.z * off) * 1000.0));
        };
        ++faces;
        // (The field falls from behind the face to in front of it: at this lattice a vertex
        // stands up to two tenths of a metre off the field's zero, so the signs are not asked.)
        if (!(at(0.2f) < at(-0.2f))) {
          if (wrong_way < 6)
            MESSAGE("a face of class " << primitive.material << " in cell " << cell
                                       << ": the field " << at(0.2f) << " in front, " << at(-0.2f)
                                       << " behind, " << at(0.0f) << " on it; normal (" << n.x
                                       << ", " << n.y << ", " << n.z << ")");
          ++wrong_way;
        }
      }
    }
  }
  MESSAGE("the surface at 0.5 m: "
          << triangles << " triangles in " << plan.cells.size() << " cells, "
          << static_cast<f64>(triangles) / static_cast<f64>(plan.report.passage_m)
          << " a metre of passage; rock " << class_triangles[0] << ", sediment "
          << class_triangles[1] << ", breakdown " << class_triangles[2] << ", water "
          << class_triangles[3] << ", apron " << class_triangles[4]);
  CHECK(on_border == 0);
  // Closed: every edge of the whole network's surface has two triangles, but an apron's rim,
  // which lies on the ground at a mouth.
  u32 open = 0, crowded = 0, odd = 0, open_off_ground = 0;
  for (const auto& side : sides) {
    if (side.second == 2) continue;
    if (side.second != 1) {
      // Two sheets of the surface through one lattice cube share its one vertex: the edge has
      // four triangles and the surface is still closed. An odd count would be a hole.
      ++crowded;
      if (side.second % 2 != 0) ++odd;
      continue;
    }
    ++open;
    const Key k = where[side.first.first];
    const i64 x_mm = plan.anchor[0] + k.x * 1000 / 1024, z_mm = plan.anchor[2] + k.z * 1000 / 1024;
    const f64 y = static_cast<f64>(plan.anchor[1]) / 1000.0 + static_cast<f64>(k.y) / 1024.0;
    if (std::fabs(y - caves::test::hill_height(&hill, x_mm, z_mm)) > 0.6) {
      if (open_off_ground < 5)
        MESSAGE("an open edge at (" << x_mm / 1000.0 << ", " << y << ", " << z_mm / 1000.0
                                    << "), the ground there "
                                    << caves::test::hill_height(&hill, x_mm, z_mm) << ", cell "
                                    << first_cell[side.first.first]);
      ++open_off_ground;
    }
  }
  MESSAGE(sides.size() << " edges: " << open << " open (the aprons' rims), " << crowded
                       << " with more than two triangles; " << faces << " faces sampled, "
                       << wrong_way << " wound the wrong way");
  CHECK(open > 0);
  CHECK(open_off_ground == 0);
  CHECK(odd == 0);
  CHECK(crowded * 500 < sides.size());  // pinches: under a fifth of a percent of the edges
  // A face on a lip thinner than the lattice (a scallop's crest, a step between two floors) can
  // read either way by a few centimetres: under half a percent of the faces, counted above.
  CHECK(wrong_way * 200 <= faces);
  // Two cells that share a vertex share a ring of them, and the plan has a portal for every such
  // pair: a host that loads by what a portal shows misses no cell. (A portal the plan lists whose
  // cells do not touch costs a load and no more; they are counted.)
  std::map<std::pair<u32, u32>, u32> meeting;
  for (u32 v = 0; v < first_cell.size(); ++v)
    if (first_cell[v] != second_cell[v])
      ++meeting[{std::min(first_cell[v], second_cell[v]), std::max(first_cell[v], second_cell[v])}];
  u32 idle = 0;
  for (const PlanPortal& p : plan.portals) {
    if (p.cell_b == k_outside) continue;
    if (meeting.find({std::min(p.cell_a, p.cell_b), std::max(p.cell_a, p.cell_b)}) == meeting.end())
      ++idle;
  }
  for (const auto& pair : meeting) {
    bool listed = false;
    for (const PlanPortal& p : plan.portals)
      listed = listed || (std::min(p.cell_a, p.cell_b) == pair.first.first &&
                          std::max(p.cell_a, p.cell_b) == pair.first.second);
    CHECK_MESSAGE(listed, "cells " << pair.first.first << " and " << pair.first.second << " share "
                                   << pair.second << " vertices and no portal");
  }
  MESSAGE(meeting.size() << " pairs of cells meet; " << plan.portals.size() << " portals, " << idle
                         << " of them between cells that do not touch");
}

TEST_CASE("caves: a walker's capsule passes every edge the plan calls walkable") {
  const caves::test::Hill hill;
  const Network n = caves::test::hill_network();
  const Plan plan = planned(n, hill, 0.25f);
  const PlanField field(plan);
  const f64 radius = static_cast<f64>(n.walker_radius_m),
            tall = static_cast<f64>(n.walker_height_m);
  // In the field the surface is the zero of: at every station of a walkable edge the capsule's two
  // spheres stand clear of the rock, the floor is under its feet, and the roof over its head.
  u32 stations = 0, blocked = 0;
  for (const PlanEdge& e : plan.edges) {
    if (!e.walkable) continue;
    for (u32 j = 0; j < e.count; j += 2) {
      const CellStation& s = plan.stations[e.first + j].station;
      ++stations;
      // The floor under the centreline: the last void going down.
      i64 feet = s.floor + 600;
      while (feet > s.floor - 600 && field.void_at(s.x, feet - 50, s.z) < 0.0)
        feet -= 50;
      // The capsule stands where its lower sphere first fits, within a step's height (0.4 m) of
      // the lowest point under the centreline: a floor is not a plane.
      bool lower = false, upper = false, head = false;
      for (i64 step_up = 0; step_up <= 400 && !(lower && upper && head); step_up += 100) {
        const i64 stand = feet + step_up;
        lower = field.void_at(s.x, stand + std::llround(radius * 1000.0) + 50, s.z) < -radius + 0.1;
        upper =
            field.void_at(s.x, stand + std::llround((tall - radius) * 1000.0), s.z) < -radius + 0.1;
        head = field.void_at(s.x, stand + std::llround(tall * 1000.0), s.z) < 0.0;
      }
      if (!(lower && upper && head)) {
        ++blocked;
        MESSAGE("blocked on a " << std::string(strand_name(e.kind)) << " at station " << j << " of "
                                << e.count << " of edge " << s.edge << ": " << lower << upper
                                << head << ", feet " << (feet - s.floor)
                                << " mm from the plan's floor, void "
                                << field.void_at(s.x, feet + 350, s.z) << " "
                                << field.void_at(s.x, feet + 1500, s.z) << " "
                                << field.void_at(s.x, feet + 1800, s.z) << ", section a " << s.a
                                << " b " << s.b << " hw " << s.slot_hw << " top " << s.slot_top
                                << " bot " << s.slot_bot);
      }
    }
  }
  CHECK(stations > 100);
  CHECK(blocked == 0);
  // And in the mesh of one cell at the quarter-metre lattice: rays from a walker's waist, down to
  // the floor, up to the roof, and to both sides.
  u32 cell = 0;
  const PlanEdge* route = nullptr;
  for (const PlanEdge& e : plan.edges)
    if (e.walkable && e.kind == StrandKind::trunk && e.count > 20 && route == nullptr) route = &e;
  REQUIRE(route != nullptr);
  cell = plan.stations[route->first + route->count / 2].station.cell;
  const Solved s = solved(plan, cell);
  MESSAGE("cell " << cell << " at 0.25 m: " << s.report.triangles << " triangles for "
                  << plan.cells[cell].passage_m << " m of passage, " << s.report.lattice_points
                  << " lattice points");
  u32 walked = 0;
  for (u32 j = 2; j + 2 < route->count; j += 3) {
    const CellStation& st = plan.stations[route->first + j].station;
    if (st.cell != cell || plan.stations[route->first + j - 2].station.cell != cell ||
        plan.stations[route->first + j + 2].station.cell != cell)
      continue;
    const f64 x = static_cast<f64>(st.x) / 1000.0, z = static_cast<f64>(st.z) / 1000.0;
    const f64 waist = static_cast<f64>(st.floor) / 1000.0 + 1.0;
    const f64 down = nearest_hit(s, x, waist, z, 0.0, -1.0, 0.0);
    const f64 up = nearest_hit(s, x, waist, z, 0.0, 1.0, 0.0);
    // Across the passage: level, square to the tangent.
    const f64 tx = static_cast<f64>(st.tx), tz = static_cast<f64>(st.tz);
    const f64 len = std::sqrt(tx * tx + tz * tz);
    const f64 left = nearest_hit(s, x, waist, z, -tz / len, 0.0, tx / len);
    const f64 right = nearest_hit(s, x, waist, z, tz / len, 0.0, -tx / len);
    CHECK(down < 1.3);
    CHECK(down + up >= tall);
    CHECK(left >= radius);
    CHECK(right >= radius);
    ++walked;
  }
  CHECK(walked >= 3);
}

TEST_CASE("caves: a cell is the same bytes every time, and pinned") {
  const caves::test::Hill hill;
  const Plan plan = planned(caves::test::hill_network(), hill, 0.5f);
  // A trunk's cell, a cell with a mouth, and the hall.
  u32 mouth = 0, hall = 0;
  for (const PlanPortal& p : plan.portals)
    if (p.cell_b == k_outside && mouth == 0) mouth = p.cell_a;
  for (u32 c = 0; c < plan.cells.size(); ++c)
    if (plan.cells[c].hall) hall = c;
  REQUIRE(mouth != 0);
  REQUIRE(hall != 0);
  u32 plain = 0;
  for (u32 c = 0; c < plan.cells.size(); ++c)
    if (!plan.cells[c].mouth && !plan.cells[c].hall && !plan.cells[c].flooded &&
        plan.cells[c].passage_m > 12.0f && plain == 0)
      plain = c;
  const u32 cells[3] = {plain, mouth, hall};
  u64 hashes[3] = {};
  u32 apron = 0, breakdown = 0;
  for (u32 k = 0; k < 3; ++k) {
    const Solved a = solved(plan, cells[k]);
    hashes[k] = mesh_hash(a.mesh);
    if (k == 0) CHECK(hashes[k] == mesh_hash(solved(plan, cells[k]).mesh));
    if (k == 1) apron = a.report.class_triangles[4];
    if (k == 2) breakdown = a.report.class_triangles[2];
    MESSAGE("cell " << cells[k] << ": " << a.report.triangles << " triangles, hash " << hashes[k]);
  }
  CHECK(apron > 0);      // the mouth's apron
  CHECK(breakdown > 0);  // the hall's breakdown
  // Taken on MSVC; Clang and GCC in the containers reproduce them (the solver rules, 1).
  CHECK(hashes[0] == 2346836310554222384ull);
  CHECK(hashes[1] == 16880174233671219797ull);
  CHECK(hashes[2] == 18039883821311708033ull);
  // A recipe says everything: read back from its text it is the same cell.
  CellRecipe recipe, back;
  cell_recipe(plan, mouth, 0, recipe);
  std::string error;
  REQUIRE_MESSAGE(read_cell_recipe(cell_recipe_json(recipe), back, error), error);
  CHECK(cell_source_hash(back) == cell_source_hash(recipe));
  assets::MeshData again;
  REQUIRE(solve_cell(back, again, nullptr, error));
  CHECK(mesh_hash(again) == hashes[1]);
  back.voxel_mm = 300;
  CHECK_FALSE(read_cell_recipe(cell_recipe_json(back), back, error));
}

TEST_CASE("caves: a cell's dressing hangs from the rock in rows, within its budget") {
  const caves::test::Hill hill;
  const Plan plan = planned(caves::test::hill_network(), hill, 0.5f);
  u32 drips = 0, stalactites = 0, stalagmites = 0, columns = 0, flowstones = 0, triangles = 0;
  u32 dressed = 0;
  for (u32 cell = 0; cell < plan.cells.size(); ++cell) {
    if (plan.cells[cell].flooded) continue;
    const Solved s = solved(plan, cell, 1);
    drips += s.report.drips;
    stalactites += s.report.stalactites;
    stalagmites += s.report.stalagmites;
    columns += s.report.columns;
    flowstones += s.report.flowstones;
    triangles += s.report.triangles;
    CHECK(s.report.triangles <= Speleothems{}.triangle_budget + 40u * Speleothems{}.sides);
    if (s.report.triangles == 0) continue;
    ++dressed;
    if (dressed != 1) continue;
    // Closed bodies: welded by position, every edge has two triangles; and the same bytes twice.
    std::map<std::tuple<f32, f32, f32>, u32> ids;
    std::vector<u32> id(s.mesh.positions.size());
    for (u32 v = 0; v < s.mesh.positions.size(); ++v) {
      const Vec3 p = s.mesh.positions[v];
      id[v] =
          ids.emplace(std::make_tuple(p.x, p.y, p.z), static_cast<u32>(ids.size())).first->second;
    }
    std::map<std::pair<u32, u32>, u32> sides;
    for (u32 i = 0; i + 2 < s.mesh.indices.size(); i += 3)
      for (u32 k = 0; k < 3; ++k) {
        const u32 a = id[s.mesh.indices[i + k]], b = id[s.mesh.indices[i + (k + 1) % 3]];
        if (a != b) ++sides[{std::min(a, b), std::max(a, b)}];
      }
    u32 odd = 0;
    for (const auto& side : sides)
      if (side.second % 2 != 0) ++odd;
    CHECK(odd == 0);
    CHECK(mesh_hash(s.mesh) == mesh_hash(solved(plan, cell, 1).mesh));
  }
  MESSAGE("the dressing: " << drips << " drips in " << dressed << " cells: " << stalactites
                           << " stalactites and straws, " << stalagmites << " stalagmites, "
                           << columns << " columns, " << flowstones << " flowstone bosses, "
                           << triangles << " triangles");
  CHECK(drips > 20);
  CHECK(stalactites > 10);
  CHECK(stalagmites > 3);
}

TEST_CASE("caves: a cell's container is built through the registry") {
  const caves::test::Hill hill;
  const Plan plan = planned(caves::test::hill_network(), hill, 0.5f);
  u32 cell = 0;
  while (plan.cells[cell].flooded || plan.cells[cell].mouth || plan.cells[cell].passage_m < 12.0f)
    ++cell;
  CellRecipe recipe;
  cell_recipe(plan, cell, 0, recipe);
  const std::string text = cell_recipe_json(recipe);
  const assets::MeshGeneratorDesc* desc = assets::MeshGeneratorRegistry::global().find("cavecell");
  REQUIRE(desc != nullptr);
  CHECK(assets::mesh_generator_of("ddc/caves/cell-0123.cavecell.json") == "cavecell");
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()), text.size());
  u64 source = 0;
  std::string error;
  REQUIRE_MESSAGE(desc->source_hash(bytes, source, error), error);
  CHECK(source == cell_source_hash(recipe));
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(desc->build(bytes, assets::MeshGeneratorInputs{}, geometry::ClusterLodOptions{},
                              0, data, error),
                  error);
  CHECK(data.generator == "cavecell");
  CHECK(data.source_hash == source);
  CHECK(data.materials.size() == 5);
  CHECK(data.cluster_material.size() == data.mesh.mesh.clusters.size());
  u32 roots = 0;
  for (const geometry::ClusterLodDesc& d : data.mesh.lod)
    if (d.parent_error >= geometry::k_lod_terminal_error) ++roots;
  MESSAGE("cell " << cell << "'s container: " << data.mesh.leaf_triangle_count
                  << " leaf triangles, " << data.mesh.mesh.clusters.size() << " clusters, "
                  << data.mesh.level_cluster_counts.size() << " levels, " << roots
                  << " root clusters");
  CHECK(data.mesh.mesh.clusters.size() > 4);
  CHECK(roots < data.mesh.mesh.clusters.size() / 4);
}
