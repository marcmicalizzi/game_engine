# bench (foundation)

**Purpose.** The micro-benchmark harness (ADR-0011; docs/plan/11-performance-principles.md §11.8): registration, calibrated timing, statistics, tunable sweeps, and reports as a table and as JSON lines, so "is this faster?" is a command. Every module keeps its benchmarks in `bench/` and `engine_module_bench()` builds one executable per module with `run_main` as its entry point.

**Model.**
- `ENGINE_BENCH(ident, "module.topic.case")` and `ENGINE_BENCH_ARGS(ident, name, 16, 256, ...)` define and register a body. The body sets up outside the loop, runs `while (state.keep_running()) { ... bench::keep(result); }`, and reports `state.set_items()` / `set_bytes()` per iteration for rate columns. `pause_timing`/`resume_timing` exclude setup inside the loop. `bench::keep` forces a value to be materialized so the optimizer keeps the work that produced it.
- The runner calibrates an iteration count that fills `min_time_ns` (default 100 ms), runs `warmup_repeats`, then `repeats` (default 7) timed repeats of that count, and reports the per-iteration median, minimum, maximum, mean, and standard deviation. `--smoke` runs one iteration once, for CTest. A fixture built once for a whole bench file (a corpus, a world) sizes itself from `bench::smoke_mode()`, so the smoke run stays seconds long in release builds too.
- `--sweep=<tunable>=<v1;v2;...>` runs every matching benchmark once per value and prints a relative column against the first value, then restores the original; `--set=<tunable=value,...>` applies overrides for the run. This is how a tunable's effect is measured (experiment E3's input).
- Output: a table on stdout and, with `--json=<path>`, one JSON object per result appended after a header line carrying the CPU brand, logical CPU and cache-domain counts, build configuration, and time. `tools/dev.ps1 bench` runs every bench executable and writes `build/<preset>/bench/<module>.jsonl`.
- The runner pins itself to the first performance CPU and raises its priority unless `--no-pin`.

**Invariants (tested).** Smoke mode touches every matching benchmark exactly once; measured mode calls the body at least once for calibration, once per warmup, and once per repeat; sweeps set each value in order and restore the original even when a value is rejected; the JSON file holds one header and one object per result; filters accept `*` globs and plain substrings; a measured run samples the machine once before and once after itself and a smoke run samples it not at all.

## Measuring on a shared machine

The development machine is not an idle benchmark rig. It runs GPU diffusion jobs that hold the whole card and most of its memory for minutes at a time, and several agents compile and test on it at once. A number taken beside those is an **upper bound** on the cost, not the cost — and the numbers that decided the first budgets were taken with no record of what else was running, so nobody can tell which of them were. The harness therefore records the machine's state around every measured run, says when it was busy, and can refuse to measure or wait.

**What is recorded**, at the start and at the end of a run (`foundation/bench/machine_state.h`):

| Field | Where it comes from | Absent when |
|---|---|---|
| `cpu_total_pct`, `cpu_own_pct`, `cpu_others_pct` | `GetSystemTimes` + `GetProcessTimes` on Windows, `/proc/stat` + `/proc/self/stat` on Linux, over a 250 ms window; "others" is total minus own, clamped at zero | the counters could not be read |
| `gpu_util_pct`, `gpu_memory_used_mib`, `gpu_memory_total_mib` | `nvidia-smi --query-gpu=...` when it is on PATH | no such tool: an AMD or Intel box, or a CI runner |
| `session_locked` | a `LogonUI.exe` process exists (Windows) | anywhere else — reported as `null`, never as "unlocked" |

**No process names, no command lines, no user names leave the sampler**, on purpose: the question is how loaded the box is, and the answer does not need to say by whom. The lock check walks the process list for one name and keeps a boolean.

`nvidia-smi` rather than NVML because the engine takes no new dependency for a measurement convenience ([08 §8.6](../plan/08-toolchain.md)) and the utility is already installed wherever the driver is; a machine without it reports nulls and everything else still works. The first GPU's line is the one read.

**The thresholds, and why those.** A run is *quiet* when other processes used no more than **10%** of the CPU and the GPU was no more than **20%** busy. Both are deliberately loose. An otherwise idle Windows desktop with a browser, a shell, an editor and the usual services sits at a few percent of 36 logical CPUs, and a desktop compositor driving three monitors keeps the GPU in the low teens, so a tighter bound would fire on a machine nobody is using and the flags would be ignored within a week. They are also loose enough to be *meaningful* in the other direction: one parallel `cmake --build` on this box is 40–90% of the CPU and a diffusion job is 100% of the GPU, so the things that actually move a benchmark are far above the line rather than near it. `--quiet-cpu=<pct>` and `--quiet-gpu=<pct>` move them when a particular measurement needs a stricter or looser rule; say in the write-up when you moved them.

An **unknown** field can never make a machine noisy. A box with no GPU reading is quiet as far as its GPU is concerned, because a rule that refused to measure on missing information would make `--require-quiet` useless on Linux CI and on every non-NVIDIA machine.

**What the flags do.**

```powershell
build/msvc-release/domain/sim/engine_sim_bench.exe --filter=sim.wheel.* --require-quiet
build/msvc-release/domain/sim/engine_sim_bench.exe --filter=sim.wheel.* --wait-quiet=7200
```

- `--require-quiet` measures nothing on a busy machine: it prints the state and exits **4**, which is a code of its own so a script can tell "come back later" from "this run failed" (2) and from a real failure.
- `--wait-quiet=<seconds>` looks every 5 seconds until the machine is quiet or the deadline passes, then runs either way and says on stderr which of the two happened and how long it waited. It never sleeps past the deadline. This is the flag to reach for before a session of measurements: start it, let the diffusion job finish, and the numbers come from the other side of it.
- `--smoke` does neither, and does not sample at all. A smoke run answers "does this benchmark run", CTest runs one per module, and a quarter of a second plus a process spawn per module would buy nothing there. Its JSON header says `"machine_state": null` rather than reporting a state it did not take.

**What a WARNING means for a write-up.** When either threshold was crossed at either end of a run, the table gets one line under it (on stderr when `--quiet` suppressed the table):

```
WARNING: other processes used 20.5% of the CPU and the GPU was 100% busy; these numbers are
upper bounds (cpu 20.6% (others 20.5%, own 0.2%), gpu 100% util 24864/32607 MiB, session unlocked)
```

It does not mean the numbers are wrong; it means they are **upper bounds** and that a quiet run would be the same or faster. A write-up may use them — sometimes the loaded case is the interesting one — but it has to say so: an experiment page that quotes a number from a warned run and does not carry a "Machine state" line is the failure this whole feature exists to prevent (see [docs/experiments/README.md](../experiments/README.md), and AGENTS.md's "Rules that are reviewed"). A *ratio* between two benchmarks measured in the same warned run survives far better than either absolute number, because both paid the same tax; a comparison against a number from another day does not survive at all.

**In the JSON.** The header line gains `machine_state` with a `start` and an `end` object of the fields above, each field `null` where it is unknown, and every result line gains `worst_others_cpu_pct` — the worst "others" CPU either sample saw — so that one line out of a `.jsonl` carries its own caveat without the reader having to find the header. The end sample only exists once the last benchmark has finished, which is why the whole file is written when the run ends rather than streamed: a run killed part way leaves no JSON, and its numbers were taken under whatever killed it anyway.

**Reading results.** Trust one machine's numbers for layout and traversal decisions; do not use them to choose cross-machine defaults (11 §11.8). Compare medians; treat a spread above a few percent as noise to investigate (background load, frequency scaling) before believing a difference — and read the run's `machine_state` first, because on a shared machine background load is the common answer. Benchmarks run in release builds; debug numbers are for smoke only.

**Public API.** `foundation/bench/bench.h`: `State`, `keep`, `clobber_memory`, `Registration`, `first_registration`, `glob_match`, `Stats`, `compute_stats`, `Options`, `Result`, `run`, `run_main`, `k_exit_not_quiet`; macros `ENGINE_BENCH`, `ENGINE_BENCH_ARGS`. `foundation/bench/machine_state.h`: `Tristate`, `MachineState`, `QuietThresholds`, `k_sample_window_ms`, `is_quiet`, `worst_of`, `describe`, `machine_state_json`, `warn_if_busy`, `MachineSampler`, `system_sampler`, `sample_machine_state` — the last three are the injection point, so a test drives the quiet and not-quiet paths with a fake instead of hoping the machine cooperates. CMake: `engine_module_bench(NAME <module> SOURCES <files> [DEPS ...])` in `cmake/EngineBench.cmake`; option `ENGINE_BUILD_BENCH`.

**Depends on.** `base`, `containers`, `json`, `platform`, `time`, `tunables`.

**Testing.** `tools/dev.ps1 test -Filter bench` runs the harness tests and every module's smoke run (`bench.<module>`, label `bench`).

**Performance notes.** The harness's own overhead per iteration is one branch and one counter increment; timing is read only at the start and end of a repeat. Benchmarks with sub-nanosecond bodies measure the loop; give them real work per iteration.
