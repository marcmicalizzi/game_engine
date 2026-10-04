# The ripples' motion against the clock (2026-10-04)

**Question.** Flying `content/test-scenes/desert-endless` at `--time-rate 1`, the owner saw the sand's ripples "only updated every few seconds instead of continuously": they held still and then jumped. His flight record (`endless-2026-10-04T1710-frames.jsonl`, at `8be7e369`) showed the terrain's own clock continuous — `terrain.game_s` up 1/60 s a frame and every level's `surface_s` with it, about 3.6 s behind. So what derived from that clock stepped, by how much and how often, and what does a ripple's speed come to at 1x?

**Answer.** The ripples' travel was drawn from the wind record's flux integral **in whole cm²**. At the scene's celerity of 2,000 m of travel per m² of transport, one cm² is **20 cm of travel, 1.67 of the 12 cm wavelengths**, and at the game's own rate a cm² takes **12 game seconds** to blow at desert-endless's own time (day 1,095, the hour after midnight) and 5.5 s on day 1's morning. So the ripples stood for those seconds and then jumped 20 cm, and on the frame of the jump the 20 cm also counted as "travel this frame", which the time-lapse's shutter fade takes as a blur of 1.67 wavelengths: that frame drew them flat. It was not a float — see "The leads", below. The fix keeps what the integral truncated (`terrain::WindRecord::magnitude_at`: the whole cm² and the fraction of the next one the record's straight line has reached), so the travel is a straight line in the clock within every hour of the record. A second fault showed up at the speed measured. Each kernel took back its own wavenumber's phase, so the reduction of the travel modulo 256 base wavelengths stepped every scaled ripple by `256 / s` cycles, every 30.7 m of travel. On this scene that is every 15 to 30 game minutes, not the "four game days" renderer.md claimed. The phase is now the base wavelength's at every scale: a ripple s times as long moves 1/s as far, and the wrap moves no crest.

**The speed.** At 1x on desert-endless the ripples travel **100 cm a game minute** at the scene's own time (1.7 cm a second), 192 cm a minute averaged over that day, and 79 cm a minute over the year from it. That is a hundred to two hundred times the "half a centimetre a minute" renderer.md stated: the celerity was fitted to a calm day after a storm (36 cm²), and an ordinary day of this record moves thousands of cm². What the owner will see is in [renderer](../subsystems/renderer.md#the-sand-close-up), "Ripples that move". The celerity is the scene's number, and changing it was not part of this.

**Machine.** RTX 5090, `msvc-debug`, the renderer as a library (`GpuScene` + `SceneRenderer` offscreen, not `engine-view`), on the shared desktop with other agents building. Nothing here is a timing: the numbers are positions read from pictures and from the block the shader is handed.

## The leads, and which it was

| Lead | What was found |
|---|---|
| `ground_time_s_` set only on a rebuild's swap | No: `TerrainMotion::frame` calls `GpuScene::set_ground_time` with the surface time every frame, and the owner's record shows that time advancing every frame. |
| `transport()` a staircase | **Yes.** `dunes_transport` returned `WindRecord::integral(t).magnitude * 1e-4`, an integer of cm² — whole units of 20 cm of travel. |
| `wind()` stepping | No: `DuneField::ripple_wind` turns over the day's first two hours in steps of 2 h / 65,536 (0.11 s), at most π/120 a game minute (`ground_detail_rings_tests.cpp`); otherwise constant within a day. |
| A float32 on the way | No: the surface time is a double into `GroundOps::transport`, integer microseconds into the record, and the travel is reduced in double before it becomes a float under 30.72 m (a step of 2e-6 m). The confirmation is the sweep near the clock's zero, below: at 108,000 s, where a float's step is 8 ms, the step was the same 20 cm, its period following the wind (5.5 s) and not the clock's magnitude. |
| The level's two times against the detail's one | No: the detail reads the one surface time; `time_a`/`time_b` time the geometry's blend only. |
| The flatten term or the flow's clock | The flow's turnover clock stepped with the same staircase (0.08 of a cycle per cm² at `flow_turnover` 800) and is continuous now with it. The flattening reads the hour's wind strength, which steps once an hour; inside the flattening band (a storm's few hours) the ripples' weight steps once an hour. Seen, not changed. |

## The CPU instrument: the block the shader is handed

`ground_motion_tests.cpp`, "the travel handed to the shader follows the clock at every rate": the block `renderer::terrain_detail_motion` makes for desert-endless's ground (what `GpuScene::ground_detail_params` makes every frame), 3,600 frames a sixtieth of a second apart at 0.1x, 1x, 60x and 3,600x. For each frame it records the block's travel step, unwrapped across the reduction; that step against the transport's own; and the transport against its hour's straight line.

**Before** (`8be7e369`):

| Start | Rate | Travel | Largest step | From the hour's line | Jumps |
|---|---|---|---|---|---|
| scene's time (94,608,000 s) | 0.1x, 6 s | 0 | 0 | 0.1 m | none in 6 s |
| | 1x, 60 s | 1.0 m | 0.2 m (1.67 λ) | 0.2 m | 5, at 12, 24, 36, 48, 60 s |
| | 60x, 1 h | 60 m | 0.2 m | 0.18 m | 300, every 12 s of game time |
| | 3,600x, 60 h | 2,769 m | 6.2 m | 0.20 m | (a frame is a minute) |
| near zero (108,000 s) | 0.1x, 6 s | 0.2 m | 0.2 m | 0.2 m | 1, at 5.5 s |
| | 1x, 60 s | 2.0 m | 0.2 m | 0.2 m | 10, every 5.5 s |

**After:**

| Start | Rate | Travel | Largest step | Step error | From the hour's line |
|---|---|---|---|---|---|
| scene's time | 0.1x | 0.100 m (100 cm/min) | 2.79e-5 m (2.3e-4 λ) | 1.1e-7 m | 6e-9 m |
| | 1x | 1.000 m (100 cm/min) | 2.78e-4 m (2.3e-3 λ) | 2.3e-7 m | 6e-9 m |
| | 60x | 60.0 m (100 cm/min) | 1.67e-2 m (0.14 λ) | 1.7e-6 m | 5e-10 m |
| | 3,600x | 2,769 m (77 cm/min) | 6.02 m (50 λ) | 1.7e-6 m | 5e-10 m |
| near zero | 0.1x | 0.217 m (217 cm/min) | 6.1e-5 m | 6.9e-7 m | 1.2e-8 m |
| | 1x | 2.173 m (217 cm/min) | 6.0e-4 m (5e-3 λ) | 9.9e-7 m | 1.2e-8 m |
| | 60x | 130.4 m (217 cm/min) | 3.6e-2 m (0.30 λ) | 1.7e-6 m | 2e-13 m |
| | 3,600x | 3,571 m (99 cm/min) | 2.17 m | 1.9e-6 m | 1e-12 m |

No frame stands still while the transport moves, and every step is the transport's to two floats of the reduced period (3.8e-6 m; the block's travel is a float under 30.72 m). The day's travel: 2,768.6 m on day 1,095 (13,843 cm²), 3,129.6 m on day 1; the year from the scene's time 1,133 m a mean day (5,666 cm²).

## The GPU instrument: the pictures

The detail view (`ResolveMode::GroundDetail`: the ripples' height, unfiltered, in red) of desert-endless's ground as a 64 m grid at 50 cm round the origin, from a camera 1.5 m over the windward slope near the origin that climbs fastest into the wind at under 12° (the slope at (-14, 16), the camera a metre upwind of it), 320 × 240. Between two captures the ground does not move (only the detail's clock is set), so every pixel shows the same ground point in both. The displacement d along the wind is the one at which the second picture best matches the first sampled — bilinear in its red — at each pixel's ground point less d along the wind and along the ground. The fit is least squares over 18,096 pixels, on a grid of a fiftieth of a wavelength within half a wavelength, refined by a parabola. The pattern repeats every wavelength along the wind, so a step larger than half a wavelength reads modulo one. A picture against itself reads 0.01 mm.

**Two frames a known time apart** (`ground_motion_tests.cpp`, "two frames a known time apart show the ripples moved by the travel"). The spacing, the patches and the steering are off for this case, so every crest moves the travel along the wind. The two frames are taken 30 s after the scene's time, 1.44 game seconds apart, which is a fifth of a wavelength of travel at the hour's rate.

| | The block's travel between them | The pictures' displacement |
|---|---|---|
| Before | 0 mm (no whole cm² blew in those 1.44 s) | 0.013 mm |
| After | 24.00 mm | 24.09 mm |

The test holds the pictures to the block's travel to a twentieth; before the change it failed. In the same case, a frame at clock value T is the same bytes whether it is reached frame by frame over five seconds or by a jump from a day later with the same last frame. That held before the change too, since the staircase was a function of the clock as well.

**The series** (the same file's skipped case, "the instrument's series", run by name with `--no-skip`). The scene's own numbers, spacing and patches included. A capture every game second for 90 game seconds from the scene's time at 1x, each frame's last frame a sixtieth of a second before it. The step is read between consecutive captures.

- **Before** (the transport in whole cm² and the old phase, rebuilt for the measurement): 83 of the 90 steps read −0.008 ± 0.012 mm, the instrument's floor (a picture against itself), so the ripples stood still. At 12, 24, 36, 48, 60, 72 and 84 s the block's travel jumped 200 mm, and the pictures read −25.3 to −22.5 mm: 200 mm modulo the wavelengths drawn there, which is all a step of 1.67 wavelengths can show. **A staircase of 20 cm every 12 game seconds.**
- **After:** all 90 steps read 15.52 to 15.62 mm (mean 15.56 mm) against the block's 16.67 mm a game second. The pictures' displacement is 1,400.4 mm over 90 s against the block's 1,500 mm: a straight line, the ripples creeping at **93 cm a game minute** on this slope. That is 7% under the block's travel because the spacing and the patches make the ripples here a little longer than the base wavelength, and a ripple s times as long moves 1/s as far.

Before: this change's tests over the transport and the phase as they were at `8be7e369` (put back for the measurement); after: this change. Both at 320 × 240 on the RTX 5090, `msvc-debug`.

## What this does not cover

- **The owner's own flight.** He was hundreds of kilometres out, where a float32 position steps by 3 cm; that is another change. This reproduction stands near the origin.
- **`engine-view` end to end.** The GPU instrument sets the detail's clock through `GpuScene::set_ground_time` directly, the call `TerrainMotion::frame` makes every frame, rather than flying `engine-view --time-rate 1` over the world's tiles. That the surface time reaching that call is continuous is the owner's record's evidence; the instrument covers what is derived from it.
- **The flattening's hourly step** inside a storm's band, and **the speed**: a celerity that runs the ripples at a centimetre a minute at 1x on this day is about 10; which number the erg scenes take is the owner's call.
