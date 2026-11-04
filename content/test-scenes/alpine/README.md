# Alpine forms

G125's vegetative specimens at authored metre scale: moss campion cushion, encrusted
saxifrage rosettes, snowbed willow dwarf scrub, then three sparse saxifrages between
small granite boulders. The three pocket centres are authored soil locations, not
automatically detected shelter. No species is scaled to make it visible.

The [grounding](../../../docs/experiments/alpine-growth-forms.md) separates botanical
dimensions from sampling choices; [flora](../../../docs/subsystems/flora.md#alpine-mats-rosettes-and-dwarf-scrub-g125)
documents every input. All sources are text recipes and need no fetched assets.

`engine-view --scene content/test-scenes/alpine/scene.json
--camera-path content/test-scenes/alpine/camera-path.json --offscreen --frames 300
--width 1280 --height 720 --marker-captures <external-dir>` captures an overview,
close views of each habit and a distant view. Run non-test GPU work through
`tools/gpu-lock.ps1 run`; write captures/caches under `D:/workspace/game_engine_local/`.
Markers 0/60/120/180/240/299 are overview/cushion/rosettes/scrub/pockets/distance.
The fixture is for visual inspection, not a timed performance baseline.
