# audio_system (systems)

**Purpose.** The ECS half of audio: it registers the `AudioEmitter` and `AudioListener` components (declared in [audio](audio.md)'s schema) with a world, and one system, `audio.emitters`, turns their changes into [audio](audio.md) mixer commands once per tick. It is the whole interface a game touches — set a component, and the sound follows — and nothing else in the engine learns about audio. It is a module of its own in `systems/` rather than part of `domain/audio` because it is the bridge between an ECS-free module and the world: `<flecs.h>` belongs to `domain/ecs`, `systems/` and `game/` ([ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 5), and `domain/audio` stays free of it, so the mixer runs, tests and benches with no world at all. An optional capability ([ADR-0027](../adr/0027-additive-capabilities.md)) that **requires** `audio` and `ecs` (`engine_capability_requires(audio_system audio ecs)`): with either off it is off, with a line on the configure output; the reverse is not true, so the `*-no-ecs` presets keep the mixer and drop this.

**Owned data.** One private, transient component per emitter, `EmitterVoice` — the emitter as last sent to the mixer, the voice it sounds through, its LOD tier, whether a one-shot has played out, and a loop's clock (below) — added with every `AudioEmitter` through flecs' `With` rule, so the system's query has no optional term and no deferred add of its own. The system itself holds, per mixer voice slot, the voice it started there and whether an emitter claimed it this tick, and the listener it last sent. Never saved: a `VoiceHandle` means nothing in the next process.

## The components are desired state

`AudioEmitter` is what the game wants to hear — clip, position and orientation, level, pitch, pan, the distance model and its distances, the cone, spread, bus, priority, the 2D flag and mapping, `looping`, `playing`, and a `cue` counter to bump for a restart. `AudioListener` is where the world is heard from. Both are schema components, hashed and saved like any other.

**Nothing the audio thread does is written back into them.** Whether a one-shot has finished is a fact about when the sound card asked for its next block, not about the simulation; writing it into a hashed component would make a replay diverge on a machine with a different buffer size. So the system keeps it in `EmitterVoice`, and a one-shot's component still says `playing: true` after it has played out — it stays silent until `playing` turns true again or `cue` changes. That is the determinism stance, `derived`: the components are state, the commands are output, and nothing flows from the output into the state.

## What a change sends

The system diffs each emitter against the copy it sent last and sends the smallest set of commands that implies the change:

| Change | Commands |
|---|---|
| a new emitter with `playing` | one `Play` |
| nothing | nothing |
| `gain`, `pitch`, `pan`, `bus`, `looping` — any number of them | one `SetParams` |
| `position`, `orientation`, the distances or distance model, the cone, `spread`, `mapping`, `two_d` — any number | one `SetSource` |
| both groups at once | one of each |
| `priority` | nothing: it matters when a voice is chosen, and is read at the next play |
| `cue` or `clip` | `Stop`, then `Play` |
| `playing` false | `Stop` |
| `playing` true again | `Play` |
| the listener moved or turned within its 64 m cell | one `SetListener`, whatever the number of emitters |
| the listener moved into another cell | one `SetListener`, and one `SetSource` per emitter holding a voice: the mixer's frame moved ("The mixer's frame", below) |
| an emitter destroyed, or its component removed | `Stop` of its voice, the next tick |

The test counts at the mixer's own controlling-thread counters, so a command the system sent and should not have is caught as surely as one it forgot. A `SetSource` carries the whole spatial block (it is 56 bytes and one command either way), so "one source update" is exact however many of its fields moved.

**A voice that ended.** The system drains the mixer's ended-voice events first each tick (`Mixer::update`), and an emitter whose voice is no longer live learns it there: a one-shot is finished until it is retriggered; a loop was stolen by a more important sound and asks for a voice again, being refused while every voice outranks it. **A destroyed emitter's loop is stopped** without the system ever holding a flecs entity id past a tick (ADR-0028 seam 3): each tick every emitter claims its voice's slot, and a voice the system started that nobody claimed belongs to an emitter that is gone — mark and sweep over the 64 slots. **A cloned emitter** (flecs copies `EmitterVoice` with the rest) finds its voice already claimed by the original, resets its copy and plays a voice of its own.

**Where the clip comes from.** `AudioEmitter::clip` is an `Id128` the clip store knows — an asset id, or `audio::clip_key()` of the encoded bytes. An emitter whose clip is not Ready waits and retries every tick (`AudioSystemStats::waiting`), which is what lets a game set the component before the decode on the Efficiency pool has finished; the decode lands in the mixer's `update()`, which the system calls first thing in its tick. A clip the store's budget evicted stays Ready: the emitter's play streams it from its file and asks the store for it back, so an emitter never waits on an eviction ([audio](audio.md#eviction)).

### The mixer's frame

**`AudioEmitter.position` and `AudioListener.position` are `worldpos`, f64** (both components version 2, 2026-10-06; [ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md)), and the mixer is handed float32 positions **in a frame at the corner of the listener's 64 m cell** (`k_world_cell_m`; `AudioSystem::mixer_frame()`): the listener's and every source's position less the corner, in f64 and then a float (`relative(p, frame)`). Until then both were float32 world positions, so 10,000 km out a source a metre to the listener's left was wherever the float's metre grid put it, and at 1e8 m on an 8 m one. Now the floats the mixer gets are a function of where the listener stands in its cell, not of where the cell is: the test stands a listener at the same place in its cell at the origin, 419 km, 10,000 km and 1e8 m, with a loop a metre to its left (and once with the loop across the cell's edge), and the mixer renders the same bytes at all four, louder on the left.

**Why the cell's corner and not the listener itself.** The brief that asked for this said listener-relative (`relative(source, listener)`), which is the finest a float can be; but the mixer's listener would then always be at the origin and every listener step would move every source, so a walking player would cost one `SetSource` per sounding emitter per tick instead of one `SetListener` — and the contract above, that a moved listener is one command, would be gone. The corner keeps it: the frame moves only when the listener crosses into another cell, and then every voice's source is sent again in the new frame (`frame_moved_`), one `SetSource` each, which the test counts. It is a function of where the listener is now, never of where it has been — no hysteresis, no history — so two runs, a save loaded and a run that never stopped hand the mixer the same floats ([ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md)'s rule that nothing depends on where the eye has been). What it costs in precision is a float at the size of the cell plus the emitter's reach: 64 m and a `max_distance` of 50 m are 7.6 µm, which no ear hears. The LOD policy's distance is the emitter's displacement from the listener in f64, narrowed once.

## The LOD policy

An emitter past its `max_distance` from the listener holds **no voice**: tier 3, virtual — it keeps its state, costs the audio thread nothing, and the pool's voices go to what can be heard. It gets a voice again inside `max_distance` (tier 0). Between the two, a band of `audio.lod.hysteresis` (a tenth of `max_distance`) keeps an emitter walking along the boundary from churning its voice. The boundary is the emitter's own `max_distance`, and not `sim::TierParams`' shared table of bands, because the question is "can this emitter be heard", and only the emitter knows how far it carries — and the tapered and linear distance models reach silence exactly there ([audio](audio.md#sources-are-objects)), so taking the voice away is inaudible by construction. A 2D emitter, one on a 2D bus, and one whose distance model is `None` never go virtual.

**A loop comes back where it would have been; a one-shot does not come back.** A loop that returns within reach restarts at the position it would have reached had it played on: each looping emitter carries a clock in its `EmitterVoice` — the frames of its clip it had played by a given tick — started on the tick it first wants to loop (new, retriggered by `cue` or `clip`, switched to looping, or turned back on) and running whether or not it holds a voice. When it gets a voice, `loop_position()` adds 800 frames a tick (the mix rate over the world's tick rate, 60 Hz) at its pitch since the clock's tick and wraps the sum into the clip's length, and the play starts there; a pitch change settles what was played at the old pitch and runs on at the new one, so a loop pitched down while out of reach comes back behind where it would have been at the old pitch. The same holds for a loop the pool turned away or stole and later let back in. It is a function of the tick count and the clip's length alone — f64 arithmetic and `fmod`, which is exact — because nothing the audio thread does is read back into the simulation (the determinism stance above), so the resumed position is where the tick says the loop should be rather than where the device's playhead actually got to; the two differ by at most the command's latency, and only while nobody can hear it. Nothing is computed while a loop holds its voice: the clock is read only when a play starts, and written only when the loop starts or its pitch changes. A one-shot that goes out of reach while playing, or is triggered out of reach, is over: it was at the edge of hearing, and a sound that starts when the listener walks up to where it happened is a worse lie than silence.

## The tick

`audio.emitters` runs in `EventsOut` — after gameplay and physics have moved everything this tick, as the tick's output to the world outside it — single-threaded, because the mixer's controlling half is single-producer. Its `sim::SystemDesc` reads `AudioEmitter` and `AudioListener`, writes `EmitterVoice`, and writes the resource `audio.mixer`, so a second system that drives the same mixer (a music director, a footstep system) is ordered against this one by the schedule rather than by luck. One listener is expected; with several, the lowest entity id is used and the rest are counted.

## Invariants

- Every change to an emitter or the listener sends exactly the commands in the table above, and an unchanged world sends none.
- Nothing is written into `AudioEmitter` or `AudioListener` by the system.
- Every voice the system started is either claimed by an emitter each tick or stopped at the end of it.
- An emitter past `max_distance × (1 + hysteresis)` holds no voice; one inside `max_distance` that wants to play and has not finished asks for one.
- A looping emitter's voice starts at `loop_position()` of its clock on the tick it is played: frame 0 on the tick the loop started, wherever the tick count and its pitch put it after that. A one-shot starts at frame 0 and only on the tick it is triggered in reach.

## Public API

`systems/audio_system/audio_system.h`: `k_system_determinism`, `k_phase_emitters`, `k_resource_mixer`, `k_tier_voiced`/`k_tier_virtual`, `lod_tier`, `EmitterVoice`, `loop_position`, `AudioSystemStats` (with `resumed`, the loops started somewhere other than their beginning), `AudioSystem` (`install`, `stats`, `desc`). The components are `<schemas/audio.h>`'s, registered by `install()` through the generated `<schemas/audio_ecs.h>`.

**Depends on.** `base`, `containers`, `math`, `log`, `tunables`, `ids`, `sim`, `ecs`, `audio`, `audio_schemas`.

**Tunables.** `audio.lod.hysteresis` (0.1), read once per tick.

**Lifetimes.** The mixer outlives the system; the system outlives every step of the world it is installed in (its flecs system calls into it), and its destructor stops every voice it started.

## Capability contract (ADR-0027)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(audio_system audio ecs)` | declared; off when either is off |
| Component types | `AudioEmitter`, `AudioListener` from `domain/audio/schemas/audio.schema`; `EmitterVoice` private | done |
| Tick scheduler entry | `audio.emitters`, a `sim::SystemDesc` through `ecs::register_system`, phase `EventsOut` | done |
| Render-graph passes | none | — |
| Content-build derived step | none | — |
| Protocol methods | none here; the components are reachable through `world.apply`, and `audio.devices` is `audio`'s | — |
| Tunables | `audio.lod.hysteresis` | done |
| LOD policy | `lod_tier()`: per emitter, against its own `max_distance` | done |
| Determinism | `derived`: commands out, nothing written back | done |
| Zero cost when unused | no linked code with the switch off; installed in a world with no `AudioEmitter`, the tick is one listener query and one ring drain | done |
| Tests and size table | `tests/audio_system_tests.cpp`, `tests/size_table.cpp` | done |
| Bench | `bench/audio_system_bench.cpp` | done |
| Removal proof | `ENGINE_WITH_AUDIO_SYSTEM`, off in the minimal build and in `*-no-ecs` | works |

## Testing

`tools/dev.ps1 test -Filter audio_system`, headless: the system's declaration; a new emitter playing once and an unchanged world sending nothing; each field group sending exactly its command (and two fields of one group sending one); `priority` sending nothing; restarts by `cue` and by `clip`; `playing` off and on; the listener sent when it moves and only then, and a second listener counted and ignored; a one-shot playing out and staying silent until retriggered; a destroyed emitter's loop and a removed component's voice swept; the LOD policy's band, virtualization, return, the 2D exemption and a one-shot triggered out of reach; a loop that goes out of reach for three ticks coming back 3,200 frames into its clip, one whose pitch doubles while it is out coming back where the two rates put it, a new cue restarting its clock, and `loop_position` on its own; a full pool turning emitters away with one mixer refusal a tick, and a stop letting the next one in; an emitter waiting for its clip; a cloned emitter getting its own voice; and a sequence of edits ticked by the engine's scheduler through `ecs::ScheduledTick` sending, tick for tick, the same commands as under flecs' own pipeline, and mixing to the same bytes — a loop that goes out of reach and comes back included, since where it resumes is read off the tick — the system declared `EventsOut` and did not change for [ADR-0038](../adr/0038-the-scheduler-owns-the-tick.md). The far sites: a loop a metre to the listener's left rendering the same bytes at the origin, 419 km, 10,000 km and 1e8 m, and a listener crossing into another cell resending each voice's source once while a step inside its cell sends the listener alone ("The mixer's frame"). The size table pins `AudioEmitter` (112 bytes: 96 until its position became f64), `AudioListener` (48: 36 until then) and `EmitterVoice` (144: 128 until the emitter it holds grew, 112 until the loop's clock added its 16).

## Performance notes

`tools/dev.ps1 bench -Preset msvc-release -Filter 'audio_system.*'`. One iteration is one `SimWorld::step()` over N emitters with a 64-voice mixer; the emitters spiral out from the listener, so 473 are within their `max_distance` and want a voice, and the rest are virtual. Measured 2026-09-24 on the i9-10980XE, `msvc-release`. **Machine state: not quiet** — other processes at 28.8% of the CPU and the GPU lock held by another agent's full suite — so these are upper bounds.

| Benchmark | Emitters | Per tick | Per emitter |
|---|---|---|---|
| `audio_system.tick.still` — nothing changes | 1,000 | 24.6 µs | 25 ns |
| | 10,000 | 168.6 µs | 17 ns |
| `audio_system.tick.moving` — every emitter moves | 1,000 | 30.6 µs | 31 ns |
| | 10,000 | 173.6 µs | 17 ns |

**The number that moved, and why.** The first version asked the mixer again for every emitter the pool had turned away, every tick: with 473 emitters in reach and 64 voices, that is about 400 refused plays a tick, each draining the event ring and scanning the pool twice, and it cost **200 µs at 1,000 emitters and 337 µs at 10,000** (same machine, others at 48.5% and 28.8% of the CPU — the before is the busier of the two, so the ratio flatters it a little). The system now remembers the highest priority the pool refused this tick: every voice outranks that until something stops, so an emitter at or below it is refused with one compare, and the memory is forgotten the moment the system stops a voice. **8.2× at 1,000 emitters, 2.0× at 10,000.** What remains at 10,000 is the diff and the LOD test themselves — a 96-byte component read, a 112-byte state read and written, a squared distance — about 17 ns an emitter. A moving world costs little more than a still one because only the 64 voiced emitters send a source update; the other movers are virtual and send nothing.

**The loop's clock, measured.** Resuming a loop where it would have been added 16 bytes to `EmitterVoice` (112 → 128, two whole cache lines) and two compares a tick for a looping emitter. Measured 2026-09-25 on the same machine, `msvc-release`, the build before and after it run in two interleaved pairs; **not quiet** — others at 37–56% of the CPU and the GPU lock held by another agent's full suite — so the pairs disagree with each other by as much as the change: at 1,000 emitters nothing moved (still 24.3–27.2 µs before, 25.6–25.9 after; moving 30.5–32.0 before, 31.5–31.7 after); at 10,000 the still world went from 162–176 µs to 180 µs, up to 11%, which is about what 14% more state per emitter to walk would cost a pass that is bound by it, and the moving world from 170–193 µs to 186–191 µs. The clock is read only when a play starts, so the rest is the size, and a world where that matters has flecs' change detection ("Not yet") to reach for first.

## Not yet

- **A transform component**: `AudioEmitter::position`/`orientation` and the listener's pose are fields of their own because the engine has none yet; when it has one, the system reads it and those fields are migrated away.
- **Sound events for gameplay** — the `Sound` event carrying position, loudness and a propagation budget that [05 §5.15](../plan/05-simulation.md#515-capability-inventory)'s hearing row proposes — belong to perception, not to this system: what the player hears and what an NPC hears are different questions.
- **Change detection by flecs** instead of a diff of every emitter every tick: the diff is exact and cheap at the bench's sizes, and flecs' table-granular change tracking would skip whole tables of still emitters when a world has tens of thousands.
