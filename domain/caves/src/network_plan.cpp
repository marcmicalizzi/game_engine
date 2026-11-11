// The plan of a cave network (network.h; docs/subsystems/caves.md, "The plan"): strands laid out in
// the host bed's coordinates, tried against the ground, joined into a graph, cut into cells.
//
// Arithmetic: f64 from integer hashes — products, sums, divisions, square roots, `det::sin` and
// `det::cos` — and every position rounded to a whole millimetre before anything reads it, so the
// plan is the same numbers on every toolchain (the solver common rules, 1).
#include "field.h"

#include <core/json/json.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/caves/network.h>

#include <algorithm>
#include <numbers>

namespace engine::caves {

using namespace detail;

const char* strand_name(StrandKind kind) noexcept {
  switch (kind) {
    case StrandKind::trunk: return "trunk";
    case StrandKind::feeder: return "feeder";
    case StrandKind::connector: return "connector";
    case StrandKind::bypass: return "bypass";
    case StrandKind::crawl: return "crawl";
    case StrandKind::lead: return "lead";
    case StrandKind::shaft: return "shaft";
    case StrandKind::fissure: return "fissure";
    case StrandKind::sump: return "sump";
  }
  return "?";
}

const char* end_name(EndKind kind) noexcept {
  switch (kind) {
    case EndKind::junction: return "junction";
    case EndKind::choke: return "choke";
    case EndKind::mouth: return "mouth";
    case EndKind::pinch: return "pinch";
    case EndKind::sump: return "sump";
    case EndKind::hall: return "hall";
  }
  return "?";
}

namespace {

constexpr f64 k_two_pi = 2.0 * std::numbers::pi;

struct Check {
  std::string& error;
  bool bad(const char* field, const char* range) {
    error = std::string("engine.caves.Network.") + field + " is out of range (" + range + ")";
    return false;
  }
  bool f(f64 v, f64 lo, f64 hi, const char* field, const char* range) {
    return (v >= lo && v <= hi) ? true : bad(field, range);
  }
  bool f(f32 v, f64 lo, f64 hi, const char* field, const char* range) {
    return f(static_cast<f64>(v), lo, hi, field, range);
  }
  bool u(u32 v, u32 lo, u32 hi, const char* field, const char* range) {
    return (v >= lo && v <= hi) ? true : bad(field, range);
  }
};

// The level beds a network with no formation is in: a metre of strong limestone, a weak parting,
// and so on, so the walls have ledges to show.
scene::Formation level_formation(const Network& net) {
  scene::Formation f;
  f.origin = net.origin;
  f.dip_deg = 0.0f;
  f.azimuth_deg = 0.0f;
  const f32 thick[4] = {0.9f, 0.3f, 0.6f, 0.25f};
  const f32 strong[4] = {0.85f, 0.15f, 0.7f, 0.2f};
  for (u32 i = 0; i < 4; ++i) {
    scene::FormationBed bed;
    bed.name = (i % 2 == 0) ? "limestone" : "parting";
    bed.thickness_m = thick[i];
    bed.resistance = strong[i];
    f.beds.push_back(bed);
  }
  return f;
}

// ---- the host's frame ---------------------------------------------------------------------------

struct Bed {
  gfx::FormationParams params;
  gfx::FormationFrame frame;
  V3 n, t, strike, down;  // the beds' normal, the dip's horizontal, the strike, down dip in the bed
  f64 sin_dip = 0.0, cos_dip = 1.0;
  bool flat = true;
  V3 q0;  // on the member's middle, under the origin (metres of the world)
  f64 c_mid = 0.0, c_base = 0.0, c_top = 0.0;

  f64 coordinate(V3 p) const { return gfx::formation_coordinate(frame, p.x, p.y, p.z).coordinate; }
  // The point u metres along strike, v down dip and dc across the beds from the member's middle.
  V3 at(f64 u, f64 v, f64 dc) const {
    V3 p = q0 + strike * u + down * v + n * dc;
    for (u32 k = 0; k < 4; ++k)
      p = p + n * (c_mid + dc - coordinate(p));
    return p;
  }
};

struct Raw {
  V3 p;
  f64 u = 0.0, v = 0.0, dc = 0.0;
  f64 a = 0.0, b = 0.0, hw = 0.0, top = 0.0, bot = 0.0;
  f64 floor_y = 0.0;
  SectionFrame frame = SectionFrame::Bed;
  bool scallops = false;
};

struct Strand {
  StrandKind kind = StrandKind::trunk;
  u8 level = 0;
  Vector<Raw> st;
  i32 host_a = -1, host_b = -1;
  u32 host_a_station = 0, host_b_station = 0;
  EndKind end_a = EndKind::choke, end_b = EndKind::choke;
  Vector<u32> used;    // stations something already joins, so the next keeps its distance
  u32 sump_until = 0;  // stations [0, sump_until] are the sump's
  bool led = false;    // a lead or a shaft: an opening the plan went looking for
};

struct Planner {
  const Network& net;
  const scene_gen::Ground& ground;
  Bed bed;
  f64 member = 0.0;  // the host's thickness
  f64 margin = 1.0;
  f64 water_y = 0.0;
  Vector<Strand> strands;
  std::string notes;

  f64 ground_y(V3 p) const {
    return ground.present()
               ? ground.surface_mm(scene_gen::nearest_mm(p.x), scene_gen::nearest_mm(p.z))
               : 0.0;
  }
  static f64 roof(const Raw& r) { return std::max(r.b + 0.3 * r.a, r.top); }
  f64 cover(const Raw& r) const { return ground_y(r.p) - (r.p.y + roof(r)); }
  // The passage is in the open: its middle stands over the ground.
  bool out(const Raw& r) const { return r.p.y - 0.4 * std::max(r.b, 0.5 * r.top) > ground_y(r.p); }
  f64 rnd(u64 a, u64 b = 0, u64 c = 0) const { return unit(hash3(net.seed, a, b, c)); }
  // The most a section may reach across the beds from `dc` and stay in the member.
  f64 room(f64 dc) const { return 0.5 * member - margin - std::fabs(dc); }
  // A tube's half height for its half width: twice as wide as high, but as round as 1.2 to 1
  // where that is what gives a walker headroom (docs/experiments/caves.md's range for a tube),
  // and never more than `most`, what the member has room for.
  f64 tube_b(f64 a, f64 most) const {
    const f64 need =
        (static_cast<f64>(net.walker_height_m) + 0.35 + static_cast<f64>(net.ledge_m)) /
        (2.0 * (1.0 - static_cast<f64>(net.fill)));
    return std::min(most, std::max(0.5 * a, std::min(a / 1.2, need)));
  }
  void note(const std::string& s) {
    if (!notes.empty()) notes += " ";
    notes += s;
  }

  // A tube's station at a bed point.
  Raw tube(f64 u, f64 v, f64 dc, f64 a, f64 b, bool scallops) const {
    Raw r;
    r.u = u;
    r.v = v;
    r.dc = dc;
    r.p = bed.at(u, v, dc);
    r.a = a;
    r.b = b;
    r.floor_y = r.p.y - b + static_cast<f64>(net.fill) * 2.0 * b;
    r.scallops = scallops;
    return r;
  }

  // Where a strand that ran `nominal` stations from its root ends against the ground: it keeps
  // what has `min_cover_m` over it; where the cover runs out it runs on into daylight if daylight
  // is within twenty stations (a mouth), else it stops there (a choke). Returns the count kept.
  u32 end_by_ground(const Vector<Raw>& cand, u32 nominal, EndKind otherwise, EndKind& end) const {
    const f64 least = static_cast<f64>(net.min_cover_m);
    u32 i = 0;
    while (i < nominal && i < cand.size() && cover(cand[i]) >= least)
      ++i;
    if (i >= nominal || i >= cand.size()) {
      end = otherwise;
      return std::min(nominal, cand.size());
    }
    for (u32 j = i; j < cand.size() && j < i + 20; ++j) {
      if (out(cand[j])) {
        end = EndKind::mouth;
        return std::min(j + 3, cand.size());
      }
    }
    end = EndKind::choke;
    return i;
  }

  // A station of `strand` between two fractions of its length that nothing joins within six
  // stations of; false when there is none.
  bool pick(Strand& strand, f64 lo, f64 hi, u64 salt, u32& out_station) const {
    const u32 n = strand.st.size();
    if (n < 16) return false;
    const u32 first = std::max<u32>(6, static_cast<u32>(lo * n));
    const u32 last = std::min<u32>(n - 7, static_cast<u32>(hi * n));
    if (last <= first) return false;
    const u32 start = first + static_cast<u32>(rnd(salt, 0x91C4u) * (last - first));
    for (u32 k = 0; k <= last - first; ++k) {
      const u32 j = first + (start - first + k) % (last - first + 1);
      bool clear = j > strand.sump_until + 4;
      for (const u32 other : strand.used)
        if ((j > other ? j - other : other - j) < 6) clear = false;
      if (clear) {
        strand.used.push_back(j);
        out_station = j;
        return true;
      }
    }
    return false;
  }
};

// ---- the strands --------------------------------------------------------------------------------

bool lay_trunks(Planner& pl, std::string& error) {
  const Network& net = pl.net;
  const Bed& bed = pl.bed;
  const f64 reach = static_cast<f64>(net.reach_strike_m);
  const f64 spacing = static_cast<f64>(net.level_spacing_m);
  const f64 fill = static_cast<f64>(net.fill);
  u32 placed = 0;
  f64 flat_step = 0.0;
  for (u32 level = 0; level < net.levels; ++level) {
    const u64 salt = 0x7A00u + level;
    const f64 width = static_cast<f64>(net.trunk_width_m);
    f64 a_max = 0.5 * width;
    // The most the member lets a tube's half height be.
    const f64 b_room = 0.5 * pl.member - pl.margin - 0.4;
    if (b_room < 0.6) {
      error = "the member is too thin for a passage: " + std::to_string(pl.member) +
              " m across the beds with a margin of " + std::to_string(pl.margin) + " m";
      return false;
    }
    a_max = std::min(a_max, 3.5 * b_room);
    const f64 b_max = pl.tube_b(a_max, b_room);
    const f64 loop_amp = std::min(1.2, std::max(0.0, pl.room(0.0) - b_max));
    // The level's height: its floor a little over the water (the lowest), then a spacing each.
    const f64 level_y = pl.water_y + 0.3 + b_max * (1.0 - 2.0 * fill) + spacing * level;
    f64 v_level = 0.0, dc_level = 0.0;
    if (!bed.flat) {
      // Where the member's middle is at the level, along the dip line through the origin.
      dc_level = (pl.rnd(salt, 1) - 0.5) * 0.6 * std::max(0.0, pl.room(0.0) - b_max - loop_amp);
      v_level = (bed.q0.y - level_y) / bed.sin_dip;
      for (u32 k = 0; k < 4; ++k)
        v_level += (bed.at(0.0, v_level, dc_level).y - level_y) / bed.sin_dip;
      if (std::fabs(v_level) > 5000.0) {
        pl.note("Level " + std::to_string(level) + " lies " + std::to_string(v_level) +
                " m down dip of the origin: left out.");
        continue;
      }
    } else {
      v_level = flat_step;
      flat_step -= 2.0 * a_max + 4.0;
      dc_level = (level_y - bed.at(0.0, v_level, 0.0).y) / bed.cos_dip;
      if (std::fabs(dc_level) + b_max > 0.5 * pl.member - pl.margin) {
        pl.note("Level " + std::to_string(level) + " at " + std::to_string(level_y) +
                " m is outside the member under the origin: left out.");
        continue;
      }
    }
    const f64 amp_c = std::min(loop_amp, std::max(0.0, pl.room(dc_level) - b_max));
    // The meander: about eleven widths long (docs/experiments/cave-networks.md, "Sinuosity").
    const f64 wave = 11.0 * width * (0.85 + 0.3 * pl.rnd(salt, 2));
    const f64 amp = wave * (0.08 + 0.07 * pl.rnd(salt, 3));
    const f64 phase = k_two_pi * pl.rnd(salt, 4), phase2 = k_two_pi * pl.rnd(salt, 5);
    const f64 wave_c = 70.0 + 40.0 * pl.rnd(salt, 6), phase_c = k_two_pi * pl.rnd(salt, 7);
    const u32 n = static_cast<u32>(reach);
    const u32 pad = 20;
    const auto bed_point = [&](f64 u, f64& v, f64& dc) {
      v = v_level + amp * det::sin(k_two_pi * u / wave + phase) +
          0.3 * amp * det::sin(k_two_pi * u / (0.37 * wave) + phase2);
      dc = dc_level + amp_c * det::sin(k_two_pi * u / wave_c + phase_c);
    };
    // Downstream is the end whose ground is lower: the valley's side.
    f64 v_end = 0.0, dc_end = 0.0;
    bed_point(-0.5 * reach, v_end, dc_end);
    const f64 ground_lo = pl.ground_y(bed.at(-0.5 * reach, v_end, dc_end));
    bed_point(0.5 * reach, v_end, dc_end);
    const f64 ground_hi = pl.ground_y(bed.at(0.5 * reach, v_end, dc_end));
    const f64 heading = ground_hi <= ground_lo ? 1.0 : -1.0;
    Vector<Raw> cand;
    for (u32 j = 0; j <= n + 2 * pad; ++j) {
      const f64 along = static_cast<f64>(j) - static_cast<f64>(pad);  // 0 .. n is the reach
      const f64 u = heading * (along - 0.5 * reach);
      f64 v = 0.0, dc = 0.0;
      bed_point(u, v, dc);
      // The area grows downstream with what the feeders bring: width as the root of the distance.
      const f64 grow = std::sqrt(0.5 + 0.5 * clamp01(along / reach));
      const f64 vary = 1.0 + 0.06 * det::sin(k_two_pi * along / 23.0 + phase2);
      const f64 a = a_max * grow * vary;
      cand.push_back(pl.tube(u, v, dc, a, pl.tube_b(a, b_room), true));
    }
    // The longest run under cover inside the reach.
    const f64 least = static_cast<f64>(net.min_cover_m);
    u32 best_a = 0, best_n = 0, run_a = 0, run_n = 0;
    for (u32 j = pad; j <= n + pad; ++j) {
      if (pl.cover(cand[j]) >= least) {
        if (run_n == 0) run_a = j;
        ++run_n;
        if (run_n > best_n) {
          best_n = run_n;
          best_a = run_a;
        }
      } else {
        run_n = 0;
      }
    }
    if (best_n < 24) {
      pl.note("Level " + std::to_string(level) + " has " + std::to_string(best_n) +
              " m under cover: left out.");
      continue;
    }
    u32 ja = best_a, jb = best_a + best_n - 1;
    Strand trunk;
    trunk.kind = StrandKind::trunk;
    trunk.level = static_cast<u8>(level);
    trunk.end_a = EndKind::choke;
    trunk.end_b = EndKind::choke;
    // Each end the ground cut short runs on into daylight if daylight is near.
    if (jb < n + pad) {
      for (u32 j = jb + 1; j < cand.size() && j <= jb + 20; ++j)
        if (pl.out(cand[j])) {
          jb = std::min(j + 3, cand.size() - 1);
          trunk.end_b = EndKind::mouth;
          break;
        }
    }
    if (ja > pad) {
      for (u32 k = 1; k <= 20 && k <= ja; ++k)
        if (pl.out(cand[ja - k])) {
          ja = ja - k >= 3 ? ja - k - 3 : 0;
          trunk.end_a = EndKind::mouth;
          break;
        }
    }
    for (u32 j = ja; j <= jb; ++j)
      trunk.st.push_back(cand[j]);
    // The lowest trunk's head goes under: down across the beds where the member has room, else
    // down dip (docs/experiments/cave-networks.md, "Sumps").
    if (level == 0 && trunk.end_a == EndKind::choke && trunk.st.size() > 40) {
      const u32 len = 14;
      const f64 depth = 4.0;
      for (u32 j = 0; j <= len; ++j) {
        Raw& r = trunk.st[j];
        const f64 w = smooth01(static_cast<f64>(len - j) / static_cast<f64>(len));
        const f64 down_dc =
            std::min(depth, std::max(0.0, r.dc + 0.5 * pl.member - pl.margin - r.b));
        f64 v = r.v, dc = r.dc - down_dc * w;
        if (!bed.flat && down_dc < depth) v += (depth - down_dc) * w / bed.sin_dip;
        const bool scallops = r.scallops;
        r = pl.tube(r.u, v, dc, r.a, r.b, scallops);
      }
      trunk.sump_until = len;
      trunk.end_a = EndKind::sump;
    }
    pl.strands.push_back(std::move(trunk));
    ++placed;
  }
  if (placed == 0) {
    error = "no level of the network lies in the member under cover: " + pl.notes;
    return false;
  }
  return true;
}

// The canyon a feeder is: three times as tall as wide (docs/experiments/caves.md), cut down as far
// as the member's base lets it, a keyhole's tube over it for the first third.
void feeder_section(const Planner& pl, Raw& r, f64 hw, f64 tube_a, f64 share) {
  const f64 depth_room = (r.dc + 0.5 * pl.member - pl.margin) / pl.bed.cos_dip;
  const f64 tall = std::min(6.0 * hw, std::max(1.2, depth_room + 0.35 * 6.0 * hw));
  r.frame = SectionFrame::Bed;
  r.hw = hw;
  r.top = 0.35 * tall;
  r.bot = -std::min(0.65 * tall, std::max(0.6, depth_room));
  r.a = tube_a * share;
  r.b = 0.5 * r.a;
  if (r.a < 0.1) {
    r.a = r.b = 0.0;
    r.frame = SectionFrame::Plumb;
  }
  r.floor_y = r.p.y + r.bot + 0.15;
  r.scallops = true;
}

void lay_feeders(Planner& pl) {
  const Network& net = pl.net;
  const u32 trunks = pl.strands.size();
  for (u32 ti = 0; ti < trunks; ++ti) {
    const u32 len = pl.strands[ti].st.size();
    const u32 count = static_cast<u32>(static_cast<f64>(net.feeders_per_100m) * len / 100.0 + 0.5);
    for (u32 k = 0; k < count; ++k) {
      const u64 salt = 0xFEED00u + ti * 256u + k;
      u32 j = 0;
      if (!pl.pick(pl.strands[ti], (k + 0.1) / count, (k + 0.9) / count, salt, j)) continue;
      const Raw root = pl.strands[ti].st[j];
      const f64 side = pl.bed.flat ? ((k % 2 == 0) ? 1.0 : -1.0) : 1.0;
      const f64 theta = (pl.rnd(salt, 1) - 0.5) * 0.9;
      const f64 du = det::sin(theta), dv = -side * det::cos(theta);
      f64 reach = static_cast<f64>(net.reach_dip_m) * (0.35 + 0.65 * pl.rnd(salt, 2));
      // A feeder of a lower trunk stops short of the trunk above it: the connectors join levels.
      if (!pl.bed.flat && ti + 1 < trunks)
        reach = std::min(reach, 0.75 * std::fabs(pl.strands[ti + 1].st[0].v - root.v));
      const f64 wave = 18.0 + 12.0 * pl.rnd(salt, 3), amp = 1.5 + 1.5 * pl.rnd(salt, 4);
      const f64 phase = k_two_pi * pl.rnd(salt, 5);
      const f64 hw = 0.6 + 0.25 * pl.rnd(salt, 6);
      const f64 tube_a = 1.0 + 0.4 * pl.rnd(salt, 7);
      const u32 nominal = static_cast<u32>(reach);
      // A feeder is graded to its trunk: its canyon's floor meets the trunk's floor at the
      // junction, so its centreline stands that much higher across the beds, and climbs a
      // little more than the bed as it goes up dip.
      const f64 tall = 6.0 * hw;
      const f64 lift = std::max(0.0, (root.floor_y - root.p.y + 0.65 * tall) * pl.bed.cos_dip);
      Vector<Raw> cand;
      for (u32 l = 0; l <= nominal + 20; ++l) {
        const f64 d = static_cast<f64>(l);
        const f64 wig = amp * (det::sin(k_two_pi * d / wave + phase) - det::sin(phase)) *
                        std::min(1.0, d / 6.0);
        Raw r;
        r.u = root.u + d * du + wig * (-dv);
        r.v = root.v + d * dv + wig * du;
        r.dc =
            std::min(root.dc + lift + 0.015 * d, 0.5 * pl.member - pl.margin - 0.35 * tall - 0.2);
        r.p = pl.bed.at(r.u, r.v, r.dc);
        const f64 third = static_cast<f64>(nominal) / 3.0;
        feeder_section(pl, r, hw, tube_a, clamp01((third - d) / 4.0 + 1.0));
        cand.push_back(r);
      }
      Strand s;
      s.kind = StrandKind::feeder;
      s.level = pl.strands[ti].level;
      s.host_a = static_cast<i32>(ti);
      s.host_a_station = j;
      s.end_a = EndKind::junction;
      const u32 keep = pl.end_by_ground(cand, nominal, EndKind::choke, s.end_b);
      if (keep < 8) continue;
      for (u32 l = 0; l < keep; ++l)
        s.st.push_back(cand[l]);
      pl.strands.push_back(std::move(s));
    }
  }
}

void lay_connectors(Planner& pl) {
  u32 trunk_of[8];
  u32 trunks = 0;
  for (u32 i = 0; i < pl.strands.size() && trunks < 8; ++i)
    if (pl.strands[i].kind == StrandKind::trunk) trunk_of[trunks++] = i;
  for (u32 t = 0; t + 1 < trunks; ++t) {
    const u32 lower = trunk_of[t], upper = trunk_of[t + 1];
    for (u32 k = 0; k < 2; ++k) {
      const u64 salt = 0xC0AA00u + t * 16u + k;
      u32 ja = 0;
      const f64 at = 0.25 + 0.5 * k;
      if (!pl.pick(pl.strands[upper], at - 0.1, at + 0.1, salt, ja)) continue;
      const Raw from = pl.strands[upper].st[ja];
      // The lower trunk's station across from it, moved along strike until the way down is walked.
      const f64 drop = from.p.y - pl.strands[lower].st[pl.strands[lower].st.size() / 2].p.y;
      const f64 dv = pl.strands[lower].st[pl.strands[lower].st.size() / 2].v - from.v;
      const f64 run = std::fabs(drop) / 0.5;
      const f64 along = run > std::fabs(dv) ? std::sqrt(run * run - dv * dv) : 0.0;
      const f64 drift = (pl.rnd(salt, 1) - 0.5) * 12.0;
      const f64 target_u = from.u + drift + (k == 0 ? 1.0 : -1.0) * along;
      u32 jb = 0;
      f64 nearest = 1.0e30;
      const Strand& low = pl.strands[lower];
      for (u32 j = low.sump_until + 6; j + 6 < low.st.size(); ++j) {
        bool clear = true;
        for (const u32 other : low.used)
          if ((j > other ? j - other : other - j) < 6) clear = false;
        const f64 d = std::fabs(low.st[j].u - target_u);
        if (clear && d < nearest) {
          nearest = d;
          jb = j;
        }
      }
      if (nearest > 30.0) continue;
      const Raw to = low.st[jb];
      const f64 chord = length(to.p - from.p);
      const u32 n = std::max<u32>(8, static_cast<u32>(chord * 1.05));
      const f64 hw = 0.6 + 0.15 * pl.rnd(salt, 2);
      const f64 tube_a = 1.2 + 0.3 * pl.rnd(salt, 3);
      const f64 bow = (pl.rnd(salt, 4) - 0.5) * 0.2 * chord;
      // Graded at both ends: the canyon's floor leaves the upper trunk's floor and arrives at
      // the lower one's.
      const f64 cut = 1.2 * tube_a + 0.6;
      const f64 lift_a = std::max(0.0, (from.floor_y - from.p.y + cut) * pl.bed.cos_dip);
      const f64 lift_b = std::max(0.0, (to.floor_y - to.p.y + cut) * pl.bed.cos_dip);
      Strand s;
      s.kind = StrandKind::connector;
      s.level = pl.strands[lower].level;
      s.host_a = static_cast<i32>(upper);
      s.host_a_station = ja;
      s.host_b = static_cast<i32>(lower);
      s.host_b_station = jb;
      s.end_a = s.end_b = EndKind::junction;
      bool covered = true;
      for (u32 l = 0; l <= n; ++l) {
        const f64 e = static_cast<f64>(l) / static_cast<f64>(n);
        const f64 arch = det::sin(std::numbers::pi * e);
        Raw r;
        r.u = lerp(from.u, to.u, e) + bow * arch;
        r.v = lerp(from.v, to.v, e);
        r.dc = std::min(lerp(from.dc + lift_a, to.dc + lift_b, smooth01(e)),
                        0.5 * pl.member - pl.margin - 0.5 * tube_a);
        r.p = pl.bed.at(r.u, r.v, r.dc);
        r.frame = SectionFrame::Bed;
        r.a = tube_a;
        r.b = std::min(0.5 * tube_a, std::max(0.3, pl.room(r.dc)));
        r.hw = hw;
        r.top = 0.0;
        const f64 depth_room = (r.dc + 0.5 * pl.member - pl.margin) / pl.bed.cos_dip;
        r.bot = -std::min(1.2 * 2.0 * r.b + 0.6, std::max(0.8, depth_room));
        r.floor_y = r.p.y + r.bot + 0.15;
        r.scallops = true;
        if (l > 2 && l + 2 < n && pl.cover(r) < static_cast<f64>(pl.net.min_cover_m))
          covered = false;
        s.st.push_back(r);
      }
      if (!covered) {
        pl.note("A connector between levels came within the cover: left out.");
        continue;
      }
      pl.strands[lower].used.push_back(jb);
      pl.strands.push_back(std::move(s));
    }
  }
}

void lay_bypasses(Planner& pl) {
  for (u32 k = 0; k < pl.net.bypasses; ++k) {
    const u64 salt = 0xB1A500u + k;
    Strand& trunk = pl.strands[0];
    u32 ja = 0;
    const f64 at = (k + 0.3) / static_cast<f64>(pl.net.bypasses + 1);
    if (!pl.pick(trunk, at, at + 0.25, salt, ja)) continue;
    const u32 span = 26 + static_cast<u32>(14.0 * pl.rnd(salt, 1));
    const u32 jb = ja + span;
    if (jb + 6 >= trunk.st.size()) continue;
    bool clear = true;
    for (const u32 other : trunk.used)
      if (other != ja && (jb > other ? jb - other : other - jb) < 6) clear = false;
    if (!clear) continue;
    const f64 side = pl.rnd(salt, 2) < 0.5 ? -1.0 : 1.0;
    const f64 offset = 7.0 + 3.0 * pl.rnd(salt, 3);
    Strand s;
    s.kind = StrandKind::bypass;
    s.level = trunk.level;
    s.host_a = 0;
    s.host_a_station = ja;
    s.host_b = 0;
    s.host_b_station = jb;
    s.end_a = s.end_b = EndKind::junction;
    bool covered = true;
    for (u32 l = 0; l <= span; ++l) {
      const Raw& base = trunk.st[ja + l];
      const f64 arch = det::sin(std::numbers::pi * static_cast<f64>(l) / static_cast<f64>(span));
      const f64 a = 0.7 * base.a;
      const f64 b = pl.tube_b(a, base.b);
      Raw r = pl.tube(base.u, base.v + side * offset * arch * arch,
                      std::min(base.dc + 0.4 * arch, 0.5 * pl.member - pl.margin - b), a, b, true);
      if (pl.cover(r) < static_cast<f64>(pl.net.min_cover_m)) covered = false;
      s.st.push_back(r);
    }
    if (!covered) continue;
    trunk.used.push_back(jb);
    pl.strands.push_back(std::move(s));
  }
}

void lay_crawls(Planner& pl) {
  u32 trunks = 0;
  for (const Strand& s : pl.strands)
    if (s.kind == StrandKind::trunk) ++trunks;
  for (u32 k = 0; k < pl.net.crawls; ++k) {
    const u64 salt = 0xC4A700u + k;
    const u32 ti = k % trunks;
    u32 j = 0;
    if (!pl.pick(pl.strands[ti], 0.15, 0.85, salt, j)) continue;
    const Raw root = pl.strands[ti].st[j];
    const f64 side = pl.rnd(salt, 1) < 0.5 ? -1.0 : 1.0;
    const f64 theta = (pl.rnd(salt, 2) - 0.5) * 1.0;
    const f64 du = det::sin(theta), dv = side * det::cos(theta);
    const u32 nominal = 12 + static_cast<u32>(13.0 * pl.rnd(salt, 3));
    Vector<Raw> cand;
    for (u32 l = 0; l <= nominal; ++l) {
      const f64 d = static_cast<f64>(l), e = d / static_cast<f64>(nominal);
      const f64 a = lerp(0.8, 0.3, e), b = lerp(0.4, 0.15, e);
      // A bedding-plane crawl: it leaves high on the trunk's wall.
      const f64 dc = std::min(root.dc + std::min(1.0, d / 3.0) * (root.b - 0.5),
                              0.5 * pl.member - pl.margin - b);
      Raw r = pl.tube(root.u + d * du, root.v + d * dv, dc, a, b, true);
      r.floor_y = r.p.y - b + 0.1;
      cand.push_back(r);
    }
    Strand s;
    s.kind = StrandKind::crawl;
    s.level = pl.strands[ti].level;
    s.host_a = static_cast<i32>(ti);
    s.host_a_station = j;
    s.end_a = EndKind::junction;
    const u32 keep = pl.end_by_ground(cand, nominal + 1, EndKind::pinch, s.end_b);
    if (keep < 6) continue;
    for (u32 l = 0; l < keep; ++l)
      s.st.push_back(cand[l]);
    pl.strands.push_back(std::move(s));
  }
}

// A shaft's stations from a point up to a metre and a half over the ground.
void shaft_stations(const Planner& pl, V3 from, f64 a, f64 b, Vector<Raw>& out) {
  const f64 top = pl.ground_y(from) + 1.5;
  const u32 n = static_cast<u32>(std::max(2.0, top - from.y + 0.999));
  for (u32 l = 0; l <= n; ++l) {
    Raw r;
    r.p = {from.x, lerp(from.y, top, static_cast<f64>(l) / static_cast<f64>(n)), from.z};
    r.a = a;
    r.b = b;
    r.frame = SectionFrame::Shaft;
    r.floor_y = from.y - 100.0;
    out.push_back(r);
  }
}

void lay_halls(Planner& pl, Vector<PlanHall>& halls) {
  u32 trunk_of[8];
  u32 trunks = 0;
  for (u32 i = 0; i < pl.strands.size() && trunks < 8; ++i)
    if (pl.strands[i].kind == StrandKind::trunk) trunk_of[trunks++] = i;
  for (u32 k = 0; k < pl.net.halls; ++k) {
    const u64 salt = 0x4A1100u + k;
    const u32 ti = trunk_of[(trunks - 1) - (k % trunks)];
    u32 j = 0;
    if (!pl.pick(pl.strands[ti], 0.3, 0.7, salt, j)) continue;
    const Raw at = pl.strands[ti].st[j];
    const Raw next = pl.strands[ti].st[j + 1];
    const V3 along = normalized(V3{next.p.x - at.p.x, 0.0, next.p.z - at.p.z}, V3{1.0, 0.0, 0.0});
    PlanHall h;
    // 13 to 18 m across the short way for a 6 m trunk: inside what a roof of one bed spans
    // (docs/experiments/cave-networks.md, "The beam").
    const f64 full = 0.5 * static_cast<f64>(pl.net.trunk_width_m);
    const f64 rb = (2.2 + 0.8 * pl.rnd(salt, 2)) * full;
    const f64 ra = (1.4 + 0.4 * pl.rnd(salt, 1)) * rb;
    const f64 rh = 0.6 * rb;
    const f64 floor_y = at.floor_y - 0.4;
    h.hall.x = scene_gen::nearest_mm(at.p.x);
    h.hall.y = scene_gen::nearest_mm(floor_y);
    h.hall.z = scene_gen::nearest_mm(at.p.z);
    h.hall.ra = static_cast<i32>(scene_gen::nearest_mm(ra));
    h.hall.rb = static_cast<i32>(scene_gen::nearest_mm(rb));
    h.hall.rh = static_cast<i32>(scene_gen::nearest_mm(rh));
    h.hall.axis_x = static_cast<i32>(std::floor(along.x * 1048576.0 + 0.5));
    h.hall.axis_z = static_cast<i32>(std::floor(along.z * 1048576.0 + 0.5));
    h.hall.floor = h.hall.y;
    h.hall.pile = static_cast<i32>(scene_gen::nearest_mm(0.3 * rh));
    h.hall.seed = hash3(pl.net.seed, salt, 9);
    const V3 centre{at.p.x, floor_y, at.p.z};
    const f64 cover = pl.ground_y(centre) - (floor_y + rh);
    h.cover_m = static_cast<f32>(cover);
    h.span_m = static_cast<f32>(2.0 * rb);
    // A roof with less rock over it than a third of its span does not stand (Hatzor, Wainshtein
    // and Bakun 2010): it has stoped through, and the gap is an opening nobody declared.
    h.fallen = cover < static_cast<f64>(pl.net.collapse_cover) * 2.0 * rb;
    if (h.fallen) {
      Strand s;
      s.kind = StrandKind::fissure;
      s.level = pl.strands[ti].level;
      s.end_a = EndKind::hall;
      s.end_b = EndKind::mouth;
      const f64 off = (pl.rnd(salt, 3) < 0.5 ? -0.3 : 0.3) * ra;
      const V3 foot{centre.x + along.x * off, floor_y + rh - 1.5, centre.z + along.z * off};
      shaft_stations(pl, foot, std::min(3.0, std::max(1.2, 0.3 * rb)), 0.5 + 0.3 * pl.rnd(salt, 4),
                     s.st);
      pl.strands.push_back(std::move(s));
    }
    halls.push_back(h);
  }
}

// A lead: from a strand's blind end along the bed to the nearest daylight, or up a shaft.
void lay_leads(Planner& pl) {
  struct End {
    u32 strand;
    f64 order;
  };
  Vector<End> ends;
  for (u32 i = 0; i < pl.strands.size(); ++i) {
    const Strand& s = pl.strands[i];
    if (s.end_b != EndKind::choke || s.st.size() < 4) continue;
    f64 order = 0.0;
    if (s.kind == StrandKind::trunk) {
      order = static_cast<f64>(s.level);  // a trunk's downstream end first, the lowest first
    } else if (s.kind == StrandKind::feeder) {
      order = 100.0 + pl.cover(s.st.back());  // then the feeders' heads, the shallowest first
    } else {
      continue;
    }
    ends.push_back(End{i, order});
  }
  std::stable_sort(ends.begin(), ends.end(),
                   [](const End& x, const End& y) { return x.order < y.order; });
  u32 led = 0;
  for (const End& e : ends) {
    if (led >= pl.net.openings) break;
    const Strand& host = pl.strands[e.strand];
    const Raw tip = host.st.back();
    const Raw before = host.st[host.st.size() - 3];
    const V3 heading = normalized(V3{tip.u - before.u, 0.0, tip.v - before.v}, V3{1.0, 0.0, 0.0});
    // A lead is a way in: wide and high enough to walk where the member has the room.
    const f64 a = std::max(1.8, 0.8 * tip.a);
    const f64 b = pl.tube_b(a, std::max(0.5, pl.room(tip.dc)));
    const u32 reach = static_cast<u32>(pl.net.lead_reach_m);
    u32 best_l = 0;
    f64 best_du = 0.0, best_dv = 0.0;
    for (u32 k = 0; k < 16; ++k) {
      const f64 angle = k_two_pi * static_cast<f64>(k) / 16.0;
      const f64 du = det::cos(angle), dv = det::sin(angle);
      if (du * heading.x + dv * heading.z < -0.2) continue;
      for (u32 l = 1; l <= reach; ++l) {
        if (best_l != 0 && l >= best_l) break;
        const Raw r = pl.tube(tip.u + l * du, tip.v + l * dv, tip.dc, a, b, true);
        if (pl.out(r)) {
          best_l = l;
          best_du = du;
          best_dv = dv;
          break;
        }
      }
    }
    Strand s;
    s.level = host.level;
    s.host_a = static_cast<i32>(e.strand);
    s.host_a_station = host.st.size() - 1;
    s.end_a = EndKind::junction;
    s.end_b = EndKind::mouth;
    s.led = true;
    if (best_l != 0) {
      s.kind = StrandKind::lead;
      for (u32 l = 0; l <= best_l + 3; ++l) {
        // The first stations ease from the host's section into the lead's.
        const f64 e01 = clamp01(static_cast<f64>(l) / 4.0);
        Raw r = pl.tube(tip.u + l * best_du, tip.v + l * best_dv, tip.dc, a, b, true);
        if (l == 0) r = tip;
        r.hw = lerp(tip.hw, 0.0, e01);
        r.top = tip.top;
        r.bot = tip.bot;
        if (r.hw < 0.06) r.hw = r.top = r.bot = 0.0;
        if (l > 0) r.floor_y = lerp(tip.floor_y, r.floor_y, e01);
        s.st.push_back(r);
      }
    } else if (pl.cover(tip) <= static_cast<f64>(pl.net.max_shaft_m)) {
      s.kind = StrandKind::shaft;
      shaft_stations(pl, V3{tip.p.x, tip.p.y + 0.5, tip.p.z}, 1.5, 1.1, s.st);
    } else {
      continue;
    }
    pl.strands[e.strand].end_b = EndKind::junction;
    pl.strands.push_back(std::move(s));
    ++led;
  }
}

// ---- from strands to the plan -------------------------------------------------------------------

i32 q20(f64 v) noexcept { return static_cast<i32>(std::floor(v * 1048576.0 + 0.5)); }
i32 to_mm32(f64 v) noexcept { return static_cast<i32>(scene_gen::nearest_mm(v)); }

// What a walker's capsule has at a station: the height from the floor to the roof, and the width
// at the height of the capsule's lower and upper spheres, the narrower of the two.
void clearance(const Plan& plan, PlanStation& ps) {
  const CellStation& s = ps.station;
  const f64 radius = static_cast<f64>(plan.network.walker_radius_m);
  const f64 tall = static_cast<f64>(plan.network.walker_height_m);
  const f64 a = s.a / 1000.0, b = s.b / 1000.0, hw = s.slot_hw / 1000.0;
  const f64 top = s.slot_top / 1000.0, bot = s.slot_bot / 1000.0;
  const f64 floor_rel = static_cast<f64>(s.floor - s.y) / 1000.0;
  const f64 roof_rel = std::max(b, hw > 0.0 ? top : -1.0e9);
  const auto half_width = [&](f64 y) {
    f64 w = 0.0;
    if (a > 0.0 && std::fabs(y) < b) w = a * std::sqrt(1.0 - (y / b) * (y / b));
    if (hw > 0.0 && y >= bot && y <= top) w = std::max(w, hw);
    return w;
  };
  const f64 height = roof_rel - floor_rel;
  const f64 width =
      2.0 * std::min(half_width(floor_rel + radius), half_width(floor_rel + tall - radius));
  ps.clear_h_mm = to_mm32(height);
  ps.clear_w_mm = to_mm32(width);
  const bool dry = !plan.has_water || s.floor >= plan.water_mm - 400;
  // The walls' detail is not in the section: a resistant bed stands a ledge's depth into the
  // passage on either side and under the roof, so the capsule is given that much more.
  const f64 ledge = static_cast<f64>(plan.network.ledge_m);
  ps.passable = s.frame != SectionFrame::Shaft && dry && height >= tall + 0.25 + ledge &&
                width >= 2.0 * radius + 0.2 + 2.0 * ledge;
}

struct NodeKey {
  u32 strand, station, node;
};

void build_graph(Planner& pl, Plan& plan) {
  const Vector<Strand>& strands = pl.strands;
  Vector<NodeKey> keys;
  const auto node_at = [&](u32 strand, u32 station, EndKind end) -> u32 {
    for (const NodeKey& k : keys)
      if (k.strand == strand && k.station == station) return k.node;
    const Raw& r = strands[strand].st[station];
    PlanNode n;
    n.x = scene_gen::nearest_mm(r.p.x);
    n.y = scene_gen::nearest_mm(r.p.y);
    n.z = scene_gen::nearest_mm(r.p.z);
    n.end = end;
    plan.nodes.push_back(n);
    keys.push_back(NodeKey{strand, station, plan.nodes.size() - 1});
    return plan.nodes.size() - 1;
  };
  // Where each strand is split: its ends, what joins it, the sump's end.
  Vector<Vector<u32>> splits;
  splits.resize(strands.size());
  for (u32 i = 0; i < strands.size(); ++i) {
    splits[i].push_back(0);
    splits[i].push_back(strands[i].st.size() - 1);
    if (strands[i].sump_until != 0) splits[i].push_back(strands[i].sump_until);
  }
  for (u32 i = 0; i < strands.size(); ++i) {
    if (strands[i].host_a >= 0)
      splits[static_cast<u32>(strands[i].host_a)].push_back(strands[i].host_a_station);
    if (strands[i].host_b >= 0)
      splits[static_cast<u32>(strands[i].host_b)].push_back(strands[i].host_b_station);
  }
  for (u32 i = 0; i < strands.size(); ++i) {
    std::sort(splits[i].begin(), splits[i].end());
    Vector<u32> unique;
    for (const u32 s : splits[i])
      if (unique.empty() || unique.back() != s) unique.push_back(s);
    splits[i] = unique;
  }
  for (u32 i = 0; i < strands.size(); ++i) {
    const Strand& strand = strands[i];
    const u32 last = strand.st.size() - 1;
    // Whole millimetres first; the tangents are of those.
    Vector<CellStation> whole;
    i64 along_mm = 0;
    for (u32 j = 0; j <= last; ++j) {
      const Raw& r = strand.st[j];
      CellStation c;
      c.x = scene_gen::nearest_mm(r.p.x);
      c.y = scene_gen::nearest_mm(r.p.y);
      c.z = scene_gen::nearest_mm(r.p.z);
      if (j > 0) {
        const CellStation& p = whole[j - 1];
        const V3 d{static_cast<f64>(c.x - p.x), static_cast<f64>(c.y - p.y),
                   static_cast<f64>(c.z - p.z)};
        along_mm += scene_gen::nearest_mm(length(d) / 1000.0);
      }
      c.s = along_mm;
      // No section is narrower than 2.2 lattice spacings: what is smaller the surface cannot
      // hold, and a crawl pinches out at that instead of at nothing.
      const i32 least = static_cast<i32>(plan.voxel_mm * 11 / 10);
      c.a = r.a > 0.0 ? std::max(to_mm32(r.a), least) : 0;
      c.b = r.b > 0.0 ? std::max(to_mm32(r.b), least) : 0;
      c.slot_hw = r.hw > 0.0 ? std::max(to_mm32(r.hw), least) : 0;
      c.slot_top = to_mm32(r.top);
      c.slot_bot = to_mm32(r.bot);
      c.floor = scene_gen::nearest_mm(r.floor_y);
      c.frame = r.frame;
      c.flags = r.scallops ? 1u : 0u;
      whole.push_back(c);
    }
    for (u32 j = 0; j <= last; ++j) {
      const CellStation& p = whole[j > 0 ? j - 1 : 0];
      const CellStation& n = whole[j < last ? j + 1 : last];
      const V3 t = normalized(
          V3{static_cast<f64>(n.x - p.x), static_cast<f64>(n.y - p.y), static_cast<f64>(n.z - p.z)},
          V3{1.0, 0.0, 0.0});
      whole[j].tx = q20(t.x);
      whole[j].ty = q20(t.y);
      whole[j].tz = q20(t.z);
    }
    for (u32 k = 0; k + 1 < splits[i].size(); ++k) {
      const u32 from = splits[i][k], to = splits[i][k + 1];
      PlanEdge edge;
      edge.kind = strand.kind;
      if (strand.sump_until != 0 && to <= strand.sump_until) edge.kind = StrandKind::sump;
      edge.level = strand.level;
      edge.first = plan.stations.size();
      edge.count = to - from + 1;
      // An end that joins a host is the host's node; any other split is this strand's own.
      const auto end_node = [&](u32 station, bool start) -> u32 {
        if (start && station == 0 && strand.host_a >= 0)
          return node_at(static_cast<u32>(strand.host_a), strand.host_a_station, EndKind::junction);
        if (!start && station == last && strand.host_b >= 0)
          return node_at(static_cast<u32>(strand.host_b), strand.host_b_station, EndKind::junction);
        const EndKind end =
            station == 0 ? strand.end_a : (station == last ? strand.end_b : EndKind::junction);
        return node_at(i, station, end);
      };
      edge.node_a = end_node(from, true);
      edge.node_b = end_node(to, false);
      const u32 id = plan.edges.size();
      for (u32 j = from; j <= to; ++j) {
        PlanStation ps;
        ps.station = whole[j];
        ps.station.edge = id;
        ps.station.index = j - from;
        ps.cover_mm = to_mm32(std::max(-1.0e6, std::min(1.0e6, pl.cover(strand.st[j]))));
        clearance(plan, ps);
        plan.stations.push_back(ps);
      }
      edge.length_m = static_cast<f32>(static_cast<f64>(whole[to].s - whole[from].s) / 1000.0);
      bool walkable = edge.kind != StrandKind::crawl && edge.kind != StrandKind::shaft &&
                      edge.kind != StrandKind::fissure && edge.kind != StrandKind::sump;
      for (u32 j = from; j <= to && walkable; ++j) {
        const PlanStation& ps = plan.stations[edge.first + (j - from)];
        if (!ps.passable) walkable = false;
        if (j > from) {
          const f64 rise = std::fabs(static_cast<f64>(whole[j].floor - whole[j - 1].floor));
          const f64 run = static_cast<f64>(whole[j].s - whole[j - 1].s);
          if (rise > 0.8 * run) walkable = false;
        }
      }
      edge.walkable = walkable;
      plan.edges.push_back(edge);
      ++plan.nodes[edge.node_a].degree;
      ++plan.nodes[edge.node_b].degree;
    }
  }
}

u32 find_root(Vector<u32>& parent, u32 i) {
  while (parent[i] != i) {
    parent[i] = parent[parent[i]];
    i = parent[i];
  }
  return i;
}

void graph_report(Plan& plan) {
  PlanReport& rep = plan.report;
  Vector<u32> parent;
  for (u32 i = 0; i < plan.nodes.size(); ++i)
    parent.push_back(i);
  for (const PlanEdge& e : plan.edges) {
    const u32 a = find_root(parent, e.node_a), b = find_root(parent, e.node_b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
    rep.passage_m += e.length_m;
    if (e.walkable) rep.walkable_m += e.length_m;
  }
  u32 components = 0;
  for (u32 i = 0; i < plan.nodes.size(); ++i)
    if (find_root(parent, i) == i) ++components;
  rep.loops = plan.edges.size() + components - plan.nodes.size();
  for (const PlanNode& n : plan.nodes)
    if (n.degree == 1 && n.end != EndKind::mouth) ++rep.dead_ends;
  // What a walker reaches from a mouth: over walkable edges, a node at a time.
  Vector<u8> reached(plan.nodes.size(), u8{0});
  Vector<u8> walked(plan.edges.size(), u8{0});
  for (u32 i = 0; i < plan.nodes.size(); ++i)
    if (plan.nodes[i].end == EndKind::mouth && plan.nodes[i].degree == 1) reached[i] = 1;
  for (bool more = true; more;) {
    more = false;
    for (u32 e = 0; e < plan.edges.size(); ++e) {
      const PlanEdge& edge = plan.edges[e];
      if (walked[e] != 0 || !edge.walkable) continue;
      if (reached[edge.node_a] == 0 && reached[edge.node_b] == 0) continue;
      walked[e] = 1;
      reached[edge.node_a] = reached[edge.node_b] = 1;
      rep.reachable_m += edge.length_m;
      more = true;
    }
  }
}

// ---- cells --------------------------------------------------------------------------------------

f64 station_reach_m(const CellStation& s) noexcept {
  const i32 r = std::max(std::max(s.a, s.b),
                         std::max(s.slot_hw, std::max(std::abs(s.slot_top), std::abs(s.slot_bot))));
  return r / 1000.0;
}

f64 field_margin(const Network& net) noexcept {
  return std::max(1.0, static_cast<f64>(net.apron_m) + 0.75);
}

void cut_cells(Plan& plan) {
  const f64 most = static_cast<f64>(plan.network.cell_length_m);
  Vector<u32> node_cell(plan.nodes.size(), k_no_cell);
  for (u32 e = 0; e < plan.edges.size(); ++e) {
    const PlanEdge& edge = plan.edges[e];
    const f64 len = static_cast<f64>(edge.length_m);
    // A short edge is its host's: a cell of a few metres would be all portal.
    if (len < most / 3.0 &&
        (node_cell[edge.node_a] != k_no_cell || node_cell[edge.node_b] != k_no_cell)) {
      const u32 cell =
          node_cell[edge.node_a] != k_no_cell ? node_cell[edge.node_a] : node_cell[edge.node_b];
      for (u32 j = 0; j < edge.count; ++j)
        plan.stations[edge.first + j].station.cell = cell;
      plan.cells[cell].passage_m += edge.length_m;
      if (node_cell[edge.node_a] == k_no_cell) node_cell[edge.node_a] = cell;
      if (node_cell[edge.node_b] == k_no_cell) node_cell[edge.node_b] = cell;
      continue;
    }
    const u32 chunks = std::max<u32>(1, static_cast<u32>(len / most + 0.999));
    const u32 base = plan.cells.size();
    for (u32 c = 0; c < chunks; ++c) {
      PlanCell cell;
      cell.passage_m = edge.length_m / static_cast<f32>(chunks);
      plan.cells.push_back(cell);
    }
    for (u32 j = 0; j < edge.count; ++j) {
      const u32 c =
          std::min(chunks - 1, static_cast<u32>(static_cast<u64>(j) * chunks / edge.count));
      plan.stations[edge.first + j].station.cell = base + c;
    }
    if (node_cell[edge.node_a] == k_no_cell) node_cell[edge.node_a] = base;
    if (node_cell[edge.node_b] == k_no_cell) node_cell[edge.node_b] = base + chunks - 1;
  }
  for (PlanHall& h : plan.halls) {
    h.hall.cell = plan.cells.size();
    PlanCell cell;
    cell.hall = true;
    plan.cells.push_back(cell);
  }
  // A cell whose every roof is under the water has no surface: it is in the graph and has no mesh.
  Vector<u8> dry(plan.cells.size(), u8{0});
  for (const PlanStation& ps : plan.stations) {
    const CellStation& s = ps.station;
    const i64 top = s.y + std::max(s.b, s.slot_hw > 0 ? s.slot_top : 0);
    if (!plan.has_water || s.frame == SectionFrame::Shaft || top > plan.water_mm + 350)
      dry[s.cell] = 1;
    ++plan.cells[s.cell].stations;
  }
  for (u32 i = 0; i < plan.cells.size(); ++i)
    plan.cells[i].flooded = !plan.cells[i].hall && dry[i] == 0;
}

// A cell's bounds on the lattice: what its stations and its hall reach, the walls' detail, two
// lattice steps, and round a mouth the apron — not the field's margin, which says what a point is
// asked of and not where the cell's own surface can be. After the mouths are found.
void bound_cells(Plan& plan) {
  const f64 voxel = static_cast<f64>(plan.voxel_mm) / 1000.0;
  Vector<u8> started(plan.cells.size(), u8{0});
  const auto reach = [&](u32 cell, f64 x, f64 y, f64 z, f64 rx, f64 ry) {
    PlanCell& c = plan.cells[cell];
    const f64 lo[3] = {x - rx, y - ry, z - rx}, hi[3] = {x + rx, y + ry, z + rx};
    for (u32 k = 0; k < 3; ++k) {
      const i64 l = static_cast<i64>(std::floor(lo[k] / voxel)) - 1;
      const i64 h = static_cast<i64>(std::floor(hi[k] / voxel)) + 2;
      if (started[cell] == 0 || l < c.lo[k]) c.lo[k] = l;
      if (started[cell] == 0 || h > c.hi[k]) c.hi[k] = h;
    }
    started[cell] = 1;
  };
  for (const PlanStation& ps : plan.stations) {
    const CellStation& s = ps.station;
    const f64 pad =
        (plan.cells[s.cell].mouth ? static_cast<f64>(plan.network.apron_m) + 0.5 : 0.0) + 0.8 +
        2.0 * voxel;
    const f64 r = station_reach_m(s) + pad + 0.5;
    reach(s.cell, static_cast<f64>(s.x - plan.anchor[0]) / 1000.0,
          static_cast<f64>(s.y - plan.anchor[1]) / 1000.0,
          static_cast<f64>(s.z - plan.anchor[2]) / 1000.0, r, r);
  }
  for (const PlanHall& h : plan.halls)
    reach(h.hall.cell, static_cast<f64>(h.hall.x - plan.anchor[0]) / 1000.0,
          static_cast<f64>(h.hall.y - plan.anchor[1]) / 1000.0,
          static_cast<f64>(h.hall.z - plan.anchor[2]) / 1000.0,
          std::max(h.hall.ra, h.hall.rb) / 1000.0 + 0.8 + 2.0 * voxel,
          h.hall.rh / 1000.0 + 0.8 + 2.0 * voxel);
  for (PlanCell& c : plan.cells) {
    const i64 mid_x = plan.anchor[0] + (c.lo[0] + c.hi[0]) * plan.voxel_mm / 2;
    const i64 mid_z = plan.anchor[2] + (c.lo[2] + c.hi[2]) * plan.voxel_mm / 2;
    const auto tile_of = [&](i64 mm_at) {
      return static_cast<i32>(mm_at >= 0 ? mm_at / plan.tile_mm
                                         : -((-mm_at + plan.tile_mm - 1) / plan.tile_mm));
    };
    c.tile = scene_gen::TileCoord{tile_of(mid_x), tile_of(mid_z)};
  }
}

void find_portals(Plan& plan) {
  const auto add = [&](u32 a, u32 b, const CellStation& s, i64 x, i64 y, i64 z) {
    PlanPortal p;
    p.cell_a = a;
    p.cell_b = b;
    p.x = x;
    p.y = y;
    p.z = z;
    p.normal = Vec3{static_cast<f32>(s.tx * k_q20), static_cast<f32>(s.ty * k_q20),
                    static_cast<f32>(s.tz * k_q20)};
    p.half_width_m = static_cast<f32>(std::max(s.a, s.slot_hw) / 1000.0);
    p.half_height_m = static_cast<f32>(std::max(s.b, (s.slot_top - s.slot_bot) / 2) / 1000.0);
    plan.portals.push_back(p);
  };
  for (const PlanEdge& edge : plan.edges) {
    for (u32 j = 0; j + 1 < edge.count; ++j) {
      const CellStation& s0 = plan.stations[edge.first + j].station;
      const CellStation& s1 = plan.stations[edge.first + j + 1].station;
      if (s0.cell != s1.cell)
        add(s0.cell, s1.cell, s0, (s0.x + s1.x) / 2, (s0.y + s1.y) / 2, (s0.z + s1.z) / 2);
      // Into and out of a hall.
      for (const PlanHall& h : plan.halls) {
        const auto in_hall = [&](const CellStation& s) {
          const f64 ax = h.hall.axis_x * k_q20, az = h.hall.axis_z * k_q20;
          const f64 dx = static_cast<f64>(s.x - h.hall.x), dz = static_cast<f64>(s.z - h.hall.z);
          const f64 dy = static_cast<f64>(s.y - h.hall.y);
          const f64 lx = (dx * ax + dz * az) / h.hall.ra, lz = (-dx * az + dz * ax) / h.hall.rb;
          const f64 ly = dy / h.hall.rh;
          return lx * lx + ly * ly + lz * lz < 1.0;
        };
        const bool in0 = in_hall(s0), in1 = in_hall(s1);
        if (in0 != in1)
          add(in0 ? h.hall.cell : s0.cell, in0 ? s1.cell : h.hall.cell, s0, (s0.x + s1.x) / 2,
              (s0.y + s1.y) / 2, (s0.z + s1.z) / 2);
      }
    }
  }
  // Wherever the voids of two cells overlap there is a way from one into the other — a junction,
  // a branch leaving its trunk's wall a few metres from the node, a canyon cutting a tube it
  // crosses: a portal for every such pair the lines above did not name, so the list is whole (a
  // host that loads by what a portal shows must miss no cell; one it lists whose cells turn out
  // not to touch costs a load and nothing else).
  const auto known = [&](u32 a, u32 b) {
    for (const PlanPortal& p : plan.portals)
      if ((p.cell_a == a && p.cell_b == b) || (p.cell_a == b && p.cell_b == a)) return true;
    return false;
  };
  for (u32 i = 0; i < plan.stations.size(); ++i) {
    const CellStation& a = plan.stations[i].station;
    for (u32 j = i + 1; j < plan.stations.size(); ++j) {
      const CellStation& b = plan.stations[j].station;
      if (a.cell == b.cell) continue;
      const f64 reach = station_reach_m(a) + station_reach_m(b) + 0.5;
      const f64 dx = static_cast<f64>(a.x - b.x) / 1000.0,
                dy = static_cast<f64>(a.y - b.y) / 1000.0;
      const f64 dz = static_cast<f64>(a.z - b.z) / 1000.0;
      if (dx * dx + dy * dy + dz * dz > reach * reach || known(a.cell, b.cell)) continue;
      add(a.cell, b.cell, b, (a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2);
    }
    for (const PlanHall& h : plan.halls) {
      if (known(a.cell, h.hall.cell)) continue;
      const f64 grow = station_reach_m(a) * 1000.0 + 500.0;
      const f64 ax = h.hall.axis_x * k_q20, az = h.hall.axis_z * k_q20;
      const f64 dx = static_cast<f64>(a.x - h.hall.x), dz = static_cast<f64>(a.z - h.hall.z);
      const f64 lx = (dx * ax + dz * az) / (h.hall.ra + grow);
      const f64 lz = (-dx * az + dz * ax) / (h.hall.rb + grow);
      const f64 ly = static_cast<f64>(a.y - h.hall.y) / (h.hall.rh + grow);
      if (lx * lx + ly * ly + lz * lz < 1.0) add(a.cell, h.hall.cell, a, a.x, a.y, a.z);
    }
  }
}

}  // namespace

namespace {

// Closed loops of the level set `value` of a grid (marching squares), the longest first in `out`.
// A point is (x, z) in lattice units of the window.
struct Loop {
  Vector<f64> xs, zs;
  f64 area = 0.0;
};

void contour(const Vector<f64>& grid, u32 nx, u32 nz, f64 level, Loop& best) {
  // Segments, each between two crossings on a cell's edges; an edge is named by its lower point
  // and its axis, so two cells name a shared edge alike and the loops close exactly.
  struct Seg {
    u32 a, b;
  };
  Vector<Seg> segs;
  const auto edge_id = [&](u32 i, u32 j, u32 axis) { return (j * nx + i) * 2u + axis; };
  for (u32 j = 0; j + 1 < nz; ++j) {
    for (u32 i = 0; i + 1 < nx; ++i) {
      const bool in[4] = {grid[j * nx + i] < level, grid[j * nx + i + 1] < level,
                          grid[(j + 1) * nx + i + 1] < level, grid[(j + 1) * nx + i] < level};
      const u32 edges[4] = {edge_id(i, j, 0), edge_id(i + 1, j, 1), edge_id(i, j + 1, 0),
                            edge_id(i, j, 1)};
      u32 crossed[4];
      u32 n = 0;
      for (u32 k = 0; k < 4; ++k)
        if (in[k] != in[(k + 1) % 4]) crossed[n++] = edges[k];
      if (n == 2) {
        segs.push_back(Seg{crossed[0], crossed[1]});
      } else if (n == 4) {
        // The saddle: join so the inside corners stay apart.
        if (in[0]) {
          segs.push_back(Seg{crossed[0], crossed[3]});
          segs.push_back(Seg{crossed[1], crossed[2]});
        } else {
          segs.push_back(Seg{crossed[0], crossed[1]});
          segs.push_back(Seg{crossed[2], crossed[3]});
        }
      }
    }
  }
  const auto point = [&](u32 id, f64& x, f64& z) {
    const u32 axis = id & 1u, at = id >> 1;
    const u32 i = at % nx, j = at / nx;
    const f64 va = grid[j * nx + i];
    const f64 vb = axis == 0 ? grid[j * nx + i + 1] : grid[(j + 1) * nx + i];
    const f64 t = (level - va) / (vb - va);
    x = static_cast<f64>(i) + (axis == 0 ? t : 0.0);
    z = static_cast<f64>(j) + (axis == 1 ? t : 0.0);
  };
  Vector<u8> used(segs.size(), u8{0});
  best = Loop{};
  for (u32 s = 0; s < segs.size(); ++s) {
    if (used[s] != 0) continue;
    Loop loop;
    u32 at = segs[s].b;
    const u32 start = segs[s].a;
    used[s] = 1;
    f64 x = 0.0, z = 0.0;
    point(start, x, z);
    loop.xs.push_back(x);
    loop.zs.push_back(z);
    bool closed = false;
    for (u32 guard = 0; guard < segs.size() + 1; ++guard) {
      point(at, x, z);
      loop.xs.push_back(x);
      loop.zs.push_back(z);
      if (at == start) {
        closed = true;
        break;
      }
      bool found = false;
      for (u32 k = 0; k < segs.size(); ++k) {
        if (used[k] != 0) continue;
        if (segs[k].a == at || segs[k].b == at) {
          at = segs[k].a == at ? segs[k].b : segs[k].a;
          used[k] = 1;
          found = true;
          break;
        }
      }
      if (!found) break;
    }
    if (!closed) continue;
    loop.xs.pop_back();
    loop.zs.pop_back();
    f64 twice = 0.0;
    for (u32 k = 0; k < loop.xs.size(); ++k) {
      const u32 m = (k + 1) % loop.xs.size();
      twice += loop.xs[k] * loop.zs[m] - loop.xs[m] * loop.zs[k];
    }
    loop.area = 0.5 * std::fabs(twice);
    if (loop.area > best.area) best = std::move(loop);
  }
}

void find_mouths(Plan& plan, const scene_gen::Ground& ground) {
  Field field;
  plan_field(plan, field);
  const i64 voxel = plan.voxel_mm;
  const f64 step = static_cast<f64>(voxel) / 1000.0;
  const f64 cover_to = field.margin + 0.75;
  for (u32 e = 0; e < plan.edges.size(); ++e) {
    const PlanEdge& edge = plan.edges[e];
    if (plan.nodes[edge.node_b].end != EndKind::mouth || plan.nodes[edge.node_b].degree != 1)
      continue;
    // The stations near the ground, and the window of lattice columns round them.
    i64 lo_i = 0, hi_i = 0, lo_j = 0, hi_j = 0;
    bool any = false;
    for (u32 j = 0; j < edge.count; ++j) {
      const PlanStation& ps = plan.stations[edge.first + j];
      if (ps.cover_mm > to_mm32(cover_to + 1.0)) continue;
      const f64 r = station_reach_m(ps.station) + cover_to + 2.0;
      const f64 x = static_cast<f64>(ps.station.x - plan.anchor[0]) / 1000.0;
      const f64 z = static_cast<f64>(ps.station.z - plan.anchor[2]) / 1000.0;
      const i64 a = static_cast<i64>(std::floor((x - r) / step));
      const i64 b = static_cast<i64>(std::floor((x + r) / step)) + 1;
      const i64 c = static_cast<i64>(std::floor((z - r) / step));
      const i64 d = static_cast<i64>(std::floor((z + r) / step)) + 1;
      lo_i = any ? std::min(lo_i, a) : a;
      hi_i = any ? std::max(hi_i, b) : b;
      lo_j = any ? std::min(lo_j, c) : c;
      hi_j = any ? std::max(hi_j, d) : d;
      any = true;
    }
    if (!any) continue;
    PlanMouth mouth;
    mouth.edge = e;
    mouth.pit = edge.kind == StrandKind::shaft || edge.kind == StrandKind::fissure;
    mouth.patch_i = lo_i;
    mouth.patch_j = lo_j;
    mouth.patch_nx = static_cast<u32>(hi_i - lo_i + 1);
    mouth.patch_nz = static_cast<u32>(hi_j - lo_j + 1);
    Vector<f64> raw;
    for (u32 j = 0; j < mouth.patch_nz; ++j) {
      for (u32 i = 0; i < mouth.patch_nx; ++i) {
        const i64 x_mm = plan.anchor[0] + (lo_i + i) * voxel;
        const i64 z_mm = plan.anchor[2] + (lo_j + j) * voxel;
        const f64 h = ground.present() ? ground.surface_mm(x_mm, z_mm) : 0.0;
        const i64 h_mm = scene_gen::nearest_mm(h) - plan.anchor[1];
        mouth.patch.push_back(static_cast<i32>(h_mm));
        const V3 p{static_cast<f64>((lo_i + i) * voxel) / 1000.0, static_cast<f64>(h_mm) / 1000.0,
                   static_cast<f64>((lo_j + j) * voxel) / 1000.0};
        raw.push_back(std::min(field.sample(p).raw, 50.0));
      }
    }
    const auto to_points = [&](const Loop& loop, Vector<OutlinePoint>& out) {
      for (u32 k = 0; k < loop.xs.size(); ++k) {
        const u32 i = std::min(mouth.patch_nx - 2, static_cast<u32>(loop.xs[k]));
        const u32 j = std::min(mouth.patch_nz - 2, static_cast<u32>(loop.zs[k]));
        const f64 tx = loop.xs[k] - i, tz = loop.zs[k] - j;
        const auto at = [&](u32 ii, u32 jj) {
          return static_cast<f64>(mouth.patch[jj * mouth.patch_nx + ii]);
        };
        const f64 h =
            lerp(lerp(at(i, j), at(i + 1, j), tx), lerp(at(i, j + 1), at(i + 1, j + 1), tx), tz);
        OutlinePoint p;
        p.x = plan.anchor[0] + scene_gen::nearest_mm((static_cast<f64>(lo_i) + loop.xs[k]) * step);
        p.z = plan.anchor[2] + scene_gen::nearest_mm((static_cast<f64>(lo_j) + loop.zs[k]) * step);
        p.y = plan.anchor[1] + static_cast<i64>(std::floor(h + 0.5));
        out.push_back(p);
      }
    };
    Loop opening, hole;
    contour(raw, mouth.patch_nx, mouth.patch_nz, 0.0, opening);
    if (opening.xs.size() < 3) continue;
    // The hole: the opening grown by six tenths of the apron (caves.md, "What the terrain's hole
    // must be").
    contour(raw, mouth.patch_nx, mouth.patch_nz, 0.6 * field.apron, hole);
    to_points(opening, mouth.outline);
    to_points(hole, mouth.hole);
    f64 cx = 0.0, cz = 0.0, min_x = 1e30, max_x = -1e30, min_z = 1e30, max_z = -1e30;
    for (u32 k = 0; k < opening.xs.size(); ++k) {
      cx += opening.xs[k];
      cz += opening.zs[k];
      min_x = std::min(min_x, opening.xs[k]);
      max_x = std::max(max_x, opening.xs[k]);
      min_z = std::min(min_z, opening.zs[k]);
      max_z = std::max(max_z, opening.zs[k]);
    }
    cx /= static_cast<f64>(opening.xs.size());
    cz /= static_cast<f64>(opening.zs.size());
    mouth.x = plan.anchor[0] + scene_gen::nearest_mm((static_cast<f64>(lo_i) + cx) * step);
    mouth.z = plan.anchor[2] + scene_gen::nearest_mm((static_cast<f64>(lo_j) + cz) * step);
    const f64 here = ground.present() ? ground.surface_mm(mouth.x, mouth.z) : 0.0;
    mouth.y = scene_gen::nearest_mm(here);
    const auto ground_at = [&](i64 dx, i64 dz) {
      return ground.present() ? ground.surface_mm(mouth.x + dx, mouth.z + dz) : 0.0;
    };
    const f64 gx = (ground_at(500, 0) - ground_at(-500, 0)),
              gz = (ground_at(0, 500) - ground_at(0, -500));
    mouth.slope = static_cast<f32>(std::sqrt(gx * gx + gz * gz));
    mouth.area_m2 = static_cast<f32>(opening.area * step * step);
    mouth.plan_depth_m = static_cast<f32>(std::min(max_x - min_x, max_z - min_z) * step);
    // The cells that own the ground round it are drawn from far away too.
    for (u32 j = 0; j < edge.count; ++j) {
      const PlanStation& ps = plan.stations[edge.first + j];
      if (ps.cover_mm <= to_mm32(cover_to)) plan.cells[ps.station.cell].mouth = true;
    }
    PlanPortal portal;
    portal.cell_a = plan.stations[edge.first + edge.count - 1].station.cell;
    portal.cell_b = k_outside;
    portal.x = mouth.x;
    portal.y = mouth.y;
    portal.z = mouth.z;
    const CellStation& tip = plan.stations[edge.first + edge.count - 1].station;
    portal.normal = Vec3{static_cast<f32>(tip.tx * k_q20), static_cast<f32>(tip.ty * k_q20),
                         static_cast<f32>(tip.tz * k_q20)};
    portal.half_width_m = static_cast<f32>(0.5 * (max_x - min_x) * step);
    portal.half_height_m = static_cast<f32>(0.5 * (max_z - min_z) * step);
    plan.portals.push_back(portal);
    plan.mouths.push_back(std::move(mouth));
  }
}

}  // namespace

bool network_in_range(const Network& n, std::string& error) {
  Check c{error};
  if (n.format != k_network_format) {
    error = "engine.caves.Network.format is \"" + n.format + "\", not \"" +
            std::string(k_network_format) + "\"";
    return false;
  }
  if (n.version != 1) return c.bad("version", "1");
  const i64 voxel = scene_gen::nearest_mm(static_cast<f64>(n.voxel_m));
  if (voxel != 125 && voxel != 250 && voxel != 500) return c.bad("voxel_m", "0.125, 0.25 or 0.5 m");
  if (!(std::fabs(n.origin.x) <= 1.0e8 && std::fabs(n.origin.y) <= 1.0e6 &&
        std::fabs(n.origin.z) <= 1.0e8))
    return c.bad("origin", "within 100,000 km");
  return c.f(n.member_thickness_m, 6.0, 400.0, "member_thickness_m", "6 to 400 m") &&
         c.f(n.member_margin_m, 0.25, 10.0, "member_margin_m", "0.25 to 10 m") &&
         c.f(n.reach_strike_m, 40.0, 2000.0, "reach_strike_m", "40 to 2,000 m") &&
         c.f(n.reach_dip_m, 15.0, 1000.0, "reach_dip_m", "15 to 1,000 m") &&
         c.u(n.levels, 1, 4, "levels", "1 to 4") &&
         c.f(n.level_spacing_m, 4.0, 40.0, "level_spacing_m", "4 to 40 m") &&
         c.f(n.base_level_m, -1.0e5, 1.0e5, "base_level_m", "within 100 km") &&
         c.f(n.trunk_width_m, 2.5, 14.0, "trunk_width_m", "2.5 to 14 m") &&
         c.f(n.feeders_per_100m, 0.0, 4.0, "feeders_per_100m", "0 to 4") &&
         c.u(n.halls, 0, 4, "halls", "0 to 4") && c.u(n.crawls, 0, 8, "crawls", "0 to 8") &&
         c.u(n.bypasses, 0, 4, "bypasses", "0 to 4") &&
         c.u(n.openings, 0, 8, "openings", "0 to 8") &&
         c.f(n.lead_reach_m, 5.0, 400.0, "lead_reach_m", "5 to 400 m") &&
         c.f(n.max_shaft_m, 0.0, 100.0, "max_shaft_m", "0 to 100 m") &&
         c.f(n.min_cover_m, 0.5, 20.0, "min_cover_m", "0.5 to 20 m") &&
         c.f(n.collapse_cover, 0.1, 1.0, "collapse_cover", "0.1 to 1") &&
         c.f(n.fill, 0.0, 0.6, "fill", "0 to 0.6") &&
         c.f(n.cell_length_m, 8.0, 64.0, "cell_length_m", "8 to 64 m") &&
         c.f(n.apron_m, 0.0, std::min(6.0, static_cast<f64>(n.min_cover_m) - 0.25), "apron_m",
             "0 to 6 m, and a quarter of a metre under min_cover_m") &&
         c.f(n.flow_speed_m_s, 0.01, 2.0, "flow_speed_m_s", "0.01 to 2 m/s") &&
         c.f(n.ledge_m, 0.0, 0.6, "ledge_m", "0 to 0.6 m") &&
         c.f(n.walker_radius_m, 0.1, 1.0, "walker_radius_m", "0.1 to 1 m") &&
         c.f(n.walker_height_m, 0.5, 3.0, "walker_height_m", "0.5 to 3 m") &&
         c.u(n.far_ring, 0, 4, "far_ring", "0 to 4");
}

bool read_network(std::string_view text, Network& out, std::string& error) {
  out = Network{};
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = "line " + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, root, ctx) || !ctx.ok()) {
    error = ctx.diagnostics.empty() ? std::string("not an engine.caves.Network")
                                    : ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message;
    return false;
  }
  return network_in_range(out, error);
}

bool plan_network(const Network& network, const scene_gen::Ground& ground, i64 tile_mm, Plan& out,
                  std::string& error) {
  out = Plan{};
  if (!network_in_range(network, error)) return false;
  out.network = network;
  out.formation = network.formation.has_value() ? *network.formation : level_formation(network);
  out.voxel_mm = scene_gen::nearest_mm(static_cast<f64>(network.voxel_m));
  out.tile_mm = tile_mm > 0 ? tile_mm : 32000;
  // The anchor: the lattice point at or below the origin.
  const f64 origin[3] = {network.origin.x, network.origin.y, network.origin.z};
  for (u32 k = 0; k < 3; ++k)
    out.anchor[k] =
        static_cast<i64>(std::floor(origin[k] * 1000.0 / static_cast<f64>(out.voxel_mm))) *
        out.voxel_mm;

  Planner pl{network, ground, Bed{}, 0.0, static_cast<f64>(network.member_margin_m), 0.0, {}, {}};
  Bed& bed = pl.bed;
  std::string why;
  if (!gfx::pack_formation(out.formation, bed.params, &why)) {
    error = "engine.caves.Network." + why;
    return false;
  }
  bed.frame = gfx::formation_frame(bed.params);
  bed.n = {bed.frame.normal[0], bed.frame.normal[1], bed.frame.normal[2]};
  bed.t = normalized(V3{bed.frame.dip[0], 0.0, bed.frame.dip[2]}, V3{1.0, 0.0, 0.0});
  bed.strike = {-bed.t.z, 0.0, bed.t.x};
  bed.sin_dip = dot(bed.n, bed.t);
  bed.cos_dip = bed.n.y;
  bed.flat = std::fabs(bed.sin_dip) < 0.035;
  bed.down = {bed.t.x * bed.cos_dip, -bed.sin_dip, bed.t.z * bed.cos_dip};
  const V3 o{origin[0], origin[1], origin[2]};
  const f64 c_origin = bed.coordinate(o);
  // The member: a named bed's occurrence nearest the origin, or a thickness about it.
  bed.c_base = c_origin - 0.5 * static_cast<f64>(network.member_thickness_m);
  bed.c_top = c_origin + 0.5 * static_cast<f64>(network.member_thickness_m);
  if (!network.member.empty()) {
    bool found = false;
    for (u32 pass = 0; pass < 2 && !found; ++pass) {
      const auto& beds = pass == 0 ? out.formation.macro_beds : out.formation.beds;
      const gfx::FormationSequence& seq = pass == 0 ? bed.params.macro : bed.params.beds;
      for (u32 i = 0; i < beds.size() && !found; ++i) {
        if (beds[i].name != network.member) continue;
        const f64 period = static_cast<f64>(seq.period_mm) / 1000.0;
        const f64 start = static_cast<f64>(seq.beds[i].start_m);
        const f64 thick = static_cast<f64>(seq.beds[i].thickness_m);
        const f64 k = std::floor((c_origin - start - 0.5 * thick) / period + 0.5);
        bed.c_base = k * period + start;
        bed.c_top = bed.c_base + thick;
        found = true;
      }
    }
    if (!found) {
      error =
          "engine.caves.Network.member names no bed of the formation: \"" + network.member + "\"";
      return false;
    }
  }
  bed.c_mid = 0.5 * (bed.c_base + bed.c_top);
  pl.member = bed.c_top - bed.c_base;
  // Under the origin, on the member's middle.
  bed.q0 = o;
  for (u32 k = 0; k < 6; ++k)
    bed.q0.y += (bed.c_mid - bed.coordinate(bed.q0)) / bed.n.y;
  // Base level: the lowest ground within the reach, every eight metres.
  if (network.base_level_auto && ground.present()) {
    const f64 half_s = 0.5 * static_cast<f64>(network.reach_strike_m);
    const f64 half_d = static_cast<f64>(network.reach_dip_m);
    f64 lowest = 1.0e30;
    for (f64 u = -half_s; u <= half_s; u += 8.0)
      for (f64 v = -half_d; v <= half_d; v += 8.0)
        lowest = std::min(lowest, pl.ground_y(bed.q0 + bed.strike * u + bed.t * v));
    pl.water_y = lowest;
  } else {
    pl.water_y = network.base_level_auto ? 0.0 : network.base_level_m;
  }
  out.has_water = true;
  out.water_mm = scene_gen::nearest_mm(pl.water_y);

  if (!lay_trunks(pl, error)) return false;
  lay_feeders(pl);
  lay_connectors(pl);
  lay_bypasses(pl);
  lay_crawls(pl);
  lay_halls(pl, out.halls);
  lay_leads(pl);

  PlanReport& rep = out.report;
  for (const Strand& s : pl.strands) {
    if (s.kind == StrandKind::trunk) ++rep.levels;
    if (s.kind == StrandKind::fissure) ++rep.fissures;
    if (s.end_b == EndKind::mouth || s.end_a == EndKind::mouth) {
      if (s.led) {
        ++rep.mouths_led;
      } else if (s.kind != StrandKind::fissure) {
        rep.mouths_found +=
            (s.end_a == EndKind::mouth ? 1u : 0u) + (s.end_b == EndKind::mouth ? 1u : 0u);
      }
    }
  }
  rep.strands = pl.strands.size();
  rep.base_level_m = pl.water_y;
  rep.member_base_m = bed.c_base;
  rep.member_top_m = bed.c_top;
  rep.dip_offset_m = pl.strands[0].st[pl.strands[0].st.size() / 2].v;
  build_graph(pl, out);
  graph_report(out);
  // The least rock between a passage's section and the member's top or base, across the beds.
  f64 least = 1.0e30;
  for (const PlanStation& ps : out.stations) {
    const CellStation& s = ps.station;
    if (s.frame == SectionFrame::Shaft) continue;
    const f64 c = bed_coordinate(out, s.x, s.y, s.z);
    const f64 up = s.b / 1000.0;
    const f64 down_reach = std::max(s.b / 1000.0, -s.slot_bot / 1000.0 * bed.cos_dip);
    least = std::min(least, std::min(bed.c_top - (c + up), (c - down_reach) - bed.c_base));
  }
  rep.least_margin_m = least;
  cut_cells(out);
  find_portals(out);
  find_mouths(out, ground);
  bound_cells(out);
  if (out.mouths.empty()) pl.note("No opening was found: the network is sealed.");
  rep.notes = pl.notes;
  return true;
}

f64 bed_coordinate(const Plan& plan, i64 x_mm, i64 y_mm, i64 z_mm) {
  gfx::FormationParams params;
  gfx::pack_formation(plan.formation, params, nullptr);
  const gfx::FormationFrame frame = gfx::formation_frame(params);
  return gfx::formation_coordinate(frame, static_cast<f64>(x_mm) / 1000.0,
                                   static_cast<f64>(y_mm) / 1000.0, static_cast<f64>(z_mm) / 1000.0)
      .coordinate;
}

u64 plan_hash(const Plan& plan) {
  Vector<i64> n;
  for (const PlanStation& ps : plan.stations) {
    const CellStation& s = ps.station;
    for (const i64 v : {s.x,
                        s.y,
                        s.z,
                        i64{s.tx},
                        i64{s.ty},
                        i64{s.tz},
                        s.s,
                        i64{s.a},
                        i64{s.b},
                        i64{s.slot_hw},
                        i64{s.slot_top},
                        i64{s.slot_bot},
                        s.floor,
                        i64{s.edge},
                        i64{s.index},
                        i64{s.cell},
                        static_cast<i64>(s.frame),
                        i64{s.flags},
                        i64{ps.cover_mm},
                        i64{ps.clear_h_mm},
                        i64{ps.clear_w_mm},
                        i64{ps.passable ? 1 : 0}})
      n.push_back(v);
  }
  for (const PlanEdge& e : plan.edges)
    for (const i64 v : {i64{e.first}, i64{e.count}, i64{e.node_a}, i64{e.node_b},
                        static_cast<i64>(e.kind), i64{e.level}, i64{e.walkable ? 1 : 0}})
      n.push_back(v);
  for (const PlanHall& h : plan.halls)
    for (const i64 v :
         {h.hall.x, h.hall.y, h.hall.z, i64{h.hall.ra}, i64{h.hall.rb}, i64{h.hall.rh},
          i64{h.hall.axis_x}, i64{h.hall.axis_z}, h.hall.floor, i64{h.hall.pile}, i64{h.hall.cell},
          static_cast<i64>(h.hall.seed), i64{h.fallen ? 1 : 0}})
      n.push_back(v);
  for (const PlanMouth& m : plan.mouths) {
    for (const OutlinePoint& p : m.outline)
      for (const i64 v : {p.x, p.y, p.z})
        n.push_back(v);
    for (const OutlinePoint& p : m.hole)
      for (const i64 v : {p.x, p.y, p.z})
        n.push_back(v);
    for (const i32 h : m.patch)
      n.push_back(h);
  }
  for (const PlanCell& c : plan.cells)
    for (const i64 v : {c.lo[0], c.lo[1], c.lo[2], c.hi[0], c.hi[1], c.hi[2], i64{c.tile.x},
                        i64{c.tile.z}, i64{c.mouth ? 1 : 0}, i64{c.flooded ? 1 : 0}})
      n.push_back(v);
  for (const PlanPortal& p : plan.portals)
    for (const i64 v : {i64{p.cell_a}, i64{p.cell_b}, p.x, p.y, p.z})
      n.push_back(v);
  n.push_back(plan.water_mm);
  return hash_bytes(n.data(), n.size() * sizeof(i64));
}

WorldPos cell_origin(const Plan& plan, u32 cell) noexcept {
  const PlanCell& c = plan.cells[cell];
  WorldPos p;
  p.x = static_cast<f64>(plan.anchor[0] + c.lo[0] * plan.voxel_mm) / 1000.0;
  p.y = static_cast<f64>(plan.anchor[1] + c.lo[1] * plan.voxel_mm) / 1000.0;
  p.z = static_cast<f64>(plan.anchor[2] + c.lo[2] * plan.voxel_mm) / 1000.0;
  return p;
}

}  // namespace engine::caves
