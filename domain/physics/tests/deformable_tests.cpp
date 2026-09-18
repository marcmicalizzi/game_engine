// ADR-0029: the deformable-volume budget, the enforced strain limit, and the conversion from an
// authored volume preservation to a volume constraint's compliance.
//
// The three things E19 (docs/experiments/e19-lattice-cage.md) found missing, in the order it
// found them: a volume compliance that knows the cell size, a `limits.max_strain` that something
// enforces, and a way for the tick to say what its cages cost it.

#include "physics_test_support.h"

#include <core/containers/vector.h>
#include <domain/physics/physics.h>

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
using namespace engine::physics;
using namespace engine::physics::testing;

namespace {

struct Extent {
  f32 min = 0.0f;
  f32 max = 0.0f;
  f32 size() const noexcept { return max - min; }
};

Extent vertical_extent(std::span<const Vec3> points) {
  Extent extent{points[0].y, points[0].y};
  for (const Vec3& p : points) {
    if (p.y < extent.min) extent.min = p.y;
    if (p.y > extent.max) extent.max = p.y;
  }
  return extent;
}

bool all_finite(std::span<const Vec3> points) {
  for (const Vec3& p : points)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
  return true;
}

// Enclosed volume of a closed, outward-wound triangle surface, by the divergence theorem. The
// lattice builder's `faces` are exactly that surface; E19's harness measures volume the same way.
f64 surface_volume(std::span<const Vec3> points, std::span<const u32> faces) {
  f64 six_v = 0.0;
  for (usize i = 0; i + 2 < faces.size(); i += 3) {
    const Vec3 a = points[faces[i]];
    const Vec3 b = points[faces[i + 1]];
    const Vec3 c = points[faces[i + 2]];
    six_v += static_cast<f64>(dot(a, cross(b, c)));
  }
  return six_v / 6.0;
}

// The largest edge stretch ratio in a pose: current length over rest, E19's "element strain"
// expressed the way plan 07 §7.10's `limits.max_strain` is (a ratio of 0.5 is 1.5 here).
f32 max_stretch(const LatticeVolume& lattice, std::span<const Vec3> points) {
  f32 worst = 0.0f;
  for (const SoftEdge& edge : lattice.edges) {
    const f32 rest = length(lattice.vertices[edge.a] - lattice.vertices[edge.b]);
    if (!(rest > 0.0f)) continue;
    const f32 ratio = length(points[edge.a] - points[edge.b]) / rest;
    if (ratio > worst) worst = ratio;
  }
  return worst;
}

}  // namespace

TEST_CASE("physics: volume compliance is derived from the cell size, not authored as one") {
  // The conversion's whole reason to exist: the same authored number has to mean the same
  // material whatever resolution the generator picked for the cage. The backend's constraint is
  // on six times the tetrahedron's volume, so the compliance goes as the cube of the cell.
  const f32 coarse = volume_compliance_for(0.8f, 0.08f);
  const f32 fine = volume_compliance_for(0.8f, 0.04f);
  CHECK(coarse > 0.0f);
  CHECK(fine == doctest::Approx(coarse / 8.0f).epsilon(1e-4));

  // The endpoints mean what plan 07 §7.10 says they mean.
  CHECK(volume_compliance_for(1.0f, 0.06f) == 0.0f);
  CHECK(volume_compliance_for(0.0f, 0.06f) == k_volume_compliance_none);
  // Stiffer preservation is a smaller compliance, monotonically.
  CHECK(volume_compliance_for(0.9f, 0.06f) < volume_compliance_for(0.8f, 0.06f));
  CHECK(volume_compliance_for(0.8f, 0.06f) < volume_compliance_for(0.5f, 0.06f));
  // A softer material wants a softer volume constraint for the same preservation weight.
  CHECK(volume_compliance_for(0.8f, 0.06f, 20.0e3f) > volume_compliance_for(0.8f, 0.06f, 200.0e3f));

  // E19 found 1e-8 by hand at a 5.714 cm cell and never knew what it was. It is a preservation
  // of about 0.63 at plan 07's default stiffness, which is why it worked: the hand-tuned number
  // and the authored default were already the same material.
  CHECK(volume_compliance_for(0.63f, 0.4f / 7.0f) == doctest::Approx(1.0e-8).epsilon(0.05));
}

TEST_CASE("physics: the cage size verdicts are the ones the content validator will apply") {
  // ADR-0029 decision 2. One solve group wide by default; wider is a warning on an ambient volume
  // and allowed on a hero; past 800 nobody gets it.
  CHECK(cage_size_verdict(256, false) == CageSizeVerdict::Ok);
  CHECK(cage_size_verdict(257, false) == CageSizeVerdict::Wide);
  CHECK(cage_size_verdict(512, false) == CageSizeVerdict::Wide);
  CHECK(cage_size_verdict(512, true) == CageSizeVerdict::Ok);
  CHECK(cage_size_verdict(800, true) == CageSizeVerdict::Ok);
  CHECK(cage_size_verdict(801, true) == CageSizeVerdict::Refused);
  CHECK(cage_size_verdict(801, false) == CageSizeVerdict::Refused);
  // The default cage is exactly one backend solve group, which is the reason for the number.
  CHECK(k_cage_elements_default == k_soft_body_constraint_batch);
  CHECK(soft_body_solve_width(k_cage_elements_default) == 1);
}

TEST_CASE("physics: a cage recovers its volume at every cell size the conversion is given") {
  // The permanent set E19 measured, gone. The same cube at three cage resolutions, each built
  // with the compliance the conversion returns for its own cell, pressed to 70% of its height and
  // released: all three come back to within 1% of their rest volume. With one compliance shared
  // across the three — which is what an authored number without the conversion is — the finest
  // cage's volume constraint is eight times too soft and it does not.
  constexpr f32 k_cube_side = 0.4f;
  constexpr f32 k_preservation = 0.8f;

  for (const u32 n : {4u, 5u, 6u}) {
    CAPTURE(n);
    const f32 spacing = k_cube_side / static_cast<f32>(n - 1);
    const LatticeVolume lattice =
        build_lattice_volume(n, spacing, 1.0e-4f, volume_compliance_for(k_preservation, spacing));
    const u32 vertex_count = static_cast<u32>(lattice.vertices.size());

    World world;
    REQUIRE(world.init(small_world_options()) == Status::Ok);
    add_ground(world);

    SoftBodyDesc desc;
    desc.vertices = std::span<const Vec3>(lattice.vertices);
    desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
    desc.edges = std::span<const SoftEdge>(lattice.edges);
    desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
    desc.faces = std::span<const u32>(lattice.faces);
    desc.transform.position = Vec3(0.0f, 0.5f * k_cube_side + 0.02f, 0.0f);
    desc.iterations = k_cage_iterations_default;
    desc.max_strain = 0.5f;
    desc.friction = 0.6f;
    desc.vertex_radius = 0.33f * spacing;
    desc.allow_sleeping = false;
    SoftBodyId cage;
    REQUIRE(world.create_soft_body(desc, cage) == Status::Ok);

    const f32 dt = world.step_seconds();
    const auto step = [&world, dt]() {
      REQUIRE(world.step(dt, k_cage_sub_steps_default) == Status::Ok);
    };

    Vector<Vec3> points(vertex_count);
    const auto read_cage = [&world, &cage, &points, vertex_count]() {
      REQUIRE(world.read_soft_body_vertices(cage, std::span<Vec3>(points)) == vertex_count);
    };

    for (u32 i = 0; i < 90; ++i)
      step();
    read_cage();
    const f32 rest_height = vertical_extent(std::span<const Vec3>(points)).size();
    const f64 rest_volume =
        surface_volume(std::span<const Vec3>(points), std::span<const u32>(lattice.faces));
    REQUIRE(rest_volume > 0.0);

    // A thick plate, driven rather than teleported, to 70% of the cage's height.
    constexpr f32 k_plate_half = 0.2f;
    ShapeId plate_shape;
    REQUIRE(world.create_box(Vec3(0.5f, k_plate_half, 0.5f), plate_shape) == Status::Ok);
    BodyDesc plate_desc;
    plate_desc.shape = plate_shape;
    const Extent settled = vertical_extent(std::span<const Vec3>(points));
    plate_desc.transform.position = Vec3(0.0f, settled.max + k_plate_half + 0.005f, 0.0f);
    plate_desc.motion = MotionType::Kinematic;
    plate_desc.layer = Layer::Kinematic;
    plate_desc.friction = 0.6f;
    BodyId plate;
    REQUIRE(world.create_body(plate_desc, plate) == Status::Ok);

    f32 plate_y = plate_desc.transform.position.y;
    const f32 target = settled.min + 0.70f * rest_height + k_plate_half;
    const auto drive_plate = [&world, &plate, &plate_y, dt](f32 y) {
      plate_y = y;
      Transform3 to;
      to.position = Vec3(0.0f, plate_y, 0.0f);
      REQUIRE(world.move_kinematic(plate, to, dt));
    };

    const f32 travel = (plate_y - target) / 30.0f;
    for (u32 i = 0; i < 30; ++i) {
      drive_plate(plate_y - travel);
      step();
    }
    for (u32 i = 0; i < 30; ++i) {
      drive_plate(target);
      step();
    }
    read_cage();
    CHECK(all_finite(std::span<const Vec3>(points)));
    const f64 pressed_volume =
        surface_volume(std::span<const Vec3>(points), std::span<const u32>(lattice.faces));
    // It really was compressed, so the recovery below is a recovery and not a no-op.
    CHECK(pressed_volume < 0.95 * rest_volume);

    for (u32 i = 0; i < 20; ++i) {
      drive_plate(plate_y + 0.05f);
      step();
    }
    for (u32 i = 0; i < 150; ++i)
      step();

    read_cage();
    CHECK(all_finite(std::span<const Vec3>(points)));
    const f64 recovered =
        surface_volume(std::span<const Vec3>(points), std::span<const u32>(lattice.faces)) /
        rest_volume;
    INFO("cell " << spacing << " m, compliance " << volume_compliance_for(k_preservation, spacing)
                 << ", volume after release " << recovered);
    CHECK(recovered > 0.99);
    CHECK(recovered < 1.01);
  }
}

TEST_CASE("physics: an authored strain limit is enforced, and nothing enforced it before") {
  // plan 07 §7.10 calls `limits.max_strain` "a per-element clamp the solver never exceeds" and
  // until ADR-0029 nothing clamped it. A cage hung from its top face under thirty gravities is
  // the cheapest way to ask an edge to stretch much further than a material would: the load is
  // constant, so the answer is a property of the constraint set and not of a contact.
  constexpr u32 k_n = 3;
  constexpr f32 k_spacing = 0.1f;
  constexpr f32 k_limit = 0.5f;  // plan 07 §7.10's default: a stretch ratio of 1.5
  const LatticeVolume lattice = build_lattice_volume(k_n, k_spacing, 1.0e-3f, 1.0e-6f);
  const u32 vertex_count = static_cast<u32>(lattice.vertices.size());

  // Returns the largest stretch ratio the cage is left holding after it has settled.
  const auto hang = [&lattice, vertex_count](f32 max_strain) {
    World world;
    REQUIRE(world.init(small_world_options()) == Status::Ok);

    // The top face pinned to fixed world points: a null body in an attachment is a point in the
    // world, and `Rigid` is the form that cannot be pulled off it.
    Vector<SoftAttachment> attachments;
    const Vec3 origin(0.0f, 5.0f, 0.0f);
    for (u32 z = 0; z < k_n; ++z) {
      for (u32 x = 0; x < k_n; ++x) {
        SoftAttachment attachment;
        attachment.vertex = lattice.index(x, k_n - 1, z);
        attachment.local_point = origin + lattice.vertices[attachment.vertex];
        attachments.push_back(attachment);
      }
    }

    SoftBodyDesc desc;
    desc.vertices = std::span<const Vec3>(lattice.vertices);
    desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
    desc.edges = std::span<const SoftEdge>(lattice.edges);
    desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
    desc.faces = std::span<const u32>(lattice.faces);
    desc.attachments = std::span<const SoftAttachment>(attachments);
    desc.transform.position = origin;
    desc.iterations = k_cage_iterations_default;
    desc.gravity_factor = 30.0f;
    desc.max_strain = max_strain;
    desc.allow_sleeping = false;
    SoftBodyId cage;
    REQUIRE(world.create_soft_body(desc, cage) == Status::Ok);

    const f32 dt = world.step_seconds();
    for (u32 i = 0; i < 180; ++i)
      REQUIRE(world.step(dt, k_cage_sub_steps_default) == Status::Ok);

    Vector<Vec3> points(vertex_count);
    REQUIRE(world.read_soft_body_vertices(cage, std::span<Vec3>(points)) == vertex_count);
    REQUIRE(all_finite(std::span<const Vec3>(points)));
    return max_stretch(lattice, std::span<const Vec3>(points));
  };

  const f32 unclamped = hang(0.0f);
  const f32 clamped = hang(k_limit);
  INFO("held stretch: unclamped " << unclamped << ", clamped " << clamped);
  // Without the clamp the authored limit is a number in a table that nothing reads.
  CHECK(unclamped > 1.0f + k_limit);
  // With it, the pose the step leaves behind is inside the limit. The tolerance is one part in a
  // thousand of the limit, not a licence: the clamp sweeps until nothing is outside, and what is
  // left is the float error of the projection itself.
  CHECK(clamped <= (1.0f + k_limit) * 1.001f);
}

TEST_CASE("physics: the strain clamp stops the cell inversion the divergence comes from") {
  // E19's failure mechanism, isolated. What made one configuration diverge outright and three
  // others latch at two thirds of their volume was not under-convergence but **cell inversion**:
  // a tetrahedron driven inside out has a volume constraint that pushes it further inside out,
  // so raising the iteration count made it worse rather than better. An element cannot invert
  // without an edge leaving the range a material would hold it in, which is why a clamp on edge
  // length is a cure for a volume failure.
  //
  // The divergence itself is measured where it happened — the E19 press grid, in msvc-release,
  // reports `diverged` per configuration (docs/experiments/e19-lattice-cage.md). What belongs in
  // a unit test is the mechanism, because a test that waits for a NaN is a test tuned to one
  // machine's rounding.
  constexpr u32 k_n = 4;
  constexpr f32 k_spacing = 0.12f;
  const LatticeVolume lattice =
      build_lattice_volume(k_n, k_spacing, 1.0e-6f, volume_compliance_for(0.99f, k_spacing));
  const u32 vertex_count = static_cast<u32>(lattice.vertices.size());
  const f32 side = static_cast<f32>(k_n - 1) * k_spacing;

  // How many of the cage's tetrahedra have turned inside out: the sign of the signed volume
  // against the sign the rest pose had.
  const auto inverted_cells = [&lattice](std::span<const Vec3> points) {
    u32 count = 0;
    for (const SoftVolumeConstraint& tet : lattice.volumes) {
      const auto signed_volume = [&tet](std::span<const Vec3> p) {
        const Vec3 a = p[tet.vertex[0]];
        return dot(p[tet.vertex[1]] - a, cross(p[tet.vertex[2]] - a, p[tet.vertex[3]] - a));
      };
      const f32 rest = signed_volume(std::span<const Vec3>(lattice.vertices));
      const f32 now = signed_volume(points);
      if (!std::isfinite(now) || rest * now <= 0.0f) ++count;
    }
    return count;
  };

  const auto crush = [&lattice, vertex_count, side](f32 max_strain, f32 height_fraction) {
    World world;
    REQUIRE(world.init(small_world_options()) == Status::Ok);
    add_ground(world);

    SoftBodyDesc desc;
    desc.vertices = std::span<const Vec3>(lattice.vertices);
    desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
    desc.edges = std::span<const SoftEdge>(lattice.edges);
    desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
    desc.faces = std::span<const u32>(lattice.faces);
    desc.transform.position = Vec3(0.0f, 0.5f * side + 0.02f, 0.0f);
    desc.iterations = 16;
    desc.max_strain = max_strain;
    desc.friction = 0.9f;
    desc.allow_sleeping = false;
    SoftBodyId cage;
    REQUIRE(world.create_soft_body(desc, cage) == Status::Ok);

    ShapeId plate_shape;
    REQUIRE(world.create_box(Vec3(0.5f, 0.2f, 0.5f), plate_shape) == Status::Ok);
    BodyDesc plate_desc;
    plate_desc.shape = plate_shape;
    plate_desc.transform.position = Vec3(0.0f, side + 0.25f, 0.0f);
    plate_desc.motion = MotionType::Kinematic;
    plate_desc.layer = Layer::Kinematic;
    plate_desc.friction = 0.9f;
    BodyId plate;
    REQUIRE(world.create_body(plate_desc, plate) == Status::Ok);

    const f32 dt = world.step_seconds();
    f32 plate_y = plate_desc.transform.position.y;
    // Driven a long way past the cage's own depth: an inextensible cage cannot win this argument
    // and something has to give.
    const f32 target = height_fraction * side + 0.2f;
    Vector<Vec3> points(vertex_count);
    for (u32 i = 0; i < 200; ++i) {
      plate_y = plate_y - 0.01f > target ? plate_y - 0.01f : target;
      Transform3 to;
      to.position = Vec3(0.0f, plate_y, 0.0f);
      REQUIRE(world.move_kinematic(plate, to, dt));
      REQUIRE(world.step(dt, 1) == Status::Ok);
    }
    REQUIRE(world.read_soft_body_vertices(cage, std::span<Vec3>(points)) == vertex_count);
    return points;
  };

  // 70% of the cage's height is E19's press: 30% of its depth. A cage this stiff cannot give that
  // much without buckling, which is the whole point of pressing it.
  const Vector<Vec3> unclamped = crush(0.0f, 0.7f);
  const Vector<Vec3> clamped = crush(0.5f, 0.7f);
  const u32 unclamped_inversions = inverted_cells(std::span<const Vec3>(unclamped));
  const u32 clamped_inversions = inverted_cells(std::span<const Vec3>(clamped));
  INFO("inverted cells of " << lattice.volumes.size() << ": unclamped " << unclamped_inversions
                            << ", clamped " << clamped_inversions);
  CHECK(unclamped_inversions > 0);
  CHECK(clamped_inversions == 0);
  CHECK(all_finite(std::span<const Vec3>(clamped)));

  // **And here is the limit of the cure**, measured on this same fixture and worth knowing before
  // anyone treats the clamp as a guarantee: driven to 60% of its height the clamp still leaves 50
  // cells inverted, and at 50% and 40% it leaves slightly *more* than no clamp at all (63 and 65
  // against 59 and 62). A kinematic plate is infinitely heavy, so a displacement-controlled press
  // past what the limit allows leaves the projection with two demands it cannot satisfy at once,
  // and it is the clamp that loses. The clamp keeps a cage inside a press a material could
  // survive; it does not make a cage survive a press it cannot. E19's fixture presses 30%.
}

TEST_CASE("physics: a tick reports what its deformable volumes cost it") {
  // ADR-0029 decision 1. The world measures one wall-clock figure for the backend's soft-body
  // phase and splits it by work, so what a test can assert without measuring a clock is the
  // shape of the report: which volumes are hero, that the split goes the way the flags say, and
  // that the overrun is zero while the budget is not exceeded.
  constexpr u32 k_n = 4;
  const LatticeVolume lattice = build_lattice_volume(k_n, 0.1f, 1.0e-4f, 1.0e-8f);

  const auto add_cage = [&lattice](World& world, Vec3 at, bool hero) {
    SoftBodyDesc desc;
    desc.vertices = std::span<const Vec3>(lattice.vertices);
    desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
    desc.edges = std::span<const SoftEdge>(lattice.edges);
    desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
    desc.faces = std::span<const u32>(lattice.faces);
    desc.transform.position = at;
    desc.iterations = k_cage_iterations_default;
    desc.hero = hero;
    desc.allow_sleeping = false;
    SoftBodyId cage;
    REQUIRE(world.create_soft_body(desc, cage) == Status::Ok);
    return cage;
  };

  SUBCASE("a world with no volumes reports nothing, not a small number") {
    World world;
    REQUIRE(world.init(small_world_options()) == Status::Ok);
    add_ground(world);
    step_n(world, 5);
    const SoftBodyBudget budget = world.stats().soft_body_budget;
    CHECK(budget.ambient_ms == 0.0f);
    CHECK(budget.hero_ms == 0.0f);
    CHECK(budget.hero_count == 0);
    CHECK_FALSE(budget.over_budget);
  }

  SUBCASE("one hero volume draws on the hero allowance and nothing else") {
    World world;
    REQUIRE(world.init(small_world_options()) == Status::Ok);
    add_ground(world);
    add_cage(world, Vec3(0.0f, 1.0f, 0.0f), true);
    step_n(world, 20);
    const SoftBodyBudget budget = world.stats().soft_body_budget;
    CHECK(budget.hero_count == 1);
    CHECK(budget.hero_ms > 0.0f);
    CHECK(budget.ambient_ms == 0.0f);
    CHECK(budget.ambient_over_ms == 0.0f);
  }

  SUBCASE("ambient and hero are reported apart, and both are inside their own budget") {
    World world;
    REQUIRE(world.init(small_world_options()) == Status::Ok);
    add_ground(world);
    add_cage(world, Vec3(0.0f, 1.0f, 0.0f), true);
    add_cage(world, Vec3(1.0f, 1.0f, 0.0f), false);
    add_cage(world, Vec3(2.0f, 1.0f, 0.0f), false);
    step_n(world, 20);
    const SoftBodyBudget budget = world.stats().soft_body_budget;
    CHECK(budget.hero_count == 1);
    CHECK(budget.hero_ms > 0.0f);
    CHECK(budget.ambient_ms > 0.0f);
    // Three cages of the same size and iteration count: two ambient against one hero, so the
    // split is the work split and not an accident of which cage the backend got to first.
    CHECK(budget.ambient_ms == doctest::Approx(2.0f * budget.hero_ms).epsilon(0.01));
    // The overrun is the budget arithmetic and nothing else, which is the part a test can pin.
    // The wall clock itself is not: three 64-particle cages go over the 1.5 ms ambient budget on
    // a *debug* build without difficulty, which is worth knowing — a debug tick is not a tick,
    // and every number ADR-0029 quotes is msvc-release.
    const f32 expected_ambient_over = budget.ambient_ms > k_deformable_ambient_budget_ms
                                          ? budget.ambient_ms - k_deformable_ambient_budget_ms
                                          : 0.0f;
    const f32 expected_hero_over = budget.hero_ms > k_deformable_hero_budget_ms
                                       ? budget.hero_ms - k_deformable_hero_budget_ms
                                       : 0.0f;
    CHECK(budget.ambient_over_ms == doctest::Approx(expected_ambient_over));
    CHECK(budget.hero_over_ms == doctest::Approx(expected_hero_over));
    CHECK(budget.over_budget == (expected_ambient_over > 0.0f || expected_hero_over > 0.0f));
  }
}

TEST_CASE("physics: a soft body refuses a strain limit that is not one") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  const ClothSheet sheet = build_cloth_sheet(4, 4, 0.1f, 0.0f, 0.0f);

  SoftBodyDesc desc;
  desc.vertices = std::span<const Vec3>(sheet.vertices);
  desc.edges = std::span<const SoftEdge>(sheet.edges);
  SoftBodyId body;

  SUBCASE("a negative limit is a typo, not 'no limit'") {
    desc.max_strain = -0.5f;
    CHECK(world.create_soft_body(desc, body) == Status::InvalidArgument);
  }
  SUBCASE("a limit of 1 makes the compression bound a length of zero") {
    desc.max_strain = 1.0f;
    CHECK(world.create_soft_body(desc, body) == Status::InvalidArgument);
  }
  SUBCASE("plan 07 §7.10's default is accepted") {
    desc.max_strain = 0.5f;
    CHECK(world.create_soft_body(desc, body) == Status::Ok);
  }
  SUBCASE("zero is the clamp switched off, and costs nothing") {
    desc.max_strain = 0.0f;
    CHECK(world.create_soft_body(desc, body) == Status::Ok);
  }
}
