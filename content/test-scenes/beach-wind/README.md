# beach-wind

[The beach](../beach/README.md) under a wind that changes through the day (roadmap W136; the model is [gfx](../../../docs/subsystems/gfx.md#the-wind-a-scene-names), its grounding [the wind page](../../../docs/experiments/wind.md)): the same bay, ground, boulders and cameras, the sea's own fixed 8 m/s replaced by the sky's `wind`, a track of the day:

| Hour | Wind 10 m up | The sea it raises over the bay's 60 km (`gfx::sea_state`) |
|---|---|---|
| 00:00 to 06:00 | calm | no wind sea: glass under the last of the evening's swell, its height e-folding in 6.4 h (about a third of the gale's at 05:00) |
| 06:00 to 12:00 | rising to 20 m/s (a gale, Beaufort 8) | a young sea, short and steep, growing |
| 12:00 to 18:00 | 20 m/s | grown to the fetch's sea by about 17:50 (2.97 m at 6.6 s) |
| 18:00 to 23:00 | falling calm | the wind sea calms with it; what it cannot hold runs on as swell |
| 23:00 to 24:00 | calm | the swell, draining over three hours |

`roughness_m` is the open sea's, 0.0002 m. The body follows the wind (`wind_follow`), so its own `wind_m_s` is not read.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/beach-wind/scene.json --offscreen --width 1920 --height 1080 --raster rt --shadows rt --time-of-day 15 --time-rate 0 --world-rate 1 --no-lights --capture <file.png>
```

`--time-of-day` picks the hour, and with it the wind and the sea (the wind is on the sky's clock); a held day (`--sun-rate 0`, the default offscreen) holds the sea. `--day-length <s>` runs the day, and the sea with it. `--tunable renderer.wind.speed_m_s=<m/s>` sets a wind by hand, its sea at once. Under `tools/gpu-lock.ps1 run` when it is not a test; run `engine-content matgen` once in a fresh worktree first.
