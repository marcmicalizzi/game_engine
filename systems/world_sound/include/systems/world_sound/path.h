#pragma once

// **The path's terms** (roadmap S114, the design's fourth stage; docs/subsystems/world_sound.md,
// "Distance"; the sources and the numbers chosen: docs/experiments/world-sound.md, "Read again by
// S114"). What the air, a forest and the ground take from a world voice between where it is made
// and the listener's ears, octave band by octave band, in dB: closed forms a refresh evaluates once
// a crown, the stage's field adds to a source's spreading. A band is one of the shaped-noise
// voice's eight, 62.5 Hz to 8 kHz (`audio::k_noise_band_hz`), the standards' octave bands 63 Hz to
// 8 kHz.

#include <core/base/types.h>

namespace engine::world_sound {

inline constexpr u32 k_path_bands = 8;

/// **Air absorption**, dB a metre, of a pure tone of `f_hz` in air at `temperature_c` and
/// `humidity_pct` relative humidity at one atmosphere: ISO 9613-1:1993's equations as sengpielaudio
/// reproduces them (the pure-tone coefficient, its oxygen and nitrogen relaxation frequencies, the
/// water vapour's molar concentration from the saturation pressure). ISO 9613-2 takes an octave
/// band's coefficient at its mid-band frequency, and so does the field. 70 % at 20 degC gives 4.98
/// dB a kilometre at 1 kHz and 76.6 at 8 kHz, the standard's own table (a case).
f64 air_absorption_db_per_m(f64 f_hz, f64 temperature_c, f64 humidity_pct) noexcept;

/// **The clip filter's two air figures** (S120), what the mixer's distance low-pass is handed
/// (`audio::MixerConfig::air_db_per_m_8k`, `_1k`), as f32. The function above is ISO's through
/// `std::pow` and `std::exp`, whose last bit is the C library's; the filter itself is `+ - * /`, a
/// square root and ldexp. So at the default air (20 degC, 70 %, `FieldConfig`'s and the tunables')
/// the figures are these constants, the standard's own as f32 (a case holds them to the function),
/// and a session's render is the same on every toolchain by construction, not because f32 rounding
/// happened to absorb a difference. Any other air is the function's, cast.
inline constexpr f64 k_default_air_c = 20.0;
inline constexpr f64 k_default_air_pct = 70.0;
inline constexpr f32 k_default_air_db_per_m_8k = 0x1.3dfc42p-4f;  // 0.0776331499
inline constexpr f32 k_default_air_db_per_m_1k = 0x1.4639cep-8f;  // 0.00497781066
void clip_air_db_per_m(f64 temperature_c, f64 humidity_pct, f32& db_per_m_8k,
                       f32& db_per_m_1k) noexcept;

/// **The forest's excess attenuation**, dB, over `r_m` metres of path under canopy: Hoover's
/// formula as Bies and Hansen give it, A = 0.01 r f^(1/3) (Arup's Strutt help, "Dense foliage
/// excess attenuation", citing *Engineering Noise Control* p. 227).
f64 foliage_db(f64 f_hz, f64 r_m) noexcept;

/// **The ground's term**, dB (negative is a gain), of octave band `band` (0 is 63 Hz, 7 is 8 kHz):
/// ISO 9613-2:1996's engineering method for ground that is roughly flat, as Arup's Strutt help
/// gives it ("Ground attenuation"): a source region at height `source_h_m` with ground factor `g`,
/// a receiver region at `receiver_h_m` with the same factor, and a middle region, over a distance
/// `ground_m` projected onto the ground. G is 0 for hard ground, 1 for porous (a forest floor,
/// grass, farmland).
f64 ground_db(u32 band, f64 source_h_m, f64 receiver_h_m, f64 ground_m, f64 g) noexcept;
/// The same for all eight bands at once (a refresh's form: the exponentials each band shares are
/// taken once).
void ground_bands_db(f64 source_h_m, f64 receiver_h_m, f64 ground_m, f64 g,
                     f64 (&out)[k_path_bands]) noexcept;

/// **A barrier's term**, dB, of a band at `f_hz` for a detour `delta_m` over one edge: ISO 9613-2's
/// D_z = 10 log10(3 + (C2 / lambda) C3 delta K_met) with C2 = 20, C3 = 1 and K_met = 1 (the help's
/// form), held to 20 dB at most, as the standard holds one edge; 0 for no detour (a path in sight).
f64 barrier_db(f64 f_hz, f64 delta_m) noexcept;

}  // namespace engine::world_sound
