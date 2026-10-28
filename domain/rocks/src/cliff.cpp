// The cliff solver (rocks.h; docs/subsystems/rocks.md, "Cliffs").
//
// A face is a **box of quads** bent along a line on the ground: lattice (i, j, k) is station i
// along the line, row j up the face and layer k from the buried back (k = 0) to the carved front (k
// = nd). The six faces of the box share their border points by lattice key, exactly as the
// cube-sphere's charts share theirs, so the box is closed and two-manifold whatever the carving
// does; and the lattice's axes map to (along, up, out), a right-handed frame, so a face of the box
// wound outwards in the lattice is wound outwards in the world. What carves the front is a function
// of (s, y) — the distance along the line and the height — so a joint, a bed and a ledge run across
// the face as planes would, and the front only ever moves along the line's horizontal normal:
// the sheet cannot fold over itself, which is what lets a ledge and an undercut be one row of quads
// stretched outwards instead of a remesh. The ends are ladders between the back and the front at
// each row; with the front held half a metre in front of the back, every rung's two triangles are
// wound the same way (rocks.md has the arithmetic). The talus skirt is a second box of the same
// kind under the face's foot, and the fallen blocks on it are the scree's fragments (solver.cpp).
//
// f64 with IEEE `+ - * /`, `sqrt`, `floor` and `det::sin`/`det::cos` only, contraction off
// (ADR-0035), one thread, one seeded stream per purpose: the same parameters give the same bytes on
// every toolchain.
#include "detail.h"
#include "geology.h"

#include <core/containers/hash_map.h>
#include <core/math/deterministic.h>
#include <domain/rocks/rocks.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace engine::rocks {

namespace {

using namespace detail;

// ---- the model by type (rocks.md, "Cliffs") ----------------------------------------------------

struct CliffModel {
  f64 relief;         // how far a block may stand back from the face, m
  f64 bed_m;          // the beds' thickness, 0 for an unbedded rock
  f64 h_spacing;      // the horizontal joints' spacing (sheeting, flow tops); 0: the beds are it
  f64 v_spacing;      // the vertical joints' across the face (basalt's: its columns)
  f64 d_spacing;      // the dipping set's
  f64 d_dip_deg;      // and its dip
  f64 rounding;       // the default `rounding`
  f64 warp;           // the low warp's amplitude, a share of the face's height
  f64 grain;          // the grain's first octave's amplitude, m
  f64 column_relief;  // a prism's ridge, m (basalt)
  bool pitted;        // the grain cuts pits (limestone)
};

CliffModel cliff_model(RockType type) {
  switch (type) {
    case RockType::Sandstone:
      return {3.5, 2.5, 0.0, 10.0, 14.0, 70.0, 0.4, 0.025, 0.06, 0.0, false};
    case RockType::Basalt: return {2.0, 0.0, 12.0, 1.4, 2.6, 80.0, 0.1, 0.02, 0.04, 0.35, false};
    case RockType::Limestone: return {4.5, 4.0, 0.0, 9.0, 12.0, 60.0, 0.6, 0.03, 0.12, 0.0, true};
    case RockType::Shale: return {2.5, 0.4, 0.0, 3.0, 6.0, 65.0, 0.15, 0.035, 0.05, 0.0, false};
    case RockType::Slate: return {2.0, 0.55, 0.0, 4.0, 7.0, 70.0, 0.08, 0.02, 0.02, 0.0, false};
    case RockType::Schist: return {3.0, 0.7, 0.0, 5.0, 8.0, 60.0, 0.18, 0.04, 0.06, 0.0, false};
    case RockType::Gneiss: return {4.0, 1.2, 0.0, 7.0, 10.0, 55.0, 0.25, 0.04, 0.06, 0.0, false};
    case RockType::Marble: return {3.5, 0.0, 5.0, 6.0, 10.0, 60.0, 0.4, 0.025, 0.05, 0.0, true};
    case RockType::Obsidian: return {3.0, 0.0, 3.0, 4.0, 6.0, 65.0, 0.05, 0.04, 0.01, 0.0, false};
    case RockType::Granite:
    default: return {4.0, 0.0, 6.0, 7.0, 9.0, 55.0, 0.35, 0.03, 0.10, 0.0, false};
  }
}

f64 smoothstep(f64 t) {
  const f64 u = clamp01(t);
  return u * u * (3.0 - 2.0 * u);
}
// A feature `wavelength` metres across is carried in full at four cells of the rung and not at all
// at two, so a coarse rung is the same face with what it cannot carry left out, never aliased.
f64 fade_for(f64 wavelength, f64 cell) { return clamp01((wavelength / cell - 2.0) * 0.5); }
f64 min2(f64 a, f64 b) { return a < b ? a : b; }
f64 max2(f64 a, f64 b) { return a > b ? a : b; }

// ---- the line --------------------------------------------------------------------------------

struct Line {
  Vector<D3> foot;  // x, y (the ground), z
  Vector<f64> s;    // the distance along the line in the ground's plane, at each vertex
  Vector<f64> height;
  Vector<f64> batter;
  f64 length = 0.0;
  f64 height_max = 0.0;
};

struct Frame {
  D3 foot;
  D3 n;  // the face's horizontal normal: the line's right
  f64 s = 0.0;
  f64 height = 0.0;
  f64 batter = 0.0;
};

void line_at(const Line& line, f64 q, D3& foot, f64& height, f64& batter) {
  const f64 at = q < 0.0 ? 0.0 : (q > line.length ? line.length : q);
  u32 lo = 0;
  u32 hi = line.s.size() - 1;
  while (hi - lo > 1) {
    const u32 mid = (lo + hi) / 2;
    if (line.s[mid] <= at) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const f64 span = line.s[hi] - line.s[lo];
  const f64 t = span > 0.0 ? (at - line.s[lo]) / span : 0.0;
  foot = line.foot[lo] + (line.foot[hi] - line.foot[lo]) * t;
  height = lerp(line.height[lo], line.height[hi], t);
  batter = lerp(line.batter[lo], line.batter[hi], t);
}

// The frame at `s`: the foot there, and the normal of the chord `window` either side, so the
// normal turns smoothly round a vertex of the line and a buried back `depth` behind the face does
// not fold where the line bends (the window is half the depth).
Frame frame_at(const Line& line, f64 s, f64 window) {
  Frame f;
  f.s = s;
  line_at(line, s, f.foot, f.height, f.batter);
  D3 a;
  D3 b;
  f64 unused_h = 0.0;
  f64 unused_b = 0.0;
  line_at(line, s - window, a, unused_h, unused_b);
  line_at(line, s + window, b, unused_h, unused_b);
  const D3 t = normalized(D3{b.x - a.x, 0.0, b.z - a.z});
  f.n = D3{-t.z, 0.0, t.x};
  return f;
}

// ---- the carving (rocks.md, "Cliffs") ----------------------------------------------------------

struct Carve {
  CliffModel m;
  Fabric fabric;
  u32 sets = 0;
  f64 relief = 0.0;
  f64 rounding = 0.0;
  f64 bed = 0.0;
  f64 undercut = 0.0;
  f64 cell = 1.0;
  f64 datum = 0.0;
  f64 h_spacing = 0.0;  // the horizontal set's: the beds' when the rock is bedded
  f64 d_cos = 0.0;
  f64 d_sin = 1.0;
  f64 warp_amp = 0.0;
  f64 warp_len = 1.0;
  f64 gully_amp = 0.0;  // the gullies between buttresses: how deep, m
  f64 gully_len = 1.0;
  f64 fade_blocks = 0.0;
  f64 fade_beds = 0.0;
  f64 fade_columns = 0.0;
  u64 seed_blocks = 0;
  u64 seed_beds = 0;
  u64 seed_warp = 0;
  u64 seed_grain = 0;
  u64 seed_wave = 0;
  u64 seed_gully = 0;
};

f64 own_extent(const Carve& c, i64 kv, i64 kh, i64 kd) {
  const f64 u = lattice_value(c.seed_blocks, kv, kh, kd);
  return -c.relief * u;
}

// Which blocks a coordinate `b` (in spacings) sees and how much of each: a single block inside,
// and across a joint the two either side blended over `w` spacings each way, so a joint's edge is
// rounded by the weathering rather than drawn as a crack.
struct Pick {
  i64 k0 = 0;
  i64 k1 = 0;
  f64 t = 0.0;
};
Pick pick(f64 b, f64 w) {
  const f64 lo = std::floor(b - w);
  const f64 hi = std::floor(b + w);
  if (lo == hi) return Pick{static_cast<i64>(lo), static_cast<i64>(lo), 0.0};
  return Pick{static_cast<i64>(lo), static_cast<i64>(hi), smoothstep((b - (hi - w)) / (2.0 * w))};
}

// How far the face stands back from its mean plane at (s, y) — never in front of it but by the
// grain — with `top_y` the face's top there for the crest's rounding; and, apart, how deep the
// undercut notches it there, which the caller takes off after the face is held up (below).
f64 carve_at(const Carve& c, f64 s, f64 y, f64 top_y, f64& notch) {
  notch = 0.0;
  const f64 yd = fabric_coordinate(c.fabric, D3{s, y - c.datum, 0.0});
  f64 e = -fabric_recess(c.fabric, D3{s, y - c.datum, 0.0}, c.cell);
  // The beds' coordinate, wavy along the face as real beds are; the horizontal joints of a bedded
  // rock are its bedding planes, so the blocks' rows and the beds are one coordinate.
  f64 tb = 0.0;
  if (c.bed > 0.0) {
    tb = yd / c.bed + 0.2 * (noise3(c.seed_wave + 2u, D3{s / (8.0 * c.bed), 2.5, 2.5}) - 0.5);
  }
  if (c.sets > 0 && c.fade_blocks > 0.0) {
    const f64 bh =
        c.m.h_spacing > 0.0
            ? yd / c.h_spacing +
                  0.15 * (noise3(c.seed_wave, D3{s / (4.0 * c.h_spacing), 0.5, 0.5}) - 0.5)
            : tb;
    const f64 bv = c.sets >= 2
                       ? s / c.m.v_spacing +
                             0.4 * (noise3(c.seed_wave + 1u, D3{s / (3.0 * c.m.v_spacing),
                                                                yd / (3.0 * c.m.v_spacing), 1.5}) -
                                    0.5)
                       : 0.0;
    const f64 bd = c.sets >= 3 ? (s * c.d_cos + yd * c.d_sin) / c.m.d_spacing : 0.0;
    const f64 wh = max2(0.02, min2(0.2, c.rounding * 0.1));
    const f64 wv = max2(0.02, min2(0.3, c.rounding * 0.15));
    const Pick ph = pick(bh, wh);
    const Pick pv = c.sets >= 2 ? pick(bv, wv) : Pick{};
    const Pick pd = c.sets >= 3 ? pick(bd, wv) : Pick{};
    f64 value = 0.0;
    for (u32 corner = 0; corner < 8; ++corner) {
      const bool uh = (corner & 1u) != 0;
      const bool uv = (corner & 2u) != 0;
      const bool ud = (corner & 4u) != 0;
      const f64 w = (uh ? ph.t : 1.0 - ph.t) * (uv ? pv.t : 1.0 - pv.t) * (ud ? pd.t : 1.0 - pd.t);
      if (w <= 0.0) continue;
      value += w * own_extent(c, uv ? pv.k1 : pv.k0, uh ? ph.k1 : ph.k0, ud ? pd.k1 : pd.k0);
    }
    const f64 mean = -c.relief * 0.5;  // a block's extent is -relief u, u uniform
    e += mean + (value - mean) * c.fade_blocks;
    // Basalt's columns: each a prism whose faces meet in a ridge mid-column, the joints recessed.
    if (c.m.column_relief > 0.0 && c.sets >= 2 && c.fade_columns > 0.0) {
      const f64 f = bv - std::floor(bv);
      e -= c.m.column_relief * std::fabs(2.0 * f - 1.0) * c.fade_columns;
    }
  }
  if (c.bed > 0.0 && c.fade_beds > 0.0) {
    const f64 kb = std::floor(tb);
    const f64 f = tb - kb;
    // A groove at every bedding plane, sharp at the plane and nothing mid-bed, as the boulders'.
    f64 g = (2.0 * f - 1.0) * (2.0 * f - 1.0);
    g = g * g;
    g = g * g;
    const f64 hard = lattice_value(c.seed_beds, static_cast<i64>(kb), 3, 7);
    e -= 0.12 * c.bed * (0.35 + 0.65 * hard) * g * c.fade_beds;
    // **The undercut**: a weak bed is notched deepest just under the hard bed above it, which
    // returns to the face within the bed's top eighth — the overhang's underside.
    if (c.undercut > 0.0 && hard < 0.45) {
      const f64 weak = (0.45 - hard) / 0.45;
      const f64 profile = f < 0.88 ? (f / 0.88) * (f / 0.88) : (1.0 - f) / 0.12;
      notch = c.undercut * c.bed * (0.4 + 0.6 * weak) * profile * c.fade_beds;
    }
  }
  // The warp: one low octave over the face, the face's shape at every rung.
  e -= c.warp_amp * noise3(c.seed_warp, D3{s / c.warp_len, yd / c.warp_len, 0.25});
  // The gullies: where the joints let water in, the face is cut back between buttresses — a
  // noise along the face that varies slowly up it, taken only above its middle so the gullies
  // are separate rather than a corrugation.
  if (c.gully_amp > 0.0) {
    const f64 g = noise3(c.seed_gully, D3{s / c.gully_len, yd / (4.0 * c.gully_len), 0.4});
    e -= c.gully_amp * smoothstep((g - 0.45) * 3.0);
  }
  // The grain: up to three octaves from 3 m, each only where it spans four cells of the rung.
  f64 len = 3.0;
  f64 amp = c.m.grain;
  for (u32 o = 0; o < 3 && len >= 4.0 * c.cell; ++o) {
    const f64 v = noise3(c.seed_grain + o, D3{s / len, yd / len, 0.75 + static_cast<f64>(o)});
    e += c.m.pitted ? -amp * std::fabs(2.0 * v - 1.0) : amp * (2.0 * v - 1.0);
    len *= 0.5;
    amp *= 0.5;
  }
  // The crest, rounded back by the weathering.
  const f64 rc = 1.0 + 3.0 * c.rounding;
  const f64 below_top = top_y - y;
  if (below_top < rc) {
    const f64 q = 1.0 - below_top / rc;
    e -= c.rounding * rc * 0.6 * q * q;
  }
  return e;
}

// ---- the box emitter ---------------------------------------------------------------------------

// Charts of whole repeats: a box face's UVs are its two in-plane coordinates in metres over the
// set's repeat (`tile_m`), cut into charts `k_chart` repeats square, each moved by a whole number
// of charts to sit round zero — so a tiled set samples every chart from the same phase, a chart's
// seam is invisible, and no coordinate passes ±16 while a cell is under four repeats (the content
// build warns past that, `geometry.uv_range`).
constexpr f64 k_chart = 24.0;

u64 lattice_key(u32 i, u32 j, u32 k) {
  return (static_cast<u64>(i) << 42) | (static_cast<u64>(j) << 21) | static_cast<u64>(k);
}

struct ChartUv {
  f64 a = 0.0;
  f64 b = 0.0;
};

// Emits one closed box: `n` cells along (i, j, k), `pos(i, j, k)` the lattice point's place and
// `uv(axis, i, j, k, place)` its two chart coordinates in metres on the faces across `axis`.
template <class Pos, class Uv>
void emit_box(const u32 n[3], const Pos& pos, const Uv& uv, f64 tile_m, assets::MeshData& out) {
  struct Face {
    u32 axis;
    bool plus;
    u32 b;
    u32 c;
  };
  // (axis, side) with in-face axes (b, c) such that b x c points outwards, as the cube-sphere's.
  constexpr Face k_faces[6] = {{0, true, 1, 2},  {0, false, 2, 1}, {1, true, 2, 0},
                               {1, false, 0, 2}, {2, true, 0, 1},  {2, false, 1, 0}};
  const u32 first = out.positions.size();
  const u32 first_index = out.indices.size();
  Vector<D3> place;     // by lattice point
  Vector<u32> slot_of;  // per emitted vertex, its lattice point
  HashMap<u64, u32> border;
  for (const Face& face : k_faces) {
    const u32 nb = n[face.b];
    const u32 nc = n[face.c];
    const u32 side = nb + 1;
    const auto lattice = [&](u32 p, u32 q, u32 c[3]) {
      c[face.axis] = face.plus ? n[face.axis] : 0u;
      c[face.b] = p;
      c[face.c] = q;
    };
    // The face's points, its border shared with its neighbours by lattice key.
    Vector<u32> slots(side * (nc + 1), ~0u);
    for (u32 q = 0; q <= nc; ++q) {
      for (u32 p = 0; p <= nb; ++p) {
        u32 c[3];
        lattice(p, q, c);
        if (p == 0 || p == nb || q == 0 || q == nc) {
          const u64 key = lattice_key(c[0], c[1], c[2]);
          const u32* found = border.find_value(key);
          if (found != nullptr) {
            slots[q * side + p] = *found;
          } else {
            border[key] = place.size();
            slots[q * side + p] = place.size();
            place.push_back(pos(c[0], c[1], c[2]));
          }
        } else {
          slots[q * side + p] = place.size();
          place.push_back(pos(c[0], c[1], c[2]));
        }
      }
    }
    // Each cell's chart, from its first corner; the cells sorted by chart, then in grid order.
    const auto chart_of = [&](u32 p, u32 q, i64& cu, i64& cv) {
      u32 c[3];
      lattice(p, q, c);
      const ChartUv m = uv(face.axis, c[0], c[1], c[2], place[slots[q * side + p]]);
      cu = static_cast<i64>(std::floor((m.a / tile_m + k_chart * 0.5) / k_chart));
      cv = static_cast<i64>(std::floor((m.b / tile_m + k_chart * 0.5) / k_chart));
    };
    Vector<u64> order;
    order.reserve(nb * nc);
    for (u32 q = 0; q < nc; ++q) {
      for (u32 p = 0; p < nb; ++p) {
        i64 cu = 0;
        i64 cv = 0;
        chart_of(p, q, cu, cv);
        const u64 chart = (static_cast<u64>(cu + 32768) << 16) | static_cast<u64>(cv + 32768);
        order.push_back((chart << 32) | (q * nb + p));
      }
    }
    std::sort(order.begin(), order.end());
    Vector<u32> vertex(side * (nc + 1), ~0u);
    Vector<u32> touched;
    u32 at = 0;
    while (at < order.size()) {
      const u64 chart = order[at] >> 32;
      const f64 shift_a = (static_cast<f64>(static_cast<i64>(chart >> 16) - 32768)) * k_chart;
      const f64 shift_b = (static_cast<f64>(static_cast<i64>(chart & 0xffffu) - 32768)) * k_chart;
      const auto vertex_of = [&](u32 p, u32 q) {
        const u32 point = q * side + p;
        if (vertex[point] == ~0u) {
          u32 c[3];
          lattice(p, q, c);
          const ChartUv m = uv(face.axis, c[0], c[1], c[2], place[slots[point]]);
          vertex[point] = out.positions.size();
          touched.push_back(point);
          out.positions.push_back(Vec3{});
          out.normals.push_back(Vec3{});
          out.uvs.push_back(Vec2{static_cast<f32>(m.a / tile_m - shift_a),
                                 static_cast<f32>(m.b / tile_m - shift_b)});
          slot_of.push_back(slots[point]);
        }
        return vertex[point];
      };
      for (; at < order.size() && (order[at] >> 32) == chart; ++at) {
        const u32 cell = static_cast<u32>(order[at] & 0xffffffffu);
        const u32 p = cell % nb;
        const u32 q = cell / nb;
        const u32 a = vertex_of(p, q);
        const u32 bq = vertex_of(p + 1, q);
        const u32 cq = vertex_of(p + 1, q + 1);
        const u32 d = vertex_of(p, q + 1);
        // Alternate the diagonal in a checkerboard, so no direction across a face is favoured.
        if (((p + q) & 1u) == 0u) {
          out.indices.push_back(a);
          out.indices.push_back(bq);
          out.indices.push_back(cq);
          out.indices.push_back(a);
          out.indices.push_back(cq);
          out.indices.push_back(d);
        } else {
          out.indices.push_back(a);
          out.indices.push_back(bq);
          out.indices.push_back(d);
          out.indices.push_back(bq);
          out.indices.push_back(cq);
          out.indices.push_back(d);
        }
      }
      for (const u32 point : touched)
        vertex[point] = ~0u;
      touched.clear();
    }
  }
  // Normals over the welded box, so a chart's seam is invisible in the shading, each triangle's
  // weighted at a corner by the sine of its angle there rather than by its area: a box's creases
  // are sharp (the crest, a ledge, the skirt's wedge inside the face) and its quads long where a
  // ledge stretches them, and an area weight lets the large quad on one side of a crease turn the
  // corner's normal past the small one's on the other. The sine is the cross product over the two
  // edges' lengths: `sqrt` and arithmetic, no `acos`, so it is the same bits everywhere.
  Vector<D3> normal(place.size());
  for (u32 t = first_index; t < out.indices.size(); t += 3) {
    const u32 corner[3] = {slot_of[out.indices[t] - first], slot_of[out.indices[t + 1] - first],
                           slot_of[out.indices[t + 2] - first]};
    const D3 fn = cross(place[corner[1]] - place[corner[0]], place[corner[2]] - place[corner[0]]);
    for (u32 e = 0; e < 3; ++e) {
      const D3 p = place[corner[e]];
      const f64 l1 = length(place[corner[(e + 1) % 3]] - p);
      const f64 l2 = length(place[corner[(e + 2) % 3]] - p);
      const f64 w = l1 * l2 > 0.0 ? 1.0 / (l1 * l2) : 0.0;
      normal[corner[e]] = normal[corner[e]] + fn * w;
    }
  }
  for (u32 v = first; v < out.positions.size(); ++v) {
    const u32 s = slot_of[v - first];
    const D3 p = place[s];
    const D3 nn = normalized(normal[s]);
    out.positions[v] = Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
    out.normals[v] = Vec3{static_cast<f32>(nn.x), static_cast<f32>(nn.y), static_cast<f32>(nn.z)};
  }
  // Strong folded beds can put the smooth normal behind a small adjacent face. Keep that
  // fracture sharp; copying positions/UVs leaves the welded shell and triangle budget intact.
  for (u32 t = first_index; t < out.indices.size(); t += 3) {
    const Vec3 a = out.positions[out.indices[t]];
    const Vec3 fn =
        cross(out.positions[out.indices[t + 1]] - a, out.positions[out.indices[t + 2]] - a);
    const Vec3 vn = out.normals[out.indices[t]] + out.normals[out.indices[t + 1]] +
                    out.normals[out.indices[t + 2]];
    if (dot(fn, vn) > 0.65f * std::sqrt(dot(fn, fn) * dot(vn, vn))) continue;
    const D3 nn = normalized(D3{wide(fn.x), wide(fn.y), wide(fn.z)});
    for (u32 corner = 0; corner < 3; ++corner) {
      const u32 old = out.indices[t + corner];
      const Vec3 position = out.positions[old];
      const Vec2 chart_uv = out.uvs[old];
      out.indices[t + corner] = out.positions.size();
      out.positions.push_back(position);
      out.uvs.push_back(chart_uv);
      out.normals.push_back(
          Vec3{static_cast<f32>(nn.x), static_cast<f32>(nn.y), static_cast<f32>(nn.z)});
    }
  }
}

u64 box_triangles(u64 a, u64 b, u64 c) { return 4u * (a * b + a * c + b * c); }

u32 cells_over(f64 metres, f64 cell, u32 at_least) {
  const f64 n = std::ceil(metres / cell - 1e-9);
  const u32 v = n > 1.0e6 ? 1000000u : static_cast<u32>(n);
  return v < at_least ? at_least : v;
}

// ---- the face and its skirt --------------------------------------------------------------------

struct SkirtStation {
  Frame frame;
  f64 x_toe = 0.0;  // offsets along the frame's normal, from the line
  f64 y_toe = 0.0;
  f64 x_top = 0.0;
  f64 y_top = 0.0;
  f64 x_ground = 0.0;  // where the skirt's surface meets the foot's height
  f64 skirt_h = 0.0;
};

struct Solved {
  Line line;
  CliffModel model;
  f64 tan_a = 0.7;
  f64 sin_a = 0.57;
  f64 depth = 4.0;
  f64 window = 2.0;
  f64 buried = 0.0;
  f64 talus = 0.0;
  f64 ramp = 20.0;
  u64 seed_skyline = 0;
};

SkirtStation skirt_station(const Solved& z, f64 s) {
  SkirtStation st;
  st.frame = frame_at(z.line, s, z.window);
  const Frame& f = st.frame;
  const f64 end = min2(s, z.line.length - s);
  // The skirt is heaped where more has fallen: its height wanders along the face about the share.
  const f64 heap = 0.55 + 0.9 * noise3(z.seed_skyline + 7u, D3{s / 18.0, 0.5, 0.5});
  const f64 ht = min2(0.7 * f.height, z.talus * f.height * heap * smoothstep(end / z.ramp));
  const f64 foot = f.foot.y;
  const f64 x0 = -f.batter * ht;
  st.skirt_h = ht;
  st.y_toe = foot - 0.5 * z.buried - 0.3;
  st.x_toe = x0 + (foot + ht - st.y_toe) / z.tan_a;
  st.x_ground = x0 + ht / z.tan_a;
  // Its top runs on up the slope into the face, behind the deepest carving, under the crest.
  f64 y_top = foot + ht + (z.depth - 1.0) * z.tan_a;
  if (y_top > foot + f.height - 0.5) y_top = foot + f.height - 0.5;
  if (y_top < foot + ht + 0.25) y_top = foot + ht + 0.25;
  st.y_top = y_top;
  st.x_top = x0 - (y_top - foot - ht) / z.tan_a;
  return st;
}

}  // namespace

bool cliff_in_range(const Cliff& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.rocks.Cliff.") + field + " is out of range (" + range + ")";
    return false;
  };
  const auto in = [](f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; };
  if (!valid_rock_type(p.type)) return bad("type", "known rock type");
  if (!valid_quality(p.quality)) return bad("quality", "Low, Medium, High");
  if (!geology_in_range(p.geology, error) || !material_factors_in_range(p.material, error))
    return false;
  if (p.points.size() < 2 || p.points.size() > 4096) return bad("points", "2 to 4,096 vertices");
  f64 length = 0.0;
  for (u32 i = 0; i < p.points.size(); ++i) {
    const CliffPoint& q = p.points[i];
    if (!in(wide(q.x), -1.0e5, 1.0e5) || !in(wide(q.y), -1.0e5, 1.0e5) ||
        !in(wide(q.z), -1.0e5, 1.0e5))
      return bad("points", "each coordinate within 100 km of the origin");
    if (q.height.has_value() && !in(wide(*q.height), 1.0, 200.0))
      return bad("points.height", "1 to 200 m");
    if (q.batter.has_value() && !in(wide(*q.batter), 0.0, 1.5))
      return bad("points.batter", "0 to 1.5");
    if (i > 0) {
      const f64 dx = wide(q.x) - wide(p.points[i - 1].x);
      const f64 dz = wide(q.z) - wide(p.points[i - 1].z);
      const f64 d = std::sqrt(dx * dx + dz * dz);
      if (!(d >= 0.05)) return bad("points", "consecutive vertices at least 5 cm apart");
      length += d;
    }
  }
  if (!in(length, 1.0, 1000.001)) return bad("points", "a line 1 to 1,000 m long");
  if (!in(wide(p.height), 1.0, 200.0)) return bad("height", "1 to 200 m");
  if (!in(wide(p.batter), 0.0, 1.5)) return bad("batter", "0 to 1.5");
  if (!in(wide(p.buried), 0.0, 20.0)) return bad("buried", "0 to 20 m");
  if (p.depth.has_value() && !in(wide(*p.depth), 2.0, 200.0)) return bad("depth", "2 to 200 m");
  if (p.relief.has_value() && !in(wide(*p.relief), 0.0, 20.0)) return bad("relief", "0 to 20 m");
  if (p.joint_sets > 3) return bad("joint_sets", "0 to 3");
  if (p.rounding.has_value() && !in(wide(*p.rounding), 0.0, 1.0)) return bad("rounding", "0 to 1");
  if (p.bed_m.has_value() && !(wide(*p.bed_m) == 0.0 || in(wide(*p.bed_m), 0.25, 50.0)))
    return bad("bed_m", "0, or 0.25 to 50 m");
  if (!in(wide(p.undercut), 0.0, 1.0)) return bad("undercut", "0 to 1");
  if (!in(wide(p.talus), 0.0, 0.6)) return bad("talus", "0 to 0.6");
  if (!in(wide(p.talus_angle_deg), 20.0, 45.0)) return bad("talus_angle_deg", "20 to 45");
  if (p.talus_blocks > 256) return bad("talus_blocks", "0 to 256");
  if (!in(wide(p.talus_block_m), 0.1, 10.0)) return bad("talus_block_m", "0.1 to 10 m");
  if (!in(wide(p.datum_y), -1.0e5, 1.0e5)) return bad("datum_y", "within 100 km");
  if (p.triangle_budget < 2000 || p.triangle_budget > 4000000)
    return bad("triangle_budget", "2,000 to 4,000,000");
  if (!in(wide(p.material.tile_m), 0.05, 20.0)) return bad("material.tile_m", "0.05 to 20 m");
  if (!in(wide(p.material.normal_scale), 0.0, 4.0)) return bad("material.normal_scale", "0 to 4");
  return true;
}

bool solve_cliff(const Cliff& p, assets::MeshData& out, CliffReport* report_out,
                 std::string* error_out) {
  out = assets::MeshData{};
  std::string error;
  const auto fail = [&](std::string sentence) {
    if (error_out != nullptr) *error_out = std::move(sentence);
    return false;
  };
  if (!cliff_in_range(p, error)) return fail(error);
  const u32 budget = quality_budget(p.triangle_budget, p.quality, 2000u);

  Solved z;
  z.model = cliff_model(p.type);
  const CliffModel& m = z.model;
  // The line, with each vertex's height and batter.
  for (u32 i = 0; i < p.points.size(); ++i) {
    const CliffPoint& q = p.points[i];
    z.line.foot.push_back(D3{wide(q.x), wide(q.y), wide(q.z)});
    z.line.height.push_back(wide(q.height.value_or(p.height)));
    z.line.batter.push_back(wide(q.batter.value_or(p.batter)));
    if (i == 0) {
      z.line.s.push_back(0.0);
    } else {
      const D3 d = z.line.foot[i] - z.line.foot[i - 1];
      z.line.s.push_back(z.line.s[i - 1] + std::sqrt(d.x * d.x + d.z * d.z));
    }
    z.line.height_max = max2(z.line.height_max, z.line.height[i]);
  }
  z.line.length = z.line.s[z.line.s.size() - 1];

  // The carving's parameters, the rung's fades set below.
  Carve carve;
  carve.fabric = make_fabric(p.type, p.geology, p.seed);
  carve.m = m;
  carve.sets = p.joint_sets;
  carve.relief = p.relief.has_value() ? wide(*p.relief) : m.relief;
  carve.rounding = (p.rounding.has_value() ? wide(*p.rounding) : m.rounding) *
                   (0.2 + 1.6 * carve.fabric.exposure);
  carve.bed = p.bed_m.has_value()
                  ? wide(*p.bed_m)
                  : (p.geology.bed_m.has_value() ? wide(*p.geology.bed_m) : m.bed_m);
  carve.undercut = wide(p.undercut);
  carve.datum = wide(p.datum_y);
  carve.h_spacing = m.h_spacing > 0.0 ? m.h_spacing : (carve.bed > 0.0 ? carve.bed : 3.0);
  carve.d_cos = det::cos(m.d_dip_deg * k_pi / 180.0);
  carve.d_sin = det::sin(m.d_dip_deg * k_pi / 180.0);
  carve.warp_amp = m.warp * z.line.height_max;
  carve.gully_amp = min2(6.0, 0.06 * z.line.height_max);
  carve.gully_len = max2(12.0, 0.6 * z.line.height_max);
  carve.warp_len = max2(16.0, 0.6 * z.line.height_max);
  {
    Rng seeds(mix64(p.seed ^ 0x434C494646ull));  // "CLIFF"
    carve.seed_blocks = seeds.next();
    carve.seed_beds = seeds.next();
    carve.seed_warp = seeds.next();
    carve.seed_grain = seeds.next();
    carve.seed_wave = seeds.next();
    carve.seed_gully = seeds.next();
    z.seed_skyline = seeds.next();
  }
  // **The back**: `depth` behind the face's mean plane, which is the carving's deepest reach plus
  // two metres when the file gives none, and never under 4 m.
  const f64 reach = carve.relief + carve.undercut * carve.bed + 0.12 * carve.bed + carve.warp_amp +
                    carve.gully_amp + 2.0 * m.grain + m.column_relief +
                    carve.rounding * (1.0 + 3.0 * carve.rounding);
  z.depth = p.depth.has_value() ? wide(*p.depth) : max2(4.0, reach + 2.0);
  z.window = max2(1.0, 0.5 * z.depth);
  z.buried = wide(p.buried);
  z.talus = wide(p.talus);
  z.ramp = max2(1.0, min2(20.0, 0.2 * z.line.length));
  const f64 angle = wide(p.talus_angle_deg) * k_pi / 180.0;
  z.sin_a = det::sin(angle);
  z.tan_a = z.sin_a / det::cos(angle);
  const f64 tile_m = wide(p.material.tile_m);

  // The skirt's longest slope, for its rows.
  f64 slope_max = 0.0;
  if (z.talus > 0.0) {
    for (u32 i = 0; i <= 64; ++i) {
      const SkirtStation st = skirt_station(z, z.line.length * static_cast<f64>(i) / 64.0);
      const f64 dx = st.x_top - st.x_toe;
      const f64 dy = st.y_top - st.y_toe;
      slope_max = max2(slope_max, std::sqrt(dx * dx + dy * dy));
    }
  }

  // ---- the budget: the blocks first, on a rung of their own, then the face and the skirt -------
  // A budget too small for every block at the coarsest rung within its 8% keeps the first ones it
  // can hold: a coarse rung is the same face with its smaller detail left out.
  const u32 block_room = static_cast<u32>(0.08 * static_cast<f64>(budget) / triangles_of_grid(4));
  const u32 blocks = p.talus_blocks < block_room ? p.talus_blocks : block_room;
  u32 block_n = 0;
  u64 block_tris = 0;
  if (blocks > 0) {
    const f64 share = 0.08 * static_cast<f64>(budget) / static_cast<f64>(blocks);
    block_n = k_grid_rungs[0];
    for (u32 r = 0; r < k_grid_rung_count; ++r) {
      if (static_cast<f64>(triangles_of_grid(k_grid_rungs[r])) <= share && k_grid_rungs[r] <= 20u)
        block_n = k_grid_rungs[r];
    }
    block_tris = static_cast<u64>(blocks) * triangles_of_grid(block_n);
  }
  const f64 face_h = z.line.height_max + z.buried;
  u32 rung = k_cliff_cell_count;
  u32 ns = 0, nh = 0, nd = 0, ns_skirt = 0, nt = 0;
  u64 face_tris = 0, skirt_tris = 0;
  for (u32 r = k_cliff_cell_count; r-- > 0;) {
    const f64 cell = k_cliff_cells_m[r];
    const u32 a = cells_over(z.line.length, cell, 2);
    const u32 b = cells_over(face_h, cell, 2);
    const u32 c = cells_over(z.depth, cell, 1);
    const u64 ft = box_triangles(a, b, c);
    u64 st = 0;
    u32 sa = 0;
    u32 sb = 0;
    if (z.talus > 0.0) {
      sa = cells_over(z.line.length, 2.0 * cell, 2);
      sb = cells_over(slope_max, 2.0 * cell, 1);
      st = box_triangles(sa, sb, 1);
    }
    if (ft + st + block_tris <= budget) {
      rung = r;
      ns = a;
      nh = b;
      nd = c;
      ns_skirt = sa;
      nt = sb;
      face_tris = ft;
      skirt_tris = st;
      break;
    }
  }
  if (rung == k_cliff_cell_count) {
    const f64 cell = k_cliff_cells_m[0];
    u64 need = box_triangles(cells_over(z.line.length, cell, 2), cells_over(face_h, cell, 2),
                             cells_over(z.depth, cell, 1)) +
               block_tris;
    if (z.talus > 0.0)
      need += box_triangles(cells_over(z.line.length, 2.0 * cell, 2),
                            cells_over(slope_max, 2.0 * cell, 1), 1);
    return fail("engine.rocks.Cliff.triangle_budget " + std::to_string(p.triangle_budget) +
                " cannot hold the face at its coarsest cell (32 m) and its " +
                std::to_string(blocks) + " blocks: that needs " + std::to_string(need) +
                " triangles");
  }
  const f64 cell = k_cliff_cells_m[rung];
  carve.cell = cell;
  const f64 smallest_set =
      carve.sets == 0 ? 0.0
                      : min2(carve.h_spacing, carve.sets >= 2 ? min2(m.v_spacing, 1.0e9) : 1.0e9);
  carve.fade_blocks = carve.sets == 0 ? 0.0 : fade_for(smallest_set, cell);
  carve.fade_beds = carve.bed > 0.0 ? fade_for(carve.bed, cell) : 0.0;
  carve.fade_columns = fade_for(m.v_spacing, cell);

  out.materials.push_back(detail::rock_material(p.type));
  material_factors(p.material, out.materials[0].base_color, out.materials[0].roughness);

  // ---- the face's box --------------------------------------------------------------------------
  Vector<Frame> frames;
  frames.reserve(ns + 1);
  for (u32 i = 0; i <= ns; ++i) {
    const f64 s =
        i == ns ? z.line.length : z.line.length * static_cast<f64>(i) / static_cast<f64>(ns);
    Frame f = frame_at(z.line, s, z.window);
    // The skyline: the top is broken back by up to 15% of the height, slowly along the face.
    f.height *= 1.0 - 0.15 * noise3(z.seed_skyline, D3{s / 30.0, 0.5, 0.5});
    frames.push_back(f);
  }
  const f64 depth = z.depth;
  const auto row_y = [&](const Frame& f, u32 j) {
    const f64 bottom = f.foot.y - z.buried;
    const f64 top = f.foot.y + f.height;
    return j == nh ? top : bottom + (top - bottom) * static_cast<f64>(j) / static_cast<f64>(nh);
  };
  // **The front, held up** (rocks.md, "Cliffs"): each station's column of rows, carved, then
  // walked from the foot up with no row standing out over the one below by more than `k_lean`
  // of the height between them — rock does not hang off the face unsupported, so a block whose
  // neighbour below has fallen back leans back over it — and only then the undercut's notches
  // taken off, whose hard beds are what overhang. Without an undercut nothing faces down more
  // steeply than the lean, which is what the overhang test holds.
  constexpr f64 k_lean = 0.4;
  Vector<f64> front((ns + 1) * (nh + 1));
  for (u32 i = 0; i <= ns; ++i) {
    const Frame& f = frames[i];
    const f64 top = f.foot.y + f.height;
    f64 below = 0.0;
    f64 below_y = 0.0;
    for (u32 j = 0; j <= nh; ++j) {
      const f64 y = row_y(f, j);
      const f64 mean = -f.batter * max2(0.0, y - f.foot.y);
      f64 notch = 0.0;
      f64 o = mean + carve_at(carve, f.s, y, top, notch);
      if (j > 0 && o > below + k_lean * (y - below_y)) o = below + k_lean * (y - below_y);
      below = o;
      below_y = y;
      o -= notch;
      // Held half a metre in front of the back, so the ends' ladders are wound one way.
      if (o < mean - depth + 0.5) o = mean - depth + 0.5;
      front[i * (nh + 1) + j] = o;
    }
  }
  // A lattice point of the face: station i's frame, row j's height between the buried bottom and
  // the top, and layer k between the back (`depth` behind the mean plane, which leans back by the
  // batter) and the front.
  const auto face_point = [&](u32 i, u32 j, u32 k) {
    const Frame& f = frames[i];
    const f64 y = row_y(f, j);
    const f64 back = -f.batter * max2(0.0, y - f.foot.y) - depth;
    f64 o = back;
    if (k > 0) {
      const f64 o_front = front[i * (nh + 1) + j];
      o = k == nd ? o_front : back + (o_front - back) * static_cast<f64>(k) / static_cast<f64>(nd);
    }
    return D3{f.foot.x + f.n.x * o, y, f.foot.z + f.n.z * o};
  };
  // Box projection by the face's orientation: the front and the back in (along, up), the top and
  // the bottom in (along, back), the ends in (back, up), all in metres.
  const auto face_uv = [&](u32 axis, u32 i, u32, u32 k, const D3& place) {
    const f64 back = depth * static_cast<f64>(nd - k) / static_cast<f64>(nd);
    if (axis == 2) return ChartUv{frames[i].s, place.y};
    if (axis == 1) return ChartUv{frames[i].s, back};
    return ChartUv{back, place.y};
  };
  const u32 face_n[3] = {ns, nh, nd};
  emit_box(face_n, face_point, face_uv, tile_m, out);
  u32 parts = 1;

  // ---- the talus skirt -------------------------------------------------------------------------
  Vector<SkirtStation> skirt;
  if (z.talus > 0.0) {
    skirt.reserve(ns_skirt + 1);
    for (u32 i = 0; i <= ns_skirt; ++i) {
      const f64 s = i == ns_skirt
                        ? z.line.length
                        : z.line.length * static_cast<f64>(i) / static_cast<f64>(ns_skirt);
      skirt.push_back(skirt_station(z, s));
    }
    const u64 grain_seed = mix64(carve.seed_grain ^ 0x5441ull);
    const f64 skirt_cell = 2.0 * cell;
    const f64 grain_1 = 0.5 * fade_for(6.0, skirt_cell);
    const f64 grain_2 = 0.25 * fade_for(3.0, skirt_cell);
    // Its cross-section is the quadrilateral toe, top, and the two points a metre under the toe's
    // height below them: convex, so the bilinear map of the lattice onto it cannot fold.
    const auto skirt_point = [&](u32 i, u32 j, u32 k) {
      const SkirtStation& st = skirt[i];
      const f64 t = static_cast<f64>(j) / static_cast<f64>(nt);
      const f64 x = j == nt ? st.x_top : lerp(st.x_toe, st.x_top, t);
      f64 y = st.y_toe - 1.0;
      if (k == 1) {
        y = j == nt ? st.y_top : lerp(st.y_toe, st.y_top, t);
        // The debris is lumpy: two octaves of noise on the slope, never at its edges.
        if (i > 0 && i < ns_skirt && j > 0 && j < nt) {
          const f64 along = st.frame.s;
          const f64 up = t * 12.0;
          y += grain_1 * (2.0 * noise3(grain_seed, D3{along / 6.0, up / 6.0, 0.5}) - 1.0);
          y += grain_2 * (2.0 * noise3(grain_seed + 1u, D3{along / 3.0, up / 3.0, 0.5}) - 1.0);
        }
      }
      return D3{st.frame.foot.x + st.frame.n.x * x, y, st.frame.foot.z + st.frame.n.z * x};
    };
    const auto skirt_uv = [&](u32 axis, u32 i, u32 j, u32, const D3& place) {
      const SkirtStation& st = skirt[i];
      const f64 dx = st.x_top - st.x_toe;
      const f64 dy = st.y_top - st.y_toe;
      const f64 up = std::sqrt(dx * dx + dy * dy) * static_cast<f64>(j) / static_cast<f64>(nt);
      if (axis == 2) return ChartUv{st.frame.s, up};
      if (axis == 1) return ChartUv{st.frame.s, place.y};
      return ChartUv{up, place.y};
    };
    const u32 skirt_n[3] = {ns_skirt, nt, 1};
    emit_box(skirt_n, skirt_point, skirt_uv, tile_m, out);
    ++parts;
  }

  // ---- the fallen blocks: the largest rolled furthest ------------------------------------------
  {
    Rng layout(mix64(p.seed ^ 0x424C4F434Bull));  // "BLOCK"
    for (u32 b = 0; b < blocks; ++b) {
      const f64 s = z.line.length * layout.range(0.04, 0.96);
      const f64 u = layout.unit();  // 0 at the skirt's toe, 1 against the face
      const f64 size = wide(p.talus_block_m) * (0.35 + 0.65 * (1.0 - u) * layout.range(0.6, 1.0));
      const D3 half{size * layout.range(0.7, 1.0), size * layout.range(0.4, 0.6),
                    size * layout.range(0.6, 0.9)};
      const f64 yaw = layout.range(0.0, 2.0 * k_pi);
      const u64 seed = layout.next();
      f64 x = layout.range(0.5, 4.0);
      f64 y = 0.0;
      Frame f;
      if (z.talus > 0.0) {
        const SkirtStation st = skirt_station(z, s);
        f = st.frame;
        const f64 along = 0.85 * u;
        x = lerp(st.x_ground, -f.batter * st.skirt_h, along);
        y = f.foot.y + st.skirt_h * along;
      } else {
        f = frame_at(z.line, s, z.window);
        y = f.foot.y;
      }
      const D3 at{f.foot.x + f.n.x * x, y, f.foot.z + f.n.z * x};
      detail::emit_fragment(p.type, half, at, yaw, 0.3, seed, block_n, tile_m, out);
      ++parts;
    }
  }

  assets::Primitive part;
  part.first_index = 0;
  part.index_count = out.indices.size();
  part.material = 0;
  out.primitives.push_back(part);

  if (report_out != nullptr) {
    CliffReport r;
    r.triangles = out.indices.size() / 3;
    r.vertices = out.positions.size();
    r.parts = parts;
    r.steps = k_cliff_cell_count - 1 - rung;
    r.stations = ns;
    r.rows = nh;
    r.depth_rows = nd;
    r.face_triangles = static_cast<u32>(face_tris);
    r.skirt_triangles = static_cast<u32>(skirt_tris);
    r.block_triangles = static_cast<u32>(block_tris);
    r.block_grid = block_n;
    r.joint_sets = carve.sets;
    r.beds = carve.bed > 0.0 ? static_cast<u32>(std::floor(face_h / carve.bed)) : 0u;
    r.cell_m = static_cast<f32>(cell);
    r.length_m = static_cast<f32>(z.line.length);
    r.height_m = static_cast<f32>(z.line.height_max);
    r.depth_m = static_cast<f32>(depth);
    r.relief_m = static_cast<f32>(carve.relief);
    r.rounding = static_cast<f32>(carve.rounding);
    *report_out = r;
  }
  return true;
}

}  // namespace engine::rocks
