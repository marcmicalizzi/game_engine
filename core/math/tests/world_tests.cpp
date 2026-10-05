#include <core/math/world.h>

#include <doctest/doctest.h>

#include <cmath>
#include <type_traits>

using namespace engine;

// What must not compile is the point of the types (ADR-0053): an absolute position does not
// become a float, and two points do not add.
static_assert(!std::is_convertible_v<WorldPos, Vec3>);
static_assert(!std::is_convertible_v<DVec3, Vec3>);
static_assert(!std::is_convertible_v<WorldPos, DVec3>);
static_assert(!std::is_convertible_v<Vec3, WorldPos>);
static_assert(std::is_convertible_v<Vec3, DVec3>);
template <class A, class B>
concept Addable = requires(A a, B b) { a + b; };
static_assert(!Addable<WorldPos, WorldPos>);
static_assert(Addable<WorldPos, DVec3>);
static_assert(Addable<WorldPos, Vec3>);  // a float32 displacement widens exactly

namespace {

// A few places a game might stand: by the origin, the owner's 420 km, 10,000 km, 1e8 m, the edge
// of what a cell can name, and each just either side of a cell's edge, in every sign.
constexpr f64 k_places[] = {0.0,       0.3,        63.999999,   64.0,         64.000001,
                            -0.000001, -64.0,      -63.5,       419070.2,     -419070.2,
                            66781.7,   10000000.4, -10000000.4, 100000000.25, -100000000.25,
                            1.0e11,    -1.0e11};

f64 f32_step(f64 v) {
  const f32 a = std::fabs(static_cast<f32>(v));
  return static_cast<f64>(std::nextafter(a, 1.0e30f)) - static_cast<f64>(a);
}
f64 f64_step(f64 v) { return std::nextafter(std::fabs(v), 1.0e300) - std::fabs(v); }
f64 axis(DVec3 v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }

// What a cell and an eye name, apart, with no absolute number in the sum: the cells' difference
// and the locals', each exact in f64.
DVec3 apart(const WorldCell& p, const WorldEye& e) {
  const auto one = [](i32 pc, f32 pl, i32 ec, f32 el, f32 er) {
    return static_cast<f64>(static_cast<i64>(pc) - static_cast<i64>(ec)) * k_world_cell_m +
           ((static_cast<f64>(pl) - static_cast<f64>(el)) - static_cast<f64>(er));
  };
  return {one(p.cell.x, p.local.x, e.cell.x, e.local.x, e.residual.x),
          one(p.cell.y, p.local.y, e.cell.y, e.local.y, e.residual.y),
          one(p.cell.z, p.local.z, e.cell.z, e.local.z, e.residual.z)};
}

}  // namespace

TEST_CASE("world: points and displacements") {
  constexpr WorldPos a{419070.25, 80.5, -66781.125};
  constexpr WorldPos b{419071.25, 82.5, -66780.125};
  static_assert(b - a == DVec3{1.0, 2.0, 1.0});
  static_assert(a + (b - a) == b);
  static_assert(relative(b, a) == Vec3{1.0f, 2.0f, 1.0f});
  static_assert(absolute(a, Vec3{1.0f, 2.0f, 1.0f}) == b);
  static_assert(lerp(a, b, 0.5) == WorldPos{419070.75, 81.5, -66780.625});

  // A second of walking at 1.5 m/s in 240 Hz ticks, which a float at 420 km loses whole
  // (docs/experiments/far-from-origin-2026-10-04.md): in f64 it arrives, to the ticks' own
  // rounding — 7 nm at 420 km, 2 um at 1e8 m.
  for (const f64 far : {419070.0, 1.0e7, 1.0e8}) {
    WorldPos p{far, 0.0, 0.0};
    const DVec3 tick{1.5 / 240.0, 0.0, 0.0};
    for (int i = 0; i < 240; ++i)
      p += tick;
    CHECK(std::fabs((p - WorldPos{far, 0.0, 0.0}).x - 1.5) <= 120.0 * f64_step(far) + 1.0e-12);
    // and the float does not: at 10,000 km and beyond the same sum in float32 moves nothing
    f32 x = static_cast<f32>(far);
    for (int i = 0; i < 240; ++i)
      x += static_cast<f32>(1.5 / 240.0);
    if (far >= 1.0e7) CHECK(x == static_cast<f32>(far));
  }
}

TEST_CASE("world: a cell and a local name the position to two micrometres") {
  for (const f64 x : k_places) {
    for (const f64 y : {0.0, 80.2, -3.0}) {
      const WorldPos p{x, y, -x * 0.5};
      REQUIRE(world_cell_valid(p));
      const WorldCell c = to_cell(p);
      for (int i = 0; i < 3; ++i) {
        CHECK(c.local[static_cast<usize>(i)] >= 0.0f);
        CHECK(c.local[static_cast<usize>(i)] <= k_world_cell_m_f32);
      }
      // A local below 64 rounds at 3.8 um at the most, so the cell names the position within
      // 1.9 um; f64's own step at the position is added because that is what the check is
      // computed in (15 um at 1e11 m, where it is the larger of the two).
      const DVec3 off = to_world(c) - p;
      CHECK(std::fabs(off.x) <= 1.91e-6 + f64_step(p.x));
      CHECK(std::fabs(off.y) <= 1.91e-6 + f64_step(p.y));
      CHECK(std::fabs(off.z) <= 1.91e-6 + f64_step(p.z));
      // The eye carries its residual, and is the position to f64's step.
      const WorldEye e = to_eye(p);
      const DVec3 eye_off = to_world(e) - p;
      CHECK(std::fabs(eye_off.x) <= f64_step(p.x) + 1.0e-12);
      CHECK(std::fabs(eye_off.y) <= f64_step(p.y) + 1.0e-12);
      CHECK(std::fabs(eye_off.z) <= f64_step(p.z) + 1.0e-12);
      CHECK(e.cell == c.cell);
      CHECK(e.local == c.local);
    }
  }
  CHECK(!world_cell_valid(WorldPos{1.4e11, 0.0, 0.0}));
  CHECK(!world_cell_valid(WorldPos{0.0, -1.4e11, 0.0}));
}

TEST_CASE("world: the shaders' relative position is the true one, wherever the eye stands") {
  // An eye at each place and points round it from a hand's breadth to 80 km: the float32 result
  // against what the cell and the eye name. The bound does not grow with the eye's distance
  // from the origin, which is the whole point: three float operations whose intermediates are
  // never more than about twice the result, so within three float steps at the result's size —
  // 11 nm for a point 5 cm from the eye, whether the eye is by the origin or 1e11 m out, in the
  // middle of a cell or at its edge.
  for (const f64 ex : k_places) {
    const WorldPos eye_pos{ex, 1.65, ex * 0.25 - 7.0};
    const WorldEye eye = to_eye(eye_pos);
    for (const f64 d : {0.05, 0.5, 3.0, 63.0, 64.0, 200.0, 5000.0, 80000.0}) {
      for (const DVec3 dir : {DVec3{1, 0, 0}, DVec3{-1, 0, 0}, DVec3{0.6, 0.0, -0.8},
                              DVec3{0.0, 1.0, 0.0}, DVec3{-0.36, 0.48, 0.8}}) {
        const WorldCell cell = to_cell(eye_pos + dir * d);
        const Vec3 got = relative(cell, eye);
        const DVec3 want = apart(cell, eye);
        for (int i = 0; i < 3; ++i) {
          const f64 w = axis(want, i);
          CAPTURE(ex);
          CAPTURE(d);
          CAPTURE(i);
          CHECK(std::fabs(static_cast<f64>(got[static_cast<usize>(i)]) - w) <=
                3.0 * f32_step(w) + 1.0e-10);
        }
      }
    }
  }
}

TEST_CASE("world: a whole number of cells moves no bit of a relative position") {
  // ADR-0053 decision 6: the scene and the camera translated by whole cells give the shader the
  // same operands, so the same result to the bit. The positions are whole 1024ths of a metre,
  // which f64 still holds exactly after the largest shift here (1e8 m); a position that is not
  // would be rounded by the shift itself, at f64's step, before any of this arithmetic saw it.
  const auto at = [](i64 x, i64 y, i64 z) {
    return WorldPos{static_cast<f64>(x) / 1024.0, static_cast<f64>(y) / 1024.0,
                    static_cast<f64>(z) / 1024.0};
  };
  const WorldPos eye_pos = at(12642, 1690, -41971);
  const WorldPos points[] = {at(12800, 1024, -41984), at(77056, 3584, -102528),
                             at(-3072768, 20480, 4096512), at(12643, 1690, -41882),
                             at(65536, 0, -65536)};
  for (const f64 cells : {6548.0, -6548.0, 156250.0, 1562500.0, -1562500.0}) {
    const DVec3 shift{cells * k_world_cell_m, 0.0, -cells * k_world_cell_m * 0.5};
    const WorldEye eye0 = to_eye(eye_pos);
    const WorldEye eye1 = to_eye(eye_pos + shift);
    CHECK(eye0.local == eye1.local);
    CHECK(eye0.residual == eye1.residual);
    for (const WorldPos& p : points) {
      const WorldCell c0 = to_cell(p);
      const WorldCell c1 = to_cell(p + shift);
      CHECK(c0.local == c1.local);
      const Vec3 r0 = relative(c0, eye0);
      const Vec3 r1 = relative(c1, eye1);
      CHECK(r0.x == r1.x);
      CHECK(r0.y == r1.y);
      CHECK(r0.z == r1.z);
    }
  }
}
