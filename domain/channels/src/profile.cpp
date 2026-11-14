// A stream's surface, solved in its own bed (profile.h; channels.md, "The profile"; roadmap W120
// stage B).
#include "detail.h"

#include <domain/channels/hydraulics.h>
#include <domain/channels/profile.h>
#include <domain/channels/stations.h>

#include <cmath>

namespace engine::channels {

using detail::powd;
using detail::round_to;

namespace {

// How far over a bed a level is looked for, metres: no stream here stands deeper.
constexpr f64 k_depth_max_m = 60.0;
// The least depth a shooting profile is looked for at, and the least a station is written with.
constexpr f64 k_depth_min_m = 1e-6;
constexpr f64 k_station_depth_min_m = 0.005;
// A rise of the level between two stations that is counted, and a rise of the head.
constexpr f64 k_rise_m = 0.0005;
constexpr f64 k_head_rise_m = 0.001;
// A change of regime with no solved jump (a choke) is drawn as a jump when the level steps up by
// more than the stations' pin.
constexpr f64 k_choke_jump_m = k_station_level_m;

// The root of a function rising through zero between lo and hi, bisected until the bracket is one
// float wide: the same bits wherever it runs.
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

// The nodes of a line and the ground across each.
struct Sections {
  u32 nodes = 0;
  u32 across = 0;  // samples a node, `centre` the one on the line; a larger index is further left
  u32 centre = 0;
  f64 step = 0;  // between samples across, metres
  f64 run = 0;   // between nodes along, metres
  Vector<f64> h;
  Vector<u32> thalweg, left_end, right_end;
  Vector<f64> x, z, q, width, bed, bank, critical, slope0, slope_power;
  const f64* row(u32 node) const noexcept { return h.data() + node * across; }
  f64 offset(u32 k) const noexcept {
    return (static_cast<f64>(k) - static_cast<f64>(centre)) * step;
  }
};

// What a level wets of a node's section: only ground connected to the thalweg under the level,
// and no further than each bank's crest.
struct Wet {
  f64 area = 0;
  f64 perimeter = 0;
  f64 top = 0;
  f64 moment = 0;  // the area's first moment about the surface
  f64 left = 0, right = 0;
  bool over = false;
};

Wet wet_at(const Sections& c, u32 node, f64 level) noexcept {
  Wet w;
  const f64* h = c.row(node);
  const u32 t = c.thalweg[node];
  if (!(level > h[t])) return w;
  const f64 d = c.step;
  // One side: `dir` +1 toward the left (rising index), -1 toward the right, as far as `end`.
  const auto side = [&](i32 dir, u32 end) {
    u32 k = t;
    while (k != end) {
      const u32 next = static_cast<u32>(static_cast<i32>(k) + dir);
      const f64 ha = h[k], hb = h[next];
      const f64 da = level - ha;
      if (hb >= level) {
        const f64 across = d * da / (hb - ha);
        w.area += 0.5 * da * across;
        w.perimeter += std::sqrt(across * across + da * da);
        w.top += across;
        w.moment += across * da * da / 6.0;
        return c.offset(k) + static_cast<f64>(dir) * across;
      }
      const f64 db = level - hb;
      w.area += 0.5 * d * (da + db);
      w.perimeter += std::sqrt(d * d + (hb - ha) * (hb - ha));
      w.top += d;
      w.moment += d * (da * da + da * db + db * db) / 6.0;
      k = next;
    }
    if (level > h[end] + k_over_bank_m) w.over = true;
    return c.offset(end);
  };
  const f64 left = side(1, c.left_end[node]);
  const f64 right = side(-1, c.right_end[node]);
  w.left = std::max(0.0, left);
  w.right = std::max(0.0, -right);
  return w;
}

// The flow of a node's discharge at a level.
struct Flow {
  bool ok = false;
  f64 level = 0;
  f64 area = 0;
  f64 top = 0;
  f64 radius = 0;
  f64 speed = 0;
  f64 head = 0;
  f64 friction = 0;
  f64 froude = 0;
  f64 force = 0;
  f64 n = 0;
};

struct Solver {
  const Sections& c;
  const ProfileOptions& o;

  Flow at(u32 node, f64 level) const noexcept {
    Flow f;
    const Wet w = wet_at(c, node, level);
    if (!(w.area > 0.0) || !(w.perimeter > 0.0) || !(w.top > 0.0)) return f;
    const f64 q = c.q[node];
    f.ok = true;
    f.level = level;
    f.area = w.area;
    f.top = w.top;
    f.radius = w.area / w.perimeter;
    f.speed = q / w.area;
    f.head = level + f.speed * f.speed / (2.0 * k_gravity);
    f.n = o.resistance == Resistance::Manning
              ? o.manning_n
              : k_jarrett_n * c.slope_power[node] *
                    powd(std::clamp(f.radius, k_jarrett_radius_min_m, k_jarrett_radius_max_m),
                         k_jarrett_radius_exponent);
    f.friction = f.n * f.n * f.speed * f.speed / (f.radius * fall_cube_root(f.radius));
    f.froude = f.speed / std::sqrt(k_gravity * w.area / w.top);
    f.force = q * q / (k_gravity * w.area) + w.moment;
    return f;
  }

  // Q^2 T = g A^3.
  f64 critical(u32 node) const noexcept {
    const f64 q = c.q[node];
    const f64 bed = c.bed[node];
    return bisect(bed + k_depth_min_m, bed + k_depth_max_m, [&](f64 level) {
      const Wet w = wet_at(c, node, level);
      return k_gravity * w.area * w.area * w.area - q * q * w.top;
    });
  }
  // The level whose friction slope is the bed's.
  f64 normal(u32 node) const noexcept {
    const f64 bed = c.bed[node];
    return bisect(bed + k_depth_min_m, bed + k_depth_max_m, [&](f64 level) {
      const Flow f = at(node, level);
      return f.ok ? c.slope0[node] - f.friction : -1.0;
    });
  }
  // The level whose wetted area is `area`.
  f64 level_of_area(u32 node, f64 area) const noexcept {
    const f64 bed = c.bed[node];
    return bisect(bed + k_depth_min_m, bed + k_depth_max_m,
                  [&](f64 level) { return wet_at(c, node, level).area - area; });
  }
  // The tranquil level on the far side of critical with the same specific force.
  f64 sequent(u32 node, f64 level) const noexcept {
    const f64 force = at(node, level).force;
    const f64 lc = c.critical[node];
    return bisect(lc, c.bed[node] + k_depth_max_m, [&](f64 l) {
      const Flow f = at(node, l);
      return f.ok ? f.force - force : -1.0;
    });
  }
  // **The standard step upstream**: node's level from the flow one node downstream, in tranquil
  // flow. `critical` when no tranquil level carries the head: the node passes critical depth.
  f64 tranquil(u32 node, const Flow& down, bool& is_critical) const noexcept {
    const f64 lc = c.critical[node];
    const f64 target = down.head + 0.5 * c.run * down.friction;
    const auto fn = [&](f64 level) {
      const Flow f = at(node, level);
      return f.ok ? f.head - 0.5 * c.run * f.friction - target : -1.0;
    };
    if (fn(lc) >= 0.0) {
      is_critical = true;
      return lc;
    }
    is_critical = false;
    return bisect(lc, c.bed[node] + k_depth_max_m, fn);
  }
  // **The standard step downstream**: node's level from the flow one node upstream, in shooting
  // flow. False when even critical depth has more head than arrives: the flow cannot stay
  // shooting (a choke).
  bool shooting(u32 node, const Flow& up, f64& level) const noexcept {
    const f64 lc = c.critical[node];
    const f64 target = up.head - 0.5 * c.run * up.friction;
    const auto fn = [&](f64 l) {
      const Flow f = at(node, l);
      return f.ok ? target - (f.head + 0.5 * c.run * f.friction) : -1.0;
    };
    if (fn(lc) < 0.0) return false;
    level = bisect(c.bed[node] + k_depth_min_m, lc, fn);
    return true;
  }
};

// A station before the selection: where it is along the line, its level, the node whose section
// it is read in, its flags, and whether it and the next are a pair.
struct Out {
  f64 s = 0;
  f64 level = 0;
  u32 node = 0;
  u16 flags = 0;
  bool keep = false;
  bool pair = false;
};

bool refuse(std::string& error, const std::string& name, const char* why) {
  error = "engine.channels: flow line \"" + name + "\": " + why;
  return false;
}

}  // namespace

bool solve_profile(const FlowLine& line, const scene_gen::Ground& ground,
                   const ProfileOptions& options, Profile& out, std::string& error) {
  out = Profile{};
  const u32 count = line.points.size();
  if (count < 2) return refuse(error, line.name, "it has fewer than two points");
  if (!ground.present()) return refuse(error, line.name, "there is no ground to solve it in");
  if (!detail::in(options.lattice_m, 0.01, 100.0))
    return refuse(error, line.name, "the lattice must be 1 cm to 100 m");
  if (options.resistance == Resistance::Manning && !detail::in(options.manning_n, 0.005, 1.0))
    return refuse(error, line.name, "Manning's n must be 0.005 to 1");
  if (!detail::in(options.reach_m, options.lattice_m, 1000.0))
    return refuse(error, line.name, "the reach across the line must be a lattice to 1 km");
  if (line.discharge.size() != 1 && line.discharge.size() != count)
    return refuse(error, line.name, "its discharge is one number or one a point");
  if (!line.width.empty() && line.width.size() != count)
    return refuse(error, line.name, "its width is absent or one a point");
  for (const f64 q : line.discharge)
    if (!(q > 0.0) || !std::isfinite(q))
      return refuse(error, line.name, "its discharge must be positive");
  for (const f64 w : line.width)
    if (!(w > 0.0) || !std::isfinite(w))
      return refuse(error, line.name, "its width must be positive");
  if (line.tail_level && !std::isfinite(line.level))
    return refuse(error, line.name, "the level it ends at is not a number");
  Vector<f64> along;
  along.push_back(0.0);
  for (u32 i = 0; i < count; ++i) {
    const WorldPos& p = line.points[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.z))
      return refuse(error, line.name, "a point is not a number");
    if (i > 0) {
      const f64 dx = p.x - line.points[i - 1].x, dz = p.z - line.points[i - 1].z;
      along.push_back(along[i - 1] + std::sqrt(dx * dx + dz * dz));
    }
  }
  const f64 total = along[count - 1];
  if (!(total > 1e-6)) return refuse(error, line.name, "it has no length");

  // ---- The nodes, and the ground across each --------------------------------------------------
  Sections c;
  const u32 steps =
      std::max<u32>(1, static_cast<u32>(std::floor(total / options.lattice_m + 1e-9)));
  c.nodes = steps + 1;
  c.run = total / static_cast<f64>(steps);
  c.step = options.lattice_m;
  f64 widest = 0.0;
  for (const f64 w : line.width)
    widest = std::max(widest, w);
  // Across: where the line names its bed's width, one and a half of the widest each side (a
  // mouth's fan opens the cut past its width) and four lattices more, so both crests are seen.
  const f64 half = line.width.empty() ? options.reach_m : 1.5 * widest + 4.0 * options.lattice_m;
  c.centre = std::clamp<u32>(static_cast<u32>(std::ceil(half / c.step)), 2, 4000);
  c.across = 2 * c.centre + 1;
  const u32 n = c.nodes;
  c.x.resize(n);
  c.z.resize(n);
  c.q.resize(n);
  c.width.resize(n);
  {
    u32 seg = 0;
    for (u32 k = 0; k < n; ++k) {
      const f64 s = k + 1 == n ? total : c.run * static_cast<f64>(k);
      while (seg + 2 < count && along[seg + 1] < s)
        ++seg;
      const f64 span = along[seg + 1] - along[seg];
      const f64 u = span > 0.0 ? std::clamp((s - along[seg]) / span, 0.0, 1.0) : 0.0;
      const WorldPos& a = line.points[seg];
      const WorldPos& b = line.points[seg + 1];
      c.x[k] = a.x + u * (b.x - a.x);
      c.z[k] = a.z + u * (b.z - a.z);
      c.q[k] = line.discharge.size() == 1
                   ? line.discharge[0]
                   : line.discharge[seg] + u * (line.discharge[seg + 1] - line.discharge[seg]);
      c.width[k] =
          line.width.empty() ? 0.0 : line.width[seg] + u * (line.width[seg + 1] - line.width[seg]);
    }
  }
  c.h.resize(n * c.across);
  c.thalweg.resize(n);
  c.left_end.resize(n);
  c.right_end.resize(n);
  c.bed.resize(n);
  c.bank.resize(n);
  {
    f64 hx = 1.0, hz = 0.0;
    for (u32 k = 0; k < n; ++k) {
      const u32 p = k > 0 ? k - 1 : k, r = k + 1 < n ? k + 1 : k;
      const f64 dx = c.x[r] - c.x[p], dz = c.z[r] - c.z[p];
      const f64 l = std::sqrt(dx * dx + dz * dz);
      if (l > 1e-9) {
        hx = dx / l;
        hz = dz / l;
      }
      // Left is the side of (-dz, dx) for a line running (dx, dz).
      const f64 nx = -hz, nz = hx;
      f64* row = c.h.data() + k * c.across;
      for (u32 j = 0; j < c.across; ++j) {
        const f64 off = c.offset(j);
        row[j] = ground.surface_mm(scene_gen::nearest_mm(c.x[k] + off * nx),
                                   scene_gen::nearest_mm(c.z[k] + off * nz));
        if (!std::isfinite(row[j])) return refuse(error, line.name, "the ground is not a number");
      }
      // The thalweg: the lowest sample within the bed's half width of the line (the nearest to
      // the line where several are as low), then downhill from there to a hollow's bottom.
      const u32 reach = line.width.empty()
                            ? 0u
                            : std::min<u32>(c.centre, static_cast<u32>(0.5 * c.width[k] / c.step));
      u32 t = c.centre;
      for (u32 e = 1; e <= reach; ++e) {
        if (row[c.centre + e] < row[t]) t = c.centre + e;
        if (row[c.centre - e] < row[t]) t = c.centre - e;
      }
      for (;;) {
        const bool up = t + 1 < c.across && row[t + 1] < row[t];
        const bool down = t > 0 && row[t - 1] < row[t];
        if (up && (!down || row[t + 1] <= row[t - 1]))
          ++t;
        else if (down)
          --t;
        else
          break;
      }
      c.thalweg[k] = t;
      c.bed[k] = row[t];
      // Each bank's crest: the highest ground reached walking out, before it falls away.
      u32 crest = t;
      for (u32 j = t + 1; j < c.across; ++j) {
        if (row[j] > row[crest]) {
          crest = j;
        } else if (row[j] < row[crest] - k_bank_crest_drop_m -
                                k_bank_crest_drop_share * (row[crest] - row[t])) {
          break;
        }
      }
      c.left_end[k] = crest;
      crest = t;
      for (u32 j = t; j-- > 0;) {
        if (row[j] > row[crest]) {
          crest = j;
        } else if (row[j] < row[crest] - k_bank_crest_drop_m -
                                k_bank_crest_drop_share * (row[crest] - row[t])) {
          break;
        }
      }
      c.right_end[k] = crest;
      c.bank[k] = std::min(row[c.left_end[k]], row[c.right_end[k]]);
    }
  }
  c.slope0.assign(n, 1e-5);
  c.slope_power.assign(n, 1.0);
  c.critical.resize(n);
  const Solver solver{c, options};
  for (u32 k = 0; k < n; ++k)
    c.critical[k] = solver.critical(k);

  // ---- The falls: the bed's own steps that pass S104's test at the solved critical depth ------
  struct Drop {
    u32 lip = 0;
    u32 foot = 0;
    f64 area = 0;  // Rand's area at the foot
  };
  Vector<Drop> drops;
  for (u32 k = 0; k + 1 < n;) {
    if (c.bed[k] - c.bed[k + 1] < k_face_gradient * c.run) {
      ++k;
      continue;
    }
    u32 end = k + 1;
    while (end + 1 < n && c.bed[end] - c.bed[end + 1] >= k_face_gradient * c.run)
      ++end;
    const f64 drop = c.bed[k] - c.bed[end];
    const Wet w = wet_at(c, k, c.critical[k]);
    const f64 depth = w.top > 0.0 ? w.area / w.top : 0.0;
    if (depth >= k_fall_min_critical_m && drop >= k_fall_min_drop_depths * depth) {
      const f64 ratio = depth / drop;
      const f64 reach = k_drop_length_heights * drop * powd(ratio, k_drop_length_exponent);
      u32 foot = end;
      while (foot + 1 < n && c.run * static_cast<f64>(foot - k) < reach &&
             c.bed[foot] - c.bed[foot + 1] < k_face_gradient * c.run)
        ++foot;
      drops.push_back(
          Drop{k, foot, k_drop_foot_share * powd(ratio, k_drop_foot_exponent) * w.area});
      k = foot;
    } else {
      k = end;
    }
  }

  // ---- Each stretch between falls ---------------------------------------------------------------
  Vector<f64> sub(n, 0.0);
  Vector<u8> crit(n, 0);
  Vector<Out> outs;
  ProfileStats& stats = out.stats;
  const u16 tail_flags = scene_gen::channel_control_flags(line.tail);
  const u16 crest_flags = scene_gen::channel_control_flags(scene_gen::ChannelControl::Crest);
  const u32 stretches = drops.size() + 1;
  for (u32 r = 0; r < stretches; ++r) {
    const u32 a = r == 0 ? 0u : drops[r - 1].foot;
    const u32 b = r + 1 < stretches ? drops[r].lip : n - 1;
    const bool lip = r + 1 < stretches;
    // The bed's slope over the reach about each node, inside this stretch.
    const u32 window = std::max<u32>(1, static_cast<u32>(std::ceil(k_reach_slope_m / c.run)));
    for (u32 i = a; i <= b; ++i) {
      const u32 i0 = i - a > window ? i - window : a;
      const u32 i1 = b - i > window ? i + window : b;
      const f64 slope =
          i1 > i0 ? (c.bed[i0] - c.bed[i1]) / (c.run * static_cast<f64>(i1 - i0)) : 0.0;
      c.slope0[i] = std::max(slope, 1e-5);
      c.slope_power[i] = powd(std::clamp(slope, k_jarrett_slope_min, k_jarrett_slope_max),
                              k_jarrett_slope_exponent);
    }
    // The tranquil profile, from the stretch's foot back to its head.
    bool tail_control = false;
    if (lip) {
      sub[b] = c.critical[b];
      crit[b] = 1;
    } else if (line.tail_level) {
      tail_control = line.level > c.critical[b];
      sub[b] = tail_control ? line.level : c.critical[b];
      crit[b] = tail_control ? 0 : 1;
    } else {
      const f64 level = solver.normal(b);
      crit[b] = level > c.critical[b] ? 0 : 1;
      sub[b] = crit[b] != 0 ? c.critical[b] : level;
    }
    for (u32 i = b; i-- > a;) {
      bool is_critical = false;
      f64 level = solver.tranquil(i, solver.at(i + 1, sub[i + 1]), is_critical);
      // The level never rises downstream in tranquil flow: where the energy equation alone would
      // have it (a fast, narrow section opening into a slow, deep one), the velocity head the
      // slowing water would recover is taken as lost in the expansion, and the level is held.
      if (level < sub[i + 1]) {
        level = sub[i + 1];
        is_critical = false;
      }
      sub[i] = level;
      crit[i] = is_critical ? 1 : 0;
    }
    const f64 s_a = c.run * static_cast<f64>(a);
    const auto tail_at = [&](f64 s) {
      const f64 f = std::clamp((s - s_a) / c.run, 0.0, static_cast<f64>(b - a));
      const u32 i = std::min<u32>(b - a, static_cast<u32>(f));
      const u32 j = std::min<u32>(b - a, i + 1);
      return sub[a + i] + (f - static_cast<f64>(i)) * (sub[a + j] - sub[a + i]);
    };
    // What a node is besides its level.
    const auto node_flags = [&](u32 i, bool critical_here, bool tranquil_here) {
      u16 flags = 0;
      if (i == a && r > 0) flags |= scene_gen::k_channel_fall_foot;
      if (i == b && lip) flags |= scene_gen::k_channel_fall_lip;
      if (critical_here) flags |= crest_flags;
      if (i == n - 1 && tail_control && tranquil_here) flags |= tail_flags;
      if (i == 0 && line.head_spill)
        flags |= scene_gen::channel_control_flags(scene_gen::ChannelControl::Spill);
      return flags;
    };
    const auto emit = [&](u32 i, f64 level, bool critical_here, bool tranquil_here) {
      Out o;
      o.s = c.run * static_cast<f64>(i);
      o.level = level;
      o.node = i;
      o.flags = node_flags(i, critical_here, tranquil_here);
      o.keep = o.flags != 0 || i == 0 || i == n - 1;
      o.pair = i == b && lip;
      if (critical_here && !(i == b && lip)) ++stats.crests;
      outs.push_back(o);
    };
    // The regime, node by node downstream: tranquil, until a node the flow shoots from (a fall's
    // foot, a node that passed critical depth), then shooting until a jump or a choke.
    u32 i = a;
    while (i <= b) {
      bool start = false, critical_start = false;
      f64 level = 0.0;
      if (i == a && r > 0) {
        const f64 foot = solver.level_of_area(a, drops[r - 1].area);
        if (foot < c.critical[a]) {
          const f64 seq = solver.sequent(a, foot);
          if (seq - tail_at(s_a + k_jump_length_rises * (seq - foot)) > 0.0) {
            start = true;
            level = foot;
          } else {
            ++stats.drowned_feet;
          }
        }
      }
      if (!start && crit[i] != 0 && i < b) {
        start = critical_start = true;
        level = c.critical[i];
      }
      if (!start) {
        emit(i, sub[i], crit[i] != 0, true);
        ++i;
        continue;
      }
      emit(i, level, critical_start, false);
      Flow up = solver.at(i, level);
      f64 before_level = level;
      f64 before_seq = critical_start ? level : solver.sequent(i, level);
      f64 before_gap = critical_start
                           ? 0.0
                           : before_seq - tail_at(c.run * static_cast<f64>(i) +
                                                  k_jump_length_rises * (before_seq - level));
      u32 next = b + 1;
      bool ended = false;
      // **A choke**: the shooting flow has not the head to pass `first` (not even at critical
      // depth there, or against the level the line ends in), so the water is tranquil from
      // somewhere upstream of it. The jump stands at the last shooting node that still has the
      // head the tranquil profile holds one node below it, so the total head never rises
      // downstream; a fall's foot with less head than that is drowned. Returns the node the
      // tranquil profile goes on from.
      const auto settle = [&](u32 first) {
        u32 j = first - 1;
        while (j > i && solver.at(j, outs.back().level).head < solver.at(j + 1, sub[j + 1]).head) {
          outs.pop_back();
          --j;
        }
        if (j == i && !critical_start &&
            solver.at(i, outs.back().level).head < solver.at(i + 1, sub[i + 1]).head) {
          outs.back().level = sub[i];
          ++stats.drowned_feet;
          return i + 1;
        }
        if (sub[j + 1] - outs.back().level > k_choke_jump_m && !(j == i && critical_start)) {
          Out& jump = outs.back();
          jump.flags |= scene_gen::k_channel_jump_head;
          jump.keep = jump.pair = true;
          emit(j + 1, sub[j + 1], crit[j + 1] != 0, true);
          outs.back().flags |= scene_gen::k_channel_jump_foot;
          outs.back().keep = true;
          ++stats.jumps;
          return j + 2;
        }
        return j + 1;
      };
      for (u32 k = i + 1; k <= b; ++k) {
        f64 here = 0.0;
        if (!solver.shooting(k, up, here)) {
          next = settle(k);  // a choke
          ended = true;
          break;
        }
        const f64 s_k = c.run * static_cast<f64>(k);
        const f64 seq = solver.sequent(k, here);
        const f64 gap = seq - tail_at(s_k + k_jump_length_rises * (seq - here));
        if (gap > 0.0) {
          emit(k, here, false, false);
          up = solver.at(k, here);
          before_level = here;
          before_seq = seq;
          before_gap = gap;
          continue;
        }
        ended = true;
        if (k - 1 == i && critical_start) {
          // The node passed critical depth and the tranquil profile holds at once below it.
          next = k;
          break;
        }
        // **The jump**: its head where the shooting level's sequent level is the level the
        // tranquil profile holds a jump's length downstream — between the last node the gap was
        // open at and this one — and its foot that length on, at the sequent level.
        f64 u = 1.0;
        if (k - 1 > i) {
          u = before_gap > gap ? before_gap / (before_gap - gap) : 1.0;
          outs.pop_back();  // the node a lattice short of the head
        }
        Out head;
        head.s = round_to(s_k - (1.0 - u) * c.run, 1e3);
        head.level = before_level + u * (here - before_level);
        head.node = k;
        head.flags = scene_gen::k_channel_jump_head;
        head.keep = head.pair = true;
        const f64 sequent_level = before_seq + u * (seq - before_seq);
        const f64 length = std::max(c.run, k_jump_length_rises * (sequent_level - head.level));
        Out foot;
        foot.s = round_to(head.s + length, 1e3);
        foot.level = sequent_level;
        foot.flags = scene_gen::k_channel_jump_foot;
        foot.keep = true;
        const f64 s_b = c.run * static_cast<f64>(b);
        next = static_cast<u32>(std::ceil((foot.s + c.run) / c.run - 1e-9));
        if (foot.s > s_b - c.run || next > b) {
          // The jump runs to the stretch's end: its foot is the end's station.
          foot.s = s_b;
          foot.level = sub[b];
          foot.node = b;
          foot.flags |= node_flags(b, crit[b] != 0, true);
          foot.pair = lip;
          next = b + 1;
        } else {
          foot.node = static_cast<u32>(std::floor(foot.s / c.run + 0.5));
        }
        outs.push_back(head);
        outs.push_back(foot);
        ++stats.jumps;
        break;
      }
      if (!ended && tail_control && b > i && sub[b] - outs.back().level > k_choke_jump_m) {
        // The flow shot to a mouth or a confluence whose level stands over it: the jump stands
        // where the shooting flow still has the head for it, and the last station carries the
        // level the line meets.
        outs.pop_back();
        next = settle(b);
      }
      i = next;
    }
  }

  // **What the line ends in is what its last station carries**, to the millimetre: where the
  // flow arrives over that level (critical at a hanging lip, or shooting), the last station still
  // stands at it, and the surface comes down to it over the last lattice. A level under the bed
  // (a tributary hanging over its main stem) cannot be carried: the line ends as it arrived.
  bool tail_forced = false;
  if (line.tail_level && !outs.empty()) {
    Out& end = outs.back();
    if (end.node == n - 1 && (end.flags & tail_flags) != tail_flags) {
      if (line.level >= c.bed[n - 1] + k_station_depth_min_m) {
        if ((end.flags & scene_gen::k_channel_control) != 0 && stats.crests > 0) --stats.crests;
        tail_forced = true;
        end.level = line.level;
        end.flags = static_cast<u16>(
            (end.flags & (scene_gen::k_channel_jump_foot | scene_gen::k_channel_fall_foot)) |
            tail_flags);
        end.keep = true;
      } else {
        stats.tail_hung = true;
      }
    }
  }

  // ---- The stations -----------------------------------------------------------------------------
  // What a station is written with: the solved level, or the lower bank's crest where the level
  // stands over it (water is never written outside its bed).
  // Once tranquil water has spilled over a bank it stands no higher downstream than where it
  // spilled, until its solved level comes down under that: so the written level never climbs from
  // one bank's crest to the next one's.
  Vector<f64> put(outs.size(), 0.0);
  Vector<u8> held(outs.size(), 0);
  {
    f64 cap = 1e300;
    for (u32 k = 0; k < outs.size(); ++k) {
      const Out& o = outs[k];
      const bool tranquil = o.level >= c.critical[o.node] - 1e-9;
      if (!tranquil || (k > 0 && outs[k - 1].pair)) cap = 1e300;
      f64 level = o.level > c.bank[o.node] + k_over_bank_m ? c.bank[o.node] : o.level;
      if (tranquil && level > cap) level = cap;
      held[k] = level < o.level - k_over_bank_m ? 1 : 0;
      cap = level < o.level && tranquil ? level : 1e300;  // while it is lowered at all
      put[k] = std::max(level, c.bed[o.node] + k_station_depth_min_m);
    }
  }
  Vector<StationNode> nodes;
  for (u32 k = 0; k < outs.size(); ++k) {
    const Out& o = outs[k];
    const Wet w = wet_at(c, o.node, put[k]);
    StationNode s;
    s.s = o.s;
    s.level = put[k];
    s.wet_left = w.left;
    s.wet_right = w.right;
    s.keep = o.keep;
    s.pair = o.pair;
    nodes.push_back(s);
  }
  stats.nodes = n;
  stats.length_m = total;
  for (const u32 index : select_stations(nodes)) {
    const Out& o = outs[index];
    const f64 level = round_to(put[index], 1e3);
    // Where the level stands over a bank the level written is the bank's, and the speed, the
    // Froude number and the friction slope are the solved flow's (the discharge through the bed
    // with a wall at each crest), not the discharge forced through what the bed holds.
    const bool over_bank = held[index] != 0;
    const Flow f =
        solver.at(o.node, over_bank ? o.level : std::max(level, c.bed[o.node] + k_depth_min_m));
    const Wet w = wet_at(c, o.node, level);
    if (!f.ok) return refuse(error, line.name, "a station has no water in its section");
    ProfileStation st;
    st.s = o.s;
    const f64 at_node = std::clamp(o.s / c.run, 0.0, static_cast<f64>(n - 1));
    const u32 i0 = std::min<u32>(n - 1, static_cast<u32>(at_node));
    const u32 i1 = std::min<u32>(n - 1, i0 + 1);
    const f64 u = at_node - static_cast<f64>(i0);
    st.x = round_to(c.x[i0] + u * (c.x[i1] - c.x[i0]), 1e3);
    st.z = round_to(c.z[i0] + u * (c.z[i1] - c.z[i0]), 1e3);
    st.level = level;
    st.bed = c.bed[o.node];
    st.depth = level - st.bed;
    st.friction = f.friction;
    st.discharge = c.q[o.node];
    st.speed = f.speed;
    st.froude = f.froude;
    st.wet_left = w.left;
    st.wet_right = w.right;
    st.width =
        c.width[o.node] > 0.0
            ? c.width[o.node]
            : std::max(c.step, c.step * static_cast<f64>(c.left_end[o.node] - c.right_end[o.node]));
    st.manning_n = f.n;
    st.flags = o.flags;
    if (over_bank) {
      st.over_m = o.level - level;
      st.flags |= scene_gen::k_channel_over_bank;
      ++stats.over_bank;
      if (st.over_m > stats.over_bank_worst_m) {
        stats.over_bank_worst_m = st.over_m;
        stats.over_bank_at = WorldPos{st.x, level, st.z};
      }
    }
    out.stations.push_back(st);
  }
  stats.stations = out.stations.size();
  f64 n_sum = 0.0;
  for (u32 k = 0; k < out.stations.size(); ++k) {
    const ProfileStation& p = out.stations[k];
    stats.froude_max = std::max(stats.froude_max, p.froude);
    if ((p.flags & scene_gen::k_channel_fall_lip) != 0) ++stats.falls;
    if (k + 1 == out.stations.size()) break;
    const ProfileStation& q = out.stations[k + 1];
    n_sum += 0.5 * (p.manning_n + q.manning_n) * (q.s - p.s);
    if ((p.flags & scene_gen::k_channel_fall_lip) != 0) {
      stats.highest_fall_m = std::max(stats.highest_fall_m, p.level - q.level);
      continue;
    }
    const f64 head_p = p.level + p.speed * p.speed / (2.0 * k_gravity);
    const f64 head_q = q.level + q.speed * q.speed / (2.0 * k_gravity);
    const bool over = ((p.flags | q.flags) & scene_gen::k_channel_over_bank) != 0;
    // (Not at a last station brought down to the level its line meets: the water there has left
    // the line's own profile for the stem's, and its speed in the section is not its speed.)
    const bool met = tail_forced && k + 2 == out.stations.size();
    if (head_q > head_p + k_head_rise_m && !over && !met) {
      ++stats.head_rises;
      stats.head_rise_worst_m = std::max(stats.head_rise_worst_m, head_q - head_p);
    }
    const f64 rise = q.level - p.level;
    if (!(rise > k_rise_m)) continue;
    if ((p.flags & scene_gen::k_channel_jump_head) != 0) {
      ++stats.rises_jump;
      stats.rise_jump_worst_m = std::max(stats.rise_jump_worst_m, rise);
    } else if (over) {
      ++stats.rises_over_bank;
    } else if (p.froude >= 1.0 || q.froude >= 1.0) {
      ++stats.rises_shooting;
      if (rise > stats.rise_shooting_worst_m) {
        stats.rise_shooting_worst_m = rise;
        stats.rise_shooting_at = WorldPos{q.x, q.level, q.z};
      }
    } else {
      ++stats.rises_other;
      if (rise > stats.rise_other_worst_m) {
        stats.rise_other_worst_m = rise;
        stats.rise_other_at = WorldPos{q.x, q.level, q.z};
      }
    }
  }
  const f64 span = out.stations.empty() ? 0.0 : out.stations.back().s - out.stations[0].s;
  stats.manning_n_mean =
      span > 0.0 ? n_sum / span : (out.stations.empty() ? 0.0 : out.stations[0].manning_n);
  return true;
}

scene::WaterChannel describe_profile(const std::string& name, const Profile& profile, u64 seed) {
  scene::WaterChannel c;
  c.name = name;
  c.seed = seed;
  c.manning_n = static_cast<f32>(round_to(profile.stats.manning_n_mean, 1e4));
  for (const ProfileStation& s : profile.stations) {
    c.points.push_back(WorldPos{s.x, s.level, s.z});
    c.width_m.push_back(static_cast<f32>(std::max(1e-3, round_to(s.width, 1e3))));
    c.depth_m.push_back(static_cast<f32>(std::max(1e-3, round_to(s.depth, 1e4))));
    c.slope.push_back(static_cast<f32>(round_to(s.friction, 1e6)));
    c.discharge_m3s.push_back(static_cast<f32>(std::max(1e-6, round_to(s.discharge, 1e6))));
    c.speed_mps.push_back(static_cast<f32>(round_to(s.speed, 1e4)));
    c.froude.push_back(static_cast<f32>(round_to(s.froude, 1e4)));
    c.wet_left_m.push_back(static_cast<f32>(round_to(s.wet_left, 1e3)));
    c.wet_right_m.push_back(static_cast<f32>(round_to(s.wet_right, 1e3)));
    c.flags.push_back(s.flags);
  }
  return c;
}

}  // namespace engine::channels
