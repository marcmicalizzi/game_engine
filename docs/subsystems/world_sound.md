# world_sound (systems)

**Purpose.** The sound stage a host creates for a game (roadmap S111, the first stage of [audio](audio.md#sound-for-the-world-the-design-s110)'s design; [ADR-0079](../adr/0079-world-sound-is-synthesised-from-world-state.md), proposed). One object, `world_sound::SoundStage`, owns the clip store, the mixer, the output (a device or the null backend), the listener, the clips a scene's `sound` block declares and the voices a game module starts; a host creates it, hands it a listener and ticks it. Sound code lives here and not in an app, as rendering code lives in [renderer](renderer.md). engine-view is the one host so far ([apps](apps.md#sound-a-module-plays)): `--audio` chooses the mode and a module's `sound_clip`, `sound_play` and `sound_stop` reach the stage. **What this stage is not yet**: the world's synthesised sources — the wind in the trees and at the ears (S112), running water and the surf (S113), the path's terms (S114) — are the design's later stages, and nothing of them is built; at S111 the stage plays a game's clips and nothing else.

**Why this shape.** The design's reasons hold ([audio](audio.md#the-capability-systemsworld_sound)): one object a host creates, so engine-view, engine-host and the editor can each host sound without each wiring a store, a mixer and an output; a capability of its own requiring only `audio`, so the minimal build proves nothing depends on it. Three things were settled while building it:

- **The handle is the stage's count, never the mixer's slot** (ADR-0079 decision 5). `play` answers `Ok` with `1, 2, 3, …` whether or not a voice sounded — muted, the pool full of louder voices, no device — and `stop` answers `Ok` for any handle issued. Refusals are the count's too: an unknown clip, a gain outside [0, 16] or a position that is not finite is `InvalidArgument` in every mode, and nothing is issued for it. So a module that keeps what the calls answered in its state (the fixture does) holds the same state heard and not heard, which the end-to-end case checks.
- **`Off` is a stage, not an absence.** A stage opened `Off` keeps the block's names and the count and builds no store, mixer, thread or output, so a muted session answers exactly as a sounding one. A scene with no `sound` block is the other case: every call answers `Unsupported`, as the hot-load header always said.
- **The block is the scene's** (`engine.scene.Sound` in `schemas/scene.schema`), not this capability's, so a build without the capability reads the same scene and a module there hears `Unsupported` rather than a refused scene.

**Owned data.** The declared clips (name, source, gain, distances) in the block's order — the index a module holds; the store, the mixer and the output when not `Off`; the last 256 handles' voices and world positions (an older handle's one-shot has ended long since, and its stop sends nothing); the listener and the mixer's frame; the null backend's block and the WAV's one-second buffer; the counters (`StageStats`).

## The scene's `sound` block

```json
"sound": {"clips": [
  {"name": "chirp", "recipe": {"kind": "Chirp", "frequency_hz": 2500, "end_hz": 4500, "duration_s": 0.18},
   "min_m": 2, "max_m": 80},
  {"name": "door", "path": "sounds/door.wav", "gain_db": -3}
]}
```

Each clip has a unique `name`, exactly one of `path` (relative to the scene file) and `recipe`, a `gain_db` (−60 to 24) and the two distances of the mixer's tapered inverse law ([audio](audio.md#sources-are-objects)): full level inside `min_m`, silent at `max_m` (defaults 1 and 60). A block with a repeated or empty name, neither or both of a path and a recipe, or a value out of range refuses the scene with a sentence naming the clip. **Formats**: a file is read by the store's decoders, so WAV, FLAC and MP3, or a `.clip` the content build wrote ([audio](audio.md#clips)); Vorbis and Opus are not decoded (the owner's question 6 on the audio page). Clips are decoded once, inline, when the stage opens (before the module's `on_load`), under a budget of 64 MB for a game (`StageConfig::clip_budget_bytes`; the mixer's own `audio.clip_budget_mb` is 256), and a clip that does not decode refuses the session.

### Recipes

**A clip the stage generates, so a fixture or a template has a sound with no binary in the tree.** `SoundRecipe`: `kind` (`Tone`, `Chirp` — a sine swept linearly in frequency, a bird's simplest note — or `Noise`), `frequency_hz` and `end_hz` (20 Hz to 20 kHz), `duration_s` (1 ms to 10 s), a linear `attack_s` and `release_s` inside it, `level_db` (−60 to 0 dBFS peak) and a noise `seed`. Mono at 48 kHz. **The same recipe is the same samples on every toolchain**: the phase is a 32-bit integer stepped by `round(f · 2³² / 48000)` (a chirp's step interpolated in integers), the sine is the mixer's own quarter-wave polynomial (`audio::sin_quarter`), the noise is xorshift32, and the level is `gain_from_db`, which takes 10^(dB/20) as a power of two (`ldexp`, exact) times eighteen terms of e^y's series — no `pow`, `exp` or `sin` from the C library, whose last bits MSVC and glibc may disagree about ([audio](audio.md#determinism)). The chirp's samples are pinned in the tests.

## The stage

| Call | Answers | What it does when sounding |
|---|---|---|
| `open(config, block)` | false with a sentence | the clips decoded or generated, the mixer built (stereo for the null backend; the device's layout by `resolve_layout`), the output opened |
| `clip(name, out)` | `Ok`, `NotFound`, `Unsupported` (no block) | — |
| `play(clip, at or null, gain, handle)` | `Ok` with the next handle; `InvalidArgument`; `Unsupported` | a one-shot on the `sfx` bus at `gain` × the clip's gain, 3D at `at` with the clip's distances, or 2D on the listener with `at` null |
| `stop(handle)` | `Ok` for any handle issued; `InvalidArgument` for 0 or one never issued | the mixer's 10 ms fade, when the handle's voice still sounds |
| `set_listener(at, forward, up)` | — | one `SetListener` when anything changed; a move into another 64 m cell moves the mixer's frame and places every 3D voice still sounding again in it, one `SetSource` each |
| `tick()` | — | the mixer's and the output's update; under the null backend 48,000 / `tick_hz` frames pulled (the remainder carried, so a second of ticks is a second of frames) and, with `wav_out`, kept |
| `close()` | false when the WAV could not be written | the WAV's sizes written; the output closed |

**The mixer's frame** is [audio_system](audio_system.md#the-mixers-frame)'s: the mixer is handed positions less the corner of the listener's 64 m cell, so the floats it gets are metres from a corner and not world coordinates, and a listener moving inside a cell is one command. The stage keeps each 3D handle's world position in f64 to place it again when the frame moves. The test stands its listener 10,000 km out.

**`--audio-out`'s WAV** is 32-bit float (`WAVE_FORMAT_IEEE_FLOAT`, the mixer's own samples, nothing converted), interleaved at the mix's channel count: a 44-byte header written at `open` with its sizes zero, the frames appended a second at a time from a buffer made at `open`, and the header's sizes written at `close` by reading the file back. The store's own decoder reads it back bit for bit (a case).

## When the device fails

The engine's rule for every binary holds here: no failing device crashes the process, blocks the frame or the tick, or opens a dialog ([platform](platform.md#no-engine-binary-opens-a-dialog-when-it-fails)). **An output that will not open, a write that fails, and a device lost that cannot be opened again each leave the stage silent**: the output is closed, nothing is mixed or sent from then on, the reason is logged once (`audio stopped; the game's calls are answered as before`, with `why`) and kept in the summary (`stopped`, `stopped_why`, `stopped_tick`), and `clip`, `play` and `stop` answer exactly as before, handles counted. None of it fails the session; only a clip that does not decode does, since that is the content's fault and not the device's. A device lost mid-session is first `audio::Output::update`'s to reopen ([audio](audio.md#device-changes-at-run-time)); only when that falls back to the null backend does the stage stop. The tick never waits on the device: commands go through the mixer's wait-free ring, and a ring a stalled device has let fill refuses a play (counted in the mixer's `queue_full`) rather than waiting. **The two cheap paths are proven on the null backend through a fault seam** (`StageConfig::fault_refuse_open`, `fault_fail_write_at_tick`): the scripted session's answers are the same as a healthy stage's, and nothing is pulled or started after the failure. A WAV that cannot be written (`--audio-out`) is the write path's own case of the same rule.

**The device is opened the conservative way**, as `domain/audio`'s output already does: shared mode (miniaudio's default; the engine never asks for exclusive), 32-bit float handed to the platform's shared mix, which converts to the endpoint's own format, a callback the device's thread calls (miniaudio's model; no push), and a period of `audio.period_frames` (480 frames, 10 ms). Nothing of it was changed for S111.

## Invariants

- What `clip`, `play` and `stop` answer, and every handle, is the same in `Off`, `Null` and `Device` (the scripted session's answers compared, and the end-to-end replay's module state).
- No handle is issued for a refused call; handles start at 1 and count up.
- A stage with no block answers `Unsupported` to all three and counts nothing.
- The tick allocates nothing in steady state: plays, stops, listener moves, cell crossings, the null render and the WAV's buffer (`mem::allocations_on_thread` over 2,400 ticks).
- The null render of a scripted session is the same bytes run after run, and pinned.
- No case opens an audio device; every one judges by the null backend's samples (ADR-0079 decision 6).

**Public API.** `include/systems/world_sound/world_sound.h`: `k_determinism`, `AudioMode`/`audio_mode_name`, `Answer`, `StageConfig`, `StageStats`, `render_recipe`, `gain_from_db`, `read_scene_sound`, `SoundStage`.

**Depends on.** `base`, `containers`, `math`, `hash`, `ids`, `log`, `io`, `json`, `schema`, `tunables`, `audio`, `audio_schemas`, `schemas` (for `engine.scene.Sound`). Not `gfx`: the design links it for the wind's header, which S112 is the first stage to read.

## Testing

`tools/dev.ps1 test -Filter world_sound`, no device of either kind:

- **The dB series** against the C library's `pow` to 1e-14, and exactly 1 at 0 dB.
- **Recipes**: the chirp's length, its silent ends and its peak at −6 dB; the chirp's and a seeded noise's samples pinned (`0x812eb5dafa81ec80`, `0x3037d56583c9bee2`); refusals naming the field.
- **The block**: read, absent, and refused for a repeated name, a clip with neither or both sources, and distances the wrong way round.
- **Left and right**: a chirp 3 m to the left of a listener 10,000 km out, facing −z, is more than 30 times as loud on the left as on the right; after a half turn the other way round, with the same power; the commands counted.
- **The pin**: a scripted session (a chirp to the left, a tone ahead stopped half-way, a 2D chirp, a half turn, a tone sounding while the listener walks across a cell's edge) rendered twice to the same bytes, its counts (four plays, two frame moves, one source sent again, 52 listener moves) and its hash pinned (`0x9b8ce750f9dab6e5`). The values were taken on MSVC; the Linux container's GCC and Clang must reproduce them.
- **Modes**: the same script `Off` and `Null`, every answer and handle the same; the refusals the same in both; a stage with no block `Unsupported`.
- **Allocation**: 2,400 ticks of every call a host makes allocate nothing.
- **Formats**: WAVs written by the case — PCM 16 bit mono at 48 kHz, 24 bit stereo at 44.1 kHz, float mono at 22.05 kHz, 16 bit stereo at 96 kHz — loaded by path from beside the scene, each half a second at the mix rate and each heard; a file that is not there refuses the stage naming the clip.
- **Failure**: the output refused at open, and its write failing at tick 100: the scripted session's answers the same as a healthy stage's, `stopped` with its reason in the summary, nothing pulled or started after the failure.
- **The WAV**: 75 ticks at 60 a second (1.25 s, past the WAV's one-second buffer) written, sized, and decoded back bit for bit.

End to end, engine-view's `game_sound_view_tests.cpp` ([apps](apps.md#sound-a-module-plays)).

## Performance notes

Benchmarks: `tools/dev.ps1 bench -Preset msvc-release -Filter 'world_sound.*'`. One iteration of `world_sound.tick_voices/N` is one host tick at 60 a second under the null backend: the controlling thread's updates and the audio thread's 800-frame block with N clip voices of a 440 Hz tone sounding, which is what a game's clips cost a session. The numbers and the machine's state they were taken in are in the table below; S110's budget for the audio thread is 200 µs a 10 ms block with every world voice of S112 to S114 sounding, which this row does not yet measure (S112's bench row does).

| N | µs a tick (800 frames) | machine |
|---|---|---|
| 0 | | |
| 8 | | |
| 32 | | |
| 64 | | |

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way. Outside its directory it touched only registration points and hosts: the scene schema's block, engine-view's glue (`game_sound.cpp`, the runner's hooks) and the fixture.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(world_sound audio)` | declared; off when audio is off |
| Component and event types | `engine.scene.Sound` in `schemas/scene.schema` | done: the scene's, so a build without the capability reads it |
| Tick scheduler entry | none: the host ticks the stage at its session's tick | not needed; sound is presentation |
| Render-graph passes | none | sound draws nothing |
| Content-build derived step | none here; a clip's `.clip` is `audio`'s | not needed |
| Protocol methods | none | not needed yet (engine-host opens no device) |
| Tunables | none of its own; the mixer's `audio.*` size it | S112 adds `audio.world.*` |
| LOD policy | the mixer's pool is the budget, its voice policy chooses ([audio](audio.md#the-voice-policy)) | S112 adds the beds |
| Determinism | `world_sound::k_determinism` = `derived` | sound is presentation (ADR-0079 decision 4) |
| Zero cost when unused | no `sound` block or `Off`: no store, mixer, thread or output | done |
| Tests and size table | `tests/world_sound_tests.cpp`, `tests/size_table.cpp` (`StageStats`, 88) | done |
| Bench | `bench/world_sound_bench.cpp` | done |
| Removal proof | `ENGINE_WITH_WORLD_SOUND`, off in the minimal build | works: engine-view's module then hears `Unsupported` |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_WORLD_SOUND=OFF`) drops the module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`. engine-view then builds without `game_sound.cpp`, its runner has no stage, and a module's three sound calls answer `Unsupported`, as before S111.
