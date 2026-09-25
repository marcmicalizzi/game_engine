#include "audio_log.h"

#include <domain/audio/audio.h>
#include <foundation/tunables/tunables.h>

#include <cmath>

namespace engine::audio {

ENGINE_LOG_CATEGORY_DEFINE(log_audio, "audio");

namespace {

// ---- tunables (ADR-0011) ----------------------------------------------------------------------
//
// Each is a budget someone might reasonably move for a platform or a game, read once by the
// constructor of the object it sizes and never again: a mixer does not grow its pool mid-game, and
// the audio thread reads none of them.

// 64 voices: the number of simultaneous sources a mix of a busy scene actually has audible at once
// is in the tens; above that the quietest are inaudible under the loudest, which is exactly the
// set the steal policy takes first. The bench prices a voice, so raising it is a measured choice.
tunables::Int voices{"audio.voices", 64, 1, 4096, "Voices in the mixer's pool"};

// Commands the ring between the controlling thread and the audio thread holds. A tick sends at
// most a few commands per voice, and the audio thread drains the ring every block, so 1024 is
// deep enough that it fills only when nothing is rendering at all.
tunables::Int command_queue{"audio.command_queue", 1024, 16, 1 << 20,
                            "Commands the controlling-to-audio ring holds"};

// Decoded clips are large (a minute of stereo at 48 kHz is 23 MB), and a game's resident set is a
// few hundred short effects plus a handful of streams-that-are-not-streamed-yet. 256 MB holds
// about eleven minutes of stereo and fails loudly past it rather than paging.
tunables::Int clip_budget_mb{"audio.clip_budget_mb", 256, 1, 1 << 16,
                             "Decoded clip memory the clip store may hold, in MiB"};

// 10 ms: short enough that a triggered sound lands within a frame at 60 Hz, long enough that the
// device thread is woken a hundred times a second and not a thousand.
tunables::Int period_frames{"audio.period_frames", 480, 32, 16384,
                            "Frames per device callback at 48 kHz"};

// How long a gain or pan change takes to arrive, and how long a stop fades: a time, not a block
// (docs/subsystems/audio.md, "Parameter changes ramp in fixed time"). 10 ms because that is the
// top of the range where a gain change stops being heard as a click and has not yet started to be
// heard as a fade: a linear ramp of length T filters the step it replaces by sinc(f T), whose
// envelope is -20 log10(pi f T) — at 10 ms, -30 dB at 1 kHz and -40 dB at 3 kHz, where the ear is
// most sensitive, against -24 dB and -33 dB at 5 ms and -10 dB at 1 ms — and psychoacoustic
// practice gates its test tones with 5 to 10 ms ramps for exactly that reason. The long end of the
// range also finishes inside one 60 Hz tick (16.7 ms), so a source the tick moves every tick
// follows it without lagging a ramp behind, and it is what v0's per-block ramp was at the default
// 480-frame period, so the default configuration sounds as it did. Read once, by the mixer.
tunables::Float ramp_ms{"audio.ramp_ms", 10.0, 1.0, 100.0,
                        "Time a gain or pan change takes to reach its target, and a stop to fade"};

// The master's look-ahead limiter (limiter.h). Off: the page's policy is that a mix too hot for its
// headroom is said so by `MixerStats::clipped_samples`, not hidden, and the limiter's look-ahead
// delays every sound by 5 ms. A game whose content cannot leave headroom turns it on.
tunables::Bool limiter{"audio.limiter", false,
                       "Run the master's look-ahead limiter in front of its hard clip"};

// The limiter's ceiling — its threshold: no sample leaves above it. -1 dBFS leaves room for what
// happens after the mix: the platform's conversion when the endpoint is not in the mix's format,
// and the inter-sample peaks a DAC's reconstruction makes of samples near full scale (the margin
// EBU R128 and the streaming services ask of a master for the same reason).
tunables::Float limiter_threshold_db{"audio.limiter.threshold_db", -1.0, -24.0, 0.0,
                                     "Limiter ceiling in dBFS: no sample leaves above it"};

// How fast the limiter lets go. 100 ms is twice the period of 20 Hz, so the gain does not ride the
// individual cycles of the lowest content — which is heard as distortion — and short enough that
// the mix is back at full level a few ticks after a burst rather than audibly ducked under it.
tunables::Float limiter_release_ms{"audio.limiter.release_ms", 100.0, 10.0, 2000.0,
                                   "Time constant of the limiter's release, in milliseconds"};

// The speaker layout the master is declared with. "auto" takes the device's own (device.h,
// `resolve_layout`); a name forces it, and a device that is something else gets the platform's
// channel conversion. The choices are indexed by `ChannelLayout`, whose `Unknown` is "auto".
constexpr const char* k_layout_choices[] = {"auto", "mono",       "stereo",     "headphones",
                                            "quad", "surround51", "surround71", "surround714"};
tunables::Enum<ChannelLayout> layout{"audio.layout", ChannelLayout::Unknown, k_layout_choices,
                                     "Speaker layout the mix is declared with; auto follows the "
                                     "device"};

}  // namespace

u32 tunable_voices() noexcept { return static_cast<u32>(voices.get()); }
u32 tunable_command_queue() noexcept { return static_cast<u32>(command_queue.get()); }
u64 tunable_clip_budget_bytes() noexcept {
  return static_cast<u64>(clip_budget_mb.get()) * 1024u * 1024u;
}
u32 tunable_period_frames() noexcept { return static_cast<u32>(period_frames.get()); }
u32 tunable_ramp_frames() noexcept {
  // Whole frames at the mix rate, rounded; at least one, which is a step.
  const f64 frames = ramp_ms.get() * static_cast<f64>(k_sample_rate) / 1000.0 + 0.5;
  return frames < 1.0 ? 1u : static_cast<u32>(frames);
}
bool tunable_limiter() noexcept { return limiter.get(); }
f32 tunable_limiter_ceiling() noexcept {
  return static_cast<f32>(std::pow(10.0, limiter_threshold_db.get() / 20.0));
}
u32 tunable_limiter_release_frames() noexcept {
  const f64 frames = limiter_release_ms.get() * static_cast<f64>(k_sample_rate) / 1000.0 + 0.5;
  return frames < 1.0 ? 1u : static_cast<u32>(frames);
}
ChannelLayout tunable_layout() noexcept { return layout.get(); }

}  // namespace engine::audio
