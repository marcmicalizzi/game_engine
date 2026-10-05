// Soft bodies: a pinned cloth sags and settles, and a lattice cube springs back from a press.
// These are the two cage kinds ADR-0026 names for v1 and the fixtures E19 will grow from.

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

}  // namespace

TEST_CASE("physics: the cloth and lattice builders produce the cages they promise") {
  const ClothSheet sheet = build_cloth_sheet(5, 4, 0.1f, 0.0f, 1.0e-5f);
  CHECK(sheet.vertices.size() == 20);
  CHECK(sheet.inverse_masses.size() == 20);
  // 4*4 along x, 5*3 along z, and two shear edges per quad.
  CHECK(sheet.edges.size() == 16 + 15 + 2 * 4 * 3);
  CHECK(sheet.faces.size() == 4 * 3 * 6);
  CHECK(sheet.index(4, 3) == 19);

  const LatticeVolume lattice = build_lattice_volume(3, 0.2f, 0.0f, 0.0f);
  CHECK(lattice.vertices.size() == 27);
  CHECK(lattice.volumes.size() == 6 * 8);  // six tetrahedra per cell
  CHECK(lattice.index(2, 2, 2) == 26);
  // Axis edges (3 * n^2 * (n-1)), face diagonals (3 * 2 * n * (n-1)^2), body diagonals (4 per
  // cell). Each is generated once, so the count is exact rather than approximate.
  CHECK(lattice.edges.size() == 3u * 9u * 2u + 3u * 2u * 3u * 4u + 4u * 8u);
  CHECK(lattice.faces.size() == 6u * 4u * 6u);

  CHECK(build_cloth_sheet(1, 4, 0.1f, 0.0f, 0.0f).vertices.empty());
  CHECK(build_lattice_volume(1, 0.1f, 0.0f, 0.0f).vertices.empty());
}

TEST_CASE("physics: a cloth pinned at two corners sags, settles, and stays finite") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);

  constexpr u32 k_side = 11;
  const ClothSheet sheet = build_cloth_sheet(k_side, k_side, 0.1f, 1.0e-6f, 1.0e-5f);
  const Vec3 origin(0.0f, 3.0f, 0.0f);

  // The two corners of one edge, pinned where they start. A null body in an attachment is a
  // fixed point in the world, which is what a tarp nailed to a wall is, given in the frame the
  // sheet was placed in.
  const u32 corner_a = sheet.index(0, 0);
  const u32 corner_b = sheet.index(k_side - 1, 0);
  SoftAttachment attachments[2];
  attachments[0].vertex = corner_a;
  attachments[0].local_point = sheet.vertices[corner_a];
  attachments[1].vertex = corner_b;
  attachments[1].local_point = sheet.vertices[corner_b];

  SoftBodyDesc desc;
  desc.vertices = std::span<const Vec3>(sheet.vertices);
  desc.inverse_masses = std::span<const f32>(sheet.inverse_masses);
  desc.edges = std::span<const SoftEdge>(sheet.edges);
  desc.attachments = std::span<const SoftAttachment>(attachments, 2);
  desc.faces = std::span<const u32>(sheet.faces);
  desc.transform.position = place(origin);
  desc.iterations = 8;
  // A sheet pinned along one edge is a pendulum: it swings for a long time at the default
  // damping, and "settles" is the property being tested here, not "swings realistically".
  desc.linear_damping = 0.5f;
  desc.allow_sleeping = false;
  SoftBodyId cloth;
  REQUIRE(world.create_soft_body(desc, cloth) == Status::Ok);
  CHECK(world.soft_body_vertex_count(cloth) == k_side * k_side);
  CHECK(world.soft_body_count() == 1);

  // Twenty-five seconds, because that is how long this takes. A sheet pinned along one edge
  // swings like a curtain, and the near-inextensible edges converge slowly: eight Gauss-Seidel
  // iterations move a constraint error about eight vertices along an eleven-vertex sheet per
  // step, so the last of the stretch comes out over hundreds of steps. Measured decay of the
  // largest movement per half second: 0.50 m at 300 steps, 0.042 at 600, 0.010 at 1200,
  // 0.0017 at 1500, 0.0001 at 2100.
  Vector<Vec3> points(k_side * k_side);
  Vector<Vec3> earlier(k_side * k_side);
  step_n(world, 1470);
  REQUIRE(world.read_soft_body_vertices(cloth, WorldPos::origin(), std::span<Vec3>(earlier)) ==
          k_side * k_side);
  step_n(world, 30);
  REQUIRE(world.read_soft_body_vertices(cloth, WorldPos::origin(), std::span<Vec3>(points)) ==
          k_side * k_side);

  CHECK(all_finite(std::span<const Vec3>(points)));

  // The pinned corners have not moved.
  CHECK(std::abs(points[corner_a].y - (origin.y + sheet.vertices[corner_a].y)) < 0.01f);
  CHECK(std::abs(points[corner_b].y - (origin.y + sheet.vertices[corner_b].y)) < 0.01f);

  // And everything else hangs below them.
  const f32 corner_height = points[corner_a].y;
  CHECK(points[sheet.index(k_side / 2, k_side / 2)].y < corner_height - 0.05f);
  CHECK(points[sheet.index(k_side / 2, k_side - 1)].y < corner_height - 0.1f);

  // It has settled: twenty more steps move nothing much.
  f32 max_move = 0.0f;
  u32 worst = 0;
  for (u32 i = 0; i < points.size(); ++i) {
    const Vec3 delta = points[i] - earlier[i];
    const f32 move = std::sqrt(dot(delta, delta));
    if (move > max_move) {
      max_move = move;
      worst = i;
    }
  }
  INFO("largest movement at vertex " << worst);
  CHECK(max_move < 0.01f);

  // The sheet is inextensible enough that it did not stretch to the floor.
  const Extent extent = vertical_extent(std::span<const Vec3>(points));
  CHECK(extent.size() < 1.2f);
}

TEST_CASE("physics: a compressed soft lattice springs back") {
  WorldOptions options = small_world_options();
  World world;
  REQUIRE(world.init(options) == Status::Ok);
  add_ground(world);

  constexpr u32 k_n = 5;
  constexpr f32 k_spacing = 0.2f;
  constexpr f32 k_side = static_cast<f32>(k_n - 1) * k_spacing;  // 0.8 m
  // Compliant enough to be pressed 30% out of shape: a cage with a compliance of 1e-6 is
  // effectively incompressible, and forcing 30% out of an incompressible cage is a fight the
  // solver does not win. Foam and flesh are the material this models, not steel.
  const LatticeVolume lattice = build_lattice_volume(k_n, k_spacing, 1.0e-4f, 1.0e-4f);

  SoftBodyDesc desc;
  desc.vertices = std::span<const Vec3>(lattice.vertices);
  desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
  desc.edges = std::span<const SoftEdge>(lattice.edges);
  desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
  desc.faces = std::span<const u32>(lattice.faces);
  desc.transform.position = place(Vec3(0.0f, 0.5f * k_side + 0.02f, 0.0f));
  desc.iterations = 10;
  desc.friction = 0.6f;
  desc.allow_sleeping = false;
  SoftBodyId cube;
  REQUIRE(world.create_soft_body(desc, cube) == Status::Ok);

  Vector<Vec3> points(k_n * k_n * k_n);
  const auto height = [&world, &cube, &points]() {
    REQUIRE(world.read_soft_body_vertices(cube, WorldPos::origin(), std::span<Vec3>(points)) ==
            points.size());
    return vertical_extent(std::span<const Vec3>(points)).size();
  };

  step_n(world, 120);
  const f32 rest_height = height();
  CHECK(rest_height > 0.5f * k_side);
  CHECK(all_finite(std::span<const Vec3>(points)));

  // A kinematic plate driven down onto the cube. This is ADR-0026's reference motion in
  // miniature: a rigid thing presses into a volume, the volume gives, and it comes back.
  // A thick plate, not a sheet. Soft-body particles are points to the solver, and a particle
  // that a stiff cage flings a few centimetres in one step can end up on the far side of a
  // thin obstacle, after which the nearest face is the wrong one and it is pushed further out.
  // Depth is the cheapest fix and the one a real press has anyway.
  constexpr f32 k_plate_half = 0.5f;
  ShapeId plate_shape;
  REQUIRE(world.create_box(Vec3(1.0f, k_plate_half, 1.0f), plate_shape) == Status::Ok);
  BodyDesc plate_desc;
  plate_desc.shape = plate_shape;
  plate_desc.transform.position = place(Vec3(0.0f, rest_height + k_plate_half + 0.25f, 0.0f));
  plate_desc.motion = MotionType::Kinematic;
  plate_desc.layer = Layer::Kinematic;
  BodyId plate;
  REQUIRE(world.create_body(plate_desc, plate) == Status::Ok);

  const f32 dt = world.step_seconds();
  const f32 floor = vertical_extent(std::span<const Vec3>(points)).min;
  const f32 target_plate_y = floor + 0.70f * rest_height + k_plate_half;
  f32 plate_y = local_of(plate_desc.transform.position).y;
  for (u32 i = 0; i < 240; ++i) {
    plate_y = plate_y > target_plate_y ? plate_y - 0.004f : target_plate_y;
    BodyTransform target;
    target.position = place(Vec3(0.0f, plate_y, 0.0f));
    REQUIRE(world.move_kinematic(plate, target, dt));
    REQUIRE(world.step() == Status::Ok);
  }

  const f32 pressed_height = height();
  CHECK(all_finite(std::span<const Vec3>(points)));
  // The press holds what it asked for, so the recovery below is measured from 70% and not from
  // "as far as the solver happened to get".
  CHECK(pressed_height == doctest::Approx(0.70f * rest_height).epsilon(0.02));
  CHECK(pressed_height < 0.8f * rest_height);

  // Lift the plate clear and let it recover. Volume constraints are what make this happen:
  // edges alone let the cube fold and stay folded.
  for (u32 i = 0; i < 60; ++i) {
    plate_y += 0.02f;
    BodyTransform target;
    target.position = place(Vec3(0.0f, plate_y, 0.0f));
    REQUIRE(world.move_kinematic(plate, target, dt));
    REQUIRE(world.step() == Status::Ok);
  }
  step_n(world, 240);

  const f32 recovered = height();
  CHECK(all_finite(std::span<const Vec3>(points)));
  CHECK(recovered > 0.90f * rest_height);
}

TEST_CASE("physics: a spring attachment lags its anchor and a rigid one does not") {
  // The difference ADR-0026 and plan 05 §5.14 care about: "bound" is a stiff spring rather than
  // a weld, so flesh lags a fast bone instead of tracking it exactly. A rigid attachment covers
  // the whole gap every step and cannot be moved by anything; a spring one keeps its mass, so
  // it trails an anchor that is moving and carries momentum when the anchor stops.
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);

  ShapeId capsule;
  REQUIRE(world.create_capsule(0.1f, 0.05f, capsule) == Status::Ok);
  BodyDesc anchor_desc;
  anchor_desc.shape = capsule;
  anchor_desc.transform.position = place(Vec3(0.0f, 2.0f, 0.0f));
  anchor_desc.motion = MotionType::Kinematic;
  anchor_desc.layer = Layer::Kinematic;
  BodyId anchor;
  REQUIRE(world.create_body(anchor_desc, anchor) == Status::Ok);

  // A small cage hanging on the anchor, with vertex 0 attached and gravity off, so the only
  // thing moving vertex 0 is the attachment.
  constexpr u32 k_n = 3;
  const LatticeVolume lattice = build_lattice_volume(k_n, 0.1f, 1.0e-4f, 1.0e-4f);
  const u32 vertex_count = k_n * k_n * k_n;

  constexpr f32 k_sweep_per_step = 0.02f;  // 1.2 m/s at 60 Hz

  // Returns how far particle 0 ends up from its anchor: with `sweep`, after the anchor has been
  // driven sideways at a constant speed; without it, after the cage has been left to hang.
  const auto run = [&](AttachmentKind kind, f32 follow_rate, bool sweep) {
    SoftAttachment attachment;
    attachment.vertex = 0;
    attachment.body = anchor;
    attachment.local_point = Vec3::zero();
    attachment.kind = kind;
    attachment.follow_rate = follow_rate;

    SoftBodyDesc desc;
    desc.vertices = std::span<const Vec3>(lattice.vertices);
    desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
    desc.edges = std::span<const SoftEdge>(lattice.edges);
    desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
    desc.faces = std::span<const u32>(lattice.faces);
    desc.attachments = std::span<const SoftAttachment>(&attachment, 1);
    desc.transform.position = place(Vec3(0.0f, 2.0f, 0.0f));
    desc.iterations = 8;
    desc.gravity_factor = sweep ? 0.0f : 1.0f;
    desc.allow_sleeping = false;
    SoftBodyId cage;
    REQUIRE(world.create_soft_body(desc, cage) == Status::Ok);

    Vector<Vec3> points(vertex_count);
    const f32 dt = world.step_seconds();
    f32 x = 0.0f;
    for (u32 i = 0; i < 120; ++i) {
      if (sweep) x += k_sweep_per_step;
      BodyTransform target;
      target.position = place(Vec3(x, 2.0f, 0.0f));
      REQUIRE(world.move_kinematic(anchor, target, dt));
      REQUIRE(world.step() == Status::Ok);
    }
    REQUIRE(world.read_soft_body_vertices(cage, WorldPos::origin(), std::span<Vec3>(points)) ==
            vertex_count);
    BodyTransform anchor_now;
    REQUIRE(world.body_transform(anchor, anchor_now));
    const f32 offset = length(local_of(anchor_now.position) - points[0]);
    REQUIRE(world.destroy_soft_body(cage));
    // Put the anchor back for the next run.
    REQUIRE(world.set_body_transform(anchor, anchor_desc.transform));
    return offset;
  };

  SUBCASE("a still anchor: rigid holds its particle, spring lets it hang") {
    const f32 rigid = run(AttachmentKind::Rigid, 0.0f, false);
    const f32 spring = run(AttachmentKind::Spring, 30.0f, false);
    INFO("rigid " << rigid << " spring(30/s) " << spring);
    // Zero inverse mass: nothing the solver or the cage's weight does can move it off the
    // anchor, which is the "stiffest form" ADR-0026 asks for.
    CHECK(rigid < 1.0e-4f);
    // The spring one carries its share of a cage hanging off one corner, so it sags. That it
    // sags at all is the whole difference: this particle has mass and can be pushed.
    CHECK(spring > 1.0e-3f);
  }

  SUBCASE("a sweeping anchor: rigid is one step behind, spring is further") {
    const f32 rigid = run(AttachmentKind::Rigid, 0.0f, true);
    const f32 spring = run(AttachmentKind::Spring, 30.0f, true);
    INFO("step travel " << k_sweep_per_step << ", rigid " << rigid << ", spring " << spring);
    // The rigid attachment's velocity covers the gap to where the anchor was when the step
    // began, and the anchor moves during that same step, so a constant-velocity anchor leaves
    // exactly one step of travel between them and never more. That is `move_kinematic`'s own
    // behaviour, in particle form, and it is why the number is this exact rather than small.
    CHECK(rigid == doctest::Approx(k_sweep_per_step).epsilon(0.05));
    // The spring one is further behind, because closing only part of the gap per step while
    // dragging a cage's worth of mass is what lagging means.
    CHECK(spring > 1.4f * k_sweep_per_step);
  }
}

TEST_CASE("physics: a soft body validates its cage before the backend sees it") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  const ClothSheet sheet = build_cloth_sheet(4, 4, 0.1f, 0.0f, 0.0f);

  SoftBodyDesc desc;
  desc.vertices = std::span<const Vec3>(sheet.vertices);
  desc.edges = std::span<const SoftEdge>(sheet.edges);
  SoftBodyId body;

  SUBCASE("an edge that names a vertex nobody has") {
    const SoftEdge bad[1] = {SoftEdge{0, 99, 0.0f}};
    desc.edges = std::span<const SoftEdge>(bad, 1);
    CHECK(world.create_soft_body(desc, body) == Status::InvalidArgument);
  }
  SUBCASE("an inverse-mass list of the wrong length") {
    const f32 masses[2] = {1.0f, 1.0f};
    desc.inverse_masses = std::span<const f32>(masses, 2);
    CHECK(world.create_soft_body(desc, body) == Status::InvalidArgument);
  }
  SUBCASE("an attachment to a body that does not exist") {
    SoftAttachment attachment;
    attachment.vertex = 0;
    attachment.body = BodyId{SlotHandle{7, 3}};
    desc.attachments = std::span<const SoftAttachment>(&attachment, 1);
    CHECK(world.create_soft_body(desc, body) == Status::NotFound);
  }
  SUBCASE("pressure with no surface to measure it over") {
    desc.pressure = 100.0f;
    CHECK(world.create_soft_body(desc, body) == Status::InvalidArgument);
  }
  SUBCASE("zero iterations") {
    desc.iterations = 0;
    CHECK(world.create_soft_body(desc, body) == Status::InvalidArgument);
  }
  SUBCASE("a spring attachment with no follow rate") {
    // It would never move towards its anchor, so it is a free particle wearing an
    // attachment's name; refusing beats behaving as if the attachment were not there.
    SoftAttachment attachment;
    attachment.vertex = 0;
    attachment.kind = AttachmentKind::Spring;
    attachment.follow_rate = 0.0f;
    desc.attachments = std::span<const SoftAttachment>(&attachment, 1);
    CHECK(world.create_soft_body(desc, body) == Status::InvalidArgument);
  }
  SUBCASE("a valid cage") {
    desc.faces = std::span<const u32>(sheet.faces);
    CHECK(world.create_soft_body(desc, body) == Status::Ok);
    CHECK(world.contains(body));
    CHECK(world.destroy_soft_body(body));
    CHECK_FALSE(world.contains(body));
  }
}
