# audio (domain)

**Purpose.** The device and mixing half of the plan's audio design ([05 §5.11](../plan/05-simulation.md#511-integration-notes): "miniaudio for devices and mixing; Steam Audio for spatialization and occlusion"), shaped by the direction note beside it — **audio is an object-based sound stage, decoded last**. The module owns output devices and a null backend that is the same mix with no device behind it, decoded clips, a fixed pool of voices that are *sources* (objects with a spatial block, never pre-panned channel pairs), a bus tree whose master declares its speaker layout, one isolated decode stage from sources to that layout, and the master's hard clip; it is driven through two lock-free queues so the audio thread never waits for the simulation. It knows nothing about entities: the `AudioEmitter` and `AudioListener` components and the system that turns their changes into commands are [audio_system](audio_system.md), because `<flecs.h>` belongs to `systems/` and not to a domain module ([ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 5). An optional capability ([ADR-0027](../adr/0027-additive-capabilities.md)): `ENGINE_WITH_AUDIO=OFF`, or a `*-minimal` preset, takes the module, its schema library, its tests, its bench and the miniaudio download out of the configuration, and `audio_system` with it.

## The shape v0 takes, and what it fixes now

The direction note in [05 §5.11](../plan/05-simulation.md#511-integration-notes) ("Direction note, 2026-09-24: audio is an object-based sound stage, decoded last") says what the engine's audio is to become and names five things a first version must not get wrong, because they cannot be retrofitted. Its second sentence puts the decode at the furthest calibrated stage — ideally the sound card or the receiver, which already knows the room — and that is why the note's "fourth output", an object stream to an external renderer, is a decoder like the others rather than a special case. v0 builds none of the later decoders; it builds the shape they plug into. Item by item:

1. **The mixer's unit is a source with its spatial block.** A voice is a clip plus a `SourceSpatial` — position, orientation (a quaternion), distance model and its two distances, a directivity pattern (omni or a cone, stored as cosines), spread, a 2D flag and a channel mapping — 56 bytes, carried whole by every `Play` and `SetSource` command (which is why a command is 128 bytes; [commands.h](../../domain/audio/include/domain/audio/commands.h) has the arithmetic). Nothing upstream of the decode stage produces a channel gain. The stereo panner ignores the parts of the block it cannot use — the direction's vertical and front/back components, orientation beyond what the cone does with it — and they are there for the decoders that can.
2. **The master carries a declared layout, and the decode is one stage behind an interface.** The layout is a **profile from a shipped table** ([format.h](../../domain/audio/include/domain/audio/format.h)): mono, stereo, headphones, quad, 5.1, 7.1 and 7.1.4, each row plain data — the enumeration, the channel count, the channel order as speaker roles (which is the mapping onto the device's channel map) and each channel's azimuth and elevation at the canonical ITU-R angles (BS.775 for 5.1; BS.2051 Systems I and J for 7.1 and 7.1.4, whose height ring is at 45 degrees up). The decode stage is a `Decoder` — a constant table of two function pointers and the size of the per-source state it wants ([decoder.h](../../domain/audio/include/domain/audio/decoder.h)) — with the **stereo panner** as its first implementation and a **null decoder** for tests. A later VBAP or ambisonic decoder reads every row's positions; a calibration file overrides the same rows; neither touches the mixer.
3. **Non-diegetic sources are 2D sources, flagged on the source and on the bus.** A source with `k_source_2d`, or any source on a bus declared 2D (the default tree's music bus), bypasses the source model — no distance, no direction — and carries a `ChannelMapping` from its own channels to the layout: `FrontPair` (the default: stereo left and right to the front pair at unity, mono panned between them at constant power) or `Direct` (source channel *k* to layout channel *k*, for a stem authored for the layout). The panner reads the mapping; it does not know what music is.
4. **The device reports the endpoint's real layout and rate, and the mixer's layout is chosen from it.** Enumeration opens each playback endpoint in its native format without starting it and reads the channel map the platform reports (WASAPI's speaker mask, PulseAudio's channel map), classifying it against the table; the owner's default device, a Sound Blaster X3, comes back as 7.1 at 48 kHz. `resolve_layout()` returns the `audio.layout` setting when it names a profile, the device's own when the setting is `auto` (the default), and stereo only when neither says — the consumer path, with no configuration. The device is then opened with the mixer's layout and its channel map, so a stereo setting on a 7.1 endpoint is the platform's conversion and not an assumption inside the mix.
5. **The determinism claim includes the layout.** Same commands, same clips, same layout, same bytes: `hash_mix()` covers the layout as well as the samples, and the determinism test pins one hash for stereo and one for 5.1.

Headphones in v0 is the stereo profile — the panner feeds the same two channels the same way — and becomes binaural when Steam Audio arrives. An endpoint cannot say it is a pair of headphones through its channel map, so the headphones profile is chosen by the setting until the platform's endpoint form factor is read.

## Owned data

The `Mixer` owns the voice pool on both sides (the controlling thread's `VoiceSlot`s and the audio thread's `VoiceState`s and `SourceSpatial`s), the decode stage's per-source state, the bus tree, the listener basis, both rings and the stats seqlock. The `ClipStore` owns every decoded clip's samples, and nothing frees one while the store lives; the voices hold raw pointers into it. An `Output` owns the device and the miniaudio context it came from. Nothing else may hold or mutate any of it.

## The pipeline, per block

```
render(out, frames):  apply every queued command (at most a ring's worth)
                      compose the bus gains, root first
                      for each 1024-frame block of the call:
                        for each voice, in slot order:
                          the voice loop:  clip at its pitch -> scratch signal       (no gain)
                          the level:       gain x bus path x source model's attenuation
                          the decode:      signal + SpatialParams -> layout channels  (decoder.h)
                          ended? -> free the slot, post a VoiceEvent
                        master: hard clip, peak, count
                      publish the stats
```

**The voice loop** reads the clip through a **32.32 fixed-point playhead** with linear interpolation between neighbouring frames. It runs in segments that keep both interpolation taps inside the clip, so the inner loop has no bounds test, no end test and no branch on the data; the one frame whose right tap would be past the end is handled outside it (the loop's first frame for a loop, silence for a one-shot). The channel count is a template parameter chosen once per voice per block, because it changes the loop's shape ([11 §11.4](../plan/11-performance-principles.md#114-branch-free-hot-paths-and-constexpr-dispatch)). At pitch 1 the fraction is zero and the interpolation returns the sample unchanged, bit for bit.

**Why pitch is the engine's own interpolation and not miniaudio's resampler per voice.** miniaudio's linear resampler is used where quality matters more than per-sample cost — once per clip, at decode time, with its low-pass filter (below). Per voice it would cost a per-frame loop with branches and a heap block of state per voice, a loop across a clip's end would take two calls, and its rate change carries a defect this module would inherit: `ma_linear_resampler_adjust_timer_for_new_rate` multiplies two `u32`s that each go up to the reduced output rate, and `set_rate_ratio` reduces over 10^6, so a pitch whose ratio does not reduce (a semitone, 1.059463) overflows and jumps the playhead by a fraction of a sample at every pitch change. The engine's loop is ten lines, has none of these, and is the thing the bench prices.

**Parameter changes ramp** linearly across each block from where the last block ended, so a gain or pan change is never a click; a new voice starts at its full level (ramping it up would smear every percussive attack across a block), and a stop ramps to zero over one block and then frees the voice.

## The thread model

Five lines:

1. **One controlling thread** — the tick (through [audio_system](audio_system.md)), or whatever single thread drives audio — owns the top half of `Mixer`: it allocates voices, writes fixed-size commands into a single-producer single-consumer ring, and drains ended-voice events from a second one.
2. **One audio thread** — the device's callback, or the caller's own thread under the null backend — owns `render()`: it applies every queued command at the top of each call, mixes, decodes, clips, posts ended-voice events, and publishes a stats snapshot through a seqlock.
3. **The audio thread allocates nothing, takes no lock and logs nothing**: every buffer it touches was sized in the constructor, both rings are wait-free, and the seqlock's writer never waits.
4. **Clips are decoded on the job system's Efficiency pool** into engine-owned buffers under a byte budget, and a voice gets a clip's pointer only once the clip is Ready — a release store its decode job makes last.
5. **Nothing crosses from the audio thread into simulation state**: an ended-voice event only frees a slot on the controlling side.

**Why two queues and not a mutex.** The audio thread runs at real-time priority (the output registers miniaudio's device thread with MMCSS as "Pro Audio" on Windows; elsewhere it runs at the highest priority the platform grants) and has a deadline every period; a mutex around the voices is a way for a lower-priority thread — the tick, preempted while holding it — to make the audio thread wait past that deadline, which is priority inversion, heard as a dropout. With a ring each way, both sides are wait-free: a push is one store into a slot and a release store of an index, a pop the mirror image, and each side re-reads the other's index only when its cached copy says the ring is full or empty. The worst case of the audio thread's call is then bounded by the ring's capacity and the voice count, not by anything the tick does. And the commands are an ordered log: the mixer's state is a function of the commands it has applied, which is what makes the mix reproducible from a command sequence at all.

**Why the rings cannot overflow in a way that matters.** The command ring (`audio.command_queue`, 1024) refuses a push when full and `ControlStats::queue_full` counts it; a refused **stop** is kept and re-sent by `update()`, because a lost stop is a loop that plays for ever. The event ring holds twice the pool, and `play()` drains it before it starts a voice: between two drains at most one voice starts and each voice ends at most once, so it can never hold more than the pool plus one. `MixerStats::events_dropped` exists to say so, and the tests assert it stays zero.

**Why the mix runs on the device's thread, not on the Efficiency pool** that plan 11 §11.5 names for audio mixing: on a machine without efficiency cores that pool runs at below-normal priority, and the one property an audio callback must have is that nothing else on the machine can make it late. Decoding, which can be late, is the Efficiency pool's.

## The voice policy

A play takes a free voice if there is one — the lowest free slot, so allocation is a function of the command sequence. With none free it **steals** the voice that matters least among those it may take: a voice already stopping first (it is fading out anyway), otherwise the lowest priority, the oldest among equals. A **one-shot may take an equal priority**, so the newest of a burst of equal-priority sounds is heard — the usual answer for footsteps and gunfire; a **loop may take only a lower one**. That asymmetry was found, not designed: with "equal steals" for everything, three looping emitters of one priority over a two-voice pool took the pool from each other on every tick — each stolen loop asked again, stole the oldest, and was stolen in turn — and the emitter system's test caught it as three plays a tick where it expected none. If nothing is takeable the play is **refused**, `play()` returns a null handle and `ControlStats::refused_pool` counts it. A stolen voice is replaced at once, without a fade: the steal is a `Play` into a live slot, and its old generation's commands are refused on both sides from then on. The pool is `audio.voices` (64): the number of sources a busy scene has audible at once is in the tens, and past that the quietest are masked by the loudest, which is exactly the set the policy takes first.

## The mix format

**48 kHz, 32-bit float, interleaved, one rate for everything.** Every clip is converted to it when it is decoded, off the audio thread, so the voice loop never converts a format and a voice at pitch 1 reads its samples unchanged. 48 kHz rather than 44.1 because it is what current output devices run at natively, so the device path usually resamples nothing. The rate is a constant rather than a tunable: every clip and every voice step is expressed against it, which makes it a data-selected dimension, and [ADR-0011](../adr/0011-tunables-before-calibration.md) keeps those out of the tunables. The channel count is the declared layout's (above).

## Sources are objects

The **source model** — `spatialize()` in [spatial.h](../../domain/audio/include/domain/audio/spatial.h) — turns a 3D source's block and the listener's basis into a `SpatialParams`: the unit direction toward the source in listener space (+x right, +y up, -z ahead), an attenuation, and the spread. That struct is the seam Steam Audio fills later from its own simulation, adding occlusion, transmission and air absorption to the attenuation; the mixer multiplies the voice's level by the attenuation and hands the rest to the decode stage.

**The distance laws**, between `min_distance` (full level at and inside it) and `max_distance`:

| Model | Law | At `max_distance` |
|---|---|---|
| `InverseTapered` (default) | `(min / d) · (max − d) / (max − min)` | silent |
| `Linear` | `(max − d) / (max − min)` | silent |
| `None` | 1 | full |

Inverse distance is the free-field law (−6 dB per doubling). The taper is there so the level **reaches zero** at `max_distance` instead of stopping at min/max and staying audible for ever, which is what lets [audio_system](audio_system.md)'s LOD policy take a voice away past `max_distance` without anyone hearing it go. Both are continuous and never rise with distance, and the tests walk them to check.

**The cone** is OpenAL's model — full level inside the inner cone around the source's facing (its −z axis, rotated by its orientation), `cone_outer_gain` outside the outer one — blended linearly in the *cosine* between them, so the audio thread never takes an inverse cosine; the widths arrive in degrees and are turned into cosines once, on the controlling thread, by `make_cone()`. **Spread** narrows the stereo panner's image toward the centre (a fully spread source is centred); a VBAP decoder will spread it over speakers instead.

**The listener** is reduced to an orthonormal basis once, when it is set, and the audio thread reads the basis: right = forward × up (core/math's right-handed, y-up, −z-ahead convention), with a degenerate forward or up repaired rather than propagated.

## The decode stage

`Decoder` is `{name, state_floats, supports(layout), decode(input, layout, state, out)}`. The mixer calls `decode` once per source per block with a `DecodeInput` — the source's signal after pitch and before any gain, its level, whether it is fresh, whether it is 2D and its resolved mapping and pan, the `SpatialParams`, and pointers to the whole `SourceSpatial` and the listener basis for a decoder that emits objects rather than channels — and the decoder adds the source into the layout's interleaved channels, keeping whatever per-source state it declared (the stereo panner keeps four gains: its last block's, where the next ramp starts).

**Why a table of function pointers and not a virtual interface**: registration points in this engine are constant-initialized tables ([ADR-0027](../adr/0027-additive-capabilities.md), [02 §2.8](../plan/02-architecture.md#28-adding-a-capability)), and the call is once per source per block with the loop over frames inside it, so there is one indirect call per source per block and none in the loop. A decoder is chosen when the mixer is built and never changes under it; one that does not support the declared layout is replaced by the stereo panner with a log line.

**The stereo panner** pans a mono 3D source between the layout's front pair at constant power (left = cos θ, right = sin θ, θ = (pan + 1)·π/4, so the power is 1 anywhere and the centre is −3 dB per channel) by the lateral component of its direction, narrowed by its spread; a stereo source by balance (unity at the centre — a constant-power law would take 3 dB off a centred music bed for nothing); a 2D source by its mapping and pan. On a profile wider than stereo it feeds the front pair and leaves every other speaker silent, which is honest about being a stereo panner; the tests assert that 5.1 and 7.1.4 carry exactly the stereo mix on channels 0 and 1 and zeros elsewhere. A mono profile gets the fold: a mono source at its level, a stereo one at half of each channel. **The null decoder** writes nothing — every voice still plays, ends and reports, which is what makes it the proof that the stage is one.

## Buses

A tree of `BusDesc {name, parent, gain, two_d, mapping}` in which every bus's parent is an earlier bus, so one forward pass per block composes each bus's effective gain as the product of the gains on its path to the master, and a voice's level is its own gain times its bus's effective gain. The default tree is master ← {music (2D), sfx, voice, ambient}; a configured tree may add any depth (the test hangs footsteps under sfx). A malformed tree — a parent that is not an earlier bus, a gain that is not finite — is replaced by the default one with a log line rather than half-honoured. Buses are gains and flags in v0, not submixes: there is no per-bus signal to run an effect on yet (see "Not yet").

## The master

**A hard clip at full scale, counted.** After every voice is decoded, each output sample is clamped to [−1, 1], the largest magnitude before the clamp is published as `MixerStats::peak`, and every sample the clamp changed is counted in `MixerStats::clipped_samples`. That is the stated policy, and it is deliberately not a limiter: a limiter hides a mix that is too hot, where a counter says so, and the engine's default gains are unity with headroom left to the content. The clamp is branch-free (`maxss`/`minss`) and exact, so it costs nothing the determinism claim has to account for.

## Clips

`ClipStore::load(key, bytes)` copies the bytes and decodes them — inline with no job system, on the **Efficiency pool** with one — into f32 at 48 kHz, mono or stereo: WAV, FLAC and MP3 through miniaudio's built-in decoders (dr_wav, dr_flac, dr_mp3), a rate other than 48 kHz through miniaudio's linear resampler with its low-pass filter at the highest order it offers (`MA_MAX_FILTER_ORDER`, since this runs once per clip and never per block), and more than two channels folded to stereo by miniaudio's channel converter. `add_pcm` takes samples already in the mix format. A key the store already holds returns the existing handle; `clip_key(bytes)` is two independent 64-bit hashes of the bytes, and an asset id works as a key just as well.

**The budget.** `audio.clip_budget_mb` (256 MB, about eleven minutes of stereo) caps the decoded bytes the store holds. A clip that would exceed it is refused as `OverBudget`, counted and logged, rather than evicting another: an evicted clip may be under a playing voice, and the store cannot know that — eviction needs the mixer's view of which clips are playing, and is on the "Not yet" list. **Lifetime**: a clip's samples do not move or disappear while the store lives, which is the guarantee a voice's raw pointer rests on; the store must outlive every mixer that plays from it, and its destructor waits for decodes in flight.

## Devices and the null backend

`enumerate_devices()` initializes the first platform backend that will start — WASAPI on Windows; PulseAudio, then ALSA, on Linux — and lists its playback devices, opening each one unstarted to read the layout and rate its endpoint reports. **Zero devices is an answer**: the CI runners and the headless GPU server have none, and the result is an empty list with the backend's name, or `"none"` when no backend would start at all, which the test accepts as readily as the eight devices on the development desktop. The Linux backends are loaded at run time, so building needs no audio headers or libraries and a machine without them runs the same binary and finds nothing.

An `Output` opens a device (named, or the default) in the mixer's layout and lets miniaudio's device thread call `Mixer::render()` once per period (`audio.period_frames`, 480: 10 ms; miniaudio's fixed-size callback guarantees the block size). When there is no device, or none by that name, it falls back to the **null backend** — unless told not to — and says why once in the log; the game keeps running and the mix is computed and not heard. The null backend is not a device at all: nothing calls `render()` until the caller does, into its own buffer, which is what every test does and what makes the mix testable headless and deterministic.

## What miniaudio is asked to do

miniaudio 0.11.25 ([third_party/LICENSES.md](../../third_party/LICENSES.md)) is compiled as C into `engine_miniaudio` by [cmake/EngineAudio.cmake](../../cmake/EngineAudio.cmake) and linked **privately**; its header is included by exactly one engine file, `src/backend.cpp`, and no public header names a miniaudio type — the same argument [physics](physics.md#why-nothing-from-jolt-is-public) makes for Jolt. Compiled in: the device layer for WASAPI, PulseAudio and ALSA; the WAV, FLAC and MP3 decoders; the data converter and resampler the decoders use. Compiled out, each because the engine does not use it: DirectSound and WinMM (every supported Windows has WASAPI), JACK, miniaudio's own null device (it plays to nowhere on a thread of its own, and enumerating it would report a device on a machine with none), the encoders, the generators, the high-level engine, its node graph and its resource manager. The defines are public on the library target because several of them change the layout of `ma_context` and `ma_device`, which `backend.cpp` allocates.

**Vorbis is not compiled.** stb_vorbis ships in the same checkout (`extras/stb_vorbis.c`) and would cost one include, so it does not bring another dependency — but nothing in the tree can produce an Ogg Vorbis file to test it with short of committing a binary fixture (AGENTS.md forbids that) or writing an encoder, an untested decoder is not a feature, and stb_vorbis's history of crashes on malformed input matters for a decoder that will read mod content. It is on the "Not yet" list with that reasoning. FLAC is tested through a writer in the tests that emits VERBATIM subframes (no predictor, no Rice coder — only the framing and its two CRCs), which is how a generated clip reaches dr_flac without a fixture on disk; MP3 is compiled and has no fixture for the same reason Vorbis has none, which is worth knowing.

## Determinism

**The claim**: for a given clip set, a given layout and decoder, and a given sequence of commands and `render()` calls with their frame counts, the output is the same bytes on every run, compiler and C library. **Why it holds**: voices are summed in slot order into a cleared buffer, and slot allocation is a function of the command sequence; every operation on the audio thread is IEEE-754 `+ − × ÷` and `sqrt` in `f32`, which are correctly rounded everywhere, with floating-point contraction off for the whole tree ([ADR-0035](../adr/0035-no-floating-point-contraction.md)); the playhead is integer; and nothing on the audio thread calls a transcendental function — the pan law and the cone use `sin_quarter`/`cos_degrees`, fixed polynomials, rather than `std::sin`, which MSVC's and glibc's libraries are allowed to disagree about in the last bit. **What it does not cover**: which block a command lands in on a live device, which depends on when the device asked for the block; and the device's own conversion after the mix, when the endpoint is not in the mix's format.

**How it is tested**: `determinism_tests.cpp` runs a 48-block scripted session — every command kind, pitch through the interpolating path, loops and one-shots running out, a stereo clip, 2D sources on a 2D bus and panned by hand, 3D sources moving past a turning listener with a cone, a spread and the linear model, a bus change, the pool stealing and refusing, and the master clipping — twice in-process, compares the bytes, and pins `hash_mix(layout, samples)` for stereo and for 5.1. The clips are generated without the C library (`exact_sine` through `sin_quarter`, and an LCG), so the hash depends on the mixer alone. The values were taken on MSVC; the Linux container build must reproduce them with GCC 13 and Clang 18, which is the cross-toolchain half of the claim.

**The decode is pinned too, with one caveat stated.** A 44.1 kHz clip goes through miniaudio's low-pass filter, whose coefficients come from libm's `sin` and `cos` in double and are then rounded to `f32`; a last-bit difference between C libraries survives that rounding only when a coefficient sits within 2^-29 of an `f32` rounding boundary. `clip_tests.cpp` pins the hash of a decoded 44.1 kHz clip generated without libm, so a toolchain that disagrees is caught rather than assumed away.

## Protocol

`audio.devices` (no params → `engine.audio.AudioDevicesResult`): the backend, each playback device with its layout, channel count, rate and default flag, and the layout a mixer would be declared with now (`resolve_layout` of the default device's). `register_methods()` adds it; engine-host calls it when the capability is in its build, so `engine-cli audio.devices` is the probe — the audio counterpart of `gpu.adapters`, and like it, an empty answer is a normal one.

## Invariants

- The audio thread allocates nothing, takes no lock and logs nothing. Checked by `realtime_tests.cpp` with a replaced global `operator new` and the engine's own allocation counter, both counting on the rendering thread, and a log sink counting records with every category at trace, over a session of every command kind, voices ending, steals, clipping, and renders longer than a block.
- A command naming a voice generation the slot has moved past is counted stale and not applied; generation 0 is never issued.
- The controlling thread frees a slot only when the audio thread's event says the voice ended, except by stealing it, which is an explicit `Play`.
- The event ring never overflows (`events_dropped` stays 0), and a stop the command ring refused is re-sent.
- A clip's samples do not move while the store lives; a voice is only ever given the pointer of a Ready clip.
- Bus parents precede their children; the effective gain of a bus is the product of the gains on its path to the master.
- The master's output is in [−1, 1]; every sample the clip changed is counted.
- Nothing upstream of the decode stage depends on the layout; with the null decoder the mix is silence and every voice's lifecycle is unchanged.

## Public API

- `domain/audio/audio.h`: the capability's header — the checklist, `k_determinism`, the tunables' accessors, and every header below.
- `domain/audio/format.h`: `k_sample_rate`, `frames_for_ms`, `k_max_layout_channels`, `Speaker`, `LayoutInfo`, `layout_info`, `layout_channels`, `layout_name`, `speaker_direction`, `layout_from_speakers`.
- `domain/audio/spatial.h`: `SourceSpatial`, `k_source_2d`, `Cone`/`make_cone`, `Listener`, `ListenerBasis`/`make_listener_basis`, `SpatialParams`, `distance_gain`, `cone_gain`, `spatialize`, `sin_quarter`, `cos_degrees`, `PanGains`, `pan_constant_power`, `pan_balance`.
- `domain/audio/decoder.h`: `DecodeInput`, `Decoder`, `k_stereo_panner`, `k_null_decoder`.
- `domain/audio/commands.h`: `VoiceHandle`, `CommandKind`, the payloads, `Command`, `VoiceEvent`.
- `domain/audio/spsc_queue.h`: `SpscQueue<T>`.
- `domain/audio/clip_store.h`: `ClipHandle`, `ClipState`, `ClipView`, `ClipInfo`, `DecodedClip`, `DecodeStatus`, `decode_clip`, `clip_key`, `ClipStoreConfig`, `ClipStore`.
- `domain/audio/mixer.h`: the bus constants, `BusDesc`/`default_buses`, `MixerConfig`, `PlayParams`, `VoiceParams`, `ControlStats`, `MixerStats`, `VoiceState`, `VoiceSlot`, `Mixer`, `hash_mix`.
- `domain/audio/device.h`: `DeviceInfo`, `DeviceList`, `enumerate_devices`, `resolve_layout`, `OutputBackend`, `OutputConfig`, `Output`.
- `domain/audio/protocol.h`: `register_methods`.
- Generated, `<schemas/audio.h>`: `ChannelLayout`, `DistanceModel`, `Directivity`, `ChannelMapping`, `AudioEmitter`, `AudioListener`, `AudioDevice`, `AudioDevicesResult` — the enumerations are the C++ API's own types, so the schema and the mixer cannot disagree about what a layout or a distance model is.

**Depends on.** `base`, `containers`, `math`, `hash`, `ids`, `jobs`, `log`, `tunables`, `protocol`, `audio_schemas`; miniaudio, privately.

**Tunables.** `audio.voices` (64), `audio.command_queue` (1024), `audio.clip_budget_mb` (256), `audio.period_frames` (480), `audio.layout` (`auto`, or a profile name). Each is read by the constructor of the object it sizes; the audio thread reads none.

## Capability contract (ADR-0027)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone; `audio_system` requires it |
| Component and record types | `domain/audio/schemas/audio.schema`, `engine_schema_library(... CAPABILITY audio)` | done |
| Tick scheduler entry | none here: [audio_system](audio_system.md)'s `audio.emitters` | done there |
| Render-graph passes | none | audio draws nothing |
| Content-build derived step | none yet | see "Not yet" |
| Protocol methods | `audio.devices`, `audio::register_methods()` | done; engine-host registers it |
| Tunables | the five above | done |
| LOD policy | the pool is the budget; per-emitter LOD is `audio_system`'s | done |
| Determinism | `k_determinism` = `derived`: never read back into the simulation, and reproducible | done |
| Zero cost when unused | no linked code with the switch off; linked and unused, no thread, no device and no allocation until a `Mixer` or `Output` exists | done |
| Tests and size table | `tests/*_tests.cpp`, `tests/size_table.cpp` | done |
| Bench | `bench/audio_bench.cpp` | done |
| Removal proof | `ENGINE_WITH_AUDIO`, off in the minimal build | works |

## Testing

`tools/dev.ps1 test -Filter audio`, all headless through the null backend:

- **Queue** (`queue_tests.cpp`): capacity rounding, refusal when full, an untouched output on an empty pop, and a producer thread and a consumer thread passing 200,000 commands through an 8-slot ring and 1,000,000 through a 1024-slot one — no command lost, reordered, duplicated or torn, the consumer only ever calling `try_pop`.
- **Mixer** (`mixer_tests.cpp`): gain, pan (constant power for mono, balance for stereo) and pitch (2, 0.5 and 1.5) against analytic expectations on generated sines, bit for bit where the arithmetic allows and within interpolation error where it does not; bus gains composing through the default tree and a configured deeper one; the ramp on a parameter change and the fade on a stop; a one-shot ending at its last frame and a loop wrapping exactly, at pitch 1 and across the loop point at half speed; a render longer than a block; the voice policy refusing a lower priority and a loop's equal one, a one-shot stealing the oldest equal, a higher priority the lowest, and anything a stopping voice first; a stop the full ring refused, kept and re-sent by `update()`; refusals counted by cause; stale commands; a 3D source panned and attenuated by its distance model, turned toward and moved out of reach; the cone; spread; the 2D music bus and the `Direct` mapping; 5.1, 7.1.4, headphones and mono profiles against the stereo mix; the null decoder; the profile table against the device classification; the master's clip and its counters.
- **Clips** (`clip_tests.cpp`): generated 16-bit stereo and float mono WAVs decoding bit for bit, a generated FLAC decoding bit for bit, a 44.1 kHz clip resampled to 48 kHz with its frequency and level intact and its bytes pinned, six channels folded to stereo, junk refused, the store's key deduplication and reporting, the budget refusing rather than evicting, and eight decodes on the Efficiency pool.
- **Determinism** (`determinism_tests.cpp`): the scripted session above, pinned for stereo and 5.1.
- **Real time** (`realtime_tests.cpp`): the allocation and log counters around `render()`, the null backend as the same mix pulled by the caller, device enumeration that passes with zero devices, and `audio.devices` through a dispatcher.
- **Model** (`audio_tests.cpp`): the tunables' defaults and `resolve_layout`, `sin_quarter` and `cos_degrees` against the true functions, the pan laws, the distance models, the listener basis, and `spatialize`.

The size table pins `VoiceState` (48), `VoiceSlot` (16), `SourceSpatial` (56), `SpatialParams` (20), `ListenerBasis` (48), the payloads, `Command` (128, two lines — [commands.h](../../domain/audio/include/domain/audio/commands.h) says why) and the handles.

## Performance notes

Benchmarks: `tools/dev.ps1 bench -Preset msvc-release -Filter 'audio.*'`, or the executable directly with `--require-quiet`. One iteration is one 10 ms block (480 frames) with N voices playing; the median over N is one voice's cost for one block.

Measured 2026-09-24 on the i9-10980XE (18 cores, 36 threads), `msvc-release`, seven repeats. **Machine state: not quiet, so these are upper bounds.** `--wait-quiet=900` gave up after fifteen minutes: other processes used 14.7% of the CPU at the start and 20.4% at the end, the GPU lock was held by another agent's full test suite, and the GPU was 6–7% busy (the harness's `machine_state`). An earlier run beside a heavier load (others at 41.6%) came back within 10% of these rows, which is the evidence that they are near the cost and not dominated by the neighbours.

| Benchmark | Voices | Per 10 ms block | Per voice per block |
|---|---|---|---|
| `audio.mix.3d` — mono, pitched 0.8–1.2, positioned | 1 | 2.61 µs | 2.61 µs |
| | 16 | 27.2 µs | 1.70 µs |
| | 64 | 92.9 µs | **1.45 µs** |
| | 256 | 367.8 µs | 1.44 µs |
| `audio.mix.2d` — stereo, pitch 1, music bus | 1 | 3.37 µs | 3.37 µs |
| | 16 | 32.5 µs | 2.03 µs |
| | 64 | 126.7 µs | 1.98 µs |
| | 256 | 502.3 µs | 1.96 µs |
| `audio.mix.3d_51` — the first, declared 5.1 | 64 | 95.5 µs | 1.49 µs |

What the rows say:

1. **A voice costs about 1.45 µs per 10 ms block** — three nanoseconds a frame for the voice loop, the source model and the decode together. The default pool of 64 is 93 µs, **0.9% of one core**; the pool's size, not the mixer, is the budget, which is why it is a tunable.
2. **The fixed cost of a block is about 1.2 µs** — clearing the output, the master's pass over it, publishing the stats — which is why the per-voice figure falls from 2.6 µs to 1.45 µs as the pool fills.
3. **A stereo 2D voice costs a third more than a mono 3D one** despite skipping the source model: it moves twice the samples through both passes. The source model's square root and two polynomials per voice per block do not show next to 480 frames.
4. **5.1 costs 3% more than stereo** for the same voices: the stereo panner still writes two channels, the stride is wider, and the master's pass covers three times the samples. A VBAP decoder writing every speaker will cost more, and the bench row is where that will be seen.
5. **Where the time goes**: the voice loop is a serial chain (the 32.32 playhead's add feeds the next frame's index) with two dependent loads a frame, and the decode is a second pass over the scratch signal. Fusing the two passes for the stereo panner and a vectorized pitch-1 path are the obvious next steps; at under 1% of a core for the default pool, neither is needed yet.

## Not yet

In the order the direction note puts them, the decode stage's later implementations and the calibration that feeds them — each with the seam it plugs into:

1. **Ambisonics.** An encoder into an intermediate B-format bus and a decoder from it to the declared profile, heights included. Seam: a `Decoder` whose per-source step encodes into its own bus (its `state_floats`) and whose output step decodes the bus into the layout's channels, reading `speaker_direction()` for every row.
2. **VBAP** — vector-base amplitude panning per object over every speaker of the profile. Seam: a `Decoder` that reads `DecodeInput::spatial.direction` in full and the profile's azimuths and elevations; the stereo panner's front-pair rule is its degenerate case.
3. **Windows Spatial Sound** (`ISpatialAudioClient`): the platform's object renderer, for headphones through Windows Sonic, Dolby Atmos or DTS:X and for Atmos-capable receivers. Seam: a `Decoder` that emits each source as a platform object from `DecodeInput::source` and the listener basis instead of mixing into `out`, with 2D sources going to the bed; it needs a device path of its own beside `Output`, since the platform owns the render.
4. **Steam Audio.** Occlusion, transmission and air absorption against the RT scene geometry fill `SpatialParams::attenuation` (and the per-band terms it will grow); HRTF rendering over headphones is a `Decoder`, and the headphones profile becomes binaural with it.
5. **An object stream to an external renderer** — a sound card or an HDMI receiver that renders positional objects against its own room calibration: the direction note's second sentence, and the "fourth output" it names ([05 §5.11](../plan/05-simulation.md#511-integration-notes)). The platform's spatial path is the only such endpoint on a PC today; ADM, MPEG-H and IAMF have none. Seam: the decode stage already takes the source objects and the layout, so a stream encoder is one more `Decoder` that emits objects instead of channels, and the mixer never learns which.
6. **A calibration file**: measured speaker positions, per-channel gain, delay and EQ — the user's, read by the decode stage and nothing else, and an override of the shipped profile table's rows rather than a separate path. The table's gains and delays are not columns yet; they arrive with it, and so does the per-channel delay line that makes a delay mean something.

And the rest, by what they would change:

- **Vorbis** — stb_vorbis is in the checkout; the blocker is a fixture, and its record on malformed input (see "What miniaudio is asked to do").
- **Streaming decode**: long music and ambience decoded incrementally from the VFS into a ring per voice, instead of whole into the store. The budget refuses a long clip today rather than streaming it.
- **Clip eviction**: freeing a clip needs the mixer's view of which clips are playing; until then the budget refuses new decodes.
- **Submixes and bus effects**: reverb sends, ducking, a per-bus filter. Buses are gains today and have no signal of their own.
- **A limiter** on the master, beside the counted hard clip, for content that cannot leave headroom.
- **Better interpolation** (cubic or windowed sinc) and anti-aliasing when a voice is pitched up; linear interpolation's error is measured in the pitch test and is inaudible for the small pitch variation effects use.
- **Headphone detection** from the endpoint's form factor, so the headphones profile is chosen without the setting.
- **Device changes at run time**: a default device that changes or disappears (hot-plug, a Bluetooth headset connecting) reopens the output; today an `Output` is opened once.
- **A wider phantom stage** for 2D music as a `ChannelMapping` (the direction note's "per user preference").
- **Capture** (microphones, voice chat).
- **Content-build decoding**: clips decoded once by the content build into the mix format in the derived-data cache, so a game's load is a copy; today they are decoded at load.

## Why this shape, in one paragraph

The things a first audio version gets wrong are the ones that cannot be retrofitted: a mixer whose unit is a pre-panned pair cannot grow a speaker layout, a layout that is assumed cannot be read from a device, a panner spread through the mixer cannot be replaced by a platform renderer, and a mix that is not a function of its inputs cannot be tested or replayed. So v0 is small where it can be — linear interpolation, a stereo panner, a hard clip, gains for buses — and exact where it has to be: sources as objects, a declared profile, one decode stage behind a table, two wait-free queues, and a hash of the output that two compilers have to agree on.
