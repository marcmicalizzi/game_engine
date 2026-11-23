// The world's sound: the wind in the crowns and at the ears (world_field.h; S112;
// docs/subsystems/world_sound.md, "The wind in the trees and at the ears"; the grounding,
// docs/experiments/world-sound.md, "Wind in vegetation" and "Wind at the listener").

#include "world_field.h"

#include <core/hash/hash.h>
#include <foundation/tunables/tunables.h>
#include <systems/world_sound/path.h>

#include <algorithm>
#include <cmath>

namespace engine::world_sound {

namespace {

// ---- the tunables (audio.world.*) ---------------------------------------------------------------

tunables::Int world_crowns{"audio.world.crowns", 6, 0, 32,
                           "Crowns heard as voices of their own, chosen by level at the listener"};
tunables::Int world_beds{"audio.world.beds", 8, 0, 16,
                         "Beds: sectors of azimuth everything not chosen merges into by power"};
tunables::Int world_max_crowns{"audio.world.max_crowns", 8192, 0, 1 << 20,
                               "Crowns a refresh takes from the canopy table; the rest counted"};
tunables::Float world_reach_m{"audio.world.reach_m", 2000.0, 10.0, 20000.0,
                              "Nothing farther than this from the listener is summed"};
tunables::Float near_m_tunable{"audio.world.near_m", 96.0, 0.0, 2000.0,
                               "Crowns nearer than this are sent one by one; farther, merged"};
tunables::Int world_refresh_hz{"audio.world.refresh_hz", 8, 1, 240,
                               "How often the canopy table is walked and the voices chosen"};
// **One loudness scale** (S112): the cinema's reference, -20 dBFS RMS reproduced at 85 dB SPL
// (SMPTE RP 200, the forest's bird calls' scale), so an RMS of 1.0 in the mix is 105 dB SPL.
tunables::Float world_reference_db{"audio.world.reference_db", 105.0, 60.0, 140.0,
                                   "dB SPL at the listener that a world band's RMS of 1.0 means"};
tunables::Float world_ears_db{"audio.world.ears_db", -30.0, -80.0, 0.0,
                              "The ears' own wind under its physical level (a choice of the mix)"};
tunables::Float world_ears_side_db{"audio.world.ears_side_db", 4.0, 0.0, 20.0,
                                   "dB the downwind ear is over the mean, side-on to the wind"};
tunables::Float world_ears_slope_db{"audio.world.ears_slope_db", 26.0, 3.0, 60.0,
                                    "dB an octave the ears' wind falls above its corner"};
tunables::Float world_calm_m_s{"audio.world.calm_m_s", 0.5, 0.0, 10.0,
                               "Wind under which a world voice is silent and held by nothing"};
tunables::Float world_floor_db{"audio.world.floor_db", 10.0, -20.0, 60.0,
                               "dB SPL at the listener under which a world voice is not held"};
// **Nothing hard-clips** (S112-2): the world's voices summed are held at this RMS, dBFS, and only
// there. The world is Gaussian-like noise whose peaks run about 12 dB over its RMS (4 sigma), so
// -12 is where its peaks reach full scale and a hard clip would begin; under it nothing changes.
tunables::Float world_ceiling_db{"audio.world.ceiling_db", -12.0, -40.0, 0.0,
                                 "RMS dBFS the world's voices summed are held at, and only there"};
tunables::Int world_near_crowns_tunable{"audio.world.near_crowns", 1024, 0, 1 << 20,
                                        "Crowns a canopy table sends one by one a refresh"};
tunables::Int world_walk_tunable{"audio.world.walk_instances", 4096, 0, 1 << 24,
                                 "Placements a canopy table walks a refresh for changed tiles"};
// **The path** (S114; path.h): the air a world voice crosses, and the ground under it. The scene
// names no air of its own yet, so the stage's air is these: ISO 9613-1's 20 degC and the design's
// 70 % (S110), a temperate forest's summer afternoon.
tunables::Float world_temperature_c{"audio.world.temperature_c", 20.0, -20.0, 50.0,
                                    "The air's temperature, degC, for its absorption by band"};
tunables::Float world_humidity{"audio.world.humidity", 70.0, 10.0, 100.0,
                               "The air's relative humidity, %, for its absorption by band"};
tunables::Float world_ground_g{"audio.world.ground_g", 1.0, 0.0, 1.0,
                               "ISO 9613-2's ground factor under the world: 0 hard, 1 porous"};
tunables::Bool world_path{"audio.world.path", true,
                          "Take the path's terms (air, forest, ground); off: spreading alone"};
tunables::Int world_solo{"audio.world.solo", 0, 0, 3,
                         "One kind of world voice heard alone: 0 all, 1 crowns, 2 beds, 3 ears"};

// ---- the laws (docs/experiments/world-sound.md) -------------------------------------------------

// A crown's level at the listener, dB SPL: `k_crown_db[class]` for `k_crown_area_m2` of leaf at
// `k_crown_ref_m` in a wind of `k_crown_wind_m_s` at the crown, then 30 log U (Fegeant's law as
// Bolin's thesis gives it [S]: 9 dB a doubling), 10 log of the leaf area (Bolin's source term is
// linear in the leaf area density [S]) and 20 log of the distance (a point's spreading [F]). The
// anchors are excerpts, not a model's constants (Fegeant's and Bolin's were not reached): a
// leafless tree's spectrum "adjusted to an average of 45 dB at 5 m/s" [S], its distance not seen
// and taken as 10 m; trees in leaf at 51 to 55 dB(A) at 40 ft [S], taken as 50 dB at 10 m for a
// crown of 100 m2 of leaf (a 6 m crown at a leaf area index of 3.5); conifers as broadleaves, their
// energy moved into the needles' tone band.
constexpr f64 k_crown_db[3] = {50.0, 50.0, 45.0};  // blade, needle, bare
constexpr f64 k_crown_area_m2 = 100.0;
constexpr f64 k_crown_ref_m = 10.0;
constexpr f64 k_crown_wind_m_s = 5.0;
constexpr f64 k_crown_min_m = 2.0;  // nearer than this a crown is all round the listener
// The needles' Aeolian tone: f = St U / d with St 0.2 (Raichel, through "Aeolian sound" [F]), a
// band whose Q stands for a crown's spread of needle widths and speeds (a choice), carrying half a
// needle crown's power ("conifers put more energy at particular frequencies" [S]; the share a
// choice).
constexpr f64 k_strouhal = 0.2;
constexpr f32 k_needle_q = 3.0f;
constexpr f64 k_needle_share = 0.5;
// Each class's octave bands, 62.5 Hz to 8 kHz, in dB before normalising: in leaf strongest below
// about 4 kHz and broadband, bare below about 1 kHz [S]. The shapes are choices that keep to those
// two sentences; no band level was read.
constexpr f64 k_shape_db[3][8] = {
    {-15.0, -10.0, -6.0, -3.0, -1.0, 0.0, -2.0, -8.0},   // blade
    {-16.0, -11.0, -7.0, -4.0, -2.0, -1.0, -2.0, -6.0},  // needle, beside its tone
    {-6.0, -3.0, -1.0, 0.0, -2.0, -7.0, -14.0, -22.0}};  // bare
// The ears' wind: 84.9 dB at 4.5 m/s at a cyclist's ear (Seidman and others 2017 [A]), rising as
// the square of the speed, 12 dB a doubling (Korhonen 2021 [F], through S110), flat below a corner
// near 300 Hz and falling steeply above it, the corner rising with the speed (the same review),
// taken here as proportional to it (a Strouhal law over the head).
constexpr f64 k_ears_db = 84.9;
constexpr f64 k_ears_wind_m_s = 4.5;
constexpr f64 k_ears_corner_hz = 300.0;
constexpr f64 k_ears_height_m = 1.7;
// A bed sounds this far from the listener toward where its power comes from.
constexpr f64 k_bed_distance_m = 20.0;
// Priorities: under a game's default (128); a game's one-shot takes a crown before a bed.
constexpr u8 k_crown_priority = 40;
constexpr u8 k_bed_priority = 50;
constexpr u8 k_ears_priority = 60;
// A description is sent again when a lane's gain moved by more than this share (0.09 dB) or the
// tone's centre by more than this share: what does not move sends nothing.
constexpr f64 k_resend = 0.01;
constexpr f64 k_resend_tone = 0.005;

constexpr f64 k_pi = 3.14159265358979323846;

// ---- the path (S114; path.h; docs/experiments/world-sound.md, "Read again by S114") -----------

// The cover's rings round the listener, metres: fine near, where a path's first metres are, and
// coarser out to the reach.
constexpr f64 k_cover_edge_m[16] = {0.0,   8.0,   16.0,  32.0,  48.0,  64.0,   96.0,   128.0,
                                    192.0, 256.0, 384.0, 512.0, 768.0, 1024.0, 1536.0, 2048.0};
// A bin is wholly under canopy when its one-sided leaf area is this many times its ground: a closed
// canopy, the leaf area index S112's crown anchor was read at (a 6 m crown at 3.5).
constexpr f64 k_closed_lai = 3.5;
// A bed's rings, by the distance its power comes from: the stand round the listener, the wood
// beyond it, and the far wood (S114: each hears the gust at its own place).
constexpr f64 k_bed_ring_m[2] = {32.0, 128.0};

f64 pow10(f64 db) noexcept { return std::pow(10.0, db / 10.0); }

// A band power through the path at `f_hz`, from the path's eight bands, interpolated in octaves:
// the needles' tone between two of the fixed bands.
f64 at_tone(const f64 (&per_band)[8], f64 f_hz) noexcept {
  f64 x = f_hz > 62.5 ? std::log2(f_hz / 62.5) : 0.0;
  x = x < 7.0 ? x : 7.0;
  const u32 i = static_cast<u32>(x);
  if (i >= 7u) return per_band[7];
  const f64 t = x - static_cast<f64>(i);
  return per_band[i] * (1.0 - t) + per_band[i + 1u] * t;
}

// The wind at height `h` from the wind 10 m up, by the neutral log law over roughness `z0`. The
// roughness is held under a quarter of the reference height (L4 of S112's review): at 5 m and over
// the law is zero, negative or infinite there, and no surface is that rough (a city centre's is
// 1 to 2 m).
f64 loglaw(f64 h, f64 z0) noexcept {
  constexpr f64 k_z0_max = gfx::k_wind_height_m / 4.0;
  const f64 z = z0 > 1e-5 ? (z0 < k_z0_max ? z0 : k_z0_max) : 1e-5;
  const f64 hh = h > 2.0 * z ? h : 2.0 * z;
  return std::log(hh / z) / std::log(gfx::k_wind_height_m / z);
}

u64 key_of(const WorldPos& p) noexcept {
  const f64 v[3] = {p.x, p.y, p.z};
  return hash_bytes(v, sizeof(v)) | 1u;
}

}  // namespace

f64 world_near_m() noexcept { return near_m_tunable.get(); }
u32 world_near_crowns() noexcept { return static_cast<u32>(world_near_crowns_tunable.get()); }
u32 world_walk_instances() noexcept { return static_cast<u32>(world_walk_tunable.get()); }

FieldConfig field_config_from_tunables() noexcept {
  FieldConfig c;
  c.crowns = static_cast<u32>(world_crowns.get());
  c.beds = static_cast<u32>(world_beds.get());
  c.max_crowns = static_cast<u32>(world_max_crowns.get());
  c.reach_m = world_reach_m.get();
  c.refresh_hz = static_cast<u32>(world_refresh_hz.get());
  c.reference_db = world_reference_db.get();
  c.ears_db = world_ears_db.get();
  c.ears_side_db = world_ears_side_db.get();
  c.ears_slope_db = world_ears_slope_db.get();
  c.calm_m_s = world_calm_m_s.get();
  c.floor_db = world_floor_db.get();
  c.ceiling_db = world_ceiling_db.get();
  c.temperature_c = world_temperature_c.get();
  c.humidity_pct = world_humidity.get();
  c.ground_g = world_ground_g.get();
  c.path = world_path.get();
  c.solo = static_cast<u32>(world_solo.get());
  return c;
}

void WorldField::open(audio::Mixer* mixer, u32 tick_hz, const FieldConfig& config) {
  mixer_ = mixer;
  config_ = config;
  tick_hz_ = tick_hz != 0 ? tick_hz : 240;
  refresh_every_ = tick_hz_ / (config_.refresh_hz != 0 ? config_.refresh_hz : 8);
  if (refresh_every_ == 0) refresh_every_ = 1;
  crowns_.reserve(config_.max_crowns);
  crown_static_.reserve(config_.max_crowns);
  crown_key_.reserve(config_.max_crowns);
  crown_dist_.reserve(config_.max_crowns);
  far_heap_.reserve(config_.max_crowns);
  chosen_.resize(config_.crowns, 0);
  beds_.resize(config_.beds);
  voices_.resize(config_.crowns + config_.beds + 2u);
  for (u32 i = 0; i < voices_.size(); ++i) {
    voices_[i].kind = i < config_.crowns                  ? WorldVoiceKind::Crown
                      : i < config_.crowns + config_.beds ? WorldVoiceKind::Bed
                      : i + 1u == voices_.size()          ? WorldVoiceKind::EarRight
                                                          : WorldVoiceKind::EarLeft;
    voices_[i].info.kind = voices_[i].kind;
  }
  for (u32 c = 0; c < 3; ++c) {
    f64 sum = 0.0;
    for (u32 b = 0; b < 8; ++b)
      sum += pow10(k_shape_db[c][b]);
    for (u32 b = 0; b < 8; ++b)
      shape_[c][b] = pow10(k_shape_db[c][b]) / sum;
  }
  for (u32 k = 0; k < 3; ++k)
    class_power_[k] = pow10(k_crown_db[k]);
  for (u32 b = 0; b < 8; ++b)
    foliage_db_m_[b] = foliage_db(static_cast<f64>(audio::k_noise_band_hz[b]), 1.0);
  for (u32 b = 0; b < 8; ++b)
    air_db_m_[b] = config_.path
                       ? air_absorption_db_per_m(static_cast<f64>(audio::k_noise_band_hz[b]),
                                                 config_.temperature_c, config_.humidity_pct)
                       : 0.0;
}

f64 WorldField::canopy_path_m(f64 dx, f64 dz) const noexcept {
  const f64 d = std::sqrt(dx * dx + dz * dz);
  f64 azimuth = std::atan2(dx, -dz);
  if (azimuth < 0.0) azimuth += 2.0 * k_pi;
  u32 s = static_cast<u32>(azimuth / (2.0 * k_pi) * k_cover_sectors);
  s = s < k_cover_sectors ? s : k_cover_sectors - 1u;
  u32 j = 0;
  while (j + 1u < k_cover_rings && d >= k_cover_edge_m[j + 1u])
    ++j;
  const f64 into = d - k_cover_edge_m[j];
  return cover_run_[s][j] + (into > 0.0 ? into : 0.0) * cover_[s][j];
}

bool WorldField::crown_power(const Crown& c, const WorldPos& listener, f64 law, f64 (&band)[8],
                             f64 (&path)[8], f64& tone) const noexcept {
  const f64 dx = c.at.x - listener.x;
  const f64 dy = c.at.y + static_cast<f64>(c.height_m) - listener.y;
  const f64 dz = c.at.z - listener.z;
  if (dx * dx + dy * dy + dz * dz > config_.reach_m * config_.reach_m) return false;
  const f64 e = static_cast<f64>(c.extent_m);
  if (!c.merged || !(e > 0.0)) {
    point_power(c, dx, dy, dz, e > k_crown_min_m ? e : k_crown_min_m, law, band, path, tone);
    return true;
  }
  // **A merged group is heard over its disc, not at its centroid** (S114: its power kept across
  // its promotion to crowns one by one). Spreading and the path are both convex in the distance, so
  // a group summed at its centroid was 3.7 dB under the same crowns sent one by one at the
  // promotion's distance (the case). The disc of the group's radius is taken by a seven-point rule
  // exact to degree five over a disc: the centre at a quarter, six points at sqrt(2/3) of the
  // radius an eighth each (its moments: r^2 averages 1/2 and r^4 1/3, as over the disc), each point
  // a crown of the group's power whose least distance is a crown's own.
  constexpr f64 k_ring = 0.816496580927726;  // sqrt(2 / 3)
  for (u32 b = 0; b < 8; ++b) {
    band[b] = 0.0;
    path[b] = 0.0;
  }
  tone = 0.0;
  f64 weight_sum = 0.0;
  for (u32 i = 0; i < 7; ++i) {
    const f64 w = i == 0 ? 0.25 : 0.125;
    const f64 a = static_cast<f64>(i) * (k_pi / 3.0);
    const f64 ox = i == 0 ? 0.0 : e * k_ring * std::cos(a);
    const f64 oz = i == 0 ? 0.0 : e * k_ring * std::sin(a);
    f64 pb[8];
    f64 pp[8];
    f64 pt = 0.0;
    point_power(c, dx + ox, dy, dz + oz, k_crown_min_m, law, pb, pp, pt);
    for (u32 b = 0; b < 8; ++b) {
      band[b] += w * pb[b];
      path[b] += w * (pt > 0.0 ? pt * pp[b] : pp[b]);
    }
    tone += w * pt;
    weight_sum += w;
  }
  for (u32 b = 0; b < 8; ++b)
    path[b] = tone > 0.0 ? path[b] / tone : path[b] / weight_sum;
  return true;
}

void WorldField::point_power(const Crown& c, f64 dx, f64 dy, f64 dz, f64 least, f64 law,
                             f64 (&band)[8], f64 (&path)[8], f64& tone) const noexcept {
  const u32 k = static_cast<u32>(c.kind) < 3 ? static_cast<u32>(c.kind) : 0u;
  const f64 r3 = std::sqrt(dx * dx + dy * dy + dz * dz);
  const f64 r = r3 > least ? r3 : least;
  const f64 power = class_power_[k] * (static_cast<f64>(c.leaf_area_m2) / k_crown_area_m2) *
                    (k_crown_ref_m / r) * (k_crown_ref_m / r) * law * law * law;
  const bool needle = c.kind == CanopyClass::Needle;
  const f64 broadband = needle ? power * (1.0 - k_needle_share) : power;
  tone = needle ? power * k_needle_share : 0.0;
  // The path's terms over the path the sound takes (S114): the air over its whole length from the
  // foliage, the forest over the share of it under canopy, the ground from the foliage's height and
  // the ears' over the distance along the ground. A crown the listener stands in is heard at its
  // edge
  // (`extent_m`) and its path is the short one it really has.
  if (!config_.path) {
    for (u32 b = 0; b < 8; ++b) {
      path[b] = 1.0;
      band[b] = broadband * shape_[k][b];
    }
    return;
  }
  // Each term in dB, then one exponential a band (S114's cost: the bands' shared exponentials
  // once, the forest's f^(1/3) once at `open`).
  const f64 ground_m = std::sqrt(dx * dx + dz * dz);
  const f64 under = canopy_path_m(dx, dz);
  f64 ground[8];
  ground_bands_db(static_cast<f64>(c.height_m), k_ears_height_m, ground_m, config_.ground_g,
                  ground);
  constexpr f64 k_db_to_ln = -0.23025850929940456840;  // -ln(10) / 10
  for (u32 b = 0; b < 8; ++b) {
    const f64 a = air_db_m_[b] * r3 + foliage_db_m_[b] * under + ground[b];
    path[b] = std::exp(a * k_db_to_ln);
    band[b] = broadband * shape_[k][b] * path[b];
  }
}

void WorldField::add_to_bed(const Crown& c, const WorldPos& listener, f64 law, const f64 (&band)[8],
                            const f64 (&path)[8], f64 tone, f64 sign) noexcept {
  const u32 sectors = static_cast<u32>(beds_.size());
  if (sectors == 0) return;
  f64 w = tone * path[4];
  for (const f64 p : band)
    w += p;
  if (!(w > 0.0)) return;
  const f64 dx = c.at.x - listener.x;
  const f64 dz = c.at.z - listener.z;
  const f64 d = std::sqrt(dx * dx + dz * dz);
  f64 azimuth = std::atan2(dx, -dz);  // 0 north (-z), clockwise from above
  if (azimuth < 0.0) azimuth += 2.0 * k_pi;
  u32 s = static_cast<u32>(azimuth / (2.0 * k_pi) * sectors);
  if (s >= sectors) s = sectors - 1;
  Bed& b = beds_[s];
  BedRing& ring = b.ring[d < k_bed_ring_m[0] ? 0u : d < k_bed_ring_m[1] ? 1u : 2u];
  for (u32 k = 0; k < 8; ++k) {
    ring.band[k] += sign * band[k];
    ring.tone[k] += sign * tone * path[k];
  }
  if (tone > 0.0) {
    const f64 needle_m = static_cast<f64>(c.needle_mm > 0.05f ? c.needle_mm : 0.05f) * 1e-3;
    ring.needle_weight += sign * tone;
    ring.needle_weight_d += sign * tone / needle_m;
    ring.needle_loglaw += sign * tone * law;
  }
  ring.px += sign * w * c.at.x;
  ring.pz += sign * w * c.at.z;
  ring.total += sign * w;
  ring.count += sign > 0.0 ? 1 : -1;
  b.total += sign * w;
  b.count += sign > 0.0 ? 1 : -1;
  if (d > 1e-6) {
    b.dx += sign * w * dx / d;
    b.dz += sign * w * dz / d;
  }
}

f64 WorldField::wind10_at(f64 x, f64 z, Vec2& to, f64& z0) const noexcept {
  const Vec2 place{static_cast<f32>(x), static_cast<f32>(z)};
  z0 = wind_.scene != nullptr ? wind_.scene->roughness_m : 0.03;
  const f64 t = wind_.sky_time_s;
  if (wind_.live.set) {
    // The sway's live branch (renderer::scene_flora_wind): the live mean and direction under the
    // scene's turbulence, the forced gust over it. A held wind is made fresh, as the sway's is.
    gfx::WindDesc held;
    held.kind = gfx::WindKind::fixed;
    held.roughness_m = z0;
    held.speed_m_s = wind_.live.mean_m_s;
    held.to_deg = wind_.live.to_deg;
    const gfx::WindSample mean = gfx::wind_at(held, t);
    to = mean.to;
    const f64 forced = gfx::wind_live_speed(wind_.live, z0) - wind_.live.mean_m_s;
    const f64 g = config_.turbulence ? gfx::wind_gust(held, mean, place, t) : mean.speed_m_s;
    const f64 u = g + (forced > 0.0 ? forced : 0.0);
    return u > 0.0 ? u : 0.0;
  }
  if (wind_.scene == nullptr || wind_.scene->kind == gfx::WindKind::none) return 0.0;
  const gfx::WindSample mean = gfx::wind_at(*wind_.scene, t);
  to = mean.to;
  return config_.turbulence ? gfx::wind_gust(*wind_.scene, mean, place, t) : mean.speed_m_s;
}

void WorldField::take(const Crown& c) noexcept {
  if (!(c.leaf_area_m2 > 0.0f)) return;
  const f64 dx = c.at.x - taking_at_.x;
  const f64 dz = c.at.z - taking_at_.z;
  const f64 d = std::sqrt(dx * dx + dz * dz);
  const u64 key = key_of(c.at);
  if (crowns_.size() < config_.max_crowns) {
    crowns_.push_back(c);
    crown_dist_.push_back(d);
    crown_key_.push_back(key);
    return;
  }
  ++stats_.crowns_dropped;
  if (config_.max_crowns == 0) return;
  // Full: the farthest kept goes if this one is nearer, by a total order (distance, then place,
  // then area and class), so which crowns are kept does not depend on the heap's implementation.
  const auto farther = [this](u32 a, u32 b) noexcept {
    if (crown_dist_[a] != crown_dist_[b]) return crown_dist_[a] < crown_dist_[b];
    if (crown_key_[a] != crown_key_[b]) return crown_key_[a] < crown_key_[b];
    if (crowns_[a].leaf_area_m2 != crowns_[b].leaf_area_m2)
      return crowns_[a].leaf_area_m2 < crowns_[b].leaf_area_m2;
    return static_cast<u32>(crowns_[a].kind) < static_cast<u32>(crowns_[b].kind);
  };
  if (far_heap_.empty()) {
    for (u32 i = 0; i < crowns_.size(); ++i)
      far_heap_.push_back(i);
    std::make_heap(far_heap_.begin(), far_heap_.end(), farther);
  }
  const u32 top = far_heap_.front();
  const bool nearer = d < crown_dist_[top] ||
                      (d == crown_dist_[top] &&
                       (key < crown_key_[top] ||
                        (key == crown_key_[top] && c.leaf_area_m2 < crowns_[top].leaf_area_m2)));
  if (!nearer) return;
  std::pop_heap(far_heap_.begin(), far_heap_.end(), farther);
  crowns_[top] = c;
  crown_dist_[top] = d;
  crown_key_[top] = key;
  std::push_heap(far_heap_.begin(), far_heap_.end(), farther);
}

void WorldField::refresh(const WorldPos& listener) noexcept {
  ++stats_.refreshes;
  crowns_.clear();
  crown_static_.clear();
  crown_key_.clear();
  crown_dist_.clear();
  far_heap_.clear();
  taking_at_ = listener;
  stats_.crowns_dropped = 0;
  if (canopy_.visit != nullptr) {
    struct Sink {
      WorldField* self;
      static void emit(void* user, const Crown& c) noexcept {
        static_cast<Sink*>(user)->self->take(c);
      }
    } sink{this};
    canopy_.visit(canopy_.user, listener, config_.reach_m, CrownSink{&sink, &Sink::emit});
  }
  stats_.crowns_seen = crowns_.size();
  Vec2 to;
  f64 z0 = 0.03;
  (void)wind10_at(listener.x, listener.z, to, z0);
  // Each crown's power at the listener per (U10 / 5)^3: what choosing and merging rank by. A crown
  // is heard at its edge when the listener is inside it (`Crown::extent_m`). The class's power and
  // the log law's denominator are the stage's and the refresh's, not a crown's (S112-2: three
  // thousand crowns a refresh on the forest staging, and a `pow` and two `log`s each were most of
  // its cost).
  const f64 law_z =
      z0 > 1e-5 ? (z0 < gfx::k_wind_height_m / 4.0 ? z0 : gfx::k_wind_height_m / 4.0) : 1e-5;
  const f64 law_inv = 1.0 / std::log(gfx::k_wind_height_m / law_z);
  const auto law = [law_z, law_inv](f64 h) noexcept {  // `loglaw(h, z0)` by its reciprocal
    const f64 hh = h > 2.0 * law_z ? h : 2.0 * law_z;
    return std::log(hh / law_z) * law_inv;
  };
  // **The cover** (S114): each crown's leaf area into its bin of sector and ring round the listener
  // (a merged group's spread over the bins its extent covers), each bin's share of a closed canopy,
  // and the metres under canopy to each ring's start along each sector.
  for (auto& row : cover_)
    for (f64& a : row)
      a = 0.0;
  for (const Crown& c : crowns_) {
    const f64 dx = c.at.x - listener.x;
    const f64 dz = c.at.z - listener.z;
    const f64 d = std::sqrt(dx * dx + dz * dz);
    f64 azimuth = std::atan2(dx, -dz);
    if (azimuth < 0.0) azimuth += 2.0 * k_pi;
    const f64 e = static_cast<f64>(c.extent_m);
    const f64 half = c.merged && e > 0.0 ? (e >= d ? k_pi : std::atan2(e, d)) : 0.0;
    const f64 lo_d = c.merged ? (d - e > 0.0 ? d - e : 0.0) : d;
    const f64 hi_d = c.merged ? d + e : d;
    const auto ring_of = [](f64 x) noexcept {
      u32 j = 0;
      while (j + 1u < k_cover_rings && x >= k_cover_edge_m[j + 1u])
        ++j;
      return j;
    };
    const u32 j0 = ring_of(lo_d);
    const u32 j1 = ring_of(hi_d);
    const f64 per = 2.0 * k_pi / k_cover_sectors;
    const i64 s0 = static_cast<i64>(std::floor((azimuth - half) / per));
    const i64 s1 = half >= k_pi ? s0 + k_cover_sectors - 1
                                : static_cast<i64>(std::floor((azimuth + half) / per));
    const f64 share = static_cast<f64>(c.leaf_area_m2) /
                      (static_cast<f64>(s1 - s0 + 1) * static_cast<f64>(j1 - j0 + 1u));
    for (i64 s = s0; s <= s1; ++s) {
      const u32 sector =
          static_cast<u32>(((s % k_cover_sectors) + k_cover_sectors) % k_cover_sectors);
      for (u32 j = j0; j <= j1; ++j)
        cover_[sector][j] += share;
    }
  }
  for (u32 s = 0; s < k_cover_sectors; ++s) {
    cover_run_[s][0] = 0.0;
    for (u32 j = 0; j < k_cover_rings; ++j) {
      const f64 r0 = k_cover_edge_m[j];
      const f64 r1 = k_cover_edge_m[j + 1u];
      const f64 ground = k_pi * (r1 * r1 - r0 * r0) / k_cover_sectors;
      const f64 lai = cover_[s][j] / ground;
      cover_[s][j] = lai < k_closed_lai ? lai / k_closed_lai : 1.0;
      cover_run_[s][j + 1u] = cover_run_[s][j] + (r1 - r0) * cover_[s][j];
    }
  }
  // Each crown's power through its path, into its sector's bed (the chosen ones are taken out of
  // their beds again below), and what choosing ranks by.
  for (Bed& b : beds_)
    b = Bed{};
  for (const Crown& c : crowns_) {
    const f64 h = static_cast<f64>(c.height_m);
    const f64 lw = law(h > 1.0 ? h : 1.0);
    f64 band[8];
    f64 path[8];
    f64 tone = 0.0;
    if (!crown_power(c, listener, lw, band, path, tone)) {
      crown_static_.push_back(0.0);
      continue;
    }
    f64 total = tone * path[4];
    for (const f64 p : band)
      total += p;
    crown_static_.push_back(total);
    add_to_bed(c, listener, lw, band, path, tone, 1.0);
  }
  // The crowns of their own: the loudest, a chosen one kept across 3 dB of hysteresis. A merged
  // group is never one of them (L6): it is the beds'.
  const u32 n = static_cast<u32>(crowns_.size());
  Vector<u64>& chosen = chosen_;
  u64 next[32] = {};
  u32 picked[32] = {};
  const u32 want = config_.crowns < 32 ? config_.crowns : 32;
  for (u32 slot = 0; slot < want; ++slot) {
    f64 best = 0.0;
    u32 best_i = ~0u;
    for (u32 i = 0; i < n; ++i) {
      f64 s = crown_static_[i];
      if (!(s > 0.0) || crowns_[i].merged) continue;
      const u64 key = crown_key_[i];
      bool taken = false;
      for (u32 j = 0; j < slot; ++j)
        taken = taken || next[j] == key;
      if (taken) continue;
      for (u32 j = 0; j < want; ++j)
        if (chosen[j] == key) s *= 2.0;  // 3 dB
      if (s > best) {
        best = s;
        best_i = i;
      }
    }
    next[slot] = best_i != ~0u ? crown_key_[best_i] : 0u;
    picked[slot] = best_i;
  }
  // Keep each chosen crown on the voice it had, so a crown that stays chosen is not restarted.
  for (u32 slot = 0; slot < want; ++slot) {
    Voice& v = voices_[slot];
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
      at = voices_[slot].key == next[j] ? slot : ~0u;
    for (u32 slot = 0; slot < want && at == ~0u; ++slot) {
      if (voices_[slot].key == 0) {
        voices_[slot].key = next[j];
        voices_[slot].has_sent = false;
        at = slot;
      }
    }
    if (at == ~0u) continue;
    // A crown of its own is the beds' no longer: its power, the same arithmetic as it went in, is
    // taken out of its bed, and its path is kept for its voice (S114).
    const Crown& c = crowns_[picked[j]];
    Voice& v = voices_[at];
    v.crown = c;
    const f64 h = static_cast<f64>(c.height_m);
    const f64 lw = law(h > 1.0 ? h : 1.0);
    f64 band[8];
    f64 tone = 0.0;
    if (crown_power(c, listener, lw, band, v.path, tone))
      add_to_bed(c, listener, lw, band, v.path, tone, -1.0);
  }
  for (u32 slot = 0; slot < want; ++slot)
    chosen[slot] = next[slot];
}

void WorldField::place(Voice& v, const WorldPos& frame) noexcept {
  if (v.voice.is_null() || !mixer_->is_live(v.voice)) {
    v.needs_place = false;
    return;
  }
  audio::SourceSpatial s;
  s.distance_model = audio::DistanceModel::None;
  s.spread = v.spread;
  s.position = relative(v.at, frame);
  ++stats_.commands;
  if (mixer_->set_source(v.voice, s)) {
    v.needs_place = false;
  } else {
    ++stats_.place_retries;  // the ring was full: placed again next tick (L3)
  }
}

void WorldField::silence(Voice& v) noexcept {
  if (!v.voice.is_null()) {
    mixer_->stop(v.voice);
    ++stats_.commands;
    ++stats_.stops;
  }
  v.voice = audio::VoiceHandle{};
  v.has_sent = false;
  v.info.sounding = false;
  v.info.level_db = -1000.0f;
}

void WorldField::describe(Voice& v, const f64 (&band_power)[9], f64 tone_hz, f64 tone_power,
                          bool two_d, f32 pan, u64 seed, u8 priority,
                          const WorldPos& frame) noexcept {
  (void)tone_power;
  f64 total = 0.0;
  for (const f64 p : band_power)
    total += p;
  const f64 level = total > 0.0 ? 10.0 * std::log10(total) : -1000.0;
  v.info.level_db = static_cast<f32>(level);
  v.info.tone_hz = static_cast<f32>(tone_hz);
  for (u32 k = 0; k < 9; ++k)
    v.info.band_db[k] =
        static_cast<f32>(band_power[k] > 0.0 ? 10.0 * std::log10(band_power[k]) : -1000.0);
  if (!(level >= config_.floor_db)) {
    silence(v);
    return;
  }
  audio::NoiseParams p;
  p.seed = static_cast<u32>(seed ^ (seed >> 32)) | 1u;
  const f64 scale = std::pow(10.0, -config_.reference_db / 10.0);
  for (u32 k = 0; k < 9; ++k)
    p.gain[k] = static_cast<f32>(std::sqrt(band_power[k] * scale));
  if (tone_hz > 0.0) p.tone = audio::noise_band(static_cast<f32>(tone_hz), k_needle_q);
  audio::SourceSpatial source;
  source.distance_model = audio::DistanceModel::None;
  source.spread = v.spread;
  if (two_d) {
    source.flags = audio::k_source_2d;
  } else {
    source.position = relative(v.at, frame);
  }
  if (v.voice.is_null() || !mixer_->is_live(v.voice)) {
    audio::NoisePlayParams play;
    play.noise = p;
    play.bus = audio::k_bus_ambient;
    play.priority = priority;
    play.pan = pan;
    play.source = source;
    v.voice = mixer_->play_noise(play);
    ++stats_.commands;
    if (!v.voice.is_null()) ++stats_.plays;
    v.sent = p;
    v.sent_tone_hz = static_cast<f32>(tone_hz);
    v.has_sent = !v.voice.is_null();
    v.info.sounding = !v.voice.is_null();
    v.needs_place = false;  // started where it stands
    return;
  }
  bool changed = !v.has_sent;
  for (u32 k = 0; k < 9 && !changed; ++k) {
    const f64 a = static_cast<f64>(p.gain[k]);
    const f64 b = static_cast<f64>(v.sent.gain[k]);
    const f64 m = a > b ? a : b;
    changed = std::fabs(a - b) > k_resend * m;
  }
  if (!changed && tone_hz > 0.0) {
    const f64 was = static_cast<f64>(v.sent_tone_hz);
    changed = std::fabs(tone_hz - was) > k_resend_tone * tone_hz;
  }
  if (changed && mixer_->set_noise(v.voice, p)) {
    ++stats_.commands;
    v.sent = p;
    v.sent_tone_hz = static_cast<f32>(tone_hz);
    v.has_sent = true;
  }
}

void WorldField::tick(const WorldPos& listener, const Vec3& forward, const WorldPos& frame,
                      bool frame_moved) noexcept {
  if (mixer_ == nullptr) return;
  const f64 dt = 1.0 / static_cast<f64>(tick_hz_);
  Vec3 velocity{};
  if (has_listener_) {
    velocity = Vec3{static_cast<f32>((listener.x - last_listener_.x) / dt),
                    static_cast<f32>((listener.y - last_listener_.y) / dt),
                    static_cast<f32>((listener.z - last_listener_.z) / dt)};
  }
  // A step faster than a running person or a cyclist is a teleport, a replay's cut or a camera
  // flown by hand, not a head moving through air: it adds no wind.
  constexpr f32 k_max_head_m_s = 15.0f;
  if (length(velocity) > k_max_head_m_s) velocity = Vec3{};
  last_listener_ = listener;
  has_listener_ = true;
  if (ticks_ % refresh_every_ == 0) refresh(listener);
  ++ticks_;

  const f64 calm = config_.calm_m_s;
  const u32 crowns = config_.crowns < 32 ? config_.crowns : 32;
  // ---- the crowns of their own
  for (u32 slot = 0; slot < crowns; ++slot) {
    Voice& v = voices_[slot];
    if (v.key == 0) continue;
    const Crown* c = &v.crown;
    Vec2 to;
    f64 z0 = 0.03;
    const f64 u10 = wind10_at(c->at.x, c->at.z, to, z0);
    const f64 height = static_cast<f64>(c->height_m);
    const f64 h = height > 1.0 ? height : 1.0;
    const f64 u = u10 * loglaw(h, z0);
    v.info.wind_m_s = static_cast<f32>(u);
    f64(&bands)[9] = v.bands;
    f64& tone_hz = v.tone_hz;
    for (f64& b : bands)
      b = 0.0;
    tone_hz = 0.0;
    if (u > calm) {
      const u32 k = static_cast<u32>(c->kind) < 3 ? static_cast<u32>(c->kind) : 0u;
      const f64 dx = c->at.x - listener.x;
      const f64 dy = c->at.y + height - listener.y;
      const f64 dz = c->at.z - listener.z;
      f64 r = std::sqrt(dx * dx + dy * dy + dz * dz);
      const f64 least = static_cast<f64>(c->extent_m) > k_crown_min_m
                            ? static_cast<f64>(c->extent_m)
                            : k_crown_min_m;
      r = r > least ? r : least;
      const f64 q = u / k_crown_wind_m_s;
      const f64 power = pow10(k_crown_db[k]) *
                        (static_cast<f64>(c->leaf_area_m2) / k_crown_area_m2) *
                        (k_crown_ref_m / r) * (k_crown_ref_m / r) * q * q * q;
      const bool needle = c->kind == CanopyClass::Needle;
      const f64 broadband = needle ? power * (1.0 - k_needle_share) : power;
      for (u32 b = 0; b < 8; ++b)
        bands[b] = broadband * shape_[k][b] * v.path[b];
      if (needle) {
        const f64 d = static_cast<f64>(c->needle_mm > 0.05f ? c->needle_mm : 0.05f) * 1e-3;
        tone_hz = k_strouhal * u / d;
        bands[audio::k_noise_tone] = power * k_needle_share * at_tone(v.path, tone_hz);
      }
    }
    v.at = WorldPos{c->at.x, c->at.y + height, c->at.z};
    v.needs_place = v.needs_place || frame_moved;
  }
  // ---- the beds
  const u32 beds = static_cast<u32>(beds_.size());
  for (u32 s = 0; s < beds; ++s) {
    Voice& v = voices_[crowns + s];
    const Bed& b = beds_[s];
    f64(&bands)[9] = v.bands;
    f64& tone_hz = v.tone_hz;
    for (f64& x : bands)
      x = 0.0;
    tone_hz = 0.0;
    if (b.count > 0 && b.total > 0.0) {
      // **Each ring hears the gust at its own place** (S114): the stand round the listener, the
      // wood beyond and the far wood each at its power-weighted place, so a gust front crossing the
      // wood reaches the bed ring by ring, and a wide sector averages over the places its power
      // comes from rather than standing on one.
      f64 wind_power = 0.0;
      f64 wind_sum = 0.0;
      f64 tone_power = 0.0;
      f64 tone_sum = 0.0;
      for (const BedRing& ring : b.ring) {
        if (ring.count <= 0 || !(ring.total > 0.0)) continue;
        Vec2 to;
        f64 z0 = 0.03;
        const f64 u10 = wind10_at(ring.px / ring.total, ring.pz / ring.total, to, z0);
        wind_power += ring.total;
        wind_sum += ring.total * u10;
        if (!(u10 > calm)) continue;
        const f64 q = u10 / k_crown_wind_m_s;
        const f64 q3 = q * q * q;
        for (u32 band = 0; band < 8; ++band)
          bands[band] += (ring.band[band] > 0.0 ? ring.band[band] : 0.0) * q3;
        if (ring.needle_weight > 0.0) {
          const f64 inv_d = ring.needle_weight_d / ring.needle_weight;
          const f64 f = k_strouhal * u10 * (ring.needle_loglaw / ring.needle_weight) * inv_d;
          const f64 p = at_tone(ring.tone, f) * q3;
          if (p > 0.0) {
            tone_power += p;
            tone_sum += p * f;
          }
        }
      }
      v.info.wind_m_s = static_cast<f32>(wind_power > 0.0 ? wind_sum / wind_power : 0.0);
      if (tone_power > 0.0) {
        bands[audio::k_noise_tone] = tone_power;
        tone_hz = tone_sum / tone_power;
      }
      // Toward where its power comes from, spread by how little it agrees on a direction.
      const f64 len = std::sqrt(b.dx * b.dx + b.dz * b.dz);
      const f64 ux = len > 1e-9 ? b.dx / len : 0.0;
      const f64 uz = len > 1e-9 ? b.dz / len : -1.0;
      const WorldPos at{listener.x + ux * k_bed_distance_m, listener.y,
                        listener.z + uz * k_bed_distance_m};
      v.spread = static_cast<f32>(1.0 - (len / b.total < 1.0 ? len / b.total : 1.0));
      const f64 mx = at.x - v.at.x;
      const f64 mz = at.z - v.at.z;
      const bool moved = mx * mx + mz * mz > 1.0;
      v.at = at;
      v.needs_place = v.needs_place || moved || frame_moved;
    }
  }
  // ---- the ears: the wind relative to the head, two 2D voices hard left and right
  {
    Vec2 to{1.0f, 0.0f};
    f64 z0 = 0.03;
    const f64 u10 = wind10_at(listener.x, listener.z, to, z0);
    const f64 u = u10 * loglaw(k_ears_height_m, z0);
    const f64 wx = u * static_cast<f64>(to.x) - static_cast<f64>(velocity.x);
    const f64 wy = -static_cast<f64>(velocity.y);
    const f64 wz = u * static_cast<f64>(to.y) - static_cast<f64>(velocity.z);
    const f64 rel = std::sqrt(wx * wx + wy * wy + wz * wz);
    // The head's right, level: forward x up, with up the world's.
    f64 rx = -static_cast<f64>(forward.z);
    f64 rz = static_cast<f64>(forward.x);
    const f64 rl = std::sqrt(rx * rx + rz * rz);
    rx = rl > 1e-9 ? rx / rl : 1.0;
    rz = rl > 1e-9 ? rz / rl : 0.0;
    const f64 side = rel > 1e-9 ? (wx * rx + wz * rz) / rel : 0.0;  // +1: blowing to the right
    const f64 corner = k_ears_corner_hz * rel / k_ears_wind_m_s;
    f64 shape[8];
    f64 sum = 0.0;
    for (u32 b = 0; b < 8; ++b) {
      const f64 f = static_cast<f64>(audio::k_noise_band_hz[b]);
      const f64 db = f <= corner ? 0.0 : -config_.ears_slope_db * std::log2(f / corner);
      shape[b] = pow10(db);
      sum += shape[b];
    }
    for (u32 e = 0; e < 2; ++e) {
      Voice& v = voices_[crowns + beds + e];
      v.info.wind_m_s = static_cast<f32>(rel);
      f64(&bands)[9] = v.bands;
      for (f64& x : bands)
        x = 0.0;
      v.tone_hz = 0.0;
      // Heard only in a wind: in still air a walker's own motion is not heard as wind at this
      // stage (a calm world is silent), though in a wind it adds to the wind the head meets.
      if (u > calm && rel > calm && sum > 0.0) {
        // The downwind ear is the louder (Seidman and others: loudest at the downwind ear with
        // the head side-on); the right ear's outward axis is +right.
        const f64 outward = e == 0 ? -side : side;
        const f64 level = k_ears_db + 40.0 * std::log10(rel / k_ears_wind_m_s) + config_.ears_db +
                          config_.ears_side_db * outward;
        const f64 power = pow10(level);
        for (u32 b = 0; b < 8; ++b)
          bands[b] = power * shape[b] / sum;
      }
    }
  }
  // ---- the world's ceiling (S112-2; world_sound.md, "The world's hold"): the voices' powers
  // summed, in full scale's units; over the ceiling every one is held by the same factor, so the
  // balance and the spectrum stay and only the whole is held. Under it nothing changes.
  f64 total = 0.0;
  for (Voice& v : voices_) {
    // One kind heard alone (`audio.world.solo`): the others are described as silent, so they hold
    // no voice and send nothing, and the kind left sounds as it does in the whole world.
    const bool ear = v.kind == WorldVoiceKind::EarLeft || v.kind == WorldVoiceKind::EarRight;
    const bool muted =
        config_.solo != 0 &&
        !((config_.solo == 1 && v.kind == WorldVoiceKind::Crown) ||
          (config_.solo == 2 && v.kind == WorldVoiceKind::Bed) || (config_.solo == 3 && ear));
    if ((v.kind == WorldVoiceKind::Crown && v.key == 0) || muted) {
      for (f64& p : v.bands)
        p = 0.0;
      continue;
    }
    for (const f64 p : v.bands)
      total += p;
  }
  const f64 full_scale = std::pow(10.0, config_.reference_db / 10.0);
  const f64 ceiling = full_scale * std::pow(10.0, config_.ceiling_db / 10.0);
  stats_.held_db = 0.0f;
  if (total > ceiling) {
    const f64 factor = ceiling / total;
    for (Voice& v : voices_)
      for (f64& p : v.bands)
        p *= factor;
    stats_.held_db = static_cast<f32>(10.0 * std::log10(factor));
    ++stats_.held_ticks;
  }
  for (u32 i = 0; i < voices_.size(); ++i) {
    Voice& v = voices_[i];
    if (v.kind == WorldVoiceKind::Crown) {
      if (v.key == 0) continue;
      describe(v, v.bands, v.tone_hz, 0.0, false, 0.0f, v.key, k_crown_priority, frame);
    } else if (v.kind == WorldVoiceKind::Bed) {
      describe(v, v.bands, v.tone_hz, 0.0, false, 0.0f, 0xbedull + (i - crowns), k_bed_priority,
               frame);
    } else {
      const bool left = v.kind == WorldVoiceKind::EarLeft;
      describe(v, v.bands, 0.0, 0.0, true, left ? -1.0f : 1.0f, left ? 0xea5ull : 0xea6ull,
               k_ears_priority, frame);
      continue;
    }
    if (v.needs_place) place(v, frame);
  }
  u32 crown_n = 0;
  u32 bed_n = 0;
  u32 ear_n = 0;
  for (const Voice& v : voices_) {
    if (v.voice.is_null()) continue;
    crown_n += v.kind == WorldVoiceKind::Crown ? 1u : 0u;
    bed_n += v.kind == WorldVoiceKind::Bed ? 1u : 0u;
    ear_n += v.kind == WorldVoiceKind::EarLeft || v.kind == WorldVoiceKind::EarRight ? 1u : 0u;
  }
  stats_.crown_voices = crown_n;
  stats_.bed_voices = bed_n;
  stats_.ear_voices = ear_n;
}

void WorldField::stop_all() noexcept {
  if (mixer_ == nullptr) return;
  for (Voice& v : voices_)
    silence(v);
}

}  // namespace engine::world_sound
