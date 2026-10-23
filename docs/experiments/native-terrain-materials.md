# Native terrain materials

Status, 2026-10-23: code validated; v6 acceptance captures in progress (W68). The forest/lake v6 fixture exposes two independently simplified ground surfaces intersecting: the authored heightfield and an imported textured floor 25 mm above it. The material belongs on the native surface.

## Grounding

Ryan Geiss, [GPU Gems 3, chapter 1, section 1.5](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu), explains why a single planar projection stretches on steep faces and blends three axis projections by the surface normal. Each projection needs its own tangent basis. This work uses that structure, with projected normal-map slopes blended in the surface tangent plane; a neutral map must preserve the input normal. It does not prescribe geology or ecological altitude bands.

## Contract

A terrain may name authored material layers: colour, OpenGL (+Y) tangent-space normal, linear roughness, and an optional linear coverage mask over the terrain's existing map frame. Layers composite in order; masks and authored weights control coverage. Map projection carries existing finite landscape paintings, while triplanar projection repeats detail at a stated size in metres. Detail fades toward the mip-chain average at authored near/far distances; geometry and collision do not move. Tiles and rings use the same material table and world anchoring, and the water's analytic bottom evaluates the same layers.

The scene chooses a layer budget; validation refuses excess layers instead of silently dropping them. The GPU material record stays 112 bytes: optional layers occupy additional entries only on terrains that name them. Separate shader modules keep the layer evaluator out of scenes that do not use it. This is native surface shading, not per-tile material-page streaming (the remainder of W25), mesh/ground contact blending (R90), or underwater object rendering.

## Cost and validation

Expected work is proportional to covered terrain pixels and the layers the scene enables, independent of source terrain extent. Triplanar detail can read three projections per map; masks are one filtered map-frame read. Images use the existing texture build, mip and sharing path. Measurements and the deterministic CPU/GPU and stock-host checks are recorded here when run.

Machine state: no timing measurement yet.

## Code checks

- Windows `msvc-debug`: the affected build passes. The affected run passed 48 of 49 CTest entries;
  its GPU entry caught the negative-coordinate defect below. After repair the full GPU executable
  passes all 99 enabled cases, 416,938 assertions (two cases disabled by default).
- Final native renderer checks: five cases, 9,279 assertions, all passing. They cover input
  validation, image-sensitive/relocatable cache identity, unchanged heights, scalar roughness R
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
