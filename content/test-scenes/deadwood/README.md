# Deadwood gallery

Nine [recipe fixtures](../../deadwood/README.md) on flat ground: fresh oak, bare oak, snag,
snapped trunk, hollow stump, hollow log, collapsed log, detached limb and fresh spruce.
Rows run from z=-9 to z=9; columns from x=-8 to x=8. All trees retain real metre dimensions.

Run your own checkout's `engine-view --offscreen --scene content/test-scenes/deadwood/scene.json
--camera-path content/test-scenes/deadwood/camera-path.json
--frames 4 --capture <local-output>.png`. The fixed camera sees the entire gallery. Generated
captures, measurements, mesh containers and their camera/recipe copies live under
`D:/workspace/game_engine_local/deadwood/2026-10-29/`, never here. See
[grounding and validation](../../../docs/experiments/deadwood.md).

## Tree-to-tree lean

`lean.scene.json` pairs `leaning-oak.deadwood.json` with `support-oak.flora.json`.
The damaged 8 m oak is pitched 55 degrees, retains 40% of its branches and no leaves;
its upper bole rests against the visible 12 m oak's trunk at approximately
(0.444, 3.459, 4.619) m. The standing oak is translated by (0.425315, 0, 5.114494) m.
The support capsule is in the leaning asset's site frame, radius .294731 m, extending
from y=0 to y=6. It approximates the trunk at contact, not its whole taper and branches.
The initial G124 prototype refit the placement and capsule to branch-aware taper (about 2 mm surface
clearance at contact), keeping the 1 cm contact tolerance and the same damaged-tree parameters. G133 refit it to the final G129 mesh, where the old fit left a 50 mm gap and the solve refused the pose: 3.0 mm from the trunk's actual section at contact, the same tolerance and parameters ([how](../../../docs/experiments/tree-branch-taper.md#baselines-recaptured-g133-2026-11-05)).
The contact test also checks the visible trunk's horizontal sections at every leaning
wood vertex and edge midpoint. Moving the capsule away must refuse `require_support`.

Use `--scene content/test-scenes/deadwood/lean.scene.json --camera-path
content/test-scenes/deadwood/lean.camera.json` with the same offscreen capture command.
The pose is already baked: do not rotate the instance again. Support discovery, equilibrium,
and joining a root plate are separate follow-ups; root plates belong to soil.
External surface/lean captures and exact textured recipe copies are under
`D:/workspace/game_engine_local/deadwood/2026-10-29-followup/`.
