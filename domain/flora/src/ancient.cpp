// The ancient tree's solver, the megaflora regime (trees.h, structure.h; docs/subsystems/flora.md,
// "Megaflora"; the habits and the limbs' mechanics, docs/experiments/megaflora-habits.md).
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
  stubs = 55,
  segments = 4096,  // a limb's named segments: limb * 512 + segment from here
  crown = 1000000,  // the recursion's shoots, one stream each from here
};

constexpr u32 k_none = 0xFFFFFFFFu;
constexpr f64 k_sink_m = 1.0;      // the foot below the ground, so the flare meets any ground
constexpr f64 k_fin_width = 0.12;  // a buttress's half-width at its root, a share of the gap
constexpr f64 k_run_inset = 0.5;   // a limb's run starts this share of the core radius out
constexpr f64 k_run_taper = 0.85;  // a run's tip radius over its base's
constexpr u32 k_crown_rows = 16;   // the structure's resolution past the runs
constexpr u32 k_part_cap = 2000;   // the recursion's motion parts: thousands a tree, not millions

// ---- the limbs' mechanics (flora.md, "Limbs a wood could carry"; megaflora-habits.md) ----------
//
// A limb system carries about its base's cross-section of wood all the way out to its tips (the
// pipe model, Shinozaki et al. 1964; Leonardo's rule), so its weight per metre of path is the
// wood's green weight times that section, and the bending moment at the base of each of its
// segments is that weight times its lever arm, summed over everything the segment carries. The
// stress 4M/(pi r^3) at every segment's base is held under the green wood's modulus of rupture
// times the duration-of-load factor over the static safety factor; a limb asked to reach further is
// shortened, run and all, until it is.
constexpr f64 k_gravity = 9.81;       // m/s^2
constexpr f64 k_load_duration = 0.6;  // a decade's sustained load over the ten-minute test's
constexpr f64 k_safety = 2.0;         // the static factor of safety a stem keeps (Niklas 1997)
constexpr f64 k_lateral = 2.5;  // a limb segment's crown laterals, in branch_length of its length
constexpr f64 k_crown_load = 2.5;         // the crown past a terminal, in its laterals' lengths
constexpr f64 k_sag_deg = 14.0;           // the droop of a level segment at its wood's limit
constexpr f64 k_segment_taper = 0.92;     // an inner segment's tip radius over its base's
constexpr f64 k_load_bearing_min = 0.25;  // the thinnest named limb part flagged load-bearing

// ---- the habits (megaflora-habits.md has the sources for every number) --------------------------

struct Habit {
  f64 low = 0.0;   // the lowest limb, a share of the height
  f64 high = 0.0;  // the highest limb
  u32 limbs = 0;
  f64 limb_share = 0.0;  // the lowest limb's radius over the trunk's core where it leaves
  f64 thinning = 0.0;    // the highest limb's radius loss
  f64 spread = 0.0;      // the crown's width over the tree's height
  f64 run_share = 0.0;   // the run's share of a limb's length
  f64 rise_gain = 0.0;   // degrees the highest run rises above the lowest
  f64 ascent0 = 0.0;     // degrees the limb past its run turns up (at upturn 1)
  f64 ascent = 0.0;      // degrees each later segment turns up
  u32 forks = 0;
  f64 shrink = 0.0;      // a fork daughter's length over its parent's
  f64 fork_angle = 0.0;  // degrees between a fork's two daughters
  f64 fork_roll = 0.0;   // degrees a fork's plane tilts from level
  f64 meander = 0.0;     // degrees a segment swings from side to side
  f64 cone = 0.0;        // the highest limb's reach loss
  bool excurrent = false;
  f64 broken = 0.0;  // the chance a last fork's lesser daughter is a broken stub
  u32 stubs = 0;     // dead stubs on the clear bole
};

// The giant sequoia and the old Douglas-fir: first big limb at about half the height, a crown
// about 0.4 of the height wide, a few great limbs leaving near level and turning up, crooked, the
// trunk through to the top.
constexpr Habit k_emergent{.low = 0.5,
                           .high = 0.75,
                           .limbs = 7,
                           .limb_share = 0.42,
                           .thinning = 0.45,
                           .spread = 0.6,
                           .run_share = 0.12,
                           .rise_gain = 25.0,
                           .ascent0 = 50.0,
                           .ascent = 25.0,
                           .forks = 2,
                           .shrink = 0.8,
                           .fork_angle = 50.0,
                           .fork_roll = 60.0,
                           .meander = 35.0,
                           .cone = 0.35,
                           .excurrent = true,
                           .broken = 0.2,
                           .stubs = 4};
// The open-grown oak and the monkeypod: the trunk dividing at a fifth to a third of the height
// into a few great limbs that fork again and again into a dome half as wide again as it is tall.
constexpr Habit k_spreading{.low = 0.2,
                            .high = 0.3,
                            .limbs = 6,
                            .limb_share = 0.45,
                            .thinning = 0.15,
                            .spread = 1.5,
                            .run_share = 0.15,
                            .rise_gain = 45.0,
                            .ascent0 = 15.0,
                            .ascent = 15.0,
                            .forks = 4,
                            .shrink = 0.85,
                            .fork_angle = 60.0,
                            .fork_roll = 15.0,
                            .meander = 28.0,
                            .cone = 0.0,
                            .excurrent = false,
                            .broken = 0.1,
                            .stubs = 0};
// An emergent's bole carrying a spreading crown.
constexpr Habit k_mixed{.low = 0.55,
                        .high = 0.7,
                        .limbs = 8,
                        .limb_share = 0.42,
                        .thinning = 0.2,
                        .spread = 0.9,
                        .run_share = 0.15,
                        .rise_gain = 35.0,
                        .ascent0 = 15.0,
                        .ascent = 15.0,
                        .forks = 4,
                        .shrink = 0.85,
                        .fork_angle = 60.0,
                        .fork_roll = 20.0,
                        .meander = 25.0,
                        .cone = 0.1,
                        .excurrent = false,
                        .broken = 0.12,
                        .stubs = 3};

const Habit& habit_of(AncientHabit h) {
  switch (h) {
    case AncientHabit::spreading: return k_spreading;
    case AncientHabit::mixed: return k_mixed;
    case AncientHabit::emergent: break;
  }
  return k_emergent;
}

// The stated fields and the habit's, resolved: what every function below reads.
struct Shape {
  const Habit* h = &k_emergent;
  f64 low = 0.0;
  f64 high = 0.0;
  u32 limbs = 0;
  f64 spread = 0.0;
  u32 forks = 0;
  f64 allowable = 0.0;  // Pa
  f64 weight = 0.0;     // N/m^3, the green wood's
};

Shape shape_of(const Ancient& p) {
  Shape s;
  s.h = &habit_of(p.habit);
  const Habit& h = *s.h;
  const f64 height = static_cast<f64>(p.height_m);
  const f64 low = static_cast<f64>(p.limb_low_m);
  const f64 high = static_cast<f64>(p.limb_high_m);
  s.low = low > 0.0 ? low : h.low * height;
  s.high = high > 0.0 ? high : h.high * height;
  if (low <= 0.0 && s.low > s.high) s.low = s.high;
  if (high <= 0.0 && s.high < s.low) s.high = s.low;
  s.limbs = p.limbs > 0 ? p.limbs : h.limbs;
  s.spread = p.crown_spread > 0.0f ? static_cast<f64>(p.crown_spread) : h.spread;
  s.forks = p.forks > 0 ? p.forks : h.forks;
  s.allowable = static_cast<f64>(p.wood_rupture_mpa) * 1.0e6 * k_load_duration / k_safety;
  s.weight = static_cast<f64>(p.wood_density) * k_gravity;
  return s;
}

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
// The trunk's top: an excurrent trunk carries through, 8% of the height past its highest limb and
// 0.8 to 0.9 of it; a decurrent one ends where it dissolves into its limbs.
f64 trunk_top_y(const Ancient& p, const Shape& s) {
  const f64 h = static_cast<f64>(p.height_m);
  if (!s.h->excurrent) {
    f64 top = s.high + (0.02 * h > 1.0 ? 0.02 * h : 1.0);
    if (top > 0.9 * h) top = 0.9 * h;
    return top;
  }
  f64 top = s.high + 0.08 * h;
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

Layout lay_out(const Ancient& p, const Shape& s) {
  Layout l;
  Rng trunk_rng = stream_of(p.seed, static_cast<u64>(Stream::trunk));
  const f64 azimuth = trunk_rng.unit() * 2.0 * k_pi;
  const f64 lean = static_cast<f64>(p.lean_deg) * k_degree;
  const D3 heading{det::cos(azimuth), 0.0, det::sin(azimuth)};
  l.t = normalized(k_up * det::cos(lean) + heading * det::sin(lean));
  l.n = perpendicular(l.t, D3{1.0, 0.0, 0.0});
  l.b = cross(l.t, l.n);
  l.base = D3{0.0, -k_sink_m, 0.0};
  l.length = (trunk_top_y(p, s) + k_sink_m) / l.t.y;
  l.r0 = static_cast<f64>(p.trunk_radius_m);
  l.r1 = l.r0 * static_cast<f64>(p.taper);
  if (!s.h->excurrent) {
    // A decurrent trunk stops where it dissolves into its limbs, so it tapers only as far as an
    // excurrent one of the tree's height would have by then: `taper` is the core's share at the
    // tree's top, and the limbs take the section the trunk still has (the pipe rule).
    const f64 top = trunk_top_y(p, s) / static_cast<f64>(p.height_m);
    l.r1 = l.r0 * (1.0 - (1.0 - static_cast<f64>(p.taper)) * top);
  }

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
// The named orders — the trunk, every major limb's straight run, and past it the limb's own
// segments, meandering and forking as the habit says, each limb no longer than its wood could
// carry — and from every limb's last segments, a little from its inner ones, and from the trunk's
// top the allometric recursion (kit.h, "the allometric crown") down to the species' own twig. Grown
// once; it never depends on the budget.

// A limb's draws, made once, before anything is sized: the wood's cap only ever scales what they
// say, so a limb shortened by its wood is the same limb, shorter.
struct LimbDraw {
  f64 f = 0.0;       // up the limb band, 0 to 1
  f64 arc = 0.0;     // where the run leaves the trunk's axis
  D3 dir;            // the run's direction
  D3 pivot;          // the run's base, inside the trunk
  f64 core = 0.0;    // the trunk's core radius there
  f64 radius = 0.0;  // the run's base radius
  f64 run = 0.0;     // asked
  f64 past = 0.0;    // asked: the path past the run to the farthest tip
  f64 run_min = 0.0;
};

f64 limb_radius_of(const Ancient& p, const Shape& s, const Layout& l) {
  if (p.limb_radius_m > 0.0f) return static_cast<f64>(p.limb_radius_m);
  f64 r = s.h->limb_share * core_radius(l, (s.low + k_sink_m) / l.t.y);
  if (r < 0.5) r = 0.5;
  if (r > 2.5) r = 2.5;
  return r;
}

Vector<LimbDraw> limb_draws(const Ancient& p, const Shape& s, const Layout& l) {
  const Habit& h = *s.h;
  Vector<LimbDraw> out;
  Rng rng = stream_of(p.seed, static_cast<u64>(Stream::limbs));
  const f64 turn = rng.unit() * 2.0 * k_pi;
  const f64 height = static_cast<f64>(p.height_m);
  const f64 radius = limb_radius_of(p, s, l);
  for (u32 i = 0; i < s.limbs; ++i) {
    LimbDraw d;
    d.f = s.limbs > 1 ? static_cast<f64>(i) / static_cast<f64>(s.limbs - 1) : 0.0;
    const f64 y = s.low + (s.high - s.low) * d.f;
    d.arc = (y + k_sink_m) / l.t.y;
    d.core = core_radius(l, d.arc);
    // Five draws a limb, always: where round the trunk, its rise, its vigour, its thickness and
    // its reach.
    const f64 around_j = rng.symmetric();
    const f64 rise_j = rng.symmetric();
    const f64 vigour_j = rng.unit();
    const f64 radius_j = rng.unit();
    const f64 reach_j = rng.unit();
    const f64 around = turn + static_cast<f64>(i) * k_golden_angle + 0.45 * around_j;
    f64 rise = static_cast<f64>(p.limb_rise_deg) + h.rise_gain * d.f + 3.0 * rise_j;
    if (rise > 45.0) rise = 45.0;
    if (rise < -5.0) rise = -5.0;
    rise *= k_degree;
    D3 out_dir = radial_at(l, around);
    out_dir = normalized(D3{out_dir.x, 0.0, out_dir.z});
    d.dir = normalized(out_dir * det::cos(rise) + k_up * det::sin(rise));
    d.pivot = l.base + l.t * d.arc + out_dir * (k_run_inset * d.core);
    d.radius = radius * (1.0 - h.thinning * d.f) * (0.9 + 0.2 * radius_j);
    d.run_min = (1.0 - k_run_inset) * d.core + 2.0;
    // The habit's length: out to the crown's edge, the upper limbs of a cone shorter.
    f64 reach = 0.5 * s.spread * height - d.core;
    if (reach < 6.0) reach = 6.0;
    reach *= (1.0 - h.cone * d.f) * (0.85 + 0.3 * vigour_j);
    if (p.limb_run_m > 0.0f) {
      d.run = static_cast<f64>(p.limb_run_m) * (1.0 - 0.35 * d.f);
    } else {
      d.run = h.run_share * reach;
      if (d.run < k_run_inset * d.core + 2.5) d.run = k_run_inset * d.core + 2.5;
    }
    if (d.run < d.run_min) d.run = d.run_min;
    if (p.limb_reach_m > 0.0f) {
      d.past = static_cast<f64>(p.limb_reach_m) * (1.0 - 0.4 * d.f) * (0.9 + 0.2 * reach_j);
    } else {
      d.past = reach - d.run;
      if (d.past < 0.4 * reach) d.past = 0.4 * reach;
      d.past *= 0.9 + 0.2 * reach_j;
    }
    if (d.past < 4.0) d.past = 4.0;
    out.push_back(d);
  }
  return out;
}

// A limb segment's centre line: `rows` even steps from its base, sagging under its load over its
// first half and turning up towards the light over its second (the oak's elbow; reaction wood
// recurving a loaded limb, Alméras and Fournier 2009), swinging from side to side by `meander`
// on a seeded wave, and wandering a little at every step. Frames are transported as `grow_axis`'s.
Axis grow_limb(D3 base, D3 direction, f64 length, u32 rows, f64 sag, f64 rise, f64 meander,
               f64 phase, f64 waves, f64 wiggle, Rng& rng) {
  Axis a;
  a.length = length;
  const f64 step = length / static_cast<f64>(rows);
  D3 t = normalized(direction);
  D3 n = perpendicular(t, (t.y < 0.9 && t.y > -0.9) ? k_up : D3{1.0, 0.0, 0.0});
  D3 p = base;
  const f64 per_row = 1.0 / static_cast<f64>(rows);
  for (u32 r = 0; r <= rows; ++r) {
    a.point.push_back(p);
    a.tangent.push_back(t);
    a.normal.push_back(n);
    a.arc.push_back(step * static_cast<f64>(r));
    if (r == rows) break;
    const f64 u = (static_cast<f64>(r) + 0.5) * per_row;
    // Down over the first 55%, up over the last 60%: the sag taken back and the rise beyond it.
    f64 vertical = 0.0;
    if (u < 0.55) vertical -= sag / 0.55 * per_row;
    if (u > 0.4) vertical += (sag + rise) / 0.6 * per_row;
    D3 next = t;
    const D3 hinge = cross(t, k_up);
    const f64 s = length_of(hinge);
    if (s > 1e-6 && !(vertical > 0.0 && t.y > 0.97) && !(vertical < 0.0 && t.y < -0.6))
      next = rotate(next, hinge * (1.0 / s), vertical);
    // The side to side swing: the heading follows meander * sin(2 pi (phase + waves u)).
    const f64 swing =
        meander * 2.0 * k_pi * waves * det::cos(2.0 * k_pi * (phase + waves * u)) * per_row;
    if (next.y < 0.95 && next.y > -0.95) next = rotate(next, perpendicular(next, k_up), swing);
    if (wiggle > 0.0) {
      const D3 b = cross(next, n);
      next = rotate(next, n, wiggle * rng.symmetric());
      next = rotate(next, normalized(b), wiggle * rng.symmetric());
    }
    next = normalized(next);
    p = p + normalized(t + next) * step;
    t = next;
    n = perpendicular(t, n);
  }
  return a;
}

// A limb's named segments past its run, in preorder: a segment's descendants follow it, up to
// `end`.
struct Segment {
  Axis axis;
  f64 r0 = 0.0;
  f64 r1 = 0.0;
  u32 parent = k_none;  // into the limb's segments; k_none for the first, whose parent is the run
  u32 end = 0;
  u32 level = 0;
  bool terminal = false;  // the crown grows from it
  bool broken = false;    // a broken stub: short, blunt, bare
};

struct LimbGeo {
  Axis run;
  f64 run_r0 = 0.0;
  f64 run_r1 = 0.0;
  Vector<Segment> segs;
  f64 branch_length = 0.0;  // the recipe's: a segment's laterals over its length, by k_lateral
  f64 renewal_length = 0.0;
  f64 renewal_arc = 0.0;
  f64 renewal_load_arc = 0.0;
};

struct LimbBuilder {
  const Ancient& p;
  const Shape& s;
  const CrownSpec& spec;
  u32 limb = 0;
  const Vector<f64>* sag = nullptr;  // each segment's load ratio, for its droop
  LimbGeo geo;

  void segment(u32 parent, D3 base, D3 dir, f64 r0, f64 nominal, u32 level, bool broken) {
    const Habit& h = *s.h;
    const u32 index = geo.segs.size();
    geo.segs.push_back(Segment{});
    Rng r = stream_of(p.seed, static_cast<u64>(Stream::segments) + 512u * limb + index);
    // Nine draws a segment, always.
    const f64 length_j = r.unit();
    const f64 phase = r.unit();
    const f64 waves_j = r.unit();
    const u64 wiggle_seed = r.next();
    const f64 side = r.unit();
    const f64 roll_j = r.symmetric();
    const f64 share_j = r.symmetric();
    const f64 three_j = r.unit();
    const f64 broken_j = r.unit();
    f64 length = nominal * (0.85 + 0.3 * length_j);
    if (broken) length *= 0.3;
    const f64 level_c2 = 1.0 - dir.y * dir.y;  // cos^2 of its elevation: only a level limb sags
    const f64 ratio = sag != nullptr && index < sag->size() ? (*sag)[index] : 0.0;
    const f64 droop = k_sag_deg * k_degree * ratio * level_c2;
    const f64 upturn = 0.6 + 0.4 * static_cast<f64>(p.upturn);
    const f64 rise = broken ? 0.0 : (level == 0 ? h.ascent0 : h.ascent) * k_degree * upturn;
    Rng wiggle(wiggle_seed);
    Axis axis = grow_limb(base, dir, length, k_crown_rows, droop, rise, h.meander * k_degree, phase,
                          0.5 + waves_j, 0.5 * static_cast<f64>(p.wander_deg) * k_degree, wiggle);
    const bool terminal = !broken && level >= s.forks;
    const f64 r1 =
        terminal ? crown_leader_radius(spec, r0) : r0 * (broken ? 0.85 : k_segment_taper);
    const D3 tip = axis.point.back();
    const D3 t = axis.tangent.back();
    const D3 last_normal = axis.normal.back();
    {
      Segment& seg = geo.segs[index];
      seg.axis = std::move(axis);
      seg.r0 = r0;
      seg.r1 = r1;
      seg.parent = parent;
      seg.level = level;
      seg.terminal = terminal;
      seg.broken = broken;
    }
    if (!terminal && !broken) {
      // The fork: two daughters (at the first fork sometimes three) sharing the parent's section
      // by the pipe rule, the greater one deviating the less — their sections' directions sum to
      // the parent's (Murray 1927) — in a plane the habit tilts from level.
      D3 across = cross(k_up, t);
      if (length_of(across) < 1e-6) across = last_normal;
      across = normalized(across);
      const D3 upish = normalized(cross(t, across));
      const f64 roll = (h.fork_roll + 25.0 * roll_j) * k_degree;
      D3 e = across * det::cos(roll) + upish * det::sin(roll);
      if (side < 0.5) e = e * -1.0;
      const bool three = level == 0 && s.forks >= 2 && three_j < 0.2;
      const f64 a1 = three ? 0.44 : 0.62 + 0.12 * share_j;
      const f64 a2 = three ? 0.32 : 1.0 - a1;
      const f64 total = h.fork_angle * (1.0 + 0.15 * share_j) * k_degree;
      const f64 ct = det::cos(total);
      const f64 st = det::sin(total);
      const f64 rx = a1 + a2 * ct;
      const f64 ry = a2 * st;
      const f64 rr = std::sqrt(rx * rx + ry * ry);
      const f64 c1 = rx / rr;
      const f64 s1 = ry / rr;
      const f64 c2 = ct * c1 + st * s1;
      const f64 s2 = st * c1 - ct * s1;
      const D3 d1 = normalized(t * c1 + e * s1);
      const D3 d2 = normalized(t * c2 - e * s2);
      const f64 inv = 1.0 / static_cast<f64>(p.area_exponent);
      const bool last = level + 1 >= s.forks;
      const bool snap = last && broken_j < h.broken;
      const f64 next = nominal * h.shrink;
      segment(index, tip, d1, r1 * det::exp(det::log(a1) * inv), next, level + 1, false);
      segment(index, tip, d2, r1 * det::exp(det::log(a2) * inv), next, level + 1, snap && !three);
      if (three) {
        const D3 d3 =
            normalized(t * det::cos(0.5 * total) + normalized(cross(t, e)) * det::sin(0.5 * total));
        segment(index, tip, d3, r1 * det::exp(det::log(1.0 - a1 - a2) * inv), next, level + 1,
                snap);
      }
    }
    geo.segs[index].end = geo.segs.size();
  }

  void build(const LimbDraw& d, f64 k) {
    geo = LimbGeo{};
    f64 run = d.run * k;
    if (run < d.run_min) run = d.run_min;
    {
      Rng none(0);
      geo.run = grow_axis(d.pivot, d.dir, run, 8, k_up, 0.0, 0.0, none);
    }
    geo.run_r0 = d.radius;
    geo.run_r1 = d.radius * k_run_taper;
    f64 sum = 0.0;
    f64 term = 1.0;
    for (u32 j = 0; j <= s.forks; ++j) {
      sum += term;
      term *= s.h->shrink;
    }
    const f64 past = d.past * k;
    geo.branch_length = static_cast<f64>(p.branch_length);
    geo.renewal_length = geo.branch_length * (run + 0.35 * past);
    geo.renewal_arc =
        p.limb_run_m > 0.0f ? std::min(0.55 * run, k_run_inset * d.core + 0.25) : 0.55 * run;
    geo.renewal_load_arc = p.limb_run_m > 0.0f ? geo.renewal_arc : 0.7 * run;
    segment(k_none, geo.run.point.back(), d.dir, geo.run_r1, past / sum, 0, false);
  }
};

// The load ratio — bending stress over the allowable — at the base of the run (element 0) and of
// every segment (1 + j), by the rule at the top of the file.
f64 load_ratios(const LimbGeo& g, const Shape& s, Vector<f64>& ratios) {
  struct Sample {
    D3 at;
    f64 w = 0.0;
    u32 seg = 0;  // k_none for the run's own
  };
  Vector<Sample> samples;
  const auto add_axis = [&](const Axis& a, f64 radius, u32 seg) {
    const f64 area = k_pi * radius * radius;
    for (u32 q = 0; q + 1 < a.point.size(); ++q) {
      Sample sm;
      sm.at = (a.point[q] + a.point[q + 1]) * 0.5;
      sm.w = s.weight * area * length_of(a.point[q + 1] - a.point[q]);
      sm.seg = seg;
      samples.push_back(sm);
    }
  };
  add_axis(g.run, g.run_r0, k_none);
  // Renewed shoots grow on the run too. Include their conservative wood-equivalent load in
  // the same mechanics that caps the rest of the limb; fullness must not bypass that cap.
  {
    Sample sm;
    const D3 t = g.run.tangent.back();
    sm.at = axis_point(g.run, g.renewal_load_arc) + D3{t.x, 0.0, t.z} * (0.35 * g.renewal_length);
    sm.w = s.weight * k_pi * g.run_r1 * g.run_r1 * 0.09 * (1.0 + k_crown_load) * g.renewal_length;
    sm.seg = k_none;
    samples.push_back(sm);
  }
  for (u32 j = 0; j < g.segs.size(); ++j) {
    const Segment& seg = g.segs[j];
    add_axis(seg.axis, seg.r0, j);
    if (!seg.broken) {
      // The crown it carries, at a lever a third of its reach on along the segment's heading.
      const f64 reach = k_crown_load * k_lateral * g.branch_length * seg.axis.length;
      D3 heading = seg.axis.tangent.back();
      heading = D3{heading.x, 0.0, heading.z};
      Sample sm;
      sm.at = seg.axis.point.back() + heading * (0.35 * reach);
      sm.w = s.weight * k_pi * seg.r0 * seg.r0 * reach * (seg.terminal ? 1.0 : 0.363);
      sm.seg = j;
      samples.push_back(sm);
    }
  }
  const auto ratio_at = [&](D3 base, f64 radius, u32 first, u32 end, bool all) {
    f64 mx = 0.0;
    f64 mz = 0.0;
    for (const Sample& sm : samples) {
      if (!all && (sm.seg == k_none || sm.seg < first || sm.seg >= end)) continue;
      mx += sm.w * (sm.at.x - base.x);
      mz += sm.w * (sm.at.z - base.z);
    }
    const f64 moment = std::sqrt(mx * mx + mz * mz);
    return 4.0 * moment / (k_pi * radius * radius * radius) / s.allowable;
  };
  ratios.clear();
  ratios.push_back(ratio_at(g.run.point[0], g.run_r0, 0, 0, true));
  f64 peak = ratios[0];
  for (u32 j = 0; j < g.segs.size(); ++j) {
    const Segment& seg = g.segs[j];
    const f64 r = ratio_at(seg.axis.point[0], seg.r0, j, seg.end, false);
    ratios.push_back(r);
    if (r > peak) peak = r;
  }
  return peak;
}

// What one limb came to: the geometry, and how much of the asked length the wood allowed.
struct Limb {
  LimbGeo geo;
  f64 scale = 1.0;
  f64 peak = 0.0;  // the largest load ratio left
  Vector<f64> ratios;
};

Limb solve_limb(const Ancient& p, const Shape& s, const CrownSpec& spec, const LimbDraw& d,
                u32 index) {
  LimbBuilder b{p, s, spec, index, nullptr, {}};
  b.build(d, 1.0);
  Vector<f64> first;
  const f64 peak = load_ratios(b.geo, s, first);
  Limb out;
  out.scale = peak > 1.0 ? std::sqrt(1.0 / peak) : 1.0;
  // The droop follows how near its limit each segment is once the limb is sized: a scale of k
  // multiplies every moment by k^2 and leaves the sections.
  Vector<f64> sag;
  for (u32 j = 1; j < first.size(); ++j) {
    const f64 r = first[j] * out.scale * out.scale;
    sag.push_back(r > 1.0 ? 1.0 : r);
  }
  b.sag = &sag;
  for (u32 pass = 0; pass < 6; ++pass) {
    b.build(d, out.scale);
    out.peak = load_ratios(b.geo, s, out.ratios);
    if (out.peak <= 1.0) break;
    out.scale *= std::sqrt(1.0 / out.peak) * 0.999;
  }
  out.geo = std::move(b.geo);
  return out;
}

struct Stub {
  Axis axis;
  f64 r0 = 0.0;
  f64 r1 = 0.0;
};

struct Named {
  u32 trunk = 0;
  Vector<u32> runs;
  Vector<u32> limbs;  // each limb's first segment past its run
  Vector<u32> forks;  // every later segment, limb by limb, parents first
  Vector<u32> stubs;  // the dead stubs on the bole
  Vector<AncientLimb> report;
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

// Dead stubs on an emergent's clear bole: the lower limbs it shed, broken off short and drooping.
Vector<Stub> stubs_of(const Ancient& p, const Shape& s, const Layout& l, f64 limb_radius) {
  Vector<Stub> out;
  const u32 count = s.h->stubs;
  if (count == 0) return out;
  const f64 height = static_cast<f64>(p.height_m);
  f64 lo = 0.25 * height;
  if (lo < 1.2 * static_cast<f64>(p.buttress_height_m))
    lo = 1.2 * static_cast<f64>(p.buttress_height_m);
  for (const Doorway& d : l.doors) {
    const f64 top = d.lintel * l.t.y - k_sink_m + 2.0;
    if (lo < top) lo = top;
  }
  const f64 hi = s.low - 3.0;
  Rng rng = stream_of(p.seed, static_cast<u64>(Stream::stubs));
  for (u32 k = 0; k < count; ++k) {
    // Five draws a stub, always, whether or not there is room for it.
    const f64 y_j = rng.unit();
    const f64 around = rng.unit() * 2.0 * k_pi;
    const f64 droop_j = rng.unit();
    const f64 length_j = rng.unit();
    const f64 radius_j = rng.unit();
    if (hi <= lo) continue;
    const f64 y = lo + (hi - lo) * y_j;
    const f64 arc = (y + k_sink_m) / l.t.y;
    const f64 core = core_radius(l, arc);
    D3 out_dir = radial_at(l, around);
    out_dir = normalized(D3{out_dir.x, 0.0, out_dir.z});
    const f64 droop = (-15.0 + 20.0 * droop_j) * k_degree;
    const D3 dir = normalized(out_dir * det::cos(droop) + k_up * det::sin(droop));
    Stub stub;
    const f64 length = 0.4 * core + 1.2 + 2.3 * length_j;
    Rng none(0);
    stub.axis = grow_axis(l.base + l.t * arc + out_dir * (0.6 * core), dir, length, 2, k_up, 0.0,
                          0.0, none);
    stub.r0 = (0.12 + 0.18 * radius_j) * limb_radius;
    if (stub.r0 < 0.15) stub.r0 = 0.15;
    stub.r1 = 0.8 * stub.r0;
    out.push_back(std::move(stub));
  }
  return out;
}

Crown grow(const Ancient& p, const Shape& s, const Layout& l, const CrownSpec& spec, Named& named) {
  Crown crown;
  {
    Rng none(0);
    const Axis trunk = grow_axis(l.base, l.t, l.length, 32, k_up, 0.0, 0.0, none);
    named.trunk = crown_add_named(crown, trunk, l.r0, l.r1, k_crown_none);
  }
  const Vector<LimbDraw> draws = limb_draws(p, s, l);
  Vector<CrownRoot> roots;
  f64 reach_sum = 0.0;
  for (u32 i = 0; i < draws.size(); ++i) {
    const LimbDraw& d = draws[i];
    const Limb limb = solve_limb(p, s, spec, d, i);
    const LimbGeo& g = limb.geo;
    const u32 run = crown_add_named(crown, g.run, g.run_r0, g.run_r1, named.trunk);
    named.runs.push_back(run);
    // A separate carrier keeps every renewal vertex off the exact straight-run part. Otherwise
    // fine shoots folded by the part budget would violate that part's anchor-envelope contract.
    Rng renewal_rng = stream_of(p.seed, 800000ull + i);
    const f64 renewal_length = g.renewal_length;
    // An explicit run reserves an unobstructed platform/walking span. Keep its renewed upright
    // at the trunk-side crotch; automatic runs can renew farther along their short scaffold.
    const Axis renewal_axis =
        grow_axis(axis_point(g.run, g.renewal_arc), normalized(k_up * 0.85 + d.dir * 0.35),
                  renewal_length, 8, k_up, 0.4 / renewal_length, 0.05, renewal_rng);
    const f64 renewal_radius = 0.3 * g.run_r1;
    const u32 renewal_node = crown_add_named(crown, renewal_axis, renewal_radius,
                                             crown_leader_radius(spec, renewal_radius), run);
    named.forks.push_back(renewal_node);
    CrownRoot renewal;
    renewal.node = renewal_node;
    renewal.radius = renewal_radius;
    renewal.laterals = 4;
    renewal.lateral_length = renewal_length;
    renewal.from = 0.3;
    renewal.to = 0.95;
    renewal.spread_scale = 0.8;
    roots.push_back(renewal);
    Vector<u32> nodes;
    AncientLimb report;
    report.height_m = static_cast<f32>(d.arc * l.t.y - k_sink_m);
    report.base_radius_m = static_cast<f32>(g.run_r0);
    report.asked_m = static_cast<f32>(d.run + d.past);
    report.scale = static_cast<f32>(limb.scale);
    report.allowable_mpa = static_cast<f32>(s.allowable * 1.0e-6);
    f64 peak = 0.0;
    for (const f64 r : limb.ratios)
      if (r > peak) peak = r;
    report.peak_stress_mpa = static_cast<f32>(peak * s.allowable * 1.0e-6);
    const D3 axis_at = l.base + l.t * d.arc;
    f64 reach = 0.0;
    for (u32 j = 0; j < g.segs.size(); ++j) {
      const Segment& seg = g.segs[j];
      const u32 parent = seg.parent == k_none ? run : nodes[seg.parent];
      const u32 node = crown_add_named(crown, seg.axis, seg.r0, seg.r1, parent);
      nodes.push_back(node);
      if (j == 0) {
        named.limbs.push_back(node);
      } else {
        named.forks.push_back(node);
      }
      const D3 off = seg.axis.point.back() - axis_at;
      const f64 flat = std::sqrt(off.x * off.x + off.z * off.z);
      if (flat > reach) reach = flat;
      if (seg.broken) {
        ++report.broken;
        continue;
      }
      CrownRoot root;
      root.node = node;
      if (seg.terminal) {
        root.radius = seg.r0;
        root.laterals = p.branches;
        root.lateral_length = k_lateral * g.branch_length * seg.axis.length;
        root.from = 0.15;
        root.to = 0.95;
      } else {
        // A few shoots on the inner limb: an old limb's inside is mostly bare, its leaves out where
        // the light is.
        root.radius = 0.55 * seg.r0;
        root.laterals = 4;
        root.lateral_length = 1.2 * k_lateral * g.branch_length * seg.axis.length;
        root.from = 0.15;
        root.to = 0.85;
        root.spread_scale = 1.2;
      }
      roots.push_back(root);
    }
    report.segments = g.segs.size();
    report.reach_m = static_cast<f32>(reach);
    named.report.push_back(report);
    reach_sum += d.past * limb.scale;
  }
  const f64 mean_past = draws.empty() ? 0.0 : reach_sum / static_cast<f64>(draws.size());
  for (const Stub& stub : stubs_of(p, s, l, limb_radius_of(p, s, l)))
    named.stubs.push_back(crown_add_named(crown, stub.axis, stub.r0, stub.r1, named.trunk));
  // A crown on the trunk's top: an excurrent trunk's leader, or where a decurrent one dissolves.
  CrownRoot top;
  top.node = named.trunk;
  top.radius = l.r1;
  top.laterals = p.branches;
  top.lateral_length = static_cast<f64>(p.branch_length) * mean_past * 0.8;
  top.from = 0.86;
  top.to = 1.0;
  top.spread_scale = 0.8;
  if (!s.h->excurrent) {
    // Where a decurrent trunk dissolves its limbs have taken nearly all its section: a few shoots
    // between them, not a crown of its own.
    top.radius = 0.3 * l.r1;
    top.laterals = 3;
    top.lateral_length = 0.5 * static_cast<f64>(p.branch_length) * mean_past;
    top.from = 0.7;
    top.spread_scale = 1.4;
  }
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
  u32 sides[3] = {0, 32, 24};  // by order: (trunk), run, the limb past it (the most a segment has)
  u32 rows[3] = {0, 12, 20};
  f64 named_edge_m = 0.2;  // a limb segment's circumference step
  f64 named_row_m = 1.5;   // a limb segment's rows: one per this many metres
  CrownPlan crown;
};

u32 segment_sides(const Plan& plan, f64 radius) {
  const u32 floor = plan.sides[2] < 6 ? plan.sides[2] : 6;
  const f64 s = 2.0 * k_pi * radius / plan.named_edge_m;
  if (s <= static_cast<f64>(floor)) return floor;
  return s >= static_cast<f64>(plan.sides[2]) ? plan.sides[2] : static_cast<u32>(s) + 1;
}
u32 segment_rows(const Plan& plan, f64 length) {
  const u32 floor = plan.rows[2] < 3 ? plan.rows[2] : 3;
  const f64 r = length / plan.named_row_m;
  if (r <= static_cast<f64>(floor)) return floor;
  return r >= static_cast<f64>(plan.rows[2]) ? plan.rows[2] : static_cast<u32>(r) + 1;
}
u32 stub_sides(const Plan& plan) { return plan.sides[2] < 8 ? plan.sides[2] : 8; }

u32 count_triangles(const Ancient& p, const Layout& l, const Crown& crown, const Named& named,
                    const Plan& plan) {
  const Grid g = trunk_grid(l, p, plan.trunk_rows, plan.trunk_sides, plan.fin_flanks);
  u32 n = trunk_triangles(g, windows_of(l, g)) +
          named.runs.size() * tube_triangles(plan.rows[1], plan.sides[1]);
  const auto segment = [&](u32 node) {
    const CrownNode& c = crown.nodes[node];
    n += tube_triangles(segment_rows(plan, c.length), segment_sides(plan, c.r0));
  };
  for (const u32 node : named.limbs)
    segment(node);
  for (const u32 node : named.forks)
    segment(node);
  n += named.stubs.size() * tube_triangles(2, stub_sides(plan));
  return n + crown_triangles(crown, plan.crown);
}

// The ladder (flora.md, "The budget"): the finest wood first, then rows and cross-sections from
// the finest order up, then the finer orders' wood folded into their parts one class at a time,
// then the trunk's rows and columns, and only then — where the budget forces it — leaf cards
// merged up the hierarchy, finest first, and the last of the rows and sides. The pinned columns
// stay on every rung, so a doorway is its size and a buttress keeps its ridge. No rung moves a
// shoot or drops a part: the skeleton, the part list and the structural table are the same bytes
// on every rung (graphics presets pick rungs; gameplay must not change with them).
bool plan_ancient(const Ancient& p, const Layout& l, const Crown& crown, const Named& named,
                  Plan& plan, u32& steps) {
  plan = Plan{};
  const u32 trunk_rows = static_cast<u32>(l.length / 0.6);
  plan.trunk_rows = trunk_rows < 24 ? 24 : (trunk_rows > 160 ? 160 : trunk_rows);
  plan.crown.edge_m = 0.1;
  plan.crown.max_sides = 12;
  plan.crown.row_m = 1.0;
  plan.crown.wood_class =
      2;  // shoots under two pipe steps over a twig (2.6 cm) fold from the start
  steps = 0;
  const auto fits = [&] { return count_triangles(p, l, crown, named, plan) <= p.triangle_budget; };
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
        plan.named_row_m = 3.0;
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
        plan.named_edge_m = 0.35;
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
        plan.named_edge_m = 0.6;
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
        plan.named_edge_m = 1.0;
        plan.named_row_m = 6.0;
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
    plan.named_edge_m = 2.0;
    plan.named_row_m = 10.0;
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
  // A limb field of 0 is the habit's.
  if (!(p.limbs == 0u || (p.limbs >= 2u && p.limbs <= 24u)))
    return bad("limbs", "0 (the habit's), or 2 to 24");
  if (!(p.limb_low_m == 0.0f || (p.limb_low_m >= 8.0f && p.limb_low_m <= 110.0f)))
    return bad("limb_low_m", "0 (the habit's), or 8 to 110 m");
  if (!(p.limb_high_m >= 0.0f)) return bad("limb_high_m", "0 (the habit's), or a height");
  if (!(p.limb_radius_m == 0.0f || (p.limb_radius_m >= 0.5f && p.limb_radius_m <= 2.5f)))
    return bad("limb_radius_m", "0 (the habit's), or 0.5 to 2.5 m");
  if (!(p.limb_rise_deg >= -5.0f && p.limb_rise_deg <= 30.0f))
    return bad("limb_rise_deg", "-5 to 30 degrees");
  if (!(p.limb_run_m == 0.0f || (p.limb_run_m >= 4.0f && p.limb_run_m <= 60.0f)))
    return bad("limb_run_m", "0 (the habit's), or 4 to 60 m");
  if (!(p.limb_reach_m == 0.0f || (p.limb_reach_m >= 4.0f && p.limb_reach_m <= 80.0f)))
    return bad("limb_reach_m", "0 (the habit's), or 4 to 80 m");
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
  if (p.card_bake_px != 0u && !(p.card_bake_px >= 16u && p.card_bake_px <= 256u))
    return bad("card_bake_px", "0, or 16 to 256");
  if (p.impostor_px != 0u && !(p.impostor_px >= 32u && p.impostor_px <= 1024u))
    return bad("impostor_px", "0, or 32 to 1024");
  if (!(p.habit == AncientHabit::emergent || p.habit == AncientHabit::spreading ||
        p.habit == AncientHabit::mixed)) {
    return bad("habit", "emergent, spreading or mixed");
  }
  if (!(p.crown_spread == 0.0f || (p.crown_spread >= 0.2f && p.crown_spread <= 2.5f)))
    return bad("crown_spread", "0 (the habit's), or 0.2 to 2.5");
  if (!(p.forks <= 4u)) return bad("forks", "0 (the habit's), or 1 to 4");
  if (!(p.wood_rupture_mpa >= 20.0f && p.wood_rupture_mpa <= 150.0f))
    return bad("wood_rupture_mpa", "20 to 150 MPa");
  if (!(p.wood_density >= 300.0f && p.wood_density <= 1300.0f))
    return bad("wood_density", "300 to 1300 kg/m3");
  // What the fields allow only together, the habit's filled in: the limb band inside the tree.
  const Shape s = shape_of(p);
  const f64 height = static_cast<f64>(p.height_m);
  if (!(s.low >= 8.0)) return bad("limb_low_m", "the lowest limb at least 8 m up");
  if (!(s.high >= s.low && s.high <= 0.85 * height))
    return bad("limb_high_m", "limb_low_m to 0.85 of height_m");
  // A stated run must leave the trunk: past the core where it starts, at every limb's height.
  const Layout l = lay_out(p, s);
  if (p.limb_run_m > 0.0f) {
    for (u32 i = 0; i < s.limbs; ++i) {
      const f64 f = static_cast<f64>(i) / static_cast<f64>(s.limbs - 1);
      const f64 y = s.low + (s.high - s.low) * f;
      const f64 r = core_radius(l, (y + k_sink_m) / l.t.y);
      if (static_cast<f64>(p.limb_run_m) * (1.0 - 0.35 * f) < (1.0 - k_run_inset) * r + 2.0) {
        return bad("limb_run_m",
                   "at least 2 m past the trunk's core at every limb, " +
                       std::to_string(static_cast<i32>((1.0 - k_run_inset) * r + 2.0)) +
                       " m and more here");
      }
    }
  }
  // A hollow is a doorway under the limbs, no wider than its trunk allows, and two never overlap.
  if (p.hollows > 0) {
    if (p.hollows > 1 && !(p.hollow_spacing_m >= p.hollow_height_m + 1.0f))
      return bad("hollow_spacing_m", "at least hollow_height_m + 1 m");
    const Doorway& top = l.doors.back();
    if (top.lintel * l.t.y - k_sink_m > s.low - 2.0)
      return bad("hollow_sill_m", "every hollow's top at least 2 m under the lowest limb");
    for (const Doorway& d : l.doors) {
      if (d.half_angle > 0.6)
        return bad("hollow_width_m", "at most 1.2 times the core radius at the hollow");
    }
  }
  return true;
}

bool solve_ancient(const Ancient& p, assets::MeshData& out, TreeReport* report, std::string* error,
                   Motion* motion) {
  return solve_ancient_chain(p, out, report, error, motion, nullptr);
}

bool solve_ancient_chain(const Ancient& p, assets::MeshData& out, TreeReport* report,
                         std::string* error, Motion* motion, TreeLevels* levels) {
  out = assets::MeshData{};
  std::string message;
  if (!ancient_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  const Shape shape = shape_of(p);
  const Layout l = lay_out(p, shape);
  const CrownSpec spec = spec_of(p);
  Named named;
  Crown crown = grow(p, shape, l, spec, named);
  place_leaves(p, crown);
  Plan plan;
  u32 steps = 0;
  if (!plan_ancient(p, l, crown, named, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Ancient.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tree: its coarsest form needs " +
               std::to_string(count_triangles(p, l, crown, named, plan)) + " triangles";
    }
    return false;
  }

  out.materials.push_back(material("bark", Vec4{0.27f, 0.22f, 0.17f, 1.0f}, 0.92f, false));
  out.materials.push_back(material("leaf", Vec4{0.09f, 0.22f, 0.06f, 1.0f}, 0.6f, true));
  Motion parts;
  Emitter emit{out, parts};
  emit.wood = WoodProfile{0.4, 0.0, 0.08, 0.06, 0.09, p.seed};

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

  // The runs and the first segments past them, in pairs; the runs straight. Then every later
  // segment, limb by limb, parents first; then the dead stubs. A limb part at least
  // `k_load_bearing_min` thick at its base is load-bearing.
  u32 load_bearing = 1;
  const auto bearing = [&](f64 r0) {
    if (r0 < k_load_bearing_min) return 0u;
    ++load_bearing;
    return k_load_bearing;
  };
  const auto emit_segment = [&](u32 node) {
    CrownNode& seg = crown.nodes[node];
    seg.part =
        emit_tube_part(emit, crown.axes[seg.axis], seg.r0, seg.r1, segment_rows(plan, seg.length),
                       segment_sides(plan, seg.r0), crown.nodes[seg.parent].part, bearing(seg.r0));
  };
  for (u32 i = 0; i < named.runs.size(); ++i) {
    CrownNode& run = crown.nodes[named.runs[i]];
    run.part =
        emit_tube_part(emit, crown.axes[run.axis], run.r0, run.r1, plan.rows[1], plan.sides[1],
                       crown.nodes[named.trunk].part, bearing(run.r0) | k_straight);
    emit_segment(named.limbs[i]);
  }
  for (const u32 node : named.forks)
    emit_segment(node);
  for (const u32 node : named.stubs) {
    CrownNode& stub = crown.nodes[node];
    stub.part = emit_tube_part(emit, crown.axes[stub.axis], stub.r0, stub.r1, 2, stub_sides(plan),
                               crown.nodes[named.trunk].part, 0u);
  }
  crown_emit_wood(crown, plan.crown, emit, true);
  assets::Primitive bark;
  bark.first_index = 0;
  bark.index_count = out.indices.size();
  bark.material = 0;
  out.primitives.push_back(bark);

  const u32 leaf_vertex = out.positions.size();
  const u32 leaf_first = out.indices.size();
  Vector<SprayRange> spray_ranges;
  const f64 card_length =
      crown_emit_leaves(crown, plan.crown, emit, static_cast<f64>(p.leaf_length_m),
                        static_cast<f64>(p.leaf_angle_deg) * k_degree, 15.0 * k_degree, 0.15,
                        levels != nullptr ? &spray_ranges : nullptr);
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
  // The impostor chain above the finest leaves (impostor.h): appended past everything, so the
  // mesh above is the bytes it is without it.
  if (levels != nullptr) {
    *levels = TreeLevels{};
    levels->leaf_first_index = leaf_first;
    levels->leaf_index_end = out.indices.size();
    ImpostorSpec chain_spec;
    chain_spec.card_px = p.card_bake_px;
    chain_spec.tree_px = p.impostor_px;
    crown_levels(crown, plan.crown, spray_ranges, chain_spec, emit, *levels);
    tree_star(chain_spec, crown.nodes[named.trunk].part, emit, *levels);
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

bool ancient_crown(const Ancient& p, Vector<CrownShoot>& out) {
  std::string error;
  if (!ancient_in_range(p, error)) return false;
  const Shape s = shape_of(p);
  const Layout l = lay_out(p, s);
  Named named;
  crown_table(grow(p, s, l, spec_of(p), named), out);
  return true;
}

bool ancient_hollows(const Ancient& p, Vector<Hollow>& out) {
  std::string error;
  if (!ancient_in_range(p, error)) return false;
  out.clear();
  const Layout l = lay_out(p, shape_of(p));
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

bool ancient_limbs(const Ancient& p, Vector<AncientLimb>& out) {
  std::string error;
  if (!ancient_in_range(p, error)) return false;
  const Shape s = shape_of(p);
  const Layout l = lay_out(p, s);
  Named named;
  grow(p, s, l, spec_of(p), named);
  out = std::move(named.report);
  return true;
}

f32 load_bearing_min_radius(std::string_view species) noexcept {
  // The smallest base radius any load-bearing part reaches inside the species' ranges: the
  // thinnest clump stem of the thinnest birch (0.03 m x 0.6), the thinnest trunks of the willow
  // and the orchard tree, and the megaflora's threshold: a named limb part is flagged
  // load-bearing only when it is at least `k_load_bearing_min` (0.25 m) at its base.
  if (species == k_species_birch) return 0.018f;
  if (species == k_species_willow) return 0.08f;
  if (species == k_species_orchard) return 0.04f;
  if (species == k_species_ancient) return static_cast<f32>(k_load_bearing_min);
  return 0.0f;
}

}  // namespace engine::flora
