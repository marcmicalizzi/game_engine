# village

**A small row-plan village with interiors, on an authored ground** (roadmap G69, S89;
[village](../../../docs/subsystems/village.md), plan
[14](../../../docs/plan/14-structures.md), the plan's numbers in
[the grounding](../../../docs/experiments/village-plan-grounding.md)): the scene that goes in at a
door, through the rooms, up the stair and to a window.

## What is in it

- **The ground**: a 256 m heightfield at 25 cm (`engine-content terrain-synth --kind ridge`): a
  40 m ridge along a diagonal and, east of it, ground rising 6 m from south to north, where the
  village stands. A tile set (`provider: "tiles"`), because the village's pads, its tread and its
  paths are ground edits and a planned edit is written into a tile set's heights.
- **The village** (`placements[0]`, generator `village`): one street of three waypoints, 129 m,
  two rows, eight houses (a village by the grounding's threshold of six), plots 5 perches deep so
  the east row stays on the heightfield. Each house is the rural filler's, on a pad, facing the
  lane, with a path from its door to the tread.
- **The cover**, after the village so it keeps off the pads, the tread and the paths: grass tufts,
  nettles and daisies (`village-cover`), and apple trees and hazels (`village-trees`), from the
  recipes in `content/flora/`.
- **The wear and the walls**, pasted from `engine-content village`'s output (a planned mask region
  and another generator's entry are not handed across yet): the lane's verge, worn margin and stony
  tread and each house's door yard as mask regions over three palette layers (`verge`, `worn`,
  `lane`), and each plot's boundary as a dry-stone wall 1.1 m high with a gateway where its path
  crosses its front (`placements[1]`, generator `walls`). `plan.json` holds both as the tool wrote
  them; **after a change to the plan, run the tool and paste again**.
- A sky at 52° north in midsummer, **at three in the afternoon** (the terrain's `time`, 54,000 s
  past the epoch's midnight: the scene opens in daylight with no flag); no water, no fires, no `world` block (the scene is read whole).

## Write the heightfield first

The grid is binary and never committed (`.gitignore`):

```powershell
build/msvc-release/bin/engine-content matgen     # once in a fresh checkout: the generated material sets
build/msvc-release/bin/engine-content terrain-synth content/test-scenes/village/ground.json `
  --kind ridge --size 1025 --spacing-mm 250 --origin-mm -128000,-128000
build/msvc-release/bin/engine-content village content/test-scenes/village/scene.json `
  content/test-scenes/village/plan.json --flight content/test-scenes/village/camera-path.json
build/msvc-release/bin/engine-view --scene content/test-scenes/village/scene.json `
  --camera-path content/test-scenes/village/camera-path.json --offscreen --terrain-tiles `
  --shadows rt --tunable renderer.gi.mode=bounce --benchmark village.jsonl
```

`engine-content village` prints the plan (`plan.json`: every plot, its state, its house and its
door), the tread's mask region, and writes `camera-path.json` — the committed one is its output for
house 0, so a change to the plan or to the house filler that moves the door is seen as a diff of
that file.

## The flight

`camera-path.json`: along the lane at eye height (1.65 m), up the path, in at the door of house 0,
through its rooms' doorways, up its stair and to an upper window, where it stays three seconds
looking out. A marker at every key names what the point is (`lane`, `path`, `door`, `inside`,
`window`), so `--marker-captures <dir>` writes a picture at each.
