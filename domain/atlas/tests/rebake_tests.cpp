// The rebake on its own (repack.h, docs/subsystems/atlas.md "The rebake"): coverage, the identity,
// a rotated and a mirrored layout, the normal-map conversion against a known answer, the dilation,
// and the same bytes at any thread count.

#include <core/jobs/job_system.h>
#include <domain/atlas/repack.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::atlas;

namespace {

// A smooth RGBA source whose every texel is distinct enough that a wrong lookup shows.
image::Image gradient_image(u32 size) {
  image::Image im;
  im.width = size;
  im.height = size;
  im.channels = 4;
  im.pixels.resize(size * size * 4u);
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      u8* p = im.pixels.data() + (static_cast<usize>(y) * size + x) * 4u;
      p[0] = static_cast<u8>((x * 255u) / (size - 1));
      p[1] = static_cast<u8>((y * 255u) / (size - 1));
      p[2] = static_cast<u8>(((x + y) * 127u) / (size - 1));
      p[3] = 255u;
    }
  }
  return im;
}

// One constant tangent-space normal, stored as a normal map stores it.
image::Image constant_normal_image(u32 size, f32 x, f32 y, f32 z) {
  image::Image im;
  im.width = size;
  im.height = size;
  im.channels = 4;
  im.pixels.resize(size * size * 4u);
  const f32 len = std::sqrt(x * x + y * y + z * z);
  const u8 c[4] = {static_cast<u8>(std::lround((x / len * 0.5f + 0.5f) * 255.0f)),
                   static_cast<u8>(std::lround((y / len * 0.5f + 0.5f) * 255.0f)),
                   static_cast<u8>(std::lround((z / len * 0.5f + 0.5f) * 255.0f)), 255u};
  for (u32 p = 0; p < size * size; ++p)
    std::memcpy(im.pixels.data() + p * 4u, c, 4);
  return im;
}

// A unit quad in z = 0 facing +z, as two counter-clockwise triangles, corner by corner.
struct Quad {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> old_uv;
  Vector<Vec2> new_uv;
  RebakeTriangles view() const {
    RebakeTriangles t;
    t.old_uv = std::span<const Vec2>(old_uv.data(), old_uv.size());
    t.new_uv = std::span<const Vec2>(new_uv.data(), new_uv.size());
    t.positions = std::span<const Vec3>(positions.data(), positions.size());
    t.normals = std::span<const Vec3>(normals.data(), normals.size());
    return t;
  }
};

// `map_new` places a unit-square UV (the quad's own parameter) into the new atlas; the old UVs
// are the parameter itself, scaled into `old_scale` of the source.
template <class F>
Quad make_quad(f32 old_scale, F map_new) {
  Quad q;
  const Vec2 param[6] = {Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 0}, Vec2{1, 1}, Vec2{0, 1}};
  for (const Vec2 s : param) {
    // Position x along u, y against v (glTF's v runs down the image), so the old frame is the
    // plain one: tangent +x, bitangent -y.
    q.positions.push_back(Vec3{s.x, 1.0f - s.y, 0.0f});
    q.normals.push_back(Vec3{0.0f, 0.0f, 1.0f});
    q.old_uv.push_back(Vec2{s.x * old_scale, s.y * old_scale});
    q.new_uv.push_back(map_new(s));
  }
  return q;
}

u32 texel(const image::Image& im, u32 x, u32 y, u32 c) {
  return im.pixels[(y * im.width + x) * 4u + c];
}

}  // namespace

TEST_CASE(
    "atlas: coverage owns interior texels first, then the ring, and ties go to the lower "
    "triangle") {
  // One triangle over the lower-left half of an 8x8 image, and the same triangle again.
  const Vec2 uv[6] = {Vec2{0, 0}, Vec2{1, 1}, Vec2{0, 1}, Vec2{0, 0}, Vec2{1, 1}, Vec2{0, 1}};
  TexelCoverage coverage;
  REQUIRE(build_coverage(std::span<const Vec2>(uv, 6), 8, coverage));
  u32 interior = 0;
  u32 ring = 0;
  u32 none = 0;
  for (u32 y = 0; y < 8; ++y) {
    for (u32 x = 0; x < 8; ++x) {
      const u32 owner = coverage.owner[y * 8 + x];
      if (owner == TexelCoverage::k_uncovered) {
        ++none;
        continue;
      }
      CHECK((owner & ~TexelCoverage::k_ring) == 0u);  // the duplicate never wins a tie
      if ((owner & TexelCoverage::k_ring) != 0) {
        ++ring;
        CHECK(x > y);  // outside, above the diagonal
      } else {
        ++interior;
        CHECK(x <= y);  // the centre is on or below the diagonal
      }
    }
  }
  CHECK(interior == 36);  // the diagonal's 8 centres lie on the edge and count as inside
  CHECK(ring == 13);      // the next two diagonals, 0.71 and 1.41 texels out; the third is 2.12
  CHECK(none == 15);
}

TEST_CASE("atlas: an identity layout rebakes to the source, sRGB through linear light and back") {
  const image::Image source = gradient_image(32);
  Quad quad = make_quad(1.0f, [](Vec2 s) { return s; });
  TexelCoverage coverage;
  REQUIRE(
      build_coverage(std::span<const Vec2>(quad.new_uv.data(), quad.new_uv.size()), 32, coverage));
  for (const TextureSpace space : {TextureSpace::linear, TextureSpace::srgb}) {
    image::Image out;
    REQUIRE(rebake_image(source, space, NormalMapMode::convert, 1.0f, quad.view(), coverage, 4,
                         nullptr, out));
    REQUIRE(out.pixels.size() == source.pixels.size());
    // Bilinear at a texel centre is the texel, and the sRGB tables round-trip every code.
    CHECK(std::memcmp(out.pixels.data(), source.pixels.data(), out.pixels.size()) == 0);
    const RebakeError e = measure_rebake(source, out, space, 1.0f, quad.view(), 1000);
    CHECK(e.samples == 8);
    CHECK(e.max < 0.5);
  }
}

TEST_CASE("atlas: a rotated, scaled and mirrored layout reads the same texture at the same point") {
  const image::Image source = gradient_image(64);
  // The quad's texture was the top-left quarter of the source; the new atlas gives it the whole
  // image at twice the texels, rotated a quarter turn, and then again mirrored.
  const Quad rotated = make_quad(0.5f, [](Vec2 s) { return Vec2{1.0f - s.y, s.x}; });
  const Quad mirrored = make_quad(0.5f, [](Vec2 s) { return Vec2{1.0f - s.x, s.y}; });
  for (const Quad* quad : {&rotated, &mirrored}) {
    TexelCoverage coverage;
    REQUIRE(build_coverage(std::span<const Vec2>(quad->new_uv.data(), quad->new_uv.size()), 64,
                           coverage));
    image::Image out;
    REQUIRE(rebake_image(source, TextureSpace::linear, NormalMapMode::convert, 1.0f, quad->view(),
                         coverage, 4, nullptr, out));
    const RebakeError e =
        measure_rebake(source, out, TextureSpace::linear, 1.0f, quad->view(), 1000);
    MESSAGE("layout error: mean " << e.mean << ", max " << e.max << " (8-bit units)");
    CHECK(e.samples == 8);
    // A smooth ramp resampled at twice the density: half a unit of rounding, and the ramp's own
    // curvature between two bilinear reconstructions.
    CHECK(e.max < 2.0);
  }
}

TEST_CASE("atlas: a normal map is converted through object space, and resampling it is wrong") {
  // A normal tilted 30 degrees towards +u. On this quad the old frame is tangent +x, bitangent -y
  // (v runs down), so the object-space normal it encodes is (sin 30, 0, cos 30).
  const image::Image source = constant_normal_image(32, 0.5f, 0.0f, 0.8660254f);
  const Quad rotated = make_quad(1.0f, [](Vec2 s) { return Vec2{1.0f - s.y, s.x}; });
  const Quad mirrored = make_quad(1.0f, [](Vec2 s) { return Vec2{1.0f - s.x, s.y}; });
  for (const Quad* quad : {&rotated, &mirrored}) {
    TexelCoverage coverage;
    REQUIRE(build_coverage(std::span<const Vec2>(quad->new_uv.data(), quad->new_uv.size()), 32,
                           coverage));
    image::Image converted;
    image::Image resampled;
    REQUIRE(rebake_image(source, TextureSpace::tangent_normal, NormalMapMode::convert, 1.0f,
                         quad->view(), coverage, 4, nullptr, converted));
    REQUIRE(rebake_image(source, TextureSpace::tangent_normal, NormalMapMode::resample, 1.0f,
                         quad->view(), coverage, 4, nullptr, resampled));
    const RebakeError c =
        measure_rebake(source, converted, TextureSpace::tangent_normal, 1.0f, quad->view(), 1000);
    const RebakeError r =
        measure_rebake(source, resampled, TextureSpace::tangent_normal, 1.0f, quad->view(), 1000);
    MESSAGE("normal map: converted " << c.max << " deg, resampled " << r.max << " deg");
    CHECK(c.samples == 8);
    CHECK(c.max < 1.0);  // 8-bit storage of a unit vector: well under a degree
    // A quarter turn swings the tilt from +u to another axis (41.4 deg between them); a mirror
    // flips it to -u (60 deg).
    CHECK(r.mean > 30.0);
  }
}

TEST_CASE(
    "atlas: the normal scale is undone after the frame, so the renderer reads the same normal") {
  const image::Image source = constant_normal_image(16, 0.3f, -0.2f, 0.9f);
  const Quad rotated = make_quad(1.0f, [](Vec2 s) { return Vec2{1.0f - s.y, s.x}; });
  TexelCoverage coverage;
  REQUIRE(build_coverage(std::span<const Vec2>(rotated.new_uv.data(), rotated.new_uv.size()), 16,
                         coverage));
  image::Image out;
  REQUIRE(rebake_image(source, TextureSpace::tangent_normal, NormalMapMode::convert, 0.6f,
                       rotated.view(), coverage, 4, nullptr, out));
  const RebakeError e =
      measure_rebake(source, out, TextureSpace::tangent_normal, 0.6f, rotated.view(), 1000);
  CHECK(e.max < 1.5);
}

TEST_CASE("atlas: gutters take their neighbours' texels, and the unused atlas the covered mean") {
  // A quad over the left half of a 16x16 atlas: the right half is gutter and unused atlas.
  const image::Image source = gradient_image(16);
  Quad quad = make_quad(1.0f, [](Vec2 s) { return Vec2{s.x * 0.5f, s.y}; });
  TexelCoverage coverage;
  REQUIRE(
      build_coverage(std::span<const Vec2>(quad.new_uv.data(), quad.new_uv.size()), 16, coverage));
  image::Image near_only;
  REQUIRE(rebake_image(source, TextureSpace::linear, NormalMapMode::convert, 1.0f, quad.view(),
                       coverage, 2, nullptr, near_only));
  // Columns 0..7 are inside; 8 and 9 are the ring (0.5 and 1.5 texels out), both sampled at their
  // closest point on the quad's edge, so they agree; 10 and 11 are the two dilation passes, each
  // the mean of the three texels before it — which on the green channel, a ramp down the rows at
  // 17 a row, is the texel beside it.
  // Rows 0 and 15 have one neighbour row, not two, so the second pass reads a mean the ramp does
  // not have; from row 2 in, every neighbourhood is whole.
  for (u32 y = 2; y < 14; ++y) {
    CHECK(texel(near_only, 9, y, 1) == texel(near_only, 8, y, 1));
    CHECK(texel(near_only, 10, y, 1) == texel(near_only, 9, y, 1));
    CHECK(texel(near_only, 11, y, 1) == texel(near_only, 10, y, 1));
  }
  // Column 12 and beyond: nothing reached them, so they hold the mean of the covered texels.
  CHECK(texel(near_only, 15, 0, 0) == texel(near_only, 12, 7, 0));
  CHECK(texel(near_only, 15, 0, 1) == texel(near_only, 12, 7, 1));
  CHECK(texel(near_only, 15, 0, 3) == 255u);
}

TEST_CASE("atlas: the rebake is the same bytes on any number of threads") {
  const image::Image source = gradient_image(128);
  const Quad quad =
      make_quad(0.75f, [](Vec2 s) { return Vec2{0.9f - s.y * 0.8f, 0.1f + s.x * 0.8f}; });
  TexelCoverage coverage;
  REQUIRE(
      build_coverage(std::span<const Vec2>(quad.new_uv.data(), quad.new_uv.size()), 128, coverage));
  image::Image serial;
  REQUIRE(rebake_image(source, TextureSpace::srgb, NormalMapMode::convert, 1.0f, quad.view(),
                       coverage, 16, nullptr, serial));
  for (const u32 workers : {1u, 3u, 8u}) {
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 1;
    config.pin_threads = false;
    jobs::JobSystem pool(config);
    image::Image parallel;
    REQUIRE(rebake_image(source, TextureSpace::srgb, NormalMapMode::convert, 1.0f, quad.view(),
                         coverage, 16, &pool, parallel));
    REQUIRE(parallel.pixels.size() == serial.pixels.size());
    CHECK(std::memcmp(parallel.pixels.data(), serial.pixels.data(), serial.pixels.size()) == 0);
  }
}

TEST_CASE("atlas: malformed rebake input is refused with a sentence") {
  const image::Image source = gradient_image(8);
  TexelCoverage coverage;
  std::string error;
  const Vec2 two[2] = {Vec2{0, 0}, Vec2{1, 1}};
  CHECK_FALSE(build_coverage(std::span<const Vec2>(two, 2), 8, coverage, &error));
  CHECK_FALSE(error.empty());
  error.clear();
  CHECK_FALSE(build_coverage(std::span<const Vec2>(two, 0), 0, coverage, &error));
  CHECK_FALSE(error.empty());

  const Quad quad = make_quad(1.0f, [](Vec2 s) { return s; });
  REQUIRE(
      build_coverage(std::span<const Vec2>(quad.new_uv.data(), quad.new_uv.size()), 8, coverage));
  image::Image rgb = source;
  rgb.channels = 3;
  image::Image out;
  error.clear();
  CHECK_FALSE(rebake_image(rgb, TextureSpace::linear, NormalMapMode::convert, 1.0f, quad.view(),
                           coverage, 2, nullptr, out, &error));
  CHECK_FALSE(error.empty());
  RebakeTriangles no_positions = quad.view();
  no_positions.positions = {};
  error.clear();
  CHECK_FALSE(rebake_image(source, TextureSpace::tangent_normal, NormalMapMode::convert, 1.0f,
                           no_positions, coverage, 2, nullptr, out, &error));
  CHECK_FALSE(error.empty());
}
