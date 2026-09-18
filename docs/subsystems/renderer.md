# renderer (systems)

**Purpose.** The engine's renderer ([04](../plan/04-renderer.md), [02 §2.3](../plan/02-architecture.md#23-subsystem-boundaries-and-layering) L3): it turns a set of meshes and instances of them into a picture and into the numbers that describe how it was made. It owns the whole frame — source-mesh loading through the derived-data cache, the GPU-resident scene, two-pass GPU culling and LOD selection, the four rasterizers, the deformed-vertex pool, the per-frame cluster acceleration structures, the material resolve, the Hi-Z pyramid, the GPU timers, and the capture — and it does all of it **without a window**. `engine-view` is the command line over it and `engine-host`'s `render.*` methods are the protocol over it; neither is privileged, and a third consumer (the editor, a test, a benchmark harness) needs nothing new.

**Why it is a module and not an app.** Until this split every one of those passes lived in `apps/engine_view/main.cpp`, 2,654 lines of it, and the consequence was not that the file was long: it was that **nothing but engine-view could render**. The protocol had no `capture` and no `benchmark`, so the Phase 1 exit criterion ("agents can capture and benchmark", [10 §10.2](../plan/10-roadmap-risks.md#102-phases)) could not be met without either duplicating the frame in the host or driving a child process and reading its stdout. The renderer is the engine's own, in the same sense `gfx` is — not a capability a game adds ([ADR-0027](../adr/0027-additive-capabilities.md)) — so it is `engine_module(... LAYER systems)` with no `OPTIONAL`: a build without it could not draw at all, and every consumer of it would have to be switched off with it.

**The window is the app's.** `foundation/window` is not a dependency of this module, and nothing here touches a surface, a swapchain, or a present queue. A frame is recorded into a color image the caller names: engine-view acquires a swapchain image and hands it in with its two semaphores; engine-host renders into an offscreen target the renderer owns. That is the *only* difference between the two, which is why a picture captured through the protocol is the picture engine-view shows. `systems/renderer/tests/renderer_tests.cpp` opens with `#if __has_include(<foundation/window/window.h>) #error`, so the day the dependency comes back the build says so rather than a hosted CI run failing to open a display.

## Owned data

| Owner | What it is sized by | What it holds |
|---|---|---|
| `SceneDesc` | — | what to load: mesh paths, instances, the instance grid, the heightfield resolution, the derived-data root |
| `SceneData` | the content | one merged cluster LOD DAG, one `geometry::ClusterMeshPart` per mesh, each mesh's materials and images, the instance table, the bounding sphere, and the counts a summary reports |
| `GpuScene` | the **scene** | every device buffer whose size is a function of the scene: geometry behind device addresses, the material table and the bindless textures, the instance table, the deformed-vertex pool, the visible list and the indirect argument blocks, and the ray tracing chain |
| `SceneRenderer` | the **screen** | the shader library and its pipelines, the visibility buffer and the Hi-Z pyramid, the frame context, the GPU timers, the render graph, the per-slot parameter blocks, and `Stats` |

That line — scene-sized against screen-sized — is also the order the two build in, and it is not arbitrary: the `gfx::MeshDesc` array names the deformed-vertex pool, the deform table, and this scene's cluster templates, so it is uploaded last, after every address it carries exists. A `GpuScene` therefore finishes itself; a `SceneRenderer` is created against a finished one.

## Invariants

- **A picture does not depend on which host drew it.** Every device- and scene-driven override lives in one function, `resolve_settings`, and nothing below it probes `gfx::DeviceFeatures` again. The order of the overrides is load-bearing and the function says so: the mesh-shader fallback decides which path runs, the path decides whether shadows can be traced, and shadows decide whether two-pass occlusion culling may run (the acceleration structures are built from one visible list, and pass 2's survivors are in a second one). Everything after that only ever forces a switch *on*.
- **What a summary reports is what ran**, not what was asked for: `ResolvedSettings::settings` carries the overrides, so a run whose `--raster hw` fell back to the vertex path says `"raster":"vertex"`.
- **The renderer never reads a buffer back inside a frame.** The visible-pair counts come from a per-slot copy of the indirect argument blocks, read one frame late; the GPU milliseconds come from `gfx::GpuTimer`, never from a CPU clock. The only blocking readbacks in the module are in `capture()`, which is not a frame path.
- Pass bodies are stored in the render graph's arena and capture by reference, so declaring, compiling, and executing a frame are **one function** (`record_frame`): every parameter block a body pushes has to still be alive when `execute()` records it.
- A capture's id buffer names the (instance, cluster, triangle) a pixel's visibility id led to, never the id itself — see below.

## The offscreen contract

```cpp
SceneData data;   load_scene(desc, data, error);
ResolvedSettings resolved;  resolve_settings(settings, device.features(), &data, resolved);
if (check_availability(resolved, device.features()) != RenderAvailability::Ok) { ... }
GpuScene scene;   scene.create(device, data, resolved, &error);
SceneRenderer r;  r.create(device, scene, resolved, {.width = w, .height = h}, &error);
```

`check_availability` is deliberately separate from `create`: a device that cannot render is not a caller's mistake, and the two hosts report it differently — engine-view exits 3 (which its tests treat as a skip) and engine-host answers protocol error 1007 (`RenderUnavailable`). It checks two things and not a third: 64-bit buffer atomics, without which nothing can write the visibility buffer, and cluster acceleration structures plus ray queries when the ray path was asked for. **Presentation is not checked**, because presenting is the window's requirement and not the renderer's.

One frame is `begin_frame()` (wait for the slot, fold in the completed frame's statistics and timings), then `submit_frame(FrameDesc)`, then — for a presented frame — the caller's present. `FrameDesc` names the color image, the layout to leave it in, and the two semaphores; a null image draws into the renderer's own offscreen target. `render_offscreen()` is the three calls plus the wait, and `capture()` is `render_offscreen` plus the readbacks. The pipelines are built for one color format (`Desc::color_format`), which engine-view sets to the swapchain's so the picture is produced exactly as it is presented; the offscreen default, `R8G8B8A8_UNORM`, differs from a typical swapchain's `B8G8R8A8_UNORM` only in channel order and so reads back byte for byte the same.

The camera is a plain struct, and `orbit_camera(center, radius, distance, frame)` is the orbit engine-view has always used — distances scale with the scene's radius, and a `distance` of zero breathes between 8 and 36 radius-tenths. `orbit_camera_at(center, radius, distance, yaw, pitch)` is the same orbit at explicit angles, where `pitch` is the elevation above the orbit circle; its default, `k_orbit_pitch` = atan(0.45), is engine-view's fixed height factor, so `--orbit 22` and `orbit {distance: 22}` are the same camera and the two hosts' captures of a scene compare directly.

## Capture channels, and the id encoding

`CaptureChannels` asks for any of four, and `CapturedFrame` carries what came back.

| Channel | Form | Notes |
|---|---|---|
| `color` | RGBA8 | the picture, as `engine-view --capture` writes it |
| `ids` | three `u32` per pixel | instance, cluster, triangle; `0xFFFFFFFF` in all three where the scene did not cover the pixel |
| `depth` | `f32` per pixel | reversed-Z clip depth, 0 where nothing covered it, plus the covered range as `depth_min`/`depth_max` |
| `normals` | RGB8 | the world normal encoded `n * 0.5 + 0.5` |

**The id channel is the plan's entity-ID buffer** ([04 §4.2](../plan/04-renderer.md#42-frame-architecture), [10 §10.2](../plan/10-roadmap-risks.md#102-phases) Phase 1), and it is resolved rather than raw on purpose. The visibility buffer holds `depth << 32 | (visible_index << 8 | triangle)`, and `visible_index` names an entry of **this frame's** visible list — a cut entry, not a cluster (AGENTS.md). Two cuts of the same scene put the same triangle under different ids, and the list is overwritten by the next frame, so a raw id is meaningless the moment the frame is over. A capture therefore reads the visible list back with the visibility buffer and writes the pair the id led to. That is the number an agent can act on ("which instance is under this pixel"), and it is stable across rasterizers and LOD cuts. The decode is the same one `visible_entry` does in `visibility_resolve.slang`, including the no-cull case where the list is null and entry *i* is `{0, i}`.

`--raster direct` writes no visibility buffer — it draws mesh shaders straight to color with a depth buffer — so asking it for `ids` or `depth` is an **error with a message**, not a buffer of nothing.

`normals` costs a second frame: the normal is what the resolve reconstructs, and only the resolve knows it, so the renderer renders again in the normals view mode and reads the color back. `ids` and `depth` come out of the visibility buffer of the first frame and cost one blocking readback each.

**On disk** (`write_capture`, which engine-host calls and the module's tests check) the channels are `<base>.png`, `<base>.ids.bin` + `<base>.ids.json`, `<base>.depth.png`, and `<base>.normals.png`. The id buffer is a raw little-endian `u32` array with a JSON header beside it rather than an image, because it is data to be indexed (`(y * width + x) * 3`) and any lossy or reordered image form would make a pixel's instance number unreadable:

```json
{"format":"engine.renderer.ids.v1","file":"shot.ids.bin","width":400,"height":300,
 "type":"u32","byte_order":"little","words_per_pixel":3,
 "channels":["instance","cluster","triangle"],"order":"row_major_top_first",
 "empty":4294967295,"covered":47213}
```

Depth is an 8-bit gray PNG normalized over the frame's own covered range (black is both "not covered" and the far end), because the engine's own encoder writes 8 bits per channel and a depth channel is looked at far more often than it is measured — a measurement takes the ids beside it or the `f32` array in `CapturedFrame`. Adding a 16-bit path to `foundation/image` is the obvious next step if that stops being true.

## Statistics

`Stats` carries exactly the fields engine-view's JSON summary prints and engine-host's `render.benchmark` returns: frames, timed frames, the three visible-pair counts and their running minimum and maximum, the GPU milliseconds of each pass (cull, hw, sw, hiz, resolve, rt, clas, deform, trace, total) as sums with `*_ms()` accessors that divide by the timed frames, and the CPU nanoseconds spent inside `submit_frame`. The visible counts are read one frame late from a per-slot copy of the indirect argument blocks; `collect_visible()` is how a caller that waited for a specific frame (engine-view's last, before it captures) folds that frame's counts in without waiting for the slot to come around. The GPU timings of that frame are *not* in it, because a timestamp pool is read when its slot is reused.

**`Stats::gpu_memory` says who else was using the card.** `budget_mib`, `used_mib` and `device_local_total_mib` come from `gfx::MemoryBudget` ([gfx](gfx.md)) through `sample_gpu_memory()`, which `create()` and `reset_stats()` call so a summary always has a figure and which a host that measures calls again at the end of a run. It is a driver query and so is **not** in the frame path, which is the same rule as the one that keeps buffer readbacks out of it. The reason it is here at all: this project's development machine renders beside GPU jobs that hold most of its memory, and a frame time measured against a contended card is a measurement of the contention. Read it together with the `machine_state` block both hosts print — the Vulkan budget under-reports a CUDA tenant, and `nvidia-smi`'s number does not ([bench](bench.md#measuring-on-a-shared-machine) says how far apart the two were, measured).

## Public API

- `systems/renderer/settings.h` — `RasterMode`, `ShadowMode`, `RenderSettings`, `ResolvedSettings`, `resolve_settings`, `RenderAvailability`, `check_availability`, and the one spelling of every name (`raster_name`/`parse_raster_mode`, and the same for shadows, deform, and view modes) that the command line and the protocol share.
- `systems/renderer/scene.h` — `SceneDesc`, `SceneInstance`, `SourceMesh`, `SceneData`, `load_scene`, `read_scene_file`, `mesh_bounds`.
- `systems/renderer/gpu_scene.h` — `GpuScene`, `k_visible_runs`.
- `systems/renderer/scene_renderer.h` — `Camera`, `orbit_camera`, `orbit_camera_at`, `k_orbit_pitch`, `GpuMemory`, `Stats`, `FrameDesc`, `SceneRenderer` (including `sample_gpu_memory`).
- `systems/renderer/capture.h` — `CaptureChannels`, `CapturedFrame`, `CaptureFiles`, `write_capture`, `id_buffer_header`, `k_no_id`, `k_id_words`.

## What engine-view still owns

The flags and their validation, the window and its events, the surface and the swapchain (including the resize and the present), the screenshot of the *presented* image, the shader-reload timer, and the JSON summary line. It keeps the swapchain path because presenting is not the renderer's business and because reading the picture back out of the presented image is what makes an engine-view capture the same bytes it has always been. See [apps](apps.md).

## Depends on

`base`, `containers`, `math`, `time`, `log`, `platform`, `io`, `image`, `geometry`, `assets`, `gfx`, `json` (the scene file). **Not** `window`, by design and by a compile-time check.

## Testing

`tools/dev.ps1 test -Preset msvc-debug -Filter renderer` runs the module's tests, all headless. They write a unit-cube GLB at test time (one primitive, one material — the fixture `apps/engine_view/tests` writes, minus the textures) into a `test::TempDir` of their own, with the derived-data root inside it, so nothing in the tree, nothing in the repository's cache, and nothing another copy of the suite is using is touched (AGENTS.md, test hygiene). They skip with a message on a machine with no Vulkan device or no 64-bit buffer atomics, exactly as engine-view exits 3. They check: a cube rendered offscreen into all four channels at once, with the center pixel's id naming instance 0, a cluster of that scene, and a triangle under 128, and an uncovered corner naming nothing; every `Stats` field present and consistent; the four channels landing on disk in the documented shapes, with the id file exactly `width * height * 3 * 4` bytes; two instances of the cube producing exactly the ids 0 and 1 and no others; `resolve_settings` making the same five decisions on a synthetic baseline device, an RT device, and a path that cannot shadow; and the direct path refusing an id capture with a message while still capturing color. `Stats::gpu_memory` is checked with the rest of the statistics: a device-local total above zero always, and a budget within that total on a device with `VK_EXT_memory_budget`.

The pictures are pinned end to end by engine-view's own suite and by the comparison in that page: the FlightHelmet at a fixed orbit through `--raster hw`, `--raster vertex`, `--raster rt --shadows rt`, `--deform wave`, and `--grid-instances 3` is **byte-identical** before and after the move out of `main.cpp` (PSNR null, SSIM 1, FLIP 0 on all five).

## Performance notes

Nothing about the frame changed in the move, and the numbers in [apps](apps.md) and [gfx](gfx.md) still hold. Two allocations were added and both are outside the frame: the render graph is heap-allocated once (it is not default-constructible), and `capture()` allocates its staging buffers per call. The visibility buffer and the visible list gained `VK_BUFFER_USAGE_TRANSFER_SRC_BIT` so a capture can read them; nothing in a frame copies from either.

## Not yet

A second `SceneRenderer` on one `GpuScene` (the scene's per-frame working set — the visible list, the argument blocks, the flags — is single-writer, so two renderers would need their own), which is what multi-view ([04 §4.6](../plan/04-renderer.md#46-extreme-displays)) will want. Changing `RenderSettings` without rebuilding the `GpuScene` and the `SceneRenderer`: the deformed-vertex pool and the ray tracing chain are sized by the settings, so engine-host rebuilds both and keeps only the `SceneData`, which is where the import and the clustering cost is. Streaming and residency ([04 §4.9](../plan/04-renderer.md#49-streaming-and-residency)): a scene is uploaded whole. The reference path tracer and the accept-or-reject loop's automation ([04 §4.8](../plan/04-renderer.md#48-reference-renderer-and-objective-optimization)) — `render.compare` is the metric half of it, and the integrator is Phase 2's.
