# static-world

A world of **imported assets placed by hand** (roadmap T130): the scene a consumer of the engine
who uses none of its solvers has. Every flown scene before it was a generator's, and the features
built for the forest were only ever looked at on the forest; this one is flown at every gate
(`content/budgets/frame-budgets.json`, run `static-1080p-rt`) and end to end through the four
rasterizers in every configuration, `msvc-minimal` included
(`apps/engine_view/tests/static_world_view_tests.cpp`). What it gets from each feature built for a
generated world, what that costs it, and what would make the feature work for it is the table in
[the static-asset world](../../../docs/experiments/static-asset-world.md).

**What is in it**, all of it in `scene.json` and nothing else:

- a 600 m ground **mesh** (`kit/ground.gltf`, 161 x 161 samples, 0.94 m apart at the middle and
  wider outwards, a 256-texel texture repeating every 4 m): no `terrain` block, no ground provider,
  no tiles, no rings;
- seven trees, ten shrubs and forty grass clumps of **branch cards**, as a bought tree has them: the
  tree is a trunk, six limbs and 75 alpha-tested, two-sided cards of 1.5 m by 1.1 m, three crossed
  planes at each of 25 points, cut by a twig of 18 leaves in the card's alpha (about a third of the
  card); the shrub is 24 such cards; the grass three crossed cards of blades with normals up;
- six boulders (`kit/rock.gltf`, a noise-pushed sphere of 2,304 triangles);
- two Khronos Lanterns (fit to 2.4 m) and the FlightHelmet (fit to 0.45 m) on the first boulder,
  from `tools/fetch-samples.ps1`'s pinned set;
- no `scatters`, no `placements`, no `world`, no `sky` (the stand-in sun), no water, no fires.

Every instance stands at a height written in the file (`ground: false`), read off the kit's ground
function, `h(x, z) = 0.015 x - 0.010 z + sum a (1 - d2/r2)^2` over six bumps
(`apps/engine_content/static_kit_commands.cpp`, `static_kit_ground_height`), as an author's tool
would have written it.

**The kit is written by a command, not committed** (`.gitignore` keeps `kit/` out): plain glTF 2.0
(`.gltf` + `.bin`) and PNGs, carrying nothing a generator adds — no generator section, no motion
table, no structural table, no seat, no impostor chain, no coverage errors
(`apps/engine_content/tests/static_kit_tests.cpp` builds each mesh and holds that). The pinned
samples have no foliage, so nothing alpha-tested could come from them; the kit's cards are made the
way a marketplace tree's are. Candidates for real bought assets, permissively licensed, for the
owner to approve and fetch (nothing was downloaded): the static-asset page lists them.

```powershell
pwsh tools/fetch-samples.ps1                                    # once: the lanterns and the helmet
build/msvc-release/bin/engine-content static-kit content/test-scenes/static-world/kit
build/msvc-release/bin/engine-view --scene content/test-scenes/static-world/scene.json --camera-path content/test-scenes/static-world/camera-path.json --offscreen --raster rt --shadows rt --benchmark static.jsonl
```

`camera-path.json` is 24 seconds: a half circle round the grove 30 to 40 m out and 7 to 9 m up,
looking at its middle, then down to eye height among the trees and past the helmet's boulder,
ending at the first lantern.
