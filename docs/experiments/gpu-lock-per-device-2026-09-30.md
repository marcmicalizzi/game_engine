# The GPU lock per device: a suite's holds before and after (2026-09-30)

**Question.** How long does a full `msvc-debug` suite hold the machine-wide GPU lock when every test takes it itself for as long as it has a GPU device open ([gpu_lock](../subsystems/gpu_lock.md), [ADR-0050](../adr/0050-tests-take-the-gpu-lock-per-device.md)), against the old way of one hold around the whole suite — and do two suites started together interleave and both pass?

**Machine.** The development desktop: i9-10980XE (18 cores, 36 threads), 128 GB, RTX 5090, Windows 11, `msvc-debug` at `be74b9f` plus this change. Shared, as always: other engine agents were building and testing in their own worktrees, and at least one of them took the GPU lock for its own captures during these runs.

## Before: one hold around the suite

`tools/gpu-lock.ps1 run -Exec "tools/dev.ps1 test -Preset msvc-debug"` on the code before this change.

| | |
|---|---|
| wall time | 2,045.6 s |
| lock held | 2,045.6 s, in **one** hold |
| tests | 88, 87 passed; `tools.docs_gate` failed on an msys `stat` error ("Function not implemented") in a throwaway repository, unrelated to the GPU and to this change |
| machine at start | CPU 19% busy, GPU 49% busy with 30.5 of 32.6 GB in use by another process, 23 GB of commit free, the lock free |
| machine at end | CPU 21%, GPU 5% with 6.0 GB in use, 44.6 GB of commit free, the lock free |

The tests that open a GPU device, and how long each ran: `gfx` 43.6 s, `renderer` 167.4 s, `engine_cli` 332.7 s (its engine-hosts render in some cases), `engine_view` 184.6 s, `mcp_bridge` 12.5 s — 741 s of test executables, of which only the device lifetimes need the lock. Everything else — `tissue` 177.8 s, `engine_content` 212.7 s, `terrain` 94.3 s, the tools' PowerShell tests 303 s — held the GPU for nobody.
