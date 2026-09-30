# ADR-0050: Tests take the GPU lock per device, at device creation

- **Status:** Proposed
- **Date:** 2026-09-30
- **Plan references:** docs/plan/09-testing-profiling.md; docs/plan/11-performance-principles.md §11.8 (measurements on a shared machine)
- **Docs touched:** `docs/subsystems/gpu_lock.md` (new), `docs/subsystems/bench.md` ("The GPU lock"), `docs/subsystems/gfx.md` ("Device"), `docs/subsystems/platform.md` (`Process::wait`), `docs/ci/self-hosted-runners.md`, `AGENTS.md`, `docs/experiments/gpu-lock-per-device-2026-09-30.md`

## Context

The development machine's one GPU is shared by several engine agents, a Blender agent and the owner's own work, under one protocol, `D:\workspace\GPU-LOCK.md`: a JSON file whose presence means the GPU is taken. The test suites were among the things that take it, and since nothing finer existed they took it around the whole command — `tools/gpu-lock.ps1 run -Exec "tools/dev.ps1 test ..."` — so a merge gate or an agent's final check held the GPU for three suites, about 63 minutes (1,776, 602 and 1,383 s on 2026-09-29), of which perhaps six to ten minutes used it. On the evening of 2026-09-29 three holders queued behind each other for most of it, and the owner asked why the lock is held so long.

What was wanted: the lock taken by the process that opens a GPU device, when it opens it, and released when it is done with it; a CPU-only test never touching it; a suite's holds several short ones that other holders interleave with; nobody having to wrap the suite, or able to forget to; the owner's own interactive runs unaffected — he never takes the lock and must not start waiting on it. The benchmark harness already took the lock from C++ (`--gpu-lock`), so there was one protocol with a PowerShell and a C++ implementation, and it had to stay one.

The alternatives:

- **Keep wrapping.** The status quo, and the problem.
- **Take it in the test helper** (`gfx_test::open_device`). Misses the renderer's own rigs, and every child process an end-to-end test starts — `engine-view`, `engine-host` (directly, or under `engine-cli` and `engine-mcp`) — which opens its device itself.
- **CTest's `RESOURCE_LOCK`, or a wrapper script per GPU test.** `RESOURCE_LOCK` orders tests inside one `ctest` invocation and knows nothing of a second suite, another agent or the Blender agent. A per-test wrapper holds for the whole executable, CPU-only cases included, and has to be told which tests use the GPU.
- **Hold from the first device to process exit.** One hold per executable, but the keeper that refreshes the lease then outlives the last device, and a process that uses the GPU briefly and then computes for minutes keeps everyone waiting.
- **Take it in device creation, behind a switch the test environment sets.** Every process opens its device through `gfx::Device::create`, so one place covers every door, including children.

## Decision

1. **`gfx::Device::create` takes the machine-wide GPU lock just before `vkCreateDevice`, and `Device::destroy` releases it as soon as `vkDestroyDevice` returns — when `ENGINE_GPU_LOCK_ON_DEVICE=1`, and not otherwise.** The shared test main sets that switch for every test executable unless it is set already, and a test's children inherit it; nothing else sets it, so the owner's and agents' interactive sessions never take the lock. `0` in the environment turns it off.
2. **The protocol's C++ implementation moves from `foundation/bench` to a module of its own, `foundation/gpu_lock`**, which `bench` and `gfx` both depend on, so device creation does not link the benchmark harness. The file format and rules do not change; `tools/lib/MachineLock.psm1` remains the other implementation.
3. **One hold per process, counted by device**: the first device takes the lock, later ones share it, the last releases it. The file carries a two-minute lease, refreshed every 30 s by a keeper thread that starts with the hold and is joined before the release returns.
4. **Children**: while a process holds the lock it sets `ENGINE_GPU_LOCK_HOLDER` to its pid, so a child finds the lock held for it and neither waits nor releases; the writer releases. A child watches its parent's hold and, if the hold goes while the child still has a device, takes the lock in its own name when it is free or expired. `tools/gpu-lock.ps1 run` already sets the same variable, so a wrapped suite finds the lock its own everywhere, as before.
5. **Giving up is a distinct outcome, not a failure**: every wait in a test's process tree ends at one absolute deadline, the test's start plus `ENGINE_TEST_GPU_LOCK_WAIT_S` (1,800 s); a process whose wait runs out prints who held the lock and exits with 75 before it used the GPU; `platform::Process::wait()` ends a parent that takes part in the lock with 75 when a child does; CTest reports 75 as a skip (`SKIP_RETURN_CODE`) for the tests that can wait, whose `TIMEOUT` is 600 s of work plus the 1,800 s of queue; and `tools/dev.ps1 test` exits 75 and names the tests that did not run.

A piece of code complies when it opens GPU devices only through `gfx::Device`, never takes the lock around a test run, and never treats exit code 75 as anything but "gave up waiting for the GPU lock".

## Consequences

- A suite holds the GPU for the device lifetimes of the tests that use one, and other holders get it between them; nobody has to remember the wrapper. The measured numbers are in `docs/experiments/gpu-lock-per-device-2026-09-30.md`.
- A library call, `Device::create`, can end the process (exit 75), and `Process::wait()` can end its caller. Both only with the switch on, which is to say only in a test's process tree; both are documented where they are declared. A process that gave up did nothing on the GPU, so nothing is left half done beyond what an early exit leaves anyway.
- Tests that can wait for the lock report a hang after 40 minutes rather than 10 (end-to-end) or 25 (the others): the price of letting a test sit in a queue rather than fail beside one.
- A gate that meets a long holder (a diffusion batch, an hour of benchmarks, an old-style wrapped suite) ends with skipped tests and exit 75 rather than a result; it runs them again. It does not report a pass.
- Two processes of one test that need a device at the same time and share no holding ancestor would serialise, and skip at the deadline. No test does this; a test that needs it holds a device in the parent first, or is reworked.
- Wrapping a test run in `tools/gpu-lock.ps1 run` still works and is no longer needed; the instructions that asked for it are gone.

## Revisit when

- A test needs two device-owning processes alive at once that do not descend from a holder.
- GPU-LOCK.md changes — a queue, a shorter poll, a different rule for back-to-back holders (rule 9) — or another tool starts taking the lock per job rather than per batch.
- The engine opens a GPU through something other than `gfx::Device` (a second backend's device, a compute runtime), which would need the same hold.
- A self-hosted runner shares its GPU with other work and should take part: it needs the lock's directory, and nothing else.
