// The same bytes on every toolchain (docs/subsystems/walls.md, "Determinism"): a solved module's
// mesh and a layer's placements on a curved ground, hashed, against pins taken on MSVC that Clang
// and GCC at v3 and GCC at v2 reproduce in the containers.

#include "walls_test_support.h"

#include <core/hash/hash.h>

#include <doctest/doctest.h>

#include <cstdio>

using namespace engine;
using namespace engine::walls;

namespace {

u64 mesh_hash(const assets::MeshData& m, const ModuleReport& r) {
  u64 h = hash_bytes(m.positions.data(), m.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(m.normals.data(), m.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(m.uvs.data(), m.uvs.size() * sizeof(Vec2)));
  h = hash_combine(h, hash_bytes(m.indices.data(), m.indices.size() * sizeof(u32)));
  h = hash_combine(h, hash_bytes(r.mid_indices.data(), r.mid_indices.size() * sizeof(u32)));
  h = hash_combine(h, hash_bytes(r.far_indices.data(), r.far_indices.size() * sizeof(u32)));
  h = hash_combine(h, hash_bytes(m.images[0].bytes.data(), m.images[0].bytes.size()));
  return h;
}

}  // namespace

TEST_CASE("walls: modules and placements are the same bytes on every toolchain") {
  // G145's first pins (k_wall_version 1) were taken after every draw was given its own statement:
  // a call's arguments are evaluated in the compiler's order, and with two draws among them MSVC
  // and GCC agreed with each other and not with Clang. G145b (k_wall_version 2) moved the four
  // module pins: split faces with knocked-back arrises, faces built to the line, closed faces, the
  // cope's variation and the palette's mean swatch; the placement pin did not move. G145c
  // (k_wall_version 3) moved the four module pins again: draws keyed by the version (G145b's
  // separate stream removed), faces of their own shape with crowns and worn arrises, closing
  // stones and pinnings no slivers, seams crossed before the faces are drawn, the cope's wedges,
  // leaners and bulging sides, the mid form's cope skyline; the placement pin did not move. Its
  // second commit moved the three that have a cope (its top bulging rather than split, its ends
  // drawn, its wedges and leaners toned down). A replacement records its reason and commit here.
  constexpr u64 expected[] = {0xa022553a997f49c9ull, 0xf6a7cf181a83776aull, 0xeb557bd4a4dfca21ull,
                              0xde95ff3bb586d8d1ull, 0xf75f3d8efeaeb99aull};
  u64 got[5] = {};
  const ModuleKind kinds[4] = {ModuleKind::run, ModuleKind::head, ModuleKind::broken,
                               ModuleKind::fallen};
  const i32 grades[4] = {250, -125, 375, 0};
  for (u32 i = 0; i < 4; ++i) {
    Module m;
    m.kind = kinds[i];
    m.grade_permille = grades[i];
    m.seed = 11 + i;
    assets::MeshData mesh;
    ModuleReport report;
    std::string error;
    REQUIRE(solve_module(m, mesh, report, error));
    got[i] = mesh_hash(mesh, report);
  }
  const Layer layer = walls::test::field_layer(419000, -10000, 0.3f);
  Plan plan;
  std::string why;
  REQUIRE(make_plan(layer, 64000, plan, why));
  const walls::test::TestPlane TestPlane{0.15, 0.05, 0.4};
  const scene_gen::Ground ground = walls::test::ground_of(TestPlane);
  u64 h = 0;
  for (const Slot& s : plan.slots) {
    const scene_gen::Placement p = stand_slot(plan, layer, s, ground).placement;
    h = hash_combine(h, walls::test::placement_hash(p));
  }
  for (const Piece& piece : plan.pieces) {
    const scene_gen::Placement p = stand_piece(plan, layer, piece, ground);
    h = hash_combine(h, walls::test::placement_hash(p));
  }
  got[4] = h;
  for (u32 i = 0; i < 5; ++i) {
    std::printf("walls pin %u: 0x%016llxull\n", i, static_cast<unsigned long long>(got[i]));
    CHECK(got[i] == expected[i]);
  }
}
