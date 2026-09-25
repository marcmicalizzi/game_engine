#pragma once

// audio capability (ADR-0027; docs/plan/02-architecture.md §2.8, docs/plan/05-simulation.md
// §5.11, docs/plan/11-performance-principles.md §11.10). The page is docs/subsystems/audio.md.
//
// The device and mixing half of the plan's audio design ("miniaudio for devices and mixing; Steam
// Audio for spatialization and occlusion"), shaped by the direction note beside it in §5.11: an
// object-based sound stage, decoded last. This module owns output devices (and a null backend that
// is the same mix with no device), decoded clips, a fixed pool of voices that are sources — objects
// with a spatial block — a bus tree whose master declares its speaker layout, one isolated decode
// stage from sources to that layout, and the master's hard clip; and it is driven through two
// lock-free queues so the audio thread never waits for the simulation. It does **not** know about
// entities: the ECS half — the `AudioEmitter` and `AudioListener` components and the system that
// turns their changes into commands — is `systems/audio_system`, because `<flecs.h>` belongs to
// `systems/` and not to a domain module (ADR-0028 seam 5).
//
//   [x] schema types      schemas/audio.schema: the components, the enumerations the mixer's API
//                         uses, and audio.devices' result
//   [ ] scheduler entry   none here; systems/audio_system registers the emitter system
//   [ ] render passes     none: audio draws nothing
//   [x] derived data      clip_file.h: the `.clip` a clip's source is decoded into once, keyed
//                         by its bytes; engine-content builds it through content_build's
//                         derived-step table, and the clip store loads it as a copy or a stream
//   [x] protocol methods  audio.devices, register_methods() in protocol.h
//   [x] tunables          audio.voices, audio.command_queue, audio.clip_budget_mb,
//                         audio.period_frames, audio.layout, audio.ramp_ms, audio.limiter,
//                         audio.limiter.threshold_db, audio.limiter.release_ms,
//                         audio.stream_threshold_kb, audio.streams, audio.stream_ring_ms,
//                         audio.stream_fill_ms — read at construction, never in the callback
//   [x] LOD policy        the emitters' is systems/audio_system's; the mixer's cost is set by its
//                         voice count, which is the budget
//   [x] determinism       k_determinism below
//   [x] zero cost unused  no linked code with ENGINE_WITH_AUDIO=OFF; linked and unused, no
//                         thread, no device and no allocation until a Mixer or Output exists
//   [x] docs, tests, size table, bench
//
// **Determinism: derived.** Nothing the mix produces is read back into the simulation, so it is
// not in the sim hash (ADR-0010). It is nevertheless *reproducible*: the same clips, the same
// layout and the same sequence of commands and render calls give the same bytes on every compiler
// (see mixer.h), and the tests pin a hash of it per layout, because a mix that is a pure function
// of its inputs is what makes an audio bug replayable.

#include <core/base/types.h>
#include <domain/audio/clip_file.h>
#include <domain/audio/clip_store.h>
#include <domain/audio/commands.h>
#include <domain/audio/decoder.h>
#include <domain/audio/device.h>
#include <domain/audio/format.h>
#include <domain/audio/limiter.h>
#include <domain/audio/mixer.h>
#include <domain/audio/spatial.h>
#include <domain/audio/stream.h>

#include <schemas/audio.h>

namespace engine::audio {

// ADR-0010's stance, in the header as the ADR requires.
inline constexpr const char* k_determinism = "derived";

// The tunables' current values (ADR-0011), for code that sizes something from them. Each is read
// once by whoever constructs the object it sizes.
u32 tunable_voices() noexcept;             // audio.voices, default 64
u32 tunable_command_queue() noexcept;      // audio.command_queue, default 1024
u64 tunable_clip_budget_bytes() noexcept;  // audio.clip_budget_mb, default 256 MB
u32 tunable_period_frames() noexcept;      // audio.period_frames, default 480 (10 ms)
u32 tunable_ramp_frames() noexcept;        // audio.ramp_ms, default 10 ms: 480 frames
bool tunable_limiter() noexcept;           // audio.limiter, default off
// audio.limiter.threshold_db, default -1 dBFS, as a linear ceiling (through the C library's pow).
f32 tunable_limiter_ceiling() noexcept;
u32 tunable_limiter_release_frames() noexcept;  // audio.limiter.release_ms, default 100 ms
// audio.layout, default `Unknown` — spelled "auto": take the device's own layout.
ChannelLayout tunable_layout() noexcept;
// audio.stream_threshold_kb, default 1875 KiB (five seconds of stereo): a clip decoded larger than
// this streams instead of being held whole.
u64 tunable_stream_threshold_bytes() noexcept;
u32 tunable_streams() noexcept;             // audio.streams, default 16
u32 tunable_stream_ring_frames() noexcept;  // audio.stream_ring_ms, default 500 ms
u32 tunable_stream_fill_frames() noexcept;  // audio.stream_fill_ms, default 250 ms

}  // namespace engine::audio
