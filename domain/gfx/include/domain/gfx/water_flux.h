#pragma once

// **The energy flux of a body's waves, as fields** (roadmap S102, second pass; docs/subsystems/
// water.md, "The surf"; the grounding and the checks, docs/experiments/shore-mechanics.md, "What
// the second pass built"). The first pass gave a body's train a direction and an arrival at every
// place (`water_arrival_field`); its height was still a closed form of the depth under the pixel,
// H0 Ks(d), so a headland stood in the surf the open beach has, its lee too, and a wave that had
// broken on a bar was whole again behind it. Here the height follows the **flux of energy along
// the train's rays**: E cg is kept between two rays until something takes it,
//
//   div(E cg s) = -D,   s = grad(tau) / |grad(tau)|  (the way the train travels, from its arrival)
//
// with E = rho g H^2 / 8 and D what breaking takes. Three fields come out of one sweep in the order
// the train arrives:
//
//   - `incident`: the square root of the flux with nothing taken, over the unrefracted flux — the
//     refraction coefficient Kr = sqrt(b0 / b) where rays converge (a point) or spread (a bay), and
//     what the scheme's own diffusion lets round a headland into its lee. The height a place's wave
//     would have unbroken is H0 Ks(d) incident. On the bank it is the nearest water's: the
//     deep-water height coastal engineering calls H0' = Kr H0, which that stretch's run-up takes.
//   - `kept`: the square root of the flux breaking left, over that one, in (0, 1]: Battjes and
//     Janssen's (1978) bore dissipation D = (alpha / 4) rho g f Qb Hm^2 with Hm = gamma d and
//     alpha = 1, **the fraction breaking Qb counted over the train's own group of seven waves**
//     (each wave of the group breaks where its own height reaches gamma d, as the shader draws it)
//     in place of their truncated Rayleigh distribution, which a regular group does not have. A
//     wave that broke on a bar carries what it kept across the trough behind, and breaks again only
//     where the depth asks.
//   - `setup`: the mean level's rise, metres — Longuet-Higgins and Stewart's (1962, 1964) balance
//     of the radiation stress along the ray, d(eta)/ds = -(1 / (d + eta)) d(S / rho g)/ds with
//     S / rho g = E' (2 n - 1/2), E' the energy of the group **as drawn** (each wave capped at
//     gamma d): a set-down of -E' k / sinh(2 k d) outside the surf, a rise through it.
//
// The scheme is the donor-cell one (first-order upwind, conservative): a texel's flux leaves
// through the faces its travel direction points out of and enters from the texels upwind of it,
// the direction on a face the mean of the two texels', so a plane beach's Kr telescopes exactly
// (the flux through each line of faces is the line before's). First order means numerical
// diffusion across the rays, about sqrt(h x) wide after x: a lee's edge is soft, as diffraction
// makes it. Deterministic: a fixed order (the arrival's, ties by index) in double.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/water_spectrum.h>
#include <domain/gfx/water_swash.h>

#include <algorithm>
#include <cmath>

namespace engine::gfx {

// Battjes and Janssen's (1978) alpha, of order one: 1.
inline constexpr f64 k_water_breaking_alpha = 1.0;
// A ratio the field stores is never under this: 0 marks a field that carries none.
inline constexpr f64 k_water_ratio_floor = 0.02;
// A wave of the group has broken where its height over gamma d passes from 0.9 to 1.05
// (water.slang, `water_bore_foam`: the same smoothstep).
inline constexpr f64 k_water_break_lo = 0.9;
inline constexpr f64 k_water_break_hi = 1.05;

// The root mean square of the group's shape: the group's rms height over its largest wave's.
inline f64 water_group_rms() noexcept {
  f64 sum = 0.0;
  for (u32 j = 0; j < k_swash_group; ++j)
    sum += swash_group(j) * swash_group(j);
  return std::sqrt(sum / static_cast<f64>(k_swash_group));
}

// **The fraction of the group breaking** where its largest wave's uncapped height is `ratio` of
// gamma d: wave j stands G(j) of it.
inline f64 water_group_breaking(f64 ratio) noexcept {
  f64 broken = 0.0;
  for (u32 j = 0; j < k_swash_group; ++j) {
    const f64 x = std::clamp(
        (ratio * swash_group(j) - k_water_break_lo) / (k_water_break_hi - k_water_break_lo), 0.0,
        1.0);
    broken += x * x * (3.0 - 2.0 * x);
  }
  return broken / static_cast<f64>(k_swash_group);
}

// The group's energy as drawn, over rho g: the mean over its waves of min(G(j) h, limit)^2 / 8.
inline f64 water_group_energy(f64 h, f64 limit) noexcept {
  f64 sum = 0.0;
  for (u32 j = 0; j < k_swash_group; ++j) {
    const f64 hj = std::min(swash_group(j) * h, limit);
    sum += hj * hj;
  }
  return sum / (8.0 * static_cast<f64>(k_swash_group));
}

// n = cg / c at wavenumber k and depth d: a half in deep water, 1 in shallow.
inline f64 water_group_ratio(f64 k, f64 d) noexcept {
  const f64 kd = k * std::max(d, 1e-3);
  return 2.0 * kd > 30.0 ? 0.5 : 0.5 * (1.0 + 2.0 * kd / std::sinh(2.0 * kd));
}

// Longuet-Higgins and Stewart's set-down under an unbroken wave whose energy over rho g is `e`:
// -e k / sinh(2 k d).
inline f64 water_set_down(f64 e, f64 k, f64 d) noexcept {
  const f64 kd2 = 2.0 * k * std::max(d, 1e-3);
  return kd2 > 30.0 ? 0.0 : -e * k / std::sinh(kd2);
}

// **The fields.** `depth` (signed, metres) and `arrival` (`water_arrival_field`'s, seconds) are
// size * size, rows along +z, and so are the three outputs. `height_m` is the sea's deep-water
// height, the group's largest wave. With no wave, or a map too small, the ratios are 1 and the
// setup 0: the closed form the shader had.
inline void water_flux_field(const f32* depth, const f32* arrival, u32 size, f64 texel_m,
                             f64 period_s, f64 height_m, f32* incident, f32* kept,
                             f32* setup) noexcept {
  const u32 count = size * size;
  for (u32 i = 0; i < count; ++i) {
    incident[i] = 1.0f;
    kept[i] = 1.0f;
    setup[i] = 0.0f;
  }
  if (size < 3 || !(period_s > 0.0) || !(texel_m > 0.0) || !(height_m > 0.0)) return;
  const f64 omega = 2.0 * 3.14159265358979323846 / period_s;
  const f64 k0 = omega * omega / k_swash_g;
  const f64 cg0 = 0.5 * k_swash_g / omega;
  const f64 rms = water_group_rms();
  const f64 e0 = rms * rms * height_m * height_m / 8.0;
  f64 deepest = 0.0;
  for (u32 i = 0; i < count; ++i)
    deepest = std::max(deepest, static_cast<f64>(depth[i]));
  if (!(deepest > 0.0)) return;
  // The arrival's own source: water at least 1 / k0 deep, or the deepest fifth.
  const f64 source = std::min(k_water_train_deep_kd / k0, 0.8 * deepest);
  const i64 n = size;
  const auto wet = [&](i64 a, i64 b) {
    return a >= 0 && b >= 0 && a < n && b < n && depth[b * n + a] > 0.0f;
  };
  const auto tau = [&](i64 a, i64 b) { return static_cast<f64>(arrival[b * n + a]); };
  // The way the train travels at each water texel: the arrival's gradient over the water beside
  // it (central where both neighbours are water, one-sided beside a wall), unit; zero where the
  // arrival is flat (a source with no direction).
  constexpr u8 k_dry = 0, k_water = 1, k_source = 2;
  Vector<f64> sx(count);
  Vector<f64> sz(count);
  Vector<u8> kind(count);
  Vector<u32> order;
  order.reserve(count);
  for (i64 j = 0; j < n; ++j) {
    for (i64 i = 0; i < n; ++i) {
      const u32 at = static_cast<u32>(j * n + i);
      sx[at] = 0.0;
      sz[at] = 0.0;
      const f64 d = static_cast<f64>(depth[at]);
      kind[at] = d > 0.0 ? (d >= source ? k_source : k_water) : k_dry;
      if (kind[at] == k_dry) continue;
      if (kind[at] == k_water) order.push_back(at);
      f64 gx = 0.0;
      f64 gz = 0.0;
      const bool xl = wet(i - 1, j);
      const bool xr = wet(i + 1, j);
      if (xl && xr)
        gx = 0.5 * (tau(i + 1, j) - tau(i - 1, j));
      else if (xr)
        gx = tau(i + 1, j) - tau(i, j);
      else if (xl)
        gx = tau(i, j) - tau(i - 1, j);
      const bool zl = wet(i, j - 1);
      const bool zr = wet(i, j + 1);
      if (zl && zr)
        gz = 0.5 * (tau(i, j + 1) - tau(i, j - 1));
      else if (zr)
        gz = tau(i, j + 1) - tau(i, j);
      else if (zl)
        gz = tau(i, j) - tau(i, j - 1);
      const f64 len = std::sqrt(gx * gx + gz * gz);
      if (len > 1e-12) {
        sx[at] = gx / len;
        sz[at] = gz / len;
      }
    }
  }
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return arrival[a] != arrival[b] ? arrival[a] < arrival[b] : a < b;
  });
  // q: the flux over the unrefracted, unbroken one (e0 cg0, the same at every depth: Ks^2 cg is
  // cg0), with breaking; qn: with nothing taken. A source texel carries 1 and its set-down.
  Vector<f64> q(count);
  Vector<f64> qn(count);
  Vector<f64> eta(count);
  Vector<f64> stress(count);
  Vector<u8> done(count);
  // The radiation stress along the ray and the set-down of the group as drawn at a texel whose
  // flux is `flux`.
  const auto local = [&](u32 at, f64 flux, f64& stress_out, f64& set_down) {
    const f64 d = std::max(static_cast<f64>(depth[at]), k_water_celerity_floor_m);
    const f64 k = water_wavenumber(k0, d);
    const f64 e =
        water_group_energy(height_m * water_shoaling(k, d) * std::sqrt(std::max(flux, 0.0)),
                           k_water_breaking_gamma * d);
    stress_out = e * (2.0 * water_group_ratio(k, d) - 0.5);
    set_down = water_set_down(e, k, d);
  };
  for (u32 at = 0; at < count; ++at) {
    q[at] = 0.0;
    qn[at] = 0.0;
    eta[at] = 0.0;
    stress[at] = 0.0;
    done[at] = 0;
    if (kind[at] != k_source) continue;
    q[at] = 1.0;
    qn[at] = 1.0;
    local(at, 1.0, stress[at], eta[at]);
    done[at] = 1;
  }
  constexpr i64 k_di[4] = {1, -1, 0, 0};
  constexpr i64 k_dj[4] = {0, 0, 1, -1};
  constexpr f64 k_flux_max = k_water_incident_max * k_water_incident_max;
  const f64 frequency = 1.0 / period_s;
  for (u32 o = 0; o < order.size(); ++o) {
    const u32 at = order[o];
    const i64 i = at % size;
    const i64 j = at / size;
    const f64 d = static_cast<f64>(depth[at]);
    const bool moving = sx[at] != 0.0 || sz[at] != 0.0;
    // What a face carries, per unit of flux: the direction on it (the two texels' mean, or the
    // one that has a direction) along the face's outward normal. Positive: out of this texel.
    f64 flow[4];
    u32 beside[4];
    bool usable[4];
    f64 out = 0.0;   // what leaves, per unit of this texel's flux
    f64 own = 0.0;   // what enters through the map's border: the sea past it is this texel's own
    f64 in = 0.0;    // what enters from the texels upwind, with breaking
    f64 in_n = 0.0;  // and with nothing taken
    for (u32 f = 0; f < 4; ++f) {
      const i64 a = i + k_di[f];
      const i64 b = j + k_dj[f];
      const bool inside = a >= 0 && b >= 0 && a < n && b < n;
      beside[f] = inside ? static_cast<u32>(b * n + a) : at;
      const bool water = inside && kind[beside[f]] != k_dry;
      f64 fx = sx[at];
      f64 fz = sz[at];
      if (water && (sx[beside[f]] != 0.0 || sz[beside[f]] != 0.0)) {
        fx = moving ? 0.5 * (fx + sx[beside[f]]) : sx[beside[f]];
        fz = moving ? 0.5 * (fz + sz[beside[f]]) : sz[beside[f]];
      }
      flow[f] = fx * static_cast<f64>(k_di[f]) + fz * static_cast<f64>(k_dj[f]);
      usable[f] = false;
      if (flow[f] > 0.0) {
        // Out: to water the train reaches later, onto the bank (absorbed there), or off the map.
        // **A face is crossed one way and counted on both sides or on neither**: what this texel
        // sends is what the texel beside it takes when its turn comes, so a face whose direction
        // points at water the train reached *earlier* — the arrival's order and the face's
        // direction disagree, where two fronts meet behind a headland — carries nothing either
        // way. Without this the sweep lost a part in a hundred of the flux behind a wall.
        if (!(water && done[beside[f]] != 0)) out += flow[f];
      } else if (!inside) {
        own -= flow[f];
      } else if (water && done[beside[f]] != 0) {
        usable[f] = flow[f] < 0.0;  // a wall, or a texel the train reaches later, sends nothing
        in -= flow[f] * q[beside[f]];
        in_n -= flow[f] * qn[beside[f]];
      }
    }
    const f64 through = out - own;
    f64 flux = 0.0;
    f64 flux_n = 0.0;
    if (through > 0.05) {
      flux = std::min(in / through, k_flux_max);
      flux_n = std::min(in_n / through, k_flux_max);
    } else if (own > 0.0) {
      flux = 1.0;  // fed by the border alone: the sea as it comes
      flux_n = 1.0;
    }
    // Breaking: q through + kappa Qb(crest sqrt(q)) = in, the left side rising with q: bisection.
    const f64 depth_here = std::max(d, k_water_celerity_floor_m);
    const f64 limit = k_water_breaking_gamma * depth_here;
    // The largest wave's uncapped height over gamma d, per unit of sqrt(flux).
    const f64 crest =
        height_m * water_shoaling(water_wavenumber(k0, depth_here), depth_here) / limit;
    if (flux > 0.0 && through > 0.05 && water_group_breaking(crest * std::sqrt(flux)) > 0.0) {
      const f64 kappa =
          texel_m * (k_water_breaking_alpha / 4.0) * frequency * limit * limit / (e0 * cg0);
      f64 lo = 0.0;
      f64 hi = flux;
      for (u32 it = 0; it < 40; ++it) {
        const f64 mid = 0.5 * (lo + hi);
        if (mid * through + kappa * water_group_breaking(crest * std::sqrt(mid)) > in)
          hi = mid;
        else
          lo = mid;
      }
      flux = 0.5 * (lo + hi);
    }
    q[at] = flux;
    qn[at] = flux_n;
    // The mean level: each upwind texel's, less the radiation stress's rise from it to here over
    // the depth between, weighted by the flux it sends.
    f64 set_down = 0.0;
    local(at, flux, stress[at], set_down);
    f64 sum = 0.0;
    f64 weights = 0.0;
    for (u32 f = 0; f < 4; ++f) {
      if (!usable[f]) continue;
      const u32 there = beside[f];
      const f64 weight = -flow[f] * q[there];
      if (!(weight > 0.0)) continue;
      const f64 between = std::max(0.5 * (d + static_cast<f64>(depth[there])) + eta[there], 0.05);
      sum += weight * (eta[there] - (stress[at] - stress[there]) / between);
      weights += weight;
    }
    eta[at] = weights > 0.0 ? sum / weights : set_down;
    done[at] = 1;
  }
  // The bank takes the nearest water's numbers: a stretch of beach has its own shoreline's
  // incident height and its setup.
  for (u32 at = 0; at < count; ++at) {
    const f64 share = qn[at] > 0.0 ? std::sqrt(std::min(q[at] / qn[at], 1.0)) : 1.0;
    qn[at] = std::sqrt(qn[at]);
    q[at] = share;
  }
  Vector<u32> distance(count);
  const auto carry = [&](Vector<f64>& value, f32* to, f64 lo, f64 hi) {
    for (u32 at = 0; at < count; ++at)
      distance[at] = kind[at] != k_dry ? 0u : k_water_carry_none;
    water_carry_nearest(size, value, distance);
    for (u32 at = 0; at < count; ++at)
      to[at] = static_cast<f32>(std::clamp(value[at], lo, hi));
  };
  carry(qn, incident, k_water_ratio_floor, k_water_incident_max);
  carry(q, kept, k_water_ratio_floor, 1.0);
  carry(eta, setup, -0.1 * height_m, 0.5 * height_m);
}

// **A second train** (S102, second pass; water.md, "The surf"). One regular train is a ruled sea:
// every crest unbroken along the bay and each wave the last one's twin. A wind sea's energy is
// spread over directions and frequencies, and its smallest honest picture past one train is two:
// the peak, and a component at 8/7 of the peak's frequency from the direction the spreading puts
// half its energy within there. Eight of its waves pass in the peak's seven, so the pair beats
// once a group — the sets a sea comes in — and since the two cross at an angle the beat's place
// along a crest differs from crest to crest: crests of finite length, no two alike, and a wave
// that is highest, and breaks first, where the two coincide.
//
// `water_cross_of_sea`: a wind sea of height `height_m` and peak period `period_s` (on the water's
// clock) split into the two by JONSWAP's own ratio of the spectrum at 8/7 of the peak to the
// peak's (0.37 at gamma 3.3), the second turned by the half width at half maximum of Donelan and
// Banner's spreading there (sech^2(beta theta) is a half where cosh(beta theta) = sqrt 2: 26
// degrees), to the side `seed` picks.
// `water_cross_of_swell`: a swell is the train; the wind sea's peak wave is the second, its period
// moved onto the group (a whole number of its waves in the swell's seven).
inline constexpr u32 k_water_cross_waves = 8;  // of the second train, in the first's group of seven
struct WaterCross {
  f64 train_m = 0.0;   // the first train's deep-water height
  f64 cross_m = 0.0;   // the second's; 0: none
  f64 period_s = 0.0;  // the second's period
  Vec2 direction{};    // the way the second travels over deep water (unit)
  f64 height_m = 0.0;  // the two together, sqrt(H1^2 + H2^2): what the flux is taken for
};

inline WaterCross water_cross_of_sea(f64 height_m, f64 period_s, Vec2 direction,
                                     u32 seed) noexcept {
  WaterCross c;
  c.train_m = height_m;
  c.height_m = height_m;
  if (!(height_m > 0.0) || !(period_s > 0.0)) return c;
  const f64 r = static_cast<f64>(k_water_cross_waves) / static_cast<f64>(k_swash_group);
  const f64 peak = 2.0 * 3.14159265358979323846 / period_s;
  const f64 share = jonswap(peak * r, 1.0, peak) / jonswap(peak, 1.0, peak);
  c.train_m = height_m / std::sqrt(1.0 + share);
  c.cross_m = height_m * std::sqrt(share / (1.0 + share));
  c.period_s = period_s / r;
  const f64 turn =
      std::acosh(std::sqrt(2.0)) / donelan_banner_beta(r) * ((seed & 1u) != 0 ? 1.0 : -1.0);
  const f64 cs = std::cos(turn);
  const f64 sn = std::sin(turn);
  const f64 dx = static_cast<f64>(direction.x);
  const f64 dz = static_cast<f64>(direction.y);
  c.direction = Vec2{static_cast<f32>(dx * cs - dz * sn), static_cast<f32>(dx * sn + dz * cs)};
  return c;
}

inline WaterCross water_cross_of_swell(f64 swell_m, f64 swell_period_s, f64 wind_m,
                                       f64 wind_period_s, Vec2 wind) noexcept {
  WaterCross c;
  c.train_m = swell_m;
  c.height_m = swell_m;
  if (!(wind_m > 0.0) || !(wind_period_s > 0.0) || !(swell_period_s > 0.0)) return c;
  const f64 group = static_cast<f64>(k_swash_group);
  const f64 waves = std::max(1.0, std::floor(group * swell_period_s / wind_period_s + 0.5));
  c.cross_m = wind_m;
  c.period_s = group * swell_period_s / waves;
  c.direction = wind;
  c.height_m = std::sqrt(swell_m * swell_m + wind_m * wind_m);
  return c;
}

// ---- the healed front (W152-2; water.md, "The front heals"; the grounding, docs/experiments/
// shore-mechanics.md, "The front heals at a wavelength")
// ----------------------------------------------------------------------------------------------
//
// The arrival field is a first-arrival eikonal: ray theory, which draws every kink where two paths'
// fronts meet, every corner of the waterline's texels and every closed front round a place reached
// last as sharply as the grid can, and the bank, which takes its nearest waterline texel's arrival,
// as cells with steps between them of whole periods. A real front is smooth at the scale of its
// wavelength because diffraction fills what ray theory leaves sharp: a front's wrinkle of
// half-width L heals once the wave has run x > pi L^2 / lambda past it (Nolet and Dahlen 2000,
// "wave front healing"), so after a path of a wavelength nothing narrower than about lambda / 2 is
// left; SWAN's refraction-diffraction smooths its fields over 0.2 to 0.4 of a wavelength for the
// same reason (Holthuijsen, Herman and Booij 2003). Read at a texel and through a spline, a step of
// several periods between two of the bank's cells, or a kink, is several crests crowded into a
// texel: the rings in the swash's sheets and the fans of fine lines behind the bores.
//
// So the field is **healed**: every channel the front carries — the arrival, the second train's,
// and the flux's `incident`, `kept` and `setup` — is averaged over the water round each texel with
// a Gaussian of `k_water_heal_share` of the local wavelength, the wavelength never that of water
// shallower than the depth the group's largest wave breaks in (its front ashore is the bore's,
// shaped at the break line). The bank takes the same average of the water near it (a normalized
// average, over the water only, coarser where no water is near), so its arrival is continuous with
// the waterline's and steps of whole periods are gone. A plane wave is left as it was (the average
// of a linear field is itself, and the map's edges reflect it oddly). Levels a power of two apart,
// each three box passes of running sums, blended by the texel's own scale: linear in the texels
// times the levels. Deterministic: a fixed order in double.
inline constexpr f64 k_water_heal_share = 0.5;
// A level whose average saw less than this share of water takes the coarser level's value.
inline constexpr f64 k_water_heal_full = 0.25;

namespace detail {
// How a line of the map is padded past one of its ends (W152-3): by what the world past the edge is
// taken to be.
enum class WaterHealPad : u8 {
  nothing,  // the line crosses the shore and ends on the bank: the bank goes on, no water past it
  water,    // the line ends in water: the water goes on (the arrival linearly)
  mirror,   // the line is bank throughout: the bank goes on as it is along the edge
};

// Three box passes of radius r — a Gaussian — along a line of n texels at `stride`, of a weighted
// field `w` (the water's mass times its value) and its mass `m` together, padded past each end as
// `first` and `last` say. Past water the mass reflects evenly and the weighted field evenly too, or
// with `odd` (an arrival) as the mirrored mass times the value reflected oddly about the end's value
// w / m, which keeps a plane wave's arrival linear; a bank line's padding is its mirror; past a
// shore line's bank end the line runs on 3r texels of nothing, so what the passes spread past the
// edge comes back as it would over the bank that is there. Until W152-3 the weighted product
// itself was reflected oddly about its end, which on a bank edge (`w` 0) mirrored minus the water's
// arrivals — the band basin of swash_scene_tests.cpp, every edge of its map bank, healed its swash
// to 4 s early on average and scrambled it within a metre, and the test bay's 30 s swell, healed
// over half its map, came ashore 20 s early at its ends — and a mirror of the mass at such an edge
// would have stood the water up again past it, on ground that is bank.
inline void water_heal_line(f64* w, f64* m, u32 n, u32 stride, u32 r, bool odd, WaterHealPad first,
                            WaterHealPad last, Vector<f64>& lw, Vector<f64>& lm, Vector<f64>& sw,
                            Vector<f64>& sm) {
  if (r == 0 || n < 2) return;
  // The line, run on past a bank end by the three passes' reach.
  const u32 head = first == WaterHealPad::nothing ? 3 * r : 0;
  const u32 tail = last == WaterHealPad::nothing ? 3 * r : 0;
  const u32 len = head + n + tail;
  r = std::min(r, len - 1);
  lw.assign(len, 0.0);
  lm.assign(len, 0.0);
  for (u32 i = 0; i < n; ++i) {
    lw[head + i] = w[static_cast<u64>(i) * stride];
    lm[head + i] = m[static_cast<u64>(i) * stride];
  }
  sw.resize(len + 2 * r + 1);
  sm.resize(len + 2 * r + 1);
  for (u32 pass = 0; pass < 3; ++pass) {
    // Past a run-on end, nothing; past water, its mirror (the arrival's value oddly); past a bank
    // line's end, its mirror.
    const bool reflect_first = first != WaterHealPad::nothing;
    const bool reflect_last = last != WaterHealPad::nothing;
    const bool odd_first = odd && first == WaterHealPad::water && lm[0] > 1e-9;
    const bool odd_last = odd && last == WaterHealPad::water && lm[len - 1] > 1e-9;
    const f64 v_first = odd_first ? lw[0] / lm[0] : 0.0;
    const f64 v_last = odd_last ? lw[len - 1] / lm[len - 1] : 0.0;
    for (u32 i = 0; i < r; ++i) {
      const u32 h = std::min(r - i, len - 1);
      const u32 t = len >= 2 + i ? len - 2 - i : 0;
      sm[i + 1] = reflect_first ? lm[h] : 0.0;
      sm[len + r + i + 1] = reflect_last ? lm[t] : 0.0;
      sw[i + 1] = !reflect_first ? 0.0 : odd_first ? 2.0 * v_first * lm[h] - lw[h] : lw[h];
      sw[len + r + i + 1] = !reflect_last ? 0.0 : odd_last ? 2.0 * v_last * lm[t] - lw[t] : lw[t];
    }
    for (u32 i = 0; i < len; ++i) {
      sw[r + i + 1] = lw[i];
      sm[r + i + 1] = lm[i];
    }
    // Running sums in place: s[k] = sum of the padded values before k.
    sw[0] = 0.0;
    sm[0] = 0.0;
    for (u32 k = 1; k < len + 2 * r + 1; ++k) {
      sw[k] += sw[k - 1];
      sm[k] += sm[k - 1];
    }
    const f64 width = static_cast<f64>(2 * r + 1);
    for (u32 i = 0; i < len; ++i) {
      lw[i] = (sw[i + 2 * r + 1] - sw[i]) / width;
      lm[i] = (sm[i + 2 * r + 1] - sm[i]) / width;
    }
  }
  for (u32 i = 0; i < n; ++i) {
    w[static_cast<u64>(i) * stride] = lw[head + i];
    m[static_cast<u64>(i) * stride] = lm[head + i];
  }
}
// How each row's and column's two ends are padded, from the map's own depth: 4 * size entries,
// rows' first and last ends, then columns'.
inline void water_heal_pads(const f32* depth, u32 size, Vector<WaterHealPad>& pads) {
  pads.resize(4 * static_cast<u64>(size));
  const auto pad = [](bool end_water, bool any_water) {
    return end_water ? WaterHealPad::water
                     : (any_water ? WaterHealPad::nothing : WaterHealPad::mirror);
  };
  for (u32 j = 0; j < size; ++j) {
    bool any = false;
    for (u32 i = 0; i < size && !any; ++i)
      any = depth[static_cast<u64>(j) * size + i] > 0.0f;
    pads[2 * j] = pad(depth[static_cast<u64>(j) * size] > 0.0f, any);
    pads[2 * j + 1] = pad(depth[static_cast<u64>(j) * size + size - 1] > 0.0f, any);
  }
  for (u32 i = 0; i < size; ++i) {
    bool any = false;
    for (u32 j = 0; j < size && !any; ++j)
      any = depth[static_cast<u64>(j) * size + i] > 0.0f;
    pads[2 * size + 2 * i] = pad(depth[i] > 0.0f, any);
    pads[2 * size + 2 * i + 1] = pad(depth[static_cast<u64>(size - 1) * size + i] > 0.0f, any);
  }
}

// A Gaussian of `sigma` texels over a size * size weighted field and its mass: along every row, then
// along every column, each line padded as `pads` says.
inline void water_heal_blur(Vector<f64>& weighted, Vector<f64>& mass,
                            const Vector<WaterHealPad>& pads, u32 size, f64 sigma, bool odd,
                            Vector<f64>& scratch) {
  const u32 r = static_cast<u32>(std::lround((std::sqrt(4.0 * sigma * sigma + 1.0) - 1.0) / 2.0));
  Vector<f64> lw, lm, sm;
  for (u32 j = 0; j < size; ++j) {
    const u64 row = static_cast<u64>(j) * size;
    water_heal_line(weighted.data() + row, mass.data() + row, size, 1, r, odd, pads[2 * j],
                    pads[2 * j + 1], lw, lm, scratch, sm);
  }
  for (u32 i = 0; i < size; ++i) {
    water_heal_line(weighted.data() + i, mass.data() + i, size, size, r, odd,
                    pads[2 * size + 2 * i], pads[2 * size + 2 * i + 1], lw, lm, scratch, sm);
  }
}}  // namespace detail

// **Heals one field** in place (`values`, size * size, rows along +z) over the water of `depth`:
// each texel its normalized Gaussian average over the water at k_water_heal_share of the
// wavelength of the train (deep-water wavenumber `k0`) at its depth, never shallower than
// `floor_m`; `linear` reflects the map's edges oddly (an arrival), else evenly (a ratio, a level).
// Returns nothing for a map under 3 a side or no water.
inline void water_heal_field(f32* values, const f32* depth, u32 size, f64 texel_m, f64 k0,
                             f64 floor_m, bool linear) {
  if (size < 3 || !(texel_m > 0.0) || !(k0 > 0.0)) return;
  const u32 count = size * size;
  Vector<f64> scale(count);  // the texel's level position: < 1 between itself and a texel
  f64 widest = 0.0;
  bool any = false;
  for (u32 i = 0; i < count; ++i) {
    const f64 d = std::max({static_cast<f64>(depth[i]), floor_m, k_water_celerity_floor_m});
    const f64 sigma =
        k_water_heal_share * 2.0 * 3.14159265358979323846 / water_wavenumber(k0, d) / texel_m;
    widest = std::max(widest, sigma);
    scale[i] = sigma < 1.0 ? sigma : 1.0 + std::log2(sigma);
    any = any || depth[i] > 0.0f;
  }
  if (!any) return;
  // The levels: 0 the texel itself, l >= 1 a Gaussian of 2^(l-1) texels, to the widest asked.
  u32 levels = 1;
  while (levels < 12 && std::ldexp(1.0, static_cast<i32>(levels) - 1) < widest)
    ++levels;
  Vector<f64> scratch;
  Vector<detail::WaterHealPad> pads;
  detail::water_heal_pads(depth, size, pads);
  Vector<f64> weighted(count);
  Vector<f64> mass(count);
  Vector<f64> coarse(count);  // the composite of the levels above the one in hand
  Vector<f64> fine(count);
  Vector<f64> out(count);
  for (u32 i = 0; i < count; ++i)
    out[i] = static_cast<f64>(values[i]);
  for (u32 level = levels; level-- > 0;) {
    // This level's average over the water and how much water it saw.
    for (u32 i = 0; i < count; ++i) {
      mass[i] = depth[i] > 0.0f ? 1.0 : 0.0;
      weighted[i] = mass[i] * static_cast<f64>(values[i]);
    }
    if (level > 0) {
      const f64 sigma = std::ldexp(1.0, static_cast<i32>(level) - 1);
      detail::water_heal_blur(weighted, mass, pads, size, sigma, linear, scratch);
    }
    for (u32 i = 0; i < count; ++i) {
      const f64 v = mass[i] > 1e-9 ? weighted[i] / mass[i] : 0.0;
      const f64 a = std::clamp(mass[i] / k_water_heal_full, 0.0, 1.0);
      fine[i] = level + 1 == levels ? (mass[i] > 1e-9 ? v : static_cast<f64>(values[i]))
                                    : a * v + (1.0 - a) * coarse[i];
    }
    // A texel whose scale lies between this level and the next takes the blend of the two.
    for (u32 i = 0; i < count; ++i) {
      const f64 x = std::min(scale[i], static_cast<f64>(levels - 1));
      if (level + 1 == levels) {
        if (x >= static_cast<f64>(level)) out[i] = fine[i];
      } else if (x >= static_cast<f64>(level) && x < static_cast<f64>(level + 1)) {
        const f64 w = x - static_cast<f64>(level);
        out[i] = fine[i] * (1.0 - w) + coarse[i] * w;
      }
    }
    std::swap(coarse, fine);
  }
  for (u32 i = 0; i < count; ++i)
    values[i] = static_cast<f32>(out[i]);
}

// **A body's field, whole** (`WaterSurface::field`; `k_water_field_channels` floats a texel): the
// depth decoded from the map's 16 bits as the GPU's image is, the first train's arrival, the grown
// share of a closed body's fetch, each texel's own stretch of shore's slope, and the second pass's
// four — the second train's arrival and the flux's fields, taken for the two trains together along
// the first's rays (the second shares the first's refraction and its lee: a field of its own is
// eight more bytes a texel). Sets the surface's second-pass members from `cross`.
struct WaterFieldDesc {
  const u16* texels = nullptr;  // the signed depth map
  u32 size = 0;
  f64 texel_m = 0.0;
  f64 depth_max = 0.0;
  f64 bank_max = 0.0;
  f64 period_s = 0.0;     // the first train's, on the water's clock
  Vec2 direction{};       // the way it travels over deep water
  f64 fetch_m = 0.0;      // a closed body's: the fetch its table was grown at; 0: grown everywhere
  Vec2 wind{1.0f, 0.0f};  // and its wind (unit)
};

inline void water_surface_field(const WaterFieldDesc& d, const WaterCross& cross, WaterSurface& out,
                                Vector<f32>& field) {
  const u32 count = d.size * d.size;
  Vector<f32> depth(count);
  const f64 range = d.depth_max + d.bank_max;
  for (u32 i = 0; i < count; ++i)
    depth[i] = static_cast<f32>(static_cast<f64>(d.texels[i]) / 65535.0 * range - d.bank_max);
  Vector<f32> arrival(count);
  water_arrival_field(depth.data(), d.size, d.texel_m, d.period_s, arrival.data(), 3, d.direction);
  // The front heals at its wavelength (W152-2), never that of water shallower than the group's
  // largest wave breaks in; the flux is taken along the healed rays and healed the same way.
  const f64 two_pi = 2.0 * 3.14159265358979323846;
  const f64 k0 = d.period_s > 0.0 ? (two_pi / d.period_s) * (two_pi / d.period_s) / k_swash_g : 0.0;
  const f64 heal_floor = cross.height_m / k_water_breaking_gamma;
  water_heal_field(arrival.data(), depth.data(), d.size, d.texel_m, k0, heal_floor, true);
  Vector<f32> shore_slope(count);
  water_shore_slope_field(depth.data(), d.size, d.texel_m, shore_slope.data());
  // The share of the body's fetch the wind has blown over at each texel (a closed body's lee): the
  // fetch field over the fetch the table was grown at. An open body's sea comes in grown.
  Vector<f32> grown(count);
  if (d.fetch_m > 0.0) {
    water_fetch_field(depth.data(), d.size, d.texel_m, d.wind, grown.data());
    for (u32 i = 0; i < count; ++i)
      grown[i] = static_cast<f32>(std::clamp(static_cast<f64>(grown[i]) / d.fetch_m, 0.0, 1.0));
  } else {
    for (u32 i = 0; i < count; ++i)
      grown[i] = 1.0f;
  }
  Vector<f32> second(count);
  for (u32 i = 0; i < count; ++i)
    second[i] = 0.0f;
  out.train_m = static_cast<f32>(cross.train_m);
  out.cross_m = 0.0f;
  out.cross_period_s = 0.0f;
  out.cross_k0 = 0.0f;
  if (cross.cross_m > 0.0 && cross.period_s > 0.0) {
    water_arrival_field(depth.data(), d.size, d.texel_m, cross.period_s, second.data(), 3,
                        cross.direction);
    const f64 omega = 2.0 * 3.14159265358979323846 / cross.period_s;
    water_heal_field(second.data(), depth.data(), d.size, d.texel_m, omega * omega / k_swash_g,
                     heal_floor, true);
    out.cross_m = static_cast<f32>(cross.cross_m);
    out.cross_period_s = static_cast<f32>(cross.period_s);
    out.cross_k0 = static_cast<f32>(omega * omega / k_swash_g);
  }
  Vector<f32> incident(count);
  Vector<f32> kept(count);
  Vector<f32> setup(count);
  water_flux_field(depth.data(), arrival.data(), d.size, d.texel_m, d.period_s, cross.height_m,
                   incident.data(), kept.data(), setup.data());
  // The flux's ray tubes end in straight cuts along the rays (a breaking stretch's ends, a lee's
  // edge sharper than the scheme's own diffusion leaves it): energy spreads across rays at the same
  // scale the front heals over (W152-2), so the three are healed with the arrival.
  water_heal_field(incident.data(), depth.data(), d.size, d.texel_m, k0, heal_floor, false);
  water_heal_field(kept.data(), depth.data(), d.size, d.texel_m, k0, heal_floor, false);
  water_heal_field(setup.data(), depth.data(), d.size, d.texel_m, k0, heal_floor, false);
  // The foreshore slope of a place's stretch of shore is its nearest waterline texel's, cells with
  // steps between them like the bank's arrival was: where the slope crosses the surging limit the
  // foam's strength (`water_foam_strength`) stepped with it, a breaking stretch ending in a
  // straight cut along a cell's edge (the forest staging's sea, 3.5 m texels). A stretch's slope
  // varies at the scale its wave does, so it is healed the same way.
  water_heal_field(shore_slope.data(), depth.data(), d.size, d.texel_m, k0, heal_floor, false);
  field.resize(k_water_field_channels * count);
  for (u32 i = 0; i < count; ++i) {
    f32* texel = field.data() + k_water_field_channels * i;
    texel[0] = depth[i];
    texel[1] = arrival[i];
    texel[2] = grown[i];
    texel[3] = shore_slope[i];
    texel[4] = second[i];
    texel[5] = incident[i];
    texel[6] = kept[i];
    texel[7] = setup[i];
  }
}

}  // namespace engine::gfx
