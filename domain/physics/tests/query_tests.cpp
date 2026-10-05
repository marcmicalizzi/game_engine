// Queries and the layer table: ray casts, shape casts, heightfields, and what collides.

#include "physics_test_support.h"

#include <core/containers/vector.h>
#include <domain/physics/physics.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace engine;
using namespace engine::physics;
using namespace engine::physics::testing;

namespace {

// How far the rolling sphere at a far site may stand off the origin's (the far case below).
constexpr f64 k_far_roll_tolerance_m = 1.0e-4;

// A square heightfield whose surface is y = offset.y + slope * (world x - offset.x).
HeightfieldDesc make_slope(Vector<f32>& heights, u32 samples, f32 slope, Vec3 offset) {
  heights.resize(samples * samples);
  for (u32 z = 0; z < samples; ++z)
    for (u32 x = 0; x < samples; ++x)
      heights[z * samples + x] = slope * static_cast<f32>(x);
  HeightfieldDesc desc;
  desc.heights = std::span<const f32>(heights);
  desc.sample_count = samples;
  desc.local_offset = offset;
  desc.scale = Vec3(1.0f, 1.0f, 1.0f);
  return desc;
}

}  // namespace

TEST_CASE("physics: the layer table is symmetric and Query never simulates") {
  for (u32 i = 0; i < k_layer_count; ++i) {
    for (u32 j = 0; j < k_layer_count; ++j) {
      const Layer a = static_cast<Layer>(i);
      const Layer b = static_cast<Layer>(j);
      CHECK(layers_collide(a, b) == layers_collide(b, a));
      if (a == Layer::Query || b == Layer::Query) CHECK_FALSE(layers_collide(a, b));
    }
  }
  // The pairs plan 05 §5.11 asks for by name.
  CHECK_FALSE(layers_collide(Layer::Static, Layer::Static));
  CHECK_FALSE(layers_collide(Layer::Debris, Layer::Debris));
  CHECK_FALSE(layers_collide(Layer::Kinematic, Layer::Kinematic));
  CHECK_FALSE(layers_collide(Layer::Static, Layer::Kinematic));
  CHECK(layers_collide(Layer::Moving, Layer::Static));
  CHECK(layers_collide(Layer::Moving, Layer::Debris));
  CHECK(layers_collide(Layer::Debris, Layer::Static));
  CHECK(layers_collide(Layer::Debris, Layer::Kinematic));

  CHECK(LayerMask::all().test(Layer::Query));
  CHECK_FALSE(LayerMask::of(Layer::Static).test(Layer::Moving));
  CHECK(LayerMask::of(Layer::Static).with(Layer::Moving).test(Layer::Moving));
  CHECK_FALSE(LayerMask::all().without(Layer::Static).test(Layer::Static));
}

TEST_CASE("physics: a ray cast reports the fraction, the body, and the surface normal") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  const BodyId ground = add_ground(world);

  RayHit hit;
  REQUIRE(world.cast_ray(place(Vec3(0.0f, 5.0f, 0.0f)), Vec3(0.0f, -10.0f, 0.0f), hit));
  CHECK(hit.body == ground);
  // The ray's direction carries its length, so the fraction is along that vector: 5 m of 10.
  CHECK(hit.fraction == doctest::Approx(0.5f).epsilon(0.001));
  CHECK(std::abs(local_of(hit.position).y) < 0.001f);
  CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(0.001));

  // A ray that stops short of the surface misses.
  CHECK_FALSE(world.cast_ray(place(Vec3(0.0f, 5.0f, 0.0f)), Vec3(0.0f, -1.0f, 0.0f), hit));
  // A mask that excludes the ground's layer misses too, which is what makes Layer::Query
  // geometry invisible to a gameplay probe and vice versa.
  CHECK_FALSE(world.cast_ray(place(Vec3(0.0f, 5.0f, 0.0f)), Vec3(0.0f, -10.0f, 0.0f), hit,
                             LayerMask::of(Layer::Moving)));
  CHECK(world.cast_ray(place(Vec3(0.0f, 5.0f, 0.0f)), Vec3(0.0f, -10.0f, 0.0f), hit,
                       LayerMask::of(Layer::Static)));

  // Layer::Query geometry is never simulated but is always findable.
  ShapeId probe_shape;
  REQUIRE(world.create_box(Vec3(1.0f, 1.0f, 1.0f), probe_shape) == Status::Ok);
  BodyDesc probe_desc;
  probe_desc.shape = probe_shape;
  probe_desc.transform.position = place(Vec3(0.0f, 3.0f, 0.0f));
  probe_desc.motion = MotionType::Static;
  probe_desc.layer = Layer::Query;
  BodyId probe;
  REQUIRE(world.create_body(probe_desc, probe) == Status::Ok);
  REQUIRE(world.cast_ray(place(Vec3(0.0f, 10.0f, 0.0f)), Vec3(0.0f, -20.0f, 0.0f), hit));
  CHECK(hit.body == probe);
}

TEST_CASE("physics: a shape cast reports where the swept shape first touches") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  const BodyId ground = add_ground(world);

  ShapeId sphere_shape;
  REQUIRE(world.create_sphere(0.5f, sphere_shape) == Status::Ok);
  BodyTransform start;
  start.position = place(Vec3(0.0f, 5.0f, 0.0f));

  ShapeHit hit;
  REQUIRE(world.cast_shape(sphere_shape, start, Vec3(0.0f, -10.0f, 0.0f), hit));
  CHECK(hit.body == ground);
  // The centre travels 4.5 m of the 10 m sweep before the surface of the sphere reaches y = 0.
  CHECK(hit.fraction == doctest::Approx(0.45f).epsilon(0.01));
  CHECK(hit.normal.y > 0.99f);
  CHECK(std::abs(local_of(hit.position).y) < 0.05f);

  // Sweeping away from the ground finds nothing.
  CHECK_FALSE(world.cast_shape(sphere_shape, start, Vec3(0.0f, 10.0f, 0.0f), hit));
}

TEST_CASE("physics: a sphere rests at the right height on a heightfield") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);

  SUBCASE("flat") {
    Vector<f32> heights;
    const HeightfieldDesc desc =
        make_slope(heights, 16, 0.0f, Vec3(-8.0f, 1.0f, -8.0f));  // a plateau at y = 1
    ShapeId field;
    REQUIRE(world.create_heightfield(desc, field) == Status::Ok);
    BodyDesc field_desc;
    field_desc.shape = field;
    field_desc.motion = MotionType::Static;
    field_desc.layer = Layer::Static;
    field_desc.friction = 0.8f;
    BodyId field_body;
    REQUIRE(world.create_body(field_desc, field_body) == Status::Ok);

    ShapeId sphere_shape;
    REQUIRE(world.create_sphere(0.5f, sphere_shape) == Status::Ok);
    BodyDesc sphere_desc;
    sphere_desc.shape = sphere_shape;
    sphere_desc.transform.position = place(Vec3(0.5f, 4.0f, 0.5f));
    BodyId sphere;
    REQUIRE(world.create_body(sphere_desc, sphere) == Status::Ok);

    step_n(world, 300);
    const Vec3 position = position_of(world, sphere);
    CHECK(std::abs(position.y - 1.5f) < 0.03f);
  }

  SUBCASE("sloped") {
    // Surface: y = 0.25 * (x + 8). A sphere on a slope rolls, so the thing to check is not
    // where it stops but that it stays exactly one radius off the surface while it goes.
    constexpr f32 k_slope = 0.25f;
    Vector<f32> heights;
    const HeightfieldDesc desc = make_slope(heights, 16, k_slope, Vec3(-8.0f, 0.0f, -8.0f));
    ShapeId field;
    REQUIRE(world.create_heightfield(desc, field) == Status::Ok);
    BodyDesc field_desc;
    field_desc.shape = field;
    field_desc.motion = MotionType::Static;
    field_desc.layer = Layer::Static;
    field_desc.friction = 0.8f;
    BodyId field_body;
    REQUIRE(world.create_body(field_desc, field_body) == Status::Ok);

    ShapeId sphere_shape;
    REQUIRE(world.create_sphere(0.5f, sphere_shape) == Status::Ok);
    BodyDesc sphere_desc;
    sphere_desc.shape = sphere_shape;
    sphere_desc.transform.position = place(Vec3(0.5f, 4.0f, 0.5f));
    BodyId sphere;
    REQUIRE(world.create_body(sphere_desc, sphere) == Status::Ok);

    step_n(world, 150);
    const Vec3 position = position_of(world, sphere);
    // Distance from the centre to the plane y - 0.25x - 2 = 0.
    const f32 distance =
        (position.y - k_slope * position.x - 2.0f) / std::sqrt(1.0f + k_slope * k_slope);
    CHECK(distance == doctest::Approx(0.5f).epsilon(0.1));
    CHECK(position.x < 0.0f);   // it rolled downhill
    CHECK(position.x > -8.0f);  // and is still on the field
  }
}

namespace {

// A ray, a shape cast and a sphere dropped onto the sloped field, all of it at `site`: what the far
// case below compares with the same at the origin. Every authored position is a whole number of
// 1024ths of a metre, so the site plus it is exact in f64 at every site (physics_test_support.h).
struct Probed {
  RayHit ray;
  ShapeHit cast;
  Vector<DVec3> sphere;  // the sphere's centre from the site after every step
};

Probed probe_at(WorldPos site) {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  Vector<f32> heights;
  const HeightfieldDesc desc = make_slope(heights, 16, 0.25f, Vec3(-8.0f, 0.0f, -8.0f));
  ShapeId field;
  REQUIRE(world.create_heightfield(desc, field) == Status::Ok);
  BodyDesc field_desc;
  field_desc.shape = field;
  field_desc.transform.position = site;
  field_desc.motion = MotionType::Static;
  field_desc.layer = Layer::Static;
  field_desc.friction = 0.8f;
  BodyId field_body;
  REQUIRE(world.create_body(field_desc, field_body) == Status::Ok);

  Probed out;
  REQUIRE(
      world.cast_ray(place(Vec3(0.25f, 10.0f, -0.75f), site), Vec3(0.5f, -20.0f, 0.25f), out.ray));
  ShapeId sphere_shape;
  REQUIRE(world.create_sphere(0.5f, sphere_shape) == Status::Ok);
  BodyTransform start;
  start.position = place(Vec3(-2.0f, 6.0f, 1.0f), site);
  REQUIRE(world.cast_shape(sphere_shape, start, Vec3(0.0f, -10.0f, 0.0f), out.cast));

  BodyDesc sphere_desc;
  sphere_desc.shape = sphere_shape;
  sphere_desc.transform.position = place(Vec3(0.5f, 4.0f, 0.5f), site);
  BodyId sphere;
  REQUIRE(world.create_body(sphere_desc, sphere) == Status::Ok);
  out.sphere.reserve(150);
  for (u32 i = 0; i < 150; ++i) {
    REQUIRE(world.step() == Status::Ok);
    BodyTransform t;
    REQUIRE(world.body_transform(sphere, t));
    out.sphere.push_back(t.position - site);
  }
  return out;
}

// The step of f64 at a site's largest coordinate: where it rounds an absolute position there.
f64 site_step(WorldPos site) {
  const f64 m = std::max(std::abs(site.x), std::max(std::abs(site.y), std::abs(site.z)));
  return m > 0.0 ? std::nextafter(m, 2.0 * m) - m : 0.0;
}

}  // namespace

// **Far from the origin, a query and a dropped body are the origin's** (ADR-0053; physics.md, "Far
// from the origin"). The backend tests a ray and a cast shape in floats relative to the body they
// meet, and the site and every authored offset are exact in f64, so the ray's and the cast's
// fractions are the origin's bits and their points differ only by where f64 rounds the sum at the
// site — at most one step of f64 there (15 nm at 1e8 m). The sphere rolling down the slope is a
// simulation, which carries that rounding through 150 steps of contact; its bound is what was
// measured, with a margin. In float32 the ray's point alone would be off by half a float step:
// 1.6 cm at 419 km.
TEST_CASE("physics: a ray, a shape cast and a rolling sphere at 419 km, 10,000 km and 1e8 m") {
  const Probed home = probe_at(WorldPos::origin());
  for (u32 s = 1; s < 4; ++s) {
    const WorldPos site = k_sites[s];
    const Probed far = probe_at(site);
    const f64 step = site_step(site);
    CHECK(far.ray.fraction == home.ray.fraction);
    CHECK(far.ray.normal == home.ray.normal);
    const DVec3 ray_off = (far.ray.position - site) - (home.ray.position - WorldPos{});
    CHECK(std::abs(ray_off.x) <= step);
    CHECK(std::abs(ray_off.y) <= step);
    CHECK(std::abs(ray_off.z) <= step);
    CHECK(far.cast.fraction == home.cast.fraction);
    const DVec3 cast_off = (far.cast.position - site) - (home.cast.position - WorldPos{});
    CHECK(length(cast_off) <= 2.0 * step);
    f64 worst = 0.0;
    for (u32 i = 0; i < home.sphere.size(); ++i)
      worst = std::max(worst, length(far.sphere[i] - home.sphere[i]));
    MESSAGE(site_name(s) << ": the ray's point off by (" << ray_off.x << ", " << ray_off.y << ", "
                         << ray_off.z << ") m, the cast's by " << length(cast_off)
                         << " m, the rolling sphere by at most " << worst << " m (f64's step "
                         << step << " m)");
    CHECK(worst < k_far_roll_tolerance_m);
  }
}

TEST_CASE("physics: a heightfield refuses a grid the backend cannot store") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  Vector<f32> heights;
  ShapeId field;

  HeightfieldDesc desc = make_slope(heights, 16, 0.0f, Vec3::zero());
  desc.sample_count = 15;  // odd: Jolt stores the field in 2x2 blocks
  CHECK(world.create_heightfield(desc, field) == Status::InvalidArgument);

  desc.sample_count = 2;  // fewer than two blocks across
  CHECK(world.create_heightfield(desc, field) == Status::InvalidArgument);

  desc = make_slope(heights, 16, 0.0f, Vec3::zero());
  desc.heights = desc.heights.subspan(0, 10);  // not sample_count squared
  CHECK(world.create_heightfield(desc, field) == Status::InvalidArgument);
}

TEST_CASE("physics: a convex hull and a compound build from engine data") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);

  const Vec3 tetra[4] = {Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                         Vec3(0.0f, 0.0f, 1.0f)};
  ShapeId hull;
  REQUIRE(world.create_convex_hull(std::span<const Vec3>(tetra), hull) == Status::Ok);
  // Bounds come back in the frame the points were given in, not in the backend's
  // centre-of-mass frame: a hull built from points in [0, 1] reports [0, 1].
  Aabb3 bounds;
  REQUIRE(world.shape_bounds(hull, bounds));
  CHECK(std::abs(bounds.min.x) < 0.05f);
  CHECK(bounds.max.x == doctest::Approx(1.0f).epsilon(0.05));

  ShapeId unused;
  CHECK(world.create_convex_hull(std::span<const Vec3>(tetra).subspan(0, 3), unused) ==
        Status::InvalidArgument);

  ShapeId box_shape;
  REQUIRE(world.create_box(Vec3(0.5f, 0.5f, 0.5f), box_shape) == Status::Ok);
  CompoundChild children[2];
  children[0].shape = box_shape;
  children[0].transform.position = Vec3(-1.0f, 0.0f, 0.0f);
  children[1].shape = box_shape;
  children[1].transform.position = Vec3(1.0f, 0.0f, 0.0f);
  ShapeId compound;
  REQUIRE(world.create_compound(std::span<const CompoundChild>(children, 2), compound) ==
          Status::Ok);
  REQUIRE(world.shape_bounds(compound, bounds));
  CHECK(bounds.min.x == doctest::Approx(-1.5f).epsilon(0.02));
  CHECK(bounds.max.x == doctest::Approx(1.5f).epsilon(0.02));

  // A compound of one is a placed shape rather than a refusal.
  ShapeId single;
  REQUIRE(world.create_compound(std::span<const CompoundChild>(children, 1), single) == Status::Ok);
  REQUIRE(world.shape_bounds(single, bounds));
  CHECK(bounds.min.x == doctest::Approx(-1.5f).epsilon(0.02));
  CHECK(bounds.max.x == doctest::Approx(-0.5f).epsilon(0.02));
}
