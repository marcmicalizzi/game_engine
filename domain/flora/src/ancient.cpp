// The ancient tree's solver, the megaflora regime (trees.h, structure.h; docs/subsystems/flora.md,
// "Megaflora").
#include "kit.h"

#include <domain/flora/flora.h>
#include <domain/flora/structure.h>
#include <domain/flora/trees.h>

#include <algorithm>
#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

// Independent streams, so a draw added to the canopy never moves a limb or a doorway.
enum class Stream : u64 {
  trunk = 51,
  buttresses = 52,
  hollows = 53,
  limbs = 54,
  crown = 1000000,  // the recursion's shoots, one stream each from here
};

constexpr f64 k_sink_m = 1.0;      // the foot below the ground, so the flare meets any ground
constexpr f64 k_fin_width = 0.12;  // a buttress's half-width at its root, a share of the gap
constexpr f64 k_run_inset = 0.5;   // a limb's run starts this share of the core radius out
constexpr f64 k_run_taper = 0.85;  // a run's tip radius over its base's
constexpr u32 k_crown_rows = 16;   // the structure's resolution past the runs
constexpr u32 k_part_cap = 2000;   // the recursion's motion parts: thousands a tree, not millions

// ---- the layout: the trunk's frame, its buttresses and its hollows ------------------------------

struct Fin {
  f64 angle = 0.0;  // round the trunk's frame
  f64 reach = 0.0;  // past the core at the ground, a share of the core radius
  f64 height = 0.0;
};

struct Doorway {
  f64 angle = 0.0;  // the centre, round the trunk's frame
  f64 sill = 0.0;   // arcs along the trunk's axis
  f64 lintel = 0.0;
  f64 half_angle = 0.0;
  f64 depth = 0.0;
};

struct Layout {
  D3 base;  // the axis's foot, `k_sink_m` below the ground
  D3 t;     // the axis, straight
  D3 n;     // the frame across it: a round angle phi is n cos phi + b sin phi
  D3 b;
  f64 length = 0.0;  // the trunk's axis
  f64 r0 = 0.0;      // the core's radius at the foot and the top
  f64 r1 = 0.0;
  Vector<Fin> fins;
  Vector<Doorway> doors;
};

f64 core_radius(const Layout& l, f64 arc) { return lerp(l.r0, l.r1, clamp01(arc / l.length)); }
D3 radial_at(const Layout& l, f64 angle) { return l.n * det::cos(angle) + l.b * det::sin(angle); }
f64 trunk_top_y(const Ancient& p) {
  const f64 h = static_cast<f64>(p.height_m);
  f64 top = static_cast<f64>(p.limb_high_m) + 0.08 * h;
  if (top < 0.8 * h) top = 0.8 * h;
  if (top > 0.9 * h) top = 0.9 * h;
  return top;
}

// The angular distance between two angles, in [0, pi].
f64 angle_between(f64 a, f64 b) {
  f64 d = a - b;
  while (d > k_pi)
    d -= 2.0 * k_pi;
  while (d < -k_pi)
    d += 2.0 * k_pi;
  return d < 0.0 ? -d : d;
}
f64 wrap(f64 a) {
  while (a >= 2.0 * k_pi)
    a -= 2.0 * k_pi;
  while (a < 0.0)
    a += 2.0 * k_pi;
  return a;
}

Layout lay_out(const Ancient& p) {
  Layout l;
  Rng trunk_rng = stream_of(p.seed, static_cast<u64>(Stream::trunk));
  const f64 azimuth = trunk_rng.unit() * 2.0 * k_pi;
  const f64 lean = static_cast<f64>(p.lean_deg) * k_degree;
  const D3 heading{det::cos(azimuth), 0.0, det::sin(azimuth)};
  l.t = normalized(k_up * det::cos(lean) + heading * det::sin(lean));
  l.n = perpendicular(l.t, D3{1.0, 0.0, 0.0});
  l.b = cross(l.t, l.n);
  l.base = D3{0.0, -k_sink_m, 0.0};
  l.length = (trunk_top_y(p) + k_sink_m) / l.t.y;
  l.r0 = static_cast<f64>(p.trunk_radius_m);
  l.r1 = l.r0 * static_cast<f64>(p.taper);

  // The buttresses: evenly round the foot from a seeded turn, each nudged, each its own reach and
  // height.
  Rng fin_rng = stream_of(p.seed, static_cast<u64>(Stream::buttresses));
  const f64 turn = fin_rng.unit() * 2.0 * k_pi;
  const f64 gap = p.buttresses > 0 ? 2.0 * k_pi / static_cast<f64>(p.buttresses) : 2.0 * k_pi;
  for (u32 k = 0; k < p.buttresses; ++k) {
    Fin fin;
    const f64 nudge = 0.25 * gap * fin_rng.symmetric();
    fin.angle = wrap(turn + gap * static_cast<f64>(k) + nudge);
    const f64 reach = 0.75 + 0.5 * fin_rng.unit();
    fin.reach = static_cast<f64>(p.buttress_reach) * reach;
    const f64 tall = 0.7 + 0.6 * fin_rng.unit();
    fin.height = static_cast<f64>(p.buttress_height_m) * tall;
    l.fins.push_back(fin);
  }
  // The hollows: each in the middle of a gap between two buttresses (the widest gaps first, in a
  // seeded order), one above the other.
  Rng door_rng = stream_of(p.seed, static_cast<u64>(Stream::hollows));
  Vector<f64> sorted;
  for (const Fin& fin : l.fins)
    sorted.push_back(fin.angle);
  std::sort(sorted.begin(), sorted.end());
  const f64 door_turn = door_rng.unit() * 2.0 * k_pi;
  const u64 first_gap = door_rng.next();
  for (u32 j = 0; j < p.hollows; ++j) {
    Doorway door;
    if (sorted.size() < 2) {
      door.angle = wrap(door_turn + static_cast<f64>(j) * k_golden_angle);
    } else {
      // Every other gap round the foot, from a seeded one.
      const u32 g = static_cast<u32>((first_gap + 2u * j) % sorted.size());
      const f64 a = sorted[g];
      const f64 b = g + 1 < sorted.size() ? sorted[g + 1] : sorted[0] + 2.0 * k_pi;
      door.angle = wrap(0.5 * (a + b));
    }
    door.sill = (static_cast<f64>(p.hollow_sill_m) +
                 static_cast<f64>(p.hollow_spacing_m) * static_cast<f64>(j) + k_sink_m) /
                l.t.y;
    door.lintel = door.sill + static_cast<f64>(p.hollow_height_m) / l.t.y;
    const f64 r = core_radius(l, door.sill);
    door.half_angle = 0.5 * static_cast<f64>(p.hollow_width_m) / r;
    door.depth = static_cast<f64>(p.hollow_depth) * r;
    l.doors.push_back(door);
  }
  return l;
}

// The surface's radius at an arc and an angle: the core, and every buttress's fin — a smooth
// plate, thickest at its ridge, sweeping in to the core by its height.
f64 surface_radius(const Layout& l, const Ancient& p, f64 arc, f64 angle) {
  const f64 core = core_radius(l, arc);
  if (l.fins.empty()) return core;
  const f64 h = arc * l.t.y - k_sink_m;
  const f64 w = k_fin_width * 2.0 * k_pi / static_cast<f64>(p.buttresses);
  f64 flare = 0.0;
  for (const Fin& fin : l.fins) {
    if (h >= fin.height) continue;
    const f64 rise = h <= 0.0 ? 1.0 : 1.0 - h / fin.height;
    const f64 x = angle_between(angle, fin.angle) / w;
    if (x >= 1.0) continue;
    const f64 bump = (1.0 - x * x) * (1.0 - x * x) * (1.0 - x * x);
    flare += fin.reach * rise * rise * bump;
  }
  return core * (1.0 + flare);
}

// ---- the trunk's grid
// ----------------------------------------------------------------------------
//
// Rows and columns at the plan's even steps, with the hollows' edges and the buttresses' ridges
// and flanks **pinned** into them: a doorway is the size asked on every rung of the ladder and a
// buttress keeps its ridge however few columns the trunk has.

struct Grid {
  Vector<f64> arcs;    // along the axis, from 0 to the trunk's length
  Vector<f64> angles;  // round it from the seam, 0 to 2 pi inclusive
  f64 seam = 0.0;      // the frame angle of column 0
};

void merge_pins(Vector<f64>& out, const Vector<f64>& pins, f64 lo, f64 hi, u32 steps) {
  const f64 step = (hi - lo) / static_cast<f64>(steps);
  Vector<f64> values = pins;
  for (u32 i = 0; i <= steps; ++i) {
    const f64 v = lo + step * static_cast<f64>(i);
    bool near = false;
    if (i > 0 && i < steps) {
      for (const f64 pin : pins)
        if (v - pin < 0.3 * step && pin - v < 0.3 * step) near = true;
    }
    if (!near) values.push_back(i == steps ? hi : v);
  }
  std::sort(values.begin(), values.end());
  out.clear();
  for (const f64 v : values)
    if (out.empty() || v - out.back() > 1e-6) out.push_back(v);
}

Grid trunk_grid(const Layout& l, const Ancient& p, u32 rows, u32 sides, bool fin_flanks) {
  Grid g;
  // The seam where no doorway is: the middle of the widest stretch of circle between them.
  if (!l.doors.empty()) {
    Vector<f64> centres;
    for (const Doorway& d : l.doors)
      centres.push_back(d.angle);
    std::sort(centres.begin(), centres.end());
    f64 best = -1.0;
    for (u32 i = 0; i < centres.size(); ++i) {
      const f64 a = centres[i];
      const f64 b = i + 1 < centres.size() ? centres[i + 1] : centres[0] + 2.0 * k_pi;
      if (b - a > best) {
        best = b - a;
        g.seam = wrap(0.5 * (a + b));
      }
    }
  }
  Vector<f64> row_pins;
  for (const Doorway& d : l.doors) {
    row_pins.push_back(d.sill);
    row_pins.push_back(d.lintel);
  }
  merge_pins(g.arcs, row_pins, 0.0, l.length, rows);

  // Column pins: the doorways' jambs first, then the buttresses' ridges and flanks where they are
  // not already within a hundredth of a radian of one.
  Vector<f64> col_pins;
  const auto pin = [&](f64 frame_angle) {
    const f64 a = wrap(frame_angle - g.seam);
    if (a < 0.01 || a > 2.0 * k_pi - 0.01) return;
    for (const f64 c : col_pins)
      if (angle_between(a, c) < 0.01) return;
    col_pins.push_back(a);
  };
  for (const Doorway& d : l.doors) {
    pin(d.angle - d.half_angle);
    pin(d.angle + d.half_angle);
  }
  const f64 w = l.fins.empty() ? 0.0 : k_fin_width * 2.0 * k_pi / static_cast<f64>(p.buttresses);
  for (const Fin& fin : l.fins) {
    pin(fin.angle);
    pin(fin.angle - 0.7 * w);
    pin(fin.angle + 0.7 * w);
    if (fin_flanks) {
      pin(fin.angle - 0.35 * w);
      pin(fin.angle + 0.35 * w);
    }
  }
  merge_pins(g.angles, col_pins, 0.0, 2.0 * k_pi, sides);
  return g;
}

u32 nearest(const Vector<f64>& values, f64 v) {
  u32 best = 0;
  for (u32 i = 1; i < values.size(); ++i) {
    const f64 d = values[i] > v ? values[i] - v : v - values[i];
    const f64 e = values[best] > v ? values[best] - v : v - values[best];
    if (d < e) best = i;
  }
  return best;
}

struct Window {
  u32 r0 = 0, r1 = 0, c0 = 0, c1 = 0;  // grid lines: rows [r0, r1], columns [c0, c1]
};

Vector<Window> windows_of(const Layout& l, const Grid& g) {
  Vector<Window> out;
  for (const Doorway& d : l.doors) {
    Window w;
    w.r0 = nearest(g.arcs, d.sill);
    w.r1 = nearest(g.arcs, d.lintel);
    w.c0 = nearest(g.angles, wrap(d.angle - d.half_angle - g.seam));
    w.c1 = nearest(g.angles, wrap(d.angle + d.half_angle - g.seam));
    out.push_back(w);
  }
  return out;
}

u32 trunk_triangles(const Grid& g, const Vector<Window>& windows) {
  const u32 rows = g.arcs.size() - 1;
  const u32 cols = g.angles.size() - 1;
  // Every quad, the cap's cone, and for each hollow its walls (the window's quads move to its back
  // wall, so they are counted once).
  u32 n = rows * cols * 2 + cols;
  for (const Window& w : windows)
    n += 2 * 2 * ((w.r1 - w.r0) + (w.c1 - w.c0));
  return n;
}

// ---- the structure ----------------------------------------------------------------------------
//
// The named orders — the trunk, every major limb's straight run and the limb past it — and from
// every limb and from the trunk's top the allometric recursion (kit.h, "the allometric crown") down
// to the species' own twig. Grown once; it never depends on the budget.

struct Named {
  u32 trunk = 0;
  Vector<u32> runs;
  Vector<u32> limbs;
};

CrownSpec spec_of(const Ancient& p) {
  CrownSpec s;
  s.area_exponent = static_cast<f64>(p.area_exponent);
  s.leader_share = static_cast<f64>(p.leader_share);
  u32 laterals = p.branching_ratio - 1;
  if (laterals > p.sub_branches) laterals = p.sub_branches;
  if (laterals > p.twigs) laterals = p.twigs;
  s.laterals = laterals;
  s.length_ratio = static_cast<f64>(p.length_ratio);
  s.twig_radius = static_cast<f64>(p.twig_radius_m);
  s.twig_length = static_cast<f64>(p.twig_length_m);
  s.branch_angle = static_cast<f64>(p.branch_angle_deg) * k_degree;
  s.leader_angle = 0.25 * s.branch_angle;
  s.upturn = static_cast<f64>(p.upturn) * 40.0 * k_degree;
  s.wander = static_cast<f64>(p.wander_deg) * k_degree;
  s.from = 0.25;
  s.part_cap = k_part_cap;
  s.seed = p.seed;
  s.stream = static_cast<u64>(Stream::crown);
  return s;
}

Crown grow(const Ancient& p, const Layout& l, const CrownSpec& spec, Named& named) {
  Crown crown;
  const f64 wander = static_cast<f64>(p.wander_deg) * k_degree;
  {
    Rng none(0);
    const Axis trunk = grow_axis(l.base, l.t, l.length, 32, k_up, 0.0, 0.0, none);
    named.trunk = crown_add_named(crown, trunk, l.r0, l.r1, k_crown_none);
  }

  // The major limbs, up the trunk on the golden angle from `limb_low_m` to `limb_high_m`: each a
  // straight run out from inside the trunk, near-horizontal — where platforms go — and then the
  // limb past it, turning up towards the light, from which the crown grows.
  Rng limb_rng = stream_of(p.seed, static_cast<u64>(Stream::limbs));
  const f64 turn = limb_rng.unit() * 2.0 * k_pi;
  const f64 low = static_cast<f64>(p.limb_low_m);
  const f64 high = static_cast<f64>(p.limb_high_m);
  Vector<CrownRoot> roots;
  for (u32 i = 0; i < p.limbs; ++i) {
    const f64 f = static_cast<f64>(i) / static_cast<f64>(p.limbs - 1);
    const f64 y = low + (high - low) * f;
    const f64 arc = (y + k_sink_m) / l.t.y;
    const f64 around = turn + static_cast<f64>(i) * k_golden_angle + 0.2 * limb_rng.symmetric();
    const f64 jitter = limb_rng.symmetric();
    f64 rise = static_cast<f64>(p.limb_rise_deg) + 8.0 * f + 2.0 * jitter;
    if (rise > 40.0) rise = 40.0;
    rise *= k_degree;
    D3 out = radial_at(l, around);
    out = normalized(D3{out.x, 0.0, out.z});
    const D3 dir = normalized(out * det::cos(rise) + k_up * det::sin(rise));
    const D3 pivot = l.base + l.t * arc + out * (k_run_inset * core_radius(l, arc));
    Axis run;
    {
      Rng none(0);
      run = grow_axis(pivot, dir, static_cast<f64>(p.limb_run_m) * (1.0 - 0.35 * f), 8, k_up, 0.0,
                      0.0, none);
    }
    const f64 run_r0 = static_cast<f64>(p.limb_radius_m) * (1.0 - 0.35 * f);
    const f64 run_r1 = run_r0 * k_run_taper;
    const D3 run_tip = run.point.back();
    named.runs.push_back(crown_add_named(crown, run, run_r0, run_r1, named.trunk));

    const f64 reach =
        static_cast<f64>(p.limb_reach_m) * (1.0 - 0.4 * f) * (0.9 + 0.2 * limb_rng.unit());
    const f64 sweep = (0.6 + 0.4 * static_cast<f64>(p.upturn)) * 60.0 * k_degree;
    const Axis limb =
        grow_axis(run_tip, dir, reach, k_crown_rows, k_up, sweep / reach, wander * 0.5, limb_rng);
    CrownRoot root;
    root.node =
        crown_add_named(crown, limb, run_r1, crown_leader_radius(spec, run_r1), named.runs.back());
    named.limbs.push_back(root.node);
    root.radius = run_r1;
    root.laterals = p.branches;
    root.lateral_length = static_cast<f64>(p.branch_length) * reach;
    root.from = 0.15;
    root.to = 0.95;
    roots.push_back(root);
  }
  // A crown on the trunk's top, where the limbs stop.
  CrownRoot top;
  top.node = named.trunk;
  top.radius = l.r1;
  top.laterals = p.branches;
  top.lateral_length = static_cast<f64>(p.branch_length) * static_cast<f64>(p.limb_reach_m) * 0.8;
  top.from = 0.86;
  top.to = 1.0;
  top.spread_scale = 0.8;
  roots.push_back(top);
  crown_grow(crown, spec, roots, 3);
  return crown;
}

// The leaf count by the leaf area index (kit.h): one-sided leaf area over the footprint, each card
// one leaf of `leaf_length_m` — the species' leaf on a 40 m tree and on a 150 m one.
void place_leaves(const Ancient& p, Crown& crown) {
  const f64 length = static_cast<f64>(p.leaf_length_m);
  const f64 width = static_cast<f64>(p.leaf_width) * length;
  const f64 angle = static_cast<f64>(p.leaf_angle_deg) * k_degree;
  crown.card_step = width / det::sin(angle);
  const f64 cards = static_cast<f64>(p.leaf_area_index) * crown.footprint / (width * length);
  crown_distribute_leaves(crown, cards, p.leaves);
}

// ---- the plan ----------------------------------------------------------------------------------

struct Plan {
  u32 trunk_sides = 96;
  u32 trunk_rows = 0;
  bool fin_flanks = true;
  u32 sides[3] = {0, 32, 24};  // by order: (trunk), run, limb past it
  u32 rows[3] = {0, 12, 20};
  CrownPlan crown;
};

u32 count_triangles(const Ancient& p, const Layout& l, const Crown& crown, const Plan& plan) {
  const Grid g = trunk_grid(l, p, plan.trunk_rows, plan.trunk_sides, plan.fin_flanks);
  return trunk_triangles(g, windows_of(l, g)) +
         p.limbs * tube_triangles(plan.rows[1], plan.sides[1]) +
         p.limbs * tube_triangles(plan.rows[2], plan.sides[2]) + crown_triangles(crown, plan.crown);
}

// The ladder (flora.md, "The budget"): the finest wood first, then rows and cross-sections from
// the finest order up, then the finer orders' wood folded into their parts one class at a time,
// then the trunk's rows and columns, and only then — where the budget forces it — leaf cards
// merged up the hierarchy, finest first, and the last of the rows and sides. The pinned columns
// stay on every rung, so a doorway is its size and a buttress keeps its ridge. No rung moves a
// shoot or drops a part: the skeleton, the part list and the structural table are the same bytes
// on every rung (graphics presets pick rungs; gameplay must not change with them).
bool plan_ancient(const Ancient& p, const Layout& l, const Crown& crown, Plan& plan, u32& steps) {
  plan = Plan{};
  const u32 trunk_rows = static_cast<u32>(l.length / 0.6);
  plan.trunk_rows = trunk_rows < 24 ? 24 : (trunk_rows > 160 ? 160 : trunk_rows);
  plan.crown.edge_m = 0.1;
  plan.crown.max_sides = 12;
  plan.crown.row_m = 1.0;
  plan.crown.wood_class =
      2;  // shoots under two pipe steps over a twig (2.6 cm) fold from the start
  steps = 0;
  const auto fits = [&] { return count_triangles(p, l, crown, plan) <= p.triangle_budget; };
  if (fits()) return true;
  const auto step = [&](auto&& change) {
    change();
    ++steps;
    return fits();
  };
  const auto halve = [](u32 v, u32 floor) { return v / 2 < floor ? floor : v / 2; };
  if (step([&] {
        plan.crown.row_m = 2.0;
        plan.rows[2] = 12;
        plan.rows[1] = 8;
      })) {
    return true;
  }
  if (step([&] {
        plan.crown.edge_m = 0.2;
        plan.crown.max_sides = 8;
      })) {
    return true;
  }
  if (step([&] {
        plan.trunk_sides = 64;
        plan.sides[1] = 24;
        plan.sides[2] = 16;
      })) {
    return true;
  }
  while (plan.crown.wood_class < crown.part_class) {
    if (step([&] { ++plan.crown.wood_class; })) return true;
  }
  if (step([&] { plan.trunk_rows = halve(plan.trunk_rows, 16); })) return true;
  if (step([&] {
        plan.crown.edge_m = 0.4;
        plan.crown.max_sides = 5;
        plan.sides[2] = 10;
        plan.sides[1] = 16;
      })) {
    return true;
  }
  if (step([&] {
        plan.trunk_sides = 40;
        plan.trunk_rows = halve(plan.trunk_rows, 12);
      })) {
    return true;
  }
  while (plan.crown.merge < crown.orders) {
    if (step([&] { ++plan.crown.merge; })) return true;
  }
  for (u32 k = 0; k < 6; ++k) {
    if (step([&] { ++plan.crown.halvings; })) return true;
  }
  if (step([&] {
        plan.trunk_sides = 20;
        plan.trunk_rows = 12;
        plan.fin_flanks = false;
        plan.sides[1] = 8;
        plan.rows[1] = 3;
        plan.sides[2] = 6;
        plan.rows[2] = 4;
        plan.crown.max_sides = 4;
        plan.crown.row_m = 4.0;
      })) {
    return true;
  }
  return step([&] {
    plan.trunk_sides = 12;
    plan.trunk_rows = 8;
    plan.sides[1] = 6;
    plan.rows[1] = 2;
    plan.sides[2] = 4;
    plan.rows[2] = 2;
    plan.crown.max_sides = 3;
    plan.crown.row_m = 8.0;
  });
}

// ---- the trunk's mesh --------------------------------------------------------------------------

void build_trunk(const Layout& l, const Ancient& p, const Grid& g, const Vector<Window>& windows,
                 Emitter& out) {
  const u32 rows = g.arcs.size();
  const u32 cols = g.angles.size();
  const u32 first = out.mesh.positions.size();
  const f64 round = k_pi * (l.r0 + l.r1);  // metres once round at the mean core radius
  const auto index = [&](u32 r, u32 c) { return first + r * cols + c; };
  for (u32 r = 0; r < rows; ++r) {
    const f64 arc = g.arcs[r];
    out.weight = arc / l.length;
    const D3 centre = l.base + l.t * arc;
    for (u32 c = 0; c < cols; ++c) {
      const f64 angle = g.seam + g.angles[c];
      const D3 radial = radial_at(l, angle);
      out.vertex(centre + radial * surface_radius(l, p, arc, angle), radial,
                 round * g.angles[c] / (2.0 * k_pi), arc);
    }
  }
  const auto inside = [&](u32 r, u32 c) {
    for (const Window& w : windows)
      if (r >= w.r0 && r < w.r1 && c >= w.c0 && c < w.c1) return true;
    return false;
  };
  // The quads, wound outwards: along the columns and up the rows is the outward orientation of the
  // frame (b = t x n), so every quad takes the same order.
  for (u32 r = 0; r + 1 < rows; ++r) {
    for (u32 c = 0; c + 1 < cols; ++c) {
      if (inside(r, c)) continue;
      out.triangle(index(r, c), index(r, c + 1), index(r + 1, c + 1));
      out.triangle(index(r, c), index(r + 1, c + 1), index(r + 1, c));
    }
  }
  // The top: a cone to a point one top radius past the last row.
  out.weight = 1.0;
  const u32 tip = out.vertex(l.base + l.t * (l.length + l.r1), l.t, 0.5 * round, l.length + l.r1);
  for (u32 c = 0; c + 1 < cols; ++c)
    out.triangle(index(rows - 1, c), index(rows - 1, c + 1), tip);

  // The hollows: the window's own vertices moved in along the doorway's axis to make the back
  // wall, and the rim duplicated (its own normals) with walls from the rim to the back.
  for (u32 h = 0; h < windows.size(); ++h) {
    const Window& w = windows[h];
    const Doorway& d = l.doors[h];
    const D3 inward = radial_at(l, d.angle) * -1.0;
    const f64 depth = d.depth;
    // The back wall: every window vertex `depth` in; the rim's back vertices are new.
    Vector<u32> back((w.r1 - w.r0 + 1) * (w.c1 - w.c0 + 1));
    const auto back_at = [&](u32 r, u32 c) -> u32& {
      return back[(r - w.r0) * (w.c1 - w.c0 + 1) + (c - w.c0)];
    };
    for (u32 r = w.r0; r <= w.r1; ++r) {
      for (u32 c = w.c0; c <= w.c1; ++c) {
        const u32 v = index(r, c);
        const D3 moved = from_f32(out.mesh.positions[v]) + inward * depth;
        const bool rim = r == w.r0 || r == w.r1 || c == w.c0 || c == w.c1;
        if (rim) {
          out.weight = g.arcs[r] / l.length;
          back_at(r, c) = out.vertex(moved, inward * -1.0, static_cast<f64>(out.mesh.uvs[v].x),
                                     static_cast<f64>(out.mesh.uvs[v].y) + depth);
        } else {
          out.mesh.positions[v] = to_f32(moved);
          out.mesh.uvs[v].y += static_cast<f32>(depth);
          back_at(r, c) = v;
        }
      }
    }
    const D3 facing = inward * -1.0;
    for (u32 r = w.r0; r < w.r1; ++r) {
      for (u32 c = w.c0; c < w.c1; ++c) {
        out.face(back_at(r, c), back_at(r, c + 1), back_at(r + 1, c + 1), facing);
        out.face(back_at(r, c), back_at(r + 1, c + 1), back_at(r + 1, c), facing);
      }
    }
    // The rim: round the window's edge, the outer vertices duplicated, a wall quad to the back
    // wall at every step, facing into the cavity.
    Vector<u32> loop_r;
    Vector<u32> loop_c;
    for (u32 c = w.c0; c < w.c1; ++c) {
      loop_r.push_back(w.r0);
      loop_c.push_back(c);
    }
    for (u32 r = w.r0; r < w.r1; ++r) {
      loop_r.push_back(r);
      loop_c.push_back(w.c1);
    }
    for (u32 c = w.c1; c > w.c0; --c) {
      loop_r.push_back(w.r1);
      loop_c.push_back(c);
    }
    for (u32 r = w.r1; r > w.r0; --r) {
      loop_r.push_back(r);
      loop_c.push_back(w.c0);
    }
    const u32 n = loop_r.size();
    Vector<u32> rim(n);
    for (u32 k = 0; k < n; ++k) {
      const u32 v = index(loop_r[k], loop_c[k]);
      out.weight = g.arcs[loop_r[k]] / l.length;
      rim[k] = out.vertex(from_f32(out.mesh.positions[v]), facing,
                          static_cast<f64>(out.mesh.uvs[v].x), static_cast<f64>(out.mesh.uvs[v].y));
    }
    const D3 middle = (from_f32(out.mesh.positions[index(w.r0, w.c0)]) +
                       from_f32(out.mesh.positions[index(w.r1, w.c1)])) *
                          0.5 +
                      inward * (0.5 * depth);
    for (u32 k = 0; k < n; ++k) {
      const u32 k1 = (k + 1) % n;
      const u32 a = rim[k];
      const u32 b = rim[k1];
      const u32 bb = back_at(loop_r[k1], loop_c[k1]);
      const u32 ab = back_at(loop_r[k], loop_c[k]);
      const D3 edge = (from_f32(out.mesh.positions[a]) + from_f32(out.mesh.positions[b])) * 0.5;
      const D3 hint = middle - edge;
      out.face(a, b, bb, hint);
      out.face(a, bb, ab, hint);
    }
  }
}

// The trunk's normals: the area-weighted normals of its own faces, the seam's two columns given
// their mean so the bark shades across it.
void trunk_normals(assets::MeshData& mesh, u32 first_vertex, u32 first_index, u32 last_index,
                   u32 rows, u32 cols) {
  Vector<Vec3> normals;
  const std::span<const u32> range(mesh.indices.data() + first_index, last_index - first_index);
  geometry::compute_vertex_normals(mesh.positions, range, normals);
  for (u32 v = first_vertex; v < mesh.positions.size(); ++v) {
    const Vec3 n = normals[v];
    if (dot(n, n) > 0.0f) mesh.normals[v] = n;
  }
  for (u32 r = 0; r < rows; ++r) {
    const u32 a = first_vertex + r * cols;
    const u32 b = a + cols - 1;
    const D3 mean = normalized(from_f32(mesh.normals[a]) + from_f32(mesh.normals[b]));
    mesh.normals[a] = to_f32(mean);
    mesh.normals[b] = to_f32(mean);
  }
}

}  // namespace

bool ancient_in_range(const Ancient& p, std::string& error) {
  const auto bad = [&](const char* field, const std::string& range) {
    error = std::string("engine.flora.Ancient.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.height_m >= 40.0f && p.height_m <= 150.0f)) return bad("height_m", "40 to 150 m");
  if (!(p.trunk_radius_m >= 2.0f && p.trunk_radius_m <= 12.5f))
    return bad("trunk_radius_m", "2 to 12.5 m");
  if (!(p.taper >= 0.15f && p.taper <= 0.8f)) return bad("taper", "0.15 to 0.8");
  if (!(p.lean_deg >= 0.0f && p.lean_deg <= 8.0f)) return bad("lean_deg", "0 to 8 degrees");
  if (!(p.buttresses <= 12u)) return bad("buttresses", "0 to 12");
  if (!(p.buttress_height_m >= 1.0f && p.buttress_height_m <= 40.0f))
    return bad("buttress_height_m", "1 to 40 m");
  if (!(p.buttress_reach >= 0.0f && p.buttress_reach <= 2.5f))
    return bad("buttress_reach", "0 to 2.5");
  if (!(p.hollows <= 3u)) return bad("hollows", "0 to 3");
  if (!(p.hollow_width_m >= 0.6f && p.hollow_width_m <= 5.0f))
    return bad("hollow_width_m", "0.6 to 5 m");
  if (!(p.hollow_height_m >= 1.2f && p.hollow_height_m <= 8.0f))
    return bad("hollow_height_m", "1.2 to 8 m");
  if (!(p.hollow_depth >= 0.15f && p.hollow_depth <= 0.7f))
    return bad("hollow_depth", "0.15 to 0.7");
  if (!(p.hollow_sill_m >= 0.0f && p.hollow_sill_m <= 60.0f))
    return bad("hollow_sill_m", "0 to 60 m");
  if (!(p.hollow_spacing_m >= 3.0f && p.hollow_spacing_m <= 60.0f))
    return bad("hollow_spacing_m", "3 to 60 m");
  if (!(p.limbs >= 2u && p.limbs <= 12u)) return bad("limbs", "2 to 12");
  if (!(p.limb_low_m >= 8.0f && p.limb_low_m <= 110.0f)) return bad("limb_low_m", "8 to 110 m");
  if (!(p.limb_high_m >= p.limb_low_m && p.limb_high_m <= 0.75f * p.height_m))
    return bad("limb_high_m", "limb_low_m to 0.75 of height_m");
  if (!(p.limb_radius_m >= 0.5f && p.limb_radius_m <= 2.5f))
    return bad("limb_radius_m", "0.5 to 2.5 m");
  if (!(p.limb_rise_deg >= -5.0f && p.limb_rise_deg <= 30.0f))
    return bad("limb_rise_deg", "-5 to 30 degrees");
  if (!(p.limb_run_m >= 4.0f && p.limb_run_m <= 60.0f)) return bad("limb_run_m", "4 to 60 m");
  if (!(p.limb_reach_m >= 4.0f && p.limb_reach_m <= 80.0f)) return bad("limb_reach_m", "4 to 80 m");
  if (!(p.branches >= 2u && p.branches <= 10u)) return bad("branches", "2 to 10");
  if (!(p.branch_angle_deg >= 15.0f && p.branch_angle_deg <= 80.0f))
    return bad("branch_angle_deg", "15 to 80 degrees");
  if (!(p.branch_length >= 0.2f && p.branch_length <= 0.9f))
    return bad("branch_length", "0.2 to 0.9");
  if (!(p.sub_branches >= 1u && p.sub_branches <= 8u)) return bad("sub_branches", "1 to 8");
  if (!(p.twigs >= 1u && p.twigs <= 12u)) return bad("twigs", "1 to 12");
  if (!(p.twig_length_m >= 0.3f && p.twig_length_m <= 1.5f))
    return bad("twig_length_m", "0.3 to 1.5 m");
  if (!(p.upturn >= -1.0f && p.upturn <= 1.0f)) return bad("upturn", "-1 to 1");
  if (!(p.wander_deg >= 0.0f && p.wander_deg <= 20.0f)) return bad("wander_deg", "0 to 20 degrees");
  if (!(p.leaves >= 1u && p.leaves <= 60u)) return bad("leaves", "1 to 60");
  if (!(p.leaf_length_m >= 0.05f && p.leaf_length_m <= 0.6f))
    return bad("leaf_length_m", "0.05 to 0.6 m");
  if (!(p.leaf_angle_deg >= 10.0f && p.leaf_angle_deg <= 85.0f))
    return bad("leaf_angle_deg", "10 to 85 degrees");
  if (!(p.triangle_budget >= 20000u && p.triangle_budget <= 4000000u))
    return bad("triangle_budget", "20,000 to 4,000,000");
  if (!(p.twig_radius_m >= 0.001f && p.twig_radius_m <= 0.012f))
    return bad("twig_radius_m", "0.001 to 0.012 m");
  if (!(p.area_exponent >= 1.8f && p.area_exponent <= 3.0f))
    return bad("area_exponent", "1.8 to 3");
  if (!(p.branching_ratio >= 2u && p.branching_ratio <= 6u))
    return bad("branching_ratio", "2 to 6");
  if (!(p.length_ratio >= 0.4f && p.length_ratio <= 0.9f)) return bad("length_ratio", "0.4 to 0.9");
  if (!(p.leader_share >= 0.1f && p.leader_share <= 0.7f)) return bad("leader_share", "0.1 to 0.7");
  if (!(p.leaf_area_index >= 0.5f && p.leaf_area_index <= 10.0f))
    return bad("leaf_area_index", "0.5 to 10");
  if (!(p.leaf_width >= 0.1f && p.leaf_width <= 1.0f)) return bad("leaf_width", "0.1 to 1");
  // What the fields allow only together. A run must leave the trunk: past the core where it
  // starts, at every limb's height.
  const Layout l = lay_out(p);
  for (u32 i = 0; i < p.limbs; ++i) {
    const f64 f = static_cast<f64>(i) / static_cast<f64>(p.limbs - 1);
    const f64 y = static_cast<f64>(p.limb_low_m) +
                  (static_cast<f64>(p.limb_high_m) - static_cast<f64>(p.limb_low_m)) * f;
    const f64 r = core_radius(l, (y + k_sink_m) / l.t.y);
    if (static_cast<f64>(p.limb_run_m) * (1.0 - 0.35 * f) < (1.0 - k_run_inset) * r + 2.0) {
      return bad("limb_run_m", "at least 2 m past the trunk's core at every limb, " +
                                   std::to_string(static_cast<i32>((1.0 - k_run_inset) * r + 2.0)) +
                                   " m and more here");
    }
  }
  // A hollow is a doorway under the limbs, no wider than its trunk allows, and two never overlap.
  if (p.hollows > 0) {
    if (p.hollows > 1 && !(p.hollow_spacing_m >= p.hollow_height_m + 1.0f))
      return bad("hollow_spacing_m", "at least hollow_height_m + 1 m");
    const Doorway& top = l.doors.back();
    if (top.lintel * l.t.y - k_sink_m > static_cast<f64>(p.limb_low_m) - 2.0)
      return bad("hollow_sill_m", "every hollow's top at least 2 m under limb_low_m");
    for (const Doorway& d : l.doors) {
      if (d.half_angle > 0.6)
        return bad("hollow_width_m", "at most 1.2 times the core radius at the hollow");
    }
  }
  return true;
}

bool solve_ancient(const Ancient& p, assets::MeshData& out, TreeReport* report, std::string* error,
                   Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!ancient_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  const Layout l = lay_out(p);
  const CrownSpec spec = spec_of(p);
  Named named;
  Crown crown = grow(p, l, spec, named);
  place_leaves(p, crown);
  Plan plan;
  u32 steps = 0;
  if (!plan_ancient(p, l, crown, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Ancient.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tree: its coarsest form needs " +
               std::to_string(count_triangles(p, l, crown, plan)) + " triangles";
    }
    return false;
  }

  out.materials.push_back(material("bark", Vec4{0.27f, 0.22f, 0.17f, 1.0f}, 0.92f, false));
  out.materials.push_back(material("leaf", Vec4{0.09f, 0.22f, 0.06f, 1.0f}, 0.6f, true));
  Motion parts;
  Emitter emit{out, parts};

  // The trunk: its core is the structural row (the buttresses stand outside it, the hollows cut
  // into it), load-bearing and straight.
  const Grid grid = trunk_grid(l, p, plan.trunk_rows, plan.trunk_sides, plan.fin_flanks);
  const Vector<Window> windows = windows_of(l, grid);
  crown.nodes[named.trunk].part =
      begin_structural_part(emit, l.base, l.t, l.length, geometry::k_motion_no_parent, l.r0, l.r1,
                            k_load_bearing | k_straight);
  const u32 trunk_vertex = out.positions.size();
  build_trunk(l, p, grid, windows, emit);
  trunk_normals(out, trunk_vertex, 0, out.indices.size(), grid.arcs.size(), grid.angles.size());

  // The runs and the limbs past them, in pairs, load-bearing; the runs straight.
  u32 load_bearing = 1;
  for (u32 i = 0; i < p.limbs; ++i) {
    CrownNode& run = crown.nodes[named.runs[i]];
    run.part =
        emit_tube_part(emit, crown.axes[run.axis], run.r0, run.r1, plan.rows[1], plan.sides[1],
                       crown.nodes[named.trunk].part, k_load_bearing | k_straight);
    CrownNode& limb = crown.nodes[named.limbs[i]];
    limb.part = emit_tube_part(emit, crown.axes[limb.axis], limb.r0, limb.r1, plan.rows[2],
                               plan.sides[2], run.part, k_load_bearing);
    load_bearing += 2;
  }
  crown_emit_wood(crown, plan.crown, emit, true);
  assets::Primitive bark;
  bark.first_index = 0;
  bark.index_count = out.indices.size();
  bark.material = 0;
  out.primitives.push_back(bark);

  const u32 leaf_vertex = out.positions.size();
  const u32 leaf_first = out.indices.size();
  const f64 card_length =
      crown_emit_leaves(crown, plan.crown, emit, static_cast<f64>(p.leaf_length_m),
                        static_cast<f64>(p.leaf_angle_deg) * k_degree, 15.0 * k_degree, 0.15);
  assets::Primitive leaves;
  leaves.first_index = leaf_first;
  leaves.index_count = out.indices.size() - leaf_first;
  leaves.material = 1;
  out.primitives.push_back(leaves);
  smooth_normals(out, leaf_vertex, leaf_first);

  if (report != nullptr) {
    *report = TreeReport{};
    report->triangles = out.indices.size() / 3;
    report->wood_triangles = bark.index_count / 3;
    report->leaf_triangles = leaves.index_count / 3;
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->shoots = crown.nodes.size();
    u32 sprays = 0;
    for (const u32 t : crown.twigs) {
      if (crown.nodes[t].cards > 0u) ++sprays;
      if (crown.nodes[t].cards > report->cards_per_side)
        report->cards_per_side = crown.nodes[t].cards;
    }
    report->sprays = sprays;
    report->trunk_sides = grid.angles.size() - 1;
    report->shoot_sides = plan.crown.max_sides;
    report->steps = steps;
    report->load_bearing = load_bearing;
    report->hollows = windows.size();
    report->twigs = crown.twigs.size();
    report->orders = crown.orders;
    report->leaf_cards = report->leaf_triangles / 2;
    report->merge_level = plan.crown.merge;
    report->wood_class = static_cast<u32>(plan.crown.wood_class);
    report->part_class = static_cast<u32>(crown.part_class);
    report->card_length = static_cast<f32>(card_length);
    report->footprint_m2 = static_cast<f32>(crown.footprint);
    report->leaf_area_m2 = static_cast<f32>(triangles_area(out, leaf_first));
    report->leaf_area_index =
        crown.footprint > 0.0 ? report->leaf_area_m2 / report->footprint_m2 : 0.0f;
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

bool ancient_crown(const Ancient& p, Vector<CrownShoot>& out) {
  std::string error;
  if (!ancient_in_range(p, error)) return false;
  const Layout l = lay_out(p);
  Named named;
  crown_table(grow(p, l, spec_of(p), named), out);
  return true;
}

bool ancient_hollows(const Ancient& p, Vector<Hollow>& out) {
  std::string error;
  if (!ancient_in_range(p, error)) return false;
  out.clear();
  const Layout l = lay_out(p);
  for (const Doorway& d : l.doors) {
    const D3 radial = radial_at(l, d.angle);
    Hollow h;
    h.sill = to_f32(l.base + l.t * d.sill + radial * core_radius(l, d.sill));
    h.inward = to_f32(radial * -1.0);
    h.up = to_f32(l.t);
    h.width = p.hollow_width_m;
    h.height = p.hollow_height_m;
    h.depth = static_cast<f32>(d.depth);
    out.push_back(h);
  }
  return true;
}

f32 load_bearing_min_radius(std::string_view species) noexcept {
  // The smallest base radius any load-bearing part reaches inside the species' ranges: the
  // thinnest third stem of the thinnest birch (0.03 m x 0.65), the thinnest trunks of the willow
  // and the orchard tree, and the megaflora's highest limb past its run at the thinnest limb
  // radius (0.5 m x 0.65 x 0.85).
  if (species == k_species_birch) return 0.0195f;
  if (species == k_species_willow) return 0.08f;
  if (species == k_species_orchard) return 0.04f;
  if (species == k_species_ancient) return 0.27f;
  return 0.0f;
}

}  // namespace engine::flora
