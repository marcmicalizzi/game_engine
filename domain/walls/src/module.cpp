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
// mottled, a lighter fleck now and then, faint foliation bands), rows 14 and 15 the stones' mean
// (the mid form's and the outline's swatch, 224) but for the last swatch, the hearting's shadow.
// A stone takes one swatch, so the stones of a wall differ from each other as the reels' do
// (dry-stone-walls.md, "Where structure was traded for triangles").
constexpr u32 k_atlas = 128;
constexpr u32 k_swatch = 8;
constexpr u32 k_stone_swatches = 14 * 16;
constexpr u32 k_swatch_mid = 14 * 16;
constexpr u32 k_swatch_core = 16 * 16 - 1;

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
  // The stones' swatches first, their texels summed: every other swatch but the hearting's is
  // their mean, plain, so the coarser forms carry the near wall's colour at every mip level (the
  // hearting's one dark swatch is a 256th of the atlas, in its far corner, and reaches the mid
  // form's swatch only in the last level, the whole atlas's mean).
  f64 sum[3] = {0, 0, 0};
  for (u32 pass = 0; pass < 2; ++pass)
    for (u32 sy = 0; sy < 16; ++sy)
      for (u32 sx = 0; sx < 16; ++sx) {
        const u32 swatch = sy * 16 + sx;
        if ((swatch < k_stone_swatches) != (pass == 0)) continue;
        Rng rng{hash_combine(0x57a11e5ull, swatch)};
        f64 r = 0, g = 0, b = 0, speckle = 0.04, band = 0.03;
        if (swatch == k_swatch_core) {
          r = g = b = 26;
          speckle = 0;
          band = 0;
        } else if (swatch >= k_stone_swatches) {
          const f64 texels = static_cast<f64>(k_stone_swatches * k_swatch * k_swatch);
          r = sum[0] / texels;
          g = sum[1] / texels;
          b = sum[2] / texels;
          speckle = 0;
          band = 0;
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
            const f64 mottle = rng.next();
            f64 m = speckle > 0 ? row * (1.0 + 0.07 * (mottle * 2.0 - 1.0)) : 1.0;
            const f64 fleck = rng.next();
            if (fleck < speckle) m *= 1.22;
            const u32 px = sx * k_swatch + tx, py = sy * k_swatch + ty;
            u8* p = &pixels[(py * k_atlas + px) * 3];
            p[0] = static_cast<u8>(std::clamp(r * m + 0.5, 0.0, 255.0));
            p[1] = static_cast<u8>(std::clamp(g * m + 0.5, 0.0, 255.0));
            p[2] = static_cast<u8>(std::clamp(b * m + 0.5, 0.0, 255.0));
            if (swatch < k_stone_swatches)
              for (u32 k = 0; k < 3; ++k)
                sum[k] += static_cast<f64>(p[k]);
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

// Which of a body's six faces (0 bottom, 1 top, 2 the za side, 3 the x1 end, 4 the zb side, 5
// the x0 end) are split and which are not drawn. A split face is the face a waller shows
// (dry-stone-walls.md, "The stone's own shape"): its face proper an outline of its own, its
// arrises rounded back to the stone's corners, while the beds stay flat. A face not drawn is one
// that lies on stone or in the hearting and is never seen: a face stone's bed and tail. `edge` is
// how far the arrises are worn back (`k_edge_*`), and `crown` raises the face proper to a crown
// off its middle (four triangles, smooth-shaded) instead of a flat fan (two).
constexpr u8 k_edge_fine = 0;   // a fallen stone's top
constexpr u8 k_edge_small = 1;  // a small face: a closing stone, a stone under 0.012 m^2
constexpr u8 k_edge_stone = 2;  // a walling stone's face, a through's or a tie's end
struct Faces {
  u8 split = 0;
  u8 hidden = 0;
  u8 edge = k_edge_stone;
  u8 crown = 0x3f;  // which split faces are crowned
  u8 pillow = 0;    // which plain faces bulge: a crown over their own corners (four triangles)
};
constexpr u8 face_bit(u32 f) { return static_cast<u8>(1u << f); }

// The split face's numbers (dry-stone-walls.md, "The stone's own shape"). Each corner of the face
// proper is drawn in from both edges that meet there, independently: from an edge by a share of
// the face's dimension across it (`k_inset`), so a long stone's ends round off further than its
// beds and the outline is a quadrilateral of its own rather than the rectangle it was laid as.
// The stone's corner behind it is set back from the face's plane by 0.6 of the larger inset and 3
// to 8 mm (5 to 19 mm), so the worn arris between them is steeper than 25 degrees; the face
// proper's corners lie within 2.5 mm of the plane and its crown 0.5 to 2.5 mm proud of them.
struct Inset {
  f64 lo, hi;        // share of the dimension across the edge
  f64 min_m, max_m;  // the inset's bounds, metres
};
constexpr Inset k_inset[3] = {
    {0.04, 0.12, 0.002, 0.020},  // fine
    {0.06, 0.18, 0.003, 0.020},  // small
    {0.03, 0.14, 0.003, 0.040},  // stone
};
constexpr f64 k_ring_relief = 0.0025;
constexpr f64 k_crown_lo = 0.0005, k_crown_hi = 0.0025;
constexpr f64 k_back_max = 0.019;  // a corner's set-back at most, so a face's relief is <= 35 mm
constexpr f64 k_broken = 0.45;     // the share of a walling stone's corners broken off

// A six-faced body from its eight corners (bottom ring 0..3, top ring 4..7, the rings in one
// order: (x0, za), (x1, za), (x1, zb), (x0, zb)), each face wound outward from the body's centre.
// A split face is its face proper, four points drawn in from the corners, with a crown (four
// triangles sharing smooth normals) or fanned flat (two), and the band of worn arrises between it
// and the set-back corners (eight, flat): 12 triangles crowned, 10 flat, two more a broken corner;
// a plain face is two. The corners of a split face are moved back before any face is drawn, so the
// beds and ends that meet it stop short of the face's plane and stay flat.
void body(Builder& b, const D3 (&c0)[8], u32 swatch, Faces faces, Rng& r) {
  D3 centre{};
  for (const D3& p : c0)
    centre = centre + p;
  centre = centre * 0.125;
  D3 lo = c0[0], hi = c0[0];
  for (const D3& p : c0) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const D3 size{std::max(hi.x - lo.x, 1e-6), std::max(hi.y - lo.y, 1e-6),
                std::max(hi.z - lo.z, 1e-6)};
  static constexpr u8 rings[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
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
  auto outward = [&](const D3(&k)[8], u32 f) {
    const D3 q0 = k[rings[f][0]], q1 = k[rings[f][1]], q2 = k[rings[f][2]], q3 = k[rings[f][3]];
    const D3 mid = (q0 + q1 + q2 + q3) * 0.25;
    D3 n = unit(cross(q2 - q0, q3 - q1));
    if (dot(n, mid - centre) < 0) n = n * -1.0;
    return n;
  };
  // One flat triangle, wound so its normal agrees with `hint`.
  auto facet = [&](D3 p0, D3 p1, D3 p2, D3 hint, D3 uv_normal) {
    D3 n = cross(p1 - p0, p2 - p0);
    if (dot(n, hint) < 0) {
      const D3 t = p1;
      p1 = p2;
      p2 = t;
      n = n * -1.0;
    }
    n = unit(n);
    f64 u0, v0, u1, v1, u2, v2;
    uv_of(p0, uv_normal, u0, v0);
    uv_of(p1, uv_normal, u1, v1);
    uv_of(p2, uv_normal, u2, v2);
    const u32 i0 = b.vertex(p0, n, u0, v0), i1 = b.vertex(p1, n, u1, v1),
              i2 = b.vertex(p2, n, u2, v2);
    b.tri(i0, i1, i2);
  };
  // The split faces' face proper, corner by corner (each drawn in from the two edges that meet
  // there), and their corners set back behind the face's plane by what the insets ask.
  D3 c[8];
  for (u32 k = 0; k < 8; ++k)
    c[k] = c0[k];
  D3 split_n[6];
  D3 ring_of[6][8];
  u32 count_of[6] = {0, 0, 0, 0, 0, 0};
  u32 first_of[6][4] = {}, last_of[6][4] = {};
  for (u32 f = 0; f < 6; ++f) {
    if ((faces.split & face_bit(f)) == 0) continue;
    const D3 n = outward(c0, f);
    split_n[f] = n;
    const Inset& in = k_inset[std::min<u32>(faces.edge, 2)];
    for (u32 k = 0; k < 4; ++k) {
      const D3 p = c0[rings[f][k]], ep = c0[rings[f][(k + 3) % 4]] - p,
               en = c0[rings[f][(k + 1) % 4]] - p;
      const f64 lp = std::max(std::sqrt(dot(ep, ep)), 1e-6),
                ln = std::max(std::sqrt(dot(en, en)), 1e-6);
      // In from the previous edge by a share of the next edge's length, and from the next edge
      // by a share of the previous one's: each corner its own, so the face proper is a polygon
      // of its own.
      const f64 share_a = r.range(in.lo, in.hi);
      const f64 share_b = r.range(in.lo, in.hi);
      const f64 hi_a = std::min(in.max_m, 0.3 * ln), hi_b = std::min(in.max_m, 0.3 * lp);
      const f64 ia = std::clamp(share_a * ln, std::min(in.min_m, hi_a), hi_a);
      const f64 ib = std::clamp(share_b * lp, std::min(in.min_m, hi_b), hi_b);
      const f64 lift = r.range(-k_ring_relief, k_ring_relief);
      // A walling stone's corner is broken off (`k_broken`, 45 %): two points, a cut 25
      // to 50 % of the shorter edge back along each edge, so the face proper is a pentagon or
      // more and its corner a dark facet where four stones meet.
      const f64 broken = r.next();
      const f64 cut_a = r.range(0.25, 0.50), cut_b = r.range(0.25, 0.50);
      first_of[f][k] = count_of[f];
      f64 deepest = std::max(ia, ib);
      if (faces.edge == k_edge_stone && broken < k_broken) {
        const f64 shorter = std::min(lp, ln);
        const f64 ca = std::max(std::min(cut_a * shorter, 0.3 * lp), ib);
        const f64 cb = std::max(std::min(cut_b * shorter, 0.3 * ln), ia);
        ring_of[f][count_of[f]++] = p + ep * (ca / lp) + en * (ia / ln) + n * lift;
        ring_of[f][count_of[f]++] = p + en * (cb / ln) + ep * (ib / lp) + n * lift;
        deepest = std::max(ca, cb);
      } else {
        ring_of[f][count_of[f]++] = p + en * (ia / ln) + ep * (ib / lp) + n * lift;
      }
      last_of[f][k] = count_of[f] - 1;
      const f64 wear = r.range(0.003, 0.008);
      const f64 back = std::clamp(0.6 * deepest + wear, 0.005, k_back_max);
      c[rings[f][k]] = c[rings[f][k]] - n * back;
    }
  }
  for (u32 f = 0; f < 6; ++f) {
    if ((faces.hidden & face_bit(f)) != 0) continue;
    if ((faces.split & face_bit(f)) != 0) {
      const D3 n = split_n[f];
      const D3 qm[4] = {c[rings[f][0]], c[rings[f][1]], c[rings[f][2]], c[rings[f][3]]};
      const D3(&ring)[8] = ring_of[f];
      const u32 count = count_of[f];
      if ((faces.crown & face_bit(f)) != 0) {
        // The crown: an interior point, off the middle by the ring's own weights, raised off
        // the ring; a triangle a ring point sharing their vertices, each vertex's normal the
        // mean of its triangles', so the face proper reads rounded and its arris band crisp.
        D3 apex{};
        f64 weight = 0;
        for (u32 k = 0; k < count; ++k) {
          const f64 w = r.range(0.5, 1.5);
          apex = apex + ring[k] * w;
          weight += w;
        }
        const f64 crown = r.range(k_crown_lo, k_crown_hi);
        apex = apex * (1.0 / weight) + n * crown;
        D3 tn[8];
        D3 all{};
        for (u32 k = 0; k < count; ++k) {
          D3 t = cross(ring[k] - apex, ring[(k + 1) % count] - apex);
          if (dot(t, n) < 0) t = t * -1.0;
          tn[k] = unit(t);
          all = all + tn[k];
        }
        f64 u, v;
        uv_of(apex, n, u, v);
        const u32 i_apex = b.vertex(apex, unit(all), u, v);
        u32 iv[8];
        for (u32 k = 0; k < count; ++k) {
          uv_of(ring[k], n, u, v);
          iv[k] = b.vertex(ring[k], unit(tn[(k + count - 1) % count] + tn[k]), u, v);
        }
        for (u32 k = 0; k < count; ++k) {
          const u32 k1 = (k + 1) % count;
          if (dot(cross(ring[k] - apex, ring[k1] - apex), n) > 0)
            b.tri(i_apex, iv[k], iv[k1]);
          else
            b.tri(i_apex, iv[k1], iv[k]);
        }
      } else {
        for (u32 k = 1; k + 1 < count; ++k)
          facet(ring[0], ring[k], ring[k + 1], n, n);
      }
      // The worn arrises: a broken corner's facet down to the stone's set-back corner, and along
      // each edge from the face proper out to the stone's two set-back corners.
      for (u32 k = 0; k < 4; ++k) {
        const u32 k1 = (k + 1) % 4;
        const D3& a = ring[first_of[f][k]];
        const D3& z = ring[last_of[f][k]];
        if (first_of[f][k] != last_of[f][k]) facet(qm[k], a, z, n, n);
        facet(qm[k], qm[k1], ring[first_of[f][k1]], n, n);
        facet(qm[k], ring[first_of[f][k1]], z, n, n);
      }
      continue;
    }
    const D3 q[4] = {c[rings[f][0]], c[rings[f][1]], c[rings[f][2]], c[rings[f][3]]};
    const D3 n = outward(c, f);
    if ((faces.pillow & face_bit(f)) != 0) {
      // A bulging side: a point off its middle 3 to 8 mm proud of its corners, four triangles
      // sharing smooth normals (a cope stone's sides: a split stone's, not a board's).
      D3 apex{};
      f64 weight = 0;
      for (u32 k = 0; k < 4; ++k) {
        const f64 w = r.range(0.5, 1.5);
        apex = apex + q[k] * w;
        weight += w;
      }
      const f64 bulge = r.range(0.003, 0.008);
      apex = apex * (1.0 / weight) + n * bulge;
      D3 tn[4];
      D3 all{};
      for (u32 k = 0; k < 4; ++k) {
        D3 t = cross(q[k] - apex, q[(k + 1) % 4] - apex);
        if (dot(t, n) < 0) t = t * -1.0;
        tn[k] = unit(t);
        all = all + tn[k];
      }
      f64 u, v;
      uv_of(apex, n, u, v);
      const u32 i_apex = b.vertex(apex, unit(all), u, v);
      u32 iv[4];
      for (u32 k = 0; k < 4; ++k) {
        uv_of(q[k], n, u, v);
        iv[k] = b.vertex(q[k], unit(tn[(k + 3) % 4] + tn[k]), u, v);
      }
      for (u32 k = 0; k < 4; ++k) {
        const u32 k1 = (k + 1) % 4;
        if (dot(cross(q[k] - apex, q[k1] - apex), n) > 0)
          b.tri(i_apex, iv[k], iv[k1]);
        else
          b.tri(i_apex, iv[k1], iv[k]);
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
constexpr f64 k_line = 0.002;     // a face stone's plane off the batter's line, either way

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
      if ((id[i] >= 0 || id[i + 1] >= 0) && id[i] != id[i + 1]) {
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

  // The triangles since the last stone are that stone's (every stone's geometry follows its
  // `add_stone`); `account` closes the tally at a change of role.
  StoneRole tally_role = StoneRole::face;
  u32 tally_mark = 0;
  void account(StoneRole next) {
    report.role_triangles[static_cast<u32>(tally_role)] += b.triangles - tally_mark;
    tally_mark = b.triangles;
    tally_role = next;
  }

  i32 add_stone(const Stone& st) {
    account(st.role);
    report.stones.push_back(st);
    if (st.role != StoneRole::pinning && st.role != StoneRole::rubble && st.role != StoneRole::core)
      report.stone_volume_m3 += static_cast<f64>(st.x1 - st.x0) *
                                static_cast<f64>(st.top - st.bed) *
                                std::abs(static_cast<f64>(st.z1 - st.z0));
    return static_cast<i32>(report.stones.size() - 1);
  }

  // A block between two levels: its corners jittered within its beds (flat to 4 mm) and its face
  // (proud or sunk by up to `jitter_face`). `face` is the side it shows (+1 the +z face, -1 the
  // -z face, 0 both: a stone across the wall); a face stone's bed and tail are not drawn, its
  // shown face is split when `rough`, and `split` names more faces to split (a head's end).
  void block(f64 x0, f64 x1, f64 bed, f64 top, f64 za, f64 zb, f64 za_top, f64 zb_top, i32 face,
             f64 jitter_face, u64 seed, u8 split = 0, bool rough = true, u8 edge = k_edge_stone,
             bool crown = true, u8 hide = 0, f64 taper = 0) {
    Rng r{seed};
    const f64 j = 0.004;
    D3 c[8] = {{x0, bed, za},     {x1, bed, za},     {x1, bed, zb},     {x0, bed, zb},
               {x0, top, za_top}, {x1, top, za_top}, {x1, top, zb_top}, {x0, top, zb_top}};
    for (D3& p : c) {
      p.x += r.range(-0.002, 0.002);  // a joint 0 to 8 mm wide with the 2 mm gap either side
      p.y += r.range(-j, j);
    }
    if (taper > 0) {
      // A through-stone's ends are a long stone's, not a box's: each end drawn in along the wall
      // by up to `taper` of its length at each corner and its top edge worn down by up to 6 mm,
      // so its end is a quadrilateral of its own (its ends' faces are split besides).
      const f64 len = x1 - x0;
      for (u32 k = 0; k < 8; ++k) {
        const f64 in = r.range(0.0, taper) * len;
        c[k].x += (k == 0 || k == 3 || k == 4 || k == 7) ? in : -in;
        if (k >= 4) c[k].y -= r.range(0.0, 0.006);
      }
    }
    if (face != 0) {
      // The face is the ring's z-high side for +z and z-low for -z: jitter it proud or sunk.
      const u32 a = face > 0 ? 2 : 0, e = face > 0 ? 3 : 1;
      for (u32 k : {a, e, a + 4, e + 4})
        c[k].z += static_cast<f64>(face) * r.range(-jitter_face, jitter_face);
    }
    Faces faces;
    if (face != 0) {
      const u32 shown = face > 0 ? 4 : 2, tail = face > 0 ? 2 : 4;
      faces.split = rough ? face_bit(shown) : 0;
      faces.hidden = static_cast<u8>(face_bit(0) | face_bit(tail));
    } else if (rough) {
      faces.split = static_cast<u8>(face_bit(2) | face_bit(4));
    }
    faces.split = static_cast<u8>(faces.split | split);
    faces.hidden = static_cast<u8>((faces.hidden | hide) & ~split);
    faces.edge = edge;
    faces.crown = crown ? u8{0x3f} : u8{0};
    // Every draw on its own statement: the order of a call's arguments is the compiler's.
    const u32 swatch = static_cast<u32>(r.bits() % k_stone_swatches);
    body(b, c, swatch, faces, r);
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
    // A head's runners reach a centimetre past the wall's middle at their bed and their top, so
    // its end shows stone and not the hearting (G145c: reaching the middle only at the top left a
    // wedge 4 cm wide at the bed between the two faces' runners, the batter's difference).
    if (head && x0 <= -0.5 * s.L + 1e-6) tail = std::max(out_b, out_t) + 0.01;
    const f64 in_b = out_b - tail;
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
    // Built to the line (TCV, Placing the stones: "just brushing the line"), so the face's plane
    // is on the batter within 2 mm and only the split face's own relief stands off it. A head's
    // first stone shows its end as well.
    const u8 end = head && x0 <= -0.5 * s.L + 1e-6 ? face_bit(5) : u8{0};
    // A walling stone's face is crowned, its arrises worn back by the stone's share (12
    // triangles, two more a broken corner); a face under 0.012 m^2 is worn by the small share; a
    // closing stone is fanned flat (10), and one under 4 cm either way is a plain chip (2). In a
    // standing face the stone's top and ends lie against its neighbours and are not drawn: a joint
    // shows the worn arrises and the hearting's shadow behind them. Where the wall has fallen, and
    // on the footing (its top is the scarcement), they are.
    const bool closer = role == StoneRole::pinning;
    const u8 edge = closer || len * (top - bed) < 0.012 ? k_edge_small : k_edge_stone;
    const bool rough = !(closer && (len < 0.04 || top - bed < 0.04));
    // Near a head the ends are drawn too: the head's end shows its joints, and a look into one
    // meets stone.
    const bool open = ragged(xm) || role == StoneRole::footing || (head && x0 < -0.5 * s.L + 0.6);
    const u8 hide =
        open ? (role == StoneRole::footing ? static_cast<u8>(face_bit(3) | face_bit(5)) : u8{0})
             : static_cast<u8>(face_bit(1) | face_bit(3) | face_bit(5));
    const u64 seed = rng.bits();
    // Drawn once the face is whole (`draw_pending`), from the stone as it stands after the seams
    // are crossed, with a joint's 2 mm either side.
    pending.push_back({static_cast<u32>(id), seed, end, edge, hide, rough, !closer});
    sky[side > 0 ? 0 : 1].set(x0, x1, top, id);
  }

  // ---- seams crossed (walls.md, "The module") ----
  // A face stone is laid in its sweep but drawn once the face is whole, so a seam the sweeps left
  // (a joint through three courses) is crossed by moving one joint of it along its course before
  // any stone is drawn: of the two stones meeting there, the one whose band lies within its
  // neighbour's is lengthened 5 cm past the seam and its neighbour shortened, and what that
  // leaves open above or below the lengthened stone is closed with the face (`close_faces`).
  struct Pending {
    u32 stone = 0;
    u64 seed = 0;
    u8 end = 0, edge = 0, hide = 0;
    bool rough = true, crown = true;
  };
  Vector<Pending> pending;

  static bool in_face(const Stone& t) {
    return t.role == StoneRole::face || t.role == StoneRole::footing ||
           t.role == StoneRole::through || t.role == StoneRole::tie;
  }
  // The lowest face, through or tie stone of a face covering x whose bed is at or above `level`
  // (the measure module_tests.cpp holds the solver to).
  i32 on_top(i8 side, f64 x, f64 level) const {
    i32 best = -1;
    for (u32 i = 0; i < report.stones.size(); ++i) {
      const Stone& t = report.stones[i];
      if (!in_face(t) || (t.side != 0 && t.side != side)) continue;
      if (static_cast<f64>(t.bed) < level - 0.006) continue;
      if (x < static_cast<f64>(t.x0) || x > static_cast<f64>(t.x1)) continue;
      if (best < 0 || t.bed < report.stones[static_cast<u32>(best)].bed) best = static_cast<i32>(i);
    }
    return best;
  }
  // How many courses above stone `i` the joint at its end runs on, and the stones either side of
  // it at each.
  u32 seam(u32 i, i32 (&ls)[3], i32 (&rs)[3]) const {
    const Stone& s0 = report.stones[i];
    const f64 x = static_cast<f64>(s0.x1);
    f64 level = static_cast<f64>(s0.top);
    u32 levels = 0;
    for (u32 k = 0; k < 3; ++k) {
      const i32 l = on_top(s0.side, x - 0.04, level), r = on_top(s0.side, x + 0.04, level);
      if (l < 0 || r < 0 || l == r) break;
      const Stone& sl = report.stones[static_cast<u32>(l)];
      const Stone& sr = report.stones[static_cast<u32>(r)];
      if (std::abs(static_cast<f64>(sl.x1) - x) >= 0.03 &&
          std::abs(static_cast<f64>(sr.x0) - x) >= 0.03)
        break;
      ls[levels] = l;
      rs[levels] = r;
      ++levels;
      level = std::min(static_cast<f64>(sl.top), static_cast<f64>(sr.top));
    }
    return levels;
  }
  static bool seam_end(const Stone& t, i8 side) {
    return (t.role == StoneRole::face || t.role == StoneRole::footing) && t.side == side &&
           t.x1 <= 1.4f && t.x1 >= -1.4f;
  }
  u32 seams_of(i8 side) const {
    u32 n = 0;
    i32 ls[3] = {-1, -1, -1}, rs[3] = {-1, -1, -1};
    for (u32 i = 0; i < report.stones.size(); ++i)
      if (seam_end(report.stones[i], side)) n += seam(i, ls, rs) >= 2 ? 1u : 0u;
    return n;
  }
  // Moves the joint between stones li and ri by d along the wall; false when it may not.
  bool shift(u32 li, u32 ri, f64 d) {
    Stone& l = report.stones[li];
    Stone& r = report.stones[ri];
    auto movable = [](const Stone& t) {
      return t.role == StoneRole::face || t.role == StoneRole::footing;
    };
    if (!movable(l) || !movable(r) || l.side != r.side) return false;
    if (std::abs(static_cast<f64>(l.x1) - static_cast<f64>(r.x0)) > 0.005) return false;
    const Stone& inner = d > 0 ? l : r;
    const Stone& outer = d > 0 ? r : l;
    if (inner.bed < outer.bed - 0.002f || inner.top > outer.top + 0.002f) return false;
    if (d > 0 && static_cast<f64>(r.x1) - (static_cast<f64>(r.x0) + d) < 0.08) return false;
    if (d < 0 && (static_cast<f64>(l.x1) + d) - static_cast<f64>(l.x0) < 0.08) return false;
    l.x1 = static_cast<f32>(static_cast<f64>(l.x1) + d);
    r.x0 = static_cast<f32>(static_cast<f64>(r.x0) + d);
    return true;
  }
  void cross_seams() {
    for (const i8 side : {i8{1}, i8{-1}}) {
      u32 now = seams_of(side);
      for (u32 i = 0; i < report.stones.size() && now > 0; ++i) {
        if (!seam_end(report.stones[i], side)) continue;
        i32 ls[3] = {-1, -1, -1}, rs[3] = {-1, -1, -1};
        if (seam(i, ls, rs) < 2) continue;
        bool done = false;
        for (const u32 k : {1u, 0u})
          for (const f64 d : {0.05, -0.05}) {
            if (done) continue;
            const u32 li = static_cast<u32>(ls[k]), ri = static_cast<u32>(rs[k]);
            const Stone keep_l = report.stones[li], keep_r = report.stones[ri];
            if (!shift(li, ri, d)) continue;
            const u32 after = seams_of(side);
            if (after < now) {
              report.seams_crossed += now - after;
              now = after;
              done = true;
            } else {
              report.stones[li] = keep_l;
              report.stones[ri] = keep_r;
            }
          }
      }
      report.seams_left += now;
    }
  }

  // Draws the face stones laid so far (and the closing stones), from their stones as they stand
  // after the seams were crossed: the face on the batter's line at the stone's middle, its tail
  // the depth it was laid with, a joint's 2 mm either side.
  void draw_pending() {
    for (const Pending& p : pending) {
      const Stone& st = report.stones[p.stone];
      account(st.role);
      const f64 x0 = static_cast<f64>(st.x0), x1 = static_cast<f64>(st.x1);
      const f64 bed = static_cast<f64>(st.bed), top = static_cast<f64>(st.top);
      const f64 xm = 0.5 * (x0 + x1);
      f64 out_b = 0.5 * s.width(std::max(bed - s.base(xm), 0.0)),
          out_t = 0.5 * s.width(std::max(top - s.base(xm), 0.0));
      if (st.role == StoneRole::footing) {
        out_b += k_scarcement_m;
        out_t += k_scarcement_m;
      }
      const f64 tail = std::abs(static_cast<f64>(st.z1) - static_cast<f64>(st.z0));
      const f64 in_b = out_b - tail, in_t = out_t - tail;
      const f64 g = 0.002;
      if (st.side > 0)
        block(x0 + g, x1 - g, bed, top, in_b, out_b, in_t, out_t, 1, k_line, p.seed, p.end, p.rough,
              p.edge, p.crown, p.hide);
      else
        block(x0 + g, x1 - g, bed, top, -out_b, -in_b, -out_t, -in_t, -1, k_line, p.seed, p.end,
              p.rough, p.edge, p.crown, p.hide);
    }
    pending.clear();
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
        // Its face on the batter's line at its foot and at its top, set back 4 mm; at a head's
        // end it reaches the wall's middle, as the runners do, so the end shows no hearting.
        const bool at_end = head && a <= -0.5 * s.L + 0.02;
        // A narrow hollow deeper than twice its width and 8 mm is packed with pinnings one on
        // another, each no taller than that, not with one sliver (G145c: a closing stone is a
        // stone; face_tests.cpp holds twice and a centimetre).
        const f64 w = z - a, depth = bed - low;
        const u32 pieces = w < 0.0505 && depth > 2.0 * w + 0.008
                               ? static_cast<u32>(std::ceil(depth / (2.0 * w + 0.008) - 1e-9))
                               : 1u;
        // In a standing face a pinning's top and ends lie against stone: only its face is drawn.
        const bool open = ragged(xm) || (head && a < -0.5 * s.L + 0.6);
        const u8 hide = open ? u8{0} : static_cast<u8>(face_bit(1) | face_bit(3) | face_bit(5));
        for (u32 p = 0; p < pieces; ++p) {
          const f64 y0 = low + depth * static_cast<f64>(p) / static_cast<f64>(pieces);
          const f64 y1 = low + depth * static_cast<f64>(p + 1) / static_cast<f64>(pieces);
          const f64 out_t = 0.5 * s.width(std::max(y1 - s.base(xm), 0.0)) - 0.004;
          const f64 out_b = 0.5 * s.width(std::max(y0 - s.base(xm), 0.0)) - 0.004;
          const f64 deep = at_end ? std::max(out_t, out_b) + 0.01 : 0.08;
          const f64 out = out_t;
          Stone st;
          st.x0 = static_cast<f32>(a);
          st.x1 = static_cast<f32>(z);
          st.bed = static_cast<f32>(y0);
          st.top = static_cast<f32>(y1);
          st.z0 = static_cast<f32>(sg * (out - deep));
          st.z1 = static_cast<f32>(sg * out);
          st.side = static_cast<i8>(side);
          st.sweep = sweep;
          st.role = StoneRole::pinning;
          add_stone(st);
          // A pinning is a chip of stone, its face plain (two triangles; it is a few centimetres).
          const u8 end = at_end ? face_bit(5) : u8{0};
          if (side > 0)
            block(a + 0.003, z - 0.003, y0 + 0.002, y1 - 0.002, out_b - deep, out_b, out_t - deep,
                  out_t, 1, k_line, rng.bits(), end, false, k_edge_small, false, hide);
          else
            block(a + 0.003, z - 0.003, y0 + 0.002, y1 - 0.002, -out_b, -out_b + deep, -out_t,
                  -out_t + deep, -1, k_line, rng.bits(), end, false, k_edge_small, false, hide);
        }
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
    const u8 end = role == StoneRole::tie ? face_bit(5) : u8{0};  // a tie shows the head's end
    const u64 seed = rng.bits();
    block(x0 + 0.002, x1 - 0.002, bed, top, -wb, wb, -wt, wt, 0, 0.0, seed, end, true, k_edge_stone,
          true, 0, role == StoneRole::through ? 0.06 : 0.0);
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
        const f64 thick = rng.range(0.10, 0.14);
        const f64 proud = rng.range(0.03, 0.10);
        across(t0, t1, bed, bed + thick, proud, StoneRole::through, sweep);
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
              // The stone starts past the joint; what it leaves short of it is closed when the
              // faces are (close_faces).
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
    cross_seams();
    close_faces();
    draw_pending();
  }

  // What the sweeps leave open in a standing face — a slot where a course resumed past a joint, a
  // hollow no stone above bridged — is closed with small stones, as a waller packs the last gaps
  // (walls.md, "The face"): over each face a centimetre grid in the module's elevation, from the
  // ground to the cover's bed, every cell covered by a stone of that face marked, and each open
  // run of two or more cells, carried up while the cells above it are open too (to 12 cm), made a
  // pinning set to the line.
  void close_faces() {
    constexpr f64 k_cell = 0.01;
    const u32 nx = static_cast<u32>(std::floor(s.L / k_cell + 0.5));
    const u32 ny = static_cast<u32>(std::floor(s.body / k_cell + 0.5));
    Vector<u8> open;
    for (i32 side : {1, -1}) {
      open.clear();
      open.resize(nx * ny, 1);
      const f64 x_lo = -0.5 * s.L;
      for (const Stone& st : report.stones) {
        if (st.side != 0 && st.side != side) continue;
        if (st.role == StoneRole::cover || st.role == StoneRole::cope ||
            st.role == StoneRole::rubble)
          continue;
        const f64 x0 = static_cast<f64>(st.x0), x1 = static_cast<f64>(st.x1);
        const f64 xm = 0.5 * (x0 + x1);
        const f64 h0 = static_cast<f64>(st.bed) - s.base(xm),
                  h1 = static_cast<f64>(st.top) - s.base(xm);
        for (u32 i = 0; i < nx; ++i) {
          const f64 xc = x_lo + (static_cast<f64>(i) + 0.5) * k_cell;
          if (xc < x0 || xc > x1) continue;
          // Heights over the ground's line at this column: a level stone on a slope.
          const f64 dh = s.base(xm) - s.base(xc);
          for (u32 j = 0; j < ny; ++j) {
            const f64 hc = (static_cast<f64>(j) + 0.5) * k_cell;
            if (hc >= h0 + dh && hc <= h1 + dh) open[j * nx + i] = 0;
          }
        }
      }
      for (u32 j = 0; j < ny; ++j) {
        const f64 hc = (static_cast<f64>(j) + 0.5) * k_cell;
        for (u32 i = 0; i < nx;) {
          if (!open[j * nx + i] || ragged(x_lo + (static_cast<f64>(i) + 0.5) * k_cell) ||
              hc > limit(x_lo + (static_cast<f64>(i) + 0.5) * k_cell) - 0.015) {
            ++i;
            continue;
          }
          u32 i1 = i;
          while (i1 < nx && open[j * nx + i1] && i1 - i < 30)
            ++i1;
          if (i1 - i < 2) {
            i = i1;
            continue;
          }
          // Carried up no further than twice its width (to 12 cm): a closing stone, not a sliver
          // (face_tests.cpp); what is left above is the next one's.
          const u32 tall = std::min(12u, 2u * (i1 - i));
          u32 j1 = j + 1;
          while (j1 < ny && j1 - j < tall) {
            bool all = true;
            for (u32 k = i; k < i1 && all; ++k)
              all = open[j1 * nx + k] != 0;
            if (!all) break;
            ++j1;
          }
          for (u32 b2 = j; b2 < j1; ++b2)
            for (u32 a2 = i; a2 < i1; ++a2)
              open[b2 * nx + a2] = 0;
          const f64 a = x_lo + static_cast<f64>(i) * k_cell,
                    z = x_lo + static_cast<f64>(i1) * k_cell;
          const f64 xm = 0.5 * (a + z);
          const f64 bed = s.base(xm) + static_cast<f64>(j) * k_cell;
          const f64 top = s.base(xm) + static_cast<f64>(j1) * k_cell;
          face_stone(side, a, z, bed, top, StoneRole::pinning, 0);
          i = i1;
        }
      }
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
      // Its two edges along the faces split, as a face stone's is, fanned flat; its bed on the
      // wall and its ends against its neighbours not drawn, but at a head's end and where a
      // broken stretch's cover stops.
      const u32 swatch = static_cast<u32>(rng.bits() % k_stone_swatches);
      u8 hide = static_cast<u8>(face_bit(0) | face_bit(3) | face_bit(5));
      if (head && x <= x0 + 1e-9) hide = static_cast<u8>(hide & ~face_bit(5));
      if (m.kind == ModuleKind::broken && e >= x1 - 1e-9)
        hide = static_cast<u8>(hide & ~face_bit(3));
      body(b, c, swatch, Faces{static_cast<u8>(face_bit(2) | face_bit(4)), hide, k_edge_stone, 0},
           rng);
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
    // The cope as random upright coping (TCV, Types of coping): stones of differing thickness,
    // mostly thin slabs and now and then a block; their heights the specification's on average,
    // taller and shorter alternating more often than not, neighbours never more than 50 mm apart
    // (Upright: "rarely ... more than 2 inches"); each set near vertical, leaning as its neighbour
    // does, within 4 degrees; packed close (TCV, Coping: 2 to 3 mm wedges), 2 to 8 mm apart.
    bool first = true;
    bool taller = rng.next() < 0.5;
    f64 prev_h = s.cope, prev_lean = 0;
    for (f64 x = x0; x < x1 - 0.04;) {
      const bool big = head && first;
      const f64 thin = rng.next();
      const f64 t = big ? 0.30 : k_cope_thin_m + (k_cope_thick_m - k_cope_thin_m) * thin * thin;
      const f64 e = std::min(x + t, x1);
      const f64 hw = big ? 0.5 * s.top_w + 0.02 : 0.5 * s.top_w * rng.range(0.78, 1.05);
      const f64 zc = rng.range(-0.015, 0.015);
      const f64 b0 = cover_top(x), b1 = cover_top(e);
      const f64 turn = rng.next();
      if (turn < 0.7) taller = !taller;
      const f64 want = s.cope * (taller ? rng.range(1.00, 1.11) : rng.range(0.92, 1.01));
      const f64 hc = big ? s.cope * 1.1 : std::clamp(want, prev_h - 0.05, prev_h + 0.05);
      prev_h = hc;
      // Its top split unevenly: one end up to 7 % below the other.
      const f64 split0 = rng.range(0.93, 1.0);
      const f64 split1 = rng.range(0.93, 1.0);
      const f64 t0 = b0 + hc * split0;
      const f64 t1 = b1 + hc * split1;
      const f64 own = rng.range(-2.5, 2.5);
      // Now and then (one in fifteen) a stone leans on the next, 5 to 8 degrees, its foot a few
      // centimetres off it, the next set past its top (G145c, a design value: no source read
      // gives a share); else vertical within 4 degrees, as its neighbour leans.
      const f64 tilt = rng.next();
      const bool leaning = !big && !first && tilt < 1.0 / 15.0;
      const f64 steep = rng.range(5.0, 8.0);
      const f64 lean_deg = leaning ? steep : std::clamp(0.5 * prev_lean + own, -4.0, 4.0);
      prev_lean = leaning ? 0.0 : lean_deg;
      const f64 lean = hc * (lean_deg * 3.141592653589793 / 180.0);  // the top's run, small angle
      // Three in ten are wedges, narrower at the top than at the bed by up to a fifth of their
      // thickness, the narrowing on either side (a split stone is seldom square; G145c's first
      // pictures, at a half, opened gaps the sky showed through).
      const f64 shape = rng.next();
      const f64 wa = shape < 0.3 ? rng.range(0.0, 0.10) * (e - x) : 0.0;
      const f64 wb = shape < 0.3 ? rng.range(0.0, 0.10) * (e - x) : 0.0;
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
      const f64 ta = xa + lean + wa, tb = xb + lean - wb;
      D3 c[8] = {{xa, b0, zc - hw}, {xb, b1, zc - hw}, {xb, b1, zc + hw}, {xa, b0, zc + hw},
                 {ta, t0, zc - hw}, {tb, t1, zc - hw}, {tb, t1, zc + hw}, {ta, t0, zc + hw}};
      for (u32 k = 4; k < 8; ++k)
        c[k].z += rng.range(-0.015, 0.015);
      // Its top and its two sides (what the field sees) bulging, so it is a split stone and not
      // a board; its ends drawn plain, since the gaps between cope stones, a wedge's and a
      // leaner's above all, show them (G145c: hidden, they let the eye into the stone); its bed,
      // on the cover, never seen. 16 triangles.
      const u32 swatch = static_cast<u32>(rng.bits() % k_stone_swatches);
      body(b, c, swatch,
           Faces{0, face_bit(0), k_edge_fine, 0,
                 static_cast<u8>(face_bit(1) | face_bit(2) | face_bit(4))},
           rng);
      const f64 gap = rng.range(0.002, 0.008);
      // Past a leaner's top, so it rests on the next stone and does not pass into it.
      x = e + gap + (leaning ? std::max(lean - wb, 0.0) : 0.0);
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
      const f64 d0 = rng.next();
      const f64 d = 0.02 + 1.4 * d0 * rng.next();
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
      // Its top split; its underside, on the ground, never seen.
      const u32 swatch = static_cast<u32>(rng.bits() % k_stone_swatches);
      body(b, c, swatch, Faces{face_bit(1), face_bit(0), k_edge_small, face_bit(1)}, rng);
    }
  }

  // The hearting: a dark closed core between the faces' tails, so a joint shows shadow.
  void lay_core() {
    // At a head the core stands 3 cm behind the end, behind its stones' knocked-back arrises.
    const f64 xa = head ? -0.5 * s.L + 0.03 : -0.5 * s.L - 0.06, xb = 0.5 * s.L + 0.06;
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
// `h1_end` is the top's height at x1 when the top slants (the mid form's cope), else h1.
void top_box(Builder& b, const Section& s, f64 x0, f64 x1, f64 h0, f64 h1, f64 hw, u32 swatch,
             f64 h1_end = -1.0) {
  const f64 h1b = h1_end >= 0 ? h1_end : h1;
  const D3 c[8] = {{x0, s.base(x0) + h0, -hw}, {x1, s.base(x1) + h0, -hw},
                   {x1, s.base(x1) + h0, hw},  {x0, s.base(x0) + h0, hw},
                   {x0, s.base(x0) + h1, -hw}, {x1, s.base(x1) + h1b, -hw},
                   {x1, s.base(x1) + h1b, hw}, {x0, s.base(x0) + h1, hw}};
  Rng plain{0};  // a plain body draws nothing
  const u32 first = static_cast<u32>(b.mesh.positions.size());
  body(b, c, swatch, Faces{}, plain);
  if (h1_end >= 0) {
    // The mid form's cope: its faces striped along the wall by one row of the stones' swatches,
    // a swatch every 8 cm (a cope stone's thickness), so the band keeps the near cope's upright
    // stones of differing greys where it hands over (G145c; walls.md, "Forms and the LOD policy").
    for (u32 i = first; i < b.mesh.positions.size(); ++i) {
      const f64 x = static_cast<f64>(b.mesh.positions[i].x);
      b.mesh.uvs[i] = {static_cast<f32>(x * (static_cast<f64>(k_swatch) / k_atlas) / 0.08),
                       static_cast<f32>((7.0 * k_swatch + 4.0) / k_atlas)};
    }
  }
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
    // A course's middle proud and its joint sunk 16 mm (G145b: 8 mm), so the bands' shading
    // carries some of the near form's joint shadow at the hand-over.
    levels.push_back(h + 0.5 * t);
    proud.push_back(0.012);
    levels.push_back(h + t);
    proud.push_back(-0.016);
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
      Rng plain{0};
      body(b, c, k_swatch_mid, Faces{}, plain);
    }
  }
  if (m.kind == ModuleKind::fallen) return;
  // The cover as one slab and the cope as blocks of three stones each, their tops the highest.
  const f64 ca = head ? -0.5 * s.L : -0.5 * s.L - 0.02;
  const f64 cb = m.kind == ModuleKind::broken ? broken_from - 0.05 : 0.5 * s.L + 0.02;
  top_box(b, s, ca, cb, s.body, s.body + 0.075, 0.5 * s.top_w + 0.03, k_swatch_mid);
  // The cope as blocks of three stones, each block's top slanting from its first stone's top to
  // its last's and the block a centimetre short of the next, so the mid form keeps the cope's
  // broken skyline where the near form hands it over (G145c; walls.md, "Forms and the LOD
  // policy") rather than a flat band.
  u32 in_group = 0;
  f64 gx0 = 0, gtop0 = 0, gtop1 = 0, gx1 = 0;
  for (const Stone& st : report.stones) {
    if (st.role != StoneRole::cope) continue;
    const f64 xm = 0.5 * static_cast<f64>(st.x0 + st.x1);
    const f64 above = static_cast<f64>(st.top) - s.base(xm) - s.body;
    if (in_group == 0) {
      gx0 = static_cast<f64>(st.x0);
      gtop0 = above;
    }
    gtop1 = above;
    gx1 = static_cast<f64>(st.x1);
    if (++in_group == 3) {
      top_box(b, s, gx0, std::max(gx1 - 0.01, gx0 + 0.02), s.body + 0.07, s.body + gtop0,
              0.5 * s.top_w, k_swatch_mid, s.body + gtop1);
      in_group = 0;
    }
  }
  if (in_group > 0)
    top_box(b, s, gx0, gx1, s.body + 0.07, s.body + gtop0, 0.5 * s.top_w, k_swatch_mid,
            s.body + gtop1);
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
    solver.account(StoneRole::core);
    solver.lay_core();
    solver.account(StoneRole::core);
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
