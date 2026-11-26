#pragma once

// **The shaped-noise voice** (docs/subsystems/audio.md, "The shaped-noise voice"; roadmap S112):
// the one voice kind that is not a clip. A seeded integer generator makes white noise; a fixed bank
// of eight octave band-pass filters, 62.5 Hz to 8 kHz, and one movable band shape it; nine gains,
// one a band, weight the bands into the voice's signal, which then goes through the same source
// model and decode stage as a clip's (it is an object with a position like any other). The world's
// wind in the crowns and at the ears (systems/world_sound) is made of these.
//
// **Why a voice kind and not band-limited clips**: a source with a spectrum would be eight clip
// voices, and the design's 27 world sources 216 voices of a pool of 64.
//
// **The bands.** Each is the audio cookbook's band-pass with a constant 0 dB peak
// (Bristow-Johnson), b0 (1 - z^-2) / (1 + a1 z^-1 + a2 z^-2) with b0 = alpha / (1 + alpha), a1 = -2
// cos w / (1 + alpha), a2 = (1 - alpha) / (1 + alpha), alpha = sin w / (2 Q), at Q = sqrt 2 (an
// octave between the -3 dB points). Such a filter passes white noise of variance s^2 at s^2 b0 (its
// squared impulse response sums to b0 exactly: computed for every band here, and held by a case),
// so each band's numerator is scaled to c = sqrt(3 b0): the generator's noise is uniform on [-1,
// 1), variance 1/3, and every band's output then has an RMS of one. **A band's gain is its RMS in
// full-scale units**, which is what the world's levels in dB SPL map onto
// (`audio.world.reference_db`). Neighbouring bands' skirts overlap, so a spectrum of eight gains is
// the bands' powers to within their overlap; a level that moves every band by one factor moves the
// voice's by exactly that factor.
//
// **Determinism.** The generator is xorshift32 (Marsaglia 2003), the samples its words over 2^31,
// the fixed bands' coefficients the literals below (rounded to f32 once, here), and the movable
// band's computed by `noise_band` with `sin_quarter` and `std::sqrt` (correctly rounded by IEEE
// 754): integer state and + - x / in f32, contraction off (ADR-0035), so the same commands are the
// same bytes on every toolchain, as the clip voices are.

#include <core/base/types.h>

namespace engine::audio {

inline constexpr u32 k_noise_bands = 8;  // the fixed octave bands
inline constexpr u32 k_noise_lanes = 9;  // and the movable band, lane 8
inline constexpr u32 k_noise_tone = 8;

// The fixed bands' centres, Hz: 62.5 x 2^k, the octave bands ISO 266 calls 63 Hz to 8 kHz.
inline constexpr f32 k_noise_band_hz[k_noise_bands] = {62.5f,   125.0f,  250.0f,  500.0f,
                                                       1000.0f, 2000.0f, 4000.0f, 8000.0f};
// The band-pass's Q: an octave wide (2^(1/2) / (2 - 1)).
inline constexpr f32 k_noise_band_q = 1.41421356f;
// The movable band's centre is held inside [this, k_noise_tone_max_hz].
inline constexpr f32 k_noise_tone_min_hz = 40.0f;
inline constexpr f32 k_noise_tone_max_hz = 12000.0f;

// One band's three coefficients: the scaled numerator c (b0 scaled to a unit-RMS output, b2 = -c)
// and the denominator's a1 and a2.
struct NoiseBand {
  f32 c = 0.0f;
  f32 a1 = 0.0f;
  f32 a2 = 0.0f;
};

// The fixed bands at 48 kHz, computed once in double and rounded to f32 (the derivation above).
inline constexpr NoiseBand k_noise_band_table[k_noise_bands] = {
    {0.0930181816f, -1.99416506f, 0.994231761f}, {0.131356105f, -1.98823082f, 0.988497078f},
    {0.185221404f, -1.97607017f, 0.977128685f},  {0.260389447f, -1.9506129f, 0.954798222f},
    {0.363781333f, -1.89541996f, 0.91177541f},   {0.501502514f, -1.76989508f, 0.832330167f},
    {0.671314597f, -1.47186017f, 0.699557781f},  {0.83859235f, -0.765587628f, 0.531175256f}};

// The movable band at `centre_hz` (held inside [k_noise_tone_min_hz, k_noise_tone_max_hz]) and `q`
// (held inside [0.5, 50]): the same filter as the fixed bands, its sine and cosine from
// `sin_quarter`, so the controlling thread computes the same coefficients on every toolchain.
NoiseBand noise_band(f32 centre_hz, f32 q) noexcept;

// **The low-pass after the bands** (S121; docs/subsystems/audio.md, "The shaped-noise voice"): a
// band-pass of the bank falls only 6 dB an octave outside its own octave, which over an octave
// band's width is about 3 dB an octave of band power, so no choice of the eight gains makes a
// spectrum fall faster than that: the low bands' skirts fill the top. A voice that must fall
// faster (the wind at the ears: 11 dB an octave over its knee) is passed through one second-order
// Butterworth low-pass after the lanes' sum and before the struck resonators, the cookbook's
// b0 (1 + 2 z^-1 + z^-2) / (1 + a1 z^-1 + a2 z^-2), b0 = (1 - cos w) / 2 / (1 + alpha), alpha =
// sin w / (2 Q), Q = 1 / sqrt 2. `b0` 0 is no low-pass, every voice's default: such a voice is
// the same bytes as before S121.
struct NoiseLowpass {
  f32 b0 = 0.0f;
  f32 a1 = 0.0f;
  f32 a2 = 0.0f;
};

// The low-pass at `corner_hz` (held inside [k_noise_tone_min_hz, k_noise_tone_max_hz]), its sine
// and cosine from `sin_quarter`, so the same corner is the same coefficients on every toolchain.
NoiseLowpass noise_lowpass(f32 corner_hz) noexcept;

// What a shaped-noise voice is told: the generator's seed (0 is taken as a fixed odd word), each
// lane's gain (its RMS in full scale; lane 8 the movable band), the movable band's coefficients
// and since S121 the low-pass after the bands. 64 bytes: one command, as a `SetSource` is.
struct NoiseParams {
  u32 seed = 0;
  f32 gain[k_noise_lanes] = {};
  NoiseBand tone{};
  NoiseLowpass lowpass{};
};

// **The struck resonators** (S113; docs/subsystems/audio.md, "The shaped-noise voice"): up to
// four damped sinusoids a voice, each struck by a command — a trickle's separate bubbles, each
// ringing at Minnaert's frequency for its radius and dying at its damping. A resonator is the
// two-pole recursion y[n] = a y[n-1] - b y[n-2], a = 2 r cos w, b = r^2, r = e^(-d / fs) for a
// damping of d a second; a strike sets y[n-1] = 0 and y[n-2] = -A sin w / r^2, so the resonator
// then rings A r^n sin(w (n + 1)) from the first frame of the next block, replacing whatever that
// resonator still rang (the controlling thread takes them in turn). It rings for `life` frames,
// until r^n is 1e-4, and is then still. Its samples are added to the voice's signal before the
// voice's gain and spatial block, in full scale's units.
inline constexpr u32 k_noise_resonators = 4;

// What a strike tells a resonator: which, its coefficients, its kick and how long it rings.
struct NoiseStrike {
  u32 resonator = 0;  // < k_noise_resonators
  f32 a = 0.0f;
  f32 b = 0.0f;
  f32 kick = 0.0f;
  u32 life = 0;  // frames
};

// A bubble of `frequency_hz` (held inside [k_noise_tone_min_hz, k_noise_tone_max_hz]) dying at
// `damping` a second (held inside [1, 20,000]) with a peak of `amplitude` (full scale), on
// resonator `resonator`. Its sine and cosine are `sin_quarter`'s and r is e^(-d / fs) by five terms
// of its series (d / fs under 0.42, where the fifth term is under 1e-4 of the first), so the same
// bubble is the same coefficients on every toolchain.
NoiseStrike noise_strike(u32 resonator, f32 frequency_hz, f32 damping, f32 amplitude) noexcept;

// The audio thread's state of one shaped-noise voice: the nine filters' two delays, their
// coefficients (the fixed bands' copied in at the play, so one loop runs every lane), the gains
// and where a ramp takes them, the generator, and since S113 the four struck resonators. A
// parallel array of the pool, sized at construction; read only for a voice whose kind is noise.
struct alignas(32) NoiseState {
  f32 s1[k_noise_lanes] = {};
  f32 s2[k_noise_lanes] = {};
  f32 c[k_noise_lanes] = {};
  f32 a1[k_noise_lanes] = {};
  f32 a2[k_noise_lanes] = {};
  f32 gain[k_noise_lanes] = {};
  f32 target[k_noise_lanes] = {};
  f32 step[k_noise_lanes] = {};
  u32 rng = 1;
  u32 ramp = 0;  // frames of the gains' ramp still to run
  // The resonators (S113): their coefficients, their two delays and the frames each still rings.
  f32 res_a[k_noise_resonators] = {};
  f32 res_b[k_noise_resonators] = {};
  f32 res_y1[k_noise_resonators] = {};
  f32 res_y2[k_noise_resonators] = {};
  u32 res_life[k_noise_resonators] = {};
  // The low-pass after the bands (S121): its coefficients (b0 0: none) and its two delays.
  f32 lp_b0 = 0.0f;
  f32 lp_a1 = 0.0f;
  f32 lp_a2 = 0.0f;
  f32 lp_s1 = 0.0f;
  f32 lp_s2 = 0.0f;
};

// A strike applied to a voice's state (the mixer's audio thread; public for the cases).
void strike_noise(NoiseState& state, const NoiseStrike& strike) noexcept;

// The generator's first word for a seed: never 0, which xorshift would keep.
constexpr u32 noise_seed(u32 seed) noexcept { return seed != 0 ? seed : 0x9E3779B9u; }

// `frames` samples of a voice's signal into `out` (mono), advancing its generator, its filters and
// its ramp. The mixer's voice loop; public so the bench and the cases run it alone.
void render_noise(NoiseState& state, f32* out, u32 frames) noexcept;

}  // namespace engine::audio
