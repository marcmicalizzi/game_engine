# village

**A small village that grew, with interiors, on an authored ground** (roadmap G69, G206, S89;
[village](../../../docs/subsystems/village.md), plan
[14](../../../docs/plan/14-structures.md), the plan's numbers in
[a village that grew](../../../docs/experiments/village-growth-grounding.md) and
[the row plan's grounding](../../../docs/experiments/village-plan-grounding.md)): the scene that
goes in at a door, through the rooms, up the stair and to a window.

## What is in it

- **The ground**: a 512 m heightfield at 25 cm (`engine-content terrain-synth --kind ridge`): a
  40 m ridge along a diagonal and, east of it, ground rising 6 m from south to north, where the
  village stands. Its centre is 51 m west of the scene's origin, so the ridge's foot lies under
  the village where the 256 m ground of the first two legs put it, and there are 78 m of ground
  east of the last croft and over 100 m north and south of the ways' ends: no view of the village
  shows the bare ground beyond the heightfield (2026-11-16; a quarter of the top-down picture had). A tile set (`provider: "tiles"`), because the village's pads, its treads and its
  paths are ground edits and a planned edit is written into a tile set's heights.
- **The village** (`placements[0]`, generator `village`): two ways, an old road along the ridge's
  foot from south to north and a way from the west over the ridge's shoulder that meets it, each
  found over the ground between its places; a green where they meet with the well at its middle;
  fourteen houses taken up nearest the green first, tofts side by side round it (seed 12, a 36 m
  green, crofts of six perches), subdivided and at uneven setbacks, cottages of one storey in the
  gaps and on the green, a plot kept for the church past the houses' end, and back lanes behind
  the built rows. Each house is the rural filler's, on a pad, facing its way, with a path from its
  door through a gate to the tread. Plots a corner of which would stand off the heightfield, or on
  the ridge's slope, are left empty and counted.
- **The cover**, after the village so it keeps off the pads, the treads, the paths and the well:
  grass tufts, nettles and daisies (`village-cover`), and apple trees and hazels
  (`village-trees`), from the recipes in `content/flora/`.
- **The wear and the walls**, pasted from `engine-content village`'s output (a planned mask region
  and another generator's entry are not handed across yet): the ways' verges (which follow the
  street's wandering edge and open into the green), worn margins and stony treads, the back lanes'
  worn tracks, the well's trodden ground and each house's door yard and path, as mask regions over
  three palette layers (`verge`, `worn`, `lane`), and each built plot's boundary as a dry-stone wall
  1.1 m high with a gateway at its gate (`placements[1]`, generator `walls`). **After a change to
  the plan, run the tool and paste again** (the commands below).
- A sky at 52° north in midsummer, **at three in the afternoon** (the terrain's `time`, 54,000 s
  past the epoch's midnight: the scene opens in daylight with no flag); no water, no fires, no
  `world` block (the scene is read whole).

## Write the heightfield first

The grid is binary and never committed (`.gitignore`):

```powershell
build/msvc-release/bin/engine-content matgen     # once in a fresh checkout: the generated material sets
build/msvc-release/bin/engine-content terrain-synth content/test-scenes/village/ground.json `
  --kind ridge --size 2049 --spacing-mm 250 --origin-mm -307000,-256000
build/msvc-release/bin/engine-content village content/test-scenes/village/scene.json `
  content/test-scenes/village/plan.json --flight content/test-scenes/village/camera-path.json
build/msvc-release/bin/engine-view --scene content/test-scenes/village/scene.json `
  --camera-path content/test-scenes/village/camera-path.json --offscreen --terrain-tiles `
  --shadows rt --tunable renderer.gi.mode=bounce --benchmark village.jsonl
```

`engine-content village` prints the plan (`plan.json`: every plot in the order it was taken up, its
kind, state, house and door, the ways, the focus and the well), the mask regions and the walls, and
writes `camera-path.json` — the committed one is its output for house 0, so a change to the plan or
to the house filler that moves the door is seen as a diff of that file. To paste the regions and
the walls, replace `terrain.masks.regions` after the first (`meadow`) with `plan.json`'s
`mask_regions` and `placements[1]` with its `walls`.

## The flight

`camera-path.json`: along the way at eye height (1.65 m), up the path, through the gate, in at the
door of house 0 (the first tenement taken, on the green), through its rooms' doorways, up its stair
and to an upper window, where it stays three seconds looking out. A marker at every key names what
the point is (`lane`, `path`, `gate`, `door`, `inside`, `window`), so `--marker-captures <dir>`
writes a picture at each.

Two shorter paths are the interior the bounce's change between frames is measured in (roadmap
R171; renderer.md, "The change between frames, as the eye sees it"): `interior-held.json` stands
twelve seconds just inside house 0's door looking across its front room (the flight's key at
28.559 s), and `interior-door.json` walks the flight's last metres to that point through the door
and holds there six seconds. Both are written by hand from `camera-path.json`'s keys, so a change
to the plan that moves the door moves them too: read the flight's diff and move them with it.