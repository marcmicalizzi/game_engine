// engine-content static-kit: the static-asset world's kit (docs/subsystems/apps.md,
// "engine-content"; docs/experiments/static-asset-world.md; roadmap T130).
//
//   static-kit <directory>     writes ground, tree, bush, grass and rock as plain glTF files
//                              (`.gltf` + `.bin`) with their PNG textures, and prints one JSON line
//
// **Why the engine writes them.** The static-world scene (content/test-scenes/static-world/) is the
// one a consumer who uses none of the solvers has: imported meshes placed by hand. Every flown
// scene before it was a generator's, and nothing proved such a world draws as well. The Khronos
// samples the repository pins have no foliage, so the foliage and the rock come from here, made the
// way a bought asset is made and **carrying nothing a generator adds**: no generator section, no
// motion table, no structural table, no seat, no impostor chain — only what glTF 2.0 itself says
// (meshes, metallic-roughness materials, `alphaMode` MASK with a cutoff, `doubleSided`, PNG
// images). The tree is a trunk, six limbs and 75 branch cards of 1.5 m by 1.1 m, three crossed
// planes at each of 25 points, cut by a twig of 18 leaves in the card's alpha: the shape of a
// middleware or marketplace tree, whose cards stand for a bough rather than a leaf. The grass
// clumps' normals point up, as a bought grass card's do. Nothing binary is committed: the files are
// written here, beside the scene (git-ignored) or into a test's scratch directory.
//
// **The same bytes everywhere.** Everything is products, sums and correctly rounded square roots:
// the angles come from a table built by halving 30 degrees twice (sqrt only) and rotating by it,
// the noise from an integer hash, the ground from polynomial bumps; no `std::sin`, and no C library
// call in any value written but the exact ones (`floor`, `fabs`). So the kit is the same files on
// every toolchain, as the content build's own output is, and a scene that names them measures the
// same meshes on every machine.
#include "content_commands.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

struct V3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

V3 operator+(V3 a, V3 b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, f64 s) { return V3{a.x * s, a.y * s, a.z * s}; }
f64 dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) {
  return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 unit(V3 a) {
  const f64 len = std::sqrt(dot(a, a));
  return len > 0.0 ? a * (1.0 / len) : V3{0.0, 1.0, 0.0};
}

// cos and sin of k * 7.5 degrees, k = 0..48 (48 is 0 again, written as 0 so a seam closes exactly):
// 7.5 degrees by halving 30 twice, then a rotation by it, in doubles. Only sqrt, so the same bits
// on every toolchain.
struct Circle {
  f64 c[49];
  f64 s[49];
  Circle() {
    const f64 c30 = std::sqrt(3.0) * 0.5;
    const f64 c15 = std::sqrt((1.0 + c30) * 0.5);
    const f64 c7 = std::sqrt((1.0 + c15) * 0.5);
    const f64 s7 = std::sqrt((1.0 - c15) * 0.5);
    c[0] = 1.0;
    s[0] = 0.0;
    for (u32 k = 1; k < 48; ++k) {
      c[k] = c[k - 1] * c7 - s[k - 1] * s7;
      s[k] = s[k - 1] * c7 + c[k - 1] * s7;
    }
    c[48] = 1.0;
    s[48] = 0.0;
  }
};

const Circle& circle() {
  static const Circle table;
  return table;
}

u32 hash3(u32 x, u32 y, u32 seed) {
  u32 h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}

f64 unit_hash(u32 x, u32 y, u32 seed) {
  return static_cast<f64>(hash3(x, y, seed) >> 8) * (1.0 / 16777216.0);
}

f64 smooth(f64 t) { return t * t * (3.0 - 2.0 * t); }

// Value noise of period `period` lattice cells over [0, 1), so a texture tiles.
f64 noise2(f64 u, f64 v, u32 period, u32 seed) {
  const f64 x = u * static_cast<f64>(period);
  const f64 y = v * static_cast<f64>(period);
  const f64 fx = std::floor(x);
  const f64 fy = std::floor(y);
  const u32 x0 = static_cast<u32>(static_cast<i64>(fx) % static_cast<i64>(period));
  const u32 y0 = static_cast<u32>(static_cast<i64>(fy) % static_cast<i64>(period));
  const u32 x1 = (x0 + 1) % period;
  const u32 y1 = (y0 + 1) % period;
  const f64 tx = smooth(x - fx);
  const f64 ty = smooth(y - fy);
  const f64 a = unit_hash(x0, y0, seed) + (unit_hash(x1, y0, seed) - unit_hash(x0, y0, seed)) * tx;
  const f64 b = unit_hash(x0, y1, seed) + (unit_hash(x1, y1, seed) - unit_hash(x0, y1, seed)) * tx;
  return a + (b - a) * ty;
}

f64 clamp01(f64 v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
u8 byte(f64 v) { return static_cast<u8>(clamp01(v) * 255.0 + 0.5); }

// --- images ---------------------------------------------------------------------------------

struct Image {
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> rgba;
  void resize(u32 w, u32 h) {
    width = w;
    height = h;
    rgba.assign(static_cast<usize>(w) * h * 4, 0);
  }
  void put(u32 x, u32 y, f64 r, f64 g, f64 b, f64 a) {
    u8* p = rgba.data() + (static_cast<usize>(y) * width + x) * 4;
    p[0] = byte(r);
    p[1] = byte(g);
    p[2] = byte(b);
    p[3] = byte(a);
  }
};

// A twig along the card's u with 18 leaves, alternate, 55 degrees off it, longest at the twig's
// base, and one at its tip: alpha 1 on the leaves and the twig, 0 off them with a one-texel soft
// edge, and black under the cut, which is what an editor exporting straight alpha leaves there.
void branch_card(Image& image) {
  const u32 n = 512;
  image.resize(n, n);
  const f64 ca = 0.573576436351046;  // cos 55 degrees, written once: a constant, not a call
  const f64 sa = 0.819152044288992;
  for (u32 y = 0; y < n; ++y) {
    const f64 v = (static_cast<f64>(y) + 0.5) / static_cast<f64>(n);
    for (u32 x = 0; x < n; ++x) {
      const f64 u = (static_cast<f64>(x) + 0.5) / static_cast<f64>(n);
      const f64 twig_v = 0.5 + 0.03 * u * u;
      f64 inside = -1e9;
      f64 r = 0.0;
      f64 g = 0.0;
      f64 b = 0.0;
      // The twig: 0.012 wide at its base, 0.004 at the tip.
      if (u < 0.93) {
        const f64 half = 0.006 - 0.004 * u;
        const f64 t = (half - std::fabs(v - twig_v)) * static_cast<f64>(n);
        if (t > inside) {
          inside = t;
          r = 0.30;
          g = 0.22;
          b = 0.12;
        }
      }
      for (u32 i = 0; i < 19; ++i) {
        const bool tip = i == 18;
        const f64 base_u = tip ? 0.86 : 0.06 + 0.045 * static_cast<f64>(i);
        const f64 side = tip ? 0.0 : ((i & 1u) != 0 ? 1.0 : -1.0);
        const f64 length = tip ? 0.14 : 0.34 - 0.010 * static_cast<f64>(i);
        const f64 du = tip ? 1.0 : ca;
        const f64 dv = tip ? 0.0 : side * sa;
        const f64 bu = base_u;
        const f64 bv = 0.5 + 0.03 * bu * bu;
        // The pixel in the leaf's frame: t along it in [0, 1], w across it.
        const f64 pu = u - bu;
        const f64 pv = v - bv;
        const f64 t = (pu * du + pv * dv) / length;
        if (t <= 0.0 || t >= 1.0) continue;
        const f64 w = -pu * dv + pv * du;
        const f64 half = 0.30 * length * 4.0 * t * (1.0 - t) * (0.6 + 0.4 * (1.0 - t));
        const f64 leaf = (half - std::fabs(w)) * static_cast<f64>(n);
        if (leaf <= inside) continue;
        inside = leaf;
        const f64 tint = unit_hash(i, 7u, 11u);
        const f64 rib = std::fabs(w) < 0.004 ? 0.82 : 1.0;
        const f64 lift = 0.85 + 0.25 * t;
        r = (0.16 + 0.10 * tint) * lift * rib;
        g = (0.36 + 0.12 * tint) * lift * rib;
        b = (0.08 + 0.04 * tint) * lift * rib;
      }
      const f64 a = clamp01(inside + 0.5);
      if (a > 0.0)
        image.put(x, y, r, g, b, a);
      else
        image.put(x, y, 0.0, 0.0, 0.0, 0.0);
    }
  }
}

// Fourteen blades from the card's bottom edge (v = 1), each leaning and tapering to a point.
void grass_card(Image& image) {
  const u32 n = 256;
  image.resize(n, n);
  for (u32 y = 0; y < n; ++y) {
    const f64 v = (static_cast<f64>(y) + 0.5) / static_cast<f64>(n);
    for (u32 x = 0; x < n; ++x) {
      const f64 u = (static_cast<f64>(x) + 0.5) / static_cast<f64>(n);
      f64 inside = -1e9;
      f64 shade = 0.0;
      f64 tip_t = 0.0;
      for (u32 j = 0; j < 14; ++j) {
        const f64 x0 = 0.07 + 0.062 * static_cast<f64>(j) + 0.02 * (unit_hash(j, 1u, 5u) - 0.5);
        const f64 top = 0.04 + 0.40 * unit_hash(j, 2u, 5u);
        const f64 lean = 0.28 * (unit_hash(j, 3u, 5u) - 0.5);
        const f64 t = (1.0 - v) / (1.0 - top);
        if (t <= 0.0 || t >= 1.0) continue;
        const f64 centre = x0 + lean * t * t;
        const f64 half = 0.014 * (1.0 - t);
        const f64 blade = (half - std::fabs(u - centre)) * static_cast<f64>(n);
        if (blade <= inside) continue;
        inside = blade;
        shade = unit_hash(j, 4u, 5u);
        tip_t = t;
      }
      const f64 a = clamp01(inside + 0.5);
      if (a > 0.0) {
        const f64 dry = tip_t * tip_t * 0.5;
        image.put(x, y, 0.20 + 0.25 * dry + 0.05 * shade, 0.38 + 0.10 * shade, 0.10, a);
      } else {
        image.put(x, y, 0.0, 0.0, 0.0, 0.0);
      }
    }
  }
}

// Bark: fissures running along v (the trunk's length), from noise stretched across them.
void bark(Image& image) {
  const u32 n = 256;
  image.resize(n, n);
  for (u32 y = 0; y < n; ++y)
    for (u32 x = 0; x < n; ++x) {
      const f64 u = (static_cast<f64>(x) + 0.5) / static_cast<f64>(n);
      const f64 v = (static_cast<f64>(y) + 0.5) / static_cast<f64>(n);
      const f64 ridge = 0.6 * noise2(u, v * 0.125, 16, 21u) + 0.4 * noise2(u, v, 32, 22u);
      const f64 crevice = ridge < 0.42 ? 0.45 : 1.0;
      const f64 grain = 0.85 + 0.3 * noise2(u, v, 64, 23u);
      image.put(x, y, 0.33 * crevice * grain, 0.27 * crevice * grain, 0.21 * crevice * grain, 1.0);
    }
}

void rock(Image& image) {
  const u32 n = 256;
  image.resize(n, n);
  for (u32 y = 0; y < n; ++y)
    for (u32 x = 0; x < n; ++x) {
      const f64 u = (static_cast<f64>(x) + 0.5) / static_cast<f64>(n);
      const f64 v = (static_cast<f64>(y) + 0.5) / static_cast<f64>(n);
      const f64 g =
          0.5 * noise2(u, v, 8, 31u) + 0.3 * noise2(u, v, 32, 32u) + 0.2 * noise2(u, v, 128, 33u);
      const f64 l = 0.30 + 0.28 * g;
      image.put(x, y, l * 1.02, l, l * 0.94, 1.0);
    }
}

void ground(Image& image) {
  const u32 n = 256;
  image.resize(n, n);
  for (u32 y = 0; y < n; ++y)
    for (u32 x = 0; x < n; ++x) {
      const f64 u = (static_cast<f64>(x) + 0.5) / static_cast<f64>(n);
      const f64 v = (static_cast<f64>(y) + 0.5) / static_cast<f64>(n);
      const f64 green = smooth(clamp01(1.6 * noise2(u, v, 4, 41u) - 0.3));
      const f64 grit = 0.8 + 0.4 * noise2(u, v, 64, 42u);
      const f64 r = (0.30 * (1.0 - green) + 0.20 * green) * grit;
      const f64 g = (0.25 * (1.0 - green) + 0.30 * green) * grit;
      const f64 b = (0.18 * (1.0 - green) + 0.10 * green) * grit;
      image.put(x, y, r, g, b, 1.0);
    }
}

// --- meshes ---------------------------------------------------------------------------------

struct Primitive {
  std::vector<f32> positions;  // xyz
  std::vector<f32> normals;    // xyz
  std::vector<f32> uvs;        // uv
  std::vector<u32> indices;
  u32 material = 0;

  u32 vertex(V3 p, V3 n, f64 u, f64 v) {
    const u32 index = static_cast<u32>(positions.size() / 3);
    positions.push_back(static_cast<f32>(p.x));
    positions.push_back(static_cast<f32>(p.y));
    positions.push_back(static_cast<f32>(p.z));
    const V3 m = unit(n);
    normals.push_back(static_cast<f32>(m.x));
    normals.push_back(static_cast<f32>(m.y));
    normals.push_back(static_cast<f32>(m.z));
    uvs.push_back(static_cast<f32>(u));
    uvs.push_back(static_cast<f32>(v));
    return index;
  }
  V3 at(u32 i) const {
    return V3{static_cast<f64>(positions[i * 3]), static_cast<f64>(positions[i * 3 + 1]),
              static_cast<f64>(positions[i * 3 + 2])};
  }
  void triangle(u32 a, u32 b, u32 c) {
    indices.push_back(a);
    indices.push_back(b);
    indices.push_back(c);
  }
  // Two triangles a b c, a c d, wound counter-clockwise seen from the side `facing` points to.
  void quad(u32 a, u32 b, u32 c, u32 d, V3 facing) {
    const V3 pa = at(a);
    const V3 pb = at(b);
    const V3 pc = at(c);
    if (dot(cross(pb - pa, pc - pa), facing) >= 0.0) {
      triangle(a, b, c);
      triangle(a, c, d);
    } else {
      triangle(a, c, b);
      triangle(a, d, c);
    }
  }
  u32 triangles() const { return static_cast<u32>(indices.size() / 3); }
};

struct Material {
  std::string name;
  std::string image;  // PNG beside the file; empty: the factor alone
  f64 color[4] = {1.0, 1.0, 1.0, 1.0};
  f64 roughness = 0.9;
  bool masked = false;
  bool clamp = false;  // a card: its UVs are [0, 1] and must not wrap
};

struct Mesh {
  std::vector<Primitive> primitives;
  std::vector<Material> materials;
  u32 triangles() const {
    u32 t = 0;
    for (const Primitive& p : primitives)
      t += p.triangles();
    return t;
  }
};

// A tube from `p0` (radius r0) to `p1` (radius r1) with `sides` sides (a divisor of 48) and
// `rings` rings, its texture wrapping `wraps` times round and repeating every `v_metres` along it.
void tube(Primitive& prim, V3 p0, V3 p1, f64 r0, f64 r1, u32 sides, u32 rings, f64 wraps,
          f64 v_metres) {
  const Circle& k = circle();
  const V3 axis = p1 - p0;
  const f64 length = std::sqrt(dot(axis, axis));
  const V3 a = unit(axis);
  const V3 ref = std::fabs(a.y) < 0.9 ? V3{0.0, 1.0, 0.0} : V3{1.0, 0.0, 0.0};
  const V3 e1 = unit(cross(a, ref));
  const V3 e2 = unit(cross(e1, a));
  const u32 step = 48 / sides;
  const u32 first = static_cast<u32>(prim.positions.size() / 3);
  for (u32 r = 0; r <= rings; ++r) {
    const f64 t = static_cast<f64>(r) / static_cast<f64>(rings);
    const V3 centre = p0 + axis * t;
    const f64 radius = r0 + (r1 - r0) * t;
    for (u32 s = 0; s <= sides; ++s) {
      const V3 out = e1 * k.c[s * step] + e2 * k.s[s * step];
      prim.vertex(centre + out * radius, out, wraps * static_cast<f64>(s) / static_cast<f64>(sides),
                  t * length / v_metres);
    }
  }
  for (u32 r = 0; r < rings; ++r)
    for (u32 s = 0; s < sides; ++s) {
      const u32 i0 = first + r * (sides + 1) + s;
      const u32 i1 = i0 + 1;
      const u32 i2 = i0 + sides + 1 + 1;
      const u32 i3 = i0 + sides + 1;
      prim.quad(i0, i1, i2, i3, e1 * k.c[s * step] + e2 * k.s[s * step]);
    }
}

// One branch card: `width` across, `length` along `twig` from `attach`, turned `roll` (a 48th of a
// turn index) about the twig. u runs along the twig, v across it.
void card(Primitive& prim, V3 attach, V3 twig, f64 length, f64 width, u32 roll) {
  const Circle& k = circle();
  const V3 t = unit(twig);
  const V3 side0 = unit(cross(t, V3{0.0, 1.0, 0.0}));
  const V3 up0 = unit(cross(side0, t));
  const V3 side = side0 * k.c[roll % 48] + up0 * k.s[roll % 48];
  const V3 normal = unit(cross(t, side));
  const V3 a = attach - t * (0.12 * length);
  const V3 h = side * (0.5 * width);
  const u32 i0 = prim.vertex(a - h, normal, 0.0, 0.0);
  const u32 i1 = prim.vertex(a + t * length - h, normal, 1.0, 0.0);
  const u32 i2 = prim.vertex(a + t * length + h, normal, 1.0, 1.0);
  const u32 i3 = prim.vertex(a + h, normal, 0.0, 1.0);
  prim.quad(i0, i1, i2, i3, normal);
}

Material bark_material() {
  Material m;
  m.name = "bark";
  m.image = "bark.png";
  m.roughness = 0.95;
  return m;
}

Material leaf_material() {
  Material m;
  m.name = "branch cards";
  m.image = "branch-card.png";
  m.roughness = 0.75;
  m.masked = true;
  m.clamp = true;
  return m;
}

// A broadleaf of about 9 m: the trunk, six limbs, and 25 points of three crossed branch cards.
void make_tree(Mesh& mesh) {
  const Circle& k = circle();
  mesh.materials = {bark_material(), leaf_material()};
  mesh.primitives.resize(2);
  Primitive& wood = mesh.primitives[0];
  Primitive& leaves = mesh.primitives[1];
  wood.material = 0;
  leaves.material = 1;
  tube(wood, V3{0.0, -0.3, 0.0}, V3{0.0, 7.6, 0.0}, 0.30, 0.09, 16, 8, 2.0, 2.0);
  std::vector<V3> points;
  for (u32 limb = 0; limb < 6; ++limb) {
    const u32 at = (limb * 8 + 3 * (limb & 1u)) % 48;
    const f64 y = 3.0 + 0.45 * static_cast<f64>(limb);
    const V3 out = unit(V3{k.c[at] * 0.80, 0.60, k.s[at] * 0.80});
    const V3 base{0.0, y, 0.0};
    const f64 length = 3.2 - 0.25 * static_cast<f64>(limb);
    tube(wood, base, base + out * length, 0.11, 0.035, 8, 3, 1.0, 1.0);
    for (const f64 f : {0.5, 0.78, 1.0})
      points.push_back(base + out * (length * f));
  }
  for (u32 i = 0; i < 6; ++i) {
    const u32 at = (i * 8 + 4) % 48;
    points.push_back(V3{k.c[at] * 0.9, 6.9 + 0.3 * static_cast<f64>(i & 1u), k.s[at] * 0.9});
  }
  points.push_back(V3{0.0, 8.0, 0.0});
  for (usize p = 0; p < points.size(); ++p) {
    const V3 at = points[p];
    V3 out = V3{at.x, 0.0, at.z};
    if (dot(out, out) < 0.01) out = V3{1.0, 0.0, 0.0};
    out = unit(unit(out) + V3{0.0, 0.35, 0.0});
    const u32 twist = static_cast<u32>(hash3(static_cast<u32>(p), 9u, 3u) % 16u);
    for (u32 c = 0; c < 3; ++c)
      card(leaves, at, out, 1.5, 1.1, twist + c * 16);
  }
}

// A shrub of about 1.2 m: 12 points round a dome, two crossed cards each, no wood.
void make_bush(Mesh& mesh) {
  const Circle& k = circle();
  mesh.materials = {leaf_material()};
  mesh.primitives.resize(1);
  Primitive& leaves = mesh.primitives[0];
  for (u32 i = 0; i < 12; ++i) {
    const u32 at = (i * 4 + 2 * (i / 6)) % 48;
    const f64 ring = i < 6 ? 0.35 : 0.15;
    const f64 y = i < 6 ? 0.35 : 0.7;
    const V3 point{k.c[at] * ring, y, k.s[at] * ring};
    const V3 out = unit(V3{k.c[at], i < 6 ? 0.25 : 0.8, k.s[at]});
    for (u32 c = 0; c < 2; ++c)
      card(leaves, point, out, 0.85, 0.65, i * 5 + c * 24);
  }
}

// A clump of grass: three cards 0.6 m wide and 0.45 m tall crossed at 60 degrees, normals up.
void make_grass(Mesh& mesh) {
  const Circle& k = circle();
  Material m;
  m.name = "grass card";
  m.image = "grass-card.png";
  m.roughness = 0.8;
  m.masked = true;
  m.clamp = true;
  mesh.materials = {m};
  mesh.primitives.resize(1);
  Primitive& prim = mesh.primitives[0];
  const V3 up{0.0, 1.0, 0.0};
  for (u32 c = 0; c < 3; ++c) {
    const u32 at = c * 8;
    const V3 side{k.c[at] * 0.3, 0.0, k.s[at] * 0.3};
    const V3 lean{k.s[at] * 0.04, 0.0, -k.c[at] * 0.04};
    const u32 i0 = prim.vertex(V3{} - side, up, 0.0, 1.0);
    const u32 i1 = prim.vertex(V3{} + side, up, 1.0, 1.0);
    const u32 i2 = prim.vertex(side + lean + V3{0.0, 0.45, 0.0}, up, 1.0, 0.0);
    const u32 i3 = prim.vertex(lean - side + V3{0.0, 0.45, 0.0}, up, 0.0, 0.0);
    prim.quad(i0, i1, i2, i3, unit(cross(side, up)));
  }
}

// A boulder of about 2 m: a sphere of 48 by 24 facets pushed in and out by noise, flattened, its
// base a little below the origin. Normals are the facets' summed round each vertex.
void make_rock(Mesh& mesh) {
  const Circle& k = circle();
  Material m;
  m.name = "rock";
  m.image = "rock.png";
  m.roughness = 0.85;
  mesh.materials = {m};
  mesh.primitives.resize(1);
  Primitive& prim = mesh.primitives[0];
  const u32 cols = 48;
  const u32 rows = 24;
  std::vector<V3> p((cols + 1) * (rows + 1));
  for (u32 r = 0; r <= rows; ++r) {
    // Row r is r * 7.5 degrees from the south pole: sin(latitude) = -cos(r * 7.5 degrees) and
    // cos(latitude) = sin(r * 7.5 degrees).
    const f64 y = -k.c[r];
    const f64 ring = k.s[r];
    for (u32 c = 0; c <= cols; ++c) {
      const V3 dir{k.c[c] * ring, y, k.s[c] * ring};
      const u32 cc = c % cols;
      const f64 bump =
          0.55 * noise2(static_cast<f64>(cc) / 48.0, static_cast<f64>(r) / 24.0, 6, 51u) +
          0.25 * noise2(static_cast<f64>(cc) / 48.0, static_cast<f64>(r) / 24.0, 12, 52u);
      const f64 radius = (r == 0 || r == rows) ? 0.85 : 0.78 + 0.32 * bump;
      p[r * (cols + 1) + c] =
          V3{dir.x * radius * 1.15, dir.y * radius * 0.72 + 0.45, dir.z * radius * 0.95};
    }
  }
  for (u32 r = 0; r <= rows; ++r)  // the seam column is the first one's point
    p[r * (cols + 1) + cols] = p[r * (cols + 1)];
  std::vector<V3> n(p.size());
  auto add = [&](u32 a, u32 b, u32 c) {
    const V3 f = cross(p[b] - p[a], p[c] - p[a]);
    for (const u32 i : {a, b, c}) {
      const u32 col = i % (cols + 1);
      const u32 key = col == cols ? i - cols : i;
      n[key] = n[key] + f;
    }
  };
  for (u32 r = 0; r < rows; ++r)
    for (u32 c = 0; c < cols; ++c) {
      const u32 i0 = r * (cols + 1) + c;
      add(i0, i0 + cols + 1, i0 + 1);
      add(i0 + 1, i0 + cols + 1, i0 + cols + 2);
    }
  for (u32 r = 0; r <= rows; ++r)
    n[r * (cols + 1) + cols] = n[r * (cols + 1)];
  for (u32 c = 0; c <= cols; ++c) {
    n[c] = V3{0.0, -1.0, 0.0};
    n[rows * (cols + 1) + c] = V3{0.0, 1.0, 0.0};
  }
  for (u32 r = 0; r <= rows; ++r)
    for (u32 c = 0; c <= cols; ++c) {
      const u32 i = r * (cols + 1) + c;
      prim.vertex(p[i], n[i], 3.0 * static_cast<f64>(c) / static_cast<f64>(cols),
                  1.5 * static_cast<f64>(r) / static_cast<f64>(rows));
    }
  for (u32 r = 0; r < rows; ++r)
    for (u32 c = 0; c < cols; ++c) {
      const u32 i0 = r * (cols + 1) + c;
      prim.triangle(i0, i0 + cols + 1, i0 + 1);
      prim.triangle(i0 + 1, i0 + cols + 1, i0 + cols + 2);
    }
}

// The static world's ground, metres: a gentle tilt and six rounded swells and hollows. The scene
// file states every instance's height outright, as a hand placement does, read off this function
// (content/test-scenes/static-world/README.md has it in a line of PowerShell).
f64 static_kit_ground_height(f64 x, f64 z) {
  struct Bump {
    f64 x, z, r, a;
  };
  constexpr Bump k_bumps[] = {{30.0, 20.0, 28.0, 3.5},   {-35.0, -25.0, 30.0, 2.8},
                              {-10.0, 40.0, 22.0, -1.8}, {45.0, -40.0, 35.0, 5.0},
                              {-60.0, 50.0, 40.0, 6.0},  {0.0, 0.0, 18.0, 0.6}};
  f64 h = 0.015 * x - 0.010 * z;
  for (const Bump& b : k_bumps) {
    const f64 d2 = (x - b.x) * (x - b.x) + (z - b.z) * (z - b.z);
    if (d2 < b.r * b.r) {
      const f64 q = 1.0 - d2 / (b.r * b.r);
      h += b.a * q * q;
    }
  }
  return h;
}

// 600 m square, 161 x 161 samples, 0.94 m apart at the middle and wider outwards (x = 300 (t/4 +
// 3t^3/4) over t in [-1, 1]), so the world's edge is past what the path sees and the bumps keep
// their shape where the trees stand; its texture repeats every 4 m.
f64 ground_coordinate(u32 i, u32 n) {
  const f64 t = 2.0 * static_cast<f64>(i) / static_cast<f64>(n - 1) - 1.0;
  return 300.0 * (0.25 * t + 0.75 * t * t * t);
}

void make_ground(Mesh& mesh) {
  Material m;
  m.name = "ground";
  m.image = "ground.png";
  m.roughness = 0.95;
  mesh.materials = {m};
  mesh.primitives.resize(1);
  Primitive& prim = mesh.primitives[0];
  const u32 n = 161;
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i) {
      const f64 x = ground_coordinate(i, n);
      const f64 z = ground_coordinate(j, n);
      const f64 e = 0.25;
      const V3 normal{static_kit_ground_height(x - e, z) - static_kit_ground_height(x + e, z),
                      2.0 * e,
                      static_kit_ground_height(x, z - e) - static_kit_ground_height(x, z + e)};
      prim.vertex(V3{x, static_kit_ground_height(x, z), z}, normal, x / 4.0, z / 4.0);
    }
  for (u32 j = 0; j + 1 < n; ++j)
    for (u32 i = 0; i + 1 < n; ++i) {
      const u32 a = j * n + i;
      prim.quad(a, a + 1, a + n + 1, a + n, V3{0.0, 1.0, 0.0});
    }
}

// --- the glTF writer ------------------------------------------------------------------------

void append(std::string& bin, const void* data, usize bytes) {
  bin.append(static_cast<const char*>(data), bytes);
  while (bin.size() % 4 != 0)
    bin.push_back('\0');
}

JsonValue numbers(std::initializer_list<f64> values) {
  JsonValue a = JsonValue::array();
  for (const f64 v : values)
    a.push_back(JsonValue(v));
  return a;
}

bool write_mesh(const std::string& dir, const std::string& stem, const Mesh& mesh,
                std::string& error) {
  std::string bin;
  JsonValue views = JsonValue::array();
  JsonValue accessors = JsonValue::array();
  auto add_view = [&](const void* data, usize bytes, u64 target) {
    JsonValue view = JsonValue::object();
    view.set("buffer", JsonValue(u64{0}));
    view.set("byteOffset", JsonValue(static_cast<u64>(bin.size())));
    view.set("byteLength", JsonValue(static_cast<u64>(bytes)));
    view.set("target", JsonValue(target));
    append(bin, data, bytes);
    views.push_back(std::move(view));
    return static_cast<u64>(views.size() - 1);
  };
  auto add_accessor = [&](u64 view, u64 component, u64 count, const char* type) {
    JsonValue a = JsonValue::object();
    a.set("bufferView", JsonValue(view));
    a.set("componentType", JsonValue(component));
    a.set("count", JsonValue(count));
    a.set("type", JsonValue(type));
    accessors.push_back(std::move(a));
    return static_cast<u64>(accessors.size() - 1);
  };
  JsonValue primitives = JsonValue::array();
  for (const Primitive& p : mesh.primitives) {
    const u64 count = p.positions.size() / 3;
    const u64 pos = add_accessor(add_view(p.positions.data(), p.positions.size() * 4, 34962), 5126,
                                 count, "VEC3");
    f32 lo[3] = {p.positions[0], p.positions[1], p.positions[2]};
    f32 hi[3] = {lo[0], lo[1], lo[2]};
    for (usize i = 0; i < p.positions.size(); ++i) {
      lo[i % 3] = p.positions[i] < lo[i % 3] ? p.positions[i] : lo[i % 3];
      hi[i % 3] = p.positions[i] > hi[i % 3] ? p.positions[i] : hi[i % 3];
    }
    accessors[static_cast<usize>(pos)].set(
        "min",
        numbers({static_cast<f64>(lo[0]), static_cast<f64>(lo[1]), static_cast<f64>(lo[2])}));
    accessors[static_cast<usize>(pos)].set(
        "max",
        numbers({static_cast<f64>(hi[0]), static_cast<f64>(hi[1]), static_cast<f64>(hi[2])}));
    const u64 nrm =
        add_accessor(add_view(p.normals.data(), p.normals.size() * 4, 34962), 5126, count, "VEC3");
    const u64 uv =
        add_accessor(add_view(p.uvs.data(), p.uvs.size() * 4, 34962), 5126, count, "VEC2");
    const u64 idx = add_accessor(add_view(p.indices.data(), p.indices.size() * 4, 34963), 5125,
                                 p.indices.size(), "SCALAR");
    JsonValue attributes = JsonValue::object();
    attributes.set("POSITION", JsonValue(pos));
    attributes.set("NORMAL", JsonValue(nrm));
    attributes.set("TEXCOORD_0", JsonValue(uv));
    JsonValue prim = JsonValue::object();
    prim.set("attributes", std::move(attributes));
    prim.set("indices", JsonValue(idx));
    prim.set("material", JsonValue(static_cast<u64>(p.material)));
    primitives.push_back(std::move(prim));
  }
  JsonValue materials = JsonValue::array();
  JsonValue images = JsonValue::array();
  JsonValue textures = JsonValue::array();
  for (const Material& m : mesh.materials) {
    JsonValue pbr = JsonValue::object();
    pbr.set("baseColorFactor", numbers({m.color[0], m.color[1], m.color[2], m.color[3]}));
    pbr.set("metallicFactor", JsonValue(0.0));
    pbr.set("roughnessFactor", JsonValue(m.roughness));
    if (!m.image.empty()) {
      JsonValue image = JsonValue::object();
      image.set("uri", JsonValue(m.image));
      images.push_back(std::move(image));
      JsonValue texture = JsonValue::object();
      texture.set("source", JsonValue(static_cast<u64>(images.size() - 1)));
      texture.set("sampler", JsonValue(static_cast<u64>(m.clamp ? 1 : 0)));
      textures.push_back(std::move(texture));
      JsonValue info = JsonValue::object();
      info.set("index", JsonValue(static_cast<u64>(textures.size() - 1)));
      pbr.set("baseColorTexture", std::move(info));
    }
    JsonValue material = JsonValue::object();
    material.set("name", JsonValue(m.name));
    material.set("pbrMetallicRoughness", std::move(pbr));
    if (m.masked) {
      material.set("alphaMode", JsonValue("MASK"));
      material.set("alphaCutoff", JsonValue(0.5));
      material.set("doubleSided", JsonValue(true));
    }
    materials.push_back(std::move(material));
  }
  // 0: repeat, for the bark, the rock and the ground; 1: clamped, for a card.
  JsonValue samplers = JsonValue::array();
  for (const u64 wrap : {u64{10497}, u64{33071}}) {
    JsonValue s = JsonValue::object();
    s.set("magFilter", JsonValue(u64{9729}));
    s.set("minFilter", JsonValue(u64{9987}));
    s.set("wrapS", JsonValue(wrap));
    s.set("wrapT", JsonValue(wrap));
    samplers.push_back(std::move(s));
  }
  JsonValue gltf = JsonValue::object();
  JsonValue asset = JsonValue::object();
  asset.set("version", JsonValue("2.0"));
  asset.set("generator", JsonValue("engine-content static-kit"));
  gltf.set("asset", std::move(asset));
  gltf.set("scene", JsonValue(u64{0}));
  JsonValue scene = JsonValue::object();
  JsonValue roots = JsonValue::array();
  roots.push_back(JsonValue(u64{0}));
  scene.set("nodes", std::move(roots));
  JsonValue scenes = JsonValue::array();
  scenes.push_back(std::move(scene));
  gltf.set("scenes", std::move(scenes));
  JsonValue node = JsonValue::object();
  node.set("name", JsonValue(stem));
  node.set("mesh", JsonValue(u64{0}));
  JsonValue nodes = JsonValue::array();
  nodes.push_back(std::move(node));
  gltf.set("nodes", std::move(nodes));
  JsonValue m = JsonValue::object();
  m.set("name", JsonValue(stem));
  m.set("primitives", std::move(primitives));
  JsonValue meshes = JsonValue::array();
  meshes.push_back(std::move(m));
  gltf.set("meshes", std::move(meshes));
  gltf.set("materials", std::move(materials));
  if (images.size() > 0) {
    gltf.set("images", std::move(images));
    gltf.set("textures", std::move(textures));
    gltf.set("samplers", std::move(samplers));
  }
  gltf.set("accessors", std::move(accessors));
  gltf.set("bufferViews", std::move(views));
  JsonValue buffer = JsonValue::object();
  buffer.set("byteLength", JsonValue(static_cast<u64>(bin.size())));
  buffer.set("uri", JsonValue(stem + ".bin"));
  JsonValue buffers = JsonValue::array();
  buffers.push_back(std::move(buffer));
  gltf.set("buffers", std::move(buffers));
  if (io::write_file(io::join_path(dir, stem + ".bin"), bin) != io::Status::Ok) {
    error = "cannot write " + io::join_path(dir, stem + ".bin");
    return false;
  }
  if (io::write_file(io::join_path(dir, stem + ".gltf"), write_json(gltf) + "\n") !=
      io::Status::Ok) {
    error = "cannot write " + io::join_path(dir, stem + ".gltf");
    return false;
  }
  return true;
}

bool write_image(const std::string& dir, const char* name, const Image& picture,
                 std::string& error) {
  const std::string path = io::join_path(dir, name);
  if (image::write_png(path, picture.width, picture.height, 4,
                       std::span<const u8>(picture.rgba.data(), picture.rgba.size())) !=
      io::Status::Ok) {
    error = "cannot write " + path;
    return false;
  }
  return true;
}

int usage(const char* why) {
  std::fprintf(stderr,
               "engine-content static-kit: %s\n"
               "usage: engine-content static-kit <directory>\n"
               "  writes ground, tree, bush, grass and rock (.gltf + .bin) and their PNGs: plain\n"
               "  glTF 2.0 with nothing a generator adds, for the static-world scene\n",
               why);
  return k_exit_usage;
}

}  // namespace

int static_kit_command(int argc, char** argv) {
  std::string dir;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (!a.empty() && a[0] == '-') return usage("unknown option");
    if (!dir.empty()) return usage("static-kit takes one directory");
    dir = std::string(a);
  }
  if (dir.empty()) return usage("static-kit takes one directory");
  if (io::make_directories(dir) != io::Status::Ok) {
    std::fprintf(stderr, "engine-content static-kit: cannot make %s\n", dir.c_str());
    return k_exit_error;
  }
  std::string error;
  struct Made {
    const char* stem;
    void (*make)(Mesh&);
  };
  constexpr Made k_meshes[] = {{"ground", make_ground},
                               {"tree", make_tree},
                               {"bush", make_bush},
                               {"grass", make_grass},
                               {"rock", make_rock}};
  JsonValue summary = JsonValue::object();
  JsonValue triangles = JsonValue::object();
  for (const Made& made : k_meshes) {
    Mesh mesh;
    made.make(mesh);
    if (!write_mesh(dir, made.stem, mesh, error)) {
      std::fprintf(stderr, "engine-content static-kit: %s\n", error.c_str());
      return k_exit_error;
    }
    triangles.set(made.stem, JsonValue(mesh.triangles()));
  }
  struct Drawn {
    const char* name;
    void (*draw)(Image&);
  };
  constexpr Drawn k_images[] = {{"branch-card.png", branch_card},
                                {"grass-card.png", grass_card},
                                {"bark.png", bark},
                                {"rock.png", rock},
                                {"ground.png", ground}};
  JsonValue coverage = JsonValue::object();
  for (const Drawn& drawn : k_images) {
    Image image;
    drawn.draw(image);
    if (!write_image(dir, drawn.name, image, error)) {
      std::fprintf(stderr, "engine-content static-kit: %s\n", error.c_str());
      return k_exit_error;
    }
    u64 kept = 0;
    for (usize i = 3; i < image.rgba.size(); i += 4)
      kept += image.rgba[i] >= 128 ? 1u : 0u;
    coverage.set(drawn.name,
                 JsonValue(static_cast<f64>(kept) / static_cast<f64>(image.rgba.size() / 4)));
  }
  summary.set("kit", JsonValue(dir));
  summary.set("triangles", std::move(triangles));
  summary.set("alpha_coverage", std::move(coverage));
  JsonWriteOptions line;
  line.pretty = false;
  std::printf("%s\n", write_json(summary, line).c_str());
  return k_exit_ok;
}

}  // namespace engine::content
