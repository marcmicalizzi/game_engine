# apps: engine-host, engine-cli, engine-view (apps)

**Purpose.** The first executables (docs/plan/02-architecture.md §2.2, ADR-0001). `engine-host` is the engine as a headless server: in Phase 0 it runs in `sim` mode only and speaks JSON-RPC 2.0 over stdio, one request per line in and one response per line out. `engine-cli` is the thin scriptable client for humans, agents, and CI: it spawns a host, optionally opens a document, sends one method, prints the result, and exits. Because sessions persist their journal and undo position on disk (see [doc](doc.md) and [protocol](protocol.md)), a sequence of separate `engine-cli` invocations behaves like one editing session, which is what the Phase 0 exit criterion asks for and what the end-to-end test checks.

**engine-host.**
```
engine-host [--stdio] [--request <json>] [--mount <scheme>=<dir>[:rw]]... [--log <spec>] [--log-json <path>] [--tunables <file>]
```
stdout belongs to the protocol. Log records go to an in-memory ring (`log.tail` reads it), warnings and above to stderr, and optionally every record as JSON lines to a file. Assertion failures are routed through the log before the process aborts. `--request` answers one request and exits, for scripts that do not want a pipe.

**engine-cli.**
```
engine-cli [--host <path>] [--doc <dir>] [--create] [--name <name>] [--mount <spec>]... [--compact] <method> [params-json]
```
With `--doc` the CLI opens the document first (creating it with `--create`) and injects the session id into the params. Results print as pretty JSON (or one line with `--compact`); errors print `error <code>: <message>` and any diagnostics to stderr and exit 1; usage problems exit 2. The host is found beside the CLI unless `--host` says otherwise.

**Example (the exit criterion).**
```bash
engine-cli --doc ./world --create --name World session.info
engine-cli --doc ./world doc.apply '{"commands":[{"kind":"CreateObject","id":"00000000000000100000000000000001","type":"engine.content.AssetProvenance"}],"attribution":{"actor":"me","role":"environment","task":"t1","rationale":"first object"}}'
engine-cli --doc ./world doc.objects
engine-cli --doc ./world doc.undo
engine-cli --doc ./world doc.redo
engine-cli --doc ./world doc.add_layer '{"name":"quest"}'
engine-cli --doc ./world doc.diff '{"from_layer":"base","to_layer":"quest"}'
engine-cli gpu.adapters                       # what the machine's GPUs support (see gfx.md)
```

**engine-view.**
```
engine-view [--width <px>] [--height <px>] [--frames <n>] [--capture <file.png>] [--no-vsync] [--adapter <i>] [--validation] [--grid <n>] [--log <spec>] [--shaders <manifest.json>] [--lod <px>] [--no-cull]
            [--raster direct|hw|sw|auto] [--sw-px <px>] [--view id|tri|depth] [--orbit <d>]
```
The engine's first window (windowed mode). A procedural heightfield is built into a cluster LOD DAG (`geometry`), uploaded behind device addresses, culled and LOD-selected on the GPU every frame (`cluster_cull.slang`, threshold `--lod` in pixels, default 1), and drawn one mesh-shader workgroup per surviving cluster through an indirect draw into the swapchain with a reversed-Z depth buffer while the camera orbits and zooms between 8 and 36 units so the cut changes visibly; each cluster gets its own color, so the picture is also the cluster-partition debug view. `--no-cull` draws every leaf cluster instead. `--raster` picks the path: `direct` is mesh shaders straight to color with a depth buffer; `hw`, `sw`, and `auto` (default) rasterize into the 64-bit visibility buffer through the hardware path, the software rasterizer, or both split at `--sw-px` pixels of projected cluster diameter, then resolve it (`--view` shows cluster colors, triangle shading, or depth). `--orbit` fixes the camera distance for measurements. The JSON summary reports cluster and level counts, DAG build time, the hardware and software visible-cluster counts (read back one frame late from per-slot copies of the indirect arguments), and GPU milliseconds per pass (cull, hw, sw, resolve) averaged over the run from timestamp queries, which is how experiment E1 was measured. `--frames N --capture out.png` renders N frames and writes the last one as a PNG, and the process prints one JSON line of statistics (frames, seconds, average ms, size, cluster and triangle counts, whether the capture was written) to stdout, so scripts and agents look at the picture and the numbers without a human at the window. Escape or closing the window exits; resizing recreates the swapchain and the depth buffer. Shaders come from the build's manifest when it sits beside the executable (`<exe dir>/../shaders/manifest.json`, or `--shaders`), so editing `cluster_mesh.slang` while the window is open recompiles it through slangc and rebuilds the pipeline within a quarter second; a save that does not compile prints slangc's diagnostics to stderr and keeps the last good shader. `--log` with an explicit spec routes those categories to stderr at the given levels. Exit codes: 0, 1 error, 2 usage, 3 unavailable (no display, no Vulkan device, no mesh shaders, or no presentation), which the end-to-end test treats as a skip. On the RTX 5090 the default 257×257 grid (131,072 triangles, 2,898 clusters in 12 levels, 861 ms to build) draws 98 to 162 clusters per frame at a 1 px threshold and renders in about 0.65 ms per frame at 960×540 without vsync.

**Build.** `engine_app(NAME engine_host OUTPUT engine-host SOURCES ... DEPS ... [E2E_TESTS ...])` in `cmake/EngineModule.cmake` builds an executable into `build/<preset>/bin/`, records it in `modules.json` with layer `apps`, and can build an end-to-end test executable that receives the app's path as `ENGINE_APP_PATH`. The CLI test also receives `ENGINE_HOST_PATH`.

**Depends on.** engine-host: `protocol`, `log`, `io`, `tunables`. engine-cli: `json`, `platform` (`Process`, `executable_directory`). engine-view: `window`, `image`, `geometry`, `gfx`, `math`, `time`, `log`.

**Testing.** `tools/dev.ps1 test -Filter engine_cli` runs the end-to-end suite: info and methods through a spawned host, error exit codes, and the exit criterion with one process per step (create, apply, objects, undo, redo across processes, add layer, diff, get, validate, journal). `-Filter engine_view` runs engine-view for six frames at 320×200 with a capture and checks the JSON summary and the PNG header, plus the usage exit codes; it records a skip where the app exits 3.

**Not yet.** engine-host `offscreen` and `windowed` modes (engine-view is the standalone window until the host learns to own one), sockets, multiple clients per host, long-running operations, the MCP bridge, a persistent-host mode for the editor.
