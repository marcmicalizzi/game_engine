// Channels of flowing water (channel.h; docs/subsystems/water.md, "Channels"; roadmap S96, S106).
//
// Every number the mesh and the depth map hold is f32 and f64 + - * / and sqrt of the description
// and the ground, in a fixed order (the lattice's nodes sorted by their index, the cells in that
// order), so both files are the same bytes from every toolchain and at any job count.
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/water_channel.h>
#include <systems/water/channel.h>

#include <algorithm>
#include <cmath>

namespace engine::water {

namespace {

constexpr u32 k_none = 0xffffffffu;
// An edge point is kept off the nodes by this share of a cell, so no triangle is degenerate.
constexpr f32 k_edge_margin = 1.0f / 64.0f;
// The least surface width a fall's unit discharge is taken over, metres.
constexpr f32 k_lip_width_min_m = 0.05f;
// The share of each side's wet reach over which a nappe thins to nothing at its side.
constexpr f64 k_nappe_taper = 0.4;

bool finite(f32 v) noexcept { return std::isfinite(v); }

f32 lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }

f32 margin_at(const Channel& c, u32 i) noexcept {
  return (c.flags[i] & scene_gen::k_channel_over_bank) != 0 ? 0.0f : k_channel_under_bank_m;
}

// A fall's drop, metres; 0 for a stretch that is not one, or whose foot does not stand lower.
f32 fall_drop(const Channel& c, u32 i) noexcept {
  if (!stretch_falls(c, i)) return 0.0f;
  return std::max(0.0f, c.points[i].y - c.points[i + 1].y);
}

}  // namespace

bool read_channel(const scene::WaterChannel& block, Channel& out, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = "water channel \"" + block.name + "\": " + std::move(sentence);
    return false;
  };
  out = Channel{};
  out.name = block.name;
  // Solved or refused (W120; channels.md, "What refuses a description"): the one check every
  // reader of a description makes, here so no host can hand the draw a laid level.
  if (!scene_gen::water_channel_solved(block, error)) return false;
  const u32 n = block.points.size();
  if (!(block.manning_n > 0.0f) || !finite(block.manning_n))
    return fail("its manning_n must be positive");
  if (!(block.shore_m >= 0.0f) || !finite(block.shore_m))
    return fail("its shore_m must not be negative");
  if (!(block.roughness >= 0.02f && block.roughness <= 1.0f))
    return fail("its roughness must be within [0.02, 1]");
  if (!(block.transmission >= 0.0f && block.transmission <= 1.0f))
    return fail("its transmission must be within [0, 1]");
  if (!(block.turbidity >= 0.0f) || !finite(block.turbidity))
    return fail("its turbidity must not be negative");
  f64 lo_x = 1e300, hi_x = -1e300, lo_z = 1e300, hi_z = -1e300;
  for (u32 i = 0; i < n; ++i) {
    const WorldPos& p = block.points[i];
    lo_x = std::min(lo_x, p.x);
    hi_x = std::max(hi_x, p.x);
    lo_z = std::min(lo_z, p.z);
    hi_z = std::max(hi_z, p.z);
    if (i == 0) continue;
    const f64 dx = p.x - block.points[i - 1].x;
    const f64 dz = p.z - block.points[i - 1].z;
    if (!(dx * dx + dz * dz > 1e-8))
      return fail("its points " + std::to_string(i - 1) + " and " + std::to_string(i) +
                  " stand at one place in plan; a station is a place along the line, and a "
                  "fall's foot stands downstream of its lip");
  }
  out.origin =
      WorldPos{std::floor(0.5 * (lo_x + hi_x)), block.points[0].y, std::floor(0.5 * (lo_z + hi_z))};
  out.points.reserve(n);
  for (u32 i = 0; i < n; ++i) {
    const WorldPos& p = block.points[i];
    out.points.push_back(Vec3{static_cast<f32>(p.x - out.origin.x),
                              static_cast<f32>(p.y - out.origin.y),
                              static_cast<f32>(p.z - out.origin.z)});
  }
  out.depth_m = block.depth_m;
  out.discharge_m3s = block.discharge_m3s;
  out.wet_left_m = block.wet_left_m;
  out.wet_right_m = block.wet_right_m;
  out.flags = block.flags;
  out.speed_mps.reserve(n);
  out.froude.reserve(n);
  for (u32 i = 0; i < n; ++i) {
    out.speed_mps.push_back(
        std::clamp(block.speed_mps[i], k_channel_min_speed, k_channel_max_speed));
    out.froude.push_back(std::min(block.froude[i], k_channel_froude_max));
  }
  out.manning_n = block.manning_n;
  out.shore_m = block.shore_m;
  out.seed = block.seed;
  return true;
}

// The cube root of a >= 0 by Newton's iteration from above, + - * / alone, a fixed number of
// steps, so it is the same bits on every toolchain (the C library's cbrt is not correctly rounded,
// and differs between them).
f64 cube_root(f64 a) noexcept {
  if (!(a > 0.0)) return 0.0;
  f64 y = std::max(a, 1.0);
  for (u32 i = 0; i < 64; ++i)
    y = (2.0 * y + a / (y * y)) / 3.0;
  return y;
}

f32 manning_speed(f32 n, f32 depth_m, f32 slope) noexcept {
  const f64 d = std::max(static_cast<f64>(depth_m), 0.0);
  const f64 s = std::max(static_cast<f64>(slope), 0.0);
  return static_cast<f32>(cube_root(d * d) * std::sqrt(s) / static_cast<f64>(n));
}

f64 shear_velocity(f64 speed, f64 depth_m, f64 n) noexcept {
  // u* = sqrt(g d S) with S from Manning's v = (1/n) d^(2/3) S^(1/2): u* = v n sqrt(g) / d^(1/6).
  const f64 sixth = std::sqrt(cube_root(std::max(depth_m, 1e-6)));
  return speed * n * std::sqrt(k_gravity) / sixth;
}

f32 froude(f32 speed, f32 depth_m) noexcept {
  const f64 d = std::max(static_cast<f64>(depth_m), 1e-6);
  return static_cast<f32>(static_cast<f64>(speed) / std::sqrt(k_gravity * d));
}

f64 lateral_speed_share(f64 d, f64 hydraulic_depth) noexcept {
  if (!(d > 0.0) || !(hydraulic_depth > 0.0)) return 0.0;
  const f64 r = d / hydraulic_depth;
  return std::min(k_channel_conveyance * cube_root(r * r), k_channel_profile_max);
}

f64 nappe_breakup_m(f64 unit_discharge) noexcept {
  return unit_discharge > 0.0 ? 5.89 * std::pow(unit_discharge, 0.319) : 0.0;
}

f32 station_hydraulic_depth(const Channel& c, u32 i) noexcept {
  const f64 fr = static_cast<f64>(c.froude[i]);
  const f64 v = static_cast<f64>(c.speed_mps[i]);
  const f64 d = fr > 0.0 ? v * v / (k_gravity * fr * fr) : static_cast<f64>(c.depth_m[i]);
  return static_cast<f32>(std::max(d, static_cast<f64>(k_channel_depth_min_m)));
}

bool stretch_falls(const Channel& c, u32 i) noexcept {
  return i + 1 < c.points.size() && (c.flags[i] & scene_gen::k_channel_fall_lip) != 0 &&
         (c.flags[i + 1] & scene_gen::k_channel_fall_foot) != 0;
}

bool stretch_jumps(const Channel& c, u32 i) noexcept {
  return i + 1 < c.points.size() && (c.flags[i] & scene_gen::k_channel_jump_head) != 0 &&
         (c.flags[i + 1] & scene_gen::k_channel_jump_foot) != 0;
}

bool stretch_drawn(const Channel& c, u32 i) noexcept {
  if (i + 1 >= c.points.size()) return false;
  if ((c.flags[i] & c.flags[i + 1] & scene_gen::k_channel_over_bank) == 0) return true;
  return over_bank_stretch_drawn(c.depth_m[i], c.depth_m[i + 1], static_cast<f64>(c.points[i].y),
                                 static_cast<f64>(c.points[i + 1].y));
}

ChannelLine channel_line(const Channel& c) {
  ChannelLine line;
  const u32 n = c.points.size();
  if (n < 2) return line;
  line.station.resize(n);
  line.tau.resize(n);
  line.hydraulic.resize(n);
  line.heading.resize(n - 1);
  line.length.resize(n - 1);
  line.run_from.resize(n - 1);
  line.run_to.resize(n - 1);
  line.station[0] = 0.0f;
  // A round head (channel.h, `Channel::cap_head`) reaches before the first station as far as the
  // sheet is wide there; the phase starts that far's travel late, so it is positive in the cap.
  line.tau[0] = c.cap_head
                    ? static_cast<f64>(std::max(c.wet_left_m[0], c.wet_right_m[0]) +
                                       k_channel_under_bank_m + 2.0f * k_channel_lattice_max_m) /
                          static_cast<f64>(c.speed_mps[0])
                    : 0.0;
  line.lo = line.hi = Vec2{c.points[0].x, c.points[0].z};
  for (u32 i = 0; i < n; ++i) {
    line.hydraulic[i] = station_hydraulic_depth(c, i);
    line.lo = Vec2{std::min(line.lo.x, c.points[i].x), std::min(line.lo.y, c.points[i].z)};
    line.hi = Vec2{std::max(line.hi.x, c.points[i].x), std::max(line.hi.y, c.points[i].z)};
    line.widest = std::max(line.widest, std::max(c.wet_left_m[i], c.wet_right_m[i]));
  }
  for (u32 i = 0; i + 1 < n; ++i) {
    const f32 dx = c.points[i + 1].x - c.points[i].x;
    const f32 dz = c.points[i + 1].z - c.points[i].z;
    const f32 len = std::sqrt(dx * dx + dz * dz);
    line.length[i] = len;
    line.heading[i] = len > 0.0f ? Vec2{dx / len, dz / len} : Vec2{1.0f, 0.0f};
    line.station[i + 1] = line.station[i] + len;
    // The stretch's travel time: a fall's is the free fall's from the lip to the foot's level; a
    // jump's is the head's speed over its length (the head's state holds to the foot); a reach's
    // is its length over the mean slowness of its two stations.
    const f64 l = static_cast<f64>(len);
    const f64 drop = static_cast<f64>(fall_drop(c, i));
    f64 dt = 0.0;
    if (drop > 0.0) {
      dt = std::sqrt(2.0 * drop / k_gravity);
    } else if (stretch_jumps(c, i)) {
      dt = l / static_cast<f64>(c.speed_mps[i]);
    } else {
      dt = 0.5 * l *
           (1.0 / static_cast<f64>(c.speed_mps[i]) + 1.0 / static_cast<f64>(c.speed_mps[i + 1]));
    }
    line.tau[i + 1] = line.tau[i] + dt;
  }
  for (u32 i = 0; i + 1 < n;) {
    if (!stretch_drawn(c, i)) {
      line.run_from[i] = line.run_to[i] = 0.0f;
      ++i;
      continue;
    }
    u32 end = i;
    while (end + 2 < n && stretch_drawn(c, end + 1))
      ++end;
    for (u32 k = i; k <= end; ++k) {
      line.run_from[k] = line.station[i];
      line.run_to[k] = line.station[end + 1];
    }
    i = end + 1;
  }
  // The pieces: a run, cut at each fall's lip and foot (a nappe is a piece of its own).
  line.piece_from.resize(n - 1);
  line.piece_to.resize(n - 1);
  for (u32 i = 0; i + 1 < n;) {
    if (!stretch_drawn(c, i)) {
      line.piece_from[i] = line.piece_to[i] = 0.0f;
      ++i;
      continue;
    }
    u32 end = i;
    if (!(fall_drop(c, i) > 0.0f))
      while (end + 2 < n && stretch_drawn(c, end + 1) && !(fall_drop(c, end + 1) > 0.0f))
        ++end;
    for (u32 k = i; k <= end; ++k) {
      line.piece_from[k] = line.station[i];
      line.piece_to[k] = line.station[end + 1];
    }
    i = end + 1;
  }
  return line;
}

ChannelAt channel_at_stretch(const Channel& c, const ChannelLine& line, u32 i, f32 x,
                             f32 z) noexcept {
  ChannelAt at;
  const u32 n = c.points.size();
  if (n < 2 || i + 1 >= n) return at;
  at.stretch = i;
  const Vec2 dir = line.heading[i];
  const f32 len = line.length[i];
  const f32 px = x - c.points[i].x;
  const f32 pz = z - c.points[i].z;
  const f32 u = px * dir.x + pz * dir.y;
  const f32 tc = std::clamp(u, 0.0f, len);
  const f32 qx = px - dir.x * tc;
  const f32 qz = pz - dir.y * tc;
  at.distance_m = std::sqrt(qx * qx + qz * qz);
  at.left = dir.x * pz - dir.y * px > 0.0f;
  at.heading = dir;
  const f32 t = len > 0.0f ? tc / len : 0.0f;
  at.t = t;
  // Along the line, past the line's own two ends as far as the place is past them, so the sheet
  // is cut square across at its head and its last station.
  f32 along = line.station[i] + tc;
  if (i == 0 && u < 0.0f) along = line.station[0] + u;
  if (i + 2 == n && u > len) along = line.station[i] + u;
  const f32 drop = fall_drop(c, i);
  // A nappe is as wide as its lip all the way down (S108): the water leaves the lip's wet reach
  // and nothing spreads it in the air, where the foot's wet reach is the pool's it lands in (the
  // sheet drawn to that width was a trapezoid wider at its foot than its lip).
  const f32 wet = drop > 0.0f ? (at.left ? c.wet_left_m[i] : c.wet_right_m[i])
                              : (at.left ? lerp(c.wet_left_m[i], c.wet_left_m[i + 1], t)
                                         : lerp(c.wet_right_m[i], c.wet_right_m[i + 1], t));
  // A nappe is in the air: nothing hides a margin, so it has none.
  const f32 margin = drop > 0.0f ? 0.0f : lerp(margin_at(c, i), margin_at(c, i + 1), t);
  at.along = along;
  at.lateral = wet - at.distance_m;
  at.margin = margin;
  if (stretch_drawn(c, i)) {
    // A round end (S108; channel.h, `Channel::cap_head`): no cut across the line at a station
    // that stands in other water, so past it the sheet reaches as far as it is wide.
    const f32 run_lo =
        c.cap_head && line.run_from[i] == line.station[0] ? 1e30f : along - line.run_from[i];
    const f32 run_hi =
        c.cap_tail && line.run_to[i] == line.station[n - 1] ? 1e30f : line.run_to[i] - along;
    const f32 run = std::min(run_lo, run_hi);
    at.inside = std::min(wet + margin - at.distance_m, run);
    at.wet_inside = std::min(wet - at.distance_m, run);
  } else {
    const f32 into = std::min(along - line.station[i], line.station[i + 1] - along);
    at.inside = at.wet_inside = -std::max(into, 0.0f);
  }
  at.depth_m = lerp(c.depth_m[i], c.depth_m[i + 1], t);
  if (drop > 0.0f) {
    // The nappe: a projectile from the lip that lands at the foot's place and level, so its
    // fall time is the free fall's, its horizontal speed the run over it, and its height a
    // parabola of the run.
    const f64 time = std::sqrt(2.0 * static_cast<f64>(drop) / k_gravity);
    const f64 across = static_cast<f64>(len) / time;
    const f64 down = k_gravity * time * static_cast<f64>(t);
    const f64 speed =
        std::min(std::sqrt(across * across + down * down), static_cast<f64>(k_channel_max_speed));
    at.fall = true;
    at.level = c.points[i].y - drop * t * t;
    at.slope = len > 0.0f ? -2.0f * drop * t / len : 0.0f;
    at.speed = static_cast<f32>(speed);
    at.tau = line.tau[i] + time * static_cast<f64>(t);
    const f64 width =
        std::max(static_cast<f64>(c.wet_left_m[i]) + static_cast<f64>(c.wet_right_m[i]),
                 static_cast<f64>(k_lip_width_min_m));
    // **The sheet thins to nothing at its two sides** (S108): whole over the middle of the lip's
    // wet reach, and down to the least the mesh writes over the outer `k_nappe_taper` of each
    // side's reach, where the lip's water is the shallows by its banks. The resolve draws white
    // water by how much water there is, so a fall's white has no straight side.
    const f64 side = std::max(static_cast<f64>(wet), static_cast<f64>(k_lip_width_min_m));
    const f64 taper = std::clamp(static_cast<f64>(at.lateral) / (k_nappe_taper * side), 0.0, 1.0);
    const f64 thickness = std::max(
        std::clamp(static_cast<f64>(c.discharge_m3s[i]) / width / speed,
                   static_cast<f64>(k_channel_depth_min_m), static_cast<f64>(line.hydraulic[i])) *
            taper,
        static_cast<f64>(k_channel_depth_min_m));
    at.hydraulic = -static_cast<f32>(thickness);
    return at;
  }
  at.level = lerp(c.points[i].y, c.points[i + 1].y, t);
  at.slope = len > 0.0f ? (c.points[i + 1].y - c.points[i].y) / len : 0.0f;
  if (stretch_jumps(c, i)) {
    // The roller: the head's shooting flow holds over the jump's length, and the foot's tranquil
    // flow begins at the foot.
    at.speed = c.speed_mps[i];
    at.tau = line.tau[i] + static_cast<f64>(tc) / static_cast<f64>(c.speed_mps[i]);
    at.hydraulic = line.hydraulic[i];
    return at;
  }
  const f64 slow_a = 1.0 / static_cast<f64>(c.speed_mps[i]);
  const f64 slow_b = 1.0 / static_cast<f64>(c.speed_mps[i + 1]);
  const f64 td = static_cast<f64>(t);
  at.speed = static_cast<f32>(1.0 / (slow_a + (slow_b - slow_a) * td));
  at.tau = line.tau[i] + static_cast<f64>(len) * (td * slow_a + 0.5 * td * td * (slow_b - slow_a));
  // **In a round end the phase runs on at the end station's speed** (S108). Held at the station's
  // phase, the triangles across the station had a phase gradient near nothing, which the resolve
  // reads as a speed near infinite — a Froude number past every bound — and drew them as a white
  // wedge at every meeting. (A head's round end is before the head: `channel_line` starts the
  // phase late enough there that it is never negative.)
  if (c.cap_tail && i + 2 == n && u > len) at.tau += static_cast<f64>(u - len) * slow_b;
  if (c.cap_head && i == 0 && u < 0.0f) at.tau += static_cast<f64>(u) * slow_a;
  at.hydraulic = lerp(line.hydraulic[i], line.hydraulic[i + 1], t);
  return at;
}

namespace {

// The distance in plan from (x, z) to stretch `i`.
f32 stretch_distance(const Channel& c, const ChannelLine& line, u32 i, f32 x, f32 z) noexcept {
  const Vec2 dir = line.heading[i];
  const f32 px = x - c.points[i].x;
  const f32 pz = z - c.points[i].z;
  const f32 tc = std::clamp(px * dir.x + pz * dir.y, 0.0f, line.length[i]);
  const f32 qx = px - dir.x * tc;
  const f32 qz = pz - dir.y * tc;
  return std::sqrt(qx * qx + qz * qz);
}

// How far from the line a stretch's sheet can reach, metres.
f32 stretch_reach(const Channel& c, u32 i) noexcept {
  return std::max(std::max(c.wet_left_m[i], c.wet_left_m[i + 1]),
                  std::max(c.wet_right_m[i], c.wet_right_m[i + 1])) +
         k_channel_under_bank_m;
}

}  // namespace

ChannelAt channel_at(const Channel& c, const ChannelLine& line, f32 x, f32 z, u32 first,
                     u32 last) noexcept {
  const u32 n = c.points.size();
  if (n < 2) return ChannelAt{};
  last = std::min(last, n - 2);
  first = std::min(first, last);
  u32 best = first;
  f32 best_d = stretch_distance(c, line, first, x, z);
  for (u32 i = first + 1; i <= last; ++i) {
    const f32 d = stretch_distance(c, line, i, x, z);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return channel_at_stretch(c, line, best, x, z);
}

// ---- where channels meet (channel.h, "Where channels meet"; S108) -------------------------------

ChannelJoins channel_joins(std::span<const Channel> channels, std::span<const ChannelLine> lines,
                           u32 k, bool& cap_head, bool& cap_tail) {
  ChannelJoins joins;
  cap_head = cap_tail = false;
  const Channel& c = channels[k];
  const u32 n = c.points.size();
  if (n < 2) return joins;
  for (u32 e = 0; e < 2; ++e) {
    const u32 s = e == 0 ? 0u : n - 1;
    const f64 ex = c.origin.x + static_cast<f64>(c.points[s].x);
    const f64 ez = c.origin.z + static_cast<f64>(c.points[s].z);
    const f32 own_reach = std::max(c.wet_left_m[s], c.wet_right_m[s]);
    for (u32 j = 0; j < channels.size(); ++j) {
      if (j == k) continue;
      const Channel& o = channels[j];
      const ChannelLine& ol = lines[j];
      if (o.points.size() < 2) continue;
      const f32 x = static_cast<f32>(ex - o.origin.x);
      const f32 z = static_cast<f32>(ez - o.origin.z);
      const f32 pad = ol.widest + k_channel_join_reach_m;
      if (x < ol.lo.x - pad || x > ol.hi.x + pad || z < ol.lo.y - pad || z > ol.hi.y + pad)
        continue;
      const ChannelAt at = channel_at(o, ol, x, z);
      if (!stretch_drawn(o, at.stretch) || at.lateral < -k_channel_join_reach_m) continue;
      (e == 0 ? cap_head : cap_tail) = true;
      // Which of the two keeps its sheet whole here: the one carrying more water; on a tie the one
      // that begins at the meeting (the link below a confluence), then the lower index.
      const u32 so = at.t < 0.5f ? at.stretch : at.stretch + 1;
      const f32 q_own = c.discharge_m3s[s];
      const f32 q_other = o.discharge_m3s[so];
      const bool own_head = e == 0;
      const bool other_head = so == 0;
      const bool outranked =
          q_other > q_own || (q_other == q_own && (other_head != own_head ? other_head : j < k));
      if (!outranked) continue;
      ChannelJoin join;
      join.other = &o;
      join.line = &ol;
      const f32 wet_there = at.lateral + at.distance_m;
      join.radius =
          own_reach + wet_there + 2.0f * k_channel_under_bank_m + k_channel_join_blend_m + 2.0f;
      // The other's stretches within that reach of the meeting, along its line.
      join.first = at.stretch;
      while (join.first > 0 &&
             ol.station[at.stretch] - ol.station[join.first] <= 2.0f * join.radius)
        --join.first;
      join.last = at.stretch;
      while (join.last + 2 < o.points.size() &&
             ol.station[join.last + 1] - ol.station[at.stretch + 1] <= 2.0f * join.radius)
        ++join.last;
      // The side of the other's line this channel arrives from: where its next station stands.
      const u32 before = e == 0 ? 1u : n - 2;
      join.from_left =
          channel_at_stretch(
              o, ol, at.stretch,
              static_cast<f32>(c.origin.x + static_cast<f64>(c.points[before].x) - o.origin.x),
              static_cast<f32>(c.origin.z + static_cast<f64>(c.points[before].z) - o.origin.z))
              .left;
      join.centre = Vec2{c.points[s].x, c.points[s].z};
      join.shift =
          Vec3{static_cast<f32>(c.origin.x - o.origin.x), static_cast<f32>(c.origin.y - o.origin.y),
               static_cast<f32>(c.origin.z - o.origin.z)};
      joins.list.push_back(join);
    }
  }
  return joins;
}

ChannelMeet channel_meet(const ChannelJoins& joins, f32 x, f32 z, f32 level) noexcept {
  ChannelMeet m;
  m.level = level;
  for (const ChannelJoin& j : joins.list) {
    const f32 dx = x - j.centre.x;
    const f32 dz = z - j.centre.y;
    if (dx * dx + dz * dz > j.radius * j.radius) continue;
    const ChannelAt at =
        channel_at(*j.other, *j.line, x + j.shift.x, z + j.shift.z, j.first, j.last);
    // How far outside the joined water's wet reach the place is; negative inside it. Inside it and
    // across its line from where this channel arrives, the place is wholly the joined water's:
    // without this the lap of one cell would keep a sliver of a round end along the far bank.
    const f32 out = at.wet_inside > 0.0f && at.left != j.from_left ? -1e3f : -at.wet_inside;
    m.outside = std::min(m.outside, out);
    const f32 w = std::clamp(1.0f - out / k_channel_join_blend_m, 0.0f, 1.0f);
    if (!(w > 0.0f)) continue;
    // The joined water's level in this channel's frame, and a little under it, so where both
    // sheets are the joined water's lies on top. Never raised: a channel that arrives lower than
    // the water it meets keeps its own level.
    const f32 joined = at.level - j.shift.y - k_channel_join_under_m;
    const f32 share = w * w * (3.0f - 2.0f * w);
    m.level = std::min(m.level, m.level + (joined - m.level) * share);
  }
  return m;
}

f32 channel_lattice(const Channel& c) noexcept {
  f32 narrow = 1e30f;
  for (u32 i = 0; i + 1 < c.points.size(); ++i) {
    if (!stretch_drawn(c, i)) continue;
    for (u32 k = i; k <= i + 1; ++k)
      narrow = std::min(narrow, c.wet_left_m[k] + c.wet_right_m[k] + 2.0f * margin_at(c, k));
  }
  return std::clamp(narrow / 4.0f, k_channel_lattice_min_m, k_channel_lattice_max_m);
}

namespace {

struct Node {
  u64 key;
  f32 distance;
  u32 stretch;
};

// The sheet over a lattice of `h` metres: false when it would pass the vertex budget.
bool build_sheet(const Channel& c, const ChannelLine& line, f32 h, const ChannelJoins* joins,
                 ChannelMesh& out) {
  out = ChannelMesh{};
  out.lattice_m = h;
  out.mesh.spacing = h;
  const u32 n = c.points.size();
  // The lattice's nodes within reach of the line, each with the stretch it is nearest to: a
  // stretch's own bounding box, grown by its reach and two cells so every corner of a cell the
  // sheet touches is found.
  f32 lo_x = 1e30f, lo_z = 1e30f, hi_x = -1e30f, widest = 0.0f;
  for (u32 i = 0; i < n; ++i) {
    lo_x = std::min(lo_x, c.points[i].x);
    hi_x = std::max(hi_x, c.points[i].x);
    lo_z = std::min(lo_z, c.points[i].z);
    widest = std::max(widest, std::max(c.wet_left_m[i], c.wet_right_m[i]));
  }
  const f32 pad = widest + k_channel_under_bank_m + 3.0f * h;
  const i64 x0 = static_cast<i64>(std::floor((lo_x - pad) / h)) - 1;
  const i64 z0 = static_cast<i64>(std::floor((lo_z - pad) / h)) - 1;
  const i64 x1 = static_cast<i64>(std::ceil((hi_x + pad) / h)) + 1;
  const u64 wide = static_cast<u64>(x1 - x0 + 1);
  Vector<Node> found;
  for (u32 i = 0; i + 1 < n; ++i) {
    const f32 reach = stretch_reach(c, i) + 2.0f * h;
    const f32 ax = c.points[i].x, az = c.points[i].z, bx = c.points[i + 1].x,
              bz = c.points[i + 1].z;
    const i64 i0 = static_cast<i64>(std::floor((std::min(ax, bx) - reach) / h));
    const i64 i1 = static_cast<i64>(std::ceil((std::max(ax, bx) + reach) / h));
    const i64 j0 = static_cast<i64>(std::floor((std::min(az, bz) - reach) / h));
    const i64 j1 = static_cast<i64>(std::ceil((std::max(az, bz) + reach) / h));
    for (i64 j = j0; j <= j1; ++j) {
      for (i64 ii = i0; ii <= i1; ++ii) {
        const f32 d =
            stretch_distance(c, line, i, static_cast<f32>(ii) * h, static_cast<f32>(j) * h);
        if (d > reach) continue;
        found.push_back(Node{static_cast<u64>(j - z0) * wide + static_cast<u64>(ii - x0), d, i});
      }
    }
  }
  std::sort(found.begin(), found.end(), [](const Node& a, const Node& b) {
    if (a.key != b.key) return a.key < b.key;
    if (a.distance != b.distance) return a.distance < b.distance;
    return a.stretch < b.stretch;
  });
  Vector<u64> keys;
  Vector<ChannelAt> nodes;
  Vector<Vec2> places;
  for (u32 k = 0; k < found.size(); ++k) {
    if (k > 0 && found[k].key == found[k - 1].key) continue;
    const i64 ii = static_cast<i64>(found[k].key % wide) + x0;
    const i64 j = static_cast<i64>(found[k].key / wide) + z0;
    const Vec2 p{static_cast<f32>(ii) * h, static_cast<f32>(j) * h};
    keys.push_back(found[k].key);
    places.push_back(p);
    nodes.push_back(channel_at_stretch(c, line, found[k].stretch, p.x, p.y));
  }
  const u32 count = nodes.size();
  // Each node against the waters the channel meets (S108): how far outside their wet reaches it
  // is. A place's solve is its own with the level brought down to the joined water's near it.
  Vector<f32> outside(count);
  for (u32 k = 0; k < count; ++k)
    outside[k] = joins != nullptr
                     ? channel_meet(*joins, places[k].x, places[k].y, nodes[k].level).outside + h
                     : 1e30f;
  const auto solved = [&](Vec2 p, ChannelAt at) {
    if (joins != nullptr) at.level = channel_meet(*joins, p.x, p.y, at.level).level;
    return at;
  };
  const auto find = [&](u64 key) {
    const auto it = std::lower_bound(keys.begin(), keys.end(), key);
    return it != keys.end() && *it == key ? static_cast<u32>(it - keys.begin()) : k_none;
  };

  // The vertices as the cells make them, before the phase's wrap: a node's, or a lattice edge's
  // crossing (keyed 2 * node + axis), so two cells that share an edge share its vertex.
  struct Raw {
    Vec3 p;
    Vec3 normal;
    f64 tau;
    f32 hydraulic;
    f32 depth;
  };
  Vector<Raw> raw;
  Vector<u32> node_vertex(count);
  Vector<u32> edge_vertex(count * 2);
  std::fill(node_vertex.begin(), node_vertex.end(), k_none);
  std::fill(edge_vertex.begin(), edge_vertex.end(), k_none);
  const auto add = [&](Vec2 p, const ChannelAt& at) {
    Raw r;
    r.p = Vec3{p.x, at.level, p.y};
    const Vec3 up{-at.slope * at.heading.x, 1.0f, -at.slope * at.heading.y};
    r.normal = up * (1.0f / length(up));
    r.tau = at.tau;
    r.hydraulic = at.hydraulic;
    r.depth = at.depth_m;
    raw.push_back(r);
    return static_cast<u32>(raw.size() - 1);
  };
  // The final vertices: a raw vertex once for each wrap of the phase a triangle reads it under.
  Mesh& m = out.mesh;
  struct Wrapped {
    i64 wrap[2] = {-1, -1};
    u32 vertex[2] = {k_none, k_none};
  };
  Vector<Wrapped> wrapped;
  const f64 period = static_cast<f64>(k_channel_phase_period_s);
  const auto emit = [&](u32 r, i64 wrap) {
    if (wrapped.size() < raw.size()) wrapped.resize(raw.size());
    Wrapped& w = wrapped[r];
    for (u32 k = 0; k < 2; ++k)
      if (w.wrap[k] == wrap) return w.vertex[k];
    const Raw& v = raw[r];
    m.positions.push_back(v.p);
    m.normals.push_back(v.normal);
    m.uvs.push_back(Vec2{static_cast<f32>(v.tau - static_cast<f64>(wrap) * period), v.hydraulic});
    m.max_depth = std::max(m.max_depth, v.depth);
    const u32 index = m.positions.size() - 1;
    for (u32 k = 0; k < 2; ++k) {
      if (w.wrap[k] >= 0) continue;
      w.wrap[k] = wrap;
      w.vertex[k] = index;
      if (k == 1) ++out.wrap_vertices;
      return index;
    }
    ++out.wrap_vertices;  // a third wrap at one vertex: written, not shared
    return index;
  };
  const auto tri = [&](u32 a, u32 b, u32 cc) {
    const Vec3 pa = raw[a].p, pb = raw[b].p, pc = raw[cc].p;
    const f32 ny = (pc.x - pa.x) * (pb.z - pa.z) - (pb.x - pa.x) * (pc.z - pa.z);
    const f64 least = std::min(raw[a].tau, std::min(raw[b].tau, raw[cc].tau));
    const i64 wrap = static_cast<i64>(std::floor(least / period));
    const u32 va = emit(a, wrap), vb = emit(b, wrap), vc = emit(cc, wrap);
    m.indices.push_back(va);
    m.indices.push_back(ny >= 0.0f ? vb : vc);
    m.indices.push_back(ny >= 0.0f ? vc : vb);
    ++out.triangles;
    if (raw[a].hydraulic < 0.0f || raw[b].hydraulic < 0.0f || raw[cc].hydraulic < 0.0f)
      ++out.fall_triangles;
  };
  // **The pieces**: each run of drawn stretches, cut at every fall's lip and foot, so a nappe is a
  // piece of its own. Each piece is the lattice's cells clipped to its own region — within the
  // wet reach (and, off a nappe, the margin under the bank) and between the piece's two ends along
  // the line — so a lip and a foot are straight edges of the mesh and never a line drawn through
  // triangles, and every vertex of a piece is the piece's kind: a reach's carry a hydraulic depth,
  // a nappe's its thickness. Two pieces that meet share their cut's points to the bit (below).
  struct Piece {
    u32 first;
    u32 last;
    f32 from;
    f32 to;
    bool fall;
  };
  Vector<Piece> pieces;
  for (u32 i = 0; i + 1 < n;) {
    if (!stretch_drawn(c, i)) {
      ++i;
      continue;
    }
    u32 end = i;
    while (end + 2 < n && stretch_drawn(c, end + 1) &&
           line.piece_from[end + 1] == line.piece_from[i] &&
           line.piece_to[end + 1] == line.piece_to[i])
      ++end;
    pieces.push_back(Piece{i, end, line.piece_from[i], line.piece_to[i], fall_drop(c, i) > 0.0f});
    i = end + 1;
  }
  u32 poly[8];
  bool poly_edge[8];
  for (const Piece& piece : pieces) {
    std::fill(node_vertex.begin(), node_vertex.end(), k_none);
    std::fill(edge_vertex.begin(), edge_vertex.end(), k_none);
    // The piece's region is where four fields are all positive: past its first station, short of
    // its last (neither at a round end: channel.h, `Channel::cap_head`), inside the wet reach (with
    // the margin, off a nappe), and outside the waters the channel joins, less a cell (S108).
    const bool round_head = c.cap_head && piece.from == line.station[0];
    const bool round_tail = c.cap_tail && piece.to == line.station[n - 1];
    const auto fields = [&](u32 node, f32 out_fields[4]) {
      const ChannelAt& at = nodes[node];
      out_fields[0] = round_head ? 1e30f : at.along - piece.from;
      out_fields[1] = round_tail ? 1e30f : piece.to - at.along;
      out_fields[2] = at.lateral + (piece.fall ? 0.0f : at.margin);
      out_fields[3] = outside[node];
    };
    const auto field = [&](u32 node) {
      f32 f[4];
      fields(node, f);
      return std::min(std::min(f[0], f[1]), std::min(f[2], f[3]));
    };
    for (u32 node = 0; node < count; ++node) {
      const u64 key = keys[node];
      const u32 cn[4] = {node, find(key + 1), find(key + wide + 1), find(key + wide)};
      if (cn[1] == k_none || cn[2] == k_none || cn[3] == k_none) continue;
      // Along +x from a node only if the next key is in the same row (the grid's pad keeps the last
      // column clear, so it is).
      const u32 e[4] = {2 * cn[0], 2 * cn[1] + 1, 2 * cn[3], 2 * cn[0] + 1};
      f32 v[4];
      bool in[4];
      u32 inside = 0;
      for (u32 k = 0; k < 4; ++k) {
        v[k] = field(cn[k]);
        in[k] = v[k] > 0.0f;
        inside += in[k] ? 1u : 0u;
      }
      if (inside == 0) continue;
      // A vertex is the solve at its place among the piece's own stretches.
      const auto corner = [&](u32 k) {
        if (node_vertex[cn[k]] == k_none) {
          const ChannelAt& own = nodes[cn[k]];
          const bool within = own.stretch >= piece.first && own.stretch <= piece.last;
          node_vertex[cn[k]] = add(
              places[cn[k]],
              solved(places[cn[k]], within ? own
                                           : channel_at(c, line, places[cn[k]].x, places[cn[k]].y,
                                                        piece.first, piece.last)));
        }
        return node_vertex[cn[k]];
      };
      const auto edge = [&](u32 k) {
        if (edge_vertex[e[k]] == k_none) {
          // The crossing: of the fields that are not positive at the outside node, the one whose
          // own zero along this edge is nearest the inside node — each field's crossing by itself,
          // not the crossing of their least, which between a cut and a bank is on neither. It is
          // measured from the edge's lower node whichever end is inside: where two pieces meet,
          // their fields along the line are each other's negatives at both nodes, so the two
          // compute the same quotient and the same point, to the bit.
          const u32 k1 = (k + 1) & 3;
          const u32 a = cn[k] < cn[k1] ? k : k1;
          const u32 b = cn[k] < cn[k1] ? k1 : k;
          const Vec2 pa = places[cn[a]];
          const Vec2 pb = places[cn[b]];
          f32 fa[4];
          f32 fb[4];
          fields(cn[a], fa);
          fields(cn[b], fb);
          const bool low_inside = in[a];
          f32 t = low_inside ? 1.0f : 0.0f;
          for (u32 f = 0; f < 4; ++f) {
            if ((low_inside ? fb[f] : fa[f]) > 0.0f) continue;
            const f32 span = fa[f] - fb[f];
            const f32 at_f = span != 0.0f ? fa[f] / span : 0.5f;
            t = low_inside ? std::min(t, at_f) : std::max(t, at_f);
          }
          t = std::clamp(t, k_edge_margin, 1.0f - k_edge_margin);
          const Vec2 p{pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t};
          edge_vertex[e[k]] =
              add(p, solved(p, channel_at(c, line, p.x, p.y, piece.first, piece.last)));
        }
        return edge_vertex[e[k]];
      };
      const auto segment = [&](u32 a, u32 b) {
        out.boundary.push_back(raw[a].p);
        out.boundary.push_back(raw[b].p);
      };
      const bool saddle = in[0] == in[2] && in[1] == in[3] && in[0] != in[1];
      const bool joined = (v[0] + v[1] + v[2] + v[3]) > 0.0f;
      if (saddle && !joined) {
        for (u32 k = 0; k < 4; ++k) {
          if (!in[k]) continue;
          const u32 a = corner(k);
          const u32 b = edge(k);
          const u32 d = edge((k + 3) & 3);
          tri(a, b, d);
          segment(b, d);
        }
        continue;
      }
      // The inside polygon, walked round the cell's corners: a corner inside, and an edge point
      // where the inside begins or ends; convex in every case marching squares makes, so a fan.
      // Two edge points in a row are a piece of the sheet's boundary.
      u32 np = 0;
      for (u32 k = 0; k < 4; ++k) {
        if (in[k]) {
          poly_edge[np] = false;
          poly[np++] = corner(k);
        }
        if (in[k] != in[(k + 1) & 3]) {
          poly_edge[np] = true;
          poly[np++] = edge(k);
        }
      }
      for (u32 k = 1; k + 1 < np; ++k)
        tri(poly[0], poly[k], poly[k + 1]);
      for (u32 k = 0; k < np; ++k) {
        const u32 k1 = (k + 1) % np;
        if (poly_edge[k] && poly_edge[k1]) segment(poly[k], poly[k1]);
      }
      if (m.positions.size() > k_max_vertices) return false;
    }
  }
  return true;
}

}  // namespace

void build_channel_mesh(const Channel& c, ChannelMesh& out, const ChannelJoins* joins) {
  out = ChannelMesh{};
  if (c.points.size() < 2) return;
  const ChannelLine line = channel_line(c);
  if (joins != nullptr && joins->list.empty()) joins = nullptr;
  // The vertex budget, as a body's: the lattice grows by a quarter until the sheet fits.
  for (f32 h = channel_lattice(c);; h *= 1.25f)
    if (build_sheet(c, line, h, joins, out)) return;
}

void build_channel_depth_map(const Channel& c, const ChannelMesh& mesh,
                             const scene_gen::Ground& ground, DepthMap& out,
                             const ChannelJoins* joins) {
  out = DepthMap{};
  const Mesh& m = mesh.mesh;
  if (m.positions.empty()) return;
  const ChannelLine line = channel_line(c);
  f32 lo_x = 1e30f, lo_z = 1e30f, hi_x = -1e30f, hi_z = -1e30f;
  for (const Vec3& p : m.positions) {
    lo_x = std::min(lo_x, p.x);
    hi_x = std::max(hi_x, p.x);
    lo_z = std::min(lo_z, p.z);
    hi_z = std::max(hi_z, p.z);
  }
  // The origin is a whole metre of the world, so a texel of a quarter metre counted from it is the
  // world's multiple; a coarser texel is counted from the square's own corner.
  const f64 side = static_cast<f64>(std::max(hi_x - lo_x, hi_z - lo_z));
  f64 texel = static_cast<f64>(k_channel_map_texel_m);
  if (side / texel + 3.0 > static_cast<f64>(k_depth_map_max))
    texel = side / static_cast<f64>(k_depth_map_max - 3);
  const bool aligned = texel == static_cast<f64>(k_channel_map_texel_m);
  const f64 mid_x = 0.5 * (static_cast<f64>(lo_x) + static_cast<f64>(hi_x));
  const f64 mid_z = 0.5 * (static_cast<f64>(lo_z) + static_cast<f64>(hi_z));
  f64 x0 = mid_x - 0.5 * side - texel;
  f64 z0 = mid_z - 0.5 * side - texel;
  if (aligned) {
    x0 = std::floor(x0 / texel) * texel;
    z0 = std::floor(z0 / texel) * texel;
  }
  const u32 size =
      static_cast<u32>(std::clamp(std::ceil(side / texel) + 3.0, static_cast<f64>(k_depth_map_min),
                                  static_cast<f64>(k_depth_map_max)));
  out.size = size;
  out.origin = Vec2{static_cast<f32>(x0), static_cast<f32>(z0)};
  out.texel_m = static_cast<f32>(texel);
  out.bank_max = k_depth_map_bank_min;
  out.mesh_far_m = mesh.lattice_m;
  out.mesh_near_m = mesh.lattice_m;
  out.mesh_grade = 0.0f;
  const f64 ox = static_cast<f64>(out.origin.x);
  const f64 oz = static_cast<f64>(out.origin.y);
  const f64 tm = static_cast<f64>(out.texel_m);
  const f64 bank = static_cast<f64>(out.bank_max);
  // Each texel's nearest stretch, stretch by stretch over the texels within its reach.
  Vector<f32> nearest(size * size);
  Vector<u32> stretch(size * size);
  std::fill(nearest.begin(), nearest.end(), 1e30f);
  std::fill(stretch.begin(), stretch.end(), k_none);
  const u32 n = c.points.size();
  for (u32 i = 0; i + 1 < n; ++i) {
    const f64 reach = static_cast<f64>(stretch_reach(c, i)) + 2.0 * tm;
    const f64 ax = static_cast<f64>(c.points[i].x), az = static_cast<f64>(c.points[i].z);
    const f64 bx = static_cast<f64>(c.points[i + 1].x), bz = static_cast<f64>(c.points[i + 1].z);
    const i64 i0 =
        std::max<i64>(0, static_cast<i64>(std::floor((std::min(ax, bx) - reach - ox) / tm)));
    const i64 i1 =
        std::min<i64>(size - 1, static_cast<i64>(std::ceil((std::max(ax, bx) + reach - ox) / tm)));
    const i64 j0 =
        std::max<i64>(0, static_cast<i64>(std::floor((std::min(az, bz) - reach - oz) / tm)));
    const i64 j1 =
        std::min<i64>(size - 1, static_cast<i64>(std::ceil((std::max(az, bz) + reach - oz) / tm)));
    for (i64 j = j0; j <= j1; ++j) {
      for (i64 ii = i0; ii <= i1; ++ii) {
        const f32 x = static_cast<f32>(ox + static_cast<f64>(ii) * tm);
        const f32 z = static_cast<f32>(oz + static_cast<f64>(j) * tm);
        const f32 d = stretch_distance(c, line, i, x, z);
        const u32 at = static_cast<u32>(j) * size + static_cast<u32>(ii);
        if (d < nearest[at]) {
          nearest[at] = d;
          stretch[at] = i;
        }
      }
    }
  }
  Vector<f64> depth(size * size);
  f64 deepest = 0.0;
  for (u32 j = 0; j < size; ++j) {
    for (u32 i = 0; i < size; ++i) {
      const u32 t = j * size + i;
      depth[t] = -bank;
      if (stretch[t] == k_none) continue;
      const f32 x = static_cast<f32>(ox + static_cast<f64>(i) * tm);
      const f32 z = static_cast<f32>(oz + static_cast<f64>(j) * tm);
      const ChannelAt at = channel_at_stretch(c, line, stretch[t], x, z);
      // Within a texel of the sheet: the level less the ground. Past it: bank.
      if (!(at.inside > -static_cast<f32>(tm))) continue;
      f64 d = 0.0;
      if (ground.present()) {
        // The level the sheet stands at here: the solve's, brought down to the water it joins.
        const f32 stands = joins != nullptr ? channel_meet(*joins, x, z, at.level).level : at.level;
        const f64 level = c.origin.y + static_cast<f64>(stands);
        const WorldPos world{c.origin.x + static_cast<f64>(x), level,
                             c.origin.z + static_cast<f64>(z)};
        d = level - static_cast<f64>(ground.surface(world));
      } else {
        d = at.wet_inside > 0.0f ? static_cast<f64>(at.depth_m) : -bank;
      }
      if (!(at.inside > 0.0f)) d = std::min(d, 0.0);
      depth[t] = std::max(d, -bank);
      deepest = std::max(deepest, d);
    }
  }
  out.depth_max = static_cast<f32>(deepest);
  out.texels.resize(size * size);
  const f64 range = static_cast<f64>(out.depth_max) + bank;
  for (u32 t = 0; t < size * size; ++t)
    out.texels[t] = static_cast<u16>(
        std::clamp(std::floor((depth[t] + bank) / range * 65535.0 + 0.5), 0.0, 65535.0));
}

ChannelEdge channel_edge_exposed(const Channel& c, const ChannelMesh& mesh,
                                 const scene_gen::Ground& ground, f32 step, f32 clear_m) {
  ChannelEdge edge;
  const ChannelLine line = channel_line(c);
  for (u32 k = 0; k + 1 < mesh.boundary.size(); k += 2) {
    const Vec3 a = mesh.boundary[k];
    const Vec3 b = mesh.boundary[k + 1];
    const f32 dx = b.x - a.x, dz = b.z - a.z;
    const f32 len = std::sqrt(dx * dx + dz * dz);
    const u32 steps = std::max(1u, static_cast<u32>(std::ceil(len / step)));
    for (u32 s = 0; s <= steps; ++s) {
      const f32 t = static_cast<f32>(s) / static_cast<f32>(steps);
      const Vec3 p = a + (b - a) * t;
      ++edge.samples;
      const ChannelAt at = channel_at(c, line, p.x, p.z);
      // A cut across the line: the sample is well inside the wet reach's margin, so what ended
      // the sheet here is its run, not its bank. A fall's sides are the nappe's own, in the air.
      if (at.lateral + at.margin > 0.02f || at.fall) {
        ++edge.cuts;
        continue;
      }
      const f64 level = c.origin.y + static_cast<f64>(p.y);
      const WorldPos world{c.origin.x + static_cast<f64>(p.x), level,
                           c.origin.z + static_cast<f64>(p.z)};
      const f32 over = static_cast<f32>(level - static_cast<f64>(ground.surface(world)));
      if (over > clear_m) {
        ++edge.exposed;
        edge.worst_m = std::max(edge.worst_m, over);
      }
    }
  }
  return edge;
}

bool channel_sample(const Channel& c, const ChannelLine& line, const scene_gen::Ground& ground,
                    WorldPos at_world, ChannelSample& out, u32 first, u32 last) noexcept {
  out = ChannelSample{};
  if (c.points.size() < 2) return false;
  const f32 x = static_cast<f32>(at_world.x - c.origin.x);
  const f32 z = static_cast<f32>(at_world.z - c.origin.z);
  const ChannelAt at = channel_at(c, line, x, z, first, last);
  if (!(at.wet_inside > 0.0f)) return false;
  const f64 level = c.origin.y + static_cast<f64>(at.level);
  f64 depth = static_cast<f64>(at.depth_m);
  if (ground.present())
    depth = level - static_cast<f64>(ground.surface(WorldPos{at_world.x, level, at_world.z}));
  if (!(depth > 0.0)) return false;
  out.level_m = level;
  out.depth_m = static_cast<f32>(depth);
  out.fall = at.fall;
  f64 speed = static_cast<f64>(at.speed);
  if (!at.fall) {
    const f64 hydraulic = static_cast<f64>(at.hydraulic);
    out.froude = static_cast<f32>(speed / std::sqrt(k_gravity * hydraulic));
    speed *= k_channel_surface_factor * lateral_speed_share(depth, hydraulic);
  }
  const f64 rise = static_cast<f64>(at.slope);
  const f64 unit = 1.0 / std::sqrt(1.0 + rise * rise);
  out.velocity = Vec3{static_cast<f32>(speed * unit * static_cast<f64>(at.heading.x)),
                      static_cast<f32>(speed * unit * rise),
                      static_cast<f32>(speed * unit * static_cast<f64>(at.heading.y))};
  return true;
}

JsonValue channel_generator_params(std::span<const scene::WaterChannel> channels) {
  JsonValue list = JsonValue::array();
  for (const scene::WaterChannel& channel : channels)
    list.push_back(schema::to_json(channel));
  JsonValue params = JsonValue::object();
  params.set("channels", std::move(list));
  return params;
}

}  // namespace engine::water
