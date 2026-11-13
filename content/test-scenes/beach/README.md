# beach

A bay to look at a wave on, from deep water to the top of the sand (roadmap S102; the audit, the models and the design are [shore mechanics](../../../docs/experiments/shore-mechanics.md), the code [water](../../../docs/subsystems/water.md)). Text only: the engine's own dunes ground with two basins and three ridges, a scatter of boulders, one open water body.

**The ground.** The sea is a basin 380 m in radius and 14 m deep centred past the east edge, so its shore is an arc that faces east in the middle of the scene; with the level at −1 m the long beach's slope at the waterline is about **1:30** (1:29.6 at z = 0, 1:31.8 at z = 120), and the sand's own low swells (half a metre over 60 m) vary it along the beach and put shallow bars off it. A second basin 46 m across at (52, −168) steepens the shore south of the point into a **pocket of about 1:8 to 1:11**. A ridge from (−45, −108) to (46, −118) is the **rocky point**, standing about 55 m into the surf and 3.5 m over the water at its tip; a short ridge at (64, −121) is the **stack** off it; a third ridge closes the pocket to the south. Sixteen or so **boulders** of 0.5 to 1.4 m lie in the swash of the long beach between z = 4 and 46.

**The sea.** Open, its outline along the coast (a polygon, so the mesh grows its rim over the bank). A wind sea alone: 8 m/s over 60 km, which by the fetch laws is about 1.1 m at 4.7 s, travelling at 205° (towards −x and −z): **25° off the long beach's normal**, so the surf arrives at the north end first and the point shelters the pocket's north side. To try a swell, add to the body for example `"swell_height_m": 0.9, "swell_period_s": 10, "swell_direction_deg": 190`.

**The cameras** (`camera-path.json`): four held poses of three seconds, each cut as the owner's reference reels are, sixteen frames a fifteenth of a second apart (`<name>-f00` … `-f15`) and then eight a quarter of a second apart (`<name>-s00` … `-s07`):

| Name | Where | What to look at |
|---|---|---|
| `along` | 0.7 m over the sand at (−31, 40), looking south along the beach to the point | The sheets on the sand, the boulders in them, the surf arriving along the beach |
| `seaward` | 1.6 m over the sand at (−33, −5), looking out to sea | One wave from offshore through its break to the top of its sheet |
| `point` | on the point at (30, −116), looking back along the surf line | The bores as walls of water, the swash along the long beach |
| `above` | 30 m up at (−45, 35), looking south-east | The crests turning to the beach, the break line, the point, the stack and the pocket |

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/beach/scene.json --camera-path content/test-scenes/beach/camera-path.json --offscreen --width 1920 --height 1080 --raster rt --shadows rt --time-of-day 15 --time-rate 0 --world-rate 1 --no-lights --marker-captures <dir>
```

under `tools/gpu-lock.ps1 run` when it is not a test. Run `engine-content matgen` once in a fresh worktree first, or the boulders draw with constants.
