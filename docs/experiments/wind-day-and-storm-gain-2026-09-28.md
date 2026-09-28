# The wind's day, and storms that scale transport (2026-09-28)

**The question.** The owner's weather note of 2026-09-27 ([13, "Day, night, and weather"](../plan/13-reference-consumer-games.md#day-night-and-weather)) asks for three things. First, a wind that turns with the day. Second, sand that gathers on structures. Third, a storm that scales transport and never the clock. This page is the measurement behind the first and the third ([terrain](../subsystems/terrain.md#the-days-wind), [terrain](../subsystems/terrain.md#a-storm-scales-transport)), and behind the wind rose the second will read ([terrain](../subsystems/terrain.md#the-wind-rose)). It also has the table the owner chooses a storm's gain from. **Where**: the 4-vCPU Linux container, `linux-clang-debug`. Every number is a count or an integral, not a time, so the build does not matter. The tests named are `domain/terrain/tests/wind_day_tests.cpp`, and `systems/renderer/tests/terrain_crest_tests.cpp` and `terrain_clock_tests.cpp`.

## The day's wind

The erg's record (seed 7, 12 storms a year) was run with a swing of 1 (calm at the night's lowest), a peak at 15:00, and a veer of 30° either side passing through the day's direction at noon:

- **A day's sand is redistributed, never added to.** Every day without a storm moves exactly the magnitude it moved without the day, and the period's total magnitude is the same to the unit.
- **The veer shortens a day's net vector**: on average a day keeps **0.971** of its net transport. The dunes therefore migrate about 3% more slowly with the veer on. The overlay's refill reads the magnitude, so it is unchanged.
- **The night is calm.** Over a week, the 15:00 hours moved 10,596 cm² and the 03:00 hours nothing.
- **The closed form is the day-by-day sum.** This holds at every seventh day across two periods and forty days, and back 400 days before the epoch. Every hour's integral is the hour's `wind_at` flux, and the direction stays within the veer of the day's.

## Exact under any split

With the day, the storms and a gain of 50 all on, the test walked 625,521 steps of odd lengths across a year and a half. For every step, the rose's sixteen sectors summed to that step's magnitude exactly. The steps' integrals summed to the whole interval's to the unit, and so did their roses. The same arithmetic a thousand years on also sums exactly. A field with the day and gained storms is the same bytes on no pool, one worker and three.

## A storm's gain, and the table to choose it by

The gain multiplies a storm's own hours, and does so exactly: a gained hour is the plain storm share times the gain, and the day's own share is untouched. The storm's day, hours, direction and strength, and `wind_at`'s direction and strength, are the ungained record's. With a band's `celerity_scale` the two multiply: over a storm, a gain of 10 and a celerity of 4 move the default field's barchans 4 × (their gain-10 travel), to a celerity height's rounding.

**At the game's own rate** (one game second a real second), each band's speed on the committed erg (`content/test-scenes/desert-erg`), in millimetres a second:

- **In a storm**: the mean over all 96 storms of the record's period, averaged over their hours.
- **Strongest hour**: the strongest single storm hour of any storm.
- **Mean day**: the record's mean day at gain 1, for comparison.

Speed is the gained magnitude integral over the band's celerity height (Bagnold, as `DuneField::displacement` takes it).

| Gain | barchan: storm / strongest | crest: storm / strongest | draa: storm / strongest | mega-draa: storm / strongest |
|---|---|---|---|---|
| mean day | 0.0021 | 0.0016 | 0.00038 | 0.000048 |
| 1 | 0.013 / 0.048 | 0.0099 / 0.036 | 0.0024 / 0.0088 | 0.00030 / 0.0011 |
| 10 | 0.11 / 0.44 | 0.086 / 0.34 | 0.021 / 0.082 | 0.0026 / 0.010 |
| 50 | 0.55 / 2.2 | 0.42 / 1.7 | 0.10 / 0.41 | 0.013 / 0.051 |
| 250 | 2.8 / 11 | 2.1 / 8.4 | 0.51 / 2.0 | 0.064 / 0.26 |
| 1,000 (extrapolated) | 11 / 44 | 8.4 / 34 | 2.0 / 8.2 | 0.26 / 1.0 |

The gain multiplies only the storm's share of its hours. The day's own share rides along unscaled, so the barchans' storm mean at 250 is 213 times gain 1's, not 250 times. **Reading it**: a barchan crossing its own length (about 30 m) in a storm hour needs about 8 mm/s, which is a gain of about 180 in a storm's strongest hour and about 750 over a storm's mean. Between storms, the sand stands at every gain, because a mean day at gain 1 moves a barchan 0.2 metres a game day. `desert-erg-storm` takes 1,000. Its storm 42 (ten hours from 09:00 on day 1,095) walks the barchans 204 m, at 0.04, 1.25, 4.72, 9.45, 12.87, 12.87, 9.45, 4.72, 1.25 and 0.05 mm/s hour by hour. The ten hours after it move them 2 cm.

## The cadence reads the gained travel

`terrain_clock_tests.cpp` runs a new case at the game's own rate. The model is the erg's three levels on one field worker at the owner's evaluation times, with a storm of forty seconds of the run's hundred. The storm moves the waves at sixteen mean hours' flux times the gain.

- **Gains 1, 10 and 50**: every pair stayed within a quarter of a sample of the waves' travel. The largest vertex move stayed within its bound, the window ratio was 1.00, and there was no stop.
- **Read against a calm day's travel**: at gain 50, the largest pair was **0.92** of a sample, a cross-fade.
- **With the old floor of a minute between fields** (`renderer.terrain.min_step_s`): **0.50**. So the floor is now a second. It only matters at slow rates, where the displacement cadence is hours.

## What is not done

- **The drifts.** They need the wind's shadow map and a GPU, and are built on the owner's machine. The rose is what they read.
- **Tying the sun's day to the wind's.** The wind's time of day is game time (hour 0 is midnight). The renderer's `FrameDesc::sun_time_s` is a clock of its own, which starts from the sun's configured position. Tying them is `sun_time_s = game time − the time of day the sun's start stands for`, a choice about where the sun starts that is left to the owner.
- **Nothing here ran in a window.**
