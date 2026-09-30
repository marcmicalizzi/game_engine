// The sky provider "earth" (docs/subsystems/sky.md; scene_gen/sky.h; ADR-0048): a scene's sky entry
// made into an ephemeris on its calendar, Earth's air at its turbidity, and the star table.
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/sky.h>
#include <domain/sky/earth.h>
#include <domain/sky/ephemeris.h>
#include <domain/sky/sky.h>

#include <cmath>
#include <string>

namespace engine::sky {

namespace {

// The seed of the star table every Earth sky draws: the stars are Earth's, not a scene's.
constexpr u32 k_star_seed = 1u;

struct EarthSky {
  Calendar calendar;
  scene_gen::Atmosphere air;
  Vector<scene_gen::Star> stars;
};

void earth_destroy(void* state) noexcept { delete static_cast<EarthSky*>(state); }

Vec3 vec3(const Direction& d) noexcept {
  return Vec3{static_cast<f32>(d.x), static_cast<f32>(d.y), static_cast<f32>(d.z)};
}

void earth_state(const void* state, f64 time_s, scene_gen::SkyState& out) noexcept {
  const auto& sky = *static_cast<const EarthSky*>(state);
  Ephemeris e;
  ephemeris(sky.calendar, time_s, e);
  out = scene_gen::SkyState{};
  out.sun = vec3(e.sun);
  out.moon = vec3(e.moon);
  out.sun_illuminance = static_cast<f32>(1.0 / (e.sun_distance_au * e.sun_distance_au));
  out.moon_illuminance =
      static_cast<f32>(moon_illuminance(k_moon_albedo, k_moon_radius_km, e.moon_distance_km,
                                        e.moon_elongation_rad, e.sun_distance_au));
  out.moon_lit = static_cast<f32>(e.moon_lit);
  out.sun_radius = static_cast<f32>(k_sun_radius_km / (e.sun_distance_au * k_au_km));
  out.moon_radius = static_cast<f32>(k_moon_radius_km / e.moon_distance_km);
  out.moon_albedo = static_cast<f32>(k_moon_albedo);
  out.celestial_x = vec3(e.celestial_x);
  out.celestial_y = vec3(e.celestial_y);
  out.celestial_z = vec3(e.celestial_z);
  out.day_of_year = e.day_of_year;
  out.hour = e.hour;
}

const scene_gen::Atmosphere* earth_air(const void* state) noexcept {
  return &static_cast<const EarthSky*>(state)->air;
}

std::span<const scene_gen::Star> earth_star_table(const void* state) noexcept {
  const auto& sky = *static_cast<const EarthSky*>(state);
  return std::span<const scene_gen::Star>(sky.stars.data(), sky.stars.size());
}

constexpr scene_gen::SkyOps k_earth_ops{.destroy = &earth_destroy,
                                        .state = &earth_state,
                                        .atmosphere = &earth_air,
                                        .stars = &earth_star_table};

bool in_range(f64 v, f64 lo, f64 hi) noexcept { return std::isfinite(v) && v >= lo && v <= hi; }

bool earth_make(const scene::Sky& entry, scene_gen::SkyProvider& out, std::string* error) {
  auto refuse = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (!in_range(static_cast<f64>(entry.latitude_deg), -90.0, 90.0))
    return refuse("sky.latitude_deg must be within [-90, 90]");
  if (!in_range(static_cast<f64>(entry.day_of_year), 1.0, 366.0))
    return refuse("sky.day_of_year must be within [1, 366]");
  if (!in_range(static_cast<f64>(entry.moon_age_days), 0.0, k_synodic_month_days))
    return refuse("sky.moon_age_days must be within [0, 29.53]");
  if (!in_range(static_cast<f64>(entry.turbidity), 1.0, 10.0))
    return refuse("sky.turbidity must be within [1, 10]");
  if (!in_range(static_cast<f64>(entry.ground_albedo), 0.0, 1.0))
    return refuse("sky.ground_albedo must be within [0, 1]");
  auto* sky = new EarthSky{};
  sky->calendar.latitude_deg = static_cast<f64>(entry.latitude_deg);
  sky->calendar.day_of_year = static_cast<f64>(entry.day_of_year);
  sky->calendar.moon_age_days = static_cast<f64>(entry.moon_age_days);
  sky->air = earth_atmosphere(static_cast<f64>(entry.turbidity));
  if (entry.stars) sky->stars = earth_stars(k_star_seed, k_star_magnitude_limit);
  out = scene_gen::SkyProvider(&k_earth_ops, sky);
  return true;
}

constexpr scene_gen::SkyProviderDesc k_earth{.name = scene_gen::k_default_sky, .make = &earth_make};
const scene_gen::Registrar k_earth_registrar{k_earth};

}  // namespace

}  // namespace engine::sky
