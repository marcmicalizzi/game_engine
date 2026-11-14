// The flow fixture (flow_fixture.h; channels.md, "The fixture"; roadmap W120 stage 0).
#include "detail.h"

#include <domain/channels/flow_fixture.h>
#include <domain/channels/hydraulics.h>
#include <domain/channels/stations.h>
#include <domain/scene_gen/water_channel.h>

#include <cmath>

namespace engine::channels {

using detail::powd;
using detail::round_to;

// ---- The closed forms ---------------------------------------------------------------------------

f64 Trapezoid::perimeter(f64 y) const noexcept {
  return bed_m + 2.0 * y * std::sqrt(1.0 + side * side);
}

f64 Trapezoid::depth_of_area(f64 a) const noexcept {
  if (!(a > 0.0)) return 0.0;
  if (!(side > 0.0)) return a / bed_m;
  // side y^2 + bed y - a = 0, in the form that loses nothing when 4 side a is small beside bed^2.
  return 2.0 * a / (bed_m + std::sqrt(bed_m * bed_m + 4.0 * side * a));
}

namespace {

// R^(2/3) and R^(4/3) by the falls' cube root: no C library power.
f64 two_thirds(f64 r) noexcept { return fall_cube_root(r * r); }
f64 four_thirds(f64 r) noexcept { return r * fall_cube_root(r); }

// The root of a function rising through zero between lo and hi, bisected until the bracket is one
// float wide (at most 200 halvings): the same bits wherever it runs.
template <typename F>
f64 bisect(f64 lo, f64 hi, F&& f) noexcept {
  for (u32 i = 0; i < 200; ++i) {
    const f64 mid = 0.5 * (lo + hi);
    if (!(mid > lo) || !(mid < hi)) break;
    if (f(mid) < 0.0)
      lo = mid;
    else
      hi = mid;
  }
  return 0.5 * (lo + hi);
}

}  // namespace

f64 trapezoid_discharge(const Trapezoid& t, f64 y, f64 n, f64 slope) noexcept {
  const f64 a = t.area(y);
  return a * two_thirds(a / t.perimeter(y)) * std::sqrt(slope) / n;
}

f64 trapezoid_friction(const Trapezoid& t, f64 y, f64 q, f64 n) noexcept {
  const f64 a = t.area(y);
  return n * n * q * q / (a * a * four_thirds(a / t.perimeter(y)));
}

f64 trapezoid_energy(const Trapezoid& t, f64 y, f64 q) noexcept {
  const f64 a = t.area(y);
  return y + q * q / (2.0 * k_gravity * a * a);
}

f64 trapezoid_force(const Trapezoid& t, f64 y, f64 q) noexcept {
  return q * q / (k_gravity * t.area(y)) + t.moment(y);
}

f64 trapezoid_froude(const Trapezoid& t, f64 y, f64 q) noexcept {
  const f64 a = t.area(y);
  return q / a / std::sqrt(k_gravity * a / t.top(y));
}

f64 trapezoid_critical(const Trapezoid& t, f64 q) noexcept {
  return bisect(1e-9, 1e3, [&](f64 y) {
    const f64 a = t.area(y);
    return k_gravity * a * a * a - q * q * t.top(y);
  });
}

f64 trapezoid_normal(const Trapezoid& t, f64 q, f64 n, f64 slope) noexcept {
  return bisect(1e-9, 1e3, [&](f64 y) { return trapezoid_discharge(t, y, n, slope) - q; });
}

f64 trapezoid_sequent(const Trapezoid& t, f64 y, f64 q) noexcept {
  const f64 critical = trapezoid_critical(t, q);
  const f64 force = trapezoid_force(t, y, q);
  // The specific force falls to its least at critical depth and rises either side of it.
  if (y < critical)
    return bisect(critical, 1e3, [&](f64 d) { return trapezoid_force(t, d, q) - force; });
  return bisect(1e-9, critical, [&](f64 d) { return force - trapezoid_force(t, d, q); });
}

void direct_step(const Trapezoid& t, f64 q, f64 n, f64 slope, f64 y0, f64 y1, u32 steps,
                 DepthTable& out) {
  out.y.clear();
  out.x.clear();
  out.y.push_back(y0);
  out.x.push_back(0.0);
  f64 x = 0.0;
  f64 energy = trapezoid_energy(t, y0, q);
  f64 friction = trapezoid_friction(t, y0, q, n);
  for (u32 i = 1; i <= steps; ++i) {
    const f64 y = y0 + (y1 - y0) * (static_cast<f64>(i) / static_cast<f64>(steps));
    const f64 e = trapezoid_energy(t, y, q);
    const f64 f = trapezoid_friction(t, y, q, n);
    x += (e - energy) / (slope - 0.5 * (friction + f));
    energy = e;
    friction = f;
    out.y.push_back(y);
    out.x.push_back(x);
  }
}

f64 depth_at(const DepthTable& table, f64 x) noexcept {
  const u32 n = table.x.size();
  if (n == 0) return 0.0;
  if (n == 1) return table.y[0];
  const bool rising = table.x[n - 1] > table.x[0];
  const auto before = [&](u32 i) { return rising ? table.x[i] <= x : table.x[i] >= x; };
  if (!before(0)) return table.y[0];
  if (before(n - 1)) return table.y[n - 1];
  u32 lo = 0, hi = n - 1;  // before(lo), !before(hi)
  while (hi - lo > 1) {
    const u32 mid = lo + (hi - lo) / 2;
    if (before(mid))
      lo = mid;
    else
      hi = mid;
  }
  const f64 span = table.x[hi] - table.x[lo];
  const f64 u = (span > 0.0 || span < 0.0) ? (x - table.x[lo]) / span : 0.0;
  return table.y[lo] + u * (table.y[hi] - table.y[lo]);
}

// ---- The fixture --------------------------------------------------------------------------------

namespace {

// The ground: 96 m by 60 m at a quarter of a metre, three trenches along +x under a flat cap.
constexpr f64 k_spacing_m = 0.25;
constexpr f64 k_x_end_m = 96.0;
constexpr f64 k_z_half_m = 30.0;
constexpr f64 k_cap_m = 13.0;
constexpr f64 k_band_m = 10.0;  // each trench's half band
constexpr f64 k_line_z[3] = {-20.0, 0.0, 20.0};

// The section and the flow every channel shares.
constexpr Trapezoid k_section{2.0, 1.0};
constexpr f64 k_slope = 0.01;
constexpr f64 k_manning = 0.04;
constexpr f64 k_normal_m = 0.5;  // chosen; the discharge follows from it

// Where each reach begins and ends, and what the step and the two bodies are.
constexpr f64 k_head_x = 8.0;
constexpr f64 k_uniform_end_x = 48.0;
constexpr f64 k_uniform_bed0 = 10.0;
constexpr f64 k_step_bed0 = 12.0;
constexpr f64 k_crest_x = 38.0;
constexpr f64 k_drop_m = 2.0;
constexpr f64 k_step_mouth_x = 58.0;
constexpr f64 k_pool_depth_at_mouth = 0.6;
constexpr f64 k_pool_wall_x = 78.0;
constexpr f64 k_mouth_bed0 = 10.0;
constexpr f64 k_lake_mouth_x = 88.0;
constexpr f64 k_lake_depth_at_mouth = 1.2;
constexpr f64 k_basin_drop_m = 1.0;  // a basin's floor under the bed at its mouth
constexpr f64 k_basin_half_m = 4.0;  // a basin's flat floor, each side of the line

f64 lerp(f64 a, f64 b, f64 u) noexcept { return a + (b - a) * u; }

// A trench's bed and its flat bed's half width at x.
struct Bed {
  f64 y = 0;
  f64 half = 0;
};
Bed bed_of(u32 line, f64 x) noexcept {
  const f64 half = 0.5 * k_section.bed_m;
  if (line == 0) return Bed{k_uniform_bed0 - k_slope * (x - k_head_x), half};
  if (line == 1) {
    const f64 crest = k_step_bed0 - k_slope * (k_crest_x - k_head_x);
    const f64 apron = crest - k_drop_m;
    const f64 floor = apron - k_basin_drop_m;
    if (x <= k_crest_x) return Bed{k_step_bed0 - k_slope * (x - k_head_x), half};
    if (x < k_crest_x + k_spacing_m)
      return Bed{lerp(crest, apron, (x - k_crest_x) / k_spacing_m), half};
    if (x <= k_step_mouth_x) return Bed{apron, half};
    if (x < k_step_mouth_x + k_spacing_m) {
      const f64 u = (x - k_step_mouth_x) / k_spacing_m;
      return Bed{lerp(apron, floor, u), lerp(half, k_basin_half_m, u)};
    }
    if (x <= k_pool_wall_x) return Bed{floor, k_basin_half_m};
    return Bed{floor + (x - k_pool_wall_x), k_basin_half_m};
  }
  const f64 mouth = k_mouth_bed0 - k_slope * (k_lake_mouth_x - k_head_x);
  if (x <= k_lake_mouth_x) return Bed{k_mouth_bed0 - k_slope * (x - k_head_x), half};
  if (x < k_lake_mouth_x + k_spacing_m) {
    const f64 u = (x - k_lake_mouth_x) / k_spacing_m;
    return Bed{lerp(mouth, mouth - k_basin_drop_m, u), lerp(half, k_basin_half_m, u)};
  }
  return Bed{mouth - k_basin_drop_m, k_basin_half_m};
}

// A node of a channel before the stations are chosen.
struct Node {
  f64 x = 0;
  f64 depth = 0;
  u16 flags = 0;
  bool keep = false;
  bool pair = false;
};

// The stations of `nodes` as a description: every number the contract names, from the trapezoid.
scene::WaterChannel describe(const char* name, u32 line, const Vector<Node>& nodes, f64 q, u64 seed,
                             u32& stations) {
  Vector<StationNode> profile;
  for (const Node& node : nodes) {
    StationNode s;
    s.s = node.x;
    s.level = bed_of(line, node.x).y + node.depth;
    s.wet_left = s.wet_right = 0.5 * k_section.top(node.depth);
    s.keep = node.keep;
    s.pair = node.pair;
    profile.push_back(s);
  }
  scene::WaterChannel c;
  c.name = name;
  c.seed = seed;
  c.manning_n = static_cast<f32>(k_manning);
  for (const u32 i : select_stations(profile)) {
    const Node& node = nodes[i];
    const f64 bed = bed_of(line, node.x).y;
    const f64 y = node.depth;
    c.points.push_back(WorldPos{round_to(node.x, 1e3), round_to(bed + y, 1e3), k_line_z[line]});
    c.width_m.push_back(static_cast<f32>(round_to(k_section.top(k_cap_m - bed), 1e3)));
    c.depth_m.push_back(static_cast<f32>(round_to(y, 1e4)));
    c.slope.push_back(
        static_cast<f32>(round_to(trapezoid_friction(k_section, y, q, k_manning), 1e6)));
    c.discharge_m3s.push_back(static_cast<f32>(round_to(q, 1e4)));
    c.speed_mps.push_back(static_cast<f32>(round_to(q / k_section.area(y), 1e4)));
    c.froude.push_back(static_cast<f32>(round_to(trapezoid_froude(k_section, y, q), 1e4)));
    const f32 wet = static_cast<f32>(round_to(0.5 * k_section.top(y), 1e3));
    c.wet_left_m.push_back(wet);
    c.wet_right_m.push_back(wet);
    c.flags.push_back(node.flags);
  }
  stations = c.points.size();
  return c;
}

scene::WaterBody body(const char* name, f64 level, f64 x0, f64 x1, f64 z, f64 half) {
  scene::WaterBody b;
  b.name = name;
  b.level_m = level;
  b.wave_amplitude_m = 0.0f;
  b.wind_follow = false;
  b.shape.polygon.push_back(WorldPos{x0, level, z - half});
  b.shape.polygon.push_back(WorldPos{x1, level, z - half});
  b.shape.polygon.push_back(WorldPos{x1, level, z + half});
  b.shape.polygon.push_back(WorldPos{x0, level, z + half});
  return b;
}

}  // namespace

f64 flow_fixture_ground(f64 x_m, f64 z_m) noexcept {
  const u32 line = z_m < -k_band_m ? 0u : (z_m > k_band_m ? 2u : 1u);
  const Bed bed = bed_of(line, x_m);
  const f64 across = std::fabs(z_m - k_line_z[line]) - bed.half;
  // Each bank rises a metre a metre (the section's `side` is 1), up to the cap.
  return std::min(k_cap_m, bed.y + std::max(0.0, across) / k_section.side);
}

bool make_flow_fixture(FlowFixture& out, std::string& error) {
  out = FlowFixture{};
  FlowFixtureNumbers& numbers = out.numbers;

  // The ground.
  out.spacing_mm = 250;
  out.origin_x_mm = 0;
  out.origin_z_mm = -static_cast<i64>(k_z_half_m * 1000.0);
  out.width = static_cast<u32>(k_x_end_m / k_spacing_m) + 1;
  out.height = static_cast<u32>(2.0 * k_z_half_m / k_spacing_m) + 1;
  out.heights.reserve(out.width * out.height);
  for (u32 j = 0; j < out.height; ++j)
    for (u32 i = 0; i < out.width; ++i)
      out.heights.push_back(static_cast<f32>(flow_fixture_ground(
          k_spacing_m * static_cast<f64>(i), k_spacing_m * static_cast<f64>(j) - k_z_half_m)));

  // The flow: the discharge a uniform reach carries at the chosen normal depth, and its critical
  // depth.
  const Trapezoid& t = k_section;
  const f64 q = trapezoid_discharge(t, k_normal_m, k_manning, k_slope);
  const f64 critical = trapezoid_critical(t, q);
  numbers.discharge_m3s = q;
  numbers.manning_n = k_manning;
  numbers.slope = k_slope;
  numbers.normal_m = k_normal_m;
  numbers.critical_m = critical;
  numbers.normal_froude = trapezoid_froude(t, k_normal_m, q);
  if (!(k_normal_m > critical)) {
    error = "engine.channels: the flow fixture's reach is not mild";
    return false;
  }
  const auto grid = [](f64 from, f64 to, Vector<f64>& xs) {
    const u32 count = static_cast<u32>(std::floor((to - from) / k_spacing_m + 1e-9));
    for (u32 k = 0; k <= count; ++k)
      xs.push_back(from + k_spacing_m * static_cast<f64>(k));
  };

  // "uniform": normal depth at every node.
  {
    Vector<f64> xs;
    grid(k_head_x, k_uniform_end_x, xs);
    Vector<Node> nodes;
    for (const f64 x : xs)
      nodes.push_back(Node{x, k_normal_m, 0, false, false});
    numbers.nodes[0] = nodes.size();
    out.channels.push_back(describe("uniform", 0, nodes, q, 11, numbers.stations[0]));
  }

  // "step".
  {
    const u32 steps = 40000;
    // M2: from critical depth at the crest up the approach toward normal depth.
    DepthTable m2;
    direct_step(t, q, k_manning, k_slope, critical, critical + 0.9999 * (k_normal_m - critical),
                steps, m2);
    // The fall's foot (Rand 1955, hydraulics.h): the area and the distance from the lip, the
    // distance taken up to the lattice.
    const f64 hydraulic_depth = t.area(critical) / t.top(critical);
    const f64 ratio = hydraulic_depth / k_drop_m;
    const f64 foot_area = k_drop_foot_share * powd(ratio, k_drop_foot_exponent) * t.area(critical);
    const f64 foot = t.depth_of_area(foot_area);
    const f64 reach = k_drop_length_heights * k_drop_m * powd(ratio, k_drop_length_exponent);
    const f64 foot_x = k_crest_x + k_spacing_m * std::ceil(reach / k_spacing_m);
    // H3: the supercritical run on the level apron below the foot, toward critical depth.
    DepthTable h3;
    direct_step(t, q, k_manning, 0.0, foot, foot + 0.999 * (critical - foot), steps, h3);
    // H2: from the pool's level at the mouth back up the apron.
    DepthTable h2;
    direct_step(t, q, k_manning, 0.0, k_pool_depth_at_mouth, 4.0 * k_pool_depth_at_mouth, steps,
                h2);
    // The jump: its head where the supercritical depth's sequent depth is the depth the pool's
    // backwater holds a jump's length downstream. `gap` falls through zero as the head moves
    // downstream (the sequent depth falls faster than the backwater does).
    const auto gap = [&](f64 x, f64* head, f64* sequent, f64* length) {
      const f64 y1 = depth_at(h3, x - foot_x);
      const f64 y2 = trapezoid_sequent(t, y1, q);
      const f64 l = k_jump_length_rises * (y2 - y1);
      if (head != nullptr) *head = y1;
      if (sequent != nullptr) *sequent = y2;
      if (length != nullptr) *length = l;
      return y2 - depth_at(h2, x + l - k_step_mouth_x);
    };
    f64 lo = foot_x, hi = k_step_mouth_x - k_spacing_m;
    if (!(gap(lo, nullptr, nullptr, nullptr) > 0.0) ||
        !(gap(hi, nullptr, nullptr, nullptr) < 0.0)) {
      error = "engine.channels: the flow fixture's jump finds no place on its apron";
      return false;
    }
    for (u32 i = 0; i < 200; ++i) {
      const f64 mid = 0.5 * (lo + hi);
      if (!(mid > lo) || !(mid < hi)) break;
      if (gap(mid, nullptr, nullptr, nullptr) > 0.0)
        lo = mid;
      else
        hi = mid;
    }
    // The head to the millimetre; the depths and the length are that head's.
    const f64 head_x = round_to(0.5 * (lo + hi), 1e3);
    f64 head = 0, sequent = 0, length = 0;
    (void)gap(head_x, &head, &sequent, &length);
    const f64 jump_foot_x = round_to(head_x + length, 1e3);

    Vector<Node> nodes;
    Vector<f64> xs;
    grid(k_head_x, k_crest_x, xs);
    for (const f64 x : xs)
      nodes.push_back(Node{x, depth_at(m2, x - k_crest_x), 0, false, false});
    numbers.approach_head_m = nodes[0].depth;
    // The lip: critical depth, the crest's control, and the fall's pair with the foot.
    Node& lip = nodes.back();
    lip.depth = critical;
    lip.flags =
        static_cast<u16>(scene_gen::channel_control_flags(scene_gen::ChannelControl::Crest) |
                         scene_gen::k_channel_fall_lip);
    lip.keep = true;
    lip.pair = true;
    nodes.push_back(Node{foot_x, foot, scene_gen::k_channel_fall_foot, true, false});
    // The apron's lattice nodes between the foot and the jump, never nearer the jump's head than
    // the lattice.
    xs.clear();
    grid(foot_x + k_spacing_m, head_x - k_spacing_m, xs);
    for (const f64 x : xs)
      nodes.push_back(Node{x, depth_at(h3, x - foot_x), 0, false, false});
    nodes.push_back(Node{head_x, head, scene_gen::k_channel_jump_head, true, true});
    nodes.push_back(Node{jump_foot_x, sequent, scene_gen::k_channel_jump_foot, true, false});
    xs.clear();
    grid(k_spacing_m * std::ceil((jump_foot_x + k_spacing_m) / k_spacing_m), k_step_mouth_x, xs);
    for (const f64 x : xs)
      nodes.push_back(Node{x, depth_at(h2, x - k_step_mouth_x), 0, false, false});
    Node& mouth = nodes.back();
    mouth.depth = k_pool_depth_at_mouth;
    mouth.flags = scene_gen::channel_control_flags(scene_gen::ChannelControl::Mouth);
    mouth.keep = true;

    const f64 apron = k_step_bed0 - k_slope * (k_crest_x - k_head_x) - k_drop_m;
    // To the millimetre, as the mouth's station carries it.
    const f64 pool_level = round_to(apron + k_pool_depth_at_mouth, 1e3);
    numbers.crest_x_m = k_crest_x;
    numbers.drop_m = k_drop_m;
    numbers.foot_x_m = foot_x;
    numbers.foot_m = foot;
    numbers.foot_froude = trapezoid_froude(t, foot, q);
    numbers.jump_head_x_m = head_x;
    numbers.jump_head_m = head;
    numbers.jump_head_froude = trapezoid_froude(t, head, q);
    numbers.jump_foot_x_m = jump_foot_x;
    numbers.jump_foot_m = sequent;
    numbers.jump_length_m = length;
    numbers.pool_mouth_m = k_pool_depth_at_mouth;
    numbers.pool_level_m = pool_level;
    numbers.nodes[1] = nodes.size();
    out.channels.push_back(describe("step", 1, nodes, q, 12, numbers.stations[1]));
    // The pool: its basin from the mouth to where its level meets the end wall and the side banks.
    const f64 rise = pool_level - (apron - k_basin_drop_m);
    out.bodies.push_back(body("pool", pool_level, k_step_mouth_x, k_pool_wall_x + rise, k_line_z[1],
                              k_basin_half_m + rise));
  }

  // "mouth": M1, from the lake's level at the mouth back up the reach toward normal depth.
  {
    DepthTable m1;
    direct_step(t, q, k_manning, k_slope, k_lake_depth_at_mouth,
                k_normal_m + 1e-4 * (k_lake_depth_at_mouth - k_normal_m), 40000, m1);
    Vector<f64> xs;
    grid(k_head_x, k_lake_mouth_x, xs);
    Vector<Node> nodes;
    for (const f64 x : xs)
      nodes.push_back(Node{x, depth_at(m1, x - k_lake_mouth_x), 0, false, false});
    Node& mouth = nodes.back();
    mouth.depth = k_lake_depth_at_mouth;
    mouth.flags = scene_gen::channel_control_flags(scene_gen::ChannelControl::Mouth);
    mouth.keep = true;
    const f64 mouth_bed = k_mouth_bed0 - k_slope * (k_lake_mouth_x - k_head_x);
    const f64 level = round_to(mouth_bed + k_lake_depth_at_mouth, 1e3);
    numbers.lake_level_m = level;
    numbers.lake_mouth_m = k_lake_depth_at_mouth;
    numbers.lake_head_m = nodes[0].depth;
    numbers.nodes[2] = nodes.size();
    out.channels.push_back(describe("mouth", 2, nodes, q, 13, numbers.stations[2]));
    const f64 rise = level - (mouth_bed - k_basin_drop_m);
    out.bodies.push_back(
        body("lake", level, k_lake_mouth_x, k_x_end_m, k_line_z[2], k_basin_half_m + rise));
  }
  return true;
}

}  // namespace engine::channels
