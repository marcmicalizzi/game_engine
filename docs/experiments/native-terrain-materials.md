# Native terrain materials

Status, 2026-10-23: implemented and validated (W68, implementation `0d4a16c6`). The forest/lake v6 fixture exposed two independently simplified ground surfaces intersecting: the authored heightfield and an imported textured floor 25 mm above it. The material now belongs on the native surface.

## Grounding

Ryan Geiss, [GPU Gems 3, chapter 1, section 1.5](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu), explains why a single planar projection stretches on steep faces and blends three axis projections by the surface normal. Each projection needs its own tangent basis. This work uses that structure, with projected normal-map slopes blended in the surface tangent plane; a neutral map must preserve the input normal. It does not prescribe geology or ecological altitude bands.

## Contract

A terrain may name authored material layers: colour, OpenGL (+Y) tangent-space normal, linear roughness, and an optional linear coverage mask over the terrain's existing map frame. Layers composite in order; masks and authored weights control coverage. Map projection carries existing finite landscape paintings, while triplanar projection repeats detail at a stated size in metres. Detail fades toward the mip-chain average at authored near/far distances; geometry and collision do not move. Tiles and rings use the same material table and world anchoring, and the water's analytic bottom evaluates the same layers.

The scene chooses a layer budget; validation refuses excess layers instead of silently dropping them. The GPU material record stays 112 bytes: optional layers occupy additional entries only on terrains that name them. Separate shader modules keep the layer evaluator out of scenes that do not use it. This is native surface shading, not per-tile material-page streaming (the remainder of W25), mesh/ground contact blending (R90), or underwater object rendering.

## Cost and validation

Expected work is proportional to covered terrain pixels and the layers the scene enables, independent of source terrain extent. Triplanar detail can read three projections per map; masks are one filtered map-frame read. Images use the existing texture build, mip and sharing path. This implementation loads the material images through that existing path; bounded material-page residency remains W25.

The v6 comparison uses the same one-surface scene, camera, geometry cut and water, with three
native layers enabled or the legacy palette alone. It is not a comparison against the old
duplicate floor. Windows `msvc-debug`, i9-10980XE, RTX 5090, NVIDIA 617.42, 1600 x 900, RT raster
and shadows, world time stopped: 60 measured frames after 30 warm-up frames, GPU lock held and
`--wait-quiet 30`. The first pair reports `quiet: true`: other CPU 7.45% -> 7.47%, GPU utilization
13% -> 10% for native; other CPU 7.32% -> 5.16%, GPU 5% -> 7% for the palette control. These
samples describe this workstation and fixture, not the baseline-tier GPU budget.

| First pair | Native layers | Palette control |
|---|---:|---:|
| Resolve GPU median | 0.400880 ms | 0.168016 ms |
| Total GPU median | 2.219120 ms | 1.934208 ms |
| Frame median | 22.677250 ms | 21.999150 ms |
| Resident texture bytes | 133,561,856 | 81,807,360 |

The measured resolve increment is 0.232864 ms; the material images add 51,754,496 bytes
(49.36 MiB, eleven built textures). Three child records add 336 bytes to the 112-byte material
table. Both runs use 497,152,880 RT bytes, report zero RT overflow frames or dropped instances,
and record zero steady-state frame allocations and device-buffer creations. The frame medians
include CPU scene work and are not a measure of material shader cost.

A second pair gives resolve medians 0.400592 / 0.168224 ms (native/control), within 0.13% of
the first pair. Its native run exhausted the quiet wait and reports `quiet: false`: other CPU
14.78% -> 15.10%, GPU 6% -> 16%, lock ours. Treat that run as an upper bound, not a second quiet
sample. The control reports `quiet: true`, other CPU 9.50% -> 6.30%, GPU 5% -> 5%, lock ours.
The same texture/RT sizes and zero overflow/allocation results hold in both repeats. Full
machine states and raw figures are in `cost-results.json`; the first-pair files have the `-r1`
directory suffix so the repeat does not overwrite the evidence.

## v6 surface acceptance

The external fixture removes exactly 61 duplicate floor/land meshes, with no replacement mesh,
vertical offset or collision change. Before/after `forest.f32` SHA-256 is
`60A35610D1006F7A887843EE53E07BCAD54D296960E855676927C13635E29911`. Original v6 content and the
owner's live scene remain intact. The prepared native copy reuses the existing v6 mask
composition and source scan sizes: leaves 2.3 m, mud 1.3 m and rock 1.8 m. Its 32-128 m detail
fade is an authored quality/cost setting, not a physical landscape constant; distant rock
becomes its linear mean while coverage painting remains. No new geology or ecology is claimed.

Inspected captures under `D:/workspace/game_engine_local/native-terrain-w68/`:

- `native-lake-south/view.png`: one continuous near shore and the same material beneath the
  analytic water; no intersecting floor patch.
- `native-lake-west/view.png`: the side shore, forest edge and lake remain continuous.
- `native-forest-return/view.png`: detailed leaves follow the native sloped forest floor.
- `native-slope-close/view.png`: rock detail on an approximately 45.46-degree source slope,
  viewed about 13.5 m away, exercises the close triplanar path instead of the far mean.
- `reference.png`: a separate four-sample, one-bounce native green-material smoke render
  contains 4,449 green pixels; the reference shader variant is exercised, not only compiled.

`prepare-v6.ps1`, `capture.ps1`, `final-captures.ps1`, `collect-results.ps1`, `fixture.json`,
camera paths, raw frame JSONL and full reports are retained alongside those images. The stock
fixture on this branch still uses the static grid route; W67 owns the streamed static source
integration. Continuity through three tiled levels and two cuts is checked independently by
the GPU coordinate-painting test below; the two branches' combined merge gate belongs to their
coordinator.

## Code checks

- Windows `msvc-debug`: the affected build passes. The affected run passed 48 of 49 CTest entries;
  its GPU entry caught the negative-coordinate defect below. After repair the full GPU executable
  passes all 99 enabled cases, 416,938 assertions (two cases disabled by default).
- Final native renderer checks: five cases, 9,279 assertions, all passing. They cover input
  validation, image-sensitive cache identity, unchanged heights, scalar roughness R
  packed into G with no metal, ordered coverage, cache/decode depth equality, and a painted
  coordinate field across three terrain tile levels, two camera positions and two geometry cuts.
- Stock engine-view material case: five assertions; document heightfield cases, including material
  edit/rebuild: 103 assertions. Both pass. The affected run's stock viewer, editor, host and frame
  allocation checks pass too.
- Linux Clang debug: all 49 affected CTest entries pass; a fresh sync of the final refinements
  passes all seven selected entries (GPU module, renderer, document bridge, viewer, lint and docs).
  The container has no supported GPU; the actual GPU checks above ran on the RTX 5090.
- Freshly compiled `main` and this branch produce byte-identical `visibility_resolve`, its ray,
  water and ray/water modules, and `path_trace`, using the same Slang compiler and flags. The five
  optional native modules total 1,636,604 bytes of SPIR-V and are selected only when a native root
  exists in the uploaded material table. The hot material record remains 112 bytes.

The coordinate painting uses a smooth 256-texel ramp and checks the decoded/filtered colour in
the display encoding within two codes. A two-texel black/white ramp was an unsuitable oracle:
finite texture-filter weights become large display-code differences near black after encoding.

External fixtures and raw logs: `D:/workspace/game_engine_local/native-terrain-w68/`; build/test
logs: the isolated worktree's `build/native-*.log`. The original v6 scene is retained as the
regression fixture; the acceptance copy removes its 61 duplicate floor/land meshes and references
the same unmodified heightfield. It reuses the existing v6 composition masks and recorded scan
dimensions; it is not a new geological or ecological model.

## Negative world coordinates

The first GPU phase probe (Slang 2026.17.1, NVIDIA RTX 5090, driver 617.42, Windows debug build)
failed all 32 negative-X cases and none of the positive ones: at a 1.5 m repeat the phase error
was 0.410667 cycles, the residue of 2^64 modulo 1,500 divided by 1,500. This is an observed
compiler/device combination, not an attribution to either alone. Addressing now takes an
unsigned magnitude's remainder and applies its sign explicitly. The probe compares against
double-precision world coordinates, including negative cells and points 100,000 km out, and
checks projected slopes and preservation of a flat normal map.

## Combined static streaming gate (2026-10-23)

The combined W67/W68 gate at `4b31ac77` used an external copy of the same native v6 fixture with the 61 duplicate floor/land meshes removed. The original heightfield hash above, collision data and authored transforms were retained. Native materials remained enabled with `--terrain-tiles --terrain-far 2 --time-rate 0`; the ordinary geometry-page streaming switch stayed off. The south/west shore, forest return and close steep-slope captures were inspected. The native surface and analytic water bottom remain continuous, and none of the four traced captures reports an RT overflow or a dropped instance/caster.

A 240-frame replay advances all thirteen wildlife poses at full animation detail (`--anim-lod off`) with flora wind, while the terrain remains at time/rate zero. It records nine tile swaps, 40 chunks built and 34 dropped, no failed rebuild, no layout lag, no refused game ground/move calls, and no frame-thread allocation or device buffer. A separate 61-frame camera travel records 26 swaps, 170 chunks built and 144 dropped, also without a failed rebuild. All six levels evaluate zero future fields in both runs. These are functional checks on a shared RTX 5090 workstation; the raw reports preserve machine state and are not new frame-cost measurements. The five existing frame-budget runs pass without a rebaseline.

Evidence, commands, scene/cache provenance, frames, reports and inspected PNGs are under `D:/workspace/game_engine_local/acceptance/terrain-batch-20261023/`. The stock document-host follow-up and complete gate results are recorded in [static authored streaming](authored-terrain-streaming.md#combined-gate-and-document-host-follow-up).
