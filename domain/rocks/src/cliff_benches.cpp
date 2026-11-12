// A tier's members, stepped (cliff_benches.h; rocks.md, "Benches").
#include "cliff_benches.h"

#include <cmath>

namespace engine::rocks::detail {

namespace {

struct Member {
  f64 lo = 0.0;
  f64 hi = 0.0;
  bool weak = false;
};

}  // namespace

bool bench_beds(const scene::Formation& formation, BenchBeds& out) {
  out.bedded = false;
  // Beds that dip more steeply than 20 degrees do not run along a face (the tiers' own rule).
  if (!(std::fabs(static_cast<f64>(formation.dip_deg)) <= 20.0)) return false;
  out.field = std::make_unique<gfx::FormationParams>();
  if (!gfx::pack_formation(formation, *out.field, nullptr) || out.field->beds.count == 0) {
    out.field.reset();
    return false;
  }
  out.frame = gfx::formation_frame(*out.field);
  out.ny = out.frame.normal[1];
  out.bedded = out.ny > 0.3;
  return out.bedded;
}

f64 BenchProfile::setback(f64 at) const {
  if (!(at > foot) || count == 0) return 0.0;
  if (at >= y[count]) return s[count] + batter * (at - y[count]);
  u32 lo = 0, hi = count;
  while (hi - lo > 1) {
    const u32 mid = (lo + hi) / 2;
    if (y[mid] <= at)
      lo = mid;
    else
      hi = mid;
  }
  const f64 span = y[lo + 1] - y[lo];
  const f64 t = span > 0.0 ? (at - y[lo]) / span : 0.0;
  return s[lo] + (s[lo + 1] - s[lo]) * t;
}

bool BenchProfile::on_bench(f64 at) const {
  if (!(at > foot) || !(at < top) || count == 0) return false;
  for (u32 k = 0; k < count; ++k)
    if (at < y[k + 1]) return weak[k] != 0;
  return false;
}

f64 BenchProfile::behind() const {
  f64 worst = 0.0;
  for (u32 k = 0; k <= count; ++k) {
    const f64 d = s[k] - batter * (y[k] - foot);
    worst = d > worst ? d : worst;
  }
  return worst;
}

void bench_profile(const BenchBeds& beds, f64 x, f64 z, f64 ux, f64 uz, f64 foot, f64 top,
                   f64 batter, BenchProfile& out) {
  out.count = 1;
  out.foot = foot;
  out.top = top;
  out.batter = batter;
  out.riser_run = batter;
  out.bench_run = batter;
  out.tread_m = 0.0;
  const f64 height = top - foot;
  out.y[0] = foot;
  out.y[1] = top > foot ? top : foot;
  out.s[0] = 0.0;
  out.s[1] = batter * (height > 0.0 ? height : 0.0);
  out.weak[0] = 0;
  if (!beds.bedded || beds.field == nullptr || !(height > 2.0 * k_member_min_m)) return;

  // The members the column crosses, bottom first: one formation query a bed boundary, the beds
  // read where the plane stands at that height.
  constexpr u32 k_raw = 256;
  Member raw[k_raw];
  u32 n = 0;
  f64 at = foot;
  while (at < top && n < k_raw) {
    const f64 back = batter * (at - foot);
    const f64 c =
        gfx::formation_coordinate(beds.frame, x + ux * back, at, z + uz * back).coordinate;
    const gfx::FormationBedSample bed = gfx::formation_bed(beds.field->beds, c);
    f64 next = at + (bed.top - c) / beds.ny;
    if (!(next > at + 0.01)) next = at + 0.01;
    if (next > top || n + 1 == k_raw) next = top;
    const bool weak = !(bed.resistance >= gfx::k_formation_cliff_resistance);
    if (n > 0 && raw[n - 1].weak == weak) {
      raw[n - 1].hi = next;
    } else {
      raw[n].lo = at;
      raw[n].hi = next;
      raw[n].weak = weak;
      ++n;
    }
    at = next;
  }
  // A member thinner than `k_member_min_m` takes its lower neighbour's class (the lowest its upper
  // neighbour's), and neighbours of one class merge.
  for (u32 k = 0; k < n; ++k) {
    if (raw[k].hi - raw[k].lo >= k_member_min_m - 1e-6) continue;
    if (k > 0)
      raw[k].weak = raw[k - 1].weak;
    else if (n > 1)
      raw[k].weak = raw[1].weak;
  }
  Member members[k_raw];
  u32 m = 0;
  for (u32 k = 0; k < n; ++k) {
    if (m > 0 && members[m - 1].weak == raw[k].weak) {
      members[m - 1].hi = raw[k].hi;
    } else {
      members[m++] = raw[k];
    }
  }
  f64 hard_h = 0.0, weak_h = 0.0, treads = 0.0;
  for (u32 k = 0; k < m; ++k) {
    const f64 t = members[k].hi - members[k].lo;
    if (members[k].weak) {
      weak_h += t;
      if (k > 0) {
        f64 w = k_tread_share * t;
        w = w < k_tread_min_m ? k_tread_min_m : (w > k_tread_max_m ? k_tread_max_m : w);
        treads += w;
      }
    } else {
      hard_h += t;
    }
  }
  if (!(weak_h > 0.0) || !(hard_h > 0.0)) return;  // one class: the plane

  // The runs: a riser at 80 degrees, a bench at 35, the treads as wide as the design says; then
  // made to add up to the plane's run, so the face's chord is its plane's. Too little run: the
  // treads go first, then the benches steepen to 60 degrees, then the risers to plumb. Too much:
  // the treads widen (a gentler ground leaves wider benches), else the benches lie back to 30
  // degrees, else the risers lean.
  const f64 run = batter * height;
  f64 riser = k_riser_run, bench = k_bench_run, tread_scale = 1.0;
  const f64 need = riser * hard_h + bench * weak_h + treads;
  if (need <= run) {
    f64 extra = run - need;
    if (treads > 0.0) {
      tread_scale = (treads + extra) / treads;
    } else {
      const f64 room = (k_bench_run_max - bench) * weak_h;
      if (extra <= room) {
        bench += extra / weak_h;
        extra = 0.0;
      } else {
        bench = k_bench_run_max;
        extra -= room;
        riser += extra / hard_h;
      }
    }
  } else {
    f64 over = need - run;
    if (treads >= over) {
      tread_scale = treads > 0.0 ? (treads - over) / treads : 1.0;
      over = 0.0;
    } else {
      tread_scale = 0.0;
      over -= treads;
    }
    if (over > 0.0) {
      const f64 room = (bench - k_bench_run_min) * weak_h;
      if (over <= room) {
        bench -= over / weak_h;
        over = 0.0;
      } else {
        bench = k_bench_run_min;
        over -= room;
      }
    }
    if (over > 0.0) {
      const f64 room = riser * hard_h;
      if (over <= room) {
        riser -= over / hard_h;
        over = 0.0;
      } else {
        riser = 0.0;
        over -= room;
      }
    }
    if (over > 0.0) bench = bench - over / weak_h > 0.0 ? bench - over / weak_h : 0.0;
  }
  out.riser_run = riser;
  out.bench_run = bench;

  // The segments: a riser a member; a bench a member, its first `k_tread_rise_m` (at most half of
  // it) the tread.
  u32 count = 0;
  f64 s = 0.0;
  out.y[0] = foot;
  out.s[0] = 0.0;
  for (u32 k = 0; k < m && count + 2 <= k_bench_segments; ++k) {
    const f64 lo = members[k].lo, hi = members[k].hi, t = hi - lo;
    if (members[k].weak && k > 0 && tread_scale > 0.0) {
      f64 w = k_tread_share * t;
      w = (w < k_tread_min_m ? k_tread_min_m : (w > k_tread_max_m ? k_tread_max_m : w)) *
          tread_scale;
      const f64 rise = k_tread_rise_m < 0.5 * t ? k_tread_rise_m : 0.5 * t;
      s += w + bench * rise;
      out.tread_m = w > out.tread_m ? w : out.tread_m;
      out.weak[count] = 1;
      out.y[++count] = lo + rise;
      out.s[count] = s;
      s += bench * (t - rise);
      out.weak[count] = 1;
      out.y[++count] = hi;
      out.s[count] = s;
      continue;
    }
    s += (members[k].weak ? bench : riser) * t;
    out.weak[count] = members[k].weak ? 1u : 0u;
    out.y[++count] = hi;
    out.s[count] = s;
  }
  if (out.y[count] < top) {  // past the segments' room: the rest leans as the plane would
    out.weak[count] = 0;
    out.y[count + 1] = top;
    out.s[count + 1] = s + batter * (top - out.y[count]);
    ++count;
  }
  out.count = count;
}

}  // namespace engine::rocks::detail
