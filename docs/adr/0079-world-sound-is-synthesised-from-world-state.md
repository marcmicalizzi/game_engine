# ADR-0079: World sound is synthesised from world state, and nothing of it flows back

- **Status:** Proposed
- **Date:** 2026-11-20
- **Plan references:** docs/plan/05-simulation.md §5.11 (the audio direction note and "wind as sound"); roadmap S110, S42, P43
- **Docs touched:** `docs/subsystems/audio.md` ("Sound for the world: the design (S110)"), `docs/experiments/world-sound.md`, `docs/plan/05-simulation.md` §5.11

## Context

The owner named sound the next milestone beside performance and realism: wind through the trees from the wind's strength and the trees nearby, bird calls from a game's wildlife, water. The mixer (`domain/audio`) plays clips through an object-based stage; no host opens a device for a game, and the hot-load interface's three sound calls answer `Unsupported`.

The usual way to give a world its sound is recorded ambience: loops by biome, crossfaded by place and weather. It was weighed and declined for the engine's own world, for three reasons. The picture here is a function of state (one scene wind and its gusts, a solved flow, a sea with a history, ADR-0066), and a loop cannot follow it: the gust seen in a crown and the wave seen breaking would not be the ones heard. The engine commits no binary asset and ships to consumers with no library to license. And the published models ([the grounding page](../experiments/world-sound.md)) give each of these sounds a level in closed form of state the engine already holds and a spectrum's shape, which costs of the order of a clip voice (an estimate until its bench row runs) and holds no samples in memory.

The alternatives weighed for the synthesis: band-limited noise clips generated at start and played by the existing voices (a source with a spectrum becomes eight voices); a general DSP graph on the audio thread (a second engine inside the mixer, for one generator); a fluid- or particle-driven bubble model (the literature's, and the engine has no such simulation). For a game's interface: returning the mixer's voice and its refusals to the module (the obvious API, and it makes a module's state depend on when a sound card asked for a block).

## Decision

1. **The world's continuous sounds are synthesised from the state the picture reads; a game's discrete sounds are clips.** Wind at the listener, wind in vegetation, streams, falls and surf are computed from the frame's wind as every reader is given it (the scene's, or the live state a session set), the trees near the listener, the flow solve and the sea. A bird's call, a door, a voice and music are clips a game declares and plays. The engine ships no recorded ambience and its own scenes use no clip where a synthesised source exists.
2. **The mixer gains exactly one generator, the shaped-noise voice**: a seeded integer noise source, a fixed bank of octave band filters with literal coefficients, one movable band, and a few struck resonators, its parameters one command. No other signal processing is added to the audio thread for the world, nothing on it calls the C library, and the module's claim holds: the same commands give the same bytes on every toolchain.
3. **Far sources merge by power into a fixed set of beds; near ones are voices of their own.** The counts are tunables and their sum is the world's share of the voice pool. No source is dropped silently: what is not a voice is in a bed.
4. **Sound is presentation.** It is `derived`: the wind, the placements, the flow, the sea and the listener's pose are world state; the fields, the voices, the seeds and the device are not hashed, saved or replayed.
5. **Nothing a game module can observe depends on audio.** `sound_play` answers `Ok` with a handle the host counts, whether or not a voice sounded; `sound_stop` answers `Ok` for any handle issued; the interface has no call that reports a voice's state, a level or a device. A host with sound off answers exactly as one with sound on.
6. **The host opens the device; a headless run and every test use the null backend.** engine-view opens one only for a session a person sits at. A test and an agent judge sound by the null render's bytes and band levels, never by a device.
7. **One capability, `world_sound`, requiring `audio` and no other.** Trees and water reach it through source tables a host fills, keyed on properties any asset can declare or that are measured from any mesh; it links no generator, no solver and no other capability.

## Consequences

- What is heard and what is seen cannot disagree about the wind or a wave, since both are one function of one clock.
- A recording made with sound replays without it, and on a machine with another device, to the same state.
- The world's sound costs no asset, no download and no launch memory; it costs voices, which are budgeted, and a fidelity ceiling: shaped noise and struck resonators are a model, and a listener may find a real recording richer. The grounding's unread sources and stated choices are where that is first improved.
- A game cannot ask whether a sound finished; it times itself by ticks and a clip's declared length.
- The scene root takes one more block (`sound`), a recorded raise of its ceiling.
- Forbidden: a clip standing in for a synthesised source in the engine's own scenes; a second wind for sound; a generator or filter on the audio thread beyond decision 2 without a new decision; a game call that returns mixer state; a link from `world_sound` to a capability other than `audio`; a test that needs a device.

## Revisit when

The owner, hearing S112 or S113, finds a synthesised source not good enough: the next step is then granular synthesis from recordings the owner approves, as a second source kind, and decision 1's last sentence is what changes. Steam Audio lands (S40): the path's terms and the ears move behind its seam. The water moves to simulated cascades, when a bubble population can be drawn from the simulation instead of the flow's state. A game needs to hear what a module did (a sound as feedback into gameplay): that is a simulation event (S55), and it does not reopen decision 5.
