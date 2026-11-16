# torrent

A small flow on a steep bed (roadmap W129, W130; [channels](../../../docs/subsystems/channels.md#how-whole-a-stream-is-drawn),
the grounding in [steep-streams](../../../docs/experiments/steep-streams.md)): the engine's own
ground for what the owner walked on the forest on 2026-11-15, a stream of about 20 l/s down a bed
steeper than 30 %, "incredibly disjointed ... with dry patches and wet patches".

- **The ground** is `engine-content terrain-synth --kind torrent`, 801 points a side at 0.5 m
  (400 m square, centred on the origin): a valley along +x whose floor falls 42 %, its thalweg
  swinging 10 m either way every 120 m, walls rising 30 %, and in its floor a step every 15 m whose
  drops cycle from 0.3 to 3.5 m (the treads between fall 33 %), with a scour pit at the foot of
  five steps in eight. The grid is binary and **never committed**: write it first (below).
- **The channels entry** plans the valley's network on the default 4 m lattice and names one
  spring of 21 l/s at the head of the main stem, the owner's line's flow. No loss is named, so the
  water is no part of the plan's key and `flow.stage` and `flow.sources` may change without a
  re-plan.

```powershell
$bin = 'build/msvc-release/bin'; $d = 'content/test-scenes/torrent'
& "$bin/engine-content" terrain-synth "$d/ground.json" --kind torrent --size 801 --spacing-mm 500 --origin-mm -200000,-200000
& "$bin/engine-content" channels "$d/scene.json" "$d/cuts.json" --cut
# paste cuts.json's edits into terrain.tiles.edits and its plan key into the entry's params.plan: scene-cut.json
& "$bin/engine-content" flow solve "$d/scene-cut.json" "$d/flow.json" --scene-out "$d/scene-solved.json"
```

The cook's line carries `runs_by_slope`: the count of how whole each line is drawn, by slope.
