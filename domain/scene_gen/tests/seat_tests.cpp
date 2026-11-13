// A placement seated by its footprint (seat.h; G201 stage 2; scene_gen.md, "Seating by the
// footprint"): the rim's 16 directions, the plane fitted to them, the seat over a plane, a bank and
// a hole, the rule that never raises and keeps a deeper sink, and the bytes pinned.
#include <core/hash/hash.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/seat.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::scene_gen;

namespace {

// The ground as a function of the place in millimetres, metres out.
struct Shape {
  f64 gx = 0.0;  // a plane's gradient
  f64 gz = 0.0;
  f64 base_m = 0.0;   // its height at the origin
  i64 step_x_mm = 0;  // with `step_m`: the ground falls by it past this x
  f64 step_m = 0.0;
};
f64 shape_height(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const Shape& s = *static_cast<const Shape*>(context);
  f64 h =
      s.base_m + s.gx * static_cast<f64>(x_mm) / 1000.0 + s.gz * static_cast<f64>(z_mm) / 1000.0;
  if (s.step_m != 0.0 && x_mm >= s.step_x_mm) h -= s.step_m;
  return h;
}
Ground ground_of(const Shape& shape) {
  Ground g;
  g.surface_fn = &shape_height;
  g.context = &shape;
  return g;
}

}  // namespace

TEST_CASE("seat: the rim's sixteen directions are symmetric, so a plane's fit is exact") {
  for (const i64 r : {i64{1}, i64{7}, i64{390}, i64{412}, i64{1143}, i64{11003}}) {
    i64 sx = 0, sz = 0, sxz = 0, sxx = 0, szz = 0;
    for (u32 k = 0; k < k_seat_rim_samples; ++k) {
      i64 dx = 0, dz = 0;
      seat_rim_offset(r, k, dx, dz);
      sx += dx;
      sz += dz;
      sxz += dx * dz;
      sxx += dx * dx;
      szz += dz * dz;
      // Each on the circle to within the rounding of a millimetre.
      const f64 len = std::sqrt(static_cast<f64>(dx * dx + dz * dz));
      CHECK(std::fabs(len - static_cast<f64>(r)) <= 0.75);
    }
    CHECK(sx == 0);
    CHECK(sz == 0);
    CHECK(sxz == 0);
    CHECK(sxx == szz);
  }
  // The first and the fifth are +x and +z: the directions are the world's, not an instance's.
  i64 dx = 0, dz = 0;
  seat_rim_offset(1000, 0, dx, dz);
  CHECK((dx == 1000 && dz == 0));
  seat_rim_offset(1000, 4, dx, dz);
  CHECK((dx == 0 && dz == 1000));
}

TEST_CASE("seat: the plane fitted to the rim is the ground's, and its words round to the step") {
  Shape s;
  s.gx = 0.25;
  s.gz = -0.6;
  s.base_m = 12.5;
  const Ground g = ground_of(s);
  const GroundDisc d = ground_disc(g, nullptr, 3000, -4000, 600);
  REQUIRE_FALSE(d.centre_hole);
  CHECK(d.rim_samples == k_seat_rim_samples);
  CHECK(d.centre_mm == nearest_mm(12.5 + 0.25 * 3.0 - 0.6 * -4.0));
  // Heights are whole millimetres, so the fit is the plane's within a millimetre over the radius.
  CHECK(std::fabs(d.gx - 0.25) < 0.002);
  CHECK(std::fabs(d.gz + 0.6) < 0.002);
  CHECK(d.residual_mm <= 1.0);
  const GroundSlope slope = d.slope();
  CHECK(std::fabs(slope.x() - 0.25) < 0.002);
  CHECK(std::fabs(slope.z() + 0.6) < 0.002);
  // The words clamp at four and round half away from zero.
  CHECK(quantize_slope(10.0, -10.0) == GroundSlope{32767, -32767});
  CHECK(quantize_slope(0.5 / k_slope_unit, -0.5 / k_slope_unit) == GroundSlope{1, -1});
  CHECK(quantize_slope(0.0, 0.0) == GroundSlope{});
}

TEST_CASE("seat: on any plane to 45 degrees the skirt is enough, and nothing moves") {
  // The tall spruce's rim and skirt (flora.md, "The trunk's skirt"): rho + 0.15 m under the origin.
  const MeshSeat seat{0.35f, 0.50f};
  for (const f64 tan_a : {0.0, 0.2679491924311227, 0.5773502691896257, 1.0}) {
    for (u32 k = 0; k < 8; ++k) {
      Shape s;
      // The fall line turned through eight directions, so the rim's fixed ones see it every way.
      const f64 c[8] = {1.0,  0.7071067811865476,  0.0, -0.7071067811865476,
                        -1.0, -0.7071067811865476, 0.0, 0.7071067811865476};
      s.gx = tan_a * c[k];
      s.gz = tan_a * c[(k + 6) % 8];
      const Ground g = ground_of(s);
      WorldPos at{1.25, 0.0, -2.5};
      at.y = g.surface(at);
      const WorldPos before = at;
      const SeatResult r = seat_on_ground(g, nullptr, seat, Vec3{1.0f, 1.0f, 1.0f}, at);
      CHECK(r.seated);
      CHECK(r.lowered_m == 0.0);
      CHECK(at == before);
    }
  }
}

TEST_CASE("seat: a rim over a bank is lowered until the skirt is under the bank's foot") {
  const MeshSeat seat{0.35f, 0.50f};
  Shape s;
  s.step_x_mm = 200;  // the bank's edge 0.2 m from the trunk's axis, inside the rim
  s.step_m = 1.0;     // a metre down
  const Ground g = ground_of(s);
  WorldPos at{0.0, 0.0, 0.0};
  const SeatResult r = seat_on_ground(g, nullptr, seat, Vec3{1.0f, 1.0f, 1.0f}, at);
  REQUIRE(r.seated);
  // The lowest ground the rim stands over is -1 m; the skirt reaches 0.5 m under the origin, kept
  // 5 cm under it: the origin goes to -1 + 0.5 - 0.05.
  CHECK(at.y == doctest::Approx(-0.55).epsilon(1e-12));
  CHECK(r.lowered_m == doctest::Approx(0.55).epsilon(1e-12));
  // The skirt's bottom is then the margin under the bank's foot.
  CHECK(at.y - 0.5 == doctest::Approx(-1.05).epsilon(1e-12));
  // Seating again is seating once.
  const WorldPos once = at;
  const SeatResult again = seat_on_ground(g, nullptr, seat, Vec3{1.0f, 1.0f, 1.0f}, at);
  CHECK(again.lowered_m == 0.0);
  CHECK(at == once);
  // An instance sunk deeper already (a scatter kind's `sink`) keeps the deeper: never raised.
  WorldPos sunk{0.0, -0.9, 0.0};
  (void)seat_on_ground(g, nullptr, seat, Vec3{1.0f, 1.0f, 1.0f}, sunk);
  CHECK(sunk.y == -0.9);
  // Scaled up, its rim and its skirt both grow: twice the tree has a skirt of 1 m, which reaches
  // under a 1 m bank with nothing to lower but the margin's worth past it.
  WorldPos big{0.0, 0.0, 0.0};
  const SeatResult scaled = seat_on_ground(g, nullptr, seat, Vec3{2.0f, 2.0f, 2.0f}, big);
  CHECK(scaled.seated);
  CHECK(big.y == doctest::Approx(-0.05).epsilon(1e-12));
  // A bank beyond the rim is not the tree's: nothing moves.
  Shape beyond = s;
  beyond.step_x_mm = 400;
  const Ground gf = ground_of(beyond);
  WorldPos clear{0.0, 0.0, 0.0};
  (void)seat_on_ground(gf, nullptr, seat, Vec3{1.0f, 1.0f, 1.0f}, clear);
  CHECK(clear.y == 0.0);
}

TEST_CASE("seat: a hole is not ground, and a centre in one seats nothing") {
  const MeshSeat seat{0.35f, 0.50f};
  Shape s;
  s.step_x_mm = 200;
  s.step_m = 1.0;
  const Ground g = ground_of(s);
  // A hole over the bank's side of the rim: what lies under it is not ground the tree stands over.
  scene::TerrainHole hole;
  hole.center = WorldPos{0.6, 0.0, 0.0};
  hole.radius_m = 0.4f;
  Exclusion exclusion;
  REQUIRE(exclusion.build({&hole, 1}, {}, {}));
  const GroundDisc d = ground_disc(g, &exclusion, 0, 0, 350);
  CHECK_FALSE(d.centre_hole);
  CHECK(d.rim_samples < k_seat_rim_samples);
  WorldPos at{0.0, 0.0, 0.0};
  (void)seat_on_ground(g, &exclusion, seat, Vec3{1.0f, 1.0f, 1.0f}, at);
  // Every sample past the edge at x >= 0.2 m within the rim is in the hole but those at x = 0.247
  // (22.5 degrees off the axis at 0.35 m, z = 0.247): outside a 0.4 m hole round x = 0.6.
  CHECK(at.y <= 0.0);
  WorldPos inside{0.6, 0.0, 0.0};
  const SeatResult r = seat_on_ground(g, &exclusion, seat, Vec3{1.0f, 1.0f, 1.0f}, inside);
  CHECK_FALSE(r.seated);
  CHECK(inside.y == 0.0);
}

// The bytes: a field of discs over a ground of planes and a bank, folded into one hash. Every
// decision is integer millimetres and one IEEE solve, so GCC and Clang at either baseline reproduce
// the hash MSVC took (the containers run this file).
TEST_CASE("seat: the discs over a sloping bank are the same bytes on every toolchain") {
  Shape s;
  s.gx = 0.41;
  s.gz = -0.17;
  s.base_m = 731.25;
  s.step_x_mm = 5300;
  s.step_m = 1.37;
  const Ground g = ground_of(s);
  u64 h = 0;
  for (i64 x = 0; x < 12; ++x) {
    for (i64 z = 0; z < 12; ++z) {
      const GroundDisc d = ground_disc(g, nullptr, 4000 + x * 237, -1000 + z * 311, 250 + x * 61);
      const i64 words[5] = {d.centre_mm, d.min_mm, d.max_mm, d.drop_mm,
                            static_cast<i64>(d.rim_samples)};
      h = hash_combine(h, hash_bytes(words, sizeof(words)));
      const GroundSlope slope = d.slope();
      h = hash_combine(h, hash_bytes(&slope, sizeof(slope)));
      u64 bits = 0;
      std::memcpy(&bits, &d.residual_mm, sizeof(bits));
      h = hash_combine(h, bits);
    }
  }
  MESSAGE("seat discs hash 0x" << std::hex << h);
  CHECK(h == 0xe08927463e2ea8aaull);  // pinned on MSVC (G201 stage 2)
}
