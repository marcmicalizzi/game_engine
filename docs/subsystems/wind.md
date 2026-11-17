# wind (the scene's wind; `domain/gfx` and `systems/renderer`)

**Purpose.** One wind a scene names, that every reader asks: the sea's and a lake's waves and their shore, and the trees' sway; later the foam's whitecaps and spray, the dust and a game module. Roadmap W136 (2026-11-16); the published models, the numbers chosen and the Beaufort check table are [the grounding page](../experiments/wind.md). It is not a module of its own: the model is a header of `domain/gfx` (`include/domain/gfx/wind.h`, beside the water's `water_spectrum.h`, since the renderer and gfx's tests both read it and the renderer may read no capability), and the scene's half is the renderer's (`include/systems/renderer/scene_wind.h`, `src/scene_wind.cpp`).

**What existed before** (the grounding page has the survey): a water body's own fixed `wind_m_s`, the dunes' record (a strength relative to its mean, the "dunes" ground's alone), and the trees' wind from the host, the dunes or tunables. Nothing a scene could say in metres a second changed in time, and the water's table was made once, at load.

## The block

`Sky.wind` (`engine.scene.Wind`, `Sky` version 3; the wind is the air's, so it lives with the sky, and the `Scene` root stays at its ceiling of 20 fields). Absent — every scene written before it — **every reader draws exactly what it drew**.

| Field | Default | Meaning |
|---|---|---|
| `speed_m_s`, `to_deg` | 0, 0 | a **fixed** wind: the ten-minute mean 10 m up, m/s, and where it blows **to** (about +y from +x towards +z, as a water body's `direction_deg`) |
| `track`, `period_h` | none, 24 | a **track**: keys `{hour, speed_m_s, to_deg}` in order of hour, blended linearly (the direction the shorter way round), wrapping over the period |
| `climate` | null | a **climate**: `mean_m_s` 6, `weibull_k` 2, `to_deg` 0, `veer_deg` 35, `synoptic_h` 48, `diurnal` 0.3, `peak_hour` 15, `seed` 1 |
| `roughness_m` | 0.03 | the ground's z₀ upwind, for the gusts' strength: 0.0002 the sea, 0.03 grassland, 0.5 to 1 a forest |

Refused with a sentence naming the field: a speed outside [0, 60] m/s, a track key out of its period or out of order, a track and a climate both, a climate number out of its range, a roughness outside (0, 2] m.

## The interface

`gfx::wind_at(desc, time_s)` — the mean wind at the sky's time (clocks.h: the weather follows the day, as the dunes' record and the trees always have). `gfx::wind_gust(desc, mean, place, time_s)` — the instantaneous speed at a place: Kaimal's turbulence at the roughness's intensity, frozen into the flow and carried down the wind. `gfx::sea_state(desc, time_s, fetch_m)` — **the sea the wind has raised by then** over a fetch: the wind sea's significant height, peak period, direction and equivalent fetch, and the swell it left behind (height, period, direction). These three are what a reader asks; **the whitecap cover and the spray read `sea_state`'s `wind_m_s` and `sea_height_m`**, and the trees `wind_gust`. Each is a closed function of the time: no state, no frame count, the same answer however the time was reached.

The mean is uniform over the scene (the weather is larger than one); the gusts are the one part that depends on the place. A sea breeze, a hill's speed-up and lee, a forest's shelter are models of the wind at a place that `wind(place, time)` is shaped to take, and none is built.

## The sea it raises

`sea_state` steps the 72 hours before the time asked in half-hours of a fixed grid of the clock (the last step partial), each at the wind of its middle, by the coastal engineering manual's equivalent-duration method over the JONSWAP laws the water already draws: the sea's energy re-expressed under the step's wind as an equivalent fetch, the step's duration added by the manual's duration law, the result capped at the fetch and at full development; energy the wind cannot hold (it dropped, turned past 45°, or fell under 1 m/s) leaves as swell at the period it had, and the swell drains with an e-folding time of fetch over its group speed. **A fixed wind's sea is its fetch-limited sea without stepping**, which is the sea a body's own `wind_m_s` always drew.

| At | Over 60 km (the beach) | Over 900 m (the forest's lake) |
|---|---|---|
| 8 m/s, grown | 1.32 m at 4.9 s | 0.13 m at 1.2 s |
| 20 m/s, grown | 2.97 m at 6.6 s, in 5.8 h | 0.29 m at 1.6 s, in 21 min |
| 20 m/s dropping to calm | swell at 6.6 s, its energy e-folding in 3.2 h | swell gone in minutes |

## The water reads it

A body with `wind_follow` (the default) in a scene with a wind takes **the scene's** — its speed and direction — in place of the ground's and its own, open or closed (`scene_water.cpp`); a closed body's fetch is still its own reach along the wind. Then:

- **A fixed wind** draws exactly the table, the shore and the run-up the body's own `wind_m_s` at that direction drew: `water_spectrum_on_grid` on a fixed wind's grid is `water_spectrum` to the bit (the test holds every byte), and nothing per frame.
- **A varying wind** (a track or a climate) draws its table on **one grid of sixteen bands** that spans every state the wind raises over the body's fetch (`gfx::water_wind_range`: 240 states sampled over the track's period or sixty days of the climate; half the longest peak to four times the youngest, no wider than 32 to one, keeping the long end). The bands' wavenumbers, cycles and phases never change, so **no wave jumps as the sea grows**: only the amplitudes, steepness, the young sea's share and the directions follow the state. The load draws the largest sea (the instance's bounds and the shore's train are sized by it), and from the first frame the GPU scene redraws each body's table at the sea of the sky's time, quantized to ten seconds, whenever that changes (`GpuScene::water_wind_frame`, `renderer::water_wind_frame`): 512 bytes into the material's wave entries and the body's 648-byte `WaterSurface` (its table, its train's height, run-up and reach), copied by the frame's **"water wind"** transfer pass, with the material table and the surfaces imported as last read everywhere so its barrier waits for the frames in flight, and every pass that reads the geometry declaring them.
- **The swell** a dropping wind leaves is drawn in the same bands, from its own direction with its own spreading where it holds a band.

**The hand wind** (`renderer.wind.speed_m_s`, `renderer.wind.to_deg`; below 0, the default, the scene's): a walker sets a wind from calm to a gale while a scene runs, and **every body that draws a wave table** — a scene wind's or a body's own — takes it **at once**, as the fully grown sea of that wind over its fetch (a walker asking for a gale wants to see one now, not in six hours), on that wind's own grid; the trees take it too. Clearing it brings the scene's sea back. A body that had no wind at load (the four sines) is not changed by it, and the hand wind's sea is not part of any clock: a capture with it set is that wind's picture.

## What it does not do

- **The shore's train keeps its load period and direction**: the arrival and energy-flux fields are built once for them (water.md, "The swell", "The surf"), so as the sea grows the surf's height, run-up and reach follow it and its period and angle of approach do not. A closed body's lee is its load direction's.
- **The instance's bounds are the largest sea's** for a varying wind, and the load's for a hand wind: a hand gale on a body loaded calm may lose a lifted crest at a cluster's edge to the cull.
- The grid is coarser than a fixed wind's (up to ln 32 / 16 = 0.22 a band against 0.13).
- Nothing reads the wind for the foam's whitecaps or spray yet (the foam is being rebuilt in another row), nor the dust, nor a game module (no ABI change).

## What it costs

**A fixed wind or none: nothing** — no state is stepped, the key never moves, nothing is copied. A varying wind: one sea state per body every ten seconds of sky time (145 steps of a closed form, a few tens of microseconds), the table's sixteen bands, and on those frames two inline copies a body behind one barrier. The measured pair is in [the roadmap row's close](#testing).

## Testing

`domain/gfx/tests/wind_tests.cpp`, no device: a fixed wind's table is `water_spectrum`'s to the bit and its height `swash_wave`'s, over the beach's, the lake's and a gale's fetch; a track's blend, its wrap and the shorter way round; a climate's mean and Rayleigh spread over 20,000 hours and its continuity at a knot; the gust's intensity at grassland roughness and its travel down the wind; the sea's growth from calm to the fetch's sea and never past it; the swell a dropping wind leaves and its drain (a lake's in minutes); the state continuous across the history's steps and the same bytes twice; and a varying wind's table on one grid whose bands never move and whose variance is the sea's within a fifth.
