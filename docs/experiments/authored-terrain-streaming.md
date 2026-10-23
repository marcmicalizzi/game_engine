# Static authored terrain through the stock terrain levels

2026-10-23, roadmap W67; [ADR-0057](../adr/0057-ground-providers-expose-their-native-tile-source.md).

**Machine state:** shared Windows desktop, RTX 5090, MSVC debug; Linux Clang debug in the local
container. Builds and other sessions ran beside these checks. The counts and equality assertions
below are deterministic; no timing in a busy debug log is claimed as a frame cost.

## The missing integration

At `84c183ac`, authored sets already matched an independent heightfield reference through the
ring renderer, but a stock host selected terrain levels only for moving ground. Its still-source
adapter also went through `GroundOps::grid`, which cannot refuse a missing filtered mip. The
typed source used by the focused test preserved that refusal; stock hosts did not reach it.

`GroundOps::tile_source` now carries that native source through the registry, leaving the renderer
independent of the tileset capability. Tile selection depends on the provider's millimetre
capability, and the actual source/filter requests are checked during setup. Static levels keep
their authored time even after a nonzero rate request. The hidden grid is a 17-point material/UV
carrier; the original extent remains the UV frame. Geometry-page streaming is not enabled.

## A steep slope exposed an unsafe encoding

The new stock GPU fixture failed its first frame on both hardware and vertex raster paths. The
existing gentle authored-reference fixture still passed. Vulkan validation was requested for
diagnosis, but the Khronos validation layer was not installed.

The stitch decoder treated `abs(normal.y) < 0.25` as an encoded level index. That was safe for the
dunes' gentle normals, and ambiguous for an authored cliff: its x/z angle could select a level
outside the GPU table. Moving the sixteen markers down 45 degrees makes them disjoint from real
heightfield normals at every slope. Ring skirts do not set the named-level flag. The original
failing stock fixture then completed its travel and return captures. No mirrored type grew.

## Bounded source residency and work

The previous reader allocated a pointer slot for every source tile and retained every payload it
visited. The reader now uses a sparse FIFO cache under a configurable payload budget, with indices
bounded by the same number of tiles. Lattice requests hold the reader lock while sampling so an
eviction cannot invalidate another worker's pointer. A last-tile hint avoids hashing adjacent
samples. The provider's default is 64 MiB (`tileset.cache_mib`); callers of `TileSet` can supply a
byte budget. Admission temporarily holds both the incoming tile payload and its file-read buffer
in addition to the retained payloads; the cache index and manifest are metadata rather than
payload bytes. These transient allocations are bounded by tile size, not source extent.

The content build remains proportional to source extent and may run on a derived-data cache
miss. These bounds describe an already-built set and runtime residency, not that preprocessing
step; large authored sources can be built with engine-content before the runtime opens a manifest.

The fixed-footprint CPU case samples an 8 m square at 50 cm, 289 points, across four source tiles
of sixteen cells. Each tile includes its apron: 19² integer heights, 1,444 bytes.

| Source side | Source extent | Tile reads for the footprint | Resident payload |
|---|---|---|---|
| 129 points | 64 m | 4 | 5,776 bytes |
| 257 points | 128 m | 4 | 5,776 bytes |
| 513 points | 256 m | 4 | 5,776 bytes |

A separate two-tile cache case travels in both directions, teleports beyond the finite source,
returns, and runs concurrent readers while entries are evicted. Heights remain the independently
quantized reference, with held payload never exceeding the budget. Missing and corrupt tiles
refuse requests; repaired files can be read again. Unsupported filters remain refusals. Scalar
point queries report missing data as NaN, and terrain mesh construction validates finiteness.

## Drawing and query checks

- The independent authored-reference GPU case still compares 125,337 pixels with zero differing
  color/depth pixels, and every level's rest-height hash agrees.
- The stock shared setup crosses tile and LOD boundaries in both directions, teleports to
  +4,096 m and -8,192 m, and returns to identical color/depth captures. Surface time stays fixed,
  even with a requested rate of 86,400 and a later rate change to 600. Field capacities and slot
  counts stay fixed. A missing requested far mip fails setup.
- The real engine-view executable selects `layout: tiles` at rate zero. The actual editor
  viewport follows its camera and returns to identical captures, using the same shared setup.
- The authored bowl collision test uses the provider's native source by default, rather than a
  typed accessor. Its lattice heights remain bit-identical and its character crosses a tile
  boundary within the existing collision tolerance. Collision preserves native request failures.
- The CPU water acceptance case checks the same authored bowl through `TerrainSampler`: native
  lattice heights, the wading ground view, mesh vertex depths and a quantized depth-map sample
  agree. The static source returns the same surface even at a requested time of 1e9 seconds.

Windows `msvc-debug` passed all 50 affected CTest entries. A final focused run passed all four
selected entries (water, document bridge, lint and docs), including the added authored-water
acceptance case and the last comment-only rebuilds. Linux Clang debug passed all 52 selected
entries, then three editor/document/docs entries and two water/docs entries after the final
refinements. GPU cases without an available Vulkan device skip their rendering assertions there;
Windows supplies the RTX 5090 rendering evidence. Formatting, documentation and whitespace
checks pass; this worktree's two Linux volumes were pruned. Native terrain materials and the v6
duplicate textured floor belong to W68; this branch does not move collision heights or remove
that floor. The combined branch receives the full batch gate separately.

## External v6 fixture

Artifacts live outside the engine repository under
`D:\workspace\game_engine_local\acceptance\authored-terrain-streaming-20261023\`.
The acceptance scene is produced from the project's existing v6 source with thirteen animated
wildlife instances, flora wind, water, and static terrain tiles. Its set is built for the stock
outer-ring spacing (2 m) and two far levels (8 and 16 m), rather than reusing the original v6
chain built from a 50 cm outer-ring spacing. The original project scene and inspection session
are not edited. Captures and machine-state reports remain external and are never committed.

The 120-frame, two-second game replay completed with exit code zero. Its report contains thirteen
animated animals with pose/playhead hashes, active game updates, and flora wind enabled. Terrain
rate stayed zero, all six levels evaluated zero future fields, and geometry-page `stream` stayed
false. The drawn-ground consumer refused no requests. This replay holds its camera in place;
it checks animation beside the terrain rather than camera travel.

A separate 61-frame overview camera route from (50, 42, -50) to (40, 30, -42) completed 26 terrain
layout swaps with no failed rebuilds or lag. It built/uploaded 170 chunks and dropped 144, keeping
70,696 across the swaps; current chunk count was 2,739. Terrain time remained zero with no future
field evaluations. Slot/arena device reservations were 235,155,912 bytes, separate from CPU fields,
source-cache payloads and the deformation pool. This roughly 13 m route views the lake and mountain
from the forest; the deterministic fixture above supplies the long travel, teleport, return and
finite-edge checks.

A four-second, 240-frame replay (`moving-animals/`) flies forward across the x = 32 m tile
boundary and back, with thirteen animated animals, their game updates and flora wind enabled in
the same run. It completed nine terrain swaps, built/uploaded 40 chunks and dropped 34, with no
failed rebuilds, no lag and no drawn-ground refusals. All six levels kept surface time and rate
zero with zero future field evaluations; geometry-page streaming remained false. The final
downward overview capture was inspected. Other-process CPU use was 13.715% at start and 8.681%
at end, GPU use 4% and 9%, with 7,408/7,475 MiB occupied; this run was also not quiet.

The game replay's recorded machine state was busy: other-process CPU use 10.225% at start and
7.719% at end; total GPU use 3% and 8%, with about 7.7 GiB occupied. `summary.quiet` was false.
Its captures (`forest-animals.png` and `travel.png`) were inspected. The distant duplicate-floor
overlap is still visible and remains W68's material acceptance issue. No frame-time claim is
made from these runs.
