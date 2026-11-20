// The shell (docs/subsystems/structures.md, "The mesher"): the walls on their centre lines, joined
// at their ends, each side a face cut by its openings and by the roof it rises to; the openings'
// reveals and frames; the roofs; the chimneys. Nothing here draws two faces in one plane over one
// another: a wall's faces end at its joins, a member that meets a face is sunk through it.

#include "emit.h"

#include <algorithm>

namespace engine::structures::detail {

using structure::Description;
using structure::Surface;

namespace {

struct IV {
  i64 x = 0, z = 0;
};
i64 dot(IV a, IV b) noexcept { return a.x * b.x + a.z * b.z; }
i64 cross(IV a, IV b) noexcept { return a.x * b.z - a.z * b.x; }
// The left of `u` seen from above, y up.
IV left_of(IV u) noexcept { return {u.z, -u.x}; }

IV from_of(const structure::Wall& w) noexcept { return {w.from[0], w.from[1]}; }
IV to_of(const structure::Wall& w) noexcept { return {w.to[0], w.to[1]}; }

// Wall `j`'s direction away from `p` when it ends there.
bool leaves(const structure::Wall& w, IV p, IV& out) noexcept {
  const IV a = from_of(w), b = to_of(w);
  if (a.x == p.x && a.z == p.z) {
    out = {b.x - a.x, b.z - a.z};
    return true;
  }
  if (b.x == p.x && b.z == p.z) {
    out = {a.x - b.x, a.z - b.z};
    return true;
  }
  return false;
}

// Whether direction `a` comes before `b` turning leftward from `u` (neither along `u`).
bool turns_before(IV u, IV a, IV b) noexcept {
  const IV l = left_of(u);
  const auto far_half = [&](IV v) {
    const i64 s = dot(l, v), c = dot(u, v);
    return s < 0 || (s == 0 && c < 0);
  };
  const bool ha = far_half(a), hb = far_half(b);
  if (ha != hb) return !ha;
  return dot(left_of(a), b) > 0;
}

struct F2 {
  f64 x = 0, z = 0;
};
f64 crossf(F2 a, F2 b) noexcept { return a.x * b.z - a.z * b.x; }

// How far along its outgoing direction `u` wall `i`'s side line (`side` +1 the left of `u`, -1 the
// right) runs before it meets the line `b + t * v`, both relative to the join.
f64 meet(F2 u, f64 half, f64 side, F2 b, F2 v, f64 limit) noexcept {
  const F2 a{side * u.z * half, -side * u.x * half};
  const f64 den = crossf(u, v);
  if (den > -1e-9 && den < 1e-9) return 0.0;
  const f64 lambda = crossf(F2{b.x - a.x, b.z - a.z}, v) / den;
  return std::clamp(lambda, -limit, limit);
}

}  // namespace

void solve_walls(const Description& d, Vector<WallGeom>& out) {
  const u32 n = d.walls.size();
  out.clear();
  out.resize(n);
  for (u32 i = 0; i < n; ++i) {
    const structure::Wall& w = d.walls[i];
    WallGeom& g = out[i];
    const f64 dx = static_cast<f64>(w.to[0] - w.from[0]),
              dz = static_cast<f64>(w.to[1] - w.from[1]);
    g.length = std::sqrt(dx * dx + dz * dz);
    g.frame.ox = w.from[0];
    g.frame.oz = w.from[1];
    g.frame.dx = dx / g.length;
    g.frame.dz = dz / g.length;
    g.frame.nx = g.frame.dz;
    g.frame.nz = -g.frame.dx;
    g.half = static_cast<f64>(w.thickness) * 0.5;
    for (u32 m = 0; m < 3; ++m)
      for (u32 side = 0; side < 2; ++side)
        g.ends[m][side][0] = 0.0, g.ends[m][side][1] = g.length;
  }
  // A side's distance from its wall's centre line in mode `m` (emit.h, `WallGeom::ends`): an
  // outer face leaned in to its top thickness (1), or stood out by the plinth's projection (2).
  const auto side_half = [&](u32 wall, f64 wall_side, u32 m) {
    const structure::Wall& w = d.walls[wall];
    const f64 half = out[wall].half;
    const bool outside = w.kind == structure::WallKind::exterior && (wall_side > 0 ? w.a : w.b) < 0;
    if (!outside) return half;
    if (m == 1) return half - lean_of(d, w).total;
    if (m == 2 && w.storey == 0) return half + static_cast<f64>(d.plinth_proud);
    return half;
  };
  for (u32 i = 0; i < n; ++i) {
    const structure::Wall& w = d.walls[i];
    WallGeom& g = out[i];
    for (u32 e = 0; e < 2; ++e) {
      const IV p = e == 0 ? from_of(w) : to_of(w);
      const IV q = e == 0 ? to_of(w) : from_of(w);
      const IV u{q.x - p.x, q.z - p.z};
      const f64 sign = e == 0 ? 1.0 : -1.0;
      const F2 uu{g.frame.dx * sign, g.frame.dz * sign};
      // The walls that end here too: the one met first turning left and the one met last.
      i32 first = -1, last = -1;
      IV first_v, last_v;
      for (u32 j = 0; j < n; ++j) {
        if (j == i || d.walls[j].storey != w.storey) continue;
        IV v;
        if (!leaves(d.walls[j], p, v)) continue;
        if (first < 0 || turns_before(u, v, first_v)) first = static_cast<i32>(j), first_v = v;
        if (last < 0 || turns_before(u, last_v, v)) last = static_cast<i32>(j), last_v = v;
      }
      const auto from_here = [&](i32 j) {
        const IV a = from_of(d.walls[static_cast<u32>(j)]);
        return a.x == p.x && a.z == p.z;
      };
      g.neighbour[e] = first >= 0 && first == last ? first : -1;
      g.neighbour_from[e] = g.neighbour[e] >= 0 && from_here(first);
      for (u32 m = 0; m < 3; ++m) {
        f64 lambda[2] = {0, 0};  // the outgoing left side's, the outgoing right side's
        if (first >= 0) {
          // A corner or a star (structures.md, "Joins"): each side's line runs to the facing
          // side's line of the neighbour on that side, so the two walls' faces meet in one edge.
          // The outgoing left is the wall's left at its `from` end and its right at its `to`
          // end; the neighbour's facing side is its outgoing right for this wall's left.
          for (u32 k = 0; k < 2; ++k) {
            const i32 j = k == 0 ? first : last;
            const IV v = k == 0 ? first_v : last_v;
            const f64 side = k == 0 ? 1.0 : -1.0;
            const f64 vl = std::sqrt(static_cast<f64>(dot(v, v)));
            const F2 vv{static_cast<f64>(v.x) / vl, static_cast<f64>(v.z) / vl};
            const f64 hi = side_half(i, side * sign, m);
            const f64 hj = side_half(static_cast<u32>(j), -side * (from_here(j) ? 1.0 : -1.0), m);
            const F2 b{-side * vv.z * hj, side * vv.x * hj};
            lambda[k] = meet(uu, hi, side, b, vv, 3.0 * (g.half + out[static_cast<u32>(j)].half));
          }
          g.cap[e] = false;
        } else {
          // A wall that ends on another's span butts its near face.
          for (u32 j = 0; j < n; ++j) {
            if (j == i || d.walls[j].storey != w.storey) continue;
            const IV a = from_of(d.walls[j]), b = to_of(d.walls[j]);
            const IV dj{b.x - a.x, b.z - a.z}, ap{p.x - a.x, p.z - a.z};
            const i64 along = dot(ap, dj);
            if (cross(dj, ap) != 0 || along <= 0 || along >= dot(dj, dj)) continue;
            const WallGeom& h = out[j];
            const F2 vv{h.frame.dx, h.frame.dz};
            const f64 near_side = h.frame.nx * uu.x + h.frame.nz * uu.z > 0 ? 1.0 : -1.0;
            const F2 base{static_cast<f64>(a.x - p.x) + near_side * h.frame.nx * h.half,
                          static_cast<f64>(a.z - p.z) + near_side * h.frame.nz * h.half};
            for (u32 k = 0; k < 2; ++k)
              lambda[k] = meet(uu, g.half, k == 0 ? 1.0 : -1.0, base, vv, 3.0 * (g.half + h.half));
            g.cap[e] = false;
            break;
          }
        }
        if (e == 0) {
          g.ends[m][0][0] = lambda[0];
          g.ends[m][1][0] = lambda[1];
        } else {
          g.ends[m][1][1] = g.length - lambda[0];
          g.ends[m][0][1] = g.length - lambda[1];
        }
      }
    }
    g.sl[0] = g.ends[0][0][0], g.sl[1] = g.ends[0][0][1];
    g.sr[0] = g.ends[0][1][0], g.sr[1] = g.ends[0][1][1];
  }
}

Lean lean_of(const Description& d, const structure::Wall& w) noexcept {
  Lean l;
  const structure::Storey& storey = d.storeys[w.storey];
  l.y0 = storey.elevation;
  if (w.storey == 0 && d.plinth_top > storey.elevation) l.y0 = d.plinth_top;
  l.y1 = storey.elevation + storey.height;
  const bool one_outside = (w.a < 0) != (w.b < 0);
  if (w.kind == structure::WallKind::exterior && one_outside && w.top_thickness > 0 &&
      w.top_thickness < w.thickness && l.y1 > l.y0 + 1.0)
    l.total = static_cast<f64>(w.thickness - w.top_thickness);
  else
    l.y1 = l.y0 + 1.0;
  return l;
}

f64 roof_underside(const structure::Roof& roof, f64 x, f64 z) noexcept {
  const f64 c = roof.ridge_along_x ? z : x;
  const f64 lo = roof.ridge_along_x ? roof.rect[1] : roof.rect[0];
  const f64 hi = roof.ridge_along_x ? roof.rect[3] : roof.rect[2];
  f64 run = c - lo;
  if (roof.form == structure::RoofForm::gable) run = std::min(c - lo, hi - c);
  return static_cast<f64>(roof.eaves) + static_cast<f64>(roof.pitch) * run / 1000.0;
}

namespace {

const structure::Roof* roof_over(const Description& d, f64 x, f64 z) noexcept {
  for (const structure::Roof& r : d.roofs)
    if (x >= r.rect[0] - 1.0 && x <= r.rect[2] + 1.0 && z >= r.rect[1] - 1.0 &&
        z <= r.rect[3] + 1.0)
      return &r;
  return nullptr;
}

}  // namespace

f64 wall_top(const Description& d, u32 wall, f64 x, f64 z) noexcept {
  const structure::Wall& w = d.walls[wall];
  const structure::Storey& storey = d.storeys[w.storey];
  const f64 flat = static_cast<f64>(storey.elevation + storey.height);
  if (w.storey + 1 != d.storeys.size()) return flat;
  const structure::Roof* roof = roof_over(d, x, z);
  if (roof == nullptr) return flat;
  const f64 under = roof_underside(*roof, x, z);
  if (w.kind == structure::WallKind::inner && storey.ceiling) return std::min(flat, under);
  return under;
}

namespace {

// An opening as its wall's faces are cut by it: stations along the wall and heights above the
// origin, centimetres. A door's foot is its threshold's top.
struct Cut {
  f64 s0 = 0, s1 = 0, y0 = 0, y1 = 0;
};
Cut cut_of(const Description& d, const structure::Opening& o) noexcept {
  const f64 base = d.storeys[d.walls[o.wall].storey].elevation;
  Cut c;
  c.s0 = static_cast<f64>(o.at) - static_cast<f64>(o.width) * 0.5;
  c.s1 = static_cast<f64>(o.at) + static_cast<f64>(o.width) * 0.5;
  c.y0 = base + static_cast<f64>(o.sill);
  if (o.kind == structure::OpeningKind::door) c.y0 += static_cast<f64>(k_threshold_mm) * 0.1;
  c.y1 = base + static_cast<f64>(o.head);
  return c;
}

// A leaf's boards are about this wide, so a door of 75 to 80 cm has four (vernacular-shell.md:
// four planks common from the seventeenth century); a mullion's side (a choice: no source read
// gives a section).
constexpr f64 k_leaf_board_cm = 19.0;
constexpr f64 k_mullion_cm = 4.5;

// An opening's frame: its depth through the wall, its face, and how far back from the outer face
// it stands (an inner wall's sits in its middle).
struct Joinery {
  f64 depth = 0, width = 0, set = 0;
};
Joinery joinery_of(const Description& d, const structure::Wall& w,
                   const structure::Opening& o) noexcept {
  const bool door = o.kind == structure::OpeningKind::door;
  const bool exterior = w.kind == structure::WallKind::exterior;
  const f64 h = static_cast<f64>(w.thickness) * 0.5;
  Joinery j;
  j.depth = door ? 10.0 : 7.0;
  j.width = door ? 6.0 : 5.0;
  const f64 set = exterior ? (door ? 6.0 : 8.0) : h - 0.5 * j.depth;
  j.set = std::max(1.0, std::min(set, 2.0 * h - j.depth - 1.0));
  // A frame stands plumb behind a leaning face: its set is kept at the head, where the face has
  // leaned in farthest.
  j.set += lean_of(d, w).at(static_cast<f64>(d.storeys[w.storey].elevation + o.head));
  return j;
}
// Depth `t` from a wall's outer face inward is `q = out_sign * (half - t)`: it runs toward the
// wall's left where its right is the outside, and from its left face otherwise.
f64 out_sign_of(const structure::Wall& w) noexcept { return w.b < 0 && w.a >= 0 ? -1.0 : 1.0; }

void sort_unique(Vector<f64>& v) {
  std::sort(v.data(), v.data() + v.size());
  u32 kept = 0;
  for (u32 i = 0; i < v.size(); ++i)
    if (kept == 0 || v[i] - v[kept - 1] > k_eps) v[kept++] = v[i];
  v.resize(kept);
}

// A waved face is a grid about this square, and its wave dies away over this distance from an
// edge it must meet exactly (an opening, an end, its foot, its top). Choices: no source read
// measures a cob face's wander (vernacular-shell.md).
constexpr f64 k_wave_cell_cm = 45.0;
constexpr f64 k_wave_fade_cm = 30.0;
// A rounded corner is this many flat strips.
constexpr u32 k_arris_segments = 3;
// A slope is at most this many courses, and its profile that many steps and an eave and a ridge.
constexpr u32 k_roof_courses = 40;
constexpr u32 k_roof_profile = 2 * k_roof_courses + 12;

// Cuts every interval of the sorted `v` from index `from` on into parts no longer than `cell`.
void subdivide(Vector<f64>& v, u32 from, f64 cell) {
  const u32 n = v.size();
  for (u32 i = from; i + 1 < n; ++i) {
    const f64 a = v[i], b = v[i + 1];
    const u32 parts = static_cast<u32>((b - a) / cell + 0.999);
    for (u32 k = 1; k < parts; ++k)
      v.push_back(a + (b - a) * static_cast<f64>(k) / static_cast<f64>(parts));
  }
  std::sort(v.data(), v.data() + v.size());
}

// A value in [-1, 1) for a lattice point of one face, by integer mixing alone.
f64 lattice(u32 wall, u32 side, i64 x, i64 y) noexcept {
  u64 h = (static_cast<u64>(wall) * 2 + side + 1) * 0x9e3779b97f4a7c15ull;
  h ^= static_cast<u64>(x) * 0xbf58476d1ce4e5b9ull;
  h ^= h >> 31;
  h += static_cast<u64>(y) * 0x94d049bb133111ebull;
  h ^= h >> 29;
  h *= 0xd6e8feb86659fd93ull;
  h ^= h >> 32;
  return static_cast<f64>(h >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}
// Smooth value noise over a lattice `period` centimetres square, in [-1, 1].
f64 value_noise(u32 wall, u32 side, f64 s, f64 y, f64 period) noexcept {
  const f64 fs = s / period, fy = y / period;
  i64 is = static_cast<i64>(fs), iy = static_cast<i64>(fy);
  if (static_cast<f64>(is) > fs) --is;
  if (static_cast<f64>(iy) > fy) --iy;
  f64 ts = fs - static_cast<f64>(is), ty = fy - static_cast<f64>(iy);
  ts = ts * ts * (3.0 - 2.0 * ts);
  ty = ty * ty * (3.0 - 2.0 * ty);
  const f64 a = lattice(wall, side, is, iy), b = lattice(wall, side, is + 1, iy);
  const f64 c = lattice(wall, side, is, iy + 1), e = lattice(wall, side, is + 1, iy + 1);
  return (a + (b - a) * ts) * (1.0 - ty) + (c + (e - c) * ts) * ty;
}
// A face's wander at a point of it, in [-1, 1]: a swell about 90 cm across and a third as much
// at 37 (structures.md, "Faces that are not planes").
f64 wave_at(u32 wall, u32 side, f64 s, f64 y) noexcept {
  return 0.75 * value_noise(wall, side, s, y, 90.0) +
         0.25 * value_noise(wall, side + 2, s, y, 37.0);
}

// A rounded corner at end `e` of a wall's outside face (`Description.arris`): this face and the
// one round the corner each stop `back[m]` short of where they would meet (in the modes of
// `WallGeom::ends`) and an arc joins them. `a` and `b` are the unit directions from the corner
// along this face and along the other.
struct Round {
  bool on = false;
  f64 back[3] = {0, 0, 0};
  u32 other = 0;
  F2 a, b;
};
Round round_of(const Context& ctx, u32 wall, f64 side, u32 e) noexcept {
  Round r;
  const Description& d = *ctx.d;
  const structure::Wall& w = d.walls[wall];
  const WallGeom& g = ctx.walls[wall];
  const bool outside = w.kind == structure::WallKind::exterior && (side > 0 ? w.a : w.b) < 0;
  if (d.arris <= 0 || g.neighbour[e] < 0 || !outside) return r;
  const u32 j = static_cast<u32>(g.neighbour[e]);
  const structure::Wall& wj = d.walls[j];
  const WallGeom& gj = ctx.walls[j];
  // This face is its wall's outgoing left or right at this end, and the face it meets is the
  // neighbour's facing side, as `solve_walls` pairs them.
  const f64 sign = e == 0 ? 1.0 : -1.0;
  const f64 out_side = side * sign;
  const f64 j_sign = g.neighbour_from[e] ? 1.0 : -1.0;
  const f64 j_side = -out_side * j_sign;
  if (wj.kind != structure::WallKind::exterior || (j_side > 0 ? wj.a : wj.b) >= 0) return r;
  r.a = {g.frame.dx * sign, g.frame.dz * sign};
  r.b = {gj.frame.dx * j_sign, gj.frame.dz * j_sign};
  // Convex seen from outside: the neighbour leaves toward the other side of this wall.
  const f64 turn = r.a.z * r.b.x - r.a.x * r.b.z;
  if (!(out_side * turn < -1e-6)) return r;
  const f64 k = (1.0 + r.a.x * r.b.x + r.a.z * r.b.z) / (turn < 0 ? -turn : turn);
  const f64 radius = d.arris;
  r.back[0] = r.back[1] = radius * k;
  r.back[2] = (radius + (w.storey == 0 ? static_cast<f64>(d.plinth_proud) : 0.0)) * k;
  if (r.back[2] > 0.4 * std::min(g.length, gj.length)) return r;
  r.on = true;
  r.other = j;
  return r;
}

// The arc that joins two outer faces at a rounded corner: `k_arris_segments` strips from the
// foot to the top, the plinth's band standing proud with its ledge, the wall's leaning in as
// its faces do. The arc is a rational quadratic through the two faces' ends, which is a circle's
// with no trigonometric function.
void round_corner(const Context& ctx, u32 wall, f64 side, u32 e, const Round& r, f64 foot,
                  f64 plinth, const Lean& lean, f64 proud) {
  const Description& d = *ctx.d;
  const WallGeom& g = ctx.walls[wall];
  const u32 side_index = side > 0 ? 0u : 1u;
  const u32 columns = k_arris_segments + 1;
  const f64 shift[3] = {0.0, -lean.total, proud};
  const f64 cosine = r.a.x * r.b.x + r.a.z * r.b.z;
  const f64 weight = std::sqrt(0.5 * (1.0 - cosine));
  F2 pos[3][k_arris_segments + 1];
  Vec3 normals[k_arris_segments + 1];
  f64 along[k_arris_segments + 1];
  for (u32 m = 0; m < 3; ++m) {
    const f64 s = g.ends[m][side_index][e], q = side * (g.half + shift[m]);
    const F2 c{g.frame.ox + g.frame.dx * s + g.frame.nx * q,
               g.frame.oz + g.frame.dz * s + g.frame.nz * q};
    const F2 ta{c.x + r.a.x * r.back[m], c.z + r.a.z * r.back[m]};
    const F2 tb{c.x + r.b.x * r.back[m], c.z + r.b.z * r.back[m]};
    const F2 centre{c.x + (r.a.x + r.b.x) * r.back[m] / (1.0 + cosine),
                    c.z + (r.a.z + r.b.z) * r.back[m] / (1.0 + cosine)};
    for (u32 i = 0; i < columns; ++i) {
      const f64 u = static_cast<f64>(i) / static_cast<f64>(k_arris_segments);
      const f64 w0 = (1.0 - u) * (1.0 - u), w1 = 2.0 * u * (1.0 - u) * weight, w2 = u * u;
      const f64 sum = w0 + w1 + w2;
      pos[m][i] = {(w0 * ta.x + w1 * c.x + w2 * tb.x) / sum,
                   (w0 * ta.z + w1 * c.z + w2 * tb.z) / sum};
      if (m != 0) continue;
      const f64 nx = pos[0][i].x - centre.x, nz = pos[0][i].z - centre.z;
      const f64 nl = std::sqrt(nx * nx + nz * nz);
      normals[i] = {static_cast<f32>(nx / nl), 0.0f, static_cast<f32>(nz / nl)};
      const f64 step =
          i == 0 ? 0.0
                 : std::sqrt((pos[0][i].x - pos[0][i - 1].x) * (pos[0][i].x - pos[0][i - 1].x) +
                             (pos[0][i].z - pos[0][i - 1].z) * (pos[0][i].z - pos[0][i - 1].z));
      // u runs on from the face's own end: past it at the wall's `to` end, before it at `from`.
      const f64 sense = e == 0 ? -1.0 : 1.0;
      along[i] = i == 0 ? s - sense * r.back[0] : along[i - 1] + sense * step;
    }
  }
  f64 tops[k_arris_segments + 1];
  for (u32 i = 0; i < columns; ++i)
    tops[i] = wall_top(d, wall, pos[1][i].x, pos[1][i].z);
  // One strip of a band: from mode `m0` at `y0` to mode `m1` at the heights `y1`.
  const auto band = [&](MeshPart& part, u32 m0, f64 y0, u32 m1, const f64* y1, f64 tilt) {
    for (u32 i = 0; i + 1 < columns; ++i) {
      if (!(y1[i] > y0 + k_eps) && !(y1[i + 1] > y0 + k_eps)) continue;
      Vec3 points[4];
      Vec2 uvs[4];
      Vec3 ns[4];
      u32 count = 0;
      const auto add = [&](u32 m, u32 column, f64 y) {
        points[count] = metres(pos[m][column].x, y, pos[m][column].z);
        uvs[count] = {static_cast<f32>(along[column] * k_m), static_cast<f32>(y * k_m)};
        ns[count] = normalize(normals[column] + Vec3{0, 1, 0} * static_cast<f32>(tilt));
        ++count;
      };
      add(m0, i, y0);
      add(m0, i + 1, y0);
      if (y1[i + 1] > y0 + k_eps) add(m1, i + 1, y1[i + 1]);
      if (y1[i] > y0 + k_eps) add(m1, i, y1[i]);
      polygon_smooth(part, points, uvs, ns, count, normalize(normals[i] + normals[i + 1]));
    }
  };
  const bool banded = plinth > foot + k_eps;
  f64 level[k_arris_segments + 1];
  const auto flat = [&](f64 y) {
    for (u32 i = 0; i < columns; ++i)
      level[i] = y;
    return &level[0];
  };
  f64 from = foot;
  if (banded) {
    MeshPart& stone = ctx.part(Surface::plinth);
    band(stone, 2, foot, 2, flat(plinth), 0.0);
    if (proud > 0.0)
      for (u32 i = 0; i + 1 < columns; ++i) {
        const Vec3 points[4] = {metres(pos[2][i].x, plinth, pos[2][i].z),
                                metres(pos[2][i + 1].x, plinth, pos[2][i + 1].z),
                                metres(pos[0][i + 1].x, plinth, pos[0][i + 1].z),
                                metres(pos[0][i].x, plinth, pos[0][i].z)};
        const Vec2 uvs[4] = {{static_cast<f32>(along[i] * k_m), 0.0f},
                             {static_cast<f32>(along[i + 1] * k_m), 0.0f},
                             {static_cast<f32>(along[i + 1] * k_m), static_cast<f32>(proud * k_m)},
                             {static_cast<f32>(along[i] * k_m), static_cast<f32>(proud * k_m)}};
        polygon(stone, points, uvs, 4, Vec3{0, 1, 0});
      }
    from = plinth;
  }
  MeshPart& face = ctx.part(Surface::wall_outer);
  if (lean.total > 0.0) {
    if (lean.y0 > from + k_eps) band(face, 0, from, 0, flat(lean.y0), 0.0);
    band(face, 0, lean.y0, 1, flat(lean.y1), lean.total / (lean.y1 - lean.y0));
    band(face, 1, lean.y1, 1, tops, 0.0);
  } else {
    band(face, 0, from, 0, tops, 0.0);
  }
}

// A face's corners shared between its cells, so a face is one welded sheet.
struct FaceVerts {
  Vector<f64> s, y;
  Vector<u32> index;
  // The corner at `(ss, yy)`: found, or made at `position` with `normal`.
  u32 get(MeshPart& part, f64 ss, f64 yy, Vec3 position, Vec3 normal) {
    for (u32 i = 0; i < s.size(); ++i)
      if (s[i] == ss && y[i] == yy) return index[i];
    const u32 id = part.positions.size();
    part.positions.push_back(position);
    part.normals.push_back(normal);
    part.uvs.push_back({static_cast<f32>(ss * k_m), static_cast<f32>(yy * k_m)});
    s.push_back(ss);
    y.push_back(yy);
    index.push_back(id);
    return id;
  }
};

// One side of a wall (`side` +1 its left, -1 its right): a grid of its stations (its ends, its
// openings' jambs, where the roof's ridge crosses it) and its levels (its foot, the plinth's top,
// its openings' sills and heads, its top at every station), each cell kept, dropped inside an
// opening or above the top, or cut by the top's line.
void wall_face(const Context& ctx, u32 wall, f64 side) {
  const Description& d = *ctx.d;
  const structure::Wall& w = d.walls[wall];
  const WallGeom& g = ctx.walls[wall];
  const structure::Storey& storey = d.storeys[w.storey];
  const bool outside = w.kind == structure::WallKind::exterior && (side > 0 ? w.a : w.b) < 0;
  if (!ctx.wants(outside)) return;
  // Where the face begins and ends in each mode of `WallGeom::ends`: at its joins, or short of a
  // rounded corner (structures.md, "Faces that are not planes").
  const u32 side_index = side > 0 ? 0u : 1u;
  const Round rounds[2] = {round_of(ctx, wall, side, 0), round_of(ctx, wall, side, 1)};
  f64 begin[3], end[3];
  for (u32 m = 0; m < 3; ++m) {
    begin[m] = g.ends[m][side_index][0] + (rounds[0].on ? rounds[0].back[m] : 0.0);
    end[m] = g.ends[m][side_index][1] - (rounds[1].on ? rounds[1].back[m] : 0.0);
  }
  const f64 s_begin = begin[0], s_end = end[0];
  if (!(s_end - s_begin > k_eps)) return;
  const f64 q = side * g.half;
  const f64 elevation = storey.elevation;
  const f64 foot =
      outside && w.storey == 0 ? elevation - static_cast<f64>(d.foundation) : elevation;
  const f64 plinth = outside && w.storey == 0 ? static_cast<f64>(d.plinth_top) : foot;
  const bool banded = plinth > foot + k_eps;
  const Lean lean = outside ? lean_of(d, w) : Lean{};
  const f64 proud = outside && banded ? static_cast<f64>(d.plinth_proud) : 0.0;
  const f64 q_top = side * (g.half - lean.total);
  const f64 wave = static_cast<f64>(w.wave) * 0.1;
  const f64 wave_foot = banded ? plinth : foot;

  Vector<f64> stations;
  stations.push_back(s_begin);
  stations.push_back(s_end);
  Vector<Cut> cuts;
  for (const structure::Opening& o : d.openings) {
    if (o.wall != wall) continue;
    const Cut c = cut_of(d, o);
    cuts.push_back(c);
    stations.push_back(c.s0);
    stations.push_back(c.s1);
  }
  const auto ground = [&](f64 s, f64& x, f64& z) {
    x = g.frame.ox + g.frame.dx * s + g.frame.nx * q;
    z = g.frame.oz + g.frame.dz * s + g.frame.nz * q;
  };
  if (w.storey + 1 == d.storeys.size()) {
    // Where the face's line crosses a gable's ridge its top turns.
    for (const structure::Roof& r : d.roofs) {
      if (r.form != structure::RoofForm::gable) continue;
      const f64 mid =
          r.ridge_along_x ? 0.5 * (r.rect[1] + r.rect[3]) : 0.5 * (r.rect[0] + r.rect[2]);
      f64 x0, z0, x1, z1;
      ground(s_begin, x0, z0);
      ground(s_end, x1, z1);
      const f64 c0 = r.ridge_along_x ? z0 : x0, c1 = r.ridge_along_x ? z1 : x1;
      if ((c0 - mid) * (c1 - mid) < 0) {
        const f64 s = s_begin + (mid - c0) * (s_end - s_begin) / (c1 - c0);
        if (s > s_begin + k_eps && s < s_end - k_eps) stations.push_back(s);
      }
    }
  }
  sort_unique(stations);
  if (wave > 0.0) subdivide(stations, 0, k_wave_cell_cm);
  const u32 last = stations.size() - 1;
  Vector<f64> tops;
  Vector<f64> levels;
  levels.push_back(foot);
  if (banded) levels.push_back(plinth);
  for (const Cut& c : cuts) {
    levels.push_back(c.y0);
    levels.push_back(c.y1);
  }
  for (u32 i = 0; i <= last; ++i) {
    // The top is the roof's underside over the face where it stands at its top: leaned in, and at
    // its ends where the leaned corner is.
    const f64 s = i == 0 ? begin[1] : (i == last ? end[1] : stations[i]);
    tops.push_back(wall_top(d, wall, g.frame.ox + g.frame.dx * s + g.frame.nx * q_top,
                            g.frame.oz + g.frame.dz * s + g.frame.nz * q_top));
    levels.push_back(tops.back());
  }
  if (lean.total > 0.0) {
    levels.push_back(lean.y0);
    levels.push_back(lean.y1);
  }
  sort_unique(levels);
  if (wave > 0.0) {
    u32 from = 0;
    while (from + 1 < levels.size() && levels[from] < wave_foot - k_eps)
      ++from;
    subdivide(levels, from, k_wave_cell_cm);
  }

  const Vec3 normal{static_cast<f32>(side * g.frame.nx), 0.0f, static_cast<f32>(side * g.frame.nz)};
  const Surface face_surface = outside ? Surface::wall_outer : Surface::wall_inner;
  FaceVerts face_verts, plinth_verts;
  // Where the grid's corner `(ss, yy)` stands along the wall: the first and the last cells are
  // stretched to the face's own end at that height, which moves as the corner leans and stands
  // out with the plinth.
  const f64 st0 = stations[0], st1 = stations[1], stm = stations[last - 1], stn = stations[last];
  const auto actual_s = [&](f64 ss, f64 yy, bool in_plinth) {
    const f64 f = lean.fraction(yy);
    const f64 b = in_plinth ? begin[2] : begin[0] + (begin[1] - begin[0]) * f;
    const f64 e = in_plinth ? end[2] : end[0] + (end[1] - end[0]) * f;
    if (ss < st1 && b != st0) return st1 - (st1 - ss) * (st1 - b) / (st1 - st0);
    if (ss > stm && e != stn) return stm + (ss - stm) * (e - stm) / (stn - stm);
    return ss;
  };
  const Vec3 up{0.0f, 1.0f, 0.0f};
  // The corner's vertex: on the face's plane, or off it by the plinth's projection, the lean at
  // its height and the face's wander, which dies away toward every edge the face must meet.
  const auto place = [&](FaceVerts& verts, MeshPart& part, bool in_plinth, f64 ss, f64 yy,
                         f64 top_here) {
    f64 off = in_plinth ? proud : -lean.at(yy);
    Vec3 n = normal;
    if (!in_plinth && lean.total > 0.0 && yy < lean.y1 + k_eps && yy > lean.y0 - k_eps)
      n = normalize(normal + up * static_cast<f32>(lean.total / (lean.y1 - lean.y0)));
    if (wave > 0.0 && !in_plinth) {
      f64 reach = std::min(std::min(ss - st0, stn - ss), std::min(yy - wave_foot, top_here - yy));
      for (const Cut& c : cuts) {
        const f64 ds = std::max(std::max(c.s0 - ss, ss - c.s1), 0.0);
        const f64 dy = std::max(std::max(c.y0 - yy, yy - c.y1), 0.0);
        reach = std::min(reach, std::max(ds, dy));
      }
      if (reach > 0.0) {
        // Half the stated wander either way about a face stood out by that half, so the face
        // never falls behind its plane: what butts it (a partition's end, a floor's edge) stays
        // buried in it.
        const f64 amp = 0.5 * wave * (reach >= k_wave_fade_cm ? 1.0 : reach / k_wave_fade_cm);
        off += amp;
        const f64 ws =
            (wave_at(wall, side_index, ss + 4.0, yy) - wave_at(wall, side_index, ss - 4.0, yy)) /
            8.0;
        const f64 wy =
            (wave_at(wall, side_index, ss, yy + 4.0) - wave_at(wall, side_index, ss, yy - 4.0)) /
            8.0;
        off += amp * wave_at(wall, side_index, ss, yy);
        n = normalize(n - g.frame.dir() * static_cast<f32>(amp * ws) -
                      up * static_cast<f32>(amp * wy));
      }
    }
    return verts.get(part, ss, yy,
                     g.frame.at(actual_s(ss, yy, in_plinth), yy, side * (g.half + off)), n);
  };
  for (u32 i = 0; i + 1 < stations.size(); ++i) {
    const f64 sa = stations[i], sb = stations[i + 1];
    const f64 ta = tops[i], tb = tops[i + 1];
    const f64 t_min = std::min(ta, tb), t_max = std::max(ta, tb);
    const f64 sm = 0.5 * (sa + sb);
    for (u32 k = 0; k + 1 < levels.size(); ++k) {
      const f64 la = levels[k], lb = levels[k + 1];
      if (la < foot - k_eps || la >= t_max - k_eps) continue;
      const f64 ym = 0.5 * (la + lb);
      bool open = false;
      for (const Cut& c : cuts)
        open = open || (sm > c.s0 && sm < c.s1 && ym > c.y0 && ym < c.y1);
      if (open) continue;
      // The cell's corners, counter-clockwise in (s, y); under the top it is whole, across the
      // top's line (which runs through the grid's corners, so it enters and leaves on the cell's
      // level lines) it is the part beneath.
      f64 ps[4], py[4];
      u32 count = 4;
      if (lb <= t_min + k_eps) {
        ps[0] = sa, py[0] = la, ps[1] = sb, py[1] = la, ps[2] = sb, py[2] = lb, ps[3] = sa,
        py[3] = lb;
      } else {
        const f64 at_la = sa + (la - ta) * (sb - sa) / (tb - ta);
        const f64 at_lb = sa + (lb - ta) * (sb - sa) / (tb - ta);
        if (ta < tb) {
          ps[0] = at_la, py[0] = la, ps[1] = sb, py[1] = la, ps[2] = sb, py[2] = lb;
          ps[3] = at_lb, py[3] = lb;
        } else {
          ps[0] = sa, py[0] = la, ps[1] = at_la, py[1] = la, ps[2] = at_lb, py[2] = lb;
          ps[3] = sa, py[3] = lb;
        }
        // Drop a corner that repeats its neighbour (the line through a cell's corner).
        u32 kept = 0;
        for (u32 c = 0; c < 4; ++c) {
          const u32 prev = kept == 0 ? 3 : kept - 1;
          const bool same = kept > 0 && ps[c] - ps[prev] < k_eps && ps[prev] - ps[c] < k_eps &&
                            py[c] - py[prev] < k_eps && py[prev] - py[c] < k_eps;
          if (!same) ps[kept] = ps[c], py[kept] = py[c], ++kept;
        }
        if (kept > 1 && ps[0] - ps[kept - 1] < k_eps && ps[kept - 1] - ps[0] < k_eps &&
            py[0] - py[kept - 1] < k_eps && py[kept - 1] - py[0] < k_eps)
          --kept;
        count = kept;
        if (count < 3) continue;
      }
      const bool is_plinth = outside && lb <= plinth + k_eps && banded;
      MeshPart& part = ctx.part(is_plinth ? Surface::plinth : face_surface);
      FaceVerts& verts = is_plinth ? plinth_verts : face_verts;
      u32 ids[4];
      for (u32 c = 0; c < count; ++c) {
        // The top over this corner: its station's, or its own height on the top's line.
        const f64 top_here = ps[c] == sa ? ta : (ps[c] == sb ? tb : py[c]);
        ids[c] = place(verts, part, is_plinth, ps[c], py[c], top_here);
      }
      // Counter-clockwise in (s, y) faces the wall's right; the left face is wound the other way.
      for (u32 c = 1; c + 1 < count; ++c) {
        part.indices.push_back(ids[0]);
        part.indices.push_back(side > 0 ? ids[c + 1] : ids[c]);
        part.indices.push_back(side > 0 ? ids[c] : ids[c + 1]);
      }
    }
  }
  if (!outside) return;
  // The plinth's ledge: the step between its face and the wall's over it, except across a door.
  if (proud > 0.0) {
    MeshPart& stone = ctx.part(Surface::plinth);
    for (u32 i = 0; i + 1 < stations.size(); ++i) {
      const f64 sa = stations[i], sb = stations[i + 1], sm = 0.5 * (sa + sb);
      bool open = false;
      for (const Cut& c : cuts)
        open = open || (sm > c.s0 && sm < c.s1 && plinth > c.y0 && plinth < c.y1);
      if (open) continue;
      const f64 out = side * (g.half + proud);
      const Vec3 points[4] = {g.frame.at(actual_s(sa, plinth, true), plinth, out),
                              g.frame.at(actual_s(sb, plinth, true), plinth, out),
                              g.frame.at(actual_s(sb, plinth, false), plinth, q),
                              g.frame.at(actual_s(sa, plinth, false), plinth, q)};
      const Vec2 uvs[4] = {{static_cast<f32>(sa * k_m), 0.0f},
                           {static_cast<f32>(sb * k_m), 0.0f},
                           {static_cast<f32>(sb * k_m), static_cast<f32>(proud * k_m)},
                           {static_cast<f32>(sa * k_m), static_cast<f32>(proud * k_m)}};
      polygon(stone, points, uvs, 4, up);
    }
  }
  // A rounded corner is drawn once, by the lower-numbered of its two walls.
  for (u32 e = 0; e < 2; ++e)
    if (rounds[e].on && wall < rounds[e].other)
      round_corner(ctx, wall, side, e, rounds[e], foot, plinth, lean, proud);
}

Vec2 uv(f64 u_cm, f64 v_cm) noexcept {
  return {static_cast<f32>(u_cm * k_m), static_cast<f32>(v_cm * k_m)};
}

// A wall's free end, and its top where nothing stands on it.
void wall_ends(const Context& ctx, u32 wall) {
  const Description& d = *ctx.d;
  const structure::Wall& w = d.walls[wall];
  const WallGeom& g = ctx.walls[wall];
  const bool exterior = w.kind == structure::WallKind::exterior;
  if (!ctx.wants(exterior)) return;
  const structure::Storey& storey = d.storeys[w.storey];
  const f64 elevation = storey.elevation;
  const f64 foot =
      exterior && w.storey == 0 ? elevation - static_cast<f64>(d.foundation) : elevation;
  MeshPart& part = ctx.part(exterior ? Surface::wall_outer : Surface::wall_inner);
  const auto top_at = [&](f64 s, f64 q) {
    return wall_top(d, wall, g.frame.ox + g.frame.dx * s + g.frame.nx * q,
                    g.frame.oz + g.frame.dz * s + g.frame.nz * q);
  };
  for (u32 e = 0; e < 2; ++e) {
    if (!g.cap[e]) continue;
    const f64 s = g.sl[e];
    const Vec3 points[4] = {g.frame.at(s, foot, -g.half), g.frame.at(s, foot, g.half),
                            g.frame.at(s, top_at(s, g.half), g.half),
                            g.frame.at(s, top_at(s, -g.half), -g.half)};
    const Vec2 uvs[4] = {uv(-g.half, foot), uv(g.half, foot), uv(g.half, top_at(s, g.half)),
                         uv(-g.half, top_at(s, -g.half))};
    const f32 sign = e == 0 ? -1.0f : 1.0f;
    polygon(part, points, uvs, 4, g.frame.dir() * sign);
  }
  const f64 mx = g.frame.ox + g.frame.dx * g.length * 0.5;
  const f64 mz = g.frame.oz + g.frame.dz * g.length * 0.5;
  const bool covered = w.storey + 1 != d.storeys.size() || roof_over(d, mx, mz) != nullptr;
  if (covered) return;
  const f64 top = elevation + static_cast<f64>(storey.height);
  const Vec3 points[4] = {g.frame.at(g.sl[0], top, g.half), g.frame.at(g.sl[1], top, g.half),
                          g.frame.at(g.sr[1], top, -g.half), g.frame.at(g.sr[0], top, -g.half)};
  const Vec2 uvs[4] = {uv(g.sl[0], g.half), uv(g.sl[1], g.half), uv(g.sr[1], -g.half),
                       uv(g.sr[0], -g.half)};
  polygon(part, points, uvs, 4, Vec3{0, 1, 0});
}

// An opening's reveal through the wall's thickness, and its timbers: a frame set back from the
// outer face, a window's glazing bars and sill board, a lintel over it (structures.md,
// "Openings"). Every timber is a closed box sunk a centimetre into what it meets.
void wall_opening(const Context& ctx, const structure::Opening& o) {
  const Description& d = *ctx.d;
  const structure::Wall& w = d.walls[o.wall];
  const WallGeom& g = ctx.walls[o.wall];
  const bool exterior = w.kind == structure::WallKind::exterior;
  if (!ctx.wants(exterior)) return;
  const Cut c = cut_of(d, o);
  const f64 h = g.half;
  const bool door = o.kind == structure::OpeningKind::door;
  // The reveal runs from the wall's one face to the other where each stands at that height: an
  // outer face leans in over the plinth's top and stands out with the plinth under it, so a jamb
  // that crosses that level is two pieces.
  const f64 out_sign = out_sign_of(w);
  const bool has_out = exterior && ((w.a < 0) != (w.b < 0));
  const Lean lean = lean_of(d, w);
  const f64 ground =
      static_cast<f64>(d.storeys[w.storey].elevation) - static_cast<f64>(d.foundation);
  const f64 proud = has_out && w.storey == 0 && static_cast<f64>(d.plinth_top) > ground + k_eps
                        ? static_cast<f64>(d.plinth_proud)
                        : 0.0;
  const auto face_q = [&](f64 sign, f64 y, bool low) {
    if (!has_out || sign != out_sign) return sign * h;
    return sign * (h + (low ? proud : -lean.at(y)));
  };
  {
    MeshPart& part = ctx.part(Surface::wall_inner);
    const bool low_sill = proud > 0.0 && c.y0 < lean.y0 - k_eps;
    const bool low_head = proud > 0.0 && c.y1 <= lean.y0 + k_eps;
    const bool spans = has_out && (proud > 0.0 || lean.total > 0.0) && c.y0 < lean.y0 - k_eps &&
                       c.y1 > lean.y0 + k_eps;
    const auto jamb = [&](f64 s, f64 y0, f64 y1, bool low, Vec3 facing) {
      const f64 qa0 = face_q(-1.0, y0, low), qb0 = face_q(1.0, y0, low);
      const f64 qa1 = face_q(-1.0, y1, low), qb1 = face_q(1.0, y1, low);
      const Vec3 points[4] = {g.frame.at(s, y0, qa0), g.frame.at(s, y0, qb0),
                              g.frame.at(s, y1, qb1), g.frame.at(s, y1, qa1)};
      const Vec2 uvs[4] = {uv(qa0, y0), uv(qb0, y0), uv(qb1, y1), uv(qa1, y1)};
      polygon(part, points, uvs, 4, facing);
    };
    for (u32 k = 0; k < 2; ++k) {
      const f64 s = k == 0 ? c.s0 : c.s1;
      const Vec3 facing = g.frame.dir() * (k == 0 ? 1.0f : -1.0f);
      if (spans) {
        jamb(s, c.y0, lean.y0, low_sill, facing);
        jamb(s, lean.y0, c.y1, false, facing);
      } else {
        jamb(s, c.y0, c.y1, low_head, facing);
      }
    }
    const f64 ha = face_q(-1.0, c.y1, low_head), hb = face_q(1.0, c.y1, low_head);
    const f64 sa = face_q(-1.0, c.y0, low_sill), sb = face_q(1.0, c.y0, low_sill);
    const Vec3 head[4] = {g.frame.at(c.s0, c.y1, ha), g.frame.at(c.s0, c.y1, hb),
                          g.frame.at(c.s1, c.y1, hb), g.frame.at(c.s1, c.y1, ha)};
    const Vec3 sill[4] = {g.frame.at(c.s0, c.y0, sa), g.frame.at(c.s0, c.y0, sb),
                          g.frame.at(c.s1, c.y0, sb), g.frame.at(c.s1, c.y0, sa)};
    const Vec2 head_uv[4] = {uv(c.s0, ha), uv(c.s0, hb), uv(c.s1, hb), uv(c.s1, ha)};
    const Vec2 sill_uv[4] = {uv(c.s0, sa), uv(c.s0, sb), uv(c.s1, sb), uv(c.s1, sa)};
    polygon(part, head, head_uv, 4, Vec3{0, -1, 0});
    polygon(door ? ctx.part(Surface::floor) : part, sill, sill_uv, 4, Vec3{0, 1, 0});
  }
  if (o.kind == structure::OpeningKind::doorway) return;
  // Depth `t` is from the outer face's plane inward; an inner wall's frame sits in its middle.
  const f64 thick = 2.0 * h;
  MeshPart& timber = ctx.part(Surface::timber);
  const auto member = [&](f64 s0, f64 s1, f64 y0, f64 y1, f64 t0, f64 t1, u32 grain) {
    const f64 qa = out_sign * (h - t0), qb = out_sign * (h - t1);
    box(timber, g.frame, s0, s1, y0, y1, std::min(qa, qb), std::max(qa, qb), grain);
  };
  const Joinery joinery = joinery_of(d, w, o);
  const f64 depth = joinery.depth, width = joinery.width, set = joinery.set;
  // The frame: two jambs to the head's top, a head between them, a window's sill member.
  member(c.s0 - 1.0, c.s0 + width, c.y0 - 1.0, c.y1 + 1.0, set, set + depth, 1);
  member(c.s1 - width, c.s1 + 1.0, c.y0 - 1.0, c.y1 + 1.0, set, set + depth, 1);
  member(c.s0 + width, c.s1 - width, c.y1 - width, c.y1 + 1.0, set, set + depth, 0);
  if (!door) {
    member(c.s0 + width, c.s1 - width, c.y0 - 1.0, c.y0 + width, set, set + depth, 0);
    const f64 clear = c.s1 - c.s0 - 2.0 * width;
    if (o.mullions > 0) {
      // An unglazed window's mullions: square timbers set on the cant (a corner toward the
      // weather), sunk a centimetre into the sill and the head, in the frame's middle.
      const f64 r = 0.5 * k_mullion_cm;
      const f64 q = out_sign * (h - set - 0.5 * depth);
      const f64 k = std::sqrt(0.5);
      for (u32 i = 1; i <= o.mullions; ++i) {
        const f64 s = c.s0 + width + clear * static_cast<f64>(i) / static_cast<f64>(o.mullions + 1);
        Frame3 frame;
        frame.o[0] = g.frame.ox + g.frame.dx * s + g.frame.nx * q;
        frame.o[1] = 0.0;
        frame.o[2] = g.frame.oz + g.frame.dz * s + g.frame.nz * q;
        frame.axis[0][0] = 0.0, frame.axis[0][1] = 1.0, frame.axis[0][2] = 0.0;
        frame.axis[1][0] = (g.frame.dx + g.frame.nx) * k, frame.axis[1][1] = 0.0;
        frame.axis[1][2] = (g.frame.dz + g.frame.nz) * k;
        frame.axis[2][0] = (g.frame.dx - g.frame.nx) * k, frame.axis[2][1] = 0.0;
        frame.axis[2][2] = (g.frame.dz - g.frame.nz) * k;
        box3(timber, frame, c.y0 + width - 1.0, c.y1 - width + 1.0, -r, r, -r, r, 0);
      }
    } else {
      // Glazing bars: an upright every half metre or so, a transom in a tall light.
      const u32 uprights = static_cast<u32>(clear / 45.0);
      for (u32 i = 1; i <= uprights; ++i) {
        const f64 s = c.s0 + width + clear * static_cast<f64>(i) / static_cast<f64>(uprights + 1);
        member(s - 1.5, s + 1.5, c.y0 + width, c.y1 - width, set + 2.0, set + 5.0, 1);
      }
      if (c.y1 - c.y0 > 100.0) {
        const f64 y = 0.5 * (c.y0 + c.y1);
        member(c.s0 + width, c.s1 - width, y - 1.5, y + 1.5, set + 2.0, set + 5.0, 0);
      }
    }
    // The sill board, proud of the outer face.
    if (exterior)
      member(c.s0 - 6.0, c.s1 + 6.0, c.y0 - 4.0, c.y0 + 1.0, -5.0 + lean.at(c.y0 - 4.0), set + 1.0,
             0);
  }
  // The lintel: a timber over the opening, bearing on the wall either side, proud of the face
  // where the face stands at its foot.
  if (exterior) {
    const f64 back = lean.at(c.y1 - 1.0);
    member(c.s0 - 15.0, c.s1 + 15.0, c.y1 - 1.0, c.y1 + 13.0, back - 1.5,
           back + std::min(12.0, thick - 2.0), 0);
  }
  // The leaves are not here: a leaf turns, so it is a mesh of its own (`mesh_leaf`, the `leaf`
  // scope; structures.md, "A leaf is a mesh of its own"), and the opening's frame stands empty.
}

// One leaf (structures.md, "Leaves and mullions"): upright boards of unequal widths, every other
// one a few millimetres proud so the joints read, on ledges across the face the leaf opens toward:
// three to a door, two to a shutter. Each ledge is sunk a centimetre into its boards. `frame`'s
// first axis runs from the hinge to the free edge and its third back through the thickness; its
// origin's height is where the description's y 0 stands in it.
void emit_leaf(MeshPart& timber, const LeafPlace& p, const Frame3& frame) {
  const u32 boards = std::min(8u, std::max(1u, static_cast<u32>(p.width / k_leaf_board_cm + 0.5)));
  const f64 share[4] = {1.12, 0.92, 1.06, 0.90};
  f64 total = 0.0;
  for (u32 b = 0; b < boards; ++b)
    total += share[b % 4];
  f64 at = 0.0;
  for (u32 b = 0; b < boards; ++b) {
    const f64 wide = p.width * share[b % 4] / total;
    const f64 stand = b % 2 == 1 ? 0.3 : 0.0;
    // Each board is cut from its own length of timber: its grain starts somewhere else.
    const f64 shift = 37.0 * static_cast<f64>(b + 1);
    Frame3 board = frame;
    board.o[1] = frame.o[1] - shift;
    box3(timber, board, at, at + wide, p.y0 + shift, p.y1 + shift, p.ledges,
         p.ledges + p.boards + stand, 1);
    at += wide;
  }
  const f64 tall = p.y1 - p.y0;
  const u32 ledges = tall > 120.0 ? 3u : 2u;
  const f64 ledge = p.door ? 15.0 : 9.0;
  const f64 inset = std::min(20.0, tall * 0.12);
  for (u32 i = 0; i < ledges; ++i) {
    const f64 y = p.y0 + inset +
                  (tall - 2.0 * inset - ledge) * static_cast<f64>(i) / static_cast<f64>(ledges - 1);
    box3(timber, frame, 2.0, p.width - 2.0, y, y + ledge, 0.0, p.ledges + 1.0, 0);
  }
}

// A roof: each slope a slab over its rectangle and its overhangs, the covering on top, at the
// eaves and at the verges, boards beneath. A coat 15 cm deep or more (a thatch) is lit soft: its
// normals turn over the ridge and round the eaves; a thin covering keeps its planes.
void mesh_roof(const Context& ctx, const structure::Roof& r) {
  if (!ctx.wants(true)) return;
  const Description& d = *ctx.d;
  const bool along_x = r.ridge_along_x;
  const f64 a0 = static_cast<f64>(along_x ? r.rect[0] : r.rect[1]) - static_cast<f64>(r.verge);
  const f64 a1 = static_cast<f64>(along_x ? r.rect[2] : r.rect[3]) + static_cast<f64>(r.verge);
  const f64 lo = along_x ? r.rect[1] : r.rect[0];
  const f64 hi = along_x ? r.rect[3] : r.rect[2];
  const f64 p = static_cast<f64>(r.pitch) / 1000.0;
  const f64 len = std::sqrt(1.0 + p * p);
  const f64 vertical = static_cast<f64>(r.thickness) * len;  // the coat's depth measured plumb
  const f64 over = r.overhang;
  const bool soft = r.thickness >= 15;
  MeshPart& cover = ctx.part(Surface::roof);
  // What is seen from beneath: a thatch's own underside between its battens, a thin covering's
  // boards.
  MeshPart& under = ctx.part(soft ? Surface::roof : Surface::boards);
  const auto point = [&](f64 a, f64 c, f64 y) {
    return along_x ? metres(a, y, c) : metres(c, y, a);
  };
  const auto vec = [&](f64 a, f64 c, f64 y) {
    return along_x ? Vec3{static_cast<f32>(a), static_cast<f32>(y), static_cast<f32>(c)}
                   : Vec3{static_cast<f32>(c), static_cast<f32>(y), static_cast<f32>(a)};
  };
  const bool gable = r.form == structure::RoofForm::gable;
  const u32 slopes = gable ? 2 : 1;
  for (u32 k = 0; k < slopes; ++k) {
    // The slope from its eave (`ce`, with the overhang) up to its ridge (`cr`); `out` is the
    // horizontal sense the eave faces.
    const f64 out = k == 0 ? -1.0 : 1.0;
    const f64 ce = k == 0 ? lo - over : hi + over;
    const f64 cr = gable ? 0.5 * (lo + hi) : hi + over;
    const f64 run = (cr - ce) * -out;  // positive
    const f64 ye = static_cast<f64>(r.eaves) - p * over;
    const f64 yr = ye + p * run;
    const Vec3 slope_n = vec(0.0, out * p / len, 1.0 / len);
    const Vec3 up = vec(0.0, 0.0, 1.0);
    // The coat's profile from the eave's foot up to the ridge: `pr` the run from the eave and `ph`
    // the height above the underside's plane, measured plumb. A thin covering is its eave's
    // plumb face and its plane. A soft coat (a thatch) rounds its eave over the top fifth of its
    // depth and swells toward the ridge, where a thatch is laid thickest and capped.
    //
    // What a thatch states beyond its depth (structures.md, "Roofs"): `eave_cut` cuts the eave
    // square to the slope, so its face looks down and out over the coat's whole depth and the
    // underside starts `cut` in from the edge; `course` lays the slope in courses that far apart
    // up it, each one's surface falling `course_step_mm` to the butts of the next, which stand
    // as a small step. A course's face keeps its own plane (`hard`), so the light shows the
    // steps.
    f64 pr[k_roof_profile], ph[k_roof_profile];
    bool hard[k_roof_profile] = {};
    u32 count = 0;
    const auto add = [&](f64 run_at, f64 height, bool crease = false) {
      pr[count] = run_at, ph[count] = height, hard[count] = crease, ++count;
    };
    const bool coursed = soft && r.course > 0;
    f64 cut = 0.0;
    if (soft && r.eave_cut)
      cut = std::min(static_cast<f64>(r.thickness) * p / len, std::max(0.0, over - 6.0));
    add(cut, 0.0);
    if (soft) {
      const f64 turn = std::min(0.6 * vertical, run * 0.2);
      f64 from = turn;
      if (cut > 0.0) {
        add(cut * 0.06, vertical * 0.94, true);
        add(cut * 0.3, vertical);
        add(cut, vertical, coursed);
        from = cut;
      } else {
        add(0.0, vertical * 0.55);
        add(turn * 0.25, vertical * 0.82);
        add(turn, vertical, coursed);
      }
      const bool ridged = gable && run > 120.0;
      const f64 to = ridged ? run - 45.0 : run;
      if (coursed) {
        const f64 step = static_cast<f64>(r.course_step_mm) * 0.1 * len;
        const u32 courses = std::min(
            k_roof_courses, std::max(1u, static_cast<u32>((to - from) * len / r.course + 0.5)));
        for (u32 c = 1; c <= courses; ++c) {
          const f64 at = from + (to - from) * static_cast<f64>(c) / static_cast<f64>(courses);
          add(at, vertical - step, true);
          if (c < courses) add(at, vertical, true);
        }
        if (ridged) add(to, vertical + 6.0, true);  // a block ridge: its skirt's cut edge
      } else if (ridged) {
        add(run - 45.0, vertical);
        add(run - 38.0, vertical + 6.0);  // the ridge course's lower edge, standing proud
      }
      add(run, gable ? vertical + (ridged ? 8.0 : 0.0) : vertical);
    } else {
      add(0.0, vertical);
      add(run, vertical);
    }
    const auto at_c = [&](u32 i) { return ce - out * pr[i]; };
    const auto at_y = [&](u32 i) { return ye + p * pr[i] + ph[i]; };
    // A segment's outward normal in (a, c, y), and the length it adds to the surface.
    Vec3 seg_n[k_roof_profile];
    f64 seg_len[k_roof_profile];
    for (u32 i = 0; i + 1 < count; ++i) {
      const f64 dc = at_c(i + 1) - at_c(i), dy = at_y(i + 1) - at_y(i);
      seg_len[i] = std::sqrt(dc * dc + dy * dy);
      seg_n[i] = vec(0.0, out * dy / seg_len[i], -out * dc / seg_len[i]);
    }
    f64 along_v = 0.0;
    for (u32 i = 0; i + 1 < count; ++i) {
      const Vec3 points[4] = {point(a0, at_c(i), at_y(i)), point(a1, at_c(i), at_y(i)),
                              point(a1, at_c(i + 1), at_y(i + 1)),
                              point(a0, at_c(i + 1), at_y(i + 1))};
      // u up the slope, v along the ridge: a thatch's stems, drawn along its set's u, lie up it.
      const Vec2 uvs[4] = {uv(along_v, a0), uv(along_v, a1), uv(along_v + seg_len[i], a1),
                           uv(along_v + seg_len[i], a0)};
      along_v += seg_len[i];
      // A soft coat's corner takes the mean of the two segments it joins; the ridge's the mean of
      // the two slopes (straight up); the eave's foot keeps the fascia's.
      Vec3 n0 = soft && i > 0 ? normalize(seg_n[i - 1] + seg_n[i]) : seg_n[i];
      Vec3 n1 = !soft ? seg_n[i]
                      : (i + 2 < count ? normalize(seg_n[i] + seg_n[i + 1])
                                       : (gable ? normalize(seg_n[i] + up) : seg_n[i]));
      if (hard[i]) n0 = seg_n[i];
      if (hard[i + 1]) n1 = seg_n[i];
      const Vec3 normals[4] = {n0, n0, n1, n1};
      polygon_smooth(cover, points, uvs, normals, 4, seg_n[i]);
      if (pr[i + 1] - pr[i] < k_eps) continue;
      // Under a cut eave the coat's lower edge is the cut's face, not the underside's plane.
      const auto lower = [&](f64 run_at) {
        return run_at < cut ? vertical * (cut - run_at) / cut : 0.0;
      };
      for (u32 e = 0; e < 2; ++e) {
        // The verge: the coat's cut end over the gable, a strip a segment.
        const f64 a = e == 0 ? a0 : a1;
        const f64 u0 = pr[i] * len, u1 = pr[i + 1] * len;
        const f64 low0 = lower(pr[i]), low1 = lower(pr[i + 1]);
        Vec3 end[4];
        Vec2 end_uv[4];
        u32 corners = 0;
        end[corners] = point(a, at_c(i), ye + p * pr[i] + low0);
        end_uv[corners++] = uv(u0, low0);
        end[corners] = point(a, at_c(i + 1), ye + p * pr[i + 1] + low1);
        end_uv[corners++] = uv(u1, low1);
        if (ph[i + 1] - low1 > k_eps) {
          end[corners] = point(a, at_c(i + 1), at_y(i + 1));
          end_uv[corners++] = uv(u1, ph[i + 1]);
        }
        if (ph[i] - low0 > k_eps) {
          end[corners] = point(a, at_c(i), at_y(i));
          end_uv[corners++] = uv(u0, ph[i]);
        }
        if (corners >= 3) polygon(cover, end, end_uv, corners, vec(e == 0 ? -1.0 : 1.0, 0.0, 0.0));
      }
    }
    {
      // The underside, from the eave's foot (in from the edge under a cut eave) to the ridge.
      const f64 cf = ce - out * cut, yf = ye + p * cut;
      const Vec3 points[4] = {point(a0, cf, yf), point(a1, cf, yf), point(a1, cr, yr),
                              point(a0, cr, yr)};
      const Vec2 uvs[4] = {uv(cut * len, a0), uv(cut * len, a1), uv(run * len, a1),
                           uv(run * len, a0)};
      polygon(under, points, uvs, 4, slope_n * -1.0f);
    }
    if (!gable) {
      // A single slope's high edge.
      const Vec3 points[4] = {point(a0, cr, yr), point(a1, cr, yr), point(a1, cr, yr + vertical),
                              point(a0, cr, yr + vertical)};
      const Vec2 uvs[4] = {uv(0.0, a0), uv(0.0, a1), uv(vertical, a1), uv(vertical, a0)};
      polygon(cover, points, uvs, 4, vec(0.0, 1.0, 0.0));
    }
  }
  // **The roof's timbers**, where the top storey is open to them (structures.md, "Roofs"): a
  // rafter every 45 cm or so under each slope from the wall plate to the ridge, a collar across
  // every third pair, each a closed box sunk a centimetre into the boards it carries. They stand
  // between the end walls: a rafter inside a gable wall would show through its face.
  if (d.storeys[d.storeys.size() - 1].ceiling) return;
  f64 inset = 0.0;
  for (const structure::Wall& w : d.walls)
    if (w.storey + 1 == d.storeys.size()) inset = std::max(inset, static_cast<f64>(w.thickness));
  const f64 first = static_cast<f64>(along_x ? r.rect[0] : r.rect[1]) + inset + 12.0;
  const f64 last = static_cast<f64>(along_x ? r.rect[2] : r.rect[3]) - inset - 12.0;
  if (last - first < 30.0) return;
  MeshPart& timber = ctx.part(Surface::timber);
  const u32 bays = std::max(1u, static_cast<u32>((last - first) / 45.0 + 0.5));
  const f64 mid = 0.5 * (lo + hi);
  const f64 rise = gable ? p * (mid - lo) : p * (hi - lo);
  const auto axis_of = [&](f64 a, f64 c, f64 y, f64* out3) {
    out3[0] = along_x ? a : c, out3[1] = y, out3[2] = along_x ? c : a;
  };
  if (soft) {
    // Under a thatch: the battens its courses are tied to, a lath every 30 cm up each slope.
    for (u32 k = 0; k < slopes; ++k) {
      const f64 out = k == 0 ? -1.0 : 1.0;
      const f64 run = gable ? mid - lo : hi - lo;
      Frame3 frame;
      axis_of(first, k == 0 ? lo : hi, r.eaves, frame.o);
      axis_of(0.0, -out / len, p / len, frame.axis[0]);
      axis_of(0.0, out * p / len, 1.0 / len, frame.axis[1]);
      axis_of(1.0, 0.0, 0.0, frame.axis[2]);
      for (f64 u = inset * len + 20.0; u < run * len - 10.0; u += 30.0)
        box3(timber, frame, u - 2.5, u + 2.5, -2.5, 1.0, -8.0, last - first + 8.0, 2);
    }
  }
  for (u32 i = 0; i <= bays; ++i) {
    const f64 a = first + (last - first) * static_cast<f64>(i) / static_cast<f64>(bays);
    for (u32 k = 0; k < slopes; ++k) {
      const f64 out = k == 0 ? -1.0 : 1.0;
      const f64 foot = k == 0 ? lo : hi;
      const f64 run = gable ? mid - lo : hi - lo;
      Frame3 frame;
      axis_of(a, foot, r.eaves, frame.o);
      axis_of(0.0, -out / len, p / len, frame.axis[0]);       // up the slope
      axis_of(0.0, out * p / len, 1.0 / len, frame.axis[1]);  // square to it, outward
      axis_of(1.0, 0.0, 0.0, frame.axis[2]);                  // along the ridge
      box3(timber, frame, 0.0, run * len, -10.0, 1.0, -4.0, 4.0, 0);
    }
    if (gable && i % 3 == 1 && rise > 150.0) {
      // The collar at three fifths of the rise, its ends inside the rafters it ties.
      const f64 y = static_cast<f64>(r.eaves) + 0.6 * rise;
      const f64 half = (rise - 0.6 * rise) / p + 6.0;
      Frame3 frame;
      axis_of(a, mid, y, frame.o);
      axis_of(0.0, 1.0, 0.0, frame.axis[0]);
      axis_of(0.0, 0.0, 1.0, frame.axis[1]);
      axis_of(1.0, 0.0, 0.0, frame.axis[2]);
      box3(timber, frame, -half, half, -12.0, 0.0, -3.0, 3.0, 0);
    }
  }
}

// A chimney (structures.md, "Chimneys"): the stack from its base to under its cap, the cap a
// course wider. A hearth opens it on one side over its storey's floor: there the stack is two
// cheeks and a back round a recess `hearth_depth` deep, on a hearth raised 3 cm, under a timber
// across the opening.
void mesh_chimney(const Context& ctx, const structure::Chimney& c) {
  if (!ctx.wants(true)) return;
  const Description& d = *ctx.d;
  MeshPart& part = ctx.part(Surface::chimney);
  const f64 cx = 0.5 * (c.rect[0] + c.rect[2]), cz = 0.5 * (c.rect[1] + c.rect[3]);
  const bool hearth = c.hearth_side < 4 && c.hearth_storey < d.storeys.size();
  // In the frame whose `d` is the way the hearth faces (+x with none), the stack is `deep`
  // along and `wide` across.
  const u32 turn = hearth ? c.hearth_side : 0u;
  const Frame2 frame = axis_frame(cx, cz, turn);
  const f64 sx = c.rect[2] - c.rect[0], sz = c.rect[3] - c.rect[1];
  const f64 deep = 0.5 * (turn % 2 == 0 ? sx : sz), wide = 0.5 * (turn % 2 == 0 ? sz : sx);
  const f64 top = static_cast<f64>(c.top) - 6.0;
  box(part, frame, -deep - 4.0, deep + 4.0, static_cast<f64>(c.top) - 12.0, c.top, -wide - 4.0,
      wide + 4.0, 3);
  const f64 base = c.base;
  if (!hearth) {
    box(part, frame, -deep, deep, base, top, -wide, wide, 3);
    return;
  }
  const f64 level = static_cast<f64>(d.storeys[c.hearth_storey].elevation) + 3.0;
  const f64 head = level - 3.0 + static_cast<f64>(c.hearth_head);
  const f64 open = 0.5 * static_cast<f64>(c.hearth_width);
  const f64 back = deep - static_cast<f64>(c.hearth_depth);
  if (base < level) box(part, frame, -deep, deep, base, level, -wide, wide, 3);
  box(part, frame, -deep, deep, level, head, -wide, -open, 3);
  box(part, frame, -deep, deep, level, head, open, wide, 3);
  box(part, frame, -deep, back, level, head, -open, open, 3);
  box(part, frame, -deep, deep, head, top, -wide, wide, 3);
  // The timber over the opening, bearing on the cheeks and proud of the stack's face.
  const f64 bear = std::min(open + 15.0, wide + 2.0);
  box(ctx.part(Surface::timber), frame, deep - 14.0, deep + 3.0, head - 1.0, head + 20.0, -bear,
      bear, 2);
}

}  // namespace

u32 leaf_places(const Description& d, const structure::Opening& o, LeafPlace (&out)[2]) noexcept {
  if (o.leaf == structure::LeafKind::none || o.kind == structure::OpeningKind::doorway) return 0;
  const structure::Wall& w = d.walls[o.wall];
  const f64 wx = static_cast<f64>(w.to[0] - w.from[0]), wz = static_cast<f64>(w.to[1] - w.from[1]);
  const f64 length = std::sqrt(wx * wx + wz * wz);
  const f64 dx = wx / length, dz = wz / length;
  const f64 nx = dz, nz = -dx;  // the wall's left
  const Cut c = cut_of(d, o);
  const Joinery j = joinery_of(d, w, o);
  const f64 h = static_cast<f64>(w.thickness) * 0.5;
  const f64 out_sign = out_sign_of(w);
  const bool door = o.kind == structure::OpeningKind::door;
  // The side it opens to, as a sense along the wall's left, and the frame's face on that side:
  // the leaf's own face there is flush with it, its ledges and then its boards behind.
  const f64 swing = o.opens_right ? -1.0 : 1.0;
  const f64 qa = out_sign * (h - j.set), qb = out_sign * (h - j.set - j.depth);
  const f64 q = swing * qa > swing * qb ? qa : qb;
  const f64 clear = c.s1 - c.s0 - 2.0 * j.width - 0.6;
  const u32 count = o.leaves >= 2 ? 2u : 1u;
  f64 sine = 0.0, cosine = 1.0;
  turn_deg(o.leaf_open, sine, cosine);
  for (u32 k = 0; k < count; ++k) {
    const bool at_to = count == 2 ? k == 1 : o.hinge_at_to;
    const f64 sigma = at_to ? -1.0 : 1.0;  // from the hinge toward the free edge when shut
    const f64 s = at_to ? c.s1 - j.width - 0.3 : c.s0 + j.width + 0.3;
    LeafPlace& p = out[k];
    p.px = static_cast<f64>(w.from[0]) + dx * s + nx * q;
    p.pz = static_cast<f64>(w.from[1]) + dz * s + nz * q;
    // Shut, `u` runs along the wall and `v` back from the face into the frame; open, both turn
    // about the pivot toward the side it opens to.
    p.ux = cosine * sigma * dx + sine * swing * nx;
    p.uz = cosine * sigma * dz + sine * swing * nz;
    p.vx = -cosine * swing * nx + sine * sigma * dx;
    p.vz = -cosine * swing * nz + sine * sigma * dz;
    p.width = count == 2 ? clear * 0.5 - 0.15 : clear;
    p.y0 = c.y0 + (door ? 0.5 : j.width + 0.3);
    p.y1 = c.y1 - j.width - 0.3;
    p.boards = door ? 3.0 : 2.0;
    p.ledges = door ? 2.5 : 2.0;
    p.door = door;
  }
  return count;
}

bool leaf_shut(const Description& d, u32 index, LeafPlace& out,
               const structure::Opening*& opening) noexcept {
  u32 seen = 0;
  for (const structure::Opening& o : d.openings) {
    LeafPlace places[2];
    const u32 leaves = leaf_places(d, o, places);
    if (index >= seen + leaves) {
      seen += leaves;
      continue;
    }
    // The place stands the leaf at the opening's angle; undone with the same whole-degree sine
    // and cosine, so the shut leaf is the same bits on every toolchain.
    LeafPlace p = places[index - seen];
    f64 sine = 0.0, cosine = 1.0;
    turn_deg(o.leaf_open, sine, cosine);
    const f64 ux = cosine * p.ux + sine * p.vx, uz = cosine * p.uz + sine * p.vz;
    const f64 vx = cosine * p.vx - sine * p.ux, vz = cosine * p.vz - sine * p.uz;
    p.ux = ux, p.uz = uz, p.vx = vx, p.vz = vz;
    out = p;
    opening = &o;
    return true;
  }
  return false;
}

f64 leaf_side(const LeafPlace& shut) noexcept {
  // The swing is -v; the y of shut x swing is +1 when the leaf turns its +x toward -z.
  const f64 cross_y = shut.uz * (-shut.vx) - shut.ux * (-shut.vz);
  return cross_y >= 0.0 ? 1.0 : -1.0;
}

void mesh_leaf(const Context& ctx) {
  LeafPlace p;
  const structure::Opening* opening = nullptr;
  if (!leaf_shut(*ctx.d, ctx.leaf, p, opening)) return;
  // The leaf's own frame: its hinge's foot at the origin, +x to the free edge, y up, its thickness
  // along +z or -z (`leaf_side`), so a host turns it about +y at the hinge to open it.
  Frame3 frame;
  frame.o[0] = 0.0, frame.o[1] = -p.y0, frame.o[2] = 0.0;
  frame.axis[2][0] = 0.0, frame.axis[2][1] = 0.0, frame.axis[2][2] = leaf_side(p);
  emit_leaf(ctx.part(Surface::timber), p, frame);
}

void mesh_shell(const Context& ctx) {
  const Description& d = *ctx.d;
  for (u32 i = 0; i < d.walls.size(); ++i) {
    wall_face(ctx, i, 1.0);
    wall_face(ctx, i, -1.0);
    wall_ends(ctx, i);
  }
  for (const structure::Opening& o : d.openings)
    wall_opening(ctx, o);
  for (const structure::Roof& r : d.roofs)
    mesh_roof(ctx, r);
  for (const structure::Chimney& c : d.chimneys)
    mesh_chimney(ctx, c);
}

}  // namespace engine::structures::detail
