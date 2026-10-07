# What to run, and when

The rule is in [AGENTS.md](../../AGENTS.md#what-to-run-and-when); this page is why it is the rule and
what it was measured against.

## The two places a change is tested

| Where | Who | What runs | Why there |
|---|---|---|---|
| **Before a branch is handed over** | whoever wrote it | `tools/dev.ps1 test -Affected` in `msvc-debug`; `tools/dev.ps1 lint` and `docs` (both are in that run); `tools/linux-build.ps1 -Preset linux-clang-debug -Test -Filter <the same modules>` when C++ changed, and then `tools/linux-build.ps1 -Prune` ([why](local-linux.md#volumes-that-outlive-their-checkout)) | It answers "did I break what I touched, and what is built from it". It does not answer "does the tree still pass", and is not asked to. |
| **At the merge** | whoever merges | the three Windows suites (`msvc-debug`, `msvc-minimal`, `msvc-no-ecs`), the GCC container, the GPU server (`tools/remote-build.ps1 -Host titanxp -Preset linux-server -Test`), and the frame budgets (`tools/frame-budget.ps1`, `msvc-release` on the RTX 5090; [below](#frame-budgets)), on the branch rebased onto `main`; then `tools/linux-build.ps1 -Prune` in the gate worktree | It is the only place the integrated tree exists, the only place the capability graph's two off-configurations are exercised, and the only place a second GPU and a second compiler see the change. |

A branch's author does not run the three suites. Until 2026-10-03 every agent did, as its last
step, and then the merge ran them again on the rebased branch: the same hour of machine twice, the
first of them on a tree that was already out of date by the time it finished.

**Several branches that are ready together are gated together.** They are rebased onto each other
in the order they will merge and the gate runs once, on the tip. If it is red, the branches are
gated one at a time to find which; that is the price of a failure and not of every merge. A
follow-up commit that changes only a test or a page is gated by the tests it changes, in the
presets that build them, not by the whole gate again.

## What "affected" means

`tools/dev.ps1 test -Affected [-Base main]` (the rule is `tools/lib/Affected.psm1`, tested by
`tools.affected`; `tools/dev.ps1 affected` prints the set without running it):

- A file that differs from the base — committed since the merge base, staged, unstaged or
  untracked — belongs to the module whose directory holds it (`build/<preset>/modules.json`).
- The affected set is those modules and every module that depends on one, transitively.
- **A changed capability also selects every module above the layer its directory is in.** A host
  and a test target link the capabilities they carry without the module depending on them
  (`renderer`'s tests link `sky` and `terrain`), and the module graph does not record what a test
  links. Until it does, this is the conservative reading.
- The build system, `tests/support`, vendored code, the schema compiler and committed content are
  built into or read by everything: a change there runs everything, and the run says why.
- Documentation selects nothing; the lint, the licence check (`lint.licenses`) and the
  documentation check run in every case.
- A module's tests are the test named after it, its bench's smoke run (`bench.<module>`), and a part
  of its tests registered as a test of its own, `<module>.<part>` — so far `engine_view.frame_loop`,
  whose three flights would double `engine_view`'s run.

On the tree of 2026-10-03 (70 modules, `msvc-debug`, serial, with the GPU lock free):

| A change in | Modules whose tests run | About |
|---|---|---|
| `apps/engine_view` | 1 | 3.5 min |
| `systems/renderer` | 5 (renderer, world, scene_collision, engine-host, engine-view) | 10 min |
| `domain/gfx` | 10 | 12 min |
| `domain/sky`, `domain/physics`, `domain/nav` (capabilities) | 15 or 16 | 20 min |
| `core/math` | 49 | most of the suite |
| the whole suite | 70 | 35 min |

The times are the sum of those tests' own times in a gate's log, not a quiet measurement.

## The gates the rules promised

Four accepted rules had no machinery behind them until 2026-10-07 (roadmap T11, T20, T46, T48).
Where each runs is chosen by what it costs and what it needs.

### The frame loop's allocations

`engine_view.frame_loop` (every preset that builds `engine_view`; [renderer](../subsystems/renderer.md#the-frame-loop-allocates-nothing))
flies the overlook, the erg's walk and the endless desert, each made small enough for a debug
build, twice, and fails on any engine allocation or device buffer the frame's thread makes in the
second flight. It needs a GPU, so it runs wherever the suites do and skips on a hosted runner as
every GPU test does. About 220 s in `msvc-debug` and 60 s in `msvc-release` on the RTX 5090; an
`-Affected` run that reaches `engine_view` includes it. **What its first run found**: one engine
allocation in every frame of every scene (the GPU timer's read-back, fixed), two more a frame that
were engine-view's own measurement (now left out), and on the endless desert's first flight 57
allocations and 6 device buffers while its lists grew to the path's high-water mark — none in the
second.

### Frame budgets

`tools/frame-budget.ps1` flies `content/budgets/frame-budgets.json`'s runs under the GPU lock and
fails a pass over its budget on a quiet machine ([bench](../subsystems/bench.md#frame-budgets) has
the file, the tolerance and the re-baseline). **It runs at the merge, once per batch, on the RTX
5090**, after the suites and on the same rebased tip, in `msvc-release`: about **13 minutes** of flying for its five runs once the derived-data cache holds the scenes' terrain and meshes (2026-10-07: 122, 245, 142, 142 and 144 s, each run's wait for a quiet machine included), plus whatever each run waits for the GPU lock and, up to `-WaitQuiet` (600 s) each, for a quiet machine; a cold cache adds the erg's and the overlook's builds, about 3 minutes, once per checkout. A batch The overlook run also needs the Khronos samples in `content/samples/` (`tools/fetch-samples.ps1`, or a copy from a checkout that has them), which a fresh gate worktree does not have: without them engine-view stops at the skyscraper's glTF and the run fails in three seconds, as the first merge gate that ran it found on 2026-10-07.
whose gate it fails does not merge until the pass is back under budget, behind a quality tier, or
re-baselined with a reason the reviewer accepts (ADR-0018). Not on an author's branch — the author
may run one run of it (`-Run <id>`) when a change is meant to move a number — and not nightly: the
desktop has no nightly runner and the hosted ones have no GPU; a nightly with significance tests
is T47's.

**What its first run found** (2026-10-07, `msvc-release`, the budgets seeded an hour earlier by the same script, `-Rebaseline`, from the same tree): every run within budget, five of five — totals at the median and p99 of 0.155 / 0.345 ms (overlook, 1080p), 0.729 / 1.344 (erg walk, 1080p), 3.484 / 3.964 (erg walk, surround), 1.610 / 2.602 (endless, 1080p, maps) and 4.362 / 5.498 (endless, surround, traced) against budgets of 0.155 / 0.358, 0.724 / 1.214, 3.482 / 4.010, 1.625 / 2.756 and 4.454 / 5.571; the erg walk's p99 at 1080p came closest to its limit (1.344 of 1.557). Four of the five ran on a machine the harness called busy — another agent's renderer tests and builds — which is the shared machine as it is; a pass is a pass on a busy one. **What seeding found**: the experiment pages the budgets were to come from agree with today's tree for the erg walk (total 0.724 against the page's 0.730 at 1080p; every surround median within 2%) and the endless desert's surround run (total 4.454 against 4.363, every pass within 8%), and **do not for the endless desert at 1080p with the maps**: [far ground](../experiments/far-ground-2026-10-03.md#the-cost) reads 2.980 / 5.103 ms, today's tree 1.625 / 2.756 on the same path, every pass lower (the cull 0.047 against 0.306). Seeded from the page, that budget would have passed a doubling, so every budget is the gate's own measurement and `sources` in the file says which page agreed. And the full-size endless flight's first pass makes 573 engine allocations on its frame thread, every one before frame 3,896 of 7,201 — lists reaching the high-water mark the 12 km path needs, the second half of the path none — which the gate reports in each summary's `frame_loop` and does not judge.

### Sanitizers

`linux-clang-asan` — `linux-clang-debug` with AddressSanitizer and UndefinedBehaviorSanitizer (`ENGINE_ASAN`, `ENGINE_UBSAN`, `cmake/EngineOptions.cmake`) — is a job of ci.yml's Linux matrix since 2026-10-07, on every push and pull request, on the hosted runners: no GPU, so the GPU tests skip there as they do in the other jobs, and what it sanitizes is everything else. Undefined behaviour is fatal (`-fno-sanitize-recover=all`), so a test that reaches it fails rather than printing a report into a log nobody reads; its test preset sets `ASAN_OPTIONS` to report leaks and initialization-order bugs and `UBSAN_OPTIONS` to print a stack. `tools/linux-build.ps1 -Preset linux-clang-asan -Test` runs the same here; it is not in `-Preset all`. It is not a merge-gate step: the merge does not wait for an ASan build, and a finding comes back from CI as a failed job. `msvc-asan` exists and is not in CI yet (T66): it strips `/RTC1` from the C++ flags only, and the C dependencies (SQLite, miniaudio, SDL) have not been built with it.

**What its first run found** (2026-10-07, the local container, `tools/linux-build.ps1 -Preset linux-clang-asan -Test`, 27 minutes cold): the first attempt compiled every object and failed at the first link — the image had no sanitizer runtime, which `clang` only recommends and the image installs without recommends; `libclang-rt-18-dev` is in the image and named in ci.yml now. The second ran the suite: **75 of 95 tests passed, 20 failed**, on four findings, each a roadmap row:

- **F13, tunables read before they are initialized**: a namespace-scope `tunables::` object read by another translation unit's dynamic initializer (ASan's initialization-order check) — the renderer's `terrain_staging_mib`, terrain's `skirt_spacings`, the world's `ring_max_deactivations`, scene collision's `collision_max_refreshes`, the audio system's `lod_hysteresis`, engine-view's `walk_jump` and `present_max_hz`. It stops every executable that links one of those modules at start-up — their tests and benches, `engine-host` (so `engine_cli` and `mcp_bridge`), `engine-view`, `engine_content` — which is most of the 20. Luau's own `FFlag` globals do the same in the scripting tests.
- **F14, a heap use after free in `domain/texture`'s tests**, unsymbolized because the image has no `llvm-symbolizer`.
- **F15, undefined behaviour in third-party code**: xatlas calls through a mismatched function type (`xatlas.cpp:4045`, the atlas tests), Recast's proximity-grid hash overflows a signed int (`DetourProximityGrid.cpp:45`, the nav tests), and the Luau `FFlag` order above; each is to be suppressed or excluded with its reason, not fixed in a fork.
- **F16, the fault probe crashes on purpose** (`core/platform`'s test of the fault reporting: a division by zero, a null write, a stack overflow, a bus error) and the sanitizers catch each crash before the engine's handler does, so the test reads their report instead of its own.

Until those rows are closed **the job reports and does not block** (`continue-on-error` for that preset in ci.yml), and T11 stays in progress; the other five Linux jobs are unchanged.

### Licences

`tools/license-check.ps1` holds `third_party/LICENSES.md` to ADR-0014's list, to every
`FetchContent_Declare` under `cmake/` and in the `CMakeLists.txt` files, to every directory vendored
under `third_party/`, and, given a configured build tree, to each fetched dependency's own licence
file. It is cheap (a second or two), so it runs everywhere: in `tools/dev.ps1 lint`, as the CTest
test `lint.licenses` in every preset (which `-Affected` always selects), and in CI's documentation
job, with no build. **Its first run found one dependency nobody had recorded**: Jolt Physics
(v5.6.0, MIT), downloaded by `cmake/EnginePhysics.cmake` since the physics capability landed and
absent from the record; its row is in now. Every other dependency's licence file agreed with its
row, and SQLite's amalgamation carries no licence file (the record says public domain, which its
source header states).

## Waiting costs more than it looks

An agent's cost is its number of tool calls times the size of its context, because every call
sends the whole conversation again. A wait written as a poll is a call every few seconds or
minutes for as long as the wait lasts, and a notification from a streaming watch is a whole turn
of whoever is watching. On 2026-09-30 three agents made 786, 610 and 914 tool calls over five to
nine hours each, most of that time waiting behind hour-long GPU-lock holds, and the coordinating
session was woken about forty times by progress lines; eleven wait loops from an abandoned
experiment were still polling six hours later. None of that was work. So:

- **Block on the command.** Run the build or the test in the foreground and let the call return
  when it does. If it will outlast a call, start it detached with its output in a log and make
  **one** wait that ends when the log does.
- **Size the wait to the run.** A wait that gives up before the run ends is a poll with a long
  period: on 2026-10-04 agents wrote "wait up to eight minutes" round `test -Affected` runs that
  take half an hour, and were woken four times for one run. Give the one wait a time limit of
  about twice the figure in the table below, not the tool's default. Where the harness can run a
  command in the background and wake the agent when it exits, that is the wait: start the build or
  the test that way with a limit of an hour or more, and write no loop at all.
- **One wait at a time, a minute or more between looks, and none left behind.** Before handing
  back, nothing the agent started is still running.
- **One line at the end.** A watch on a gate reports once, when every part of it has finished or
  one has failed — never per test and never per suite.
- **A long task is two agents.** Past about three hundred tool calls an agent's context is most
  of what it pays for; it writes a handoff (what stands, what is next, where the scratch files
  are) and a second agent starts from that with an empty context.

The GPU lock stopped being a reason to wait on 2026-10-01: a test holds it for the seconds it has
a device open ([gpu_lock](../subsystems/gpu_lock.md), ADR-0049), so a suite queues behind another
agent's test and not behind its hour.

### How long things take

So that a wait can be sized before it is written. Measured on the desktop on 2026-10-04, with one
to four agents building and testing beside each run, so these are what an agent will actually
meet and not quiet figures; CTest's own "Total Test time" in each case, builds not included.

| Run | Time |
|---|---|
| `test -Affected` that reaches the renderer (36 tests: `gfx`, `renderer`, `engine_view`, the capabilities above them) | 29 min |
| `test -Affected` that selects everything (a changed test scene, `cmake/`, a `core/` header): the `msvc-debug` suite, 91 tests | 36 min |
| `test -Filter engine_cli` alone (the end-to-end host tests) | 7 min |
| `msvc-minimal` suite, 59 tests | 13 min |
| `msvc-no-ecs` suite, 81 tests | 29 min |
| Linux container, warm, with a `-Filter` of a few modules | 1 to 7 min including the build |
| Titan Xp (`remote-build.ps1 -Test`), `engine_cli` alone / `renderer` and `engine_cli` | 6 min / 24 min |
| The whole merge gate (three Windows builds and suites, the GCC container, the server), in parallel | about 2 h |
| The frame budgets (`tools/frame-budget.ps1`, five runs, `msvc-release`, warm cache; 2026-10-07) | 13 min, plus lock and quiet waits |
| `engine_view.frame_loop` alone (2026-10-07) | 220 s in `msvc-debug`, 60 s in `msvc-release` |

A first build in a fresh worktree comes on top of these, and a fresh worktree's first container
run compiles every third-party dependency ([local Linux builds](local-linux.md) has those times).
When a figure here is found wrong by more than a third, correct it in the change that found it.
