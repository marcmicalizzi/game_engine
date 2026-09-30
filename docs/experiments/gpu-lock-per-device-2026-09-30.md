# The GPU lock per device: a suite's holds before and after (2026-09-30)

**Question.** How long does a full `msvc-debug` suite hold the machine-wide GPU lock when every test takes it itself for as long as it has a GPU device open ([gpu_lock](../subsystems/gpu_lock.md), [ADR-0050](../adr/0050-tests-take-the-gpu-lock-per-device.md)), against the old way of one hold around the whole suite — and do two suites started together interleave and both pass?

**How.** Wall time from the script that started the suite; holds from the hold log (`ENGINE_GPU_LOCK_LOG`, one JSON line per hold with its length and how long it waited), counting only holds of the machine's lock, `D:\workspace\gpu.lock` — the suite's own tests of the lock write scratch locks of their own, which are not counted. The machine's state — CPU and GPU busy, memory, who held the lock — was sampled at each end of each run.

**Machine.** The development desktop: i9-10980XE (18 cores, 36 threads), 64 GB, RTX 5090, Windows 11, `msvc-debug`. **Shared, and busy with GPU work that was not this change's**: other engine agents were building and testing in their own worktrees, and during the runs below they took the GPU lock for their own captures, measurements and GPU tests dozens of times — including four holds of about an hour each around **whole suites**, the old way ("sky: three Windows suites", and merge gates of three suites each). That is the situation this change is for, and it is in the numbers: every wait below was for one of them, for a sibling suite of this measurement, or for the crash described under "What it found".

## Before: one hold around the suite

`tools/gpu-lock.ps1 run -Exec "tools/dev.ps1 test -Preset msvc-debug"` on the code before this change (`be74b9f`).

| | |
|---|---|
| wall time | 2,045.6 s |
| lock held | 2,045.6 s, in **one** hold |
| tests | 88, 87 passed; `tools.docs_gate` failed on an msys `stat` error ("Function not implemented") inside a throwaway repository — unrelated to the GPU and to this change, and it passed in every later run |
| machine at start | CPU 19% busy, GPU 49% busy with 30.5 of 32.6 GB in use by another process, 23 GB of commit free, the lock free |
| machine at end | CPU 21%, GPU 5% with 6.0 GB in use, 44.6 GB of commit free, the lock free |

The tests that open a GPU device, and how long each ran: `gfx` 43.6 s, `renderer` 167.4 s, `engine_cli` 332.7 s (its engine-hosts render in a handful of cases), `engine_view` 184.6 s, `mcp_bridge` 12.5 s — 741 s of test executables, of which only the device lifetimes need the lock. Everything else — `tissue` 177.8 s, `engine_content` 212.7 s, `terrain` 94.3 s, the tools' PowerShell tests 303 s — held the GPU for nobody.

## After: every test takes it itself

`tools/dev.ps1 test -Preset msvc-debug`, unwrapped, with this change (at `9e00c4d` without its last two fixes — the release moved ahead of the instance's teardown, and the adapter enumeration's volk table — so it met the crash below once).

| | |
|---|---|
| wall time | 2,363.5 s |
| lock held | **301.5 s, in 195 holds** — 15% of the wall time, against 100% |
| longest hold | 29.6 s (one `renderer` test case's device) |
| holds by executable | `engine_renderer_tests` 82 holds, 162.0 s; `engine-view` 57, 97.8 s; `engine_gfx_tests` 51, 28.8 s; `engine-host` 5, 13.0 s (and one more that crashed, below) |
| waited | 240.3 s logged, in three waits: `gfx` 15 s and `renderer` 60 s behind another agent's "world tiles" GPU tests, an `engine-host` 165 s behind the crashed host's lock and then another agent's "sky: erg captures"; and 165 s more by the host that crashed, behind a third "world tiles" run, which its missing log line does not count |
| tests | 89 of 89 passed, none skipped |
| machine at start | CPU 8% busy, GPU 6% with 5.8 GB in use, 44.7 GB of commit free, the lock free |
| machine at end | CPU 14%, GPU 3% with 5.6 GB in use, 42.8 GB of commit free, the lock free |

The wall time is longer by the time the suite spent queued behind other agents' GPU work — 405 s of it — which the old way would not have paid, because under the old way those agents queued behind this suite instead, for all of its 34 minutes. The machine was the same machine, with the same kind of company, but not the same company: the two wall times are not a measurement of the change's own overhead. That was measured on its own: a take and a release of a free lock cost 2.28–2.35 ms in `msvc-debug` (`engine_gpu_lock_probe cycles 200`, three runs, a Linux container build beside it, so an upper bound), against 1.6 µs with the switch off — 195 holds are about half a second of a 39-minute suite.

## What it found

An `engine-host` in `engine_cli`'s first render test took the lock, answered its requests, and exited — **without releasing it**, and without the hold's log line. It had crashed on exit, with an access violation, in the Vulkan instance's teardown, and it does so without this change too: `gpu.adapters` beside an open device left volk's process-wide instance table pointing at the adapter enumeration's destroyed instance, and the device's own teardown went through it. The end-to-end test does not read the host's exit code, so nothing had ever said so. With the lock, the crash left the GPU spoken for until the lease ran out, and the next test's host waited it out. Two fixes, both in the change: the device releases the lock as soon as `vkDestroyDevice` returns, before the instance's teardown, so a crash there cannot leave the lock behind; and enumeration puts the live device's instance back ([gfx](../subsystems/gfx.md), "One live device per process"), with a test. Replayed afterwards against a scratch lock, the seven render cases took five holds, left no lock, and nothing waited.

## Two suites at once

`msvc-debug` and `msvc-no-ecs` started within the same second from two build directories of one worktree, both unwrapped, each with its own hold log. Twice: the first run was stopped part way, and why is the second finding of this page.

**First run** (14:33:46 UTC, at `5061066`): the machine was the busiest it was all day. Besides these two suites, four other holders took the lock during it — another agent's captures and measurements ("sky: cost at 1080p and surround3", "sky: resolve cost by part at surround", "sky: three Windows suites", "sky: cost by day"), a third agent's "world-tiles" smoke runs, a fourth's "sand third pass" GPU cases, and from 15:37 a **merge gate holding the lock around three whole suites**, the old way. The two suites' holds never overlapped and took turns eight times — in the middle of `gfx`, twice, and in the middle of `renderer`:

| from (UTC) | suite | executable | holds | waited before the first |
|---|---|---|---|---|
| 14:51:25 | no-ecs | `engine_gfx_tests` | 48 | 557 s |
| 14:53:28 | debug | `engine_gfx_tests` | 40 | 692 s |
| 14:58:14 | no-ecs | `engine_gfx_tests` | 4 | 330 s |
| 15:02:45 | debug | `engine_gfx_tests` | 12 | 271 s |
| 15:07:38 | no-ecs | `engine_renderer_tests` | 50 | 165 s |
| 15:10:35 | debug | `engine_renderer_tests` | 82 | 60 s |
| 15:14:29 | no-ecs | `engine_renderer_tests` | 32 | 330 s |
| 15:29:13 | no-ecs | `engine-host` (engine_cli) | 5 | 661 s |
| 15:29:34 | debug | `engine-host` (engine_cli) | 1 | 480 s |
| 15:37:40 | no-ecs | `engine-view` (engine_view) | 1 | 480 s |

By the time it was stopped (about 15:50, 76 minutes in, both suites in `engine_view` with five tests each to go) the `msvc-debug` suite had held the lock for **230.6 s in 135 holds** and waited **1,773 s**; `msvc-no-ecs` had held it for **225.5 s in 140 holds** and waited **2,568 s**. Every test that had finished had passed, except `msvc-debug`'s `engine_cli`, **skipped** at 1,801 s: one of its engine-hosts had waited 998 s behind the merge gate when the test's half hour ran out, and ended with the line in [gpu_lock](../subsystems/gpu_lock.md#waiting-behind-a-long-holder), which the test executable passed on as its own exit 75 — the give-up path, taken for real.

It was stopped because of what it showed next: both suites' `engine_view` tests had started an `engine-view` that **opened its window and then waited** for the lock in `Device::create`, whose sleep pumps no messages, so the owner found two windows marked "Not responding" on his desktop for six and fifteen minutes. The fix — a Vulkan window takes the lock before it appears ([gpu_lock](../subsystems/gpu_lock.md#windows)) — is `44994af`.

**Second run, on the final code** (`3f6c5b7`): `msvc-debug` and `msvc-no-ecs` together from 16:31 UTC, and `msvc-minimal` beside them from 17:20 — the three suites a gate runs, all at once. The afternoon had not quietened: from 16:42 to about 17:50 another agent held the lock around **three whole suites of its own** ("sky: three Windows suites", the old way), then a merge gate took it again for its GPU tests, then a measurement, another agent's tests and a "world-tiles" flight.

| | `msvc-debug` | `msvc-no-ecs` | `msvc-minimal` |
|---|---|---|---|
| wall time | 7,041 s | 7,022 s | 4,100 s |
| tests | 89: 86 passed, 3 skipped | 79: 76 passed, 3 skipped | 58: 57 passed, 1 skipped |
| skipped (gave up on the lock) | `gfx`, `renderer` (each 30 min behind "sky: three Windows suites"), `engine_view` (its engine-views' waits behind the gate, the measurement, the tests and the flight added up to its half hour) | the same three | `engine_view`, the same way |
| lock held | 94.3 s in 39 holds | 68.8 s in 54 holds | 149.9 s in 148 holds |
| waited, in holds that got the lock | 1,801 s | 2,492 s | 3,167 s |
| `tools/dev.ps1 test` | exit 75, naming the three and printing each "gave up waiting ... held by 'claude-engine' (pid 41816): sky: three Windows suites" line | the same | exit 75 |
| machine at start / end | CPU 15% / 18%, GPU 4% / 1%, the merge gate holding / free | CPU 15% / 32%, GPU 4% / 1% | CPU 17% / 27%, GPU 6% / 1%, "sky" holding / free |

Nothing failed and nothing hung: every test that could not get the GPU within its half hour said so and was skipped, and CTest's own timeout ended nothing. The three suites' holds never overlapped one another, and took turns 25 times. And the window fix was visible in the middle of it: at 18:12, with the three suites' `engine_view` tests all waiting behind "sky", there were three `engine-view` processes of this worktree alive, every one with no window (`MainWindowHandle` 0) and responding.

**The skipped tests, again, all three presets at once** (from 19:52, when a merge gate's hour around three suites let the lock go): `msvc-debug`'s and `msvc-no-ecs`'s `gfx`, `renderer` and `engine_view`, and `msvc-minimal`'s `engine_view`, started together with `tools/dev.ps1 test -Filter`. **All seven passed**, in 947, 920 and 766 s. Their 399 holds — 334.3 s, 280.6 s and 69.6 s of lock — took turns 34 times and never overlapped; the waits, 495.5 s, 540.6 s and 645.4 s, were for one another. So every test of the three suites passed on the final code: in the suite run, or in this one.

**What a suite's wall time is when it has to queue.** On a machine where other work holds the GPU, a suite's wall time is its own time plus its GPU tests' queue, and the queue is set by the other holders, not by this change. In the first run it was 30 to 43 minutes of 76. In the second, with an hour-long three-suite hold in the middle, it was most of the run: `msvc-debug` waited about 90 of its 117 minutes (30 in waits that got the lock, 60 in the two that ran out and the tail of a third), `msvc-no-ecs` about 97 of 117, `msvc-minimal` about 57 of 68. The old way does not remove that wait; it moves it — to before the suite starts (a wrapped suite waits for the lock once, then holds it for the whole run), or onto everybody else (while it holds it). What this change makes different is who waits for whom: a CPU-only test never waits, a GPU test waits only for the device it is about to open, and an hour-long holder costs a run the GPU tests that could not wait that long, named and skipped, rather than the whole run or a failure.

## What it settles, and what it does not

- **The hold.** A full `msvc-debug` suite needs the GPU for about five minutes of its thirty-odd (301.5 s in 195 holds, the longest 29.6 s), and now holds it for that and no longer. The other 85% of the suite runs beside whatever else the machine's GPU is doing.
- **Interleaving.** Two and three suites at once, and a dozen other holders, took turns without an overlap, between executables and inside them; the three presets' GPU tests run together took turns 34 times in a quarter of an hour and all passed.
- **Waiting is not failing.** Eight tests over the two runs waited their half hour and ended as named skips with the holder in the line; none failed and none hit CTest's timeout.
- **What it cannot fix.** The long waits were behind holds of about an hour around whole suites — other agents and gates still doing it the old way, which the per-device lock cannot shorten. The per-device lock makes those holds unnecessary; it is the briefs and the gate that have to stop taking them. Until they do, a suite run beside one loses its GPU tests to skips and has to run them again.
- **Not measured:** a quiet machine. Every run here had company, so none of the wall times is the suite's own; the hold figures are, because a hold is timed from the lock file this process wrote to the one it deleted.

