// The building description and its mesher (docs/subsystems/structures.md): the example cot meshes
// into every surface, every triangle wound the way its normals face, every opening clear where
// the description puts it and the wall solid beside it, the walls joined at their corners and
// risen to the roof, the walk records and the rooms' graph, the validator's refusals by name, a
// house of two storeys with an inner wall, a doorway and a stair through the interiors' seam, the
// recipe's file, the container, and the bytes pinned.

#include <core/hash/hash.h>
#include <domain/structures/structures.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace engine;
using namespace engine::structures;
using structure::Description;
using structure::MeshScope;
using structure::Surface;

namespace {

struct Solved {
  MeshPart parts[k_surfaces];
  Report report;
};

void solved(const Description& d, MeshScope scope, Solved& out) {
  std::string error;
  const bool ok = solve(d, scope, out.parts, out.report, error);
  INFO(error);
  REQUIRE(ok);
}

u32 triangles(const Solved& s, Surface surface) {
  return s.report.surface_triangles[static_cast<u32>(surface)];
}

struct D3 {
  f64 x = 0, y = 0, z = 0;
};
D3 d3(Vec3 v) { return {static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)}; }
D3 sub(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 cross(D3 a, D3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f64 dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// How many triangles of the mesh the segment from `a` to `b` (metres) passes through.
u32 crossings(const Solved& s, D3 a, D3 b) {
  u32 hits = 0;
  const D3 dir = sub(b, a);
  for (const MeshPart& part : s.parts)
    for (u32 t = 0; t + 2 < part.indices.size(); t += 3) {
      const D3 p0 = d3(part.positions[part.indices[t]]);
      const D3 e1 = sub(d3(part.positions[part.indices[t + 1]]), p0);
      const D3 e2 = sub(d3(part.positions[part.indices[t + 2]]), p0);
      const D3 h = cross(dir, e2);
      const f64 det = dot(e1, h);
      if (std::fabs(det) < 1e-12) continue;
      const D3 sv = sub(a, p0);
      const f64 u = dot(sv, h) / det;
      const D3 qv = cross(sv, e1);
      const f64 v = dot(dir, qv) / det;
      const f64 w = dot(e2, qv) / det;
      if (u >= 0 && v >= 0 && u + v <= 1 && w >= 0 && w <= 1) ++hits;
    }
  return hits;
}

// The segment through wall `wall` square to it at station `s` cm and height `y` cm, 30 cm clear
// of each face.
void through(const Description& d, u32 wall, f64 s, f64 y, D3& a, D3& b) {
  const structure::Wall& w = d.walls[wall];
  const f64 dx = w.to[0] - w.from[0], dz = w.to[1] - w.from[1];
  const f64 len = std::sqrt(dx * dx + dz * dz);
  const f64 ux = dx / len, uz = dz / len;
  const f64 reach = w.thickness * 0.5 + 30.0;
  const f64 x = w.from[0] + ux * s, z = w.from[1] + uz * s;
  a = {(x + uz * reach) * 0.01, y * 0.01, (z - ux * reach) * 0.01};
  b = {(x - uz * reach) * 0.01, y * 0.01, (z + ux * reach) * 0.01};
}

u64 mesh_hash(const Solved& s) {
  u64 h = 0;
  for (const MeshPart& part : s.parts) {
    h = hash_combine(h, hash_bytes(part.positions.data(), part.positions.size() * sizeof(Vec3)));
    h = hash_combine(h, hash_bytes(part.normals.data(), part.normals.size() * sizeof(Vec3)));
    h = hash_combine(h, hash_bytes(part.uvs.data(), part.uvs.size() * sizeof(Vec2)));
    h = hash_combine(h, hash_bytes(part.indices.data(), part.indices.size() * sizeof(u32)));
  }
  return h;
}

// The cot with a loft: an inner wall and a doorway on the ground floor, a second storey under the
// roof, and a straight stair up to it.
Description two_storeys() {
  Description d = example_cot(600, 400, 50);
  const i32 b = 450;
  structure::Storey loft;
  loft.elevation = 210;
  loft.height = 120;
  loft.floor_thickness = 18;
  d.storeys.push_back(loft);
  d.roofs[0].eaves = 330;
  d.chimneys[0].top += 120;
  for (u32 i = 0; i < 4; ++i) {
    structure::Wall w = d.walls[i];
    w.storey = 1;
    w.a = 2;
    d.walls.push_back(w);
  }
  structure::Room parlour;
  parlour.id = 1;
  parlour.kind = structure::RoomKind::parlour;
  d.rooms.push_back(parlour);
  structure::Room chamber;
  chamber.id = 2;
  chamber.storey = 1;
  chamber.kind = structure::RoomKind::chamber;
  d.rooms.push_back(chamber);
  structure::Wall inner;
  inner.from[0] = 420, inner.from[1] = 0;
  inner.to[0] = 420, inner.to[1] = b;
  inner.thickness = 12;
  inner.kind = structure::WallKind::inner;
  inner.a = 1;  // the left of +z is +x
  inner.b = 0;
  d.walls.push_back(inner);
  structure::Opening doorway;
  doorway.wall = d.walls.size() - 1;
  doorway.kind = structure::OpeningKind::doorway;
  doorway.at = 120;
  doorway.width = 85;
  doorway.head = 190;
  d.openings.push_back(doorway);
  structure::Stair stair;
  stair.storey = 0;
  stair.foot[0] = 120, stair.foot[1] = 380;
  stair.dir = 0;
  stair.width = 80;
  stair.risers = 12;
  stair.going = 22;
  stair.well[0] = 120, stair.well[1] = 340, stair.well[2] = 400, stair.well[3] = 420;
  stair.from_room = 0;
  stair.to_room = 2;
  d.stairs.push_back(stair);
  return d;
}

}  // namespace

TEST_CASE("structures: the example cot meshes into every surface it has") {
  const Description cot = example_cot();
  std::string why;
  const bool valid = validate(cot, why);
  INFO(why);
  REQUIRE(valid);
  Solved s;
  solved(cot, MeshScope::whole, s);
  for (const Surface surface :
       {Surface::wall_outer, Surface::plinth, Surface::wall_inner, Surface::roof, Surface::timber,
        Surface::floor, Surface::boards, Surface::chimney})
    CHECK(triangles(s, surface) > 0);
  CHECK(triangles(s, Surface::ceiling) == 0);  // one storey, open to the roof
  CHECK(s.report.triangles < 4000);            // structures.md, "Budgets"
  std::printf("structures: the example cot is %u triangles\n", s.report.triangles);
}

TEST_CASE("structures: every triangle is wound the way its normals face, and none is empty") {
  for (const Description& d : {example_cot(), two_storeys()}) {
    Solved s;
    solved(d, MeshScope::whole, s);
    u32 wrong = 0, empty = 0;
    for (const MeshPart& part : s.parts) {
      REQUIRE(part.positions.size() == part.normals.size());
      REQUIRE(part.positions.size() == part.uvs.size());
      for (u32 t = 0; t + 2 < part.indices.size(); t += 3) {
        const u32 i0 = part.indices[t], i1 = part.indices[t + 1], i2 = part.indices[t + 2];
        const D3 n = cross(sub(d3(part.positions[i1]), d3(part.positions[i0])),
                           sub(d3(part.positions[i2]), d3(part.positions[i0])));
        if (dot(n, n) < 1e-16) ++empty;
        for (const u32 i : {i0, i1, i2})
          if (dot(n, d3(part.normals[i])) <= 0) ++wrong;
      }
      for (const Vec2 uv : part.uvs) {
        CHECK(std::fabs(uv.x) <= 16.0f);  // geometry's UV range
        CHECK(std::fabs(uv.y) <= 16.0f);
      }
    }
    CHECK(wrong == 0);
    CHECK(empty == 0);
  }
}

TEST_CASE("structures: an opening is clear where its description puts it, the wall solid beside") {
  const Description cot = example_cot();
  Solved s;
  solved(cot, MeshScope::whole, s);
  for (u32 i = 0; i < cot.openings.size(); ++i) {
    const structure::Opening& o = cot.openings[i];
    CAPTURE(i);
    // Off the middle, where a window's glazing bar stands, and off the frame at the edges.
    for (const f64 fs : {0.25, 0.75})
      for (const f64 fy : {0.3, 0.7}) {
        D3 a, b;
        through(cot, o.wall, o.at + (fs - 0.5) * o.width, o.sill + fy * (o.head - o.sill), a, b);
        CHECK(crossings(s, a, b) == 0);
      }
    // The pier beside it and the wall over it are two faces thick.
    D3 a, b;
    through(cot, o.wall, o.at + 0.5 * o.width + 8.0, o.sill + 0.5 * (o.head - o.sill), a, b);
    CHECK(crossings(s, a, b) == 2);
  }
  // A door's frame stands inside its reveal: clear a frame's width in from the jamb, timber
  // (a box, two faces) at the jamb itself.
  const structure::Opening& door = cot.openings[0];
  D3 a, b;
  through(cot, door.wall, door.at - 0.5 * door.width + 6.5, 100.0, a, b);
  CHECK(crossings(s, a, b) == 0);
  through(cot, door.wall, door.at - 0.5 * door.width + 3.0, 100.0, a, b);
  CHECK(crossings(s, a, b) == 2);
}

TEST_CASE("structures: the walls meet at their corners and rise to the roof") {
  const Description cot = example_cot(600, 400, 50);
  Solved s;
  solved(cot, MeshScope::whole, s);
  f32 lo_x = 1e9f, hi_x = -1e9f, lo_z = 1e9f, hi_z = -1e9f, hi_y = -1e9f;
  for (const Vec3 p : s.parts[static_cast<u32>(Surface::wall_outer)].positions) {
    lo_x = std::min(lo_x, p.x), hi_x = std::max(hi_x, p.x);
    lo_z = std::min(lo_z, p.z), hi_z = std::max(hi_z, p.z);
    hi_y = std::max(hi_y, p.y);
  }
  // The outer faces are the centre lines' rectangle grown by half the wall: mitred, not lapped.
  CHECK(lo_x == doctest::Approx(-0.25).epsilon(1e-5));
  CHECK(hi_x == doctest::Approx(6.75).epsilon(1e-5));
  CHECK(lo_z == doctest::Approx(-0.25).epsilon(1e-5));
  CHECK(hi_z == doctest::Approx(4.75).epsilon(1e-5));
  // The gable's peak is the roof's underside at the ridge: the eaves plus the pitch over half
  // the rectangle's depth.
  CHECK(hi_y == doctest::Approx(2.10 + 1.192 * 2.50).epsilon(1e-5));
  // A corner is closed: a segment through it from outside to inside, off its diagonal, crosses
  // one wall's outer face and the other's inner face.
  const D3 outside{-0.5, 1.0, -0.4}, inside{0.5, 1.0, 0.45};
  CHECK(crossings(s, outside, inside) == 2);
}

TEST_CASE("structures: the walk records and the rooms' graph of the cot") {
  const Description cot = example_cot();
  Walk walk;
  std::string error;
  REQUIRE(walk_records(cot, walk, error));
  CHECK(walk.floors.size() == 1);
  CHECK(walk.floors[0].count == 4);
  CHECK(walk.floors[0].y == 0.0f);
  CHECK(walk.walls.size() == 4);
  CHECK(walk.walls[0].y0 == doctest::Approx(-0.60));
  // The front wall's centre line stands half a wall inside the eaves, under a fifty-degree roof.
  CHECK(walk.walls[3].y1 == doctest::Approx(2.10 + 1.192 * 0.30));
  CHECK(walk.openings.size() == 3);
  CHECK(walk.openings[0].kind == structure::OpeningKind::door);
  CHECK(walk.openings[0].room_a == 0);
  CHECK(walk.openings[0].room_b == -1);
  const WalkOpening& door = walk.openings[0];
  CHECK(std::fabs(door.b.x - door.a.x) == doctest::Approx(0.90));
  CHECK(door.a.y == doctest::Approx(0.0));
  CHECK(door.head == doctest::Approx(1.90));
  CHECK(walk.flights.empty());
  CHECK(walk.wells.empty());

  Vector<RoomLink> links;
  room_links(cot, links);
  REQUIRE(links.size() == 1);  // the door; windows join nothing
  CHECK(links[0].a == 0);
  CHECK(links[0].b == -1);
  CHECK(rooms_reachable(cot));
  Description shut = cot;
  structure::Room cell;
  cell.id = 7;
  shut.rooms.push_back(cell);
  i32 which = -1;
  CHECK_FALSE(rooms_reachable(shut, &which));
  CHECK(which == 7);
}

TEST_CASE("structures: the validator refuses by name and repairs nothing") {
  const Description cot = example_cot();
  std::string why;
  {
    Description d = cot;
    d.openings[0].at = 40;  // the door against the wall's end
    CHECK_FALSE(validate(d, why));
    CHECK(why.find("opening 0") != std::string::npos);
  }
  {
    Description d = cot;
    d.openings[1].at = d.openings[0].at + 60;  // the window into the door
    CHECK_FALSE(validate(d, why));
    CHECK(why.find("opening 1") != std::string::npos);
  }
  {
    Description d = cot;
    d.openings[0].head = 230;  // over the eaves
    CHECK_FALSE(validate(d, why));
    CHECK(why.find("top") != std::string::npos);
  }
  {
    Description d = cot;
    d.walls[2].bulge = 6000;
    CHECK_FALSE(validate(d, why));
    CHECK(why.find("arc") != std::string::npos);
  }
  {
    Description d = cot;
    d.roofs[0].form = structure::RoofForm::hip;
    CHECK_FALSE(validate(d, why));
    CHECK(why.find("hip") != std::string::npos);
  }
  {
    Description d = two_storeys();
    d.storeys[1].elevation = 250;
    CHECK_FALSE(validate(d, why));
    CHECK(why.find("storey 1") != std::string::npos);
  }
}

TEST_CASE("structures: two storeys, an inner wall, a doorway and a stair through the seam") {
  const Description d = two_storeys();
  std::string why;
  const bool valid = validate(d, why);
  INFO(why);
  REQUIRE(valid);
  Solved s;
  solved(d, MeshScope::whole, s);
  CHECK(triangles(s, Surface::ceiling) > 0);  // the loft's floor from beneath
  // The inner wall stops a walker either side of its doorway and not in it.
  const u32 inner = d.walls.size() - 1;
  D3 a, b;
  through(d, inner, 120.0, 100.0, a, b);
  CHECK(crossings(s, a, b) == 0);
  through(d, inner, 300.0, 100.0, a, b);
  CHECK(crossings(s, a, b) == 2);
  // A bare doorway is clear to the millimetre: open a millimetre inside its jamb (120 - 42.5 cm
  // along the wall), wall a millimetre outside it, and wall a millimetre over its head.
  through(d, inner, 77.6, 100.0, a, b);
  CHECK(crossings(s, a, b) == 0);
  through(d, inner, 77.4, 100.0, a, b);
  CHECK(crossings(s, a, b) == 2);
  through(d, inner, 120.0, 189.9, a, b);
  CHECK(crossings(s, a, b) == 0);
  through(d, inner, 120.0, 190.1, a, b);
  CHECK(crossings(s, a, b) == 2);
  // It butts the front wall's inner face (z = 0.25): a line along the front wall just inside it
  // crosses the inner wall's two faces, one just outside the house crosses nothing of it.
  CHECK(crossings(s, {4.0, 1.0, 0.30}, {4.4, 1.0, 0.30}) == 2);
  CHECK(crossings(s, {4.0, 1.0, -0.35}, {4.4, 1.0, -0.35}) == 0);
  // The loft's floor has the stair's well in it: a plumb line through the well meets no floor,
  // one beside it meets the boards and the ceiling beneath them.
  CHECK(crossings(s, {2.6, 1.5, 3.8}, {2.6, 2.5, 3.8}) == 0);
  CHECK(crossings(s, {2.6, 1.5, 2.0}, {2.6, 2.5, 2.0}) == 2);
  // The flight's steps rise to the loft: one over the eighth tread is under the landing's level.
  f32 highest = 0;
  for (const Vec3 p : s.parts[static_cast<u32>(Surface::boards)].positions)
    if (p.z > 3.39f && p.z < 4.21f && p.x < 4.0f && p.y < 2.09f) highest = std::max(highest, p.y);
  CHECK(highest == doctest::Approx(2.10 * 11.0 / 12.0).epsilon(1e-4));

  Walk walk;
  std::string error;
  REQUIRE(walk_records(d, walk, error));
  REQUIRE(walk.flights.size() == 1);
  CHECK(walk.flights[0].rise == doctest::Approx(0.175));
  CHECK(walk.flights[0].top == doctest::Approx(2.10));
  REQUIRE(walk.wells.size() == 1);
  CHECK(walk.wells[0].storey == 1);
  CHECK(rooms_reachable(d));
  Description cut = d;
  cut.stairs.clear();
  i32 which = -1;
  CHECK_FALSE(rooms_reachable(cut, &which));
  CHECK(which == 2);
}

TEST_CASE("structures: the exterior and the interior are the whole between them") {
  for (const Description& d : {example_cot(), two_storeys()}) {
    Solved whole, outside, inside;
    solved(d, MeshScope::whole, whole);
    solved(d, MeshScope::exterior, outside);
    solved(d, MeshScope::interior, inside);
    CHECK(outside.report.triangles + inside.report.triangles == whole.report.triangles);
    CHECK(triangles(inside, Surface::roof) == 0);
    CHECK(triangles(inside, Surface::wall_outer) == 0);
    CHECK(triangles(outside, Surface::ceiling) == 0);
  }
}

TEST_CASE("structures: the recipe's file, its hash and its container") {
  structure::Recipe recipe;
  recipe.description = example_cot();
  const std::string text = canonical_json(recipe);
  structure::Recipe back;
  std::string error;
  const bool read = read_file(text, back, error);
  INFO(error);
  REQUIRE(read);
  CHECK(canonical_json(back) == text);
  CHECK(source_hash(back) == source_hash(recipe));
  structure::Recipe other = recipe;
  other.scope = MeshScope::exterior;
  CHECK(source_hash(other) != source_hash(recipe));
  CHECK_FALSE(read_file("{\"description\": {\"storeys\": []}}", back, error));
  CHECK(error.find("storeys") != std::string::npos);

  geometry::ClusterFileData container;
  Report report;
  const assets::MeshGeneratorInputs inputs;
  const geometry::ClusterLodOptions lod;
  const bool built = build_container(recipe, inputs, lod, 0, container, report, error);
  INFO(error);
  REQUIRE(built);
  CHECK(container.generator == "structure");
  CHECK(container.materials.size() == 8);  // every surface of the cot but a ceiling
  CHECK(container.cluster_material.size() == container.mesh.mesh.clusters.size());
  CHECK(report.clusters > 0);
  CHECK(report.root_triangles > 0);
  CHECK(report.root_triangles <= report.triangles);
  std::printf("structures: the cot's container is %u clusters, %u triangles at its root\n",
              report.clusters, report.root_triangles);
}

TEST_CASE("structures: the same description is the same bytes") {
  Solved a, b;
  solved(example_cot(), MeshScope::whole, a);
  solved(example_cot(), MeshScope::whole, b);
  CHECK(mesh_hash(a) == mesh_hash(b));
  Solved two;
  solved(two_storeys(), MeshScope::whole, two);
  std::printf("structures: mesh hashes cot %016llx two storeys %016llx\n",
              static_cast<unsigned long long>(mesh_hash(a)),
              static_cast<unsigned long long>(mesh_hash(two)));
  // Pinned on MSVC (x86-64-v3); GCC and Clang reproduce them (structures.md, "Determinism"). A
  // mesher change that moves a triangle bumps `k_structure_version` and re-takes these.
  CHECK(mesh_hash(a) == 0xbbd9965c04e1da47ull);
  CHECK(mesh_hash(two) == 0x2b6c4cc1f9504659ull);
}
