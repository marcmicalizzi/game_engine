# ADR-0048: The sky is a provider's model drawn by the renderer, and a scene that names none draws what it drew

- **Status:** Proposed
- **Date:** 2026-09-30
- **Plan references:** docs/plan/04-renderer.md §4.2 (step 5, "sky/atmosphere"; step 8, "exposure"), docs/plan/13-reference-consumer-games.md §13.4 item 8 (time of day as a world input) and §13.5 (Phase 1: sky and atmosphere), docs/plan/02-architecture.md §2.8. Builds on [ADR-0027](0027-additive-capabilities.md) (decisions 1 and 3) and [ADR-0046](0046-scene-generators-register-themselves.md) (the registry a scene's named content is looked up in).
- **Docs touched:** [renderer](../subsystems/renderer.md#the-sky), [gfx](../subsystems/gfx.md#the-sky), [scene_gen](../subsystems/scene_gen.md#sky-providers), [sky](../subsystems/sky.md) (new), [terrain](../subsystems/terrain.md) ("Which clock"), [apps](../subsystems/apps.md), [04](../plan/04-renderer.md) and [13](../plan/13-reference-consumer-games.md) (status notes), [the experiment](../experiments/sky-2026-09-30.md).

## Context

Until now the renderer lit every scene with a stand-in: a constant sky colour (`k_sky`, the resolve's clear value and the reference's background), a white sun of intensity 1 on a circle fit through a start (`--sun`, "The sun's day"), two orbiting point lights, a hemisphere ambient of two uniform halves, and a display transform of a gamma curve and a clip, with no exposure. Desert Survival needs a sky that is a function of the world's time: a sun that reddens and dims through the air at dusk, twilight, a moon with its phase and its light, stars, an exposure that follows a hundred-thousand-lux noon down to a moonless night, and the ground lit by the same sky.

A physical sky has two halves that belong in different places:

- **The model**: what the air is made of (Rayleigh, aerosol, ozone), where the sun and the moon stand on a calendar at a latitude, and which stars there are. That is content. A studio with another planet, two moons or a real star catalogue replaces it and changes nothing else; ADR-0027 says such content is a capability, removable, reached through a registration point.
- **The drawing**: the scattering integrals and their tables, the discs, the stars' spots, the air between the eye and a surface, the ambient term, the exposure. Every host needs it, and three readers must agree on it to the byte: the resolve (every covered and uncovered pixel), the ray path (its uncovered pixels) and the reference path tracer (its misses, its next-event estimate and its escaped rays). The resolve and the path tracer are `domain/gfx` shaders, not capabilities.

The alternatives on the table:

1. **All of it in `systems/renderer`**: the simplest wiring, and it makes an ephemeris, an atmosphere and a star table part of a module that cannot be switched off, which is exactly what ADR-0027 decision 1 forbids for content.
2. **All of it in a capability that adds render-graph passes**: the capability would own the tables' shaders, and then the resolve and the path tracer, which must read the sky, would include a capability's shader file. `domain/gfx` cannot depend on a capability.
3. **The model in a capability, the drawing in `domain/gfx` and `systems/renderer`, joined by a registration point** — chosen.

The drawing's method was a second choice. **Bruneton and Neyret's precomputed 4D scattering** is exact for any view but its precomputation is seconds, redone when the air changes, and its 4D table is tens of megabytes; **Hillaire's 2020 LUTs** (a transmittance table and a multiple-scattering table once per scene, a sky-view table and an aerial-perspective volume every frame, each a few hundred thousand texels) fit a sun that moves every frame, an eye at any height, and multiple scattering to a few percent of the brute force, for tens of microseconds. The aerial-perspective volume is **parameterized by direction** (the sky-view table's own azimuth and elevation) and distance, not as a froxel volume of one camera's frustum, because the renderer draws up to eight views of one frame (surround, Panini) and one table serves them all.

## Decision

1. **A third kind of scene generator: the sky provider** (`scene_gen::SkyProviderDesc { name, make }`, `include/domain/scene_gen/sky.h`), in ADR-0046's registry. A scene's optional `sky` entry (`engine.scene.Sky`, a versioned block of `engine.scene.Scene` version 6) names a provider (empty is "earth"); the reader makes it to check the entry and refuses a name the executable does not carry, or an entry the provider refuses, with a sentence. A provider is **a function of game time alone** — the lights and the heavens (`SkyState`) — plus the scene's air (`Atmosphere`) and star table (`Star`).
2. **The capability `domain/sky`** (`OPTIONAL`, `ENGINE_WITH_SKY`, `WHOLE_ARCHIVE`) registers "earth": a low-precision ephemeris (the Astronomical Almanac's sun, Meeus' main lunar terms, the game's hour as apparent solar time), Earth's air at a Linke-style turbidity, and a procedural star table with the sky's magnitude counts to the naked eye's 6.5. Hosts and test targets link it; nothing below a host names it.
3. **The drawing is `domain/gfx`'s and `systems/renderer`'s, and names no provider.** `domain/gfx/shaders/sky.slang` is the sky's functions with no bindings (as `brdf.slang` is the BSDF's), read by the resolve, the ray path and the path tracer through a `gfx::SkyParams` block at a device address; `sky_luts.slang` builds the tables; `domain/gfx/tests/sky_reference.h` mirrors every function and table in double precision, and a test holds the GPU to it. `systems/renderer`'s `SkyPass` makes the provider through the registry, owns the tables' buffers and passes, fills the frame's block and records the passes.
4. **A scene that names no sky draws exactly what it drew.** The resolve's and the path tracer's sky branches run only when the block's address is nonzero; without one every line of the stand-in's code runs as before, and no golden or hash moves.
5. **One clock.** The sky is evaluated at the time the ground's surface stands at (`GpuScene::ground_time_s`: the scene's `Terrain.time`, or a moving terrain's surface time) plus the frame's own offset (`FrameDesc::sun_time_s`), so the sun's hour, the ground's wind (which reads the same game time) and the moon's month cannot drift apart.
6. **Exposure only with a sky.** A scene with a sky is exposed by a declared rule (an incident-light meter at ISO 100 with a knee below which the eye compensates by a fraction of each stop, a tunable, the scene's compensation or fixed value, and the host's keys) and shown through a soft shoulder; the stand-in keeps its gamma curve and its clip.

## Consequences

- A different sky — another planet, two moons, a real catalogue whose licence has been checked, an authored sky for a stylized game — is a provider and nothing else. The drawing has no Earth constant in it but the display's.
- The minimal build has no sky and draws every scene without one; a scene with a sky is refused there, loudly.
- The three readers must stay one function: a change to how the sky is drawn is a change to `sky.slang` and the mirror in the same commit, with the tolerances restated.
- The world is flat for geometry and spherical for the air: the planet's surface is the world's y = 0, and the eye's height in the tables is its world height, clamped to 10 m.
- The stars are not the real constellations until a provider carries a catalogue.
- The exposure is not eye adaptation: it is a function of the frame's light with no history, so a cut from noon to a cave is instant. A histogram or a temporal adaptation would be a step on top of the rule, not a replacement of it.

## Revisit when

- The engine's `GameClock` (Phase 3) owns game time: the sky's time is then the clock's, and `FrameDesc::sun_time_s` a viewer's offset.
- Clouds or the participating-media field ([04 §4.2](../plan/04-renderer.md#42-frame-architecture) step 7's direction note) arrive: the far-field layer must be lit by these tables, and the aerial-perspective volume may become the fog's.
- A second planet or a stylized sky wants a model the `Atmosphere` block cannot say (a third scattering species, a coloured sun): the block grows, append-only.
- Eye adaptation is asked for.
