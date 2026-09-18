# profiling (core)

**Purpose.** The profiler hooks every module instruments itself with (docs/plan/09-testing-profiling.md §9.3, ADR-0012). With the CMake option `ENGINE_TRACY` the macros are Tracy client zones, frame marks, plots, messages, and allocation events; without it they expand to nothing, so instrumentation stays in the code at zero cost. Tracy runs in on-demand mode: until a profiler connects, a zone costs a predictable few nanoseconds.

**Placement.** The plan lists profiling under the foundation layer; the hooks live in core so that the job system, I/O, and every other core module can carry zones. The profiling *service* (capture export for the `profile` protocol tool, frame reports, budget checks) is the foundation-layer module that will build on this.

**Public API.** `core/profiling/profile.h`: `ENGINE_PROFILE_ZONE()`, `ENGINE_PROFILE_ZONE_NAMED(name)`, `ENGINE_PROFILE_ZONE_TEXT(text, size)`, `ENGINE_PROFILE_FRAME()`, `ENGINE_PROFILE_FRAME_NAMED(name)`, `ENGINE_PROFILE_THREAD(name)`, `ENGINE_PROFILE_PLOT(name, value)`, `ENGINE_PROFILE_MESSAGE(text, size)`, `ENGINE_PROFILE_ALLOC(ptr, size)`, `ENGINE_PROFILE_FREE(ptr)`; `profiling::enabled()`, `profiling::connected()`.

**Rules.** One `ENGINE_PROFILE_ZONE` per scope (it declares a local). A zone per system and per pass, not per element; per-job zones exist in the job system because a job is the unit of scheduling. Plots for the counts a frame budget tracks. Worker threads name themselves.

**Instrumented today.** Job workers (thread names, a zone per executed job), protocol dispatch, document store load and save.

**Build.** `cmake/EngineProfiling.cmake` fetches Tracy v0.14.1 (BSD-3, `third_party/LICENSES.md`) when `ENGINE_TRACY` is on; the `msvc-release` and `linux-gcc-release` presets turn it on, debug presets leave it off, so both modes compile in CI. The client is configured loopback-only (below). Tracy's headers are included as system headers so the engine's warning policy applies only to engine code.

**The client listens on loopback only.** A Tracy client opens a listening TCP socket when the process starts and announces itself by UDP broadcast, and Tracy's default is every interface. On Windows a listener on a non-loopback address makes the firewall raise a prompt once per executable *path*, and this tree makes paths cheaply: every preset and every agent worktree has its own copy of every test, bench, and app. `ENGINE_TRACY` therefore sets `TRACY_ONLY_LOCALHOST` and `TRACY_NO_BROADCAST`; a profiler on the same machine connects as before, and `-DENGINE_TRACY_REMOTE=ON` restores Tracy's default for profiling another machine (a baseline-tier box from the desktop), where one prompt for one binary is the expected price.

Why this is written down at length: it cost most of a night. On 2026-09-18 every process on the development machine that created a Vulkan device crashed with an access violation inside the NVIDIA driver (`nvoglv64.dll` or `nvcuda64.dll` under `vkCreateDevice`, driver 610.88), twice, for hours, with binaries that had passed minutes earlier. It was not a wedged driver, not the locked screen, not memory pressure (device creation worked with 31 GB of 32 in use), and not an implicit Vulkan layer. Each pending firewall prompt is a `PickerHost.exe FirewallNotificationDialogServer` process holding a GPU context; thirty-five of them took the machine from 49 GPU client processes to 77, and somewhere between those two numbers the driver stops surviving `vkCreateDevice`. Dismissing the prompts fixed it on the spot. The diagnosis, for the next time device creation crashes machine-wide: `nvidia-smi --query-compute-apps=pid --format=csv,noheader` for the client count, `Get-CimInstance Win32_Process -Filter "Name='PickerHost.exe'"` for the prompts, and the Application event log (Id 1000, faulting module `nv*`) for when it began. The block rules Windows added for the old paths are harmless and the owner's to remove.

**Depends on.** `base`; Tracy when enabled.

**Testing.** `tools/dev.ps1 test -Filter profiling` compiles every macro in whichever mode the preset selects and checks `enabled()` and `connected()`.

**Performance notes.** Off: nothing. On, not connected: an atomic load per zone. Connected: Tracy's own overhead, about 20 ns per zone. Memory allocation events are not wired into `core/memory` yet; that hook arrives with the mimalloc backend so both change once.
