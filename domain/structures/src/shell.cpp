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
    g.sl[0] = g.sr[0] = 0.0;
    g.sl[1] = g.sr[1] = g.length;
  }
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
      f64 lambda[2] = {0, 0};  // the outgoing left side's, the outgoing right side's
      if (first >= 0) {
        // A corner or a star (structures.md, "Joins"): each side's line runs to the facing side's
        // line of the neighbour on that side, so the two walls' faces meet in one edge.
        for (u32 k = 0; k < 2; ++k) {
          const u32 j = static_cast<u32>(k == 0 ? first : last);
          const IV v = k == 0 ? first_v : last_v;
          const f64 side = k == 0 ? 1.0 : -1.0;
          const f64 vl = std::sqrt(static_cast<f64>(dot(v, v)));
          const F2 vv{static_cast<f64>(v.x) / vl, static_cast<f64>(v.z) / vl};
          const f64 hj = out[j].half;
          const F2 b{-side * vv.z * hj, side * vv.x * hj};
          lambda[k] = meet(uu, g.half, side, b, vv, 3.0 * (g.half + hj));
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
      // The outgoing left is the wall's left at its `from` end and its right at its `to` end.
      if (e == 0) {
        g.sl[0] = lambda[0];
        g.sr[0] = lambda[1];
      } else {
        g.sr[1] = g.length - lambda[0];
        g.sl[1] = g.length - lambda[1];
      }
    }
  }
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
Joinery joinery_of(const structure::Wall& w, const structure::Opening& o) noexcept {
  const bool door = o.kind == structure::OpeningKind::door;
  const bool exterior = w.kind == structure::WallKind::exterior;
  const f64 h = static_cast<f64>(w.thickness) * 0.5;
  Joinery j;
  j.depth = door ? 10.0 : 7.0;
  j.width = door ? 6.0 : 5.0;
  const f64 set = exterior ? (door ? 6.0 : 8.0) : h - 0.5 * j.depth;
  j.set = std::max(1.0, std::min(set, 2.0 * h - j.depth - 1.0));
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

// A face's corners shared between its cells, so a face is one welded sheet.
struct FaceVerts {
  Vector<f64> s, y;
  Vector<u32> index;
  u32 get(MeshPart& part, const Frame2& frame, f64 q, Vec3 normal, f64 ss, f64 yy) {
    for (u32 i = 0; i < s.size(); ++i)
      if (s[i] == ss && y[i] == yy) return index[i];
    const u32 id = part.positions.size();
    part.positions.push_back(frame.at(ss, yy, q));
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
  const f64 s_begin = side > 0 ? g.sl[0] : g.sr[0];
  const f64 s_end = side > 0 ? g.sl[1] : g.sr[1];
  if (!(s_end - s_begin > k_eps)) return;
  const f64 q = side * g.half;
  const f64 elevation = storey.elevation;
  const f64 foot =
      outside && w.storey == 0 ? elevation - static_cast<f64>(d.foundation) : elevation;
  const f64 plinth = outside && w.storey == 0 ? static_cast<f64>(d.plinth_top) : foot;

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
  Vector<f64> tops;
  Vector<f64> levels;
  levels.push_back(foot);
  if (plinth > foot + k_eps) levels.push_back(plinth);
  for (const Cut& c : cuts) {
    levels.push_back(c.y0);
    levels.push_back(c.y1);
  }
  for (const f64 s : stations) {
    f64 x, z;
    ground(s, x, z);
    tops.push_back(wall_top(d, wall, x, z));
    levels.push_back(tops.back());
  }
  sort_unique(levels);

  const Vec3 normal{static_cast<f32>(side * g.frame.nx), 0.0f, static_cast<f32>(side * g.frame.nz)};
  const Surface face_surface = outside ? Surface::wall_outer : Surface::wall_inner;
  FaceVerts face_verts, plinth_verts;
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
      const bool is_plinth = outside && lb <= plinth + k_eps && plinth > foot + k_eps;
      MeshPart& part = ctx.part(is_plinth ? Surface::plinth : face_surface);
      FaceVerts& verts = is_plinth ? plinth_verts : face_verts;
      u32 ids[4];
      for (u32 c = 0; c < count; ++c)
        ids[c] = verts.get(part, g.frame, q, normal, ps[c], py[c]);
      // Counter-clockwise in (s, y) faces the wall's right; the left face is wound the other way.
      for (u32 c = 1; c + 1 < count; ++c) {
        part.indices.push_back(ids[0]);
        part.indices.push_back(side > 0 ? ids[c + 1] : ids[c]);
        part.indices.push_back(side > 0 ? ids[c] : ids[c + 1]);
      }
    }
  }
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
  {
    MeshPart& part = ctx.part(Surface::wall_inner);
    const Vec3 lo[4] = {g.frame.at(c.s0, c.y0, -h), g.frame.at(c.s0, c.y0, h),
                        g.frame.at(c.s0, c.y1, h), g.frame.at(c.s0, c.y1, -h)};
    const Vec3 hi[4] = {g.frame.at(c.s1, c.y0, -h), g.frame.at(c.s1, c.y0, h),
                        g.frame.at(c.s1, c.y1, h), g.frame.at(c.s1, c.y1, -h)};
    const Vec2 jamb[4] = {uv(-h, c.y0), uv(h, c.y0), uv(h, c.y1), uv(-h, c.y1)};
    polygon(part, lo, jamb, 4, g.frame.dir());
    polygon(part, hi, jamb, 4, g.frame.dir() * -1.0f);
    const Vec3 head[4] = {lo[3], lo[2], hi[2], hi[3]};
    const Vec3 sill[4] = {lo[0], lo[1], hi[1], hi[0]};
    const Vec2 flat[4] = {uv(c.s0, -h), uv(c.s0, h), uv(c.s1, h), uv(c.s1, -h)};
    polygon(part, head, flat, 4, Vec3{0, -1, 0});
    polygon(door ? ctx.part(Surface::floor) : part, sill, flat, 4, Vec3{0, 1, 0});
  }
  if (o.kind == structure::OpeningKind::doorway) return;
  // Depth `t` is from the outer face inward; an inner wall's frame sits in its middle.
  const f64 out_sign = out_sign_of(w);
  const f64 thick = 2.0 * h;
  MeshPart& timber = ctx.part(Surface::timber);
  const auto member = [&](f64 s0, f64 s1, f64 y0, f64 y1, f64 t0, f64 t1, u32 grain) {
    const f64 qa = out_sign * (h - t0), qb = out_sign * (h - t1);
    box(timber, g.frame, s0, s1, y0, y1, std::min(qa, qb), std::max(qa, qb), grain);
  };
  const Joinery joinery = joinery_of(w, o);
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
    if (exterior) member(c.s0 - 6.0, c.s1 + 6.0, c.y0 - 4.0, c.y0 + 1.0, -5.0, set + 1.0, 0);
  }
  // The lintel: a timber over the opening, bearing on the wall either side, proud of the face.
  if (exterior)
    member(c.s0 - 15.0, c.s1 + 15.0, c.y1 - 1.0, c.y1 + 13.0, -1.5, std::min(12.0, thick - 2.0), 0);
  // The leaves (structures.md, "Leaves and mullions"): upright boards of unequal widths, every
  // other one a few millimetres proud so the joints read, on ledges across the face the leaf
  // opens toward: three to a door, two to a shutter. Each ledge is sunk a centimetre into its
  // boards.
  LeafPlace places[2];
  const u32 leaves = leaf_places(d, o, places);
  for (u32 l = 0; l < leaves; ++l) {
    const LeafPlace& p = places[l];
    Frame3 frame;
    frame.o[0] = p.px, frame.o[1] = 0.0, frame.o[2] = p.pz;
    frame.axis[0][0] = p.ux, frame.axis[0][1] = 0.0, frame.axis[0][2] = p.uz;
    frame.axis[1][0] = 0.0, frame.axis[1][1] = 1.0, frame.axis[1][2] = 0.0;
    frame.axis[2][0] = p.vx, frame.axis[2][1] = 0.0, frame.axis[2][2] = p.vz;
    const u32 boards =
        std::min(8u, std::max(1u, static_cast<u32>(p.width / k_leaf_board_cm + 0.5)));
    const f64 share[4] = {1.12, 0.92, 1.06, 0.90};
    f64 total = 0.0;
    for (u32 b = 0; b < boards; ++b)
      total += share[b % 4];
    f64 at = 0.0;
    for (u32 b = 0; b < boards; ++b) {
      const f64 wide = p.width * share[b % 4] / total;
      const f64 proud = b % 2 == 1 ? 0.3 : 0.0;
      box3(timber, frame, at, at + wide, p.y0, p.y1, p.ledges, p.ledges + p.boards + proud, 1);
      at += wide;
    }
    const f64 tall = p.y1 - p.y0;
    const u32 ledges = tall > 120.0 ? 3u : 2u;
    const f64 ledge = p.door ? 15.0 : 9.0;
    const f64 inset = std::min(20.0, tall * 0.12);
    for (u32 i = 0; i < ledges; ++i) {
      const f64 y =
          p.y0 + inset +
          (tall - 2.0 * inset - ledge) * static_cast<f64>(i) / static_cast<f64>(ledges - 1);
      box3(timber, frame, 2.0, p.width - 2.0, y, y + ledge, 0.0, p.ledges + 1.0, 0);
    }
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
    f64 pr[8], ph[8];
    u32 count = 0;
    const auto add = [&](f64 run_at, f64 height) {
      pr[count] = run_at, ph[count] = height, ++count;
    };
    add(0.0, 0.0);
    if (soft) {
      const f64 turn = std::min(0.6 * vertical, run * 0.2);
      add(0.0, vertical * 0.55);
      add(turn * 0.25, vertical * 0.82);
      add(turn, vertical);
      if (gable && run > 120.0) {
        add(run - 45.0, vertical);
        add(run - 38.0, vertical + 6.0);  // the ridge course's lower edge, standing proud
      }
      add(run, gable ? vertical + (run > 120.0 ? 8.0 : 0.0) : vertical);
    } else {
      add(0.0, vertical);
      add(run, vertical);
    }
    const auto at_c = [&](u32 i) { return ce - out * pr[i]; };
    const auto at_y = [&](u32 i) { return ye + p * pr[i] + ph[i]; };
    // A segment's outward normal in (a, c, y), and the length it adds to the surface.
    Vec3 seg_n[8];
    f64 seg_len[8];
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
      const Vec3 n0 = soft && i > 0 ? normalize(seg_n[i - 1] + seg_n[i]) : seg_n[i];
      const Vec3 n1 = !soft ? seg_n[i]
                            : (i + 2 < count ? normalize(seg_n[i] + seg_n[i + 1])
                                             : (gable ? normalize(seg_n[i] + up) : seg_n[i]));
      const Vec3 normals[4] = {n0, n0, n1, n1};
      polygon_smooth(cover, points, uvs, normals, 4, seg_n[i]);
      if (pr[i + 1] - pr[i] < k_eps) continue;
      for (u32 e = 0; e < 2; ++e) {
        // The verge: the coat's cut end over the gable, a strip a segment.
        const f64 a = e == 0 ? a0 : a1;
        const f64 u0 = pr[i] * len, u1 = pr[i + 1] * len;
        const Vec3 end[4] = {point(a, at_c(i), ye + p * pr[i]),
                             point(a, at_c(i + 1), ye + p * pr[i + 1]),
                             point(a, at_c(i + 1), at_y(i + 1)), point(a, at_c(i), at_y(i))};
        const Vec2 end_uv[4] = {uv(u0, 0.0), uv(u1, 0.0), uv(u1, ph[i + 1]), uv(u0, ph[i])};
        polygon(cover, end, end_uv, 4, vec(e == 0 ? -1.0 : 1.0, 0.0, 0.0));
      }
    }
    {
      const Vec3 points[4] = {point(a0, ce, ye), point(a1, ce, ye), point(a1, cr, yr),
                              point(a0, cr, yr)};
      const Vec2 uvs[4] = {uv(0.0, a0), uv(0.0, a1), uv(run * len, a1), uv(run * len, a0)};
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
  const Joinery j = joinery_of(w, o);
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
