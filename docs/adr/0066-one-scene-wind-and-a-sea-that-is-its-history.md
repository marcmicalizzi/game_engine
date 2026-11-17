# ADR-0066: One scene wind, and a sea that is a function of its history

- **Status:** Proposed
- **Date:** 2026-11-16
- **Plan references:** docs/plan/05-simulation.md §5.15 (the capability inventory's fluids and water surfaces); roadmap W136
- **Docs touched:** `docs/subsystems/gfx.md` ("The wind a scene names"), `docs/subsystems/water.md` ("The scene's wind"), `docs/subsystems/sky.md`, `docs/experiments/wind.md`

## Context

The owner asked for variable wind, to exercise the larger waves and flat calm. Until then each water body carried its own fixed `wind_m_s`, made into a wave table once at load; the dunes' wind record (ADR-0043) is a strength relative to its mean, owned by one ground provider; the trees swayed in whichever of the host's wind, the dunes' record or tunables was there. Nothing a scene could say in metres a second changed in time. The water's shore (arrival and energy-flux fields) is built once per body for one period and direction; the wave table is sixteen Gerstner bands whose phases are whole cycles of a 1,024 s clock. A sea does not follow its wind at once: it grows with fetch and duration and outlives its wind as swell (the grounding page).

The alternatives weighed: a wind block on each water body (one wind per reader, the thing to avoid); a root `wind` field (the `Scene` root is at its ceiling of 20 fields); a wind provider found through the scene-generator registry (a capability the renderer cannot depend on, for a model that is a few closed forms); a per-frame simulation of the sea (cheap, but a capture at t would depend on the frames before it); re-gridding the table to the current peak as the sea grows (every band's frequency moves, so every phase jumps at each change).

## Decision

1. **One wind a scene names, under its sky** (`Sky.wind`, `engine.scene.Wind`): fixed, a track through a period, or a climate drawn with a seed; absent, every reader draws what it drew. The wind is the air's; the sky provider carries it and never reads it.
2. **The model is closed forms of the sky's clock in `domain/gfx/wind.h`**: `wind_at(desc, t)`, `wind_gust(desc, mean, place, t)`, and `sea_state(desc, t, fetch)`. The sea state is the coastal engineering manual's equivalent-duration method over JONSWAP's fetch laws, stepped on a fixed grid of the clock over a fixed history window, so **it is a function of t alone**. A fixed wind's sea is its fetch-limited sea without stepping, which is what a body's own wind drew.
3. **A varying wind's table is drawn on one grid of bands** spanning every state the wind raises over the body's fetch; only amplitudes, steepness, the young sea's share and directions follow the state, so no wave's phase ever jumps. A fixed wind's grid is its own peak's, so its table is `water_spectrum`'s to the bit.
4. **The renderer redraws, the shaders do not change**: a body whose sea moved has its wave table and `WaterSurface` copied in by one transfer pass in the frame, the buffers declared to the render graph like the record upload's. No shader knows the wind.
5. **Whitecaps, spray, dust and a game module read `sea_state` and `wind_gust`** rather than a wind of their own.

## Consequences

- A wind that holds still costs nothing per frame; a varying one a few tens of microseconds per body every ten sky seconds and two small copies behind one barrier.
- The shore's train keeps the period and direction its fields were built for; making those follow the sea needs the arrival field per period band, a later row.
- The bands are coarser for a varying wind than for a fixed one (up to 32 to 1 over sixteen bands against 8 to 1).
- Forbidden: a reader with a private wind; a sea state that depends on the frames before it; moving a body's band frequencies while it draws.

## Revisit when

The water moves to FFT cascades (the grid becomes the cascades' fixed spectrum and the table goes away); the wind needs to vary over a scene (a coast's sea breeze, a hill's lee, a forest's shelter), when `wind_at` takes the place it already names; or a game module needs the wind (a host interface row).
