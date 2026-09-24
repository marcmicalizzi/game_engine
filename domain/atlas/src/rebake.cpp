// The rebake: one image from the old UV space into the new one (repack.h, docs/subsystems/atlas.md
// "The rebake"). Three passes over a square image:
//
//   1. coverage (`build_coverage`): which triangle owns each texel — interior first, then the ring
//      bilinear filtering reaches from inside a chart;
//   2. shading (`rebake_image`): each owned texel samples the source at the old UV of its point,
//      one row per job, each row a pure function of the inputs;
//   3. dilation: covered texels spread into the gutters, pass by pass, reading only texels a
//      previous pass filled, so a pass is a pure function of the one before it.
//
// Nothing here depends on how many threads ran it: every texel is written by exactly one row job
// and computed from data no job writes.

#include <core/jobs/job_system.h>
#include <domain/atlas/repack.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine::atlas {

namespace {

// A ring texel is one whose centre lies outside every triangle but within this many texels of
// one. Bilinear filtering at a point inside a chart reads texels whose centres are within one
// texel of it on each axis, so a texel centre within sqrt(2) of the chart can be read; 1.5 covers
// that. The packer keeps charts `padding` texels further apart than the ring (xatlas `bilinear`),
// so two charts' rings never meet.
constexpr f64 k_ring_radius = 1.5;

// The renderer's `tangent_frame` thresholds (domain/gfx/shaders/material.slang), mirrored so that
// a triangle the renderer gives no frame gets none here either.
constexpr f64 k_frame_area_epsilon = 1e-12;
constexpr f64 k_frame_length_epsilon = 1e-12;

struct D2 {
  f64 x = 0.0;
  f64 y = 0.0;
};

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

D2 operator-(D2 a, D2 b) noexcept { return D2{a.x - b.x, a.y - b.y}; }
D2 operator+(D2 a, D2 b) noexcept { return D2{a.x + b.x, a.y + b.y}; }
D2 operator*(D2 a, f64 s) noexcept { return D2{a.x * s, a.y * s}; }
f64 cross2(D2 a, D2 b) noexcept { return a.x * b.y - a.y * b.x; }
f64 dot2(D2 a, D2 b) noexcept { return a.x * b.x + a.y * b.y; }

D3 operator-(D3 a, D3 b) noexcept { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator+(D3 a, D3 b) noexcept { return D3{a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator*(D3 a, f64 s) noexcept { return D3{a.x * s, a.y * s, a.z * s}; }
f64 dot3(D3 a, D3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross3(D3 a, D3 b) noexcept {
  return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f64 length3(D3 a) noexcept { return std::sqrt(dot3(a, a)); }

D2 to_d(Vec2 v) noexcept { return D2{static_cast<f64>(v.x), static_cast<f64>(v.y)}; }
D3 to_d(Vec3 v) noexcept {
  return D3{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}

// ---- sRGB -----------------------------------------------------------------------------------

struct SrgbTables {
  f32 to_linear[256] = {};
  // Linear light quantized to 16 bits, to the nearest 8-bit sRGB code. 64 KiB, exact to the code.
  u8 from_linear[65536] = {};
};

SrgbTables make_srgb_tables() {
  SrgbTables t;
  for (u32 i = 0; i < 256; ++i) {
    const f64 c = static_cast<f64>(i) / 255.0;
    const f64 l = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    t.to_linear[i] = static_cast<f32>(l);
  }
  for (u32 i = 0; i < 65536; ++i) {
    const f64 l = static_cast<f64>(i) / 65535.0;
    const f64 c = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
    const f64 code = std::floor(std::clamp(c, 0.0, 1.0) * 255.0 + 0.5);
    t.from_linear[i] = static_cast<u8>(code);
  }
  return t;
}

const SrgbTables& srgb_tables() {
  static const SrgbTables tables = make_srgb_tables();  // magic static: thread-safe
  return tables;
}

u8 encode_unit(f32 v) noexcept {
  const f32 c = std::clamp(v, 0.0f, 1.0f);
  return static_cast<u8>(std::floor(c * 255.0f + 0.5f));
}

u8 encode_srgb(f32 linear) noexcept {
  const f32 c = std::clamp(linear, 0.0f, 1.0f);
  return srgb_tables().from_linear[static_cast<u32>(std::floor(c * 65535.0f + 0.5f))];
}

// ---- sampling -------------------------------------------------------------------------------

i64 wrap(i64 i, i64 n) noexcept {
  const i64 r = i % n;
  return r < 0 ? r + n : r;
}

// Bilinear with repeat wrap — glTF's default sampler, and the one the renderer binds — at a UV in
// image space, texel centres at half-integers. `linearize` converts the colour channels through
// the sRGB curve before filtering (alpha is always linear), which is what the GPU does when it
// filters an sRGB texture. Out: four channels in 0..1, linear light when linearized.
void sample_bilinear(const image::Image& image, D2 uv, bool linearize, f32 out[4]) noexcept {
  f64 u = uv.x;
  f64 v = uv.y;
  if (!std::isfinite(u)) u = 0.0;
  if (!std::isfinite(v)) v = 0.0;
  const i64 w = image.width;
  const i64 h = image.height;
  const f64 x = u * static_cast<f64>(w) - 0.5;
  const f64 y = v * static_cast<f64>(h) - 0.5;
  const f64 fx = std::floor(x);
  const f64 fy = std::floor(y);
  // A UV this far out is a broken unwrap (the build warns at 16 wraps); fold it before the cast.
  const f64 k_fold = 1.0e15;
  const i64 x0 = static_cast<i64>(std::fmod(fx, k_fold));
  const i64 y0 = static_cast<i64>(std::fmod(fy, k_fold));
  const f32 tx = static_cast<f32>(x - fx);
  const f32 ty = static_cast<f32>(y - fy);
  const i64 xs[2] = {wrap(x0, w), wrap(x0 + 1, w)};
  const i64 ys[2] = {wrap(y0, h), wrap(y0 + 1, h)};
  const f32 wx[2] = {1.0f - tx, tx};
  const f32 wy[2] = {1.0f - ty, ty};
  const SrgbTables& srgb = srgb_tables();
  for (u32 c = 0; c < 4; ++c)
    out[c] = 0.0f;
  for (u32 j = 0; j < 2; ++j) {
    for (u32 i = 0; i < 2; ++i) {
      const f32 weight = wx[i] * wy[j];
      const u8* p =
          image.pixels.data() +
          (static_cast<usize>(ys[j]) * static_cast<usize>(w) + static_cast<usize>(xs[i])) * 4u;
      for (u32 c = 0; c < 3; ++c)
        out[c] += weight * (linearize ? srgb.to_linear[p[c]] : static_cast<f32>(p[c]) / 255.0f);
      out[3] += weight * (static_cast<f32>(p[3]) / 255.0f);
    }
  }
}

// ---- triangles ------------------------------------------------------------------------------

struct TexTriangle {
  D2 p[3];
  f64 area = 0.0;  // signed, in texels²
};

TexTriangle texel_triangle(const Vec2* uv, f64 size) noexcept {
  TexTriangle t;
  for (u32 k = 0; k < 3; ++k)
    t.p[k] = to_d(uv[k]) * size;
  t.area = cross2(t.p[1] - t.p[0], t.p[2] - t.p[0]);
  return t;
}

// Barycentrics of `p` in `t` (whose area is not zero).
void barycentrics(const TexTriangle& t, D2 p, f64 out[3]) noexcept {
  const f64 inv = 1.0 / t.area;
  out[0] = cross2(t.p[1] - p, t.p[2] - p) * inv;
  out[1] = cross2(t.p[2] - p, t.p[0] - p) * inv;
  out[2] = 1.0 - out[0] - out[1];
}

D2 closest_on_segment(D2 p, D2 a, D2 b) noexcept {
  const D2 ab = b - a;
  const f64 len2 = dot2(ab, ab);
  const f64 s = len2 > 0.0 ? std::clamp(dot2(p - a, ab) / len2, 0.0, 1.0) : 0.0;
  return a + ab * s;
}

// The closest point of the triangle's boundary to `p` (p is outside, or the question is only
// asked of texels outside every triangle), and its distance.
D2 closest_on_triangle(const TexTriangle& t, D2 p, f64& distance) noexcept {
  D2 best{};
  f64 best_d2 = std::numeric_limits<f64>::infinity();
  for (u32 k = 0; k < 3; ++k) {
    const D2 q = closest_on_segment(p, t.p[k], t.p[(k + 1) % 3]);
    const D2 d = p - q;
    const f64 d2 = dot2(d, d);
    if (d2 < best_d2) {
      best_d2 = d2;
      best = q;
    }
  }
  distance = std::sqrt(best_d2);
  return best;
}

bool inside(const TexTriangle& t, D2 p) noexcept {
  const f64 sign = t.area > 0.0 ? 1.0 : -1.0;
  return cross2(t.p[1] - t.p[0], p - t.p[0]) * sign >= 0.0 &&
         cross2(t.p[2] - t.p[1], p - t.p[1]) * sign >= 0.0 &&
         cross2(t.p[0] - t.p[2], p - t.p[2]) * sign >= 0.0;
}

// The texel range whose centres (x + 0.5) can fall within [lo - margin, hi + margin].
void texel_range(f64 lo, f64 hi, f64 margin, u32 size, u32& first, u32& last) noexcept {
  const f64 a = std::ceil(lo - margin - 0.5);
  const f64 b = std::floor(hi + margin - 0.5);
  const f64 top = static_cast<f64>(size) - 1.0;
  first = static_cast<u32>(std::clamp(a, 0.0, top));
  last = static_cast<u32>(std::clamp(b, 0.0, top));
  if (b < 0.0 || a > top) {
    first = 1;
    last = 0;  // empty
  }
}

// The renderer's frame (material.slang `tangent_frame`): tangent and bitangent from the triangle's
// position edges and UV deltas, then Gram-Schmidt against `normal`. False where the renderer
// would not perturb the normal at all.
bool tangent_frame(const D3 p[3], const D2 uv[3], D3 normal, D3& tangent, D3& bitangent) noexcept {
  const D3 e1 = p[1] - p[0];
  const D3 e2 = p[2] - p[0];
  const D2 d1 = uv[1] - uv[0];
  const D2 d2 = uv[2] - uv[0];
  const f64 area = d1.x * d2.y - d2.x * d1.y;
  if (std::fabs(area) < k_frame_area_epsilon) return false;
  const f64 r = 1.0 / area;
  D3 t = (e1 * d2.y - e2 * d1.y) * r;
  D3 b = (e2 * d1.x - e1 * d2.x) * r;
  t = t - normal * dot3(normal, t);
  const f64 t_length = length3(t);
  if (t_length < k_frame_length_epsilon) return false;
  tangent = t * (1.0 / t_length);
  b = b - (normal * dot3(normal, b) + tangent * dot3(tangent, b));
  const f64 b_length = length3(b);
  if (b_length < k_frame_length_epsilon) return false;
  bitangent = b * (1.0 / b_length);
  return true;
}

// The shading normal at barycentrics `l` of corner-triple `t`: the interpolated vertex normal, or
// the face normal when the mesh gave none.
D3 shading_normal(const RebakeTriangles& tris, usize t, const f64 l[3]) noexcept {
  D3 n{};
  if (tris.normals.size() == tris.old_uv.size()) {
    for (u32 k = 0; k < 3; ++k)
      n = n + to_d(tris.normals[t * 3 + k]) * l[k];
  }
  f64 len = length3(n);
  if (len < 1e-20) {
    n = cross3(to_d(tris.positions[t * 3 + 1]) - to_d(tris.positions[t * 3]),
               to_d(tris.positions[t * 3 + 2]) - to_d(tris.positions[t * 3]));
    len = length3(n);
  }
  return len < 1e-30 ? D3{0.0, 0.0, 1.0} : n * (1.0 / len);
}

// A stored tangent-space sample (0..1 per channel) to the object-space normal the renderer would
// shade with, through the frame of `uv` (material.slang `apply_normal_map`).
D3 decode_normal(const f32 stored[3], f32 scale, const D3 p[3], const D2 uv[3], D3 n) noexcept {
  D3 tangent{};
  D3 bitangent{};
  if (!tangent_frame(p, uv, n, tangent, bitangent)) return n;
  D3 v{static_cast<f64>(stored[0]) * 2.0 - 1.0, static_cast<f64>(stored[1]) * 2.0 - 1.0,
       static_cast<f64>(stored[2]) * 2.0 - 1.0};
  v.x *= static_cast<f64>(scale);
  v.y *= static_cast<f64>(scale);
  const f64 len2 = dot3(v, v);
  if (len2 <= 1e-12) return n;
  v = v * (1.0 / std::sqrt(len2));
  const D3 out = tangent * v.x + bitangent * v.y + n * v.z;
  const f64 len = length3(out);
  return len < 1e-30 ? n : out * (1.0 / len);
}

// The inverse: the stored 0..1 triple that decodes to `object` through the frame of `uv`. Where
// the renderer builds no frame the map is not read at all, and the flat normal is stored.
void encode_normal(D3 object, f32 scale, const D3 p[3], const D2 uv[3], D3 n,
                   f32 stored[3]) noexcept {
  D3 tangent{};
  D3 bitangent{};
  D3 ts{0.0, 0.0, 1.0};
  if (tangent_frame(p, uv, n, tangent, bitangent)) {
    ts = D3{dot3(tangent, object), dot3(bitangent, object), dot3(n, object)};
    if (scale != 0.0f) {
      ts.x /= static_cast<f64>(scale);
      ts.y /= static_cast<f64>(scale);
    }
  }
  stored[0] = static_cast<f32>(std::clamp(ts.x, -1.0, 1.0) * 0.5 + 0.5);
  stored[1] = static_cast<f32>(std::clamp(ts.y, -1.0, 1.0) * 0.5 + 0.5);
  stored[2] = static_cast<f32>(std::clamp(ts.z, -1.0, 1.0) * 0.5 + 0.5);
}

void corner_data(const RebakeTriangles& tris, usize t, D3 p[3], D2 old_uv[3],
                 D2 new_uv[3]) noexcept {
  for (u32 k = 0; k < 3; ++k) {
    p[k] = tris.positions.empty() ? D3{} : to_d(tris.positions[t * 3 + k]);
    old_uv[k] = to_d(tris.old_uv[t * 3 + k]);
    new_uv[k] = to_d(tris.new_uv[t * 3 + k]);
  }
}

template <class F>
void for_rows(jobs::JobSystem* pool, u32 rows, F&& body) {
  if (pool != nullptr) {
    pool->parallel_for(jobs::Pool::Performance, rows, 8, [&](u32 begin, u32 end) {
      for (u32 y = begin; y < end; ++y)
        body(y);
    });
  } else {
    for (u32 y = 0; y < rows; ++y)
      body(y);
  }
}

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

}  // namespace

// ---- coverage -------------------------------------------------------------------------------

bool build_coverage(std::span<const Vec2> new_uv, u32 size, TexelCoverage& out,
                    std::string* error) {
  out.size = 0;
  out.owner.clear();
  if (size == 0 || size > 16384) return fail(error, "rebake size is not in 1..16384");
  if (new_uv.size() % 3 != 0) return fail(error, "rebake UVs are not three per triangle");
  const usize triangle_count = new_uv.size() / 3;
  if (triangle_count >= TexelCoverage::k_ring)
    return fail(error, "too many triangles for one rebake");
  const u32 texels = size * size;  // size <= 16384: fits, and so does 4 bytes a texel
  out.size = size;
  out.owner.assign(texels, TexelCoverage::k_uncovered);
  const f64 s = static_cast<f64>(size);

  // Interior: the first triangle, in index order, whose closed interior holds the centre.
  for (usize t = 0; t < triangle_count; ++t) {
    const TexTriangle tri = texel_triangle(&new_uv[t * 3], s);
    if (tri.area == 0.0 || !std::isfinite(tri.area)) continue;
    u32 x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    texel_range(std::min({tri.p[0].x, tri.p[1].x, tri.p[2].x}),
                std::max({tri.p[0].x, tri.p[1].x, tri.p[2].x}), 0.0, size, x0, x1);
    texel_range(std::min({tri.p[0].y, tri.p[1].y, tri.p[2].y}),
                std::max({tri.p[0].y, tri.p[1].y, tri.p[2].y}), 0.0, size, y0, y1);
    for (u32 y = y0; y <= y1 && y0 <= y1; ++y) {
      for (u32 x = x0; x <= x1 && x0 <= x1; ++x) {
        u32& owner = out.owner[y * size + x];
        if (owner != TexelCoverage::k_uncovered) continue;
        if (inside(tri, D2{static_cast<f64>(x) + 0.5, static_cast<f64>(y) + 0.5}))
          owner = static_cast<u32>(t);
      }
    }
  }

  // The ring: texels outside every triangle, to the nearest triangle within the radius. Ties keep
  // the earlier triangle (a strictly smaller distance is needed to take a texel over).
  Vector<f32> distance(texels, std::numeric_limits<f32>::infinity());
  for (usize t = 0; t < triangle_count; ++t) {
    const TexTriangle tri = texel_triangle(&new_uv[t * 3], s);
    if (tri.area == 0.0 || !std::isfinite(tri.area)) continue;
    u32 x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    texel_range(std::min({tri.p[0].x, tri.p[1].x, tri.p[2].x}),
                std::max({tri.p[0].x, tri.p[1].x, tri.p[2].x}), k_ring_radius, size, x0, x1);
    texel_range(std::min({tri.p[0].y, tri.p[1].y, tri.p[2].y}),
                std::max({tri.p[0].y, tri.p[1].y, tri.p[2].y}), k_ring_radius, size, y0, y1);
    for (u32 y = y0; y <= y1 && y0 <= y1; ++y) {
      for (u32 x = x0; x <= x1 && x0 <= x1; ++x) {
        const u32 p = y * size + x;
        const u32 owner = out.owner[p];
        if (owner != TexelCoverage::k_uncovered && (owner & TexelCoverage::k_ring) == 0) continue;
        f64 d = 0.0;
        closest_on_triangle(tri, D2{static_cast<f64>(x) + 0.5, static_cast<f64>(y) + 0.5}, d);
        if (d > k_ring_radius) continue;
        const f32 df = static_cast<f32>(d);
        if (df < distance[p]) {
          distance[p] = df;
          out.owner[p] = static_cast<u32>(t) | TexelCoverage::k_ring;
        }
      }
    }
  }
  return true;
}

// ---- the rebake -------------------------------------------------------------------------------

bool rebake_image(const image::Image& source, TextureSpace space, NormalMapMode mode,
                  f32 normal_scale, const RebakeTriangles& triangles, const TexelCoverage& coverage,
                  u32 dilation, jobs::JobSystem* pool, image::Image& out, std::string* error) {
  out = image::Image{};
  if (source.channels != 4 || source.width == 0 || source.height == 0 ||
      source.pixels.size() != static_cast<usize>(source.width) * source.height * 4u) {
    return fail(error, "rebake source is not an RGBA8 image");
  }
  const usize corners = triangles.old_uv.size();
  if (corners % 3 != 0 || triangles.new_uv.size() != corners)
    return fail(error, "rebake triangles need old and new UVs, three per triangle");
  const bool convert = space == TextureSpace::tangent_normal && mode == NormalMapMode::convert;
  if (convert && triangles.positions.size() != corners)
    return fail(error, "a normal map conversion needs the triangles' positions");
  if (convert && !triangles.normals.empty() && triangles.normals.size() != corners)
    return fail(error, "rebake normals are not three per triangle");
  const u32 size = coverage.size;
  if (size == 0 || size > 16384 || coverage.owner.size() != size * size)
    return fail(error, "rebake coverage is empty");

  out.width = size;
  out.height = size;
  out.channels = 4;
  out.pixels.assign(size * size * 4u, u8{0});
  Vector<u8> filled(size * size, u8{0});
  const f64 s = static_cast<f64>(size);
  const bool linearize = space == TextureSpace::srgb;

  for_rows(pool, size, [&](u32 y) {
    for (u32 x = 0; x < size; ++x) {
      const u32 p = y * size + x;
      const u32 owner = coverage.owner[p];
      if (owner == TexelCoverage::k_uncovered) continue;
      const u32 t = owner & ~TexelCoverage::k_ring;
      const TexTriangle tri = texel_triangle(&triangles.new_uv[t * 3], s);
      D2 point{static_cast<f64>(x) + 0.5, static_cast<f64>(y) + 0.5};
      if ((owner & TexelCoverage::k_ring) != 0) {
        f64 d = 0.0;
        point = closest_on_triangle(tri, point, d);
      }
      f64 l[3];
      barycentrics(tri, point, l);
      // Clamped and renormalized: a ring texel's closest point is on an edge, where rounding can
      // put a barycentric a hair outside [0, 1].
      f64 sum = 0.0;
      for (f64& v : l) {
        v = std::clamp(v, 0.0, 1.0);
        sum += v;
      }
      for (f64& v : l)
        v = sum > 0.0 ? v / sum : 1.0 / 3.0;
      D3 pos[3];
      D2 old_uv[3];
      D2 new_uv[3];
      corner_data(triangles, t, pos, old_uv, new_uv);
      const D2 uv = old_uv[0] * l[0] + old_uv[1] * l[1] + old_uv[2] * l[2];
      f32 texel[4];
      sample_bilinear(source, uv, linearize, texel);
      u8* dst = out.pixels.data() + p * 4u;
      if (convert) {
        const D3 n = shading_normal(triangles, t, l);
        const D3 object = decode_normal(texel, normal_scale, pos, old_uv, n);
        f32 stored[3];
        encode_normal(object, normal_scale, pos, new_uv, n, stored);
        for (u32 c = 0; c < 3; ++c)
          dst[c] = encode_unit(stored[c]);
      } else if (linearize) {
        for (u32 c = 0; c < 3; ++c)
          dst[c] = encode_srgb(texel[c]);
      } else {
        for (u32 c = 0; c < 3; ++c)
          dst[c] = encode_unit(texel[c]);
      }
      dst[3] = encode_unit(texel[3]);
      filled[p] = 1u;
    }
  });

  // The covered mean, in row order, for whatever the dilation does not reach.
  u64 totals[4] = {0, 0, 0, 0};
  u64 covered = 0;
  for (u32 p = 0; p < filled.size(); ++p) {
    if (filled[p] == 0) continue;
    ++covered;
    for (u32 c = 0; c < 4; ++c)
      totals[c] += out.pixels[p * 4u + c];
  }

  // Dilation: an empty texel with a filled 8-neighbour takes the mean of its filled neighbours as
  // they were before this pass. Reads only texels `filled` marks and writes only texels it does
  // not, so the rows of one pass never see each other's writes.
  Vector<u8> next;
  Vector<u32> changed(size, 0u);
  for (u32 pass = 0; pass < dilation; ++pass) {
    next = filled;
    for_rows(pool, size, [&](u32 y) {
      u32 count_changed = 0;
      for (u32 x = 0; x < size; ++x) {
        const u32 p = y * size + x;
        if (filled[p] != 0) continue;
        u32 sum[4] = {0, 0, 0, 0};
        u32 n = 0;
        for (i32 dy = -1; dy <= 1; ++dy) {
          const i64 yy = static_cast<i64>(y) + dy;
          if (yy < 0 || yy >= static_cast<i64>(size)) continue;
          for (i32 dx = -1; dx <= 1; ++dx) {
            const i64 xx = static_cast<i64>(x) + dx;
            if ((dx == 0 && dy == 0) || xx < 0 || xx >= static_cast<i64>(size)) continue;
            const u32 q = static_cast<u32>(yy) * size + static_cast<u32>(xx);
            if (filled[q] == 0) continue;
            ++n;
            for (u32 c = 0; c < 4; ++c)
              sum[c] += out.pixels[q * 4u + c];
          }
        }
        if (n == 0) continue;
        for (u32 c = 0; c < 4; ++c)
          out.pixels[p * 4u + c] = static_cast<u8>((sum[c] + n / 2) / n);
        next[p] = 1u;
        ++count_changed;
      }
      changed[y] = count_changed;
    });
    u64 total_changed = 0;
    for (const u32 c : changed)
      total_changed += c;
    filled.swap(next);
    if (total_changed == 0) break;
  }
  if (covered != 0) {
    u8 mean[4];
    for (u32 c = 0; c < 4; ++c)
      mean[c] = static_cast<u8>((totals[c] + covered / 2) / covered);
    for (u32 p = 0; p < filled.size(); ++p) {
      if (filled[p] != 0) continue;
      std::memcpy(out.pixels.data() + p * 4u, mean, 4);
    }
  }
  return true;
}

// ---- the error --------------------------------------------------------------------------------

RebakeError measure_rebake(const image::Image& source, const image::Image& rebaked,
                           TextureSpace space, f32 normal_scale, const RebakeTriangles& triangles,
                           u32 max_samples) {
  RebakeError out;
  const usize corners = triangles.old_uv.size();
  if (max_samples == 0 || corners == 0 || corners % 3 != 0 || triangles.new_uv.size() != corners ||
      source.channels != 4 || rebaked.channels != 4 || source.pixels.empty() ||
      rebaked.pixels.empty()) {
    return out;
  }
  const bool normal_map = space == TextureSpace::tangent_normal;
  if (normal_map && triangles.positions.size() != corners) return out;
  // Four points a triangle — the centroid and three toward the corners — at a fixed stride.
  constexpr f64 k_points[4][3] = {{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0},
                                  {2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0},
                                  {1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0},
                                  {1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0}};
  const usize triangle_count = corners / 3;
  const usize total = triangle_count * 4;
  const usize stride = std::max<usize>(1, (total + max_samples - 1) / max_samples);
  Vector<f64> errors;
  errors.reserve(static_cast<u32>(std::min<usize>(total / stride + 1, max_samples + 1u)));
  for (usize i = 0; i < total; i += stride) {
    const usize t = i / 4;
    const f64* l = k_points[i % 4];
    D3 pos[3];
    D2 old_uv[3];
    D2 new_uv[3];
    corner_data(triangles, t, pos, old_uv, new_uv);
    if (cross2(new_uv[1] - new_uv[0], new_uv[2] - new_uv[0]) == 0.0) continue;  // not baked
    const D2 a = old_uv[0] * l[0] + old_uv[1] * l[1] + old_uv[2] * l[2];
    const D2 b = new_uv[0] * l[0] + new_uv[1] * l[1] + new_uv[2] * l[2];
    // Filtered the way the GPU filters each side — an sRGB texture in linear light — and compared
    // as the 8-bit codes that filtered value would store. Filtering an sRGB texture as stored
    // instead disagrees with the GPU by tens of codes wherever a magnified texture interpolates
    // across an edge (a 4x4 checker at 64x: 37 of 255 halfway between 40 and 240).
    const bool linearize = space == TextureSpace::srgb;
    f32 va[4];
    f32 vb[4];
    sample_bilinear(source, a, linearize, va);
    sample_bilinear(rebaked, b, linearize, vb);
    if (linearize) {
      for (u32 c = 0; c < 3; ++c) {
        va[c] = static_cast<f32>(encode_srgb(va[c])) / 255.0f;
        vb[c] = static_cast<f32>(encode_srgb(vb[c])) / 255.0f;
      }
    }
    f64 e = 0.0;
    if (normal_map) {
      const D3 n = shading_normal(triangles, t, l);
      const D3 na = decode_normal(va, normal_scale, pos, old_uv, n);
      const D3 nb = decode_normal(vb, normal_scale, pos, new_uv, n);
      const f64 c = std::clamp(dot3(na, nb), -1.0, 1.0);
      e = std::acos(c) * (180.0 / 3.14159265358979323846);
    } else {
      for (u32 c = 0; c < 4; ++c)
        e = std::max(e, std::fabs(static_cast<f64>(va[c]) - static_cast<f64>(vb[c])) * 255.0);
    }
    errors.push_back(e);
  }
  if (errors.empty()) return out;
  f64 sum = 0.0;
  for (const f64 e : errors)
    sum += e;
  std::sort(errors.begin(), errors.end());
  out.samples = static_cast<u32>(errors.size());
  out.mean = sum / static_cast<f64>(errors.size());
  out.p99 = errors[std::min(errors.size() - 1, (errors.size() * 99) / 100)];
  out.max = errors.back();
  return out;
}

}  // namespace engine::atlas
