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
  // The civil form (W185): a place and a moment, when the entry gives them.
  bool civil = false;
  CivilCalendar civil_calendar;
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
  if (sky.civil)
    ephemeris(sky.civil_calendar, time_s, e);
  else
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

// The solar form's defaults (engine.scene.Sky): a civil sky that moves one of them gave it by hand.
constexpr f32 k_default_day_of_year = 80.0f;
constexpr f32 k_default_moon_age_days = 0.0f;

// The civil form (W185; sky.md, "The sky by place and civil time"): all four fields or none, never
// beside the solar form's day or moon age, each within its range. Empty: the entry is fine (or is
// the solar form); otherwise the sentence that refuses it.
std::string civil_refusal(const scene::Sky& entry) {
  const bool any = entry.longitude_deg.has_value() || entry.date.has_value() ||
                   entry.time_of_day_h.has_value() || entry.utc_offset_h.has_value();
  if (!any) return {};
  std::string missing;
  auto lack = [&](bool has, const char* name) {
    if (has) return;
    missing += missing.empty() ? "" : ", ";
    missing += name;
  };
  lack(entry.longitude_deg.has_value(), "sky.longitude_deg");
  lack(entry.date.has_value(), "sky.date");
  lack(entry.time_of_day_h.has_value(), "sky.time_of_day_h");
  lack(entry.utc_offset_h.has_value(), "sky.utc_offset_h");
  if (!missing.empty())
    return "a sky by place and civil time gives sky.longitude_deg, sky.date, sky.time_of_day_h and "
           "sky.utc_offset_h together; this one has no " +
           missing;
  if (entry.day_of_year != k_default_day_of_year)
    return "sky.day_of_year is given beside a civil date: the date gives the day, so drop "
           "sky.day_of_year (or the civil fields)";
  if (entry.moon_age_days != k_default_moon_age_days)
    return "sky.moon_age_days is given beside a civil date: the ephemeris gives the moon's age, so "
           "drop sky.moon_age_days (or the civil fields)";
  if (!in_range(*entry.longitude_deg, -180.0, 180.0))
    return "sky.longitude_deg must be within [-180, 180]";
  if (entry.date->year < 1900 || entry.date->year > 2100)
    return "sky.date.year must be within [1900, 2100], where the ephemeris's formulae hold";
  if (!valid_civil_date(entry.date->year, entry.date->month, entry.date->day))
    return "sky.date is not a day of the calendar (" + std::to_string(entry.date->year) + "-" +
           std::to_string(entry.date->month) + "-" + std::to_string(entry.date->day) + ")";
  if (!(std::isfinite(*entry.time_of_day_h) && *entry.time_of_day_h >= 0.0 &&
        *entry.time_of_day_h < 24.0))
    return "sky.time_of_day_h must be within [0, 24) (15.5 is 15:30)";
  if (!in_range(*entry.utc_offset_h, -14.0, 14.0))
    return "sky.utc_offset_h must be within [-14, 14] hours (-4 is EDT)";
  return {};
}

bool earth_make(const scene::Sky& entry, scene_gen::SkyProvider& out, std::string* error) {
  auto refuse = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (!in_range(static_cast<f64>(entry.latitude_deg), -90.0, 90.0))
    return refuse("sky.latitude_deg must be within [-90, 90]");
  if (const std::string civil = civil_refusal(entry); !civil.empty()) return refuse(civil.c_str());
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
  if (entry.date.has_value()) {  // the civil form, checked whole above
    sky->civil = true;
    sky->civil_calendar.latitude_deg = static_cast<f64>(entry.latitude_deg);
    sky->civil_calendar.longitude_deg = *entry.longitude_deg;
    sky->civil_calendar.utc_offset_h = *entry.utc_offset_h;
    sky->civil_calendar.days_j2000 =
        days_from_j2000(entry.date->year, entry.date->month, entry.date->day, *entry.time_of_day_h,
                        *entry.utc_offset_h);
  }
  sky->air = earth_atmosphere(static_cast<f64>(entry.turbidity));
  if (entry.stars) sky->stars = earth_stars(k_star_seed, k_star_magnitude_limit);
  out = scene_gen::SkyProvider(&k_earth_ops, sky);
  return true;
}

constexpr scene_gen::SkyProviderDesc k_earth{.name = scene_gen::k_default_sky, .make = &earth_make};
const scene_gen::Registrar k_earth_registrar{k_earth};

}  // namespace

}  // namespace engine::sky
