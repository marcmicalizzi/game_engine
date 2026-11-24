// Running water and the surf (world_field.h; S113; docs/subsystems/world_sound.md, "Running
// water and the surf"; the grounding, docs/experiments/world-sound.md, "Running water", "Surf"
// and "Read again by S113").
//
// A host's flow table sends the stretches of its solved channels and its surf table the stretches
// of shore where a body's train breaks; at a refresh each is a source whose power at the listener
// is the power the water gives up, a share of it heard in the air (`k_water_airborne_efficiency`,
// a stated choice), spread as a line near and a point far and carried through the same path as a
// crown's. The loudest of each kind are voices of their own; the rest are their sector's bed's,
// by power. A trickle is heard as separate bubbles struck on its voice's resonators.

#include "world_field.h"

#include <core/hash/hash.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/tunables/tunables.h>
#include <systems/world_sound/path.h>

#include <algorithm>
#include <cmath>

namespace engine::world_sound {

namespace {

tunables::Int world_streams{"audio.world.streams", 4, 0, 16,
                            "Stretches of running water heard as voices of their own"};
tunables::Int world_falls{"audio.world.falls", 4, 0, 16, "Falls heard as voices of their own"};
tunables::Int world_surfs{"audio.world.surf", 3, 0, 16,
                          "Stretches of surf heard as voices of their own"};
tunables::Float world_water_reach{
    "audio.world.water_reach_m", 800.0, 10.0, 20000.0,
    "A stream or surf farther than this is not summed (a fall: reach_m)"};
tunables::Int world_max_water{
    "audio.world.max_water", 4096, 0, 1 << 20,
    "Stretches and surf a refresh takes from the tables; the rest counted"};
tunables::Bool world_surf_cull{
    "audio.world.surf_cull", true,
    "Skip a stretch of surf whose bound at the listener is 20 dB under the floor (off: cost all)"};

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_rho = 998.2;  // water at 20 degC, kg/m^3
constexpr f64 k_g = 9.81;
// The reference intensity of dB SPL's scale in a plane wave (rho c ~ 400 rayl: within 0.1 dB of
// 20 uPa), so an intensity over it is the power a band's level in dB SPL stands for here.
constexpr f64 k_intensity_ref = 1e-12;

// **The spectrum: bubbles.** Water alone is nearly silent; what rings is air (Minnaert: f a = 3.26
// m/s [F]). Under breaking water bubbles larger than the Hinze scale, about 1 mm, number as radius
// to the -10/3 and smaller ones as radius to the -3/2 (Deane and Stokes 2002 [A]). Taking each
// bubble's energy as its surface area (a choice: no source read gives a bubble's acoustic energy
// against its radius), the power an octave of radius carries is a^(-1/3) above the Hinze scale and
// a^(3/2) below it: in frequency, rising 1 dB an octave to the Hinze scale's ring and falling 4.5
// dB an octave over it. Running water rings there, 3.26 kHz.
constexpr f64 k_minnaert_m_s = 3.26;
constexpr f64 k_hinze_hz = 3260.0;
// **A fall**: its height and its flow raise the level, each with less return as it grows, and
// falling on water gives 5 to 10 dB more between 250 Hz and 2 kHz than falling on stone, which adds
// about 5 dB at the top (Galbrun and Ali, through two theses [S]); taken as 7.5 dB on 250 Hz to 2
// kHz on water and 5 dB on 4 and 8 kHz on the bed, the spectrum then scaled back to the law's
// power. Its low end grows with its flow (S110's table): the bubbles' ring is lowered as the cube
// root of 1 + Q (Q in m^3/s; a choice). **The cap**: at a plunge pool the acoustic power rises with
// the flow to a threshold and then holds (Gauvain and others 2026 [A]), and past a critical
// discharge a free fall's further flow is hardly perceived (Schalko and Boes, seven Swiss falls
// [A]); neither gives the threshold, so a fall's power is counted to a unit discharge of 0.5 m^2/s
// over its lip's wet width and held there (a choice).
constexpr f64 k_fall_unit_cap_m2_s = 0.5;
constexpr f64 k_fall_water_db = 7.5;
constexpr f64 k_fall_rock_db = 5.0;
// **The surf**: a broad peak near 1 kHz, moving to 250 to 400 Hz for waves over 1 m (Tollefsen and
// Byrne reading Bolin and Abom's figure [S]): the bubbles' ring at 1 kHz to a breaking height of 1
// m, lowered in octaves to 325 Hz at 2 m and held there. A surging wave (surf similarity over 2,
// Battjes 1974, as `gfx::breaker_type`) runs up and back with little air entrained: a quarter of
// the power (a choice).
constexpr f64 k_surf_peak_hz = 1000.0;
constexpr f64 k_surf_peak_big_hz = 325.0;
constexpr f64 k_surging_share = 0.25;
// **The wave's own sound**: a wave is heard as it breaks at a place, so a stretch of surf's power
// follows the train's phase there (the arrival field's crest at the break: the water's clock less
// the arrival, over the period). Its envelope rises over the last 5 % of a period as the crest
// arrives and falls as e^(-phi / 0.25) through the bore (choices: the roller's life is the
// picture's 0.3 of a period, water.md "Where a crest breaks"), normalised to its mean so the power
// over a period is the law's.
constexpr f64 k_surf_rise = 0.05;
constexpr f64 k_surf_decay = 0.25;
// **A trickle is separate bubbles** (S110's table: sparse pings when the flow is small): a
// stream's share heard as bubbles is 1 under 2 L/s and 0 over 20 L/s, linear between (choices),
// struck at 15 a second for each L/s, at most 60 a second, on its voice's four resonators. Each is
// a radius drawn from Deane and Stokes' two slopes between 0.25 and 5 mm (a drop entrains a bubble
// regularly between 1 and 5 mm across, Phillips and others 2018 [F]), ringing at Minnaert's
// frequency for it and damped at van den Doel's 0.043 f + 0.0014 f^1.5 a second (his liquid sounds,
// through GRAME's Faust example that cites them [F]); each carries an equal share of the power.
constexpr f64 k_trickle_full_m3_s = 0.002;
constexpr f64 k_trickle_none_m3_s = 0.020;
constexpr f64 k_bubbles_per_m3 = 15000.0;
constexpr f64 k_bubbles_max_hz = 60.0;
constexpr f64 k_bubble_min_mm = 0.25;
constexpr f64 k_bubble_max_mm = 5.0;
// Where the sound leaves from, over the ground for the ground's term, and how near it is heard.
constexpr f64 k_stream_height_m = 0.2;
constexpr f64 k_fall_height_m = 0.5;
constexpr f64 k_surf_height_m = 0.5;
constexpr f64 k_water_min_m = 1.0;
constexpr f64 k_fall_min_m = 2.0;

f64 pow10(f64 db) noexcept { return std::pow(10.0, db / 10.0); }

// The bubbles' octave shape with its ring at `peak_hz`, powers summing to one.
void bubble_shape(f64 peak_hz, f64 (&out)[8]) noexcept {
  f64 sum = 0.0;
  for (u32 b = 0; b < 8; ++b) {
    const f64 x = static_cast<f64>(audio::k_noise_band_hz[b]) / peak_hz;
    out[b] = x <= 1.0 ? std::cbrt(x) : 1.0 / (x * std::sqrt(x));
    sum += out[b];
  }
  for (f64& p : out)
    p /= sum;
}

u64 key_of(const WorldPos& p, u64 salt) noexcept {
  const f64 v[3] = {p.x, p.y, p.z};
  return (hash_bytes(v, sizeof(v)) ^ salt) | 1u;
}

// A line from `a` to `b` heard from `at`: the nearest point on it, the perpendicular distance to
// it (at least `least`) and the angle it subtends there, so its intensity is W' angle / (2 pi r)
// for W' watts a metre radiated into the half space over the ground (the line's own closed form:
// 3 dB a doubling near, 6 far).
void line_from(const WorldPos& a, const WorldPos& b, const WorldPos& at, f64 least,
               WorldPos& nearest, f64& r, f64& angle) noexcept {
  const f64 ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
  const f64 len = std::sqrt(ux * ux + uy * uy + uz * uz);
  const f64 px = at.x - a.x, py = at.y - a.y, pz = at.z - a.z;
  if (!(len > 1e-6)) {
    nearest = a;
    r = std::sqrt(px * px + py * py + pz * pz);
    r = r > least ? r : least;
    angle = 0.0;
    return;
  }
  const f64 s = (px * ux + py * uy + pz * uz) / len;  // along the line from `a`
  const f64 fx = px - s * ux / len, fy = py - s * uy / len, fz = pz - s * uz / len;
  r = std::sqrt(fx * fx + fy * fy + fz * fz);
  r = r > least ? r : least;
  angle = std::atan((len - s) / r) - std::atan(-s / r);
  const f64 t = s < 0.0 ? 0.0 : (s > len ? len : s);
  nearest = WorldPos{a.x + ux * t / len, a.y + uy * t / len, a.z + uz * t / len};
}

// **What a stretch of surf can deliver at the listener, at most** (S113's surf cull): its line's
// intensity is W' angle / (2 pi r) with r its perpendicular distance (at least `least`), and
// angle / r is the integral over the line of 1 / (r^2 + x^2), each term at most 1 / rho^2 for rho
// the distance to the line's nearest point (and at most 1 / least^2), so the intensity is at most
// W' L / (2 pi max(rho, least)^2); rho is at least the horizontal distance to its middle less its
// half. The path's terms take nothing back but the ground's, which gives at most 6 dB (-1.5 at
// each end and -3 over the middle, ISO 9613-2's least: path.cpp), and the shape sums to one, so
// this times 10^0.6 bounds every band's sum after the path. In the units of `cull_power_`.
f64 surf_bound_power(const Surf& su, const WorldPos& listener, f64 eff) noexcept {
  if (!(su.power_w_m > 0.0f) || !(su.length_m > 0.0f)) return 0.0;
  const f64 xi = static_cast<f64>(su.surf_similarity);
  const f64 watts_m = eff * static_cast<f64>(su.power_w_m) * (xi > 2.0 ? k_surging_share : 1.0);
  const f64 sx = su.at.x - listener.x, sz = su.at.z - listener.z;
  const f64 edge = std::sqrt(sx * sx + sz * sz) - 0.5 * static_cast<f64>(su.length_m);
  const f64 rho = edge > k_fall_min_m ? edge : k_fall_min_m;
  constexpr f64 k_ground_gain_max = 3.9810717055349722;  // 10^0.6
  return watts_m * static_cast<f64>(su.length_m) * k_ground_gain_max / (2.0 * k_pi * rho * rho) /
         k_intensity_ref;
}

}  // namespace

f64 surf_bound_db(const Surf& surf, const WorldPos& listener) noexcept {
  const f64 p = surf_bound_power(surf, listener, k_water_airborne_efficiency);
  return p > 0.0 ? 10.0 * std::log10(p) : -1000.0;
}

f64 surf_power_w_m(f64 height_m, f64 period_s) noexcept {
  if (!(height_m > 0.0) || !(period_s > 0.0)) return 0.0;
  return k_rho * k_g * k_g * height_m * height_m * period_s / (64.0 * k_pi);
}

f64 surf_envelope_mean() noexcept {
  // The rise's mean over its share, then e^(-phi / decay)'s over the rest.
  return k_surf_rise * 0.5 + k_surf_decay * (1.0 - std::exp(-(1.0 - k_surf_rise) / k_surf_decay));
}

void water_config_from_tunables(FieldConfig& c) noexcept {
  c.streams = static_cast<u32>(world_streams.get());
  c.falls = static_cast<u32>(world_falls.get());
  c.surfs = static_cast<u32>(world_surfs.get());
  c.max_water = static_cast<u32>(world_max_water.get());
  c.water_reach_m = world_water_reach.get();
  c.surf_cull = world_surf_cull.get();
}

void WorldField::take_water(const WaterSource& s) noexcept {
  if (!(s.total > 0.0)) return;
  if (water_.size() < config_.max_water) {
    water_.push_back(s);
    return;
  }
  // Full: one source is dropped whichever it is — this one, or the farthest kept, which this one
  // replaces — so the count is the sources sent past the cap, the same either way.
  ++stats_.water_dropped;
  if (config_.max_water == 0) return;
  // Full: the farthest kept goes if this one is nearer (a total order: distance, then key).
  const auto farther = [this](u32 a, u32 b) noexcept {
    if (water_[a].dist != water_[b].dist) return water_[a].dist < water_[b].dist;
    return water_[a].key < water_[b].key;
  };
  if (water_heap_.empty()) {
    for (u32 i = 0; i < water_.size(); ++i)
      water_heap_.push_back(i);
    std::make_heap(water_heap_.begin(), water_heap_.end(), farther);
  }
  const u32 top = water_heap_.front();
  const bool nearer =
      s.dist < water_[top].dist || (s.dist == water_[top].dist && s.key < water_[top].key);
  if (!nearer) return;
  std::pop_heap(water_heap_.begin(), water_heap_.end(), farther);
  water_[top] = s;
  std::push_heap(water_heap_.begin(), water_heap_.end(), farther);
}

void WorldField::add_water_to_bed(const WaterSource& s, const WorldPos& listener,
                                  f64 sign) noexcept {
  const u32 sectors = static_cast<u32>(beds_.size());
  if (sectors == 0 || !(s.total > 0.0)) return;
  const f64 dx = s.at.x - listener.x;
  const f64 dz = s.at.z - listener.z;
  const f64 d = std::sqrt(dx * dx + dz * dz);
  f64 azimuth = std::atan2(dx, -dz);  // 0 north (-z), clockwise from above, as the crowns'
  if (azimuth < 0.0) azimuth += 2.0 * k_pi;
  u32 sector = static_cast<u32>(azimuth / (2.0 * k_pi) * sectors);
  if (sector >= sectors) sector = sectors - 1;
  Bed& b = beds_[sector];
  for (u32 k = 0; k < 8; ++k)
    b.water[k] += sign * s.band[k];
  b.water_total += sign * s.total;
  b.water_count += sign > 0.0 ? 1 : -1;
  if (d > 1e-6) {
    b.dx += sign * s.total * dx / d;
    b.dz += sign * s.total * dz / d;
  }
}

void WorldField::refresh_water(const WorldPos& listener) noexcept {
  water_.clear();
  water_heap_.clear();
  stats_.water_dropped = 0;
  stats_.surf_culled = 0;
  const f64 eff = k_water_airborne_efficiency;
  struct Sink {
    WorldField* self;
    WorldPos listener;
    f64 eff;
    // A stretch or a fall: its power, its spreading and its path, as one source.
    static void stretch(void* user, const Stretch& st) noexcept {
      Sink& k = *static_cast<Sink*>(user);
      WorldField& f = *k.self;
      const f64 q = st.discharge_m3s > 0.0f ? static_cast<f64>(st.discharge_m3s) : 0.0;
      const f64 h = st.drop_m > 0.0f ? static_cast<f64>(st.drop_m) : 0.0;
      ++f.stats_.stretches_seen;
      if (!(q > 0.0) || !(h > 0.0)) return;  // no flow, or still water: nothing given up
      // A stream past `audio.world.water_reach_m` is not taken (a fall is, to the whole reach):
      // the forest staging's walk sent 39,142 stretches a refresh within 2 km, at 10 ms.
      if (!st.fall) {
        const f64 mx = 0.5 * (st.a.x + st.b.x) - k.listener.x,
                  mz = 0.5 * (st.a.z + st.b.z) - k.listener.z;
        const f64 hx = 0.5 * (st.b.x - st.a.x), hz = 0.5 * (st.b.z - st.a.z);
        const f64 far = std::sqrt(mx * mx + mz * mz) - std::sqrt(hx * hx + hz * hz);
        if (far > f.config_.water_reach_m) return;
      }
      WaterSource s;
      s.kind = st.fall ? WorldVoiceKind::Fall : WorldVoiceKind::Stream;
      f64 shape[8];
      f64 intensity = 0.0;
      f64 r = 0.0;
      f64 height = 0.0;
      if (st.fall) {
        const f64 width = st.wet_width_m > 0.1f ? static_cast<f64>(st.wet_width_m) : 0.1;
        const f64 held = q < k_fall_unit_cap_m2_s * width ? q : k_fall_unit_cap_m2_s * width;
        const f64 watts = k.eff * k_rho * k_g * held * h;
        s.at = st.b;  // the foot
        const f64 dx = s.at.x - k.listener.x, dy = s.at.y - k.listener.y,
                  dz = s.at.z - k.listener.z;
        r = std::sqrt(dx * dx + dy * dy + dz * dz);
        r = r > k_fall_min_m ? r : k_fall_min_m;
        intensity = watts / (2.0 * k_pi * r * r);  // a point over the ground: the half space
        bubble_shape(k_hinze_hz / std::cbrt(1.0 + q), shape);
        f64 sum = 0.0;
        for (u32 b = 0; b < 8; ++b) {
          if (st.lands_on_water && b >= 2 && b <= 5) shape[b] *= pow10(k_fall_water_db);
          if (!st.lands_on_water && b >= 6) shape[b] *= pow10(k_fall_rock_db);
          sum += shape[b];
        }
        for (f64& p : shape)
          p /= sum;
        height = k_fall_height_m;
        s.spread = 0.0f;
      } else {
        const f64 watts = k.eff * k_rho * k_g * q * h;
        const f64 lx = st.b.x - st.a.x, ly = st.b.y - st.a.y, lz = st.b.z - st.a.z;
        const f64 len = std::sqrt(lx * lx + ly * ly + lz * lz);
        const f64 half = 0.5 * static_cast<f64>(st.wet_width_m);
        const f64 least = half > k_water_min_m ? half : k_water_min_m;
        f64 angle = 0.0;
        line_from(st.a, st.b, k.listener, least, s.at, r, angle);
        intensity = len > 1e-6 && angle > 0.0 ? (watts / len) * angle / (2.0 * k_pi * r)
                                              : watts / (2.0 * k_pi * r * r);
        s.spread = static_cast<f32>(angle / k_pi < 1.0 ? angle / k_pi : 1.0);
        bubble_shape(k_hinze_hz, shape);
        height = k_stream_height_m;
        const f64 share = (k_trickle_none_m3_s - q) / (k_trickle_none_m3_s - k_trickle_full_m3_s);
        s.trickle = share < 0.0 ? 0.0 : (share > 1.0 ? 1.0 : share);
        const f64 rate = k_bubbles_per_m3 * q;
        s.bubbles_hz = s.trickle > 0.0 ? (rate < k_bubbles_max_hz ? rate : k_bubbles_max_hz) : 0.0;
      }
      const f64 power = intensity / k_intensity_ref;
      // A stretch 20 dB under the floor before its path is not taken: a thousand of them summed in
      // a bed would still be at the floor.
      if (!(power > f.cull_power_)) return;
      const f64 dx = s.at.x - k.listener.x, dy = s.at.y - k.listener.y, dz = s.at.z - k.listener.z;
      s.dist = std::sqrt(dx * dx + dz * dz);
      if (dx * dx + dy * dy + dz * dz > f.config_.reach_m * f.config_.reach_m) return;
      f.path_terms(dx, dy + height, dz, height, s.path);
      s.total = 0.0;
      for (u32 b = 0; b < 8; ++b) {
        s.band[b] = power * shape[b] * s.path[b];
        s.total += s.band[b];
      }
      // Identity: where the stretch starts and ends (a fall: its foot), so a stretch keeps its
      // voice across refreshes while the listener moves along it.
      s.key = key_of(st.a, key_of(st.b, st.fall ? 0xfa11ull : 0x5eull));
      f.take_water(s);
    }
    static void surf(void* user, const Surf& su) noexcept {
      Sink& k = *static_cast<Sink*>(user);
      WorldField& f = *k.self;
      if (!(su.power_w_m > 0.0f) || !(su.length_m > 0.0f) || !(su.period_s > 0.0f)) return;
      const f64 sx = su.at.x - k.listener.x, sz = su.at.z - k.listener.z;
      if (std::sqrt(sx * sx + sz * sz) - 0.5 * static_cast<f64>(su.length_m) >
          f.config_.water_reach_m)
        return;
      // The streams' cull before the closed forms, on a bound of what it could deliver after its
      // path: one under the floor less 20 dB is skipped, counted and never costed.
      if (f.config_.surf_cull && !(surf_bound_power(su, k.listener, k.eff) > f.cull_power_)) {
        ++f.stats_.surf_culled;
        return;
      }
      const f64 xi = static_cast<f64>(su.surf_similarity);
      const f64 watts_m =
          k.eff * static_cast<f64>(su.power_w_m) * (xi > 2.0 ? k_surging_share : 1.0);
      const f64 half = 0.5 * static_cast<f64>(su.length_m);
      const f64 ax = static_cast<f64>(su.along.x), az = static_cast<f64>(su.along.y);
      const f64 al = std::sqrt(ax * ax + az * az);
      const f64 ux = al > 1e-9 ? ax / al : 1.0, uz = al > 1e-9 ? az / al : 0.0;
      const WorldPos a{su.at.x - ux * half, su.at.y, su.at.z - uz * half};
      const WorldPos b{su.at.x + ux * half, su.at.y, su.at.z + uz * half};
      WaterSource s;
      s.kind = WorldVoiceKind::Surf;
      f64 r = 0.0;
      f64 angle = 0.0;
      line_from(a, b, k.listener, k_fall_min_m, s.at, r, angle);
      const f64 intensity = watts_m * angle / (2.0 * k_pi * r);
      s.spread = static_cast<f32>(angle / k_pi < 1.0 ? angle / k_pi : 1.0);
      const f64 hb = static_cast<f64>(su.height_m);
      const f64 octaves = hb <= 1.0 ? 0.0 : (hb >= 2.0 ? 1.0 : hb - 1.0);
      f64 shape[8];
      bubble_shape(k_surf_peak_hz * std::pow(k_surf_peak_big_hz / k_surf_peak_hz, octaves), shape);
      const f64 power = intensity / k_intensity_ref;
      const f64 dx = s.at.x - k.listener.x, dy = s.at.y - k.listener.y, dz = s.at.z - k.listener.z;
      s.dist = std::sqrt(dx * dx + dz * dz);
      if (dx * dx + dy * dy + dz * dz > f.config_.reach_m * f.config_.reach_m) return;
      f.path_terms(dx, dy + k_surf_height_m, dz, k_surf_height_m, s.path);
      s.total = 0.0;
      for (u32 i = 0; i < 8; ++i) {
        s.band[i] = power * shape[i] * s.path[i];
        s.total += s.band[i];
      }
      s.period_s = static_cast<f64>(su.period_s);
      s.arrival_s = static_cast<f64>(su.arrival_s);
      s.key = key_of(su.at, 0x5afull);
      ++f.stats_.surf_seen;
      f.take_water(s);
    }
  } sink{this, listener, eff};
  stats_.stretches_seen = 0;
  stats_.surf_seen = 0;
  if (flow_.visit != nullptr)
    flow_.visit(flow_.user, listener, config_.reach_m,
                StretchSink{&sink, &Sink::stretch, config_.water_reach_m});
  if (surf_.visit != nullptr)  // the surf is taken to `water_reach_m` (`Sink::surf`)
    surf_.visit(surf_.user, listener, config_.water_reach_m, SurfSink{&sink, &Sink::surf});
  // Everything into its sector's bed, by power (the surf at its mean over a period).
  for (const WaterSource& s : water_)
    add_water_to_bed(s, listener, 1.0);
  // The loudest of each kind as voices of their own, a chosen one kept across 3 dB, each taken out
  // of its bed again by the same arithmetic it went in with: its power is kept across a promotion.
  const WorldVoiceKind kinds[3] = {WorldVoiceKind::Stream, WorldVoiceKind::Fall,
                                   WorldVoiceKind::Surf};
  const u32 counts[3] = {config_.streams, config_.falls, config_.surfs};
  u32 first = water_first_;
  const u32 n = static_cast<u32>(water_.size());
  for (u32 kind = 0; kind < 3; ++kind) {
    const u32 want = counts[kind] < 16 ? counts[kind] : 16;
    u64 next[16] = {};
    u32 picked[16] = {};
    for (u32 slot = 0; slot < want; ++slot) {
      f64 best = 0.0;
      u32 best_i = ~0u;
      for (u32 i = 0; i < n; ++i) {
        const WaterSource& s = water_[i];
        if (s.kind != kinds[kind]) continue;
        bool taken = false;
        for (u32 j = 0; j < slot; ++j)
          taken = taken || next[j] == s.key;
        if (taken) continue;
        f64 level = s.total;
        for (u32 j = 0; j < want; ++j)
          if (voices_[first + j].key == s.key) level *= 2.0;  // 3 dB
        if (level > best) {
          best = level;
          best_i = i;
        }
      }
      next[slot] = best_i != ~0u ? water_[best_i].key : 0u;
      picked[slot] = best_i;
    }
    // A voice whose source is still chosen keeps it; the others go quiet, then take the new ones.
    for (u32 slot = 0; slot < want; ++slot) {
      Voice& v = voices_[first + slot];
      bool kept = false;
      for (u32 j = 0; j < want && !kept; ++j)
        kept = next[j] != 0 && next[j] == v.key;
      if (!kept && v.key != 0) {
        silence(v);
        v.key = 0;
      }
    }
    for (u32 j = 0; j < want; ++j) {
      if (next[j] == 0) continue;
      u32 at = ~0u;
      for (u32 slot = 0; slot < want && at == ~0u; ++slot)
        at = voices_[first + slot].key == next[j] ? slot : ~0u;
      for (u32 slot = 0; slot < want && at == ~0u; ++slot) {
        if (voices_[first + slot].key == 0) {
          voices_[first + slot].key = next[j];
          voices_[first + slot].has_sent = false;
          WaterVoice& w = water_voices_[first + slot - water_first_];
          w.next_bubble_s = -1.0;
          w.rng = static_cast<u32>(next[j] ^ (next[j] >> 32)) | 1u;
          at = slot;
        }
      }
      if (at == ~0u) continue;
      const WaterSource& s = water_[picked[j]];
      water_voices_[first + at - water_first_].source = s;
      add_water_to_bed(s, listener, -1.0);
    }
    first += counts[kind];
  }
}

void WorldField::tick_water(const WorldPos& frame, bool frame_moved) noexcept {
  (void)frame;
  const f64 clock = static_cast<f64>(gfx::water_clock(wind_.world_time_s));
  for (u32 i = water_first_; i < voices_.size(); ++i) {
    Voice& v = voices_[i];
    for (f64& p : v.bands)
      p = 0.0;
    v.tone_hz = 0.0;
    if (v.key == 0) continue;
    const WaterSource& s = water_voices_[i - water_first_].source;
    f64 scale = 1.0 - s.trickle;  // a trickle's share is its bubbles'
    if (s.kind == WorldVoiceKind::Surf && s.period_s > 0.0) {
      // The wave as it breaks here: its phase at the water's clock, the crest at 0.
      f64 phi = (clock - s.arrival_s) / s.period_s;
      phi -= std::floor(phi);
      const f64 e = phi >= 1.0 - k_surf_rise ? (phi - (1.0 - k_surf_rise)) / k_surf_rise
                                             : std::exp(-phi / k_surf_decay);
      scale = e / surf_shape_mean_;
    }
    for (u32 b = 0; b < 8; ++b)
      v.bands[b] = s.band[b] * scale;
    v.info.wind_m_s = 0.0f;
    const f64 mx = s.at.x - v.at.x, my = s.at.y - v.at.y, mz = s.at.z - v.at.z;
    const bool moved = mx * mx + my * my + mz * mz > 1.0;
    v.at = s.at;
    v.spread = s.spread;
    v.needs_place = v.needs_place || moved || frame_moved;
  }
}

void WorldField::account_levels() noexcept {
  // After the hold: each voice's bands as described this tick (a trickle's bubbles in its level),
  // the beds' water share scaled by the hold's factor.
  const f64 held = std::pow(10.0, static_cast<f64>(stats_.held_db) / 10.0);
  f64 p[k_level_kinds] = {};
  for (u32 i = 0; i < voices_.size(); ++i) {
    const Voice& v = voices_[i];
    f64 sum = 0.0;
    for (const f64 b : v.bands)
      sum += b;
    switch (v.kind) {
      case WorldVoiceKind::Crown: p[0] += sum; break;
      case WorldVoiceKind::Bed: {
        p[1] += sum;
        if (sum > 0.0) {
          const Bed& b = beds_[i - config_.crowns];
          for (const f64 w : b.water)
            p[2] += w > 0.0 ? w * held : 0.0;
        }
        break;
      }
      case WorldVoiceKind::EarLeft:
      case WorldVoiceKind::EarRight: p[3] += sum; break;
      case WorldVoiceKind::Stream:
      case WorldVoiceKind::Fall:
      case WorldVoiceKind::Surf: {
        const WaterSource& s = water_voices_[i - water_first_].source;
        const bool heard =
            v.key != 0 && (config_.solo == 0 || config_.solo == static_cast<u32>(v.kind) - 1u);
        const f64 bubbles = heard ? s.total * s.trickle : 0.0;
        p[4 + static_cast<u32>(v.kind) - static_cast<u32>(WorldVoiceKind::Stream)] += sum + bubbles;
        break;
      }
      default: break;
    }
  }
  for (u32 k = 0; k < k_level_kinds; ++k)
    level_sum_[k] += p[k];
  ++level_ticks_;
  const f64 wind = p[0] + p[3] + (p[1] - p[2] > 0.0 ? p[1] - p[2] : 0.0);
  if (p[4] + p[5] > wind) ++water_over_wind_;
}

f64 WorldField::mean_level_db(u32 kind) const noexcept {
  if (kind >= k_level_kinds || level_ticks_ == 0) return -1000.0;
  const f64 mean = level_sum_[kind] / static_cast<f64>(level_ticks_);
  return mean > 0.0 ? 10.0 * std::log10(mean) : -1000.0;
}

void WorldField::strike_bubbles(Voice& v, WaterVoice& w, f64 now_s, f64 dt) noexcept {
  const WaterSource& s = w.source;
  if (!(s.trickle > 0.0) || !(s.bubbles_hz > 0.0) || v.voice.is_null()) return;
  if (w.next_bubble_s < 0.0) w.next_bubble_s = now_s;
  // The bubbles' power at the listener before the path, in full scale's units, shared equally.
  f64 before = 0.0;
  for (u32 b = 0; b < 8; ++b)
    before += s.path[b] > 0.0 ? s.band[b] / s.path[b] : 0.0;
  const f64 scale = std::pow(10.0, -config_.reference_db / 10.0);
  const f64 share = s.trickle * before * scale / s.bubbles_hz;  // a bubble's energy a second
  const auto next_u = [&w]() noexcept {
    w.rng ^= w.rng << 13;
    w.rng ^= w.rng >> 17;
    w.rng ^= w.rng << 5;
    return (static_cast<f64>(w.rng >> 8) + 0.5) / 16777216.0;  // (0, 1)
  };
  // Deane and Stokes' two slopes as one distribution of radius over [0.25, 5] mm, drawn by its
  // inverse: n ~ a^(-3/2) under 1 mm (its integral 2 there), a^(-10/3) over it (0.4186).
  const f64 small = 2.0 * (1.0 / std::sqrt(k_bubble_min_mm) - 1.0);
  const f64 large = (1.0 - std::pow(k_bubble_max_mm, -7.0 / 3.0)) * 3.0 / 7.0;
  u32 struck = 0;
  while (w.next_bubble_s <= now_s + dt && struck < 4) {
    const f64 x = next_u() * (small + large);
    const f64 a_mm = x < small ? 1.0 / ((1.0 / std::sqrt(k_bubble_min_mm) - 0.5 * x) *
                                        (1.0 / std::sqrt(k_bubble_min_mm) - 0.5 * x))
                               : std::pow(1.0 - (x - small) * 7.0 / 3.0, -3.0 / 7.0);
    const f64 f = k_minnaert_m_s / (a_mm * 1e-3);
    const f64 damping = 0.043 * f + 0.0014 * f * std::sqrt(f);
    // A ring A r^n sin carries A^2 / (2 (1 - r^2)) ~ A^2 fs / (4 d) in its samples: so a bubble of
    // `share` x fs over a second's samples has A = sqrt(4 d share), and its band's path on it.
    f64 path = 1.0;
    {
      f64 o = f > 62.5 ? std::log2(f / 62.5) : 0.0;
      o = o < 7.0 ? o : 7.0;
      const u32 i = static_cast<u32>(o);
      const f64 t = o - static_cast<f64>(i);
      path = i >= 7 ? s.path[7] : s.path[i] * (1.0 - t) + s.path[i + 1] * t;
    }
    const f64 amp = std::sqrt(4.0 * damping * share * path);
    if (mixer_->strike_noise(
            v.voice, audio::noise_strike(w.resonator, static_cast<f32>(f),
                                         static_cast<f32>(damping), static_cast<f32>(amp)))) {
      ++stats_.commands;
      ++stats_.bubbles;
    }
    w.resonator = (w.resonator + 1u) % audio::k_noise_resonators;
    w.next_bubble_s += -std::log(next_u()) / s.bubbles_hz;
    ++struck;
  }
  if (w.next_bubble_s < now_s) w.next_bubble_s = now_s;  // a backlog is not struck all at once
}

}  // namespace engine::world_sound
