// Water that leaves its bed (roadmap S104, stages 0 and 1; channels.md, "Where the water leaves
// its bed (S104)"; the design and its sources in docs/experiments/waterfalls.md): the fall test on
// the solver's own steps and the cook that marches each lip column along its ballistic path.
// Everything a decision rests on is + - x / and a correctly rounded square root, with the cube root
// a fixed Newton iteration, so the bytes are the same from every toolchain; links are cooked one a
// job and merged by link index, so they are the same at any thread count.
#include "detail.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/channels/falls.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace engine::channels {

namespace {
using namespace detail;

constexpr u32 k_falls_magic = 0x4C464843u;  // "CHFL"
static_assert(sizeof(FallVertex) == 28, "a vertex is written as its bytes: no padding");
static_assert(sizeof(FallColumn) == 28, "a column is written as its bytes: no padding");
static_assert(sizeof(Fall) == 88, "a fall is written as its bytes: no padding");

// The bed's half-implicit step (march_column): a step every this many metres of bed.
constexpr f64 k_bed_step_m = .05;
// The slowest a column runs on its bed (the water capability's `k_channel_min_speed`).
constexpr f64 k_min_speed = .05;
// A column leaves where the parabola stands more than this over the bed at the next station.
constexpr f64 k_leave_m = 1e-3;
// The cook's window about a run: from this many of the lip's widths above it (and at least a metre)
// to as many below its last point, as the design's profile (waterfalls.md, "What the description
// must carry"); a flight aloft at the window's end is followed to its landing.
constexpr f64 k_window_widths = 2.0;
// The approach slope is the bed's fall over the knickpoint's window above the lip.
constexpr f64 k_approach_widths = 3.0;
constexpr f64 k_approach_min_m = 2.0;
// A flight's path is sampled at a step that starts at a quarter of the sheet's thickness at the lip
// and grows with the path's length to a metre (waterfalls.md, "What is drawn": rows close together
// at the lip where it curves and up to a metre apart lower down).
constexpr f64 k_air_step_growth = .15;
constexpr f64 k_air_step_max_m = 1.0;
constexpr f64 k_air_step_min_m = .01;
constexpr u32 k_profile_beyond_points = 400;

f64 sq(f64 v) { return v * v; }

// A stretch whose bed falls at least its run is a face: a flight comes down on its rock. Elsewhere
// it comes down on the water standing over the bed, and the stretch it leaves over is its bed (the
// water flowing over a lip is the jet).
bool is_face(const FallProfile& p, u32 j) {
  const f64 run = p.s[j + 1] - p.s[j];
  return p.bed[j] - p.bed[j + 1] >= k_face_gradient * run;
}
// The first x past `xa` (relative to the launch) where y(x) = y0 - tan0 x - A x^2 comes down to
// the line l(x) = la + m (x - xa); `f(xa) > 0` (or 0 on the stretch it leaves over). In the stable
// form of the quadratic's root.
f64 first_root(f64 y0, f64 tan0, f64 A, f64 xa, f64 la, f64 m) {
  const f64 C = y0 - tan0 * xa - A * xa * xa - la;
  const f64 B = -tan0 - 2 * A * xa - m;
  const f64 D = std::sqrt(std::max(0.0, B * B + 4 * A * std::max(C, 0.0)));
  const f64 X = B >= 0 ? (B + D) / (2 * A) : 2 * std::max(C, 0.0) / (D - B);
  return xa + X;
}
f64 bed_at(const FallProfile& p, u32 j, f64 at) {
  const f64 run = p.s[j + 1] - p.s[j];
  const f64 t = run > 0 ? (at - p.s[j]) / run : 0.0;
  return p.bed[j] + t * (p.bed[j + 1] - p.bed[j]);
}
// Friction slope n^2 u^2 / d^(4/3) at d = q / u: n^2 u^(10/3) / q^(4/3).
f64 friction(f64 n2_over_q43, f64 u) { return n2_over_q43 * u * u * u * fall_cube_root(u); }
// Along a stretch of bed from (s, y) to (s', y'), the energy equation in E = u^2 / 2 against
// friction, d E / d l = g (sin a - S_f), stepped every `k_bed_step_m` of the stretch's length in
// the half-implicit form E' = (E + h g sin a) / (1 + h g S_f / E), which is positive, tends to the
// normal speed (S_f = sin a) and never overshoots it. Returns the speed at the end; adds the time.
f64 run_bed(f64 ds, f64 dy_down, f64 u, f64 n2_over_q43, f64& time) {
  const f64 length = std::sqrt(ds * ds + dy_down * dy_down);
  if (!(length > 0)) return u;
  const f64 sine = dy_down / length;
  const u32 steps = std::max(1u, static_cast<u32>(std::ceil(length / k_bed_step_m)));
  const f64 h = length / static_cast<f64>(steps);
  f64 e = .5 * u * u;
  const f64 e_min = .5 * k_min_speed * k_min_speed;
  for (u32 i = 0; i < steps; ++i) {
    const f64 before = std::sqrt(2 * e);
    const f64 sf = friction(n2_over_q43, before);
    e = std::max(e_min, (e + h * k_fall_gravity * sine) / (1 + h * k_fall_gravity * sf / e));
    const f64 after = std::sqrt(2 * e);
    time += h / (.5 * (before + after));
  }
  return std::sqrt(2 * e);
}
}  // namespace

f64 fall_cube_root(f64 a) noexcept {
  if (!(a > 0.0)) return 0.0;
  f64 y = std::max(a, 1.0);
  for (u32 i = 0; i < 64; ++i)
    y = (2.0 * y + a / (y * y)) / 3.0;
  return y;
}
f64 critical_depth(f64 q) noexcept { return fall_cube_root(q * q / k_fall_gravity); }

bool march_column(const FallProfile& profile, f64 q, f64 speed, f64 manning_n, u32 from, u32 to,
                  f64 tan_in, FallMarch& out) {
  out = {};
  const FallProfile& p = profile;
  const u32 m = p.s.size();
  if (m < 2 || p.bed.size() != m || p.water.size() != m || from >= m - 1 || to >= m || to < from ||
      !(q > 0) || !(speed > 0) || !(manning_n > 0))
    return false;
  for (u32 i = 0; i + 1 < m; ++i)
    if (!(p.s[i + 1] > p.s[i])) return false;
  const f64 g = k_fall_gravity;
  const f64 dc = critical_depth(q);
  const f64 n2_over_q43 = manning_n * manning_n / (q * fall_cube_root(q));
  f64 u = std::max(speed, k_min_speed), t = 0;
  u32 k = from;
  f64 at = p.s[from];  // where the column is: station k, or past it on stretch k
  bool on_station = true;
  const auto phase_of = [&](u32 j) { return is_face(p, j) ? FallPhase::Slide : FallPhase::Bed; };
  out.path.push_back({at, p.bed[from], 0.0, u, phase_of(from)});
  for (;;) {
    if (on_station) {
      if (k >= to || k + 1 >= m) break;
      // **Leave**: the parabola launched along the stretch above, at the column's speed — the
      // brink's where it arrives subcritical (Rouse 1936: q / (0.715 d_c)) — over the bed at the
      // next station.
      const f64 tan0 = k == from ? tan_in : (p.bed[k - 1] - p.bed[k]) / (p.s[k] - p.s[k - 1]);
      const f64 depth = q / u;
      const f64 v = u * u < g * depth ? q / (k_brink_depth_share * dc) : u;
      const f64 A = g * (1 + tan0 * tan0) / (2 * v * v);
      const f64 run = p.s[k + 1] - p.s[k];
      const f64 y0 = p.bed[k];
      if (y0 - tan0 * run - A * run * run > p.bed[k + 1] + k_leave_m) {
        // **Fly**: the first root against each stretch's surface in turn, then past the end.
        f64 x1 = 0, y1 = 0;
        FallLanding landing = FallLanding::None;
        u32 landed = k;
        for (u32 j = k; j + 1 < m && landing == FallLanding::None; ++j) {
          const f64 xa = p.s[j] - p.s[k], xb = p.s[j + 1] - p.s[k];
          const bool face = j == k || is_face(p, j);
          const f64 la = face ? p.bed[j] : p.bed[j] + p.water[j];
          const f64 lb = face ? p.bed[j + 1] : p.bed[j + 1] + p.water[j + 1];
          f64 best = first_root(y0, tan0, A, xa, la, (lb - la) / (xb - xa));
          FallLanding kind = face ? FallLanding::Rock : FallLanding::Water;
          if (face && j > k) {
            // A pool's level backs up against a face's foot: the next stretch's water, or the
            // body's past the end.
            f64 level = 0;
            bool has = false;
            if (j + 2 < m && !is_face(p, j + 1)) {
              level = p.bed[j + 1] + p.water[j + 1];
              has = true;
            } else if (j + 2 == m && p.tail_body) {
              level = p.tail_level;
              has = true;
            }
            if (has && level > std::min(la, lb)) {
              const f64 c = first_root(y0, tan0, A, xa, level, 0.0);
              if (c < best) {
                best = c;
                kind = j + 2 == m && p.tail_body ? FallLanding::Body : FallLanding::Water;
              }
            }
          }
          if (best <= xb) {
            x1 = std::max(best, xa);
            y1 = y0 - tan0 * x1 - A * x1 * x1;
            landing = kind;
            landed = j;
          }
        }
        if (landing == FallLanding::None) {
          // Past the line's last station: a body's level at a mouth, else the last surface level.
          const f64 xa = p.s[m - 1] - p.s[k];
          const f64 level = p.tail_body ? p.tail_level : p.bed[m - 1] + p.water[m - 1];
          x1 = std::max(first_root(y0, tan0, A, xa, level, 0.0), xa);
          y1 = y0 - tan0 * x1 - A * x1 * x1;
          landing = p.tail_body ? FallLanding::Body : FallLanding::End;
          landed = m - 1;
        }
        // The flight's path, sampled by time: the horizontal speed is constant, the vertical the
        // free fall's.
        const f64 h = v / std::sqrt(1 + tan0 * tan0);
        const f64 tau = x1 / h;
        const f64 lip_thickness = q / v;
        f64 tau_at = 0, length = 0;
        for (;;) {
          const f64 vy = h * tan0 + g * tau_at;
          const f64 here = std::sqrt(h * h + vy * vy);
          const f64 step = std::min(
              k_air_step_max_m,
              std::max(k_air_step_min_m, .25 * lip_thickness + k_air_step_growth * length));
          const f64 next = tau_at + step / here;
          if (next >= tau) break;
          length += step;
          tau_at = next;
          const f64 x = h * tau_at;
          out.path.push_back({p.s[k] + x, y0 - tan0 * x - A * x * x, t + tau_at,
                              std::sqrt(h * h + sq(h * tan0 + g * tau_at)), FallPhase::Air});
        }
        const f64 vy1 = h * tan0 + g * tau;
        const f64 speed1 = std::sqrt(h * h + vy1 * vy1);
        t += tau;
        out.path.push_back({p.s[k] + x1, y1, t, speed1, FallPhase::Air});
        FallFlight flight;
        flight.s0 = p.s[k];
        flight.y0 = y0;
        flight.s1 = p.s[k] + x1;
        flight.y1 = y1;
        flight.speed0 = v;
        flight.tan0 = tan0;
        flight.time_s = tau;
        flight.speed1 = speed1;
        flight.landing = landing;
        out.flights.push_back(flight);
        if (landing == FallLanding::Body || landing == FallLanding::End) break;
        // **Land**: the part of the velocity along the surface it struck is kept: the rock's
        // slope on a face, the water's on a stretch it stands over, level on a pool backing up
        // against a face's foot.
        const f64 run_l = p.s[landed + 1] - p.s[landed];
        f64 rise = 0;
        if (landing == FallLanding::Rock)
          rise = (p.bed[landed + 1] - p.bed[landed]) / run_l;
        else if (!is_face(p, landed))
          rise = ((p.bed[landed + 1] + p.water[landed + 1]) - (p.bed[landed] + p.water[landed])) /
                 run_l;
        u = std::max(k_min_speed, (h - rise * vy1) / std::sqrt(1 + rise * rise));
        k = landed;
        at = std::clamp(flight.s1, p.s[k], p.s[k + 1]);
        on_station = false;
        // A plunge goes down to the bed under the water it entered.
        if (landing == FallLanding::Water)
          out.path.push_back({at, bed_at(p, k, at), t, u, phase_of(k)});
        continue;
      }
    }
    // **Run** (or slide) along the bed to the next station.
    if (k + 1 >= m) break;
    const f64 y_here = bed_at(p, k, at);
    u = run_bed(p.s[k + 1] - at, y_here - p.bed[k + 1], u, n2_over_q43, t);
    ++k;
    at = p.s[k];
    on_station = true;
    out.path.push_back({at, p.bed[k], t, u, k + 1 < m ? phase_of(k) : FallPhase::Bed});
  }
  return true;
}

// ---- the cook ---------------------------------------------------------------------------------

bool fall_options_in_range(const FallOptions& o, std::string& error) {
  if (!(o.stage > 0.0f) || !(o.stage <= 1.0f)) return fail(error, "falls stage (0, 1]");
  if (!(o.manning_n >= .005f) || !(o.manning_n <= .3f))
    return fail(error, "falls manning_n (0.005 to 0.3)");
  if (o.columns < 3 || o.columns > 33) return fail(error, "falls columns (3 to 33)");
  if (!(o.min_area_m2 >= 0) || !std::isfinite(o.min_area_m2))
    return fail(error, "falls min_area_m2");
  return true;
}
u64 fall_options_hash(const FallOptions& o) {
  u64 h = hash_combine(0x46414C4C53ull, k_falls_version);
  h = hash_combine(h, std::bit_cast<u32>(o.stage));
  h = hash_combine(h, std::bit_cast<u32>(o.manning_n));
  h = hash_combine(h, o.columns);
  return hash_combine(h, std::bit_cast<u64>(o.min_area_m2));
}

namespace {
// What one link's cook gives, merged in link order.
struct LinkFalls {
  Vector<Fall> falls;
  Vector<FallColumn> columns;
  Vector<FallVertex> vertices;
  FallStats stats;
};
i32 to_mm(f64 metres) { return static_cast<i32>(std::llround(metres * 1e3)); }
u32 to_unsigned(f64 v) {
  return v <= 0 ? 0u : (v >= 4294967295.0 ? 4294967295u : static_cast<u32>(std::llround(v)));
}
u8 turbulence_class(ChannelReach r) {
  switch (r) {
    case ChannelReach::StepPool:
    case ChannelReach::Cascade: return 2;
    case ChannelReach::Bedrock:
    case ChannelReach::Colluvial: return 1;
    default: return 0;
  }
}
// How far the cut's section at point i rises over its deepest at `a` across the line (positive to
// the left): the flat bed, then each bank at its slope, up to the top (`bend_section`'s shape, as
// `terrain_cuts_json` writes it).
struct Across {
  BendSection section;
  f64 depth = 0;  // the cut's depth at the line, top to bed
};
f64 rise_at(const Across& c, f64 a) {
  const BendSection& b = c.section;
  const f64 left = a - b.centre - .5 * b.bed, right = b.centre - .5 * b.bed - a;
  f64 r = 0;
  if (left > 0) r = left * b.slope_left;
  if (right > 0) r = right * b.slope_right;
  return std::min(r, std::max(c.depth, 0.0));
}

void cook_link(const ChannelLayer& p, const ChannelNetwork& n, u32 index, const FallOptions& o,
               LinkFalls& out) {
  const ChannelLink& link = n.links[index];
  const u32 m = std::min<u32>(link.keep, link.line.size());
  if (m < 2 || link.bed.size() != link.line.size()) return;
  ++out.stats.links;
  Vector<f64> s(m, 0.0);
  for (u32 i = 1; i < m; ++i) {
    const f64 dx = link.line[i].at.x - link.line[i - 1].at.x;
    const f64 dz = link.line[i].at.z - link.line[i - 1].at.z;
    s[i] = s[i - 1] + std::sqrt(dx * dx + dz * dz);
  }
  for (u32 i = 0; i + 1 < m; ++i)
    if (!(s[i + 1] > s[i])) return;  // a line with two points at one place has no stretch there
  // The water at each point as the description lays it in the cut: `stage` of the thalweg over the
  // bed, and on a link that ends in a body never under the body's level.
  Vector<f64> water(m, 0.0);
  Vector<Across> across(m);
  for (u32 i = 0; i < m; ++i) {
    const ChannelPoint& pt = link.line[i];
    const f64 thalweg =
        round_to(section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg, 1e3);
    water[i] = wide(o.stage) * thalweg;
    if (link.mouth) water[i] = std::max(water[i], link.mouth_level - link.bed[i]);
    across[i].section = bend_section(p, wide(pt.width_m), wide(pt.depth_m),
                                     i < link.asym.size() ? wide(link.asym[i]) : 0.0,
                                     i < link.swell.size() ? wide(link.swell[i]) : 0.0);
    const f64 top = pt.at.y - (i < link.seat.size() ? wide(link.seat[i]) : 0.0);
    across[i].depth = top - link.bed[i];
  }
  const bool drawn = wide(link.line[m - 1].area_m2) >= o.min_area_m2;
  const f64 n_manning = wide(o.manning_n);
  for (u32 a = 0; a + 1 < m;) {
    if (link.bed[a] - link.bed[a + 1] < k_face_gradient * (s[a + 1] - s[a])) {
      ++a;
      continue;
    }
    // A run of the solver's steps: stretches a .. b - 1, points a (the lip) to b.
    u32 b = a + 1;
    while (b + 1 < m && link.bed[b] - link.bed[b + 1] >= k_face_gradient * (s[b + 1] - s[b]))
      ++b;
    ++out.stats.runs;
    const ChannelPoint& lip = link.line[a];
    const f64 w = std::max(wide(lip.width_m), .1);
    // The window: two widths above the lip (at least a metre, and at least a point) to two below
    // the run's last point.
    const f64 reach_above = std::max(k_window_widths * w, 1.0);
    u32 i0 = static_cast<u32>(std::lower_bound(s.begin(), s.begin() + a, s[a] - reach_above) -
                              s.begin());
    if (a > 0) i0 = std::min(i0, a - 1);
    u32 to = b;
    while (to + 1 < m && s[to] < s[b] + reach_above)
      ++to;
    // The approach: the bed's fall per metre over the knickpoint's window above the lip.
    const f64 window = std::max(k_approach_min_m, k_approach_widths * w);
    const u32 up =
        static_cast<u32>(std::lower_bound(s.begin(), s.begin() + a, s[a] - window) - s.begin());
    const f64 approach = a > up ? (link.bed[up] - link.bed[a]) / (s[a] - s[up])
                                : static_cast<f64>(std::max(lip.slope, 0.0f));
    const f64 slope = std::clamp(approach, 1e-4, 1.0);
    // The water at the lip: its depth, Manning's speed, the wet width across the section and the
    // columns over it, each with its head over the lip and a share of the discharge as that head
    // to the 3/2 power (waterfalls.md, "A lip is not level").
    const f64 d = water[a];
    const BendSection& sec = across[a].section;
    const f64 left_edge = sec.centre + .5 * sec.bed + d / std::max(sec.slope_left, .02);
    const f64 right_edge = sec.centre - .5 * sec.bed - d / std::max(sec.slope_right, .02);
    const f64 wet = left_edge - right_edge;
    const f64 speed = fall_cube_root(d * d) * std::sqrt(slope) / n_manning;
    const u32 count = o.columns;
    const f64 da = wet / static_cast<f64>(count);
    Vector<f64> offset(count), head(count);
    f64 area = 0, weight = 0;
    for (u32 j = 0; j < count; ++j) {
      offset[j] = right_edge + (static_cast<f64>(j) + .5) * da;
      head[j] = std::max(0.0, d - rise_at(across[a], offset[j]));
      area += head[j] * da;
      weight += head[j] * std::sqrt(head[j]) * da;
    }
    const f64 discharge = speed * area;
    const f64 q_mean = wet > 0 ? discharge / wet : 0.0;
    const f64 dc_mean = critical_depth(q_mean);
    Fall fall;
    fall.x_mm = static_cast<i64>(std::llround(lip.at.x * 1e3));
    fall.y_mm = static_cast<i64>(std::llround(link.bed[a] * 1e3));
    fall.z_mm = static_cast<i64>(std::llround(lip.at.z * 1e3));
    fall.link = index;
    fall.lip = a;
    fall.end = b;
    // The heading at the lip: the chord from the point above to the point below.
    const u32 ha = a > 0 ? a - 1 : a, hb = a + 1;
    const f64 hx = link.line[hb].at.x - link.line[ha].at.x,
              hz = link.line[hb].at.z - link.line[ha].at.z;
    const f64 hl = std::sqrt(hx * hx + hz * hz);
    const f64 tx = hl > 0 ? hx / hl : 1.0, tz = hl > 0 ? hz / hl : 0.0;
    fall.heading_x = static_cast<i32>(std::llround(tx * 1e4));
    fall.heading_z = static_cast<i32>(std::llround(tz * 1e4));
    fall.discharge_ml_s = to_unsigned(discharge * 1e6);
    fall.critical_um = to_unsigned(dc_mean * 1e6);
    const u32 above = a > 0 ? a - 1 : a;
    const u8 reach = above < link.reach.size() ? link.reach[above] : static_cast<u8>(0);
    fall.reach = reach;
    fall.turbulence = turbulence_class(static_cast<ChannelReach>(reach));
    fall.breakup_mm = to_unsigned(k_breakup_depths[fall.turbulence] * dc_mean * 1e3);
    const auto knick_at = [&](u32 i) { return i < link.knick.size() && link.knick[i] != 0; };
    fall.knick = static_cast<u8>(knick_at(a) || knick_at(a + 1) ? 1 : 0);
    fall.drawn = static_cast<u8>(drawn ? 1 : 0);
    fall.first_column = out.columns.size();
    // Each column down its own bed: the section's rise at its place, at every point of the window.
    FallKind run_kind = FallKind::Held;
    FallKind deciding_kind = FallKind::Held;
    f64 deciding_drop = -1;
    for (u32 j = 0; j < count; ++j) {
      FallColumn column;
      column.offset_mm = to_mm(offset[j]);
      column.first_vertex = out.vertices.size();
      const f64 q = weight > 0 ? discharge * head[j] * std::sqrt(head[j]) / weight : 0.0;
      column.q_mm2_s = to_unsigned(q * 1e6);
      const f64 dc = critical_depth(q);
      column.critical_um = to_unsigned(dc * 1e6);
      FallKind kind = FallKind::Held;
      if (q > 0 && head[j] > 0) {
        // The column's bed from the window's start on, far enough for any flight a channel makes
        // to come down (200 m of line at the default step past the window).
        FallProfile profile;
        const u32 last = std::min(m, to + 1 + k_profile_beyond_points);
        for (u32 i = i0; i < last; ++i) {
          const f64 rise = rise_at(across[i], offset[j]);
          profile.s.push_back(s[i]);
          profile.bed.push_back(link.bed[i] + rise);
          profile.water.push_back(std::max(0.0, water[i] - rise));
        }
        profile.tail_body = link.mouth && last == m;
        profile.tail_level = link.mouth_level;
        const f64 tan_in = a > i0 ? (link.bed[i0] - link.bed[i0 + 1]) / (s[i0 + 1] - s[i0]) : slope;
        FallMarch march;
        if (march_column(profile, q, q / head[j], n_manning, 0, to - i0, tan_in, march)) {
          u32 falls = 0;
          const FallFlight* best = nullptr;
          for (const FallFlight& f : march.flights) {
            if (best == nullptr || f.drop() > best->drop()) best = &f;
            falls += f.drop() >= k_fall_min_drop_depths * dc && dc >= k_fall_min_critical_m ? 1 : 0;
          }
          column.flights = static_cast<u8>(std::min<usize>(march.flights.size(), 255));
          if (best != nullptr) {
            column.drop_mm = to_mm(best->drop());
            column.landing = static_cast<u8>(best->landing);
            if (dc < k_fall_min_critical_m)
              kind = FallKind::Trickle;
            else if (falls == 0)
              kind = FallKind::Drowned;
            else if (falls >= 2)
              kind = FallKind::Tiered;
            else
              kind = best->landing == FallLanding::Rock ? FallKind::Horsetail : FallKind::Plunge;
            if (falls > 0 && best->drop() > deciding_drop) {
              deciding_drop = best->drop();
              deciding_kind = kind;
              fall.landing = static_cast<u8>(best->landing);
              fall.tail_level_mm = to_mm(best->y1 - link.bed[a]);
              // The water's depth where it came down (none on rock, unknown under a body).
              f64 depth_there = 0;
              if (best->landing == FallLanding::Water) {
                u32 j2 = 0;
                while (j2 + 2 < profile.s.size() && profile.s[j2 + 1] < best->s1)
                  ++j2;
                depth_there = std::max(0.0, best->y1 - bed_at(profile, j2, best->s1));
              }
              fall.tail_depth_mm = to_unsigned(depth_there * 1e3);
            }
            if (j == count / 2) fall.launch_mm_s = to_unsigned(march.flights[0].speed0 * 1e3);
          }
          // The path as vertices from the fall's origin: on the bed the line's place at the
          // station, offset across it; aloft the straight jet from where it left.
          const auto place = [&](f64 at, f64& x, f64& z, f64& nx, f64& nz) {
            u32 i = i0;  // every station the march gives is at or past the window's start
            while (i + 2 < m && s[i + 1] < at)
              ++i;
            const f64 run = s[i + 1] - s[i];
            const f64 u = std::clamp((at - s[i]) / run, 0.0, 1.0);
            const f64 dx = link.line[i + 1].at.x - link.line[i].at.x;
            const f64 dz = link.line[i + 1].at.z - link.line[i].at.z;
            x = link.line[i].at.x + u * dx;
            z = link.line[i].at.z + u * dz;
            nx = -dz / run;
            nz = dx / run;
          };
          f64 jet_x = 0, jet_z = 0, jet_tx = 1, jet_tz = 0, jet_nx = 0, jet_nz = 1, jet_s = 0;
          for (u32 v = 0; v < march.path.size(); ++v) {
            const FallSample& sample = march.path[v];
            f64 x = 0, z = 0, nx = 0, nz = 0;
            if (sample.phase == FallPhase::Air) {
              x = jet_x + jet_tx * (sample.s - jet_s) + jet_nx * offset[j];
              z = jet_z + jet_tz * (sample.s - jet_s) + jet_nz * offset[j];
            } else {
              place(sample.s, x, z, nx, nz);
              // Where a flight may leave next: the line's tangent here.
              jet_x = x;
              jet_z = z;
              jet_s = sample.s;
              jet_tx = nz;
              jet_tz = -nx;
              jet_nx = nx;
              jet_nz = nz;
              x += nx * offset[j];
              z += nz * offset[j];
            }
            FallVertex vertex;
            vertex.x_mm = static_cast<i32>(static_cast<i64>(std::llround(x * 1e3)) - fall.x_mm);
            vertex.y_mm =
                static_cast<i32>(static_cast<i64>(std::llround(sample.y * 1e3)) - fall.y_mm);
            vertex.z_mm = static_cast<i32>(static_cast<i64>(std::llround(z * 1e3)) - fall.z_mm);
            vertex.time_us = to_unsigned(sample.time_s * 1e6);
            vertex.speed_mm_s = to_unsigned(sample.speed * 1e3);
            vertex.thickness_um = to_unsigned(q / sample.speed * 1e6);
            vertex.phase = static_cast<u8>(sample.phase);
            out.vertices.push_back(vertex);
          }
        }
      }
      column.kind = static_cast<u8>(kind);
      column.vertex_count = out.vertices.size() - column.first_vertex;
      run_kind = std::max(run_kind, kind);
      out.columns.push_back(column);
    }
    // The run's kind is its deciding column's (the highest fall), else the most a column came to
    // (held, drowned, a trickle).
    if (deciding_drop >= 0) {
      run_kind = deciding_kind;
      fall.drop_mm = to_mm(deciding_drop);
    } else {
      i32 most = 0;
      for (u32 j = fall.first_column; j < out.columns.size(); ++j)
        most = std::max(most, out.columns[j].drop_mm);
      fall.drop_mm = most;
    }
    fall.kind = static_cast<u8>(run_kind);
    fall.column_count = out.columns.size() - fall.first_column;
    FallStats& st = out.stats;
    switch (run_kind) {
      case FallKind::Held: ++st.held; break;
      case FallKind::Drowned: ++st.drowned; break;
      case FallKind::Trickle: ++st.trickles; break;
      case FallKind::Plunge: ++st.plunges; break;
      case FallKind::Horsetail: ++st.horsetails; break;
      case FallKind::Tiered: ++st.tiered; break;
    }
    if (run_kind >= FallKind::Plunge) {
      ++st.falls;
      st.drawn_falls += drawn ? 1u : 0u;
      // At a mouth: the run reaches the shore (the link's last point), whatever it lands on.
      st.mouth_falls += link.mouth && b + 1 >= m ? 1u : 0u;
      st.knick_falls += fall.knick;
      st.highest_m = std::max(st.highest_m, deciding_drop);
    }
    out.falls.push_back(fall);
    a = b;
  }
  out.stats.columns = out.columns.size();
  out.stats.vertices = out.vertices.size();
}
}  // namespace

bool cook_falls(const ChannelLayer& layer, const ChannelNetwork& network, u64 plan_key,
                const FallOptions& options, jobs::JobSystem* jobs, FallSet& out,
                std::string& error) {
  out = {};
  if (!fall_options_in_range(options, error)) return false;
  out.plan_key = plan_key;
  out.options = fall_options_hash(options);
  const u32 count = network.links.size();
  Vector<LinkFalls> each(count);
  const auto cook = [&](u32 begin, u32 end) {
    for (u32 k = begin; k < end; ++k)
      cook_link(layer, network, k, options, each[k]);
  };
  if (jobs != nullptr && count > 1)
    jobs->parallel_for(jobs::Pool::Performance, count, 4,
                       [&](u32 begin, u32 end) { cook(begin, end); });
  else
    cook(0, count);
  // Merged by link index, never by completion order.
  FallStats& st = out.stats;
  for (u32 k = 0; k < count; ++k) {
    LinkFalls& l = each[k];
    const u32 column_base = out.columns.size(), vertex_base = out.vertices.size();
    for (Fall f : l.falls) {
      f.first_column += column_base;
      out.falls.push_back(f);
    }
    for (FallColumn c : l.columns) {
      c.first_vertex += vertex_base;
      out.columns.push_back(c);
    }
    for (const FallVertex& v : l.vertices)
      out.vertices.push_back(v);
    st.links += l.stats.links;
    st.runs += l.stats.runs;
    st.held += l.stats.held;
    st.drowned += l.stats.drowned;
    st.trickles += l.stats.trickles;
    st.falls += l.stats.falls;
    st.plunges += l.stats.plunges;
    st.horsetails += l.stats.horsetails;
    st.tiered += l.stats.tiered;
    st.drawn_falls += l.stats.drawn_falls;
    st.mouth_falls += l.stats.mouth_falls;
    st.knick_falls += l.stats.knick_falls;
    st.highest_m = std::max(st.highest_m, l.stats.highest_m);
  }
  st.columns = out.columns.size();
  st.vertices = out.vertices.size();
  return true;
}

// ---- the file ----------------------------------------------------------------------------------

namespace {
struct Writer {
  std::string bytes;
  template <class T>
  void put(const T& v) {
    bytes.append(reinterpret_cast<const char*>(&v), sizeof(T));
  }
  template <class T>
  void list(const Vector<T>& v) {
    put(static_cast<u32>(v.size()));
    if (!v.empty()) bytes.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
  }
};
struct Reader {
  const char* at;
  const char* end;
  bool ok = true;
  template <class T>
  T get() {
    T v{};
    if (!ok || static_cast<usize>(end - at) < sizeof(T)) {
      ok = false;
      return v;
    }
    std::memcpy(&v, at, sizeof(T));
    at += sizeof(T);
    return v;
  }
  template <class T>
  void list(Vector<T>& v) {
    const u32 count = get<u32>();
    if (!ok || static_cast<usize>(end - at) / sizeof(T) < count) {
      ok = false;
      return;
    }
    v.resize(count);
    if (count > 0) std::memcpy(v.data(), at, static_cast<usize>(count) * sizeof(T));
    at += static_cast<usize>(count) * sizeof(T);
  }
};
template <class F>
void stats_fields(FallStats& s, F&& f) {
  f(s.links);
  f(s.runs);
  f(s.held);
  f(s.drowned);
  f(s.trickles);
  f(s.falls);
  f(s.plunges);
  f(s.horsetails);
  f(s.tiered);
  f(s.drawn_falls);
  f(s.mouth_falls);
  f(s.knick_falls);
  f(s.columns);
  f(s.vertices);
}
}  // namespace

std::string falls_bytes(const FallSet& set) {
  Writer w;
  w.put(k_falls_magic);
  w.put(k_falls_version);
  w.put(set.plan_key);
  w.put(set.options);
  FallStats stats = set.stats;
  stats_fields(stats, [&](u64 v) { w.put(v); });
  w.put(stats.highest_m);
  w.list(set.falls);
  w.list(set.columns);
  w.list(set.vertices);
  w.put(hash_bytes(w.bytes.data(), w.bytes.size()));
  return std::move(w.bytes);
}

bool read_falls(std::string_view bytes, FallSet& out, std::string& error) {
  out = {};
  const auto refuse = [&](const char* why) {
    out = {};
    error = std::string("channels: the falls file ") + why;
    return false;
  };
  if (bytes.size() < 2 * sizeof(u32) + 3 * sizeof(u64)) return refuse("is too short to be one");
  Reader r{bytes.data(), bytes.data() + bytes.size() - sizeof(u64)};
  if (r.get<u32>() != k_falls_magic) return refuse("is not a channel falls file");
  if (r.get<u32>() != k_falls_version) return refuse("was written by another version of the cook");
  u64 closing = 0;
  std::memcpy(&closing, r.end, sizeof(u64));
  if (closing != hash_bytes(bytes.data(), bytes.size() - sizeof(u64)))
    return refuse("does not hash to what it closes with");
  out.plan_key = r.get<u64>();
  out.options = r.get<u64>();
  stats_fields(out.stats, [&](u64& v) { v = r.get<u64>(); });
  out.stats.highest_m = r.get<f64>();
  r.list(out.falls);
  r.list(out.columns);
  r.list(out.vertices);
  if (!r.ok || r.at != r.end) return refuse("is not the length its contents say");
  for (const Fall& f : out.falls)
    if (f.first_column > out.columns.size() || f.column_count > out.columns.size() - f.first_column)
      return refuse("holds a fall whose columns run past the columns");
  for (const FallColumn& c : out.columns)
    if (c.first_vertex > out.vertices.size() ||
        c.vertex_count > out.vertices.size() - c.first_vertex)
      return refuse("holds a column whose path runs past the vertices");
  return true;
}

std::string falls_path(std::string_view dir, u64 plan_key, u64 options) {
  return io::join_path(
      dir, "falls-" + plan_key_text(plan_key) + "-" + plan_key_text(options) + ".channel-falls");
}

bool ensure_falls(const ChannelLayer& layer, const ChannelPlan& plan, const FallOptions& options,
                  jobs::JobSystem* jobs, std::string_view dir, FallSet& out, bool& cooked,
                  std::string& error) {
  cooked = false;
  if (!fall_options_in_range(options, error)) return false;
  const u64 hash = fall_options_hash(options);
  const std::string path = falls_path(dir, plan.key, hash);
  std::string bytes;
  std::string ignored;
  if (io::read_file(path, bytes) == io::Status::Ok && read_falls(bytes, out, ignored) &&
      out.plan_key == plan.key && out.options == hash)
    return true;
  if (!cook_falls(layer, plan.network, plan.key, options, jobs, out, error)) return false;
  cooked = true;
  // Best effort, as the plan's own file is.
  (void)io::write_file_atomic(path, falls_bytes(out));
  return true;
}

JsonValue falls_json(const ChannelLayer& layer, const FallSet& set, bool paths) {
  static const char* kinds[] = {"held", "drowned", "trickle", "plunge", "horsetail", "tiered"};
  static const char* landings[] = {"none", "water", "rock", "body", "end"};
  const auto name_of = [](const char* const* names, u32 count, u8 v) {
    return v < count ? names[v] : "?";
  };
  const auto metres = [](f64 v) { return JsonValue(std::round(v) / 1e3); };
  JsonValue list = JsonValue::array();
  for (const Fall& f : set.falls) {
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(layer.name + " link " + std::to_string(f.link));
    o["link"] = JsonValue(f.link);
    o["lip"] = JsonValue(f.lip);
    o["end"] = JsonValue(f.end);
    JsonValue at = JsonValue::array();
    at.push_back(metres(static_cast<f64>(f.x_mm)));
    at.push_back(metres(static_cast<f64>(f.y_mm)));
    at.push_back(metres(static_cast<f64>(f.z_mm)));
    o["at"] = std::move(at);
    o["kind"] = JsonValue(name_of(kinds, 6, f.kind));
    o["landing"] = JsonValue(name_of(landings, 5, f.landing));
    o["drop_m"] = metres(static_cast<f64>(f.drop_mm));
    o["discharge_m3s"] = JsonValue(static_cast<f64>(f.discharge_ml_s) / 1e6);
    o["critical_m"] = JsonValue(static_cast<f64>(f.critical_um) / 1e6);
    o["launch_m_s"] = metres(static_cast<f64>(f.launch_mm_s));
    o["breakup_m"] = metres(static_cast<f64>(f.breakup_mm));
    o["tail_level_m"] = metres(static_cast<f64>(f.tail_level_mm));
    o["tail_depth_m"] = metres(static_cast<f64>(f.tail_depth_mm));
    o["reach"] = JsonValue(static_cast<u32>(f.reach));
    o["knick"] = JsonValue(f.knick != 0);
    o["drawn"] = JsonValue(f.drawn != 0);
    JsonValue columns = JsonValue::array();
    for (u32 j = f.first_column; j < f.first_column + f.column_count; ++j) {
      const FallColumn& c = set.columns[j];
      JsonValue col = JsonValue::object();
      col["offset_m"] = metres(static_cast<f64>(c.offset_mm));
      col["q_m2s"] = JsonValue(static_cast<f64>(c.q_mm2_s) / 1e6);
      col["critical_m"] = JsonValue(static_cast<f64>(c.critical_um) / 1e6);
      col["drop_m"] = metres(static_cast<f64>(c.drop_mm));
      col["kind"] = JsonValue(name_of(kinds, 6, c.kind));
      col["landing"] = JsonValue(name_of(landings, 5, c.landing));
      col["flights"] = JsonValue(static_cast<u32>(c.flights));
      if (paths) {
        JsonValue rows = JsonValue::array();
        for (u32 v = c.first_vertex; v < c.first_vertex + c.vertex_count; ++v) {
          const FallVertex& x = set.vertices[v];
          JsonValue row = JsonValue::array();
          row.push_back(metres(static_cast<f64>(x.x_mm)));
          row.push_back(metres(static_cast<f64>(x.y_mm)));
          row.push_back(metres(static_cast<f64>(x.z_mm)));
          row.push_back(JsonValue(static_cast<f64>(x.time_us) / 1e6));
          row.push_back(metres(static_cast<f64>(x.speed_mm_s)));
          row.push_back(JsonValue(static_cast<f64>(x.thickness_um) / 1e6));
          row.push_back(JsonValue(static_cast<u32>(x.phase)));
          rows.push_back(std::move(row));
        }
        col["path"] = std::move(rows);
      }
      columns.push_back(std::move(col));
    }
    o["columns"] = std::move(columns);
    list.push_back(std::move(o));
  }
  return list;
}

}  // namespace engine::channels
