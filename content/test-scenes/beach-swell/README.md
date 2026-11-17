# beach-swell

[The beach](../beach/README.md) under a light air with a storm's swell arriving (roadmap W136, third leg; the model is [gfx](../../../docs/subsystems/gfx.md#a-swell-a-scene-names), its grounding [the wind page](../../../docs/experiments/wind.md#swell)): the same bay, ground, boulders and cameras as [beach-wind](../beach-wind/README.md), the sky's wind a fixed 2 m/s (Beaufort 2: glassy wavelets, no breaking offshore), and the sky's `swell` a track over two days from a storm 3,000 km away:

| Hour | Height in deep water | Peak period | Why |
|---|---|---|---|
| 0 | 1.0 m | 17.0 s | the storm's longest waves arrive first: they travel fastest (c_g = g T / 4π) |
| 6 | 2.5 m | 15.5 s | |
| 12 to 18 | 4.0 m | 14.3 to 13.2 s | the swell at its height: the big day |
| 24 to 42 | 3.5 to 2.0 m | 12.3 to 10.2 s | dropping and shortening |

The periods are the dispersion's, T = 4π D / (g t) for waves t seconds after they left a storm D away; the track keys it every six hours and blends by frequency. It travels to 195°, ten degrees off the wind, and its directions are as narrow as 3,000 km makes them (`distance_km`; Goda and Suzuki's spreading, s ≈ 64). The shore's first train is the swell at its largest (4 m at about 14 s), shoaling, turning and breaking where the depth says; the second is the light air's chop.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/beach-swell/scene.json --offscreen --width 1920 --height 1080 --raster rt --shadows rt --time-of-day 15 --time-rate 0 --world-rate 1 --no-lights --capture <file.png>
```

`--time-of-day` picks the hour of the first day (the track's period is 48 h; the sky's clock carries the second). `--tunable renderer.swell.height_m=<m>` (with `renderer.swell.period_s`, `renderer.swell.to_deg`) sets a swell by hand on any open body that draws a wave table, at once; on [beach-wind](../beach-wind/README.md) that is a swell under its own wind. Under `tools/gpu-lock.ps1 run` when it is not a test; run `engine-content matgen` once in a fresh worktree first.
