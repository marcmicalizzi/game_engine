#pragma once

// Sky providers (docs/subsystems/scene_gen.md, "Sky providers"; ADR-0048): the third kind of scene
// generator. A scene names its sky (`engine.scene.Sky`, `provider`), a capability registers the
// provider under that name, and the renderer — which draws a sky and names no capability — asks it,
// once a frame, where the sun, the moon and the heavens are at the frame's game time, and once a
// scene what the air is made of and which stars there are.
//
// **What is the provider's and what is the renderer's.** The *model* of a sky is content: an
// atmosphere's coefficients, the ephemeris that places a sun and a moon on a calendar at a
// latitude, a table of stars. Those are the provider's, and removable with its capability (the
// minimal build has none, and a scene naming a sky there is refused with the registry's sentence).
// Drawing pixels from them — the scattering integrals, the discs, the stars' spots, the exposure —
// is the renderer's and `domain/gfx`'s, and needs nothing from a provider but the numbers below. A
// studio with another planet, two moons or a real catalogue writes a provider; nothing in the
// renderer changes.
//
// **One clock.** A provider's `state` is a function of game time alone — seconds since the world's
// epoch, which is midnight — the same time the ground's wind reads (`GroundOps::wind`), so the
// sun's hour, the wind's day and the moon's month cannot drift apart. The renderer evaluates it at
// the time the ground's surface stands at plus the frame's own offset (renderer.md, "One clock").

#include <core/base/types.h>
#include <core/math/math.h>

#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::scene_gen {

// Where the lights and the heavens are at one game time, in the world's frame: y up, north -z, east
// +x (the frame the renderer's camera paths call "looking north" along -z).
struct SkyState {
  Vec3 sun{0.0f, 1.0f, 0.0f};    // unit, towards the sun
  Vec3 moon{0.0f, -1.0f, 0.0f};  // unit, towards the moon
  // The sun's illuminance outside the atmosphere, in units of its mean (1 at one astronomical unit;
  // the orbit's eccentricity moves it 3.4% over a year).
  f32 sun_illuminance = 1.0f;
  // The moon's, in the same units: its albedo, its size at its distance and its phase. About 1.6e-6
  // at full, nothing at new.
  f32 moon_illuminance = 0.0f;
  // Its illuminated fraction as seen from here, 0 new to 1 full: reported, since the renderer
  // shades the moon's disc from the two directions and needs no phase of its own.
  f32 moon_lit = 0.0f;
  f32 sun_radius = 4.65e-3f;   // angular radius, radians
  f32 moon_radius = 4.52e-3f;  // angular radius, radians
  f32 moon_albedo = 0.12f;     // what the disc is shaded with, and what `moon_illuminance` implies
  // The world directions of the celestial frame's axes: x towards the vernal equinox, z the north
  // celestial pole, right-handed. A star at celestial direction d is at world `x d.x + y d.y + z
  // d.z`, so the stars turn about the pole with the sidereal day.
  Vec3 celestial_x{1.0f, 0.0f, 0.0f};
  Vec3 celestial_y{0.0f, 1.0f, 0.0f};
  Vec3 celestial_z{0.0f, 0.0f, 1.0f};
  // The calendar at that time, for a summary: the day of the year (1 is January the first,
  // fractional) and the local solar hour, [0, 24).
  f64 day_of_year = 1.0;
  f64 hour = 0.0;
};

// The air (Bruneton and Neyret 2008, Hillaire 2020): kilometres and inverse kilometres. Rayleigh
// and Mie fall off exponentially with height; ozone absorbs in a tent about its peak.
struct Atmosphere {
  f32 bottom_radius_km = 6360.0f;
  f32 top_radius_km = 6460.0f;
  Vec3 rayleigh_scattering{5.802e-3f, 13.558e-3f, 33.1e-3f};  // at the ground, for 680, 550, 440 nm
  f32 rayleigh_height_km = 8.0f;
  Vec3 mie_scattering{3.996e-3f, 3.996e-3f, 3.996e-3f};
  Vec3 mie_extinction{4.40e-3f, 4.40e-3f, 4.40e-3f};
  f32 mie_height_km = 1.2f;
  f32 mie_g = 0.8f;
  Vec3 ozone_absorption{0.650e-3f, 1.881e-3f, 0.085e-3f};  // at the layer's peak
  f32 ozone_peak_km = 25.0f;
  f32 ozone_half_width_km = 15.0f;
  // The night sky's own glow above the horizon, sun units per steradian: airglow, zodiacal light
  // and the stars too faint to list. A moonless night's sky is about 2e-4 cd/m^2.
  Vec3 night_glow{1.4e-9f, 1.6e-9f, 1.9e-9f};
};

// One star: its direction in the celestial frame, its visual magnitude and its colour (linear, with
// a luminance of 1: the magnitude says how bright).
struct Star {
  Vec3 direction{0.0f, 0.0f, 1.0f};
  f32 magnitude = 0.0f;
  Vec3 color{1.0f, 1.0f, 1.0f};
};

// A sky's functions over the state `make` allocated.
struct SkyOps {
  void (*destroy)(void* state) noexcept = nullptr;
  // The lights and the heavens at game time `time_s`. Thread-safe; a closed function of the time.
  void (*state)(const void* state, f64 time_s, SkyState& out) noexcept = nullptr;
  // The air, the same at every time.
  const Atmosphere* (*atmosphere)(const void* state) noexcept = nullptr;
  // The stars, brightest first; empty for a sky that draws none.
  std::span<const Star> (*stars)(const void* state) noexcept = nullptr;
};

class SkyProvider {
 public:
  SkyProvider() noexcept = default;
  SkyProvider(const SkyOps* ops, void* state) noexcept : ops_(ops), state_(state) {}
  ~SkyProvider() { reset(); }
  SkyProvider(SkyProvider&& other) noexcept : ops_(other.ops_), state_(other.state_) {
    other.ops_ = nullptr;
    other.state_ = nullptr;
  }
  SkyProvider& operator=(SkyProvider&& other) noexcept;
  SkyProvider(const SkyProvider&) = delete;
  SkyProvider& operator=(const SkyProvider&) = delete;
  void reset() noexcept;

  bool valid() const noexcept { return ops_ != nullptr; }
  void state(f64 time_s, SkyState& out) const noexcept { ops_->state(state_, time_s, out); }
  const Atmosphere& atmosphere() const noexcept { return *ops_->atmosphere(state_); }
  std::span<const Star> stars() const noexcept {
    return ops_->stars != nullptr ? ops_->stars(state_) : std::span<const Star>{};
  }

 private:
  const SkyOps* ops_ = nullptr;
  void* state_ = nullptr;
};

// A sky provider: made from a scene's sky entry. `make` checks the entry and says why it cannot be
// drawn with a sentence.
struct SkyProviderDesc {
  const char* name = nullptr;
  bool (*make)(const scene::Sky& entry, SkyProvider& out, std::string* error) = nullptr;
};

// The provider an entry names: its `provider`, or "earth" when that is empty — the sky
// capability's, the one a scene written before a second existed means.
inline constexpr const char* k_default_sky = "earth";
inline std::string_view sky_provider_name(const scene::Sky& entry) noexcept {
  return entry.provider.empty() ? std::string_view(k_default_sky)
                                : std::string_view(entry.provider);
}

}  // namespace engine::scene_gen
