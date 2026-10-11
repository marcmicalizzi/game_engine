// The zero-area repair (docs/subsystems/geometry.md, "Zero-area triangles: the repair"). Every
// case is a handful of triangles laid out by hand, so the right answer — which triangles survive,
// in which order, and where each run starts — can be written down in full.

#include <domain/geometry/zero_area_repair.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

struct Mesh {
  Vector<Vec3> positions;
  Vector<u32> indices;
};

void add_triangle(Mesh& m, u32 a, u32 b, u32 c) {
  m.indices.push_back(a);
  m.indices.push_back(b);
  m.indices.push_back(c);
}

bool repair(Mesh& m, Vector<ZeroAreaRepairRange>& ranges, ZeroAreaRepairReport& report,
            std::string* error = nullptr) {
  return repair_zero_area_triangles(
      std::span<const Vec3>(m.positions.data(), m.positions.size()), m.indices,
      std::span<ZeroAreaRepairRange>(ranges.data(), ranges.size()), report, error);
}

// Two runs, the way two primitives of an exported file sit in one index buffer. Run 0 is a unit
// quad (vertices 0..3), a collinear triangle over three distinct vertices on the quad's diagonal
// line (4, 5, 6: the middle one exactly halfway), and one more sound triangle; run 1 is a second
// quad beside it (7..10) with a triangle between it whose third corner is a vertex of its own at
// the position of another (11, at 8's position: two distinct indices, one point) — what a quad
// remesher leaves when it splits a quad through a corner that sits on the opposite edge.
Mesh make_two_runs(Vector<ZeroAreaRepairRange>& ranges) {
  Mesh m;
  m.positions = {
      Vec3{0, 0, 0}, Vec3{1, 0, 0},       Vec3{0, 1, 0}, Vec3{1, 1, 0},  // 0..3: quad one
      Vec3{0, 0, 1}, Vec3{0.5f, 0.5f, 1}, Vec3{1, 1, 1},                 // 4..6: on one line
      Vec3{2, 0, 0}, Vec3{3, 0, 0},       Vec3{2, 1, 0}, Vec3{3, 1, 0},  // 7..10: quad two
      Vec3{3, 0, 0},                                                     // 11: 8's position
  };
  add_triangle(m, 0, 1, 2);    // run 0, triangle 0
  add_triangle(m, 1, 3, 2);    //        1
  add_triangle(m, 4, 5, 6);    //        2: collinear
  add_triangle(m, 0, 3, 2);    //        3
  add_triangle(m, 7, 8, 9);    // run 1, triangle 4
  add_triangle(m, 8, 11, 10);  //      5: coincident (8 and 11)
  add_triangle(m, 8, 10, 9);   //      6
  ranges.clear();
  ranges.push_back(ZeroAreaRepairRange{0, 12});
  ranges.push_back(ZeroAreaRepairRange{12, 9});
  return m;
}

}  // namespace

TEST_CASE("zero-area repair: the test is the importer's, exactly") {
  const Vec3 a{0.25f, -1.5f, 3.0f};
  const Vec3 b{1.75f, 0.5f, -2.0f};
  const Vec3 c{-0.5f, 2.0f, 0.125f};
  CHECK_FALSE(spans_zero_area(a, b, c));
  // Coincident corners, in each of the three places: an exact zero with no contraction, because
  // `x*y - x*y` rounds both products the same way.
  CHECK(spans_zero_area(a, a, c));
  CHECK(spans_zero_area(a, b, b));
  CHECK(spans_zero_area(c, b, c));
  CHECK(spans_zero_area(a, a, a));
  // Three distinct points on one line, exactly representable.
  CHECK(spans_zero_area(Vec3{0, 0, 0}, Vec3{0.5f, 0.5f, 0.5f}, Vec3{2, 2, 2}));
  // A sliver is not zero area, however thin: the repair is not an area threshold.
  CHECK_FALSE(spans_zero_area(Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0.5f, 1.0e-6f, 0}));
  // -0 is 0: a corner at -0 coincides with one at +0, as `==` says it does.
  CHECK(spans_zero_area(Vec3{0, 0, 0}, Vec3{-0.0f, 0, 0}, Vec3{1, 2, 3}));
}

TEST_CASE("zero-area repair: a collinear and a coincident triangle among sound ones") {
  Vector<ZeroAreaRepairRange> ranges;
  Mesh m = make_two_runs(ranges);
  const Vector<Vec3> positions_before = m.positions;
  ZeroAreaRepairReport report;
  std::string error;
  REQUIRE_MESSAGE(repair(m, ranges, report, &error), error);
  CHECK(report.triangles == 2);
  CHECK(report.collinear == 1);
  CHECK(report.coincident == 1);
  CHECK(report.dropped == 2);
  CHECK(report.unrepaired == 0);
  CHECK(report.repeated == 0);
  CHECK(report.changed());

  // The survivors, in their order, and nothing else.
  const u32 expected[] = {0, 1, 2, 1, 3, 2, 0, 3, 2, 7, 8, 9, 8, 10, 9};
  REQUIRE(m.indices.size() == 15);
  for (u32 i = 0; i < 15; ++i)
    CHECK(m.indices[i] == expected[i]);
  // Each run lost its own triangle and the second moved down by the first's three indices.
  REQUIRE(ranges.size() == 2);
  CHECK(ranges[0].first == 0);
  CHECK(ranges[0].count == 9);
  CHECK(ranges[1].first == 9);
  CHECK(ranges[1].count == 6);
  // Vertices are never touched: every per-vertex stream keeps its length and its values, and the
  // vertices only a dropped triangle used (4, 5, 6, 11) are left for the weld to drop.
  REQUIRE(m.positions.size() == positions_before.size());
  for (u32 v = 0; v < m.positions.size(); ++v) {
    CHECK(m.positions[v].x == positions_before[v].x);
    CHECK(m.positions[v].y == positions_before[v].y);
    CHECK(m.positions[v].z == positions_before[v].z);
  }

  // A second pass has nothing left to do, and changes nothing.
  ZeroAreaRepairReport again;
  REQUIRE(repair(m, ranges, again));
  CHECK(again.triangles == 0);
  CHECK_FALSE(again.changed());
  CHECK(m.indices.size() == 15);
}

TEST_CASE("zero-area repair: a run is never emptied, and a repeated index is not its fault") {
  Mesh m;
  m.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{2, 0, 0}, Vec3{0, 1, 0}, Vec3{1, 1, 0}};
  add_triangle(m, 0, 1, 2);  // run 0: two collinear triangles and nothing else
  add_triangle(m, 2, 1, 0);
  add_triangle(m, 0, 1, 3);  // run 1: a sound triangle and one that repeats an index
  add_triangle(m, 1, 1, 4);
  Vector<ZeroAreaRepairRange> ranges;
  ranges.push_back(ZeroAreaRepairRange{0, 6});
  ranges.push_back(ZeroAreaRepairRange{6, 6});
  ZeroAreaRepairReport report;
  REQUIRE(repair(m, ranges, report));
  CHECK(report.triangles == 2);
  CHECK(report.collinear == 2);
  CHECK(report.dropped == 1);     // the first; the run's last stays...
  CHECK(report.unrepaired == 1);  // ...and is counted, for the content build to refuse
  CHECK(report.repeated == 1);    // left exactly where it was
  const u32 expected[] = {2, 1, 0, 0, 1, 3, 1, 1, 4};
  REQUIRE(m.indices.size() == 9);
  for (u32 i = 0; i < 9; ++i)
    CHECK(m.indices[i] == expected[i]);
  CHECK(ranges[0].first == 0);
  CHECK(ranges[0].count == 3);
  CHECK(ranges[1].first == 3);
  CHECK(ranges[1].count == 6);
}

TEST_CASE(
    "zero-area repair: indices outside every run are carried over, and a clean mesh "
    "is untouched") {
  Mesh m;
  m.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 0}};
  add_triangle(m, 0, 1, 2);  // outside every run
  add_triangle(m, 0, 3, 1);  // run 0: coincident (0 and 3), dropped
  add_triangle(m, 0, 1, 2);
  add_triangle(m, 0, 3, 2);  // outside every run, and zero-area, but nobody's to repair
  Vector<ZeroAreaRepairRange> ranges;
  ranges.push_back(ZeroAreaRepairRange{3, 6});
  ZeroAreaRepairReport report;
  REQUIRE(repair(m, ranges, report));
  CHECK(report.dropped == 1);
  const u32 expected[] = {0, 1, 2, 0, 1, 2, 0, 3, 2};
  REQUIRE(m.indices.size() == 9);
  for (u32 i = 0; i < 9; ++i)
    CHECK(m.indices[i] == expected[i]);
  CHECK(ranges[0].first == 3);
  CHECK(ranges[0].count == 3);

  // A mesh with nothing to repair keeps its very buffer.
  Mesh clean;
  clean.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
  add_triangle(clean, 0, 1, 2);
  const u32* before = clean.indices.data();
  Vector<ZeroAreaRepairRange> one;
  one.push_back(ZeroAreaRepairRange{0, 3});
  ZeroAreaRepairReport nothing;
  REQUIRE(repair(clean, one, nothing));
  CHECK_FALSE(nothing.changed());
  CHECK(clean.indices.data() == before);
}

TEST_CASE("zero-area repair: malformed input is refused with the mesh unchanged") {
  Vector<ZeroAreaRepairRange> ranges;
  Mesh m = make_two_runs(ranges);
  auto refused = [&](Vector<ZeroAreaRepairRange> bad, const char* says) {
    const Vector<u32> before = m.indices;
    ZeroAreaRepairReport report;
    std::string error;
    CHECK_FALSE(repair(m, bad, report, &error));
    CHECK_MESSAGE(error.find(says) != std::string::npos, error);
    CHECK(m.indices == before);
  };
  Vector<ZeroAreaRepairRange> partial;
  partial.push_back(ZeroAreaRepairRange{0, 4});
  refused(partial, "whole number of triangles");
  Vector<ZeroAreaRepairRange> past;
  past.push_back(ZeroAreaRepairRange{0, 24});
  refused(past, "runs past");
  Vector<ZeroAreaRepairRange> backwards;
  backwards.push_back(ZeroAreaRepairRange{12, 9});
  backwards.push_back(ZeroAreaRepairRange{0, 12});
  refused(backwards, "overlaps or precedes");
  m.indices[0] = 99;
  refused(ranges, "outside the 12 vertices");
}
