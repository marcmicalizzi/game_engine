# Cascaded shadow maps: the sun's shadow without ray queries

- **Question ([04 §4.4](../plan/04-renderer.md#44-ray-tracing), the 2026-09-23 direction note "shadows without ray queries"):** the baseline tier has no ray queries, so it had no shadows. Can the sun's shadow come from depth maps drawn by the same cull pass and the same rasterizers as the picture, agree with the ray-traced shadow wherever both are defined, and cost what a baseline-tier frame can pay? And three choices the note left open: whose LOD a cascade draws, which bias, and which filter.
- **Date:** 2026-09-24. **Machines:** the development desktop — Intel i9-10980XE (18 cores / 36 threads), 64 GB, Windows 11 Pro, **RTX 5090** (32 GB, driver 610.88, Vulkan 1.4.341), `msvc-release` (`msvc-debug` for the fixture cases); and the headless server — Intel Xeon E5-2670 (Sandy Bridge-EP, 8 cores / 16 threads, no AVX2), 31 GB, Gentoo, **TITAN Xp** (Pascal, 12 GB, driver 580.178.04, Vulkan 1.4.312), `linux-server` (GCC 14.3.1, x86-64-v2) through `tools/remote-build.ps1`. **Build:** main at `12fda09` ("visibility: the id is the scene's pair, and a tie goes to the larger") plus this change.
- **Machine state:** see each section. Every RTX 5090 number was taken under the machine-wide GPU lock (`agent-csm`), one lock acquisition per group, the lock released between groups and 60 s left before the next was asked for (GPU-LOCK.md rule 9); other agents were compiling and testing on the same CPU throughout, and their GPU work waited for the lock. Every TITAN Xp group started only once the 1-minute load average was under 1.0 and the GPU under 5% busy.
- **Decision:** the design in [renderer](../subsystems/renderer.md#cascaded-shadow-maps) and [gfx](../subsystems/gfx.md): cascades drawn from **the camera's cut**, a per-texel **receiver-plane bias** plus half a texel, a **normal offset** of three texels at grazing, a **3×3 bilinear PCF**. `auto` draws maps wherever ray queries are absent and keeps rays where they are present; whether the RTX 5090 class should default to maps is left to the owner with the flythrough rows below as the evidence. Status note in [04 §4.4](../plan/04-renderer.md#44-ray-tracing).

## Setup

**Correctness, on fixtures** (`systems/renderer/tests/shadow_map_tests.cpp`, `domain/gfx/tests/shading_tests.cpp`; [renderer](../subsystems/renderer.md#testing)). The casters' fixture — a card, a slab, a cube and an L over a ground, from 794824d — is drawn with maps and compared per pixel against a **CPU ray** (Möller-Trumbore in double precision against the caster's triangles), through the mesh path and the vertex path; a card is moved outside the camera's frustum to see it cast; the cascades' frustum test is turned off to see it change nothing; and the maps are compared against the ray-traced shadow of the same frame. gfx's case puts a quad and an occluder through the resolve with the occluder only in the map, against a CPU reference with the sun removed.

**Agreement and the choices, on the Khronos samples** (the skipped case `renderer: cascaded shadow maps on the Khronos samples`, RTX 5090, `msvc-release`). FlightHelmet, SciFiHelmet, BoomBox, Lantern, Corset, Avocado and Suzanne, each standing on a flat ground six sample radii wide, drawn at 1280×720 from sixteen cameras — eight yaws at engine-view's orbit pitch and eight low over the horizon — in the shadow view, once with rays and once with maps. A pixel is **interior** when the rays give it and every pixel within three of it the same answer; there the maps must give that answer too. An interior pixel the rays light and the maps black out is **acne**, the reverse is a **leak**, and a partial answer is **grey**. The same frames are drawn with the normal offset at 0, 2, 3 and 4 texels (`ENGINE_SHADOW_NORMAL_OFFSET`), and with the cascades' LOD threshold at 1, 2 and 4 times the picture's (`RenderSettings::shadow_lod_scale`). Then each configuration is timed at engine-view's own camera: 60 frames of warm-up and 240 timed, off, rays, maps at each LOD, forwards and then backwards.

```powershell
tools/gpu-lock.ps1 run -Purpose "csm: Khronos" -Exec "build/msvc-release/systems/renderer/engine_renderer_tests.exe -tc=*cascaded*Khronos* -ns -s"
# the offset sweep: $env:ENGINE_SHADOW_NORMAL_OFFSET = 0|2|4; $env:ENGINE_SHADOW_PICTURE_LOD_ONLY = 1; $env:ENGINE_SHADOW_NO_TIMING = 1
```

**Cost, by the E1 rerun's method** ([E1 on Pascal](e1-pascal-rerun.md#setup)): one `engine-host --stdio` per machine, `render.load` once per scene — the procedural heightfield (`grid: 1025`, orbit 9) and 64 FlightHelmets (`grid_instances: 8`, orbit 22) — then per resolution and shadow mode a warm-up counted in time (chunks of frames, each sized from the one before, until two seconds of them have run) and three measured repeats of about one second each, the camera held still; the tables give the median repeat. `raster: hw` (the mesh path on the RTX 5090, the vertex path's indexed draw on the TITAN Xp), occlusion culling on — it stays on under maps, and `rt` turns it off, as it always has.

```text
{"jsonrpc":"2.0","id":"1","method":"render.load","params":{"mesh":"","scene":"","grid":1025,"cache":false,"settings":{"shadows":"off"}}}
{"jsonrpc":"2.0","id":"2","method":"render.benchmark","params":{"scene":"scene1","width":1920,"height":1080,"orbit":{"distance":9,"yaw_deg":0},"settings":{"raster":"hw","shadows":"csm","lod_px":1.0,"cull":true,"occlusion":true,"cone":true,"lights":true,"deform":"none"},"frames":60}}
```

The second line is repeated with `frames` growing until two seconds of warm-up have run, then three times with about a second's worth. The helmet grid loads with `"mesh":"<FlightHelmet.gltf>","grid_instances":8` and uses `"distance":22`.

**Cost along a path**: the [desert overlook flythrough](flythrough-desert-overlook.md), `tools/flythrough.ps1 -Shadows csm -Resolutions 1920x1080,3840x2160 -Occlusion on,off`, both asset sets on the RTX 5090 and the substitute set on the TITAN Xp, the same flags as that page's rows, plus a same-build `-Shadows off` run beside each.

## Results

### Correctness on the fixtures

Machine state: the RTX 5090 cases ran under the GPU lock in a debug build; the TITAN Xp's in the server's full suite (`remote-build.ps1 -Test`, 54 of 54 passed), which is a correctness run and not gated on load.

| check | RTX 5090, mesh path | RTX 5090, vertex path | TITAN Xp, vertex path |
|---|---|---|---|
| gfx: shadowed and lit pixels against the CPU reference with the sun removed (worst difference of 255) | 1,296 and 9,396 (1) | 1,296 and 9,396 (1) | 1,296 and 9,396 (1) |
| gfx: flat quad shadowing itself at a 24° and a 75° sun | 0, 0 | 0, 0 | 0, 0 |
| maps against a CPU ray, card / slab / cube / L: interior pixels that differ (interior, shadowed) | 0 (34,740, 304) / 0 (34,054, 340) / 0 (33,456, 920) / 0 (33,726, 921) | the same, and the same bytes | the same |
| a card outside the camera's frustum: shadowed interior pixels, differing from a CPU ray | 61,997, 0 | — | 61,997, 0 |
| the same card, ray-traced | 0 shadowed | — | no ray queries |
| cascades' frustum test off against on: colour bytes and id words that differ (cube, L, heightfield, heightfield under `--deform wave`) | 0 | — | 0 |
| maps against the ray-traced shadow of the same frame: interior pixels that differ, card / slab / cube / L | 0 / 0 / 0 / 0 | — | no ray queries |

The band a filter needs is stated where it is computed and is narrow: gfx's case leaves out pixels whose sun ray passes within 0.345 world units of the occluder's edge (a quarter unit of CPU-against-GPU edge, three texels of filter footprint, a third of a centimetre of normal offset; 1,408 pixels, 180 of them grey), the fixture's cases two pixels either side of a CPU-ray edge (2,333–2,652 pixels, 31–41 grey). The frustum test removes 143 of 198, 145 of 201 and 336 of 792 pairs from the maps (334 of 780 on the TITAN Xp, whose GCC build makes a slightly different heightfield).

### Mapped against ray-traced, on the Khronos samples

Opposite answers inside the band, by normal offset, at the picture's cut. Acne is lit by the rays and black in the maps; leak the reverse; grey a partial answer. Interior pixels over the sixteen views, and of them the rays' shadowed ones.

| sample | interior | shadowed by rays | offset 0: acne / grey | offset 2: acne / grey | **offset 3: acne / grey** | offset 4: acne / grey |
|---|---|---|---|---|---|---|
| FlightHelmet | 7,314,530 | 446,987 | 6,835 / 28,520 | 3 / 23,156 | **0 / 19,819** | 0 / 17,117 |
| SciFiHelmet | 6,644,874 | 404,830 | 2,875 / 50,028 | 1 / 16,997 | **0 / 12,906** | 0 / 10,548 |
| BoomBox | 7,395,505 | 475,584 | 1,913 / 39,393 | 0 / 2,933 | **0 / 2,322** | 0 / 2,233 |
| Lantern | 6,354,783 | 131,687 | 162 / 23,752 | 0 / 22,319 | **0 / 21,758** | 0 / 21,076 |
| Corset | 7,298,222 | 448,910 | 4,320 / 51,319 | 0 / 11,378 | **0 / 8,188** | 0 / 6,455 |
| Avocado | 6,367,377 | 337,053 | 7,215 / 55,434 | 0 / 5,640 | **0 / 3,730** | 0 / 2,766 |
| Suzanne | 7,126,778 | 397,647 | 21,627 / 103,781 | 3 / 24,220 | **0 / 15,144** | 0 / 11,439 |

**Leaks: 0 in every cell.** No offset up to four texels made the maps light an interior pixel the rays shadow. Three is the default: the smallest offset that gave no opposite answer on any sample. The grey that remains, 0.03–0.34% of the interior, is where the last cascade's 4×4 footprint, about two pixels a texel at the orbit's distance, reaches past the three-pixel band. In the band itself (320,000–690,000 pixels a sample), the maps give the opposite of the rays' answer on 0–129 pixels at offset 3, against 6,187–31,397 at offset 0.

The same frames with the cascades drawn from a coarser cut than the picture's, offset 3:

| sample | pairs in the maps, x1 / x2 / x4 (over 16 views) | acne x1 / x2 / x4 | leak x1 / x2 / x4 |
|---|---|---|---|
| FlightHelmet | 10,670 / 5,839 / 3,058 | 0 / 423 / 2,482 | 0 / 0 / 34 |
| SciFiHelmet | 7,705 / 5,272 / 2,823 | 0 / 8,436 / 38,888 | 0 / 0 / 0 |
| BoomBox | 1,616 / 1,076 / 702 | 0 / 16 / 113 | 0 / 0 / 0 |
| Lantern | 746 / 582 / 576 | 0 / 2,559 / 2,708 | 0 / 0 / 0 |
| Corset | 3,360 / 1,920 / 1,472 | 0 / 3,608 / 10,752 | 0 / 0 / 104 |
| Avocado | 320 / 320 / 320 | 0 / 0 / 0 | 0 / 0 / 0 |
| Suzanne | 1,402 / 1,180 / 766 | 0 / 296 / 15,727 | 0 / 0 / 0 |

(The Avocado is so small in the frame that its cut is the same at all three thresholds.)

What they cost at engine-view's camera, 1280×720, milliseconds per frame from `gfx::GpuTimer` — the mean of the forward and the backward run, which agree within 0.004 ms:

| sample | off: frame | rays: frame (chain) | maps: frame | maps' passes (of which culls) | resolve: off / rays / maps | maps x4: frame |
|---|---|---|---|---|---|---|
| FlightHelmet | 0.077 | 0.251 (0.183) | 0.107 | 0.020 (0.010) | 0.028 / 0.047 / 0.037 | 0.104 |
| SciFiHelmet | 0.077 | 0.232 (0.168) | 0.106 | 0.021 (0.011) | 0.027 / 0.044 / 0.036 | 0.104 |
| BoomBox | 0.073 | 0.203 (0.143) | 0.098 | 0.016 (0.010) | 0.028 / 0.041 / 0.037 | 0.096 |
| Lantern | 0.068 | 0.180 (0.131) | 0.088 | 0.014 (0.010) | 0.024 / 0.034 / 0.034 | 0.089 |
| Corset | 0.075 | 0.215 (0.157) | 0.102 | 0.017 (0.011) | 0.027 / 0.040 / 0.036 | 0.101 |
| Avocado | 0.068 | 0.180 (0.126) | 0.089 | 0.013 (0.010) | 0.024 / 0.037 / 0.034 | 0.090 |
| Suzanne | 0.072 | 0.198 (0.141) | 0.097 | 0.017 (0.011) | 0.025 / 0.039 / 0.035 | 0.096 |

Machine state: under the GPU lock; the test harness does not sample the machine, and other agents were compiling on the CPU during the group, so these are upper bounds. The ratios within a sample were taken seconds apart.

### What the maps cost: two scenes, three resolutions (the E1 rerun's method)

Milliseconds of GPU work per frame (the sum of the timed passes), the median of three repeats; "maps" is every pass that draws them (their culls in brackets), and the resolve's growth under maps is the filter. `rt` runs with occlusion culling off, as it must; the other two with it on. Pairs are the picture's and the maps' (summed over the cascades); the ray-traced rows count the chain's shadow casters in the picture's.

**RTX 5090** (mesh path). Machine state: under the GPU lock, which waited 661 s for another agent's suite; the GPU 0–10% busy before every run with 7.5–9.4 GB of its 32 GB held by the owner's resident tools; other processes used a median 23% and at most 47% of the CPU (the owner's ComfyUI backend and other agents' builds), which the harness flags. Repeats agree within 2.4%.

| scene | resolution | off | maps: frame | maps' passes (culls) | resolve, off → maps | rays: frame | rays' chain | pairs: picture / maps |
|---|---|---|---|---|---|---|---|---|
| heightfield, orbit 9 | 1920×1080 | 0.119 | 0.190 | 0.051 (0.014) | 0.051 → 0.071 | 0.456 | 0.226 | 717 / 2,017 |
| heightfield, orbit 9 | 2560×1440 | 0.177 | 0.261 | 0.056 (0.015) | 0.081 → 0.111 | 0.621 | 0.247 | 852 / 2,344 |
| heightfield, orbit 9 | 3840×2160 | 0.360 | 0.475 | 0.060 (0.015) | 0.169 → 0.230 | 1.060 | 0.278 | 1,075 / 2,800 |
| 64 FlightHelmets, orbit 22 | 1920×1080 | 0.073 | 0.094 | 0.018 (0.012) | 0.020 → 0.023 | 2.092 | 2.036 | 1,713 / 1,766 |
| 64 FlightHelmets, orbit 22 | 2560×1440 | 0.091 | 0.112 | 0.019 (0.012) | 0.027 → 0.030 | 2.530 | 2.460 | 2,403 / 2,478 |
| 64 FlightHelmets, orbit 22 | 3840×2160 | 0.218 | 0.240 | 0.023 (0.014) | 0.062 → 0.065 | 3.004 | 2.861 | 3,426 / 3,556 |

The maps cost **0.07–0.12 ms** on the heightfield and **0.02 ms** on the helmet grid, against **0.34–0.70 ms** and **2.0–2.8 ms** for rays — rays as they were on this tree, before the chain was sized by the frame and its bottom-level structures became one build ([the flythrough's section on it](flythrough-desert-overlook.md#the-ray-tracing-chain-sized-by-the-frame)); the helmet grid's 64 serial bottom-level builds are most of its 2.0–2.8 ms, so its rays rows are the ones that change most. The helmet grid's orbit frames all of it, so its one cascade is the grid's bounds, and the maps draw exactly the pairs the ray chain builds (the picture's plus its shadow casters). The heightfield's camera stands inside the terrain's bounds, so its cascades are slices of the view, and together they draw 2.6–2.8 times the picture's pairs at the camera's cut: terrain beside and behind the view that lies within a cascade's sphere. On the terrain, which fills the frame, a quarter to a half of the maps' cost is the filter in the resolve, and it grows with the pixels (0.020 → 0.061 ms from 1080p to 4K). The maps' own passes barely move with resolution (0.051 → 0.060 ms).

**TITAN Xp** (the vertex path's indexed draw; no ray queries, so no rays column). Machine state: every group started at a load average of 0.46–0.99 with the GPU 0% busy; inside the runs other processes used a median 1.2% and at most 8.6% of the CPU, and the card held 285–1,136 MiB, all of it this process's. Repeats agree within 0.8%. A first pass whose warm-up was sized from one 60-frame probe, and so ran about 0.4 s rather than 2 s, agreed with these to 0.5%; it is not used.

| scene | resolution | off | maps: frame | maps' passes (culls) | resolve, off → maps | pairs: picture / maps |
|---|---|---|---|---|---|---|
| heightfield, orbit 9 | 1920×1080 | 0.805 | 1.168 | 0.167 (0.044) | 0.478 → 0.672 | 682 / 2,016 |
| heightfield, orbit 9 | 2560×1440 | 1.381 | 1.897 | 0.181 (0.044) | 0.835 → 1.168 | 842 / 2,320 |
| heightfield, orbit 9 | 3840×2160 | 3.027 | 3.933 | 0.197 (0.044) | 1.847 → 2.558 | 1,047 / 2,799 |
| 64 FlightHelmets, orbit 22 | 1920×1080 | 0.379 | 0.462 | 0.070 (0.042) | 0.145 → 0.155 | 1,713 / 1,766 |
| 64 FlightHelmets, orbit 22 | 2560×1440 | 0.575 | 0.671 | 0.077 (0.043) | 0.237 → 0.254 | 2,403 / 2,478 |
| 64 FlightHelmets, orbit 22 | 3840×2160 | 1.104 | 1.236 | 0.094 (0.044) | 0.491 → 0.526 | 3,426 / 3,556 |

**On Pascal the filter costs more than the maps.** On the frame-filling terrain the resolve grows by 0.19 ms at 1080p, 0.33 at 1440p and 0.71 at 4K, about 85 ps a pixel, against 0.17–0.20 ms for every pass that draws the maps. On the RTX 5090 the same filter is 0.020–0.061 ms. Four gathers of a 64 MiB depth atlas and the arithmetic of sixteen per-texel references are cheap where memory is fast and expensive where it is not. The maps' own passes are the cull pass run once per cascade (0.044 ms in all, against 0.051–0.074 for the picture's cull) and a depth-only draw of 1,800–3,600 pairs, and they move little with resolution.

### Along the desert overlook's path

The frame's median, p95 and p99 over the path's 2,401 frames; the rows and the columns are in the [flythrough write-up](flythrough-desert-overlook.md#cascaded-shadow-maps-rtx-5090-and-titan-xp), beside that page's rows without shadows and with rays. In short, occlusion culling on (off for rays, which impose it):

| card, set | resolution | no shadows | maps | maps' passes | rays (that page, the chain before it was sized by the frame) |
|---|---|---|---|---|---|
| RTX 5090, substitute | 1920×1080 | 0.119 / 0.147 / 0.153 | 0.215 / 0.267 / 0.275 | 0.080 / 0.103 / 0.106 | 2.401 / 2.863 / 2.922 |
| RTX 5090, substitute | 3840×2160 | 0.350 / 0.432 / 0.448 | 0.487 / 0.605 / 0.690 | 0.094 / 0.132 / 0.141 | 3.136 / 3.841 / 3.946 |
| RTX 5090, overlay | 1920×1080 | 0.134 / 0.199 / 0.218 (that page) | 0.312 / 0.491 / 0.512 | 0.150 / 0.276 / 0.297 | 3.790 / 5.696 / 5.915 |
| RTX 5090, overlay | 3840×2160 | 0.375 / 0.509 / 0.554 (that page) | 0.639 / 0.944 / 0.996 | 0.214 / 0.406 / 0.426 | 6.011 / 7.791 / 8.125 |
| TITAN Xp, substitute | 1920×1080 | 0.794 / 1.029 / 1.083 | 1.391 / 1.839 / 1.902 | 0.437 / 0.699 / 0.716 | — |
| TITAN Xp, substitute | 3840×2160 | 2.819 / 3.767 / 3.970 | 3.966 / 5.729 / 5.944 | 0.557 / 1.883 / 1.947 | — |

**The maps draw seven to fourteen times the picture's pairs along this path** with occlusion culling on (four to five times with it off): 11,695 at the 1080p median against 1,522, 25,659 at p95; 41,299 and 109,167 with the owner's landmarks. A cascade holds a slice's sphere and everything between it and the light, and with no shadow distance set the last cascade reaches the far side of a 5 km scene. Everything in it is drawn at the camera's cut, which is coarse far from the camera, except for the overlay's palms, whose cut does not coarsen ([E10](e10-generated-props.md)). On the RTX 5090 that is still 0.08–0.21 ms of median frame: a twenty-fourth to a twenty-ninth of the ray-traced chain as this page's rays column measured it, and a fifth (substitute) and a quarter (overlay) of the chain at 1080p since it is sized by the frame (0.400 and 0.606 ms at the median, [flythrough](flythrough-desert-overlook.md#the-ray-tracing-chain-sized-by-the-frame)). On the TITAN Xp it is 0.44–0.56 ms at the median and 0.70–1.88 ms at p95, where the camera stands high over the overlook and every cascade is full of terrain.

## What surprised me

- **The first disagreement was the rays', and it came from the test rig.** The first Khronos run found the BoomBox's whole ground shadow displaced by 9–13 pixels between the two implementations: 11,813 interior pixels lit by the maps that the rays shadowed. The ground was scaled six sample radii across and 1 in height. A bounding sphere scales by the largest axis, so under a 2 cm sample the scene's radius became the unscaled quad's, 0.707 against 0.078. The ray-traced bias, a thousandth of that radius along the surface normal, became nine times what it is for every other sample. A lifted ray start moves a ground shadow by the lift over the tangent of the sun's elevation. The Corset and the Avocado, both under 4 cm, carried the same error at smaller size. With the ground scaled uniformly the leaks went to 0 on every sample. The bias rule is the ray path's and was left alone. The lesson is general: **the ray-traced bias is proportional to the scene's bounds, so anything that inflates the bounds — a flat mesh with an untouched axis, a far-away prop — moves every ray-traced shadow.** On the desert overlook, whose bounding sphere has a radius of about 3.6 km, it is about 3.6 m.
- **The terminator, not the ground, is where maps and rays part.** Every opposite answer left after the rig was fixed was on a sample's own surface, in two shapes. The first was a band a triangle wide along the terminator. There the geometric normal of a low-poly curve already faces away from the sun while the shading normal still faces it, the first version answered 0 by rule, and a ray lifted off the triangle skims the convex surface and reaches the sun. The second was lines along concave creases, where the neighbouring triangle rises above the receiver's plane inside the 4×4 footprint. The receiver-plane bias is exact for a flat receiver and was, on the fixtures, at every angle; it cannot see either of these. The normal offset of three texels at grazing removed both, and it moved nothing on the flat fixtures.
- **A coarser cut in the maps is worse, not cheaper.** The expected trade was a coarser cascade cut for fewer pairs at some cost in accuracy. What came back was self-shadowing from the mismatch between the triangle the resolve reconstructs and the triangle in the map (up to 38,888 interior pixels on the SciFiHelmet at four times the threshold), and a saving of at most 0.002 ms, because a cascade at these sizes costs its dispatches, not its pairs.
- **Pascal pays for the filter, not the maps.** On the RTX 5090 the filter is a small part of the maps' cost; on the TITAN Xp, on a frame-filling terrain, it is more than every pass that draws them (above).
- **The maps' pairs are the path's cost on the baseline tier.** The p95 of the TITAN Xp's maps passes at 4K, 1.88 ms against a median of 0.56, is the overlook: the camera high, the cascades long, and 38,759 pairs in them at the camera's cut, most of them terrain beside and behind the view.

## What it decides

**The cascades draw the picture's cut.** The alternative left open by the direction note, a cut chosen from the light's own view, was measured as a coarser cut in the maps (x2, x4): it brought back self-shadowing that no bias predicts and saved nothing measurable. **The bias is the receiver's plane per texel, plus half a texel, plus a normal offset of three texels at grazing**: 0 opposite answers against the rays on seven samples and on the fixtures, and 0 leaks at any offset tried. **The filter is a 3×3 bilinear PCF**, a three-texel penumbra that is sub-pixel wherever the fit matches texels to pixels. **`auto` is `csm` without ray queries and `rt` with them**, which leaves the RTX 5090's picture and cost unchanged. Whether the RTX 5090 class should default to maps is the owner's decision; this page is the evidence. Along the path, against the chain as it now is (sized by the frame, one bottom-level build), the maps' passes cost a fifth to a quarter of the ray-traced chain at 1080p and the whole frame with maps is a third to two-fifths of the frame with rays; against the chain these rows were first compared with, a twenty-fourth to a twenty-ninth and a sixth to a twelfth. The maps keep occlusion culling on and cast from outside the frustum. They shadow only the sun, with a filtered edge instead of a hard one.

**What it does not decide:** point and spot lights under maps (none cast a mapped shadow); cascade blending; caching the maps across frames; a tighter caster volume than a cascade's sphere and box; and the filter's cost on Pascal. Each is a follow-up with a number attached.

## Caveats

- One path, one sun direction, one scene scale, and seven samples. The Khronos rig's sun is fixed and the ground flat, so steep terrain under a low sun is covered only by the flythrough's pictures, which were not compared against rays here.
- **Every rays number on this page is the chain before it was sized by the frame.** That change (`e77ea19`, "size the ray tracing chain by the frame, not the scene") landed on main just before this one and made the chain six to seven times faster by building every instance's bottom-level structure in one command; this page's maps rows were taken on the tree before it, and nothing here was re-measured after. The maps' own numbers do not depend on it (the maps never build the chain). The ratios against rays are restated above against the flythrough's after rows where those exist (1080p and the surround); the helmet grid's and the Khronos samples' rays columns would be shorter now.
- The Khronos timings come from the test case, which does not sample the machine. They were taken under the lock while the CPU was shared, so they are upper bounds, and the ratios within one sample are what they support.
- The RTX 5090's rows carry the harness's WARNING (the owner's resident tools and other agents' builds on the CPU). They are GPU timestamps of a card that was otherwise idle under the lock, and the same-build rows without shadows reproduce the flythrough page's earlier rows to within 3% (0.119 against 0.118 ms, 0.350 against 0.345). The ray-traced flythrough rows are that page's, from an earlier build.
- The TITAN Xp's flythrough is the substitute set only, as on the flythrough page.

## Follow-ups

- **A tighter caster volume.** A cascade culls against its sphere's box extended towards the light, so terrain beside and behind the frustum is drawn at the camera's cut. On the path that is seven to fourteen times the picture's pairs. Culling casters against the view frustum's planes that face away from the light, or a default shadow distance well short of a 5 km scene's far side, would cut most of it. The first needs the cull pass to take more than six planes.
- **Cache the far cascades.** The sun and the terrain do not move between frames; only the cascades that the camera's movement shifts need redrawing.
- **The filter on Pascal**: a single gather (2×2) on the baseline tier, a 16-bit atlas, or choosing the cascade once per tile, measured against today's 85 ps a pixel.
- **The ray-traced path's light-space cull**, so that an off-frustum caster casts under rays as it now does under maps (owned with the acceleration-structure sizing), and a **bias for rays that does not scale with the scene's bounds** (the rig's lesson above).
- **Point and spot lights**: a cube or perspective map each, or the ReSTIR-sampled lights of [04 §4.4](../plan/04-renderer.md#44-ray-tracing).
- **Cascade blending**, so the penumbra's width does not step where one cascade hands over to the next.
