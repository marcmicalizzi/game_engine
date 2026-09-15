# profiling (core)

**Purpose.** The profiler hooks every module instruments itself with (docs/plan/09-testing-profiling.md §9.3, ADR-0012). With the CMake option `ENGINE_TRACY` the macros are Tracy client zones, frame marks, plots, messages, and allocation events; without it they expand to nothing, so instrumentation stays in the code at zero cost. Tracy runs in on-demand mode: until a profiler connects, a zone costs a predictable few nanoseconds.

**Placement.** The plan lists profiling under the foundation layer; the hooks live in core so that the job system, I/O, and every other core module can carry zones. The profiling *service* (capture export for the `profile` protocol tool, frame reports, budget checks) is the foundation-layer module that will build on this.

**Public API.** `core/profiling/profile.h`: `ENGINE_PROFILE_ZONE()`, `ENGINE_PROFILE_ZONE_NAMED(name)`, `ENGINE_PROFILE_ZONE_TEXT(text, size)`, `ENGINE_PROFILE_FRAME()`, `ENGINE_PROFILE_FRAME_NAMED(name)`, `ENGINE_PROFILE_THREAD(name)`, `ENGINE_PROFILE_PLOT(name, value)`, `ENGINE_PROFILE_MESSAGE(text, size)`, `ENGINE_PROFILE_ALLOC(ptr, size)`, `ENGINE_PROFILE_FREE(ptr)`; `profiling::enabled()`, `profiling::connected()`.

**Rules.** One `ENGINE_PROFILE_ZONE` per scope (it declares a local). A zone per system and per pass, not per element; per-job zones exist in the job system because a job is the unit of scheduling. Plots for the counts a frame budget tracks. Worker threads name themselves.

**Instrumented today.** Job workers (thread names, a zone per executed job), protocol dispatch, document store load and save.

**Build.** `cmake/EngineProfiling.cmake` fetches Tracy v0.14.1 (BSD-3, `third_party/LICENSES.md`) when `ENGINE_TRACY` is on; the `msvc-release` and `linux-gcc-release` presets turn it on, debug presets leave it off, so both modes compile in CI. Tracy's headers are included as system headers so the engine's warning policy applies only to engine code.

**Depends on.** `base`; Tracy when enabled.

**Testing.** `tools/dev.ps1 test -Filter profiling` compiles every macro in whichever mode the preset selects and checks `enabled()` and `connected()`.

**Performance notes.** Off: nothing. On, not connected: an atomic load per zone. Connected: Tracy's own overhead, about 20 ns per zone. Memory allocation events are not wired into `core/memory` yet; that hook arrives with the mimalloc backend so both change once.
