// The second batch of species — the birch, the willow, the orchard tree and the ancient tree — and
// the structural table they emit (docs/subsystems/flora.md, "The birch", "The willow", "The orchard
// tree", "Megaflora", "The structural table").
#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/structure.h>
#include <domain/flora/trees.h>
#include <domain/geometry/cluster_pages.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

using namespace engine;

namespace {

f64 triangle_area(const assets::MeshData& mesh, u32 t) {
  const Vec3 a = mesh.positions[mesh.indices[t * 3 + 0]];
  const Vec3 b = mesh.positions[mesh.indices[t * 3 + 1]];
  const Vec3 c = mesh.positions[mesh.indices[t * 3 + 2]];
  const Vec3 n = cross(b - a, c - a);
  return 0.5 * std::sqrt(static_cast<f64>(dot(n, n)));
}

// The mesh's every stream, its motion contract and its structural table, hashed in order.
u64 tree_hash(const assets::MeshData& mesh, const flora::Motion& motion) {
  u64 h = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(mesh.normals.data(), mesh.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2)));
  h = hash_combine(h, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
  for (const assets::Primitive& p : mesh.primitives) {
    h = hash_combine(h, p.first_index);
    h = hash_combine(h, p.index_count);
    h = hash_combine(h, static_cast<u64>(static_cast<u32>(p.material)));
  }
  h = hash_combine(h, hash_bytes(motion.parts.data(),
                                 motion.parts.size() * sizeof(geometry::ClusterFileMotionPart)));
  h = hash_combine(h,
                   hash_bytes(motion.vertex_part.data(), motion.vertex_part.size() * sizeof(u32)));
  h = hash_combine(
      h, hash_bytes(motion.vertex_weight.data(), motion.vertex_weight.size() * sizeof(f32)));
  return hash_combine(h, hash_bytes(motion.structure.data(),
                                    motion.structure.size() * sizeof(flora::StructuralPart)));
}

// Valid, in budget, its primitives covering the index buffer, bark one-sided first and leaves
// two-sided second; the motion contract dense with parents first and every part reaching its root
// and its tip; and the structural table one record a part, in order, radii falling along every
// part, every load-bearing part at least the species' minimum.
void check_tree(const std::string& species, const assets::MeshData& mesh,
                const flora::Motion& motion, const flora::TreeReport& r, u32 budget) {
  REQUIRE(!mesh.positions.empty());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  REQUIRE(mesh.uvs.size() == mesh.positions.size());
  CHECK(mesh.indices.size() / 3 <= budget);
  CHECK(r.triangles == mesh.indices.size() / 3);
  CHECK(r.wood_triangles + r.leaf_triangles + r.fruit_triangles == r.triangles);
  REQUIRE(mesh.primitives.size() >= 2);
  CHECK_FALSE(mesh.materials[0].double_sided);
  CHECK(mesh.materials[1].double_sided);
  u32 covered = 0;
  for (const assets::Primitive& prim : mesh.primitives) {
    CHECK(prim.first_index == covered);
    covered += prim.index_count;
  }
  CHECK(covered == mesh.indices.size());
  for (const Vec3& v : mesh.positions) {
    REQUIRE(std::isfinite(v.x));
    REQUIRE(std::isfinite(v.y));
    REQUIRE(std::isfinite(v.z));
  }
  u32 degenerate = 0;
  for (u32 t = 0; t < mesh.indices.size() / 3; ++t) {
    const u32 a = mesh.indices[t * 3];
    const u32 b = mesh.indices[t * 3 + 1];
    const u32 c = mesh.indices[t * 3 + 2];
    REQUIRE(a < mesh.positions.size());
    REQUIRE(b < mesh.positions.size());
    REQUIRE(c < mesh.positions.size());
    if (a == b || b == c || a == c || !(triangle_area(mesh, t) > 1e-10)) ++degenerate;
  }
  CHECK_MESSAGE(degenerate == 0, degenerate << " degenerate triangles");

  REQUIRE(motion.vertex_part.size() == mesh.positions.size());
  REQUIRE(motion.vertex_weight.size() == mesh.positions.size());
  REQUIRE(motion.parts.size() == r.parts);
  CHECK(motion.parts[0].parent == geometry::k_motion_no_parent);
  Vector<f32> lo(motion.parts.size(), 2.0f);
  Vector<f32> hi(motion.parts.size(), -1.0f);
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    const u32 part = motion.vertex_part[v];
    REQUIRE(part < motion.parts.size());
    lo[part] = std::min(lo[part], motion.vertex_weight[v]);
    hi[part] = std::max(hi[part], motion.vertex_weight[v]);
  }
  u32 load_bearing = 0;
  REQUIRE(motion.structure.size() == motion.parts.size());
  const f32 minimum = flora::load_bearing_min_radius(species);
  CHECK(minimum > 0.0f);
  for (u32 p = 0; p < motion.parts.size(); ++p) {
    CAPTURE(p);
    if (p > 0) CHECK(motion.parts[p].parent < p);
    CHECK(lo[p] <= 0.01f);
    CHECK(hi[p] >= 0.99f);
    const flora::StructuralPart& s = motion.structure[p];
    CHECK(s.part == p);
    CHECK(s.tip_radius >= 0.0f);
    CHECK(s.tip_radius <= s.base_radius);  // a radius never grows along a part
    if (p > 0 && s.base_radius > 0.0f) {
      // Nor from a parent into its child: a child is no thicker than its parent where it leaves.
      const u32 parent = motion.parts[p].parent;
      CHECK(s.base_radius <= motion.structure[parent].base_radius + 1e-6f);
    }
    if ((s.flags & flora::k_load_bearing) != 0u) {
      ++load_bearing;
      CHECK(s.base_radius >= minimum);
    }
  }
  CHECK(load_bearing == r.load_bearing);
  CHECK((motion.structure[0].flags & flora::k_load_bearing) != 0u);
}

template <typename Params, typename Solve>
u64 check_species(const std::string& species, const Params& params, Solve solve, u32 expected_parts,
                  std::initializer_list<u32> budgets, u64 golden) {
  CAPTURE(species);
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE_MESSAGE(solve(params, mesh, &report, &error, &motion), error);
  MESSAGE(species << ": " << report.triangles << " triangles (bark " << report.wood_triangles
                  << ", leaves " << report.leaf_triangles << ", fruit " << report.fruit_triangles
                  << "), " << report.parts << " parts (" << report.load_bearing
                  << " load-bearing), " << report.sprays << " sprays of " << report.cards_per_side
                  << " cards a side, " << report.steps << " budget steps");
  CHECK(report.steps == 0);
  CHECK(report.parts == expected_parts);
  check_tree(species, mesh, motion, report, params.triangle_budget);
  // Every rung holds and names the same parts with the same structure.
  for (const u32 budget : budgets) {
    CAPTURE(budget);
    Params q = params;
    q.triangle_budget = budget;
    assets::MeshData small;
    flora::Motion small_motion;
    flora::TreeReport small_report;
    REQUIRE_MESSAGE(solve(q, small, &small_report, &error, &small_motion), error);
    MESSAGE(species << " on " << budget << ": " << small_report.triangles << " triangles after "
                    << small_report.steps << " steps, " << small_report.cards_per_side
                    << " cards a side");
    check_tree(species, small, small_motion, small_report, budget);
    REQUIRE(small_motion.parts.size() == motion.parts.size());
    CHECK(std::memcmp(small_motion.parts.data(), motion.parts.data(),
                      motion.parts.size() * sizeof(flora::MotionPart)) == 0);
    CHECK(std::memcmp(small_motion.structure.data(), motion.structure.data(),
                      motion.structure.size() * sizeof(flora::StructuralPart)) == 0);
  }
  // One seed one tree, the same bytes on every toolchain; another seed another tree.
  assets::MeshData again;
  flora::Motion again_motion;
  REQUIRE(solve(params, again, nullptr, &error, &again_motion));
  const u64 got = tree_hash(again, again_motion);
  CHECK(got == tree_hash(mesh, motion));
  char hex[32];
  std::snprintf(hex, sizeof(hex), "0x%016llxull", static_cast<unsigned long long>(got));
  CHECK_MESSAGE(got == golden, species << " hashes to " << hex);
  Params other = params;
  other.seed = 2;
  assets::MeshData other_mesh;
  flora::Motion other_motion;
  REQUIRE(solve(other, other_mesh, nullptr, &error, &other_motion));
  CHECK(tree_hash(other_mesh, other_motion) != got);
  // The mesh is the same bytes whether the contract is asked for or not.
  assets::MeshData bare;
  REQUIRE(solve(params, bare, nullptr, &error, nullptr));
  CHECK(bare.positions.size() == mesh.positions.size());
  CHECK(std::memcmp(bare.positions.data(), mesh.positions.data(),
                    mesh.positions.size() * sizeof(Vec3)) == 0);
  return got;
}

// Every vertex of a straight part stands at its structural radius from the segment the rule
// names, wherever it is along it: the rule holds to the millimetre (the tube's rows are its
// resampled stations, so the radius is the tube's own linear one).
void check_straight_tubes(const assets::MeshData& mesh, const flora::Motion& motion,
                          u32 first_part) {
  u32 checked = 0;
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    const u32 p = motion.vertex_part[v];
    if (p < first_part) continue;
    const flora::StructuralPart& s = motion.structure[p];
    if ((s.flags & flora::k_straight) == 0u) continue;
    const flora::MotionPart& part = motion.parts[p];
    const Vec3 d = mesh.positions[v] - part.pivot;
    const f32 along = dot(d, part.direction);
    if (along > part.length) continue;  // the tip's cone
    const Vec3 off = d - part.direction * along;
    const f32 radius = std::sqrt(dot(off, off));
    CHECK(std::abs(radius - flora::structural_radius(s, part, along)) <= 0.002f);
    ++checked;
  }
  CHECK(checked > 0);
}

}  // namespace

TEST_CASE("flora: the birch — slender stems from one foot, a narrow crown, drooping twigs") {
  const flora::Birch p;
  const u32 parts = p.stems * (1 + p.limbs + p.limbs * p.twigs + p.twigs);
  check_species(flora::k_species_birch, p, flora::solve_birch, parts, {12000u, 8000u},
                0x977c615c8a5c0094ull);
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE(flora::solve_birch(p, mesh, &report, &error, &motion));
  // The stems: load-bearing, the first part and the next, from one foot.
  CHECK(report.load_bearing == p.stems);
  CHECK(motion.parts[1].parent == 0u);
  CHECK(std::abs(motion.parts[1].pivot.y) < 1e-6f);
  // The crown is narrow: nothing reaches further from the foot than a third of the height.
  f32 reach = 0.0f;
  for (const Vec3& v : mesh.positions)
    reach = std::max(reach, std::sqrt(v.x * v.x + v.z * v.z));
  MESSAGE("the birch's crown reaches " << reach << " m from its foot, " << p.height_m << " m tall");
  CHECK(reach < 0.36f * p.height_m);
  // The twigs droop: on average a twig's tip (its last station) hangs below where it would be had
  // the twig kept the direction it left its limb in.
  f64 fall = 0.0;
  u32 tips = 0;
  const u32 first_twig = p.stems * (1 + p.limbs);
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    const u32 k = motion.vertex_part[v];
    if (k < first_twig || motion.vertex_weight[v] < 0.999f) continue;
    const flora::MotionPart& t = motion.parts[k];
    fall += static_cast<f64>(mesh.positions[v].y - (t.pivot.y + t.direction.y * t.length));
    ++tips;
  }
  REQUIRE(tips > 0u);
  MESSAGE("the twigs' tips hang " << -fall / tips << " m below their straight line on average");
  CHECK(fall / tips < -0.1);
}

TEST_CASE("flora: the willow — a short trunk, arching limbs, curtains hanging to the ground") {
  const flora::Willow p;
  const u32 parts = 1 + p.limbs + p.limbs * p.branches + p.limbs * p.branches * p.curtains;
  check_species(flora::k_species_willow, p, flora::solve_willow, parts, {20000u, 12000u},
                0xab29e637ff287d6cull);
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE(flora::solve_willow(p, mesh, &report, &error, &motion));
  // Every curtain is its own part with no wood, hanging: its vertices' lowest is far below its
  // pivot, and never under the ground.
  const u32 first_curtain = 1 + p.limbs + p.limbs * p.branches;
  Vector<f32> lowest(motion.parts.size(), 1e9f);
  for (u32 v = 0; v < mesh.positions.size(); ++v)
    lowest[motion.vertex_part[v]] = std::min(lowest[motion.vertex_part[v]], mesh.positions[v].y);
  u32 hanging = 0;
  for (u32 k = first_curtain; k < motion.parts.size(); ++k) {
    CHECK(motion.structure[k].base_radius == 0.0f);
    CHECK(lowest[k] > 0.0f);
    if (motion.parts[k].pivot.y - lowest[k] > 0.5f * std::min(p.curtain_length_m, 2.0f)) ++hanging;
  }
  MESSAGE(hanging << " of " << motion.parts.size() - first_curtain << " curtains hang");
  CHECK(hanging * 10 >= (motion.parts.size() - first_curtain) * 9);
  // The trunk divides low.
  CHECK(motion.parts[0].length < 0.45f * p.height_m);
}

TEST_CASE("flora: the orchard tree — a low broad crown, pruned flat or free, with fruit") {
  const flora::Orchard p;
  const u32 parts = 1 + p.limbs + p.limbs * p.branches + p.limbs * p.branches * p.twigs;
  check_species(flora::k_species_orchard, p, flora::solve_orchard, parts, {8000u},
                0xa34242b45ea067f6ull);
  // With fruit: a third primitive, one-sided, its triangles eight a fruit.
  flora::Orchard fruiting = p;
  fruiting.fruit = 2;
  check_species(flora::k_species_orchard, fruiting, flora::solve_orchard, parts, {},
                0xaed4db8cbb6228edull);
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE(flora::solve_orchard(fruiting, mesh, &report, &error, &motion));
  REQUIRE(mesh.primitives.size() == 3);
  CHECK_FALSE(mesh.materials[2].double_sided);
  CHECK(report.fruit_triangles == p.limbs * p.branches * p.twigs * fruiting.fruit * 8);
  CHECK(mesh.materials[2].base_color.x == fruiting.fruit_red);
  // Pruned, the crown is flat-topped and broad: wider than tall above the trunk, and the free
  // tree's crown is taller.
  const auto extent = [](const assets::MeshData& m, f32& width, f32& top) {
    width = 0.0f;
    top = 0.0f;
    for (const Vec3& v : m.positions) {
      width = std::max(width, std::sqrt(v.x * v.x + v.z * v.z));
      top = std::max(top, v.y);
    }
  };
  f32 width = 0.0f;
  f32 top = 0.0f;
  extent(mesh, width, top);
  MESSAGE("the pruned orchard tree: " << 2.0f * width << " m across, " << top << " m tall");
  CHECK(2.0f * width > top);
  flora::Orchard free = p;
  free.pruned = false;
  REQUIRE(flora::solve_orchard(free, mesh, &report, &error, &motion));
  f32 free_width = 0.0f;
  f32 free_top = 0.0f;
  extent(mesh, free_width, free_top);
  MESSAGE("the free orchard tree: " << 2.0f * free_width << " m across, " << free_top << " m tall");
  CHECK(free_top > top);
}

TEST_CASE("flora: the ancient tree — buttresses, hollows, major limbs, a canopy, its structure") {
  const flora::Ancient p;
  // Parts: the trunk, the runs, the limbs' named segments past them, the dead stubs on the bole,
  // and the recursion's shoots of the part class and up (flora.md, "Megaflora"); the finer shoots
  // and the twigs ride them. 972 until the habits (G79).
  Vector<flora::CrownShoot> crown;
  REQUIRE(flora::ancient_crown(p, crown));
  u32 parts = 0;
  for (const flora::CrownShoot& s : crown)
    parts += (s.flags & flora::k_crown_shoot_part) != 0u ? 1u : 0u;
  CHECK(parts == 496u);
  // 0xe9f2fe3b015def05 until the crown became the allometric recursion (G72), 0xf43a0a8e4b677b3f
  // until the habits and the limbs a wood could carry (G79).
  check_species(flora::k_species_ancient, p, flora::solve_ancient, parts, {100000u, 20000u},
                0x19b81c05b8285745ull);
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE(flora::solve_ancient(p, mesh, &report, &error, &motion));
  check_straight_tubes(mesh, motion, 1);
  // The trunk, every run and every limb's first segment past it are load-bearing, and its later
  // segments while they are thick enough; the trunk and every run are straight; the runs leave the
  // trunk in the emergent habit's band, half to three quarters of the height up, the lowest near
  // level and the highest rising by up to the habit's 25 degrees more, metres thick.
  Vector<flora::AncientLimb> limbs;
  REQUIRE(flora::ancient_limbs(p, limbs));
  u32 bearing = 0;
  for (const flora::StructuralPart& s : motion.structure)
    bearing += (s.flags & flora::k_load_bearing) != 0u ? 1u : 0u;
  CHECK(report.load_bearing == bearing);
  CHECK(report.load_bearing >= 1 + 2 * limbs.size());
  CHECK((motion.structure[0].flags & flora::k_straight) != 0u);
  const f32 low = 0.5f * p.height_m;
  for (u32 i = 0; i < limbs.size(); ++i) {
    CAPTURE(i);
    const flora::MotionPart& run = motion.parts[1 + 2 * i];
    const flora::StructuralPart& s = motion.structure[1 + 2 * i];
    CHECK(run.parent == 0u);
    CHECK((s.flags & flora::k_straight) != 0u);
    CHECK((s.flags & flora::k_load_bearing) != 0u);
    CHECK((motion.structure[2 + 2 * i].flags & flora::k_load_bearing) != 0u);
    CHECK(run.pivot.y >= low - 0.01f);
    CHECK(run.pivot.y <= 0.75f * p.height_m + 0.01f);
    CHECK(2.0f * s.base_radius >= 1.0f);
    const f32 rise = std::asin(run.direction.y) * 57.29578f;
    CHECK(rise < p.limb_rise_deg + 25.0f + 3.01f);
    CHECK(rise > p.limb_rise_deg - 3.01f);
    CHECK(motion.parts[2 + 2 * i].parent == 1 + 2 * i);  // the limb past its run
  }
  // The buttresses: at the ground the bark reaches past the core; well above them it is the core.
  const flora::MotionPart& trunk = motion.parts[0];
  const flora::StructuralPart& core = motion.structure[0];
  f32 foot = 0.0f;
  f32 above = 0.0f;
  f32 above_min = 1e9f;
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    if (motion.vertex_part[v] != 0u) continue;
    const Vec3 d = mesh.positions[v] - trunk.pivot;
    const f32 along = dot(d, trunk.direction);
    const Vec3 off = d - trunk.direction * along;
    const f32 r = std::sqrt(dot(off, off));
    const f32 y = mesh.positions[v].y;
    if (y > -0.5f && y < 0.5f) foot = std::max(foot, r);
    if (y > p.buttress_height_m * 1.4f && y < low && along < trunk.length) {
      above = std::max(above, r - flora::structural_radius(core, trunk, along));
      above_min = std::min(above_min, r - flora::structural_radius(core, trunk, along));
    }
  }
  MESSAGE("the foot reaches " << foot << " m from the axis on a " << p.trunk_radius_m
                              << " m core; above the buttresses the bark is the core to " << above
                              << " / " << above_min << " m");
  CHECK(foot > p.trunk_radius_m * (1.0f + 0.5f * p.buttress_reach));
  CHECK(above < 0.01f);
  CHECK(above_min > -0.01f);

  // The hollow: a doorway open from outside into the trunk and closed at its back.
  Vector<flora::Hollow> hollows;
  REQUIRE(flora::ancient_hollows(p, hollows));
  REQUIRE(hollows.size() == p.hollows);
  CHECK(report.hollows == p.hollows);
  const auto hits = [&](Vec3 from, Vec3 to) {
    const assets::Primitive& bark = mesh.primitives[0];
    for (u32 i = bark.first_index; i < bark.first_index + bark.index_count; i += 3) {
      const Vec3 a = mesh.positions[mesh.indices[i]];
      const Vec3 b = mesh.positions[mesh.indices[i + 1]];
      const Vec3 c = mesh.positions[mesh.indices[i + 2]];
      // Moller-Trumbore over the segment.
      const Vec3 dir = to - from;
      const Vec3 e1 = b - a;
      const Vec3 e2 = c - a;
      const Vec3 h = cross(dir, e2);
      const f32 determinant = dot(e1, h);
      if (std::abs(determinant) < 1e-9f) continue;
      const f32 inv = 1.0f / determinant;
      const Vec3 s = from - a;
      const f32 u = dot(s, h) * inv;
      if (u < 0.0f || u > 1.0f) continue;
      const Vec3 q = cross(s, e1);
      const f32 w = dot(dir, q) * inv;
      if (w < 0.0f || u + w > 1.0f) continue;
      const f32 t = dot(e2, q) * inv;
      if (t >= 0.0f && t <= 1.0f) return true;
    }
    return false;
  };
  for (const flora::Hollow& h : hollows) {
    const Vec3 middle = h.sill + h.up * (0.5f * h.height);
    CHECK_FALSE(hits(middle - h.inward * 1.0f, middle + h.inward * (0.8f * h.depth)));
    CHECK(hits(middle + h.inward * (0.8f * h.depth), middle + h.inward * (1.2f * h.depth)));
    // And its floor: from inside the cavity straight down, the floor is there.
    const Vec3 inside = h.sill + h.inward * (0.5f * h.depth) + h.up * 0.5f;
    CHECK(hits(inside, inside - h.up * 0.6f));
    MESSAGE("a hollow " << h.width << " x " << h.height << " m, " << h.depth << " m deep, its sill "
                        << h.sill.y << " m up");
  }
}

// The megaflora's habits (G79; flora.md "Megaflora", docs/experiments/megaflora-habits.md): the
// emergent's crown high on a long bole and narrow, the spreading one's low and broad, the mixed
// one's high and broad; every limb no longer than its wood could carry; a straight, near-level,
// load-bearing run on every habit's lowest limb.
TEST_CASE("flora: the ancient tree's habits, and limbs a wood could carry") {
  struct Measured {
    f32 width = 0.0f;   // the leaves' extent across, seen from above, the widest of two axes
    f32 top = 0.0f;     // the highest vertex
    f32 lowest = 0.0f;  // the lowest run's height
  };
  const auto measure = [](const flora::Ancient& p, const char* name) {
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeReport report;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_ancient(p, mesh, &report, &error, &motion), error);
    check_tree(flora::k_species_ancient, mesh, motion, report, p.triangle_budget);
    check_straight_tubes(mesh, motion, 1);
    Measured m;
    f32 x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    const assets::Primitive& leaves = mesh.primitives[1];
    for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; ++i) {
      const Vec3 v = mesh.positions[mesh.indices[i]];
      x0 = std::min(x0, v.x);
      x1 = std::max(x1, v.x);
      z0 = std::min(z0, v.z);
      z1 = std::max(z1, v.z);
    }
    for (const Vec3& v : mesh.positions)
      m.top = std::max(m.top, v.y);
    m.width = std::max(x1 - x0, z1 - z0);
    // The lowest limb's run: part 1, straight, load-bearing, near level.
    const flora::MotionPart& run = motion.parts[1];
    const flora::StructuralPart& s = motion.structure[1];
    m.lowest = run.pivot.y;
    CHECK((s.flags & flora::k_straight) != 0u);
    CHECK((s.flags & flora::k_load_bearing) != 0u);
    const f32 rise = std::asin(run.direction.y) * 57.29578f;
    CHECK(rise < p.limb_rise_deg + 4.0f);
    // Every limb inside its wood's strength, and the report's limbs the table's runs.
    Vector<flora::AncientLimb> limbs;
    REQUIRE(flora::ancient_limbs(p, limbs));
    u32 shortened = 0;
    u32 segments = 0;
    u32 broken = 0;
    for (u32 i = 0; i < limbs.size(); ++i) {
      CAPTURE(i);
      const flora::AncientLimb& l = limbs[i];
      CHECK(l.peak_stress_mpa <= l.allowable_mpa * 1.001f);
      CHECK(l.scale > 0.0f);
      CHECK(l.scale <= 1.0f);
      CHECK(std::abs(motion.structure[1 + 2 * i].base_radius - l.base_radius_m) < 1e-5f);
      shortened += l.scale < 1.0f ? 1u : 0u;
      segments += l.segments;
      broken += l.broken;
    }
    MESSAGE(std::string(name) << ": " << m.width << " m across, " << m.top << " m tall (W/H "
                              << m.width / m.top << "), lowest run " << m.lowest << " m up at "
                              << rise << " degrees, " << s.base_radius * 2.0f << " m thick, "
                              << run.length << " m long; " << limbs.size() << " limbs, " << segments
                              << " segments, " << broken << " broken, " << shortened
                              << " shortened by their wood; " << report.triangles
                              << " triangles after " << report.steps << " steps, " << report.parts
                              << " parts, " << report.load_bearing << " load-bearing");
    for (const flora::AncientLimb& l : limbs) {
      MESSAGE("  limb at " << l.height_m << " m, r " << l.base_radius_m << " m, asked " << l.asked_m
                           << " m, kept " << l.scale << ", reach " << l.reach_m << " m, peak "
                           << l.peak_stress_mpa << " of " << l.allowable_mpa << " MPa");
    }
    return m;
  };
  flora::Ancient emergent;
  emergent.habit = flora::AncientHabit::emergent;
  flora::Ancient spreading;
  spreading.habit = flora::AncientHabit::spreading;
  spreading.height_m = 50.0f;
  spreading.trunk_radius_m = 3.0f;
  spreading.buttress_height_m = 4.0f;
  spreading.hollow_spacing_m = 6.0f;
  flora::Ancient mixed;
  mixed.habit = flora::AncientHabit::mixed;
  mixed.height_m = 80.0f;
  mixed.trunk_radius_m = 4.0f;
  const Measured e = measure(emergent, "emergent, 100 m");
  const Measured s = measure(spreading, "spreading, 50 m");
  const Measured m = measure(mixed, "mixed, 80 m");
  // The emergent's crown on the top half of a long bole, the spreading one's on a short trunk.
  CHECK(e.lowest > 0.45f * emergent.height_m);
  CHECK(s.lowest < 0.3f * spreading.height_m);
  CHECK(m.lowest > 0.45f * mixed.height_m);
  // Crown width against height: narrow, broad, between.
  CHECK(e.width / e.top < 0.65f);
  CHECK(s.width / s.top > 1.1f);
  CHECK(m.width / m.top > e.width / e.top);

  // A recipe asking for what no wood could carry — the forest village's anchor tree, 20 m of run
  // and 28 m past it near level on 1.6 m limbs (2026-10-22) — gets limbs its wood can carry.
  flora::Ancient anchor;
  anchor.seed = 101;
  anchor.height_m = 80.0f;
  anchor.trunk_radius_m = 4.5f;
  anchor.limbs = 7;
  anchor.limb_low_m = 18.0f;
  anchor.limb_high_m = 48.0f;
  anchor.limb_rise_deg = 2.0f;
  anchor.limb_run_m = 20.0f;
  anchor.limb_reach_m = 28.0f;
  measure(anchor, "the forest village's anchor A");
  Vector<flora::AncientLimb> limbs;
  REQUIRE(flora::ancient_limbs(anchor, limbs));
  CHECK(limbs[0].scale < 1.0f);  // the lowest limb, asked 48 m at 2 degrees, is shortened
  anchor.seed = 203;
  anchor.height_m = 75.0f;
  anchor.trunk_radius_m = 4.0f;
  anchor.limb_high_m = 45.0f;
  anchor.limb_rise_deg = 3.0f;
  anchor.limb_reach_m = 26.0f;
  measure(anchor, "the forest village's anchor B");
}

TEST_CASE("flora: the structural table round-trips through the container, appended after 36") {
  flora::FloraFile file;
  std::string error;
  REQUIRE_MESSAGE(
      flora::read_flora_file("{\"species\":\"birch\",\"birch\":{\"seed\":4}}", file, error), error);
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(flora::build_flora_container(file, geometry::ClusterLodOptions{},
                                               geometry::ClusterPagesOptions{}.page_bytes, data,
                                               nullptr, error),
                  error);
  CHECK(data.generator_kind == "birch");
  REQUIRE(data.structural_parts.size() == data.motion_parts.size());
  engine::test::TempDir tmp("flora_structure");
  const std::string path = tmp.file("birch.clusters");
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, data, &error), error);
  geometry::ClusterFileData back;
  REQUIRE_MESSAGE(geometry::read_cluster_file(path, back, &error), error);
  REQUIRE(back.structural_parts.size() == data.structural_parts.size());
  CHECK(std::memcmp(back.structural_parts.data(), data.structural_parts.data(),
                    data.structural_parts.size() * sizeof(flora::StructuralPart)) == 0);
  // The palm emits none, so its container has no section 37 and is the bytes it was.
  flora::FloraFile palm;
  REQUIRE(flora::read_flora_file("{}", palm, error));
  geometry::ClusterFileData palm_data;
  REQUIRE(flora::build_flora_container(palm, geometry::ClusterLodOptions{}, 0, palm_data, nullptr,
                                       error));
  CHECK(palm_data.structural_parts.empty());
  CHECK(flora::load_bearing_min_radius(flora::k_species_date_palm) == 0.0f);
  CHECK(std::string(geometry::cluster_section_name(
            static_cast<u32>(geometry::ClusterSection::StructuralParts))) == "structural_parts");
}

TEST_CASE("flora: the second batch's ranges, ends and refusals") {
  std::string error;
  flora::Birch b;
  b.stems = 4;
  CHECK_FALSE(flora::birch_in_range(b, error));
  CHECK(error.find("stems") != std::string::npos);
  flora::Willow w;
  w.curtains = 0;
  CHECK_FALSE(flora::willow_in_range(w, error));
  CHECK(error.find("curtains") != std::string::npos);
  flora::Orchard o;
  o.fruit = 7;
  CHECK_FALSE(flora::orchard_in_range(o, error));
  CHECK(error.find("fruit") != std::string::npos);
  flora::Ancient a;
  a.height_m = 160.0f;
  CHECK_FALSE(flora::ancient_in_range(a, error));
  CHECK(error.find("height_m") != std::string::npos);
  // The ancient tree's fields that hold only together: a run too short to leave its trunk, and a
  // hollow reaching up among the limbs.
  a = flora::Ancient{};
  a.trunk_radius_m = 12.0f;
  a.limb_run_m = 6.0f;
  CHECK_FALSE(flora::ancient_in_range(a, error));
  CHECK(error.find("limb_run_m") != std::string::npos);
  a = flora::Ancient{};
  a.hollows = 3;
  a.hollow_spacing_m = 13.0f;
  a.limb_low_m = 30.0f;  // the default's limbs start at half the height (G79)
  CHECK_FALSE(flora::ancient_in_range(a, error));
  CHECK(error.find("hollow_sill_m") != std::string::npos);

  // Everything at its most expensive at once on the smallest budget, where the bottom rung holds.
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  flora::Birch wb;
  wb.stems = 3;
  wb.limbs = 16;
  wb.twigs = 16;
  wb.leaves = 30;
  wb.triangle_budget = 8000;
  REQUIRE_MESSAGE(flora::solve_birch(wb, mesh, &report, &error, &motion), error);
  check_tree(flora::k_species_birch, mesh, motion, report, wb.triangle_budget);
  MESSAGE("birch at its most expensive on 8,000: " << report.triangles << " after " << report.steps
                                                   << " steps");
  flora::Willow ww;
  ww.limbs = 12;
  ww.branches = 10;
  ww.curtains = 16;
  ww.leaves = 40;
  ww.triangle_budget = 12000;
  REQUIRE_MESSAGE(flora::solve_willow(ww, mesh, &report, &error, &motion), error);
  check_tree(flora::k_species_willow, mesh, motion, report, ww.triangle_budget);
  MESSAGE("willow at its most expensive on 12,000: " << report.triangles << " after "
                                                     << report.steps << " steps");
  flora::Orchard wo;
  wo.limbs = 8;
  wo.branches = 8;
  wo.twigs = 12;
  wo.leaves = 30;
  wo.triangle_budget = 8000;
  REQUIRE_MESSAGE(flora::solve_orchard(wo, mesh, &report, &error, &motion), error);
  check_tree(flora::k_species_orchard, mesh, motion, report, wo.triangle_budget);
  MESSAGE("orchard tree at its most expensive on 8,000: " << report.triangles << " after "
                                                          << report.steps << " steps");
  // Fruit is never coarsened: a budget that cannot hold it is refused with the count.
  wo.fruit = 6;
  CHECK_FALSE(flora::solve_orchard(wo, mesh, &report, &error, &motion));
  CHECK(error.find("cannot hold") != std::string::npos);
  MESSAGE(error);
  // The ancient tree's coarsest rung, and the smallest trees of every species.
  flora::Ancient wa;
  wa.triangle_budget = 20000;
  REQUIRE_MESSAGE(flora::solve_ancient(wa, mesh, &report, &error, &motion), error);
  check_tree(flora::k_species_ancient, mesh, motion, report, wa.triangle_budget);
  MESSAGE("the default ancient tree on 20,000: " << report.triangles << " after " << report.steps
                                                 << " steps");
  wa.limbs = 12;
  wa.branches = 10;
  wa.sub_branches = 8;
  wa.twigs = 12;
  // The recursion's parts are capped (the finer shoots fold into them), so even the tree with the
  // most limbs and laterals holds the floor: it was refused there until the crown became the
  // allometric recursion (G72).
  REQUIRE_MESSAGE(flora::solve_ancient(wa, mesh, &report, &error, &motion), error);
  check_tree(flora::k_species_ancient, mesh, motion, report, wa.triangle_budget);
  MESSAGE("the ancient tree at its most limbs on 20,000: " << report.triangles << " after "
                                                           << report.steps << " steps, "
                                                           << report.parts << " parts");
  flora::Ancient sa;
  sa.height_m = 40.0f;
  sa.trunk_radius_m = 2.0f;
  sa.limb_low_m = 12.0f;
  sa.limb_high_m = 28.0f;
  sa.limb_radius_m = 0.5f;
  sa.limb_run_m = 6.0f;
  sa.limb_reach_m = 8.0f;
  sa.limbs = 2;
  sa.branches = 2;
  sa.sub_branches = 1;
  sa.twigs = 1;
  sa.leaves = 1;
  sa.hollow_width_m = 0.8f;
  sa.hollow_height_m = 1.6f;
  REQUIRE_MESSAGE(flora::solve_ancient(sa, mesh, &report, &error, &motion), error);
  check_tree(flora::k_species_ancient, mesh, motion, report, sa.triangle_budget);
  flora::Birch sb;
  sb.height_m = 3.0f;
  sb.stem_radius_m = 0.03f;
  sb.stems = 3;
  sb.limbs = 2;
  sb.twigs = 2;
  sb.leaves = 1;
  REQUIRE_MESSAGE(flora::solve_birch(sb, mesh, &report, &error, &motion), error);
  check_tree(flora::k_species_birch, mesh, motion, report, sb.triangle_budget);
  flora::Willow sw;
  sw.height_m = 4.0f;
  sw.limbs = 2;
  sw.branches = 1;
  sw.curtains = 1;
  REQUIRE_MESSAGE(flora::solve_willow(sw, mesh, &report, &error, &motion), error);
  check_tree(flora::k_species_willow, mesh, motion, report, sw.triangle_budget);
  flora::Orchard so;
  so.height_m = 2.0f;
  so.limbs = 2;
  so.branches = 1;
  so.twigs = 1;
  so.leaves = 1;
  REQUIRE_MESSAGE(flora::solve_orchard(so, mesh, &report, &error, &motion), error);
  check_tree(flora::k_species_orchard, mesh, motion, report, so.triangle_budget);
}

// Each new species is a member of the flora file and builds a container through the same path,
// and its canonical form carries that species alone.
TEST_CASE("flora: a file names each new species, and its key carries that species alone") {
  for (const char* species : {flora::k_species_birch, flora::k_species_willow,
                              flora::k_species_orchard, flora::k_species_ancient}) {
    CAPTURE(species);
    flora::FloraFile file;
    std::string error;
    const std::string text = std::string("{\"species\":\"") + species + "\"}";
    REQUIRE_MESSAGE(flora::read_flora_file(text, file, error), error);
    const std::string canonical = flora::canonical_flora_json(file);
    CHECK(canonical.find(std::string("\"") + species + "\":") != std::string::npos);
    CHECK(canonical.find("\"palm\"") == std::string::npos);
    CHECK(canonical.find("\"broadleaf\"") == std::string::npos);
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeReport report;
    REQUIRE_MESSAGE(flora::solve_flora(file, mesh, motion, nullptr, &report, error), error);
    CHECK(motion.structure.size() == motion.parts.size());
  }
}
