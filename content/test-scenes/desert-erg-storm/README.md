# desert-erg-storm

The erg of [`desert-erg`](../desert-erg/README.md) — the same seed, bands and camera path — with the **wind's day** on and every **storm's transport multiplied by 1,000**, starting at 12:50 on the erg's own day (three years in, day 1,095 of the wind record), in the ten-hour storm that blows from 09:00 to 19:00, ten minutes before its two strongest hours. It is for flying at the **game's own rate**: the barchans walk while the storm blows and stand between storms, and the clock — the wind's direction, the day and night it follows, everything the player lives by — runs at one game second a real second throughout ([terrain](../../../docs/subsystems/terrain.md#a-storm-scales-transport)). **The gain is a stylization, not physics**: a real storm moves tens of times a mean day's sand, and a real barchan still only fractions of a millimetre a second.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg-storm/scene.json --interactive --terrain-rings --time-rate 1
build/msvc-release/bin/engine-content terrain content/test-scenes/desert-erg-storm/scene.json --tile 0,0 --storms
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg-storm/scene.json --interactive --terrain-rings --time-rate 60
```

`--time-rate 1` is the game's rate: the sand moves because the storm's transport is gained, not because time is fast. `--time-rate 60` (a game hour a real minute) is a viewer's time-lapse for seeing the storm end and the calm evening after it without waiting five hours — the storm is then a faster clock again, which is exactly what the gain exists so that a game does not need. `engine-content terrain --storms` lists every storm of the record's eight-year period, in day order: storm 42 is this one, and a scene's `storm_gains` can give any of them a gain of its own.

## What differs from the erg

| Field | Here | The erg | Why |
|---|---|---|---|
| `time` | 94,654,200 s | 94,608,000 s | 12:50 of the same day: inside storm 42 (09:00–19:00), ten minutes before its strongest hours |
| `diurnal_strength` | 1 | 0 | the wind's strength swings from calm at 03:00 to its peak at 15:00 (the day's sand redistributed, never added to) |
| `diurnal_peak_hour` | 15 | — | mid-afternoon, after the day's heat |
| `diurnal_veer_deg` | 30 | 0 | the wind veers 30° either side of the day's direction, through it at noon |
| `storm_gain` | 1,000 | 1 | every storm's hours move a thousand times the sand their wind does |

Everything else — the band table, the storms' days, hours, directions and strengths (twelve a year), the sand flux, and the sand's ripples and grain (`detail`, [the erg's README](../desert-erg/README.md#the-sand-close-up)) — is the erg's, so the storms are the erg's storms, at the same hours.

## What to look at

- **The barchans walking** (the crescents on the interdune floors, 1.5–5 m): at 13:00–15:00 they move **12.9 mm a second**, about 0.8 m a minute, downwind (the storm moves the sand towards 189°, about −x). The storm's hours, from 09:00: 0.04, 1.25, 4.72, 9.45, 12.87, 12.87, 9.45, 4.72, 1.25, 0.05 mm/s — a half-sine of wind, cubed.
- **The crests** (2.5–6 m) at three quarters of that, **the draa** (10–25 m) at a fifth, **the mega-draa** at a fortieth: 0.3 mm/s at the peak, a still horizon.
- **The storm's end, and the evening**: from 19:00 the barchans stand — the ten hours after the storm move them 2 cm — and the night's wind falls calm by the day's own profile.
- **The ripples under your feet** (`--walk`): they lie across the **day's** wind, the ripple term's rule, and so do not turn with the storm or the day's veer; they stand while the barchans walk over them and do not flatten in the storm's wind. That is the detail's first version — a function of position that turns with the wind but does not migrate ([renderer](../../../docs/subsystems/renderer.md#the-sand-close-up), "What does not reach it yet") — and a storm is where it shows most.

## The numbers

Over storm 42 (its ten hours, the gain on; `terrain_crest_tests.cpp`, "the storm erg's barchans"): the barchans travel **204 m**, the crests about 156 m, the draa about 38 m, the mega-draa about 4.7 m — its 662.6 m² of gained transport over each band's celerity height, the day's own 0.72 m² beside it. The ten hours after it: 2 cm. Across all the erg's storms at the game's rate ([the experiment](../../../docs/experiments/wind-day-and-storm-gain-2026-09-28.md)), a gain of 1,000 walks the barchans about 11 mm/s through a storm on the mean and 44 mm/s at the strongest hour of any, where a day without a storm moves them about 2 µm/s.
