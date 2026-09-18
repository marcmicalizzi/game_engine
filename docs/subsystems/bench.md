# bench (foundation)

**Purpose.** The micro-benchmark harness (ADR-0011; docs/plan/11-performance-principles.md §11.8): registration, calibrated timing, statistics, tunable sweeps, and reports as a table and as JSON lines, so "is this faster?" is a command. Every module keeps its benchmarks in `bench/` and `engine_module_bench()` builds one executable per module with `run_main` as its entry point.

**Model.**
- `ENGINE_BENCH(ident, "module.topic.case")` and `ENGINE_BENCH_ARGS(ident, name, 16, 256, ...)` define and register a body. The body sets up outside the loop, runs `while (state.keep_running()) { ... bench::keep(result); }`, and reports `state.set_items()` / `set_bytes()` per iteration for rate columns. `pause_timing`/`resume_timing` exclude setup inside the loop. `bench::keep` forces a value to be materialized so the optimizer keeps the work that produced it.
- The runner calibrates an iteration count that fills `min_time_ns` (default 100 ms), runs `warmup_repeats`, then `repeats` (default 7) timed repeats of that count, and reports the per-iteration median, minimum, maximum, mean, and standard deviation. `--smoke` runs one iteration once, for CTest. A fixture built once for a whole bench file (a corpus, a world) sizes itself from `bench::smoke_mode()`, so the smoke run stays seconds long in release builds too.
- `--sweep=<tunable>=<v1;v2;...>` runs every matching benchmark once per value and prints a relative column against the first value, then restores the original; `--set=<tunable=value,...>` applies overrides for the run. This is how a tunable's effect is measured (experiment E3's input).
- Output: a table on stdout and, with `--json=<path>`, one JSON object per result appended after a header line carrying the CPU brand, logical CPU and cache-domain counts, build configuration, and time. `tools/dev.ps1 bench` runs every bench executable and writes `build/<preset>/bench/<module>.jsonl`.
- The runner pins itself to the first performance CPU and raises its priority unless `--no-pin`.

**Invariants (tested).** Smoke mode touches every matching benchmark exactly once; measured mode calls the body at least once for calibration, once per warmup, and once per repeat; sweeps set each value in order and restore the original even when a value is rejected; the JSON file holds one header and one object per result; filters accept `*` globs and plain substrings.

**Reading results.** Trust one machine's numbers for layout and traversal decisions; do not use them to choose cross-machine defaults (11 §11.8). Compare medians; treat a spread above a few percent as noise to investigate (background load, frequency scaling) before believing a difference. Benchmarks run in release builds; debug numbers are for smoke only.

**Public API.** `foundation/bench/bench.h`: `State`, `keep`, `clobber_memory`, `Registration`, `first_registration`, `glob_match`, `Stats`, `compute_stats`, `Options`, `Result`, `run`, `run_main`; macros `ENGINE_BENCH`, `ENGINE_BENCH_ARGS`. CMake: `engine_module_bench(NAME <module> SOURCES <files> [DEPS ...])` in `cmake/EngineBench.cmake`; option `ENGINE_BUILD_BENCH`.

**Depends on.** `base`, `containers`, `json`, `platform`, `time`, `tunables`.

**Testing.** `tools/dev.ps1 test -Filter bench` runs the harness tests and every module's smoke run (`bench.<module>`, label `bench`).

**Performance notes.** The harness's own overhead per iteration is one branch and one counter increment; timing is read only at the start and end of a repeat. Benchmarks with sub-nanosecond bodies measure the loop; give them real work per iteration.
