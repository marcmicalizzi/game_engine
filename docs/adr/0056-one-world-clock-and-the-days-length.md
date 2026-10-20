# ADR-0056: One world clock, and the day's length

- **Status:** Proposed
- **Date:** 2026-10-20
- **Plan references:** docs/plan/04-renderer.md (the sky), docs/plan/13-reference-consumer-games.md §13.1; supersedes decision 5 ("One clock") of [ADR-0048](0048-the-sky-is-a-providers-model-drawn-by-the-renderer.md)
- **Docs touched:** `docs/subsystems/renderer.md` ("One world clock, and the day's length", "The discs' sizes", "The dunes in time-lapse", "Fires", "The flora stage"), `docs/subsystems/apps.md` ("The two rates on four keys", `--game`), `water.md`, `volumetrics.md`, `flora.md` ("Motion"), `terrain.md` ("Which clock"), `docs/game-authoring.md`, `AGENTS.md`

## Context

ADR-0048 decision 5 put the sky on the ground's surface time plus the frame's offset, and every other thing that moves (the water, the medium's drift, a fire's flicker, the flora's gust) followed onto that one clock, so the sun's hour, the wind's day and the moon's month could never disagree. The cost showed in the proof-of-concept game: with its sun at 60× the smoke, the waves, the sway and the cloud drift ran at 60× too; a dune time-lapse dragged the sky round with the sand and turned the screen into a day-night strobe; and a production game that wants a sixty-minute day with every animation at real speed could not have one. The owner's framing (2026-10-20): keep one world clock at real time that every animation reads, and make the sky's day length the knob; then (the same day) let `,` and `.` scale that world clock for inspection while `[` and `]` set the day's length, measured in real seconds.

Alternatives considered: a day measured in world seconds (the sky follows the world rate) — equivalent in words, but a world rate of 0 would stop the sun too, and the sky's sum would depend on the world rate's history; one clock with per-reader rate multipliers — every reader would need to know every rate, and a replay would have to carry them all.

## Decision

1. **Three times, each a sum of the session's ticks** (`renderer::WorldClocks`, `systems/renderer/clocks.h`), with `session_s = ticks / tick_hz` unscaled: the **world clock** `world_start + Σ dt · world_rate`; the **sky's hour** `epoch + sky_start + Σ dt · sun_rate`; the **ground's time** `epoch + time_rate · (world − world_start)`. `epoch` is the scene's `Terrain.time`; `sky_start` the `--time-of-day` offset.
2. **The day's length is in real seconds** and is said as `sun_rate = 86,400 / day_length_s`; the rate is what is stored, so every flag, key, request and state file that spoke it keeps its bits.
3. **The reader rule: a light source is on the sky's clock, a surface or a medium that moves is on world time, the ground's geology is on its own time-lapse of it.** The wind the world feels is read from the record at the sky's hour; what it moves, moves on world time.
4. `FrameDesc` carries `world_time_s` and `sky_time_s` (the sky's seconds past the epoch); `sun_time_s`, an offset on the ground's moving surface time, is retired and every reader migrated.

## Consequences

- A dune time-lapse never moves the sky; a sixty-minute day draws the same water, smoke and sway as a real day at the same world time; a world rate of 0 pauses every animation while the sun keeps its pace.
- Frames at a held or real day with no time-lapse draw the sky they drew. The water, a fire's flicker and the medium's flames and drift stand at the world time, which starts at 0, where they stood at the epoch plus the sun's offset: pictures of those at a non-zero epoch change, and they no longer freeze under a held sun.
- The state file's session block (version 2), the recording header (`clock` block) and the game tick view (`GameTick` version 2: `world_time_s` is now the world clock, `sky_time_s` the sky's hour) carry the clocks, so a replay is a replay.
- The far clouds' drift is a closed-form integral of the record from the world's start, so its direction follows the record counted on world time rather than the sky's hour; matching the sky's hour while moving by world time would need a history.

## Revisit when

A game needs a clock this does not give (a second sky, a calendar that skips), or the clouds' drift direction and the sky's weather are seen to disagree.
