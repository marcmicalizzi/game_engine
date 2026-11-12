# Meadow sward (G90)

Eight source-bounded botanical recipes for the sward solver, separate from G75 tufts.
[Grounding](../../../docs/experiments/grass-sward.md) and
[contract](../../../docs/subsystems/flora.md#grass-sward-g90) state what is implemented and
what still needs acceptance. `camera-path.json` moves through 1, 5, 20 and 80 m.

`tools/sward-cook.ps1 -Out D:/workspace/game_engine_local/sward/2026-10-25/meadow` cooks a
dense tall-fescue family by default, the ground layer, density paint, heightfield and scene outside the checkout. `-Recipe <file>` selects another management state; `-Species` optionally overrides its species. No generated
PNGs, meshes, height grids or capture outputs belong here. It also writes recipes and provenance.

The fixture has a field, three existing birches at a shaded clearing edge, a slope, a wet pond margin and a trail. Its
layout is a deliberately small acceptance fixture, not a botanical/ecological simulation. Density
is authored once in the paint and read from W72's resulting palette by `sward` placement.
The script records the shade approximation and its controls. It never writes to the forest POC.
Wetness is present in the ground mask; wetness-driven blade height and colour, and slope-driven
stature, are still missing. The original v4 family had up to 20.67% card error. V5 changes shoot structure, allows leaf overhang to avoid cell seams and fits its coarse width/card cutoff at cook time.
The fixture is an acceptance candidate, not an accepted visual baseline.

Use `engine-view --offscreen` for every capture. Current-main shadow measurements must use
`--shadows csm --shadow-distance 8` and be labelled as a stand-in for the pending kind policy.
No 0/3/8 m/s or gust acceptance is claimed from main's dimensionless CPU wind-strength knob.
The measured frame-budget row belongs to the separately briefed engine integration; no invented
baseline is entered here. The owner's captures and pair/shadow budgets remain the exit gate.

## Dense cover and managed grass candidates

The v5 continuation adds `dense`, `dense-low`, `mown`, `grazed`, `trodden`, `clumps` and `tall`
recipes. Dense/tall use grouped tall-fescue leaves, not uniformly scattered isolated blades.
Mown leaves are clipped at 5 cm; grazing varies the cut height. Trodden is an authored short,
sparse state, not a simulation of a footstep. Clumps gather shoots into distinct crowns.
Sample counts, crown radii and cutting heights are authored examples, not species constants.
The high candidate has an explicit 8500 all-rung triangle cap; dense-low stays within 2400.
Neither is a claim about runtime performance or accepted visual quality.

```powershell
tools/sward-showcase.ps1 -Out D:/workspace/game_engine_local/sward/showcase -Capture
```

The showcase cooks seven 8x8 m stands, near/card scenes and matching close, walking-height and
overview captures. It uses no terrain to hide holes and disables wind/shadows for asset comparison.
Full scene integration still uses `sward-cook.ps1`. Persistent walked tracks require an optional
runtime contact/recovery field; low graphics quality must pay none of its allocation/pass costs.
The solver's roots and arc weights are ready for that consumer, but this branch does not add it.
ComfyUI PBR comparisons remain possible after silhouette/coverage is sound; every accepted leaf
appearance must also feed the card and ground bake consistently, with provenance.

## Growth forms and the meadow's forbs (G137)

Four recipes are the sward's growth forms, each from a source-bounded profile
([grounding](../../../docs/experiments/grass-sward.md#growth-forms-the-stature-rule-and-the-meadows-forbs-g137-2026-11-08),
[contract](../../../docs/subsystems/flora.md#growth-forms-by-site-g137)):

| Recipe | Form | Species | Leaves | Layout |
|---|---|---|---|---|
| `tall-sod` | tall sod | red fescue | 22–40 cm × 0.5–1 mm | 768 blades/m², dispersed shoots, 6,400-triangle cap |
| `tall-tussock` | tall tussock | tufted hair-grass | 30–60 cm × 2–5 mm | crowns 0.5 m apart, 0.12 m radius, 5 cm raised base, 30 % short turf between; 5,300 cap |
| `short-sod` | short sod | bent | 4–8.5 cm × 1–4 mm | 1,024 blades/m², two-segment ribbons; 6,400 cap |
| `short-tussock` | short tussock | mat-grass | 12–27 cm × 0.5–1 mm | crowns 0.25 m apart, 0.05 m radius, 35 % turf between; 4,200 cap |

`oxeye-daisy.flora.json` is one of the meadow's forbs: the understory's herb at 0.5 m (*Leucanthemum
vulgare*'s stems are 20–90 cm across the floras). A sward placement entry names a form per family
(`"form": "tall_tussock"`) and, since G140, the meadow's community, a species of the table and its mesh
each (`"community": [{"species": "leucanthemum_vulgare", "mesh": "oxeye-daisy.clusters"}, …]`;
G137's `"forbs"` is refused): each cell then draws a plant by the covers its site gives the species
([flora](../../../docs/subsystems/flora.md#the-meadow-community-g140)), and the stature rule picks its
form from the same site's wetness, its slope, shade and a 20 m mosaic.
The crown spacings, radii and raised base are authored, not measured; the caps are explicit
candidate budgets, not a claim about frame cost.

## The community on a valley floor (G166)

`community/scene.json` is the first scene that draws the meadow's community
([flora](../../../docs/subsystems/flora.md#the-meadow-community-g140)): one sward entry naming the
four growth forms (two density classes of two variants each) and all thirteen species as their
stand-ins, in a 512 m bowl (`terrain-synth --kind basin` at a vertical scale of 2.5: a level floor
for about 80 m round a 30 m pond, flanks steepening to 17°), so the pond's wet margin, the level
floor and the dry flank each draw what the site gives them. Like `scene.json` it is a cook
template: its paths resolve in a directory outside the checkout, filled by these lines.

```powershell
$out = 'D:/workspace/game_engine_local/meadow-community'; $bin = 'build/msvc-debug/bin'
foreach ($form in 'tall-sod','tall-tussock','short-sod','short-tussock') { foreach ($d in 50,100) { foreach ($v in 0,1) {
  $dir = "$out/$form/d$d-v$v"; New-Item -ItemType Directory -Force $dir | Out-Null
  $p = Get-Content "content/test-scenes/meadow/$form.flora.json" -Raw | ConvertFrom-Json -AsHashtable
  $p.sward.seed = 8801; $p.sward.variant = $v; $p.sward.density = $(if ($p.sward.density) { $p.sward.density } else { 1 }) * $d / 100
  $p | ConvertTo-Json -Depth 10 | Set-Content "$dir/patch.flora.json"
  & "$bin/engine-content" flora "$dir/patch.flora.json" "$dir/patch.clusters" --no-materials --sward-assets $dir } } }
& "$bin/engine-content" terrain-synth "$out/ground.json" --kind basin --size 513 --spacing-mm 1000 --origin-mm -256000,-256000 --seed 0
Copy-Item content/test-scenes/meadow/community/*.json $out
& "$bin/engine-view" --scene "$out/scene.json" --camera-path "$out/camera-path.json" --offscreen --width 1920 --height 1080 --marker-captures "$out/captures"
```

The sward layer's `far_color` is the short sod's bake's (`short-sod/d100-v0/provenance.json`,
`far_color_linear`), copied in by hand. The camera holds four views two seconds each, so the inner
ring's blades have faded in before each marker: `above-50m`, `wet-margin`, `dry-slope`,
`along-margin`. The pond stands in for a stream's margin (the niche reads both alike); the
pictures and what still reads wrong beside reels r01 and r04 are roadmap G166's.
