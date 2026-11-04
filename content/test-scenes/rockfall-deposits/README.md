# Rockfall and sediment deposits

G128's opt-in [rocks](../../../docs/subsystems/rocks.md#terrain-aware-deposits-g128) deposition
study. A narrow analytic ridge supplies talus; a shallow hollow collects channel sediment. The 0.5 m sample grid is explicit; coarser sampling makes the channel boundary more visibly stepped.
The cliff faces have their legacy skirts disabled; the deposit explicitly ignores those source cliffs as exclusion footprints. No generated assets are committed or downloaded. The `deposits` entry writes sampled patch
recipes into the selected derived-data cache on load. Geometry is static, so set time rate zero.

Use a worktree's own engine-view, offscreen, under the GPU lock:

```powershell
tools/gpu-lock.ps1 run -Purpose "G128 deposit study" -Exec "build/msvc-debug/bin/engine-view.exe --scene content/test-scenes/rockfall-deposits/scene.json --ddc D:/workspace/game_engine_local/rockfall/2026-11-04/review-ddc --offscreen --width 1280 --height 720 --camera-path content/test-scenes/rockfall-deposits/camera-path.json --frames 241 --marker-captures D:/workspace/game_engine_local/rockfall/2026-11-04/review --shadows csm --time-rate 0 --sun -30,45 --sun-rate 0"
```

Keep the scene, camera path and command beside captures outside the repository. Compare the
same scene with only the deposits entry removed, retaining the source cliffs. The marker names are
approximate approach distances, not an exact rangefinder measurement. The far representation
is the usual cluster DAG, not an impostor switch. A suitable stone/gravel PBR pack improves the
fine sediment; without one the patch uses the lithology's flat material.

The finite window is the bake's catchment boundary. Expand it explicitly when upstream supply
or drainage outside the example must contribute. Do not enable a second overlapping legacy
cliff skirt on the same foot; use `Cliff.talus: 0` when composing a cliff face with this deposit.
