// The module solver (docs/subsystems/walls.md, "The module"; the numbers and their sources,
// docs/experiments/dry-stone-walls.md). A module is laid the way a waller lays a length of wall:
// the footing first, then sweep after sweep along each face, every stone bedded level on the
// highest point of what is under it, the gaps under it pinned, its end kept off the joint below
// ("one on two, two on one"), its height graded by how high it stands; a row of through-stones
// in the band the sources give; a cover course across the full width; the cope on edge. All of it
// is f64 arithmetic of draws from `hash_combine` and correctly rounded square roots, so a module is
// the same bytes on every toolchain (determinism_tests.cpp).

#include <core/hash/hash.h>
#include <core/math/deterministic.h>
#include <domain/walls/walls.h>
#include <foundation/image/png.h>

#include <algorithm>
#include <cmath>

namespace engine::walls {
namespace {

struct D3 {
  f64 x = 0, y = 0, z = 0;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
D3 cross(D3 a, D3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f64 dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 unit(D3 a) {
  const f64 l = std::sqrt(dot(a, a));
  return l > 1e-12 ? a * (1.0 / l) : D3{0, 1, 0};
}
Vec3 f3(D3 a) { return {static_cast<f32>(a.x), static_cast<f32>(a.y), static_cast<f32>(a.z)}; }

// A stream of uniform draws in [0, 1): a stone's, a sweep's, the module's.
struct Rng {
  explicit Rng(u64 s) : seed(s) {}
  u64 seed = 0;
  u64 n = 0;
  f64 next() {
    return static_cast<f64>(hash_combine(seed, ++n) >> 11) * (1.0 / 9007199254740992.0);
  }
  f64 range(f64 a, f64 b) { return a + (b - a) * next(); }
  u64 bits() { return hash_combine(seed, ++n); }
};

// ---- the stone palette ------------------------------------------------------------------------
//
// A 128 x 128 sRGB image of 16 x 16 swatches of 8 x 8 texels: rows 0..13 the stones' (a slabby
// metamorphic stone's greys, blue-greys, brown-greys and weathered light slabs, each texel
// mottled, a lighter fleck now and then, faint foliation bands), row 14 the mid form's average and
// row 15 the hearting's shadow. A stone takes one swatch, so the stones of a wall differ from each
// other as the reels' do (dry-stone-walls.md, "Where structure was traded for triangles").
constexpr u32 k_atlas = 128;
constexpr u32 k_swatch = 8;
constexpr u32 k_stone_swatches = 14 * 16;
constexpr u32 k_swatch_mid = 14 * 16;
constexpr u32 k_swatch_core = 15 * 16;

void palette_image(Vector<u8>& out) {
  struct Tint {
    f64 r, g, b, weight;
  };
  constexpr Tint tints[] = {
      {78, 79, 80, 0.24},     // dark grey
      {98, 98, 96, 0.30},     // mid grey
      {90, 94, 98, 0.16},     // blue grey
      {102, 97, 90, 0.14},    // brown grey
      {124, 123, 119, 0.12},  // weathered, light
      {104, 94, 82, 0.04},    // iron-stained
  };
  Vector<u8> pixels;
  pixels.resize(k_atlas * k_atlas * 3);
  for (u32 sy = 0; sy < 16; ++sy)
    for (u32 sx = 0; sx < 16; ++sx) {
      const u32 swatch = sy * 16 + sx;
      Rng rng{hash_combine(0x57a11e5ull, swatch)};
      f64 r = 0, g = 0, b = 0, speckle = 0.04, band = 0.03;
      if (swatch >= k_swatch_core) {
        r = g = b = 26;
        speckle = 0;
        band = 0;
      } else if (swatch >= k_swatch_mid) {
        r = 98;
        g = 97;
        b = 95;
      } else {
        f64 pick = rng.next(), acc = 0;
        for (const Tint& t : tints) {
          acc += t.weight;
          r = t.r;
          g = t.g;
          b = t.b;
          if (pick < acc) break;
        }
        const f64 bright = rng.range(0.90, 1.10);
        r *= bright;
        g *= bright;
        b *= bright;
      }
      for (u32 ty = 0; ty < k_swatch; ++ty) {
        const f64 row = 1.0 + band * (rng.next() * 2.0 - 1.0);
        for (u32 tx = 0; tx < k_swatch; ++tx) {
          f64 m = row * (1.0 + 0.07 * (rng.next() * 2.0 - 1.0));
          if (rng.next() < speckle) m *= 1.22;
          const u32 px = sx * k_swatch + tx, py = sy * k_swatch + ty;
          u8* p = &pixels[(py * k_atlas + px) * 3];
          p[0] = static_cast<u8>(std::clamp(r * m + 0.5, 0.0, 255.0));
          p[1] = static_cast<u8>(std::clamp(g * m + 0.5, 0.0, 255.0));
          p[2] = static_cast<u8>(std::clamp(b * m + 0.5, 0.0, 255.0));
        }
      }
    }
  out.clear();
  image::encode_png(k_atlas, k_atlas, 3, pixels, out);
}

// ---- the mesh being built -----------------------------------------------------------------------

struct Builder {
  explicit Builder(assets::MeshData& m) : mesh(m) {}
  assets::MeshData& mesh;
  Vector<u32>* indices = nullptr;  // the form being written
  u32 triangles = 0;

  u32 vertex(D3 p, D3 n, f64 u, f64 v) {
    mesh.positions.push_back(f3(p));
    mesh.normals.push_back(f3(n));
    mesh.uvs.push_back({static_cast<f32>(u), static_cast<f32>(v)});
    return static_cast<u32>(mesh.positions.size() - 1);
  }
  void tri(u32 a, u32 b, u32 c) {
    indices->push_back(a);
    indices->push_back(b);
    indices->push_back(c);
    ++triangles;
  }
};

// A swatch's UV for a point's place (fu, fv in [0, 1]) inside it, inset 1.5 texels so the
// bilinear filter never reads a neighbour.
void swatch_uv(u32 swatch, f64 fu, f64 fv, f64& u, f64& v) {
  const f64 sx = static_cast<f64>(swatch % 16), sy = static_cast<f64>(swatch / 16);
  u = (sx * k_swatch + 1.5 + 5.0 * std::clamp(fu, 0.0, 1.0)) / k_atlas;
  v = (sy * k_swatch + 1.5 + 5.0 * std::clamp(fv, 0.0, 1.0)) / k_atlas;
}

// A closed six-faced body from its eight corners (bottom ring 0..3, top ring 4..7, the rings in
// one order: (x0, za), (x1, za), (x1, zb), (x0, zb)), each face flat-shaded and wound outward
// from the body's centre. The face `split_face` (0 bottom, 1 top, 2 the za side, 3 the x1 end,
// 4 the zb side, 5 the x0 end; -1 none) is cut into four planes about a point `bulge` proud of
// its middle: a stone's split face, angular rather than flat.
void body(Builder& b, const D3 (&c)[8], u32 swatch, i32 split_face, f64 bulge) {
  D3 centre{};
  for (const D3& p : c)
    centre = centre + p;
  centre = centre * 0.125;
  D3 lo = c[0], hi = c[0];
  for (const D3& p : c) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const D3 size{std::max(hi.x - lo.x, 1e-6), std::max(hi.y - lo.y, 1e-6),
                std::max(hi.z - lo.z, 1e-6)};
  static constexpr u8 faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                     {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
  auto uv_of = [&](D3 p, D3 n, f64& u, f64& v) {
    // Project on the two axes the face lies across.
    const f64 ax = std::abs(n.x), ay = std::abs(n.y);
    f64 fu = (p.x - lo.x) / size.x, fv = (p.y - lo.y) / size.y;
    if (ax >= ay && ax >= std::abs(n.z))
      fu = (p.z - lo.z) / size.z;
    else if (ay >= std::abs(n.z))
      fv = (p.z - lo.z) / size.z;
    swatch_uv(swatch, fu, fv, u, v);
  };
  for (i32 f = 0; f < 6; ++f) {
    const D3 q[4] = {c[faces[f][0]], c[faces[f][1]], c[faces[f][2]], c[faces[f][3]]};
    D3 mid = (q[0] + q[1] + q[2] + q[3]) * 0.25;
    D3 n = unit(cross(q[2] - q[0], q[3] - q[1]));
    const bool flip = dot(n, mid - centre) < 0;
    if (flip) n = n * -1.0;
    if (f == split_face) {
      const D3 apex = mid + n * bulge;
      for (u32 e = 0; e < 4; ++e) {
        const D3 a = q[e], c2 = q[(e + 1) % 4];
        D3 fn = unit(cross(c2 - a, apex - a));
        if (dot(fn, n) < 0) fn = fn * -1.0;
        f64 u0, v0, u1, v1, u2, v2;
        uv_of(a, n, u0, v0);
        uv_of(c2, n, u1, v1);
        uv_of(apex, n, u2, v2);
        const u32 i0 = b.vertex(a, fn, u0, v0), i1 = b.vertex(c2, fn, u1, v1),
                  i2 = b.vertex(apex, fn, u2, v2);
        if (dot(cross(c2 - a, apex - a), fn) > 0)
          b.tri(i0, i1, i2);
        else
          b.tri(i0, i2, i1);
      }
      continue;
    }
    u32 id[4];
    for (u32 e = 0; e < 4; ++e) {
      f64 u, v;
      uv_of(q[e], n, u, v);
      id[e] = b.vertex(q[e], n, u, v);
    }
    if (dot(cross(q[1] - q[0], q[2] - q[0]), n) > 0) {
      b.tri(id[0], id[1], id[2]);
      b.tri(id[0], id[2], id[3]);
    } else {
      b.tri(id[0], id[2], id[1]);
      b.tri(id[0], id[3], id[2]);
    }
  }
}

// ---- the wall's section -----------------------------------------------------------------------

struct Section {
  f64 L = 3, H = 1.3, cope = 0.27, cover = 0.075, base_w = 0.7, top_w = 0.4, foot = 0.1, g = 0;
  f64 body = 0.955;  // ground to the cover's bed
  f64 base(f64 x) const { return g * x; }
  // The wall's width at a height above the ground (the batter: straight from the base to the top).
  f64 width(f64 h) const { return base_w - (base_w - top_w) * std::clamp(h / body, 0.0, 1.0); }
};

Section section_of(const Module& m) {
  Section s;
  s.L = static_cast<f64>(m.length_cm) / 100.0;
  s.H = static_cast<f64>(m.height_cm) / 100.0;
  s.cope = static_cast<f64>(m.cope_cm) / 100.0;
  s.base_w = static_cast<f64>(m.base_width_cm) / 100.0;
  s.top_w = static_cast<f64>(m.top_width_cm) / 100.0;
  s.foot = static_cast<f64>(m.footing_cm) / 100.0;
  s.g = static_cast<f64>(m.grade_permille) / 1000.0;
  s.body = s.H - s.cope - s.cover;
  return s;
}

// The course height graded from the foot to the cover (dry-stone-walls.md: 150 mm to 60 mm).
f64 graded(const Section& s, f64 h) {
  return 0.15 + (0.06 - 0.15) * std::clamp(h / s.body, 0.0, 1.0);
}

constexpr f64 k_dx = 0.01;        // the skyline's sampling, metres
constexpr f64 k_overhang = 0.12;  // how far a course may run past the module's end

struct Skyline {
  f64 x_lo = 0;
  Vector<f64> top;  // per sample
  Vector<i32> id;   // the stone whose top it is, -1 none
  u32 at(f64 x) const {
    const f64 i = std::floor((x - x_lo) / k_dx + 0.5);
    return static_cast<u32>(std::clamp(i, 0.0, static_cast<f64>(top.size() - 1)));
  }
  f64 x_of(u32 i) const { return x_lo + static_cast<f64>(i) * k_dx; }
  // The samples a stone over [a, b) covers: those at or past a and short of b, so two stones
  // laid end to end share none.
  u32 first_at(f64 a) const {
    const f64 i = std::ceil((a - x_lo) / k_dx - 1e-6);
    return static_cast<u32>(std::clamp(i, 0.0, static_cast<f64>(top.size())));
  }
  f64 max_over(f64 a, f64 b) const {
    f64 m = -1e9;
    const u32 e = std::max(first_at(b), first_at(a) + 1);
    for (u32 i = first_at(a); i < e && i < top.size(); ++i)
      m = std::max(m, top[i]);
    return m;
  }
  void set(f64 a, f64 b, f64 y, i32 stone) {
    for (u32 i = first_at(a), e = first_at(b); i < e && i < top.size(); ++i) {
      top[i] = y;
      id[i] = stone;
    }
  }
  // A joint of the stones below within `tolerance` of x: the place two neighbours meet.
  bool joint_near(f64 x, f64 tolerance, f64& where) const {
    const u32 a = at(x - tolerance), e = at(x + tolerance);
    for (u32 i = a; i < e && i + 1 < top.size(); ++i)
      if (id[i] >= 0 && id[i + 1] >= 0 && id[i] != id[i + 1]) {
        where = x_of(i) + 0.5 * k_dx;
        return true;
      }
    return false;
  }
};

struct Solver {
  Solver(const Module& module, const Section& section, Builder& builder, ModuleReport& r, u64 seed)
      : m(module), s(section), b(builder), report(r), rng(seed) {}
  const Module& m;
  Section s;
  Builder& b;
  ModuleReport& report;
  Rng rng;
  Skyline sky[2];  // [0] the +z face, [1] the -z face
  f64 x_start = 0, x_end = 0;
  bool head = false;

  // Where the face courses stop, height above the ground: the cover's bed for a standing wall,
  // falling toward the high end of a broken one, the footing's course in a fallen one.
  f64 broken_from() const { return -0.5 * s.L + 0.3 * s.L; }
  f64 limit(f64 x) const {
    switch (m.kind) {
      case ModuleKind::fallen: return 0.16;
      case ModuleKind::broken: {
        const f64 x0 = broken_from();
        if (x <= x0) return s.body;
        const f64 t = std::clamp((x - x0) / (0.5 * s.L - x0), 0.0, 1.0);
        return s.body + (0.16 - s.body) * t;
      }
      default: return s.body;
    }
  }
  bool ragged(f64 x) const {
    return m.kind == ModuleKind::fallen || (m.kind == ModuleKind::broken && x > broken_from());
  }

  i32 add_stone(const Stone& st) {
    report.stones.push_back(st);
    if (st.role != StoneRole::pinning && st.role != StoneRole::rubble && st.role != StoneRole::core)
      report.stone_volume_m3 += static_cast<f64>(st.x1 - st.x0) *
                                static_cast<f64>(st.top - st.bed) *
                                std::abs(static_cast<f64>(st.z1 - st.z0));
    return static_cast<i32>(report.stones.size() - 1);
  }

  // A block between two levels: its corners jittered within its beds (flat to 5 mm) and its face.
  void block(f64 x0, f64 x1, f64 bed, f64 top, f64 za, f64 zb, f64 za_top, f64 zb_top, i32 face,
             f64 jitter_face, u64 seed) {
    Rng r{seed};
    const f64 j = 0.004;
    D3 c[8] = {{x0, bed, za},     {x1, bed, za},     {x1, bed, zb},     {x0, bed, zb},
               {x0, top, za_top}, {x1, top, za_top}, {x1, top, zb_top}, {x0, top, zb_top}};
    for (D3& p : c) {
      p.x += r.range(-0.006, 0.006);
      p.y += r.range(-j, j);
    }
    if (face != 0) {
      // The face is the ring's z-high side for +z and z-low for -z: jitter it proud or sunk.
      const u32 a = face > 0 ? 2 : 0, e = face > 0 ? 3 : 1;
      for (u32 k : {a, e, a + 4, e + 4})
        c[k].z += static_cast<f64>(face) * r.range(-jitter_face, jitter_face);
    }
    const i32 split = face > 0 ? 4 : face < 0 ? 2 : -1;
    body(b, c, static_cast<u32>(r.bits() % k_stone_swatches), split, r.range(0.004, 0.014));
  }

  void face_stone(i32 side, f64 x0, f64 x1, f64 bed, f64 top, StoneRole role, u16 sweep) {
    const f64 sg = side > 0 ? 1.0 : -1.0;
    const f64 xm = 0.5 * (x0 + x1);
    const f64 hb = bed - s.base(xm), ht = top - s.base(xm);
    f64 out_b = 0.5 * s.width(std::max(hb, 0.0)), out_t = 0.5 * s.width(std::max(ht, 0.0));
    if (role == StoneRole::footing) {
      out_b += k_scarcement_m;
      out_t += k_scarcement_m;
    }
    const f64 len = x1 - x0;
    const f64 tail_max = std::max(0.12, out_t - 0.04);
    f64 tail = std::clamp(len * rng.range(0.9, 1.5), std::min(0.16, tail_max), tail_max);
    // A head's runners reach the wall's middle, so its end shows stone and not the hearting.
    if (head && x0 <= -0.5 * s.L + 1e-6) tail = std::min(out_b, out_t);
    const f64 in_b = out_b - tail, in_t = out_t - tail;
    Stone st;
    st.x0 = static_cast<f32>(x0);
    st.x1 = static_cast<f32>(x1);
    st.bed = static_cast<f32>(bed);
    st.top = static_cast<f32>(top);
    st.z0 = static_cast<f32>(sg * in_b);
    st.z1 = static_cast<f32>(sg * out_b);
    st.side = static_cast<i8>(side);
    st.sweep = sweep;
    st.role = role;
    const i32 id = add_stone(st);
    const f64 g = 0.002;  // a joint's width either side
    if (side > 0)
      block(x0 + g, x1 - g, bed, top, in_b, out_b, in_t, out_t, 1, 0.012, rng.bits());
    else
      block(x0 + g, x1 - g, bed, top, -out_b, -in_b, -out_t, -in_t, -1, 0.012, rng.bits());
    sky[side > 0 ? 0 : 1].set(x0, x1, top, id);
  }

  // Pinnings: the small stones knocked into the face under a stone where it bridges a hollow.
  void pin_under(i32 side, f64 x0, f64 x1, f64 bed, u16 sweep) {
    const Skyline& k = sky[side > 0 ? 0 : 1];
    u32 i = k.first_at(x0);
    const u32 e = k.first_at(x1);
    while (i < e && i < k.top.size()) {
      if (bed - k.top[i] <= 0.015) {
        ++i;
        continue;
      }
      u32 j = i;
      f64 low = k.top[i];
      while (j < e && j < k.top.size() && bed - k.top[j] > 0.015) {
        low = std::min(low, k.top[j]);
        ++j;
      }
      const f64 a = k.x_of(i), z = k.x_of(j);
      if (z - a >= 0.03) {
        const f64 sg = side > 0 ? 1.0 : -1.0;
        const f64 xm = 0.5 * (a + z);
        const f64 out = 0.5 * s.width(std::max(bed - s.base(xm), 0.0)) - 0.004;
        Stone st;
        st.x0 = static_cast<f32>(a);
        st.x1 = static_cast<f32>(z);
        st.bed = static_cast<f32>(low);
        st.top = static_cast<f32>(bed);
        st.z0 = static_cast<f32>(sg * (out - 0.08));
        st.z1 = static_cast<f32>(sg * out);
        st.side = static_cast<i8>(side);
        st.sweep = sweep;
        st.role = StoneRole::pinning;
        add_stone(st);
        if (side > 0)
          block(a + 0.003, z - 0.003, low + 0.002, bed - 0.002, out - 0.08, out, out - 0.08, out, 1,
                0.004, rng.bits());
        else
          block(a + 0.003, z - 0.003, low + 0.002, bed - 0.002, -out, -out + 0.08, -out,
                -out + 0.08, -1, 0.004, rng.bits());
      }
      i = j;
    }
  }

  // A stone across the whole wall: a through-stone in the row, a tie at a head, a cover stone.
  void across(f64 x0, f64 x1, f64 bed, f64 top, f64 proud, StoneRole role, u16 sweep) {
    const f64 xm = 0.5 * (x0 + x1);
    const f64 hb = bed - s.base(xm), ht = top - s.base(xm);
    const f64 wb = 0.5 * s.width(std::max(hb, 0.0)) + proud,
              wt = 0.5 * s.width(std::max(ht, 0.0)) + proud;
    Stone st;
    st.x0 = static_cast<f32>(x0);
    st.x1 = static_cast<f32>(x1);
    st.bed = static_cast<f32>(bed);
    st.top = static_cast<f32>(top);
    st.z0 = static_cast<f32>(-wb);
    st.z1 = static_cast<f32>(wb);
    st.side = 0;
    st.sweep = sweep;
    st.role = role;
    const i32 id = add_stone(st);
    block(x0 + 0.002, x1 - 0.002, bed, top, -wb, wb, -wt, wt, 0, 0.0, rng.bits());
    sky[0].set(x0, x1, top, id);
    sky[1].set(x0, x1, top, id);
  }

  void lay_faces() {
    const u32 n = static_cast<u32>(std::floor((s.L + 2 * k_overhang) / k_dx + 0.5)) + 1;
    for (Skyline& k : sky) {
      k.x_lo = -0.5 * s.L - k_overhang;
      k.top.resize(n);
      k.id.resize(n);
      for (u32 i = 0; i < n; ++i) {
        k.top[i] = s.base(k.x_of(i)) - s.foot;
        k.id[i] = -1;
      }
    }
    // The row of through-stones: nominal 0.90 m apart from a draw's start (dry-stone-walls.md).
    Vector<f64> throughs;
    Vector<u8> placed;
    if (m.kind == ModuleKind::run || m.kind == ModuleKind::head || m.kind == ModuleKind::broken) {
      // As many as keep them under k_through_spacing_m apart, half a spacing in from each end,
      // so the row runs on across the next module at the same spacing.
      const u32 rows = static_cast<u32>(std::ceil(s.L / k_through_spacing_m));
      const f64 step = s.L / static_cast<f64>(rows);
      for (u32 i = 0; i < rows; ++i) {
        const f64 xt = -0.5 * s.L + step * (static_cast<f64>(i) + 0.5) + rng.range(-0.03, 0.03);
        if (m.kind != ModuleKind::broken || xt < broken_from()) throughs.push_back(xt);
      }
      placed.resize(throughs.size(), 0);
    }
    for (u16 sweep = 0; sweep < 48; ++sweep) {
      bool any = false;
      const f64 sweep_scale = rng.range(0.88, 1.12);
      // The through-stones' row: a through is laid at its planned place at its own height in the
      // band (0.52 to 0.60 m) in the sweep before the faces could pass it — a sweep raises a face
      // by a course and a step, at most 0.34 m — packed under where the faces are lower, or on
      // what is there when they already stand higher; the stones of either face then bed
      // against it.
      for (u32 t = 0; t < throughs.size() && sweep > 0; ++t) {
        if (placed[t]) continue;
        const f64 lt = rng.range(0.24, 0.32);
        const f64 want = rng.range(k_through_low_m + 0.02, k_through_low_m + 0.10);
        f64 t0 = throughs[t] - 0.5 * lt, t1 = throughs[t] + 0.5 * lt;
        // Its ends off the joints below on either face, by lengthening it.
        for (u32 pass = 0; pass < 2; ++pass)
          for (const Skyline& below : sky) {
            f64 jx = 0;
            if (below.joint_near(t0, 0.035, jx)) t0 = jx - 0.045;
            if (below.joint_near(t1, 0.035, jx)) t1 = jx + 0.045;
          }
        const f64 under = std::max(sky[0].max_over(t0, t1), sky[1].max_over(t0, t1));
        if (under - s.base(throughs[t]) < want - 0.14) continue;
        const f64 bed = std::max(under, s.base(throughs[t]) + want);
        placed[t] = 1;
        pin_under(1, t0, t1, bed, sweep);
        pin_under(-1, t0, t1, bed, sweep);
        across(t0, t1, bed, bed + rng.range(0.10, 0.14), rng.range(0.03, 0.10), StoneRole::through,
               sweep);
        any = true;
      }
      for (i32 side : {1, -1}) {
        Skyline& k = sky[side > 0 ? 0 : 1];
        f64 x = head ? -0.5 * s.L : -0.5 * s.L - k_overhang * rng.range(0.0, 1.0);
        const f64 end = 0.5 * s.L + k_overhang * rng.range(0.0, 1.0);
        bool first = true;
        f64 prev_end = x;
        while (x < end - 0.03) {
          const f64 here = k.top[k.at(x)] - s.base(x);
          if (here >= limit(x) - 0.03) {
            x += 0.02;
            first = false;
            continue;
          }
          // A stone that does not start where the last one ended (after a stretch already at
          // the cover, or a through) starts off any joint below, as an end does.
          if (x != prev_end && !first) {
            f64 jx = 0;
            if (k.joint_near(x, 0.035, jx)) {
              x = jx + 0.045;
              continue;
            }
          }
          const bool footing = sweep == 0;
          f64 h = footing ? rng.range(0.15, 0.19)
                          : graded(s, std::max(here, 0.0)) * sweep_scale * rng.range(0.9, 1.1);
          f64 len =
              footing ? rng.range(0.32, 0.60) : std::clamp(h * rng.range(1.5, 4.0), 0.12, 0.60);
          // A head: ties and runners alternating (TCV, Wall heads), flush with the wall's end.
          if (head && first && x <= -0.5 * s.L + 1e-9 && !footing) {
            if (sweep % 2 == 1 && side > 0) {
              const f64 lt = rng.range(0.28, 0.40);
              const f64 bed = std::max(sky[0].max_over(x, x + lt), sky[1].max_over(x, x + lt));
              const f64 ht = std::min(bed + h, s.base(x) + limit(x));
              if (ht - bed > 0.03) {
                pin_under(1, x, x + lt, bed, sweep);
                pin_under(-1, x, x + lt, bed, sweep);
                across(x, x + lt, bed, ht, 0.0, StoneRole::tie, sweep);
                x += lt;
                first = false;
                any = true;
                continue;
              }
            }
            len = std::max(len, 0.35);  // a runner
          }
          first = false;
          // Where the stone may end: no further than a step in what is under it (a rise of more
          // than half its height), and — one on two — never within 3.5 cm of a joint below.
          f64 step = end;
          const f64 under0 = k.top[k.at(x)];
          for (u32 i = k.at(x) + 1, ie = k.at(std::min(x + len, end)); i < ie; ++i)
            if (k.top[i] > under0 + 0.5 * h && k.x_of(i) - x >= 0.08) {
              step = k.x_of(i);
              break;
            }
          f64 e = std::min(std::min(x + len, end), step);
          for (u32 attempt = 0; attempt < 4; ++attempt) {
            f64 jx = 0;
            if (!k.joint_near(e, 0.035, jx)) break;
            const f64 past = jx + 0.05, short_of = jx - 0.04;
            // Past it when the stone may run on; short of it when that leaves a stone; else past
            // it over the step (the hollow under it pinned): never on it. Again, since the new
            // end may meet another joint.
            if (past <= step && past - x <= 0.65)
              e = past;
            else if (short_of - x >= 0.06)
              e = short_of;
            else
              e = std::min(past, end);
          }
          if (e - x < 0.06) e = std::min(x + 0.10, end);
          const f64 bed = k.max_over(x, e);
          h = footing ? h
                      : graded(s, std::max(bed - s.base(0.5 * (x + e)), 0.0)) * sweep_scale *
                            rng.range(0.9, 1.1);
          f64 top = bed + h;
          if (e >= step - 1e-9 && step < end) {
            // Ended against a step: brought up level with it (a jumper, up to two courses, at most
            // 0.28 m), so the next course crosses the joint instead of climbing beside it.
            const f64 beside =
                k.top[std::min(k.first_at(step), static_cast<u32>(k.top.size() - 1))];
            if (beside > top && beside - bed <= 0.28) top = beside;
          }
          {
            // A standing course stops at the cover's bed; a fallen one is left ragged, a course
            // or so above what still stands.
            const f64 over = ragged(0.5 * (x + e)) ? 0.12 : 0.0;
            const f64 cap = std::min(s.base(x) + limit(x), s.base(e) + limit(e)) + over;
            if (top > cap) top = cap;
          }
          if (top - bed < 0.03) {
            k.set(x, e, std::max(k.top[k.at(x)], top), -1);
            x = e;
            continue;
          }
          pin_under(side, x, e, bed, sweep);
          face_stone(side, x, e, bed, top, footing ? StoneRole::footing : StoneRole::face, sweep);
          x = e;
          prev_end = e;
          any = true;
        }
      }
      if (!any) break;
    }
  }

  // The cover course across the full width, on the line of the cover's bed, and the cope on edge
  // standing on it, each stone vertical (TCV, Coping); the first at a head a large square topstone.
  void lay_top() {
    if (m.kind == ModuleKind::fallen) return;
    const f64 x0 = head ? -0.5 * s.L : -0.5 * s.L - rng.range(0.0, 0.04);
    const f64 x1 = m.kind == ModuleKind::broken ? broken_from() - rng.range(0.0, 0.10)
                                                : 0.5 * s.L + rng.range(0.0, 0.04);
    Vector<f64> cover_x, cover_t;  // each cover stone's start and its thickness
    for (f64 x = x0; x < x1 - 0.05;) {
      const f64 e = std::min(x + rng.range(0.25, 0.45), x1);
      const f64 t = rng.range(0.06, 0.09);
      const f64 proud = rng.range(0.02, 0.04);
      const f64 hw = 0.5 * s.top_w + proud;
      const f64 bed0 = s.base(x) + s.body, bed1 = s.base(e) + s.body;
      Stone st;
      st.x0 = static_cast<f32>(x);
      st.x1 = static_cast<f32>(e);
      st.bed = static_cast<f32>(std::min(bed0, bed1));
      st.top = static_cast<f32>(std::max(bed0, bed1) + t);
      st.z0 = static_cast<f32>(-hw);
      st.z1 = static_cast<f32>(hw);
      st.role = StoneRole::cover;
      add_stone(st);
      D3 c[8] = {{x + 0.002, bed0, -hw},    {e - 0.002, bed1, -hw},     {e - 0.002, bed1, hw},
                 {x + 0.002, bed0, hw},     {x + 0.002, bed0 + t, -hw}, {e - 0.002, bed1 + t, -hw},
                 {e - 0.002, bed1 + t, hw}, {x + 0.002, bed0 + t, hw}};
      for (D3& p : c)
        p.z += rng.range(-0.008, 0.008);
      body(b, c, static_cast<u32>(rng.bits() % k_stone_swatches), -1, 0.0);
      cover_x.push_back(x);
      cover_t.push_back(t);
      x = e;
    }
    auto cover_top = [&](f64 x) {
      u32 i = 0;
      while (i + 1 < cover_x.size() && cover_x[i + 1] <= x)
        ++i;
      return s.base(x) + s.body + (cover_t.empty() ? 0.075 : cover_t[i]);
    };
    bool first = true;
    for (f64 x = x0; x < x1 - 0.04;) {
      const bool big = head && first;
      const f64 t = big ? 0.30 : rng.range(0.05, 0.12);
      const f64 e = std::min(x + t, x1);
      const f64 hw = big ? 0.5 * s.top_w + 0.02 : 0.5 * s.top_w * rng.range(0.78, 1.05);
      const f64 zc = rng.range(-0.015, 0.015);
      const f64 b0 = cover_top(x), b1 = cover_top(e);
      const f64 hc = s.cope * (big ? 1.1 : 1.0);
      // A stone's own height, then its two ends' split: a ragged line of slabs on edge.
      const f64 tall = rng.range(0.90, 1.08);
      const f64 t0 = b0 + hc * tall * rng.range(0.90, 1.04);
      const f64 t1 = b1 + hc * tall * rng.range(0.90, 1.04);
      const f64 lean = rng.range(-0.025, 0.025);
      Stone st;
      st.x0 = static_cast<f32>(x);
      st.x1 = static_cast<f32>(e);
      st.bed = static_cast<f32>(std::min(b0, b1));
      st.top = static_cast<f32>(std::max(t0, t1));
      st.z0 = static_cast<f32>(zc - hw);
      st.z1 = static_cast<f32>(zc + hw);
      st.role = StoneRole::cope;
      add_stone(st);
      const f64 xa = x + 0.002, xb = e - 0.002;
      D3 c[8] = {{xa, b0, zc - hw},        {xb, b1, zc - hw},        {xb, b1, zc + hw},
                 {xa, b0, zc + hw},        {xa + lean, t0, zc - hw}, {xb + lean, t1, zc - hw},
                 {xb + lean, t1, zc + hw}, {xa + lean, t0, zc + hw}};
      for (u32 k = 4; k < 8; ++k)
        c[k].z += rng.range(-0.025, 0.025);
      body(b, c, static_cast<u32>(rng.bits() % k_stone_swatches), 1, rng.range(0.004, 0.012));
      x = e + rng.range(0.003, 0.015);
      first = false;
    }
  }

  // The stone of what fell, lying at the foot on both sides, most of it close to the face
  // (TCV, Gapping: the gap's stone "from alongside the gap").
  void lay_rubble() {
    if (m.kind != ModuleKind::fallen && m.kind != ModuleKind::broken) return;
    const f64 xa = m.kind == ModuleKind::fallen ? -0.5 * s.L : broken_from() - 0.2;
    const f64 xb = 0.5 * s.L;
    const u32 count = static_cast<u32>((xb - xa) * (m.kind == ModuleKind::fallen ? 48.0 : 30.0));
    for (u32 i = 0; i < count; ++i) {
      const f64 sg = rng.next() < 0.5 ? 1.0 : -1.0;
      const f64 d = 0.02 + 1.4 * rng.next() * rng.next();
      const f64 x = rng.range(xa, xb);
      const f64 h = rng.range(0.05, 0.14), len = rng.range(0.14, 0.42), dep = rng.range(0.14, 0.32);
      const f64 z = sg * (0.5 * s.base_w + d + 0.5 * dep);
      const f64 heap = 0.30 * std::max(0.0, 1.0 - d / 0.9) * rng.next();
      const f64 y = s.base(x) - 0.03 + heap;
      const f64 a = rng.range(0.0, 3.141592653589793);
      const f64 ca = det::cos(a), sa = det::sin(a);
      const f64 tx = rng.range(-0.25, 0.25), tz = rng.range(-0.25, 0.25);  // a tilt, as slopes
      D3 c[8];
      const f64 hx = 0.5 * len, hz = 0.5 * dep;
      const f64 loc[4][2] = {{-hx, -hz}, {hx, -hz}, {hx, hz}, {-hx, hz}};
      for (u32 k = 0; k < 8; ++k) {
        const f64 lx = loc[k % 4][0], lz = loc[k % 4][1];
        const f64 px = x + ca * lx - sa * lz, pz = z + sa * lx + ca * lz;
        const f64 py = y + (k >= 4 ? h : 0.0) + tx * lx + tz * lz;
        c[k] = {px + rng.range(-0.01, 0.01), py + rng.range(-0.005, 0.005),
                pz + rng.range(-0.01, 0.01)};
      }
      Stone st;
      st.x0 = static_cast<f32>(x - hx);
      st.x1 = static_cast<f32>(x + hx);
      st.bed = static_cast<f32>(y);
      st.top = static_cast<f32>(y + h);
      st.z0 = static_cast<f32>(z - hz);
      st.z1 = static_cast<f32>(z + hz);
      st.side = static_cast<i8>(sg > 0 ? 1 : -1);
      st.role = StoneRole::rubble;
      add_stone(st);
      body(b, c, static_cast<u32>(rng.bits() % k_stone_swatches), 1, rng.range(0.004, 0.012));
    }
  }

  // The hearting: a dark closed core between the faces' tails, so a joint shows shadow.
  void lay_core() {
    const f64 xa = head ? -0.5 * s.L + 0.01 : -0.5 * s.L - 0.06, xb = 0.5 * s.L + 0.06;
    const u32 n = static_cast<u32>(std::ceil((xb - xa) / 0.25));
    Vector<D3> lo_n, lo_p, hi_n, hi_p;  // per station: bottom -z, bottom +z, top -z, top +z
    for (u32 i = 0; i <= n; ++i) {
      const f64 x = xa + (xb - xa) * static_cast<f64>(i) / static_cast<f64>(n);
      const f64 top_h = ragged(x) ? std::max(limit(x) - 0.06, 0.02) : limit(x) - 0.01;
      const f64 yb = s.base(x) - s.foot + 0.03, yt = s.base(x) + top_h;
      const f64 wb = std::max(0.5 * s.width(0.0) - 0.07, 0.05);
      const f64 wt = std::max(0.5 * s.width(top_h) - 0.07, 0.05);
      lo_n.push_back({x, yb, -wb});
      lo_p.push_back({x, yb, wb});
      hi_n.push_back({x, yt, -wt});
      hi_p.push_back({x, yt, wt});
    }
    const u32 sw = k_swatch_core;
    auto quad = [&](D3 a, D3 c1, D3 c2, D3 d, D3 out) {
      D3 nrm = unit(cross(c1 - a, d - a));
      if (dot(nrm, out) < 0) nrm = nrm * -1.0;
      f64 u, v;
      swatch_uv(sw, 0.5, 0.5, u, v);
      const u32 i0 = b.vertex(a, nrm, u, v), i1 = b.vertex(c1, nrm, u, v),
                i2 = b.vertex(c2, nrm, u, v), i3 = b.vertex(d, nrm, u, v);
      if (dot(cross(c1 - a, c2 - a), nrm) > 0) {
        b.tri(i0, i1, i2);
        b.tri(i0, i2, i3);
      } else {
        b.tri(i0, i2, i1);
        b.tri(i0, i3, i2);
      }
    };
    for (u32 i = 0; i < n; ++i) {
      quad(lo_p[i], lo_p[i + 1], hi_p[i + 1], hi_p[i], {0, 0, 1});
      quad(lo_n[i], lo_n[i + 1], hi_n[i + 1], hi_n[i], {0, 0, -1});
      quad(hi_n[i], hi_n[i + 1], hi_p[i + 1], hi_p[i], {0, 1, 0});
    }
    quad(lo_n[0], lo_p[0], hi_p[0], hi_n[0], {-1, 0, 0});
    quad(lo_n[n], lo_p[n], hi_p[n], hi_n[n], {1, 0, 0});
  }
};

// ---- the coarser forms ------------------------------------------------------------------------

// A battered face as a grid over x stations and levels, smooth-shaded: the mid form's courses as
// bands (proud at a band's middle, sunk at its joint), or the outline's plain face.
void face_grid(Builder& b, const Section& s, i32 side, f64 xa, f64 xb, u32 columns,
               const Vector<f64>& levels, const Vector<f64>& proud, f64 (*top_of)(const void*, f64),
               const void* ctx, f64 uv_scale_u, f64 uv_scale_v, u32 swatch) {
  const f64 sg = side > 0 ? 1.0 : -1.0;
  const u32 rows = static_cast<u32>(levels.size());
  const u32 first = static_cast<u32>(b.mesh.positions.size());
  for (u32 c = 0; c <= columns; ++c) {
    const f64 x = xa + (xb - xa) * static_cast<f64>(c) / static_cast<f64>(columns);
    const f64 top = top_of(ctx, x);
    for (u32 r = 0; r < rows; ++r) {
      const f64 h = std::min(levels[r], top);
      const f64 z = sg * (0.5 * s.width(std::max(h, 0.0)) + proud[r]);
      const D3 p{x, s.base(x) + h, z};
      f64 u, v;
      if (swatch == k_swatch_mid) {
        // A mosaic of the stones' own swatches (rows 0..13), a swatch about a stone's size, so the
        // mid form averages to the near form's colour.
        u = x * uv_scale_u;
        v = std::clamp((h + s.foot) / (s.body + s.foot), 0.0, 1.0) * uv_scale_v;
      } else {
        swatch_uv(swatch, 0.5, 0.5, u, v);
      }
      b.vertex(p, {0, 0, sg}, u, v);
    }
  }
  // Smooth normals from the grid's own quads.
  Vector<D3> acc;
  acc.resize((columns + 1) * rows);
  auto at = [&](u32 c, u32 r) { return first + c * rows + r; };
  auto pos = [&](u32 i) {
    const Vec3 p = b.mesh.positions[i];
    return D3{static_cast<f64>(p.x), static_cast<f64>(p.y), static_cast<f64>(p.z)};
  };
  for (u32 c = 0; c < columns; ++c)
    for (u32 r = 0; r + 1 < rows; ++r) {
      const u32 i0 = at(c, r), i1 = at(c + 1, r), i2 = at(c + 1, r + 1), i3 = at(c, r + 1);
      D3 n = cross(pos(i2) - pos(i0), pos(i3) - pos(i1));
      if (n.z * sg < 0) n = n * -1.0;
      for (u32 i : {i0, i1, i2, i3})
        acc[i - first] = acc[i - first] + n;
      if (dot(cross(pos(i1) - pos(i0), pos(i2) - pos(i0)), n) > 0) {
        b.tri(i0, i1, i2);
        b.tri(i0, i2, i3);
      } else {
        b.tri(i0, i2, i1);
        b.tri(i0, i3, i2);
      }
    }
  for (u32 i = 0; i < acc.size(); ++i)
    b.mesh.normals[first + i] = f3(unit(acc[i]));
}

struct TopCtx {
  const Section* s;
  ModuleKind kind;
  f64 broken_from;
};
f64 top_of(const void* ctx, f64 x) {
  const auto& t = *static_cast<const TopCtx*>(ctx);
  switch (t.kind) {
    case ModuleKind::fallen: return 0.16;
    case ModuleKind::broken: {
      if (x <= t.broken_from) return t.s->body;
      const f64 f = std::clamp((x - t.broken_from) / (0.5 * t.s->L - t.broken_from), 0.0, 1.0);
      return t.s->body + (0.16 - t.s->body) * f;
    }
    default: return t.s->body;
  }
}

// A box over [x0, x1] on the line y = base(x) + h0..h1, the width w across: the cover, the cope's
// groups, the outline's top.
void top_box(Builder& b, const Section& s, f64 x0, f64 x1, f64 h0, f64 h1, f64 hw, u32 swatch) {
  const D3 c[8] = {{x0, s.base(x0) + h0, -hw}, {x1, s.base(x1) + h0, -hw},
                   {x1, s.base(x1) + h0, hw},  {x0, s.base(x0) + h0, hw},
                   {x0, s.base(x0) + h1, -hw}, {x1, s.base(x1) + h1, -hw},
                   {x1, s.base(x1) + h1, hw},  {x0, s.base(x0) + h1, hw}};
  body(b, c, swatch, -1, 0.0);
}

void mid_form(Builder& b, const Module& m, const Section& s, const ModuleReport& report,
              f64 broken_from) {
  const TopCtx ctx{&s, m.kind, broken_from};
  const bool head = m.kind == ModuleKind::head;
  const f64 xa = head ? -0.5 * s.L : -0.5 * s.L - 0.04, xb = 0.5 * s.L + 0.04;
  Vector<f64> levels, proud;
  f64 h = -s.foot;
  levels.push_back(h);
  proud.push_back(m.kind == ModuleKind::strip ? 0.0 : -0.004);
  while (h < s.body - 0.02) {
    const f64 t = std::min(graded(s, std::max(h, 0.0)), s.body - h);
    levels.push_back(h + 0.5 * t);
    proud.push_back(0.012);
    levels.push_back(h + t);
    proud.push_back(-0.008);
    h += t;
  }
  const u32 columns = static_cast<u32>(std::ceil((xb - xa) / 0.5));
  face_grid(b, s, 1, xa, xb, columns, levels, proud, top_of, &ctx, 0.6, 13.0 / 16.0, k_swatch_mid);
  face_grid(b, s, -1, xa, xb, columns, levels, proud, top_of, &ctx, 0.6, 13.0 / 16.0, k_swatch_mid);
  if (m.kind == ModuleKind::fallen || m.kind == ModuleKind::broken) {
    // The fallen stone as a low heap either side.
    const f64 ra = m.kind == ModuleKind::fallen ? xa : broken_from - 0.2;
    for (f64 sg : {1.0, -1.0}) {
      const D3 c[8] = {{ra, s.base(ra) - 0.03, sg * 0.5 * s.base_w},
                       {xb, s.base(xb) - 0.03, sg * 0.5 * s.base_w},
                       {xb, s.base(xb) - 0.03, sg * (0.5 * s.base_w + 1.1)},
                       {ra, s.base(ra) - 0.03, sg * (0.5 * s.base_w + 1.1)},
                       {ra, s.base(ra) + 0.16, sg * 0.5 * s.base_w},
                       {xb, s.base(xb) + 0.16, sg * 0.5 * s.base_w},
                       {xb, s.base(xb) + 0.02, sg * (0.5 * s.base_w + 1.0)},
                       {ra, s.base(ra) + 0.02, sg * (0.5 * s.base_w + 1.0)}};
      body(b, c, k_swatch_mid, -1, 0.0);
    }
  }
  if (m.kind == ModuleKind::fallen) return;
  // The cover as one slab and the cope as blocks of three stones each, their tops the highest.
  const f64 ca = head ? -0.5 * s.L : -0.5 * s.L - 0.02;
  const f64 cb = m.kind == ModuleKind::broken ? broken_from - 0.05 : 0.5 * s.L + 0.02;
  top_box(b, s, ca, cb, s.body, s.body + 0.075, 0.5 * s.top_w + 0.03, k_swatch_mid);
  u32 in_group = 0;
  f64 gx0 = 0, gtop = 0;
  for (const Stone& st : report.stones) {
    if (st.role != StoneRole::cope) continue;
    if (in_group == 0) {
      gx0 = static_cast<f64>(st.x0);
      gtop = 0;
    }
    // The block's top above the line, at its stones' highest.
    const f64 xm = 0.5 * static_cast<f64>(st.x0 + st.x1);
    gtop = std::max(gtop, static_cast<f64>(st.top) - s.base(xm) - s.body);
    if (++in_group == 3) {
      top_box(b, s, gx0, static_cast<f64>(st.x1), s.body + 0.07, s.body + gtop, 0.5 * s.top_w,
              k_swatch_mid);
      in_group = 0;
    }
  }
  if (in_group > 0) {
    const Stone* last = nullptr;
    for (const Stone& st : report.stones)
      if (st.role == StoneRole::cope) last = &st;
    top_box(b, s, gx0, static_cast<f64>(last->x1), s.body + 0.07, s.body + gtop, 0.5 * s.top_w,
            k_swatch_mid);
  }
}

void far_form(Builder& b, const Module& m, const Section& s, f64 broken_from) {
  const TopCtx ctx{&s, m.kind, broken_from};
  const bool head = m.kind == ModuleKind::head;
  const f64 xa = head ? -0.5 * s.L : -0.5 * s.L - 0.02, xb = 0.5 * s.L + 0.02;
  const Vector<f64> levels{-s.foot, s.body};
  const Vector<f64> proud{0.0, 0.0};
  const u32 columns = m.kind == ModuleKind::broken ? 6 : 1;
  face_grid(b, s, 1, xa, xb, columns, levels, proud, top_of, &ctx, 0.6, 13.0 / 16.0, k_swatch_mid);
  face_grid(b, s, -1, xa, xb, columns, levels, proud, top_of, &ctx, 0.6, 13.0 / 16.0, k_swatch_mid);
  if (m.kind == ModuleKind::fallen) return;
  const f64 cb = m.kind == ModuleKind::broken ? broken_from : xb;
  top_box(b, s, xa, cb, s.body, s.H, 0.5 * s.top_w, k_swatch_mid);
}

}  // namespace

f64 body_height(const Module& m) noexcept { return section_of(m).body; }

f64 batter(const Module& m) noexcept {
  const Section s = section_of(m);
  return 0.5 * (s.base_w - s.top_w) / s.body;
}

bool module_in_range(const Module& m, std::string& why) {
  const bool strip = m.kind == ModuleKind::strip;
  if (m.kind > ModuleKind::strip) return why = "kind is not one the solver has", false;
  if (m.grade_permille < -500 || m.grade_permille > 500)
    return why = "grade_permille must be -500..500", false;
  if (m.length_cm < 100 || m.length_cm > 600) return why = "length_cm must be 100..600", false;
  if (m.height_cm < 100 || m.height_cm > 180) return why = "height_cm must be 100..180", false;
  if (m.cope_cm < 25 || m.cope_cm > 30) return why = "cope_cm must be 25..30", false;
  if (m.base_width_cm < 60 || m.base_width_cm > 90)
    return why = "base_width_cm must be 60..90", false;
  if (m.top_width_cm < 30 || m.top_width_cm > 50) return why = "top_width_cm must be 30..50", false;
  if (m.footing_cm < 5 || m.footing_cm > 20) return why = "footing_cm must be 5..20", false;
  const f64 bt = batter(m);
  if (!strip && (bt < 0.1 - 1e-9 || bt > 1.0 / 6.0 + 1e-9))
    return why = "the widths and the height give a batter outside 1:6 to 1:10", false;
  return true;
}

bool solve_module(const Module& m, assets::MeshData& out, ModuleReport& report,
                  std::string& error) {
  out = {};
  report = {};
  if (!module_in_range(m, error)) return false;
  Builder b(out);
  const Section s = section_of(m);
  report.length_m = s.L;
  b.indices = &out.indices;
  Solver solver(m, s, b, report,
                hash_combine(hash_combine(m.seed, k_wall_version), static_cast<u64>(m.kind)));
  solver.head = m.kind == ModuleKind::head;
  const f64 broken_from = solver.broken_from();
  if (m.kind == ModuleKind::strip) {
    far_form(b, m, s, broken_from);
    report.near_triangles = b.triangles;
  } else {
    solver.lay_faces();
    solver.lay_top();
    solver.lay_rubble();
    solver.lay_core();
    report.near_triangles = b.triangles;
    b.triangles = 0;
    b.indices = &report.mid_indices;
    mid_form(b, m, s, report, broken_from);
    report.mid_triangles = b.triangles;
    b.triangles = 0;
    b.indices = &report.far_indices;
    far_form(b, m, s, broken_from);
    report.far_triangles = b.triangles;
  }
  assets::Primitive part;
  part.first_index = 0;
  part.index_count = static_cast<u32>(out.indices.size());
  part.material = 0;
  out.primitives.push_back(part);
  assets::Material material;
  material.name = "wall stone";
  material.base_color = {1, 1, 1, 1};
  material.metallic = 0;
  material.roughness = 0.86f;
  material.base_color_image = 0;
  out.materials.push_back(material);
  assets::ImageRef image;
  image.name = "wall stone palette";
  image.mime_type = "image/png";
  palette_image(image.bytes);
  out.images.push_back(std::move(image));
  return true;
}

}  // namespace engine::walls
