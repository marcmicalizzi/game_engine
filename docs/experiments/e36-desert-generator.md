# E36: What the desert's dune generator costs on the CPU, and what a tile stores

- **Question:** the terrain capability ([terrain](../subsystems/terrain.md), [ADR-0043](../adr/0043-dunes-as-a-function-of-time.md), proposed) makes the endless desert's dunes a closed-form function of (seed, tile, game time) and the player's changes a bounded overlay. Is a tile cheap enough to evaluate when the world ring activates it, does a tile a thousand years in cost what a tile at t = 0 does, what does the overlay's decay cost a held tile, and how many bytes does a trampled tile actually store against its bound? [05 §5.13](../plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies) and [07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content) asked for the generator; no experiment row in [10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) names these numbers, so this page is where they live.
- **Date:** 2026-09-25. **Machine:** a cloud Linux container — Intel Xeon @ 2.10 GHz, 4 logical CPUs, 15 GiB, Linux 6.18; **GPU:** none. **Build:** `linux-gcc-release` (GCC 13.3, RelWithDebInfo, x86-64-v3, contraction off).
- **Machine state:** quiet. Every run with `--wait-quiet`, and the harness recorded other processes at 0% of the CPU before and after (`machine_state` in the JSON header). A container on a shared host: the neighbours the harness cannot see are the caveat below.
- **Decision:** the defaults of [terrain](../subsystems/terrain.md) (tile cells by distance, the overlay's rules) and the bound in [ADR-0043](../adr/0043-dunes-as-a-function-of-time.md). Nothing here was measured on the owner's machine; the visual acceptance is his fly-through.

## Setup

The benches are `domain/terrain/bench/terrain_bench.cpp`; the storage numbers come from the tests in `domain/terrain/tests/overlay_tests.cpp`, which print them.

```
cmake --preset linux-gcc-release && cmake --build --preset linux-gcc-release --target engine_terrain_bench engine_terrain_tests
build/linux-gcc-release/domain/terrain/engine_terrain_bench --repeats=7 --wait-quiet=120 --json=e36.jsonl
build/linux-gcc-release/domain/terrain/engine_terrain_tests -s -tc="overlay*"
```

The field is the default description with seed 2026: 3 m dunes 90 m apart, 200 m² a year of sand flux, no ridge or basin (a ridge adds a segment distance per point per ridge within reach). A tile is 32 m with a one-vertex apron, heights, normals, materials and crest lines.

## Results

**A tile** (`terrain.tile.eval`, median of 7 repeats; the minimum beside it):

| Cells a side | Vertices | Median | Min | Per vertex |
|---|---|---|---|---|
| 32 (1 m) | 1,089 | 0.283 ms | 0.267 ms | 260 ns |
| 64 (50 cm) | 4,225 | 0.827 ms | 0.737 ms | 196 ns |
| 128 (25 cm, the overlay's grid) | 16,641 | 2.622 ms | 2.401 ms | 158 ns |
| 128, a thousand years in (`eval_far`) | 16,641 | 2.659 ms | 2.432 ms | 160 ns |

**The rest:**

| Bench | Median |
|---|---|
| `terrain.overlay.decay/20` — an hour's advance, a few footprints | 6.2 µs |
| `terrain.overlay.decay/2000` — an hour's advance, every block trampled | 230 µs |
| `terrain.wind.integral` — the closed form | 9.4 ns |
| `terrain.field.build` — a field from its description (2,920 wind days, prefix sums, bands) | 124 µs |

**What a tile stores** (the overlay tests, default rules, a wind of seed 3–29):

| Case | Result |
|---|---|
| a 4 cm footprint | buried after 5 game hours; the tile then stores 0 bytes |
| 30 days of a player crossing one tile all day — 12,010 footprints, a 0.9 m pit every third day | largest record 28,728 bytes (bound 33,848); buried 4 days after the last stamp, leaving a 56-byte header holding the lag the pits made (10 units) |
| a 0.5 m drift a new wall is short of | at its steady state in 5 days: 0 bytes |

Before the reciprocals — the first version divided by each primitive's widths per point — the 128-cell tile was 3.56 ms (same build, load average 0.6 and not a quiet run, so an upper bound).

## What surprised me

- **A thousand years costs what three do**, to within the spread (2.66 against 2.62 ms): the claim the whole design rests on, and the reason a tile's lattice moves with the wind rather than a tile owning its primitives. The primitives a tile gathers are the same number at any t (37–42 for the reference tile at 0, 90 days and 25 years).
- **The cost is the primitives, not the arithmetic's type.** A point tests every primitive of its gather (about 40); the draa's reach is some 300 m, so almost every draa primitive passes its bounding test at every point and computes a profile. Integer arithmetic is not what makes it slow: the divisions were, and moving them into the gather took a third off.
- **The bound is the grid, and a month of trampling does not reach it.** A player crossing a tile all day for a month touched 28 KB of its 32 KB of grid; weeks and a day store the same kind of number, and four days after the player leaves the tile is its lag.

## What it decides

- **Tile cells by ring.** At 2.6 ms a 25 cm tile is too much for eight activations on one frame (21 ms), and too fine for a tile 256 m away anyway. The world consumer builds tiles at 128 cells in the inner ring (the overlay's grid, where the player is), 64 in the middle and 32 beyond: eight outer-ring activations are 2.3 ms, and the performance pool (`evaluate_tiles`) spreads them.
- **The overlay can advance every tick** where the player is: a settled tile is microseconds, and even a fully trampled one is under a quarter of a millisecond an hour of game time — and since an advance is exact at any cadence, a host that wants less can advance once a second and lose nothing.
- **The bound stands as stated**: 33,848 bytes a tile, and nothing once buried.

It does not decide how the dunes look — that is the owner's fly-through on his machine — or what the renderer's pass for blowing sand costs.

## Caveats

- One container on a shared host: the harness sees the container's processes, not its neighbours'. The per-vertex numbers are within 10% of a noisy run taken before the quiet one.
- GCC only for the timing; Clang reproduces the same bits (the golden hashes) but was not timed.
- No ridges in the bench field: a scene's ridges add a segment distance per point per ridge within twice its width, and the desert overlook has three.
- The storage numbers are the overlay tests' walks, not a played game's: a real player's footprints follow paths, which touch fewer blocks than the tests' sweep.
