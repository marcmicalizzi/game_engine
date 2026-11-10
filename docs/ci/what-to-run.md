# What to run, and when

The rule is in [AGENTS.md](../../AGENTS.md#what-to-run-and-when); this page is why it is the rule and
what it was measured against.

## The two places a change is tested

| Where | Who | What runs | Why there |
|---|---|---|---|
| **Before a branch is handed over** | whoever wrote it | `tools/dev.ps1 test -Affected` in `msvc-debug`; `tools/dev.ps1 lint` and `docs` (both are in that run); `tools/linux-build.ps1 -Preset linux-clang-debug -Test -Filter <the same modules>` when C++ changed, and then `tools/linux-build.ps1 -Prune` ([why](local-linux.md#volumes-that-outlive-their-checkout)) | It answers "did I break what I touched, and what is built from it". It does not answer "does the tree still pass", and is not asked to. |
| **At the merge** | whoever merges | the three Windows suites (`msvc-debug`, `msvc-minimal`, `msvc-no-ecs`), the apps' end-to-end tests in `msvc-release` (`engine_view`, `engine_editor`, `engine_host`, `engine_cli`, `engine_content`: a debug build leaves a dead stack frame intact, so the editor's dangling action map passed the debug window test and crashed the release binary at startup, 2026-10-10), the three Linux containers the hosted matrix runs (`linux-gcc-release`, `linux-clang-debug`, `linux-clang-minimal`), and the frame budgets (`tools/frame-budget.ps1`, `msvc-release` on the RTX 5090; [below](#frame-budgets)), on the batch stacked onto `main` in the persistent gate worktree, whose build directories and container volumes stay warm between batches ([how long things take](#how-long-things-take)). **The GPU server** (`tools/remote-build.ps1 -Host titanxp -Preset linux-server -Test`, [remote Linux builds](remote-linux.md)) **runs on the same stack beside the merge and does not hold it** (the owner, 2026-11-09): it is there for the lower-end hardware (a Pascal card with no mesh shaders or ray queries, a v2 CPU) and a full build on a real Linux machine, and its suite runs serially for about four hours against two for the rest of the gate (gate b57), so the merge goes ahead when the Windows half and the containers are green, and a red server leg opens a fix row at the top of the next batch | It is the only place the integrated tree exists, the only place the capability graph's two off-configurations are exercised, and the only place a second GPU and a second compiler see the change. |

A branch's author does not run the three suites. Until 2026-10-03 every agent did, as its last
step, and then the merge ran them again on the rebased branch: the same hour of machine twice, the
first of them on a tree that was already out of date by the time it finished.

**Several branches that are ready together are gated together.** They are rebased onto each other
in the order they will merge and the gate runs once, on the tip. If it is red, the branches are
gated one at a time to find which; that is the price of a failure and not of every merge. A
follow-up commit that changes only a test or a page is gated by the tests it changes, in the
presets that build them, not by the whole gate again.

**An agent's worktree is removed at the merge.** A session's agents work in throwaway worktrees under
`.claude/worktrees/`, and nothing removes them: on 2026-11-01 about a hundred of them, merged weeks
before, still held their builds. So whoever merges a branch removes its worktree once the branch is on
`main` (`git worktree remove --force <path>`, then `git worktree prune`; the branch stays), and a
worktree whose branch was never merged is listed for the owner, not removed. A harness worktree is
fresh every time, and a fresh worktree's first build is the slowest step of an agent's day, which is
what the slot pool is for.

### The slot pool (T75)

An early attempt at a persistent worktree per agent was refused by the harness (an isolated agent may
write only in its own worktree; one launched from the repository root may not switch into another).
That is a fact about the harness's own isolation, not about this arrangement: the coordinator keeps
four **slot worktrees**, `.claude/worktrees/slot-1` to `slot-4`, each with warm `build/msvc-debug` and
`build/msvc-release`, and **assigns one to each heavy agent by its brief**. The agent leaves its
harness worktree untouched, works in the slot on a branch of its own (`b57-<row>`), and the
coordinator resets the slot to `main` after the merge. A slot has no branch of its own: between agents
it is detached at a commit of `main`, so no two worktrees ever hold one branch.

`tools/slot.ps1` keeps that honest (exit 0 done, 1 refused with the reason, 2 usage):

| Command | What it does |
|---|---|
| `slot.ps1 list` | each slot's path, HEAD, branch or detached, dirty count, its build directories with the time of the newest file under each, and its owner |
| `slot.ps1 claim -Slot 2 -Owner S100` / `release -Slot 2` | writes or removes `build/slot-owner.json` (owner, time, branch; git-ignored with `build/`); a second owner is refused |
| `slot.ps1 reset -Slot 2 [-To <sha\|main>] [-Force]` | `git checkout --detach`, `build/` left in place; refuses a dirty tree, a process whose executable is under the slot (best effort: a compiler, a test run, an engine binary), and a claimed slot unless `-Force` |
| `slot.ps1 warm -Slot 2 [-Preset msvc-debug,msvc-release] [-Base <sha>]` | `tools/dev.ps1 build` for each preset inside the slot, `-Affected -Base` when a base is given; honours `CMAKE_BUILD_PARALLEL_LEVEL` |

`tools/slot.Tests.ps1` (CTest `tools.slot`) drives every command over a throwaway repository with two
worktrees, including the refusals. A slot is not an agent's own checkout to leave dirty: whoever
finishes commits or abandons the branch, and the coordinator releases and resets the slot.

What it saves, measured on 2026-11-08 (host i9-10980XE, 36 threads, `CMAKE_BUILD_PARALLEL_LEVEL=4`,
other agents building beside the run at 67 to 70% CPU, so upper bounds), in a fresh harness worktree
with ccache on and warm from earlier worktrees:

| Step | Time |
|---|---|
| `tools/dev.ps1 configure -Preset msvc-debug`, fresh worktree | 211 s |
| `build -Preset msvc-debug -Affected -Base <sha>` after a one-line change to `systems/volumetrics/tests/cloud_tests.cpp` (1,403 ninja steps for the 19 targets its dependents reach, most of them upstream modules a fresh tree has to build first), fresh worktree | 304 s (ninja's log: 1,140 objects, 673 s of compiler time, nearly all of it ccache hits) |
| of that, the shader step (`engine_shaders()`'s slangc compiles, not cached: about 40 SPIR-V outputs, about 350 s of slangc time, the four `visibility_resolve_*water*` variants alone 33 to 77 s each) | about 110 s of wall time with four jobs, over a third of the build |
| the same build again after another one-line change to the same file, same worktree | 12 s: one object, one link, no shader |
| an incremental release build of a one-commit change in a warm slot (coordinator, 2026-11-08) | 15 s |

So a fresh worktree costs about eight minutes of build before the first useful edit, a third of it
shaders that nothing caches, and a warm slot costs seconds. The shader step is the part ccache cannot
help with: roadmap T76.

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
- `test -Filter` narrows CTest only; its default build still builds the whole preset. It cannot
  be combined with `-Affected`. On a fresh worktree whose affected targets alone were built,
  a filtered follow-up can therefore compile hundreds of unrelated targets (650 additional
  build steps in W67's 2026-10-23 follow-up). Account for that build when choosing the command;
  the filter's small test count does not imply a small build.
- A module's tests are the test named after it, its bench's smoke run (`bench.<module>`), and a part
  of its tests registered as a test of its own, `<module>.<part>` — so far `engine_view.frame_loop`,
  whose three flights would double `engine_view`'s run.
- **It builds only what those tests need** (since 2026-10-20, roadmap T68): each affected module's
  library, test executable and bench, the apps among them, every app when the tools' own tests run
  (they start them; [local Linux](local-linux.md#build--affected)), and whatever those tests name with
  `add_dependencies` (`cmake --build --preset <p> --target <list>`; `tools/dev.ps1 build -Affected`
  does the same without the tests, and `tools/dev.ps1 affected` prints the list). A fresh worktree's
  first build is where that counts: measured on the 2026-10-20 tree in `msvc-debug`, a change in
  `domain/flora`, a capability and so everything above its layer, asks for 1,146 of the tree's 1,542
  compiles and 44 of its 128 links; one in `foundation/input` for 1,028 and 21; one in
  `apps/engine_cli`, whose tests start engine-host, engine-view and engine-content, for 954 and 7.
  The floor under all of them is the third-party code and `core/`, which everything links. In a
  tree that is already built, the saving is whatever else is out of date — main's changes after a
  rebase, say — which a whole build compiles and this does not; the change's own files are
  recompiled either way. What takes the floor out of a fresh worktree is
  [the compiler cache](local-linux.md#the-compiler-cache).

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
A GPU queue skip can exhaust the deadline of the **whole test executable**, not one late
case: tests/support/test_main.cpp sets one deadline at process start, inherited by its children.
On 2026-10-29, the 142-case viewer group reached that deadline twice while continuing to
complete cases on a shared GPU. Repeating the entire group under the same contention wastes
the completed work. After the normal affected build/run, a bounded follow-up may run the same
executable with explicit doctest ranges: use --count and --list-test-cases, fix --order-by=file,
and cover 1 through the reported count exactly once with non-overlapping --first/--last ranges.
Keep each range's result and verify that concatenating its ranged case list reproduces the full
list exactly once. Doctest 2.4.11's footer counts cases before range selection, so do not sum its
reported passed counts across ranges. Preserve
the default skip policy; separately registered cases such as engine_view.frame_loop still need
their normal run. This changes process grouping only, not assertions, the GPU lock, or its queue
budget. Do not wrap these runs in an outer GPU lock or report a skipped range as passed.

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

`linux-clang-asan` — `linux-clang-debug` with AddressSanitizer and UndefinedBehaviorSanitizer (`ENGINE_ASAN`, `ENGINE_UBSAN`, `cmake/EngineOptions.cmake`) — is a job of ci.yml's Linux matrix since 2026-10-07, on every push and pull request, on the hosted runners: no GPU, so the GPU tests skip there as they do in the other jobs, and what it sanitizes is everything else. Undefined behaviour is fatal (`-fno-sanitize-recover=all`), so a test that reaches it fails rather than printing a report into a log nobody reads; its test preset sets `ASAN_OPTIONS` to report leaks and initialization-order bugs (`check_initialization_order=1`, not strict: F13 below says why) and `UBSAN_OPTIONS` to print a stack, which `llvm-symbolizer` in the image and the job turns into function, file and line. Third-party files are excused from one named check each, every entry with its reason, by `cmake/sanitizer-ignorelist.txt` (F15 below); engine code never is. `tools/linux-build.ps1 -Preset linux-clang-asan -Test` runs the same here; it is not in `-Preset all`. It is not a merge-gate step: the merge does not wait for an ASan build, and a finding comes back from CI as a failed job. `msvc-asan` — `msvc-debug` with MSVC's AddressSanitizer — is its Windows half, in ci.yml's Windows matrix since 2026-10-09 (T66, below); MSVC has no UBSan, so the undefined-behaviour half is Linux's alone.

**What its first run found** (2026-10-07, the local container, `tools/linux-build.ps1 -Preset linux-clang-asan -Test`, 27 minutes cold): the first attempt compiled every object and failed at the first link — the image had no sanitizer runtime, which `clang` only recommends and the image installs without recommends; `libclang-rt-18-dev` is in the image and named in ci.yml now. The second ran the suite: **75 of 95 tests passed, 20 failed**, on four findings, each a roadmap row:

- **F13, tunables read before they are initialized**: a namespace-scope `tunables::` object read by another translation unit's dynamic initializer (ASan's initialization-order check) — the renderer's `terrain_staging_mib`, terrain's `skirt_spacings`, the world's `ring_max_deactivations`, scene collision's `collision_max_refreshes`, the audio system's `lod_hysteresis`, engine-view's `walk_jump` and `present_max_hz`. It stops every executable that links one of those modules at start-up — their tests and benches, `engine-host` (so `engine_cli` and `mcp_bridge`), `engine-view`, `engine_content` — which is most of the 20. Luau's own `FFlag` globals do the same in the scripting tests. **Closed 2026-10-09: not a defect; strict mode reports reads of initialized globals by design.** With `check_initialization_order=1` and `strict_init_order=0` nothing remains, in the tunables or in Luau. The registry in `foundation/tunables` is constant-initialized (`platform::SpinLock` has a constexpr constructor, `head` and `count` are constants), and a tunable's constructor walks the tunables already registered to refuse a duplicate name: a read of other translation units' globals whose initializers **have** run. Strict mode poisons every other unit's dynamically initialized globals for the whole of each unit's initialization, initialized or not, on the assumption that no initializer ever reads another unit's global; non-strict poisons only the units not initialized yet, which is the fiasco itself. So the preset keeps the check and drops `strict_init_order`, and a read of a global whose unit has not run yet still fails the job.
- **F14, a heap use after free in `domain/texture`'s tests**, unsymbolized because the image has no `llvm-symbolizer`. **Closed 2026-10-09: a defect in the test.** With `llvm-symbolizer` in the image (`llvm`, [local Linux builds](local-linux.md)) and in ci.yml's install step, the stack named `texture_file_tests.cpp`'s `too_long.levels.push_back(too_long.levels.back())`: an argument to `Vector::push_back` that refers into the vector, read after the push's growth freed the buffer — the use [containers](../subsystems/containers.md) forbids and `std::vector` permits. The test copies the level out first; `domain/texture` itself was not at fault.
- **F15, undefined behaviour in third-party code**: xatlas calls through a mismatched function type (`xatlas.cpp:4045`, the atlas tests), Recast's proximity-grid hash overflows a signed int (`DetourProximityGrid.cpp:45`, the nav tests), and the Luau `FFlag` order above; each is to be suppressed or excluded with its reason, not fixed in a fork. **Closed 2026-10-09** by `cmake/sanitizer-ignorelist.txt`, which `cmake/EngineOptions.cmake` passes to Clang as `-fsanitize-ignorelist` in a sanitizer build: `[function]` for xatlas's one file (OpenNL's matrix callbacks stored as base-typed pointers) and `[signed-integer-overflow]` for `DetourProximityGrid.cpp` alone, each with a sentence on what it hides and why it is the dependency's. Compile-time, because UBSan's run-time suppressions do not apply to a check compiled unrecoverable, and an unrecoverable report ends the process whatever the file says. The Luau entry was never needed: its `FFlag` report was F13's strict mode. The C dependencies are compiled by the image's `cc`, which is GCC even in the Clang presets, so the list is passed to Clang compiles only.
- **F16, the fault probe crashes on purpose** (`core/platform`'s test of the fault reporting: a division by zero, a null write, a stack overflow, a bus error) and the sanitizers catch each crash before the engine's handler does, so the test reads their report instead of its own. **Closed 2026-10-09**: under a sanitizer the module installs no handler at all ([platform](../subsystems/platform.md), "What it cannot catch"), so every kind failed, not four. The test now reads the one definition of that rule (`ENGINE_FAULT_UNDER_SANITIZER`, `core/platform/src/fault_report.h`), prints that the one-line checks are skipped and why, and asserts the other half instead: the probe still dies, and no engine line joins the runtime's report. Every other build checks the line as before.

**What the next runs found** (2026-10-09, the local container). The first, with `llvm-symbolizer` in the image and strict mode off, built cold and ran **85 of 96** (a test was added since): no initialization-order report anywhere, the platform, atlas, texture and nav tests on F14 to F16 as above, and seven more — terrain, its bench, renderer, world, scene collision, `engine_view` and `engine_content` — stopping on undefined behaviour F13's start-up stops had hidden: **F19**, the dunes' ripple shift for yesterday computed as `86'400 * speed_q16` in `int`, which overflows on any day whose wind beats 0.38 of the mean (`domain/terrain/src/dunes.cpp`; [terrain](../subsystems/terrain.md#the-time-function)). On every other build it wrapped, so the full-detail surface's ripples jumped at midnight; it is an `i64` now, and a test holds the next day's yesterday to the last second of the day. With the five fixed, **96 of 96 passed** (46 minutes 38 seconds warm, almost all of it the tests: `engine_cli` 606 s, `bench.physics` 329 s, `terrain` 256 s), and no sanitizer report appears in the log.

**The job blocks since 2026-10-09**: with those rows closed, ci.yml's `continue-on-error` for the preset is gone, and a sanitizer finding fails a push like any other failed test (T11, done). Until then it reported and did not block.

**`msvc-asan`, first built and run** (2026-10-09, this desktop, `tools/dev.ps1 configure`, `build` and `test -Preset msvc-asan`, with the RTX 5090, so the GPU tests ran too, which on the hosted runner they skip). It had never been built. The build's first link of `engine_ruins_tests` died with 0xC000009A, insufficient resources, beside other agents' builds with 15 GB of commit free; repeated once with 20 GB free, it built clean. MSVC 14.51 compiled the C dependencies (SQLite, miniaudio, SDL, flecs, volk) with `/RTC1` and `/fsanitize=address` together without a word, though its documentation calls the two incompatible, so `cmake/EngineOptions.cmake` now strips `/RTC1` from the C flags as it did from the C++ ones. The suite ran **95 of 96**, and the one was a finding: `systems/renderer/tests/vertex_id_tests.cpp` looped over `Quads{}.ids`, the member of a temporary, which standard C++ keeps alive for the loop and MSVC's code did not — ASan reported a stack use after scope of the compiler's temporary. GPU-only, so no Linux job could have seen it; the test names the object now, and the renderer's tests then passed under `msvc-asan` (1,021 s): **96 of 96**. The whole suite took 115 minutes beside a concurrent `msvc-debug -Affected` run and other agents' work, so read it as an upper bound: `engine_cli` 1,895 s and `renderer` 1,022 s, against 356 s and 355 s for the same tests in `msvc-debug` in the run beside it. It is in ci.yml's Windows matrix, blocking, since that run (T66, done); without a GPU there the renderer's cases skip, and `engine_cli` and the build are most of the job.

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
  command in the background and wake the agent when it exits, that is the wait for a session
  working alone: start the run that way with a limit of an hour or more, and write no loop. **A
  sub-agent of a coordinating session never does**: its turn ends while the command runs, and
  that end and the command's completion each wake the coordinator with the coordinator's whole
  context, the largest on the machine (2026-10-20: a night of such "interim" wakes cost a few
  percent of a day's budget). It starts the run detached (`Start-Process … -PassThru`, output to
  a log) and waits in the foreground with `Wait-Process -Id <id> -Timeout 570` a call, re-issued
  until the process is gone: a poll with a ten-minute period, the cheapest wait a sub-agent has.
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

So that a wait can be sized before it is written. Measured on the desktop on 2026-10-04 and, where a row says so, 2026-10-10, with one to four
agents building and testing beside each run, so these are what an agent will actually meet and
not quiet figures; CTest's own "Total Test time" in each case, builds not included.

| Run | Time |
|---|---|
| `test -Affected` that reaches the renderer (36 tests: `gfx`, `renderer`, `engine_view`, the capabilities above them) | 29 min |
| `test -Affected` that selects everything (a changed test scene, `cmake/`, a `core/` header): the `msvc-debug` suite | 36 min (91 tests, 2026-10-04); 42 to 49 min beside a gate's containers (99 tests, 2026-10-10) |
| `test -Filter engine_cli` alone (the end-to-end host tests) | 7 min |
| `msvc-minimal` suite | 13 min (59 tests, 2026-10-04); 17 min beside the other two suites (68 tests, 2026-10-10) |
| `msvc-no-ecs` suite | 29 min (81 tests, 2026-10-04); 45 min beside the other two suites (90 tests, 2026-10-10) |
| Linux container, warm, with a `-Filter` of a few modules | 1 to 7 min including the build |
| Titan Xp (`remote-build.ps1 -Test`), `engine_cli` alone / `renderer` and `engine_cli` | 6 min / 24 min |
| The whole merge gate on the persistent gate worktree (below; 2026-10-10, batch 13): the Windows half after its four incremental builds (the three suites side by side, then the budgets) / the three Linux containers, warm / the server | 62 min (52 of suites, bounded by `msvc-debug`, and 9.5 of budgets) / 28 min / 74 min, all three in parallel |
| The three Windows suites in series, the way a per-batch gate ran them before 2026-10-10 | about 2 h, plus a fresh worktree's four builds |
| The whole merge gate on gate b57 (2026-11-09; 112 tests on the server, 133 in a container): the Windows half (nine minutes of incremental builds, the three suites side by side with the release app checks, then the budgets) / the three containers / the server | 2 h 09 (1 h 53 of suites, which is `msvc-debug`'s serial run, 6,765 s of CTest: `engine_view` 1,536 s, the four `renderer` parts 1,164 together, `flora` 624, `engine_cli` 459, `gfx` 285; 8 min of budgets) / 58 min / about 3 h 50 (3 h 34 of CTest; the `renderer` parts 862, 1,414, 3,306 and 1,107 s, `engine_view` 1,436, `water` 1,268). Every leg ran CTest serially: no test preset set `jobs`. Measured from its logs ([the gate's wall clock](#the-gates-wall-clock-t77)): `msvc-debug`'s own work one test at a time was 5,755 of its 6,765 s and waits for the lock at least 904 s, so serial CTest and not the lock was the limit |
| The frame budgets (`tools/frame-budget.ps1`, five runs, `msvc-release`, warm cache; 2026-10-07) | 13 min, plus lock and quiet waits |
| `engine_view.frame_loop` alone (2026-10-07) | 220 s in `msvc-debug`, 60 s in `msvc-release` |
| A fresh worktree's first three containers (`linux-clang-debug`, `linux-gcc-release`, `linux-clang-minimal`, build only), compiler cache warm (2026-10-20) | 3 min (1 m 33 s, 0 m 48 s, 0 m 38 s; 100% hits); half an hour or more cold, which is now the first run on the machine after a toolchain or flag change ([local Linux](local-linux.md#with-the-compiler-cache-2026-10-20)) |
| A fresh worktree's first `msvc-debug` build, no compiler cache (2026-10-20, host CPU 96–100%) | 9 min: about 4 configuring and fetching, 5 compiling and linking (1,542 compiles, 128 links). With ccache installed and warm: not yet measured, pending the install ([the Windows half](local-linux.md#the-windows-half)) |

**The merge gate runs in a worktree that persists** (`D:workspacege-gate`, branch `gate`, locked so the
volume sweep keeps it): main is reset onto it and the batch's branches are cherry-picked on top, so its
four MSVC build directories, its derived-data cache and its two container volumes are warm from the
last batch and a build is the batch's own compilation units (1.5 to 2 minutes each at batch 13), its
three suites run side by side (the GPU lock serialises the GPU tests across them, so the three take the
longest one's time), and its release binaries are the ones installed, because the merge fast-forwards
main to the very commit they were built from. Before it, every batch built four presets from nothing
in a fresh worktree and ran the suites one after another, which is where the four to five hours
between the agents' hand-back and the merge went. **One lesson from its first run:** a wait for free
memory before each build must be sized to one build and must say what it is waiting for. The desktop
holds about 106 GB of its 125 GB commit limit at rest (a hundred Python processes, Edge, Visual Studio,
and some 37 GB outside any process), so a bar of 20 GB of commit to spare was met only by luck, and
the gate's first run spent three of its five hours in that wait, silently, because a PowerShell function
that writes a string and returns a value hands both to the caller's `if`. Ten GB to spare and four
free, fifteen minutes, then proceed and log it: a build that fails for memory is retried anyway.

A first build in a fresh worktree comes on top of these (the table's last two rows); with the compiler
cache warm the containers' share of it is minutes, and `build -Affected` builds only what the
change's tests need ([what "affected" means](#what-affected-means)).
When a figure here is found wrong by more than a third, correct it in the change that found it.

**Windows shell-test environment (2026-10-23 gate).** A hidden PowerShell launched with `-NoProfile` can find Git's Bash while its inherited `PATH` lacks Git's Unix tools. Then `tools.docs_gate` reports `dirname` and `mktemp` missing before its fixture exists; this is an environment failure. Put Git for Windows' `usr/bin` on that process's `PATH` before running the suite (on this desktop, `C:\Program Files\Git\usr\bin`). Re-running only `tools.docs_gate` in the three completed Windows presets passed all 28 checks each; the Linux suites already passed the same test.

The 2026-10-23 terrain batch has 121 tests in Windows debug: **70 minutes** for that suite, **23 minutes** for minimal (76 tests) and **59 minutes** for no-ECS (110), side by side with the release app checks (**25 minutes**, seven entries) and the containers. All four Windows builds preceded those runs. The three Linux presets took **39 minutes** together (120, 120 and 76 tests), the GPU server **107 minutes** end to end (**87 minutes** of CTest, 100 tests), and the five warm frame-budget flights **six minutes** after the desktop suites. These are shared-machine elapsed times, not quiet costs; the budget reports retain their machine states. Size a new wait from these latest counts rather than the smaller October 4 tree.

**Checking the release stamp.** The build-time stamp is regenerated from Git on every build (`cmake/EngineBuildStamp.cmake`), and the build relinks the applications when it changes. `engine-view`, `engine-editor`, `engine-content`, `engine-image` and `engine-mcp` expose it through `--version`. The host, CLI and input probe currently have no `--version` switch; verify their completed relink against the clean generated stamp and retain their file hashes with an install, rather than treating an unsupported flag as a stale binary.

## The gate's wall clock (T77)

The owner, 2026-11-09: five to ten hours a day waiting for gates is too high. Until T77 every leg of
the gate ran CTest one test at a time — no test preset set `jobs` and `tools/dev.ps1 test` passed
none — and nobody knew whether the Windows half, four suites side by side, was waiting on that or
on the one machine-wide GPU lock they share.

### Gate b57, measured

From gate b57's second run (2026-11-09, `D:\workspace\ge-gate\build\<preset>\Testing\Temporary\LastTest.log`
and the gate's console logs; the first run's logs for the same tests beside it). The lock's own
record of holds (`ENGINE_GPU_LOCK_LOG`, [gpu_lock](../subsystems/gpu_lock.md)) was not on, so what a
log gives is a test's wall seconds and the waits its own process printed (`gpu-lock: taken after N
s`); an end-to-end test's children wait out of sight, since the test keeps their output. **Own** is
the smaller of the run-1 time and the run-2 time less its printed waits: the test's work with the
lock free, holding or not. The four suites started at 11:54 and ran beside the three containers
and the GPU server.

| Suite | Tests | Wall (CTest real) | Sum of test times | GPU-labelled tests | Printed lock waits | Own work | of it, CPU-only tests |
|---|---|---|---|---|---|---|---|
| `msvc-debug` | 134 | 6,765 s (11:54–13:46) | 6,753 s | 4,849 s | 904 s | 5,755 s | 1,851 s |
| `msvc-no-ecs` | 123 | 5,848 s (11:54–13:31) | 5,840 s | 3,986 s | 724 s | 4,847 s | 1,812 s |
| `msvc-minimal` | 81 | 2,015 s (11:54–12:27) | 2,011 s | 1,534 s | 379 s | 1,472 s | 450 s |
| `msvc-release`, five app tests | 5 | 2,738 s (11:54–12:39) | 2,737 s | 2,737 s | none printed | 2,710 s | 0 |

| Test | `msvc-debug` (waited) | `msvc-no-ecs` (waited) | `msvc-minimal` (waited) | run 1, debug / no-ecs | Titan Xp |
|---|---|---|---|---|---|
| `engine_view` | 1,536 (children) | 913 | 293 | 1,597 / 1,270 | 1,436 |
| renderer, four parts | 1,164 (465) | 787 (90) | 581 (60) | 1,432 / 1,447 (one test) | 6,689 |
| `flora` (CPU) | 624 | 626 | — | 627 / 629 | 601 |
| `engine_cli` | 459 (children) | 176 (45) | 74 (30) | 466 / 87 | 446 |
| `gfx` | 285 (135) | 254 (105) | 432 (285) | 929 / 989 | 543 |
| `engine_content` | 266 | 262 | 9 | 266 / 280 | 220 |
| `rocks` | 244 | 245 | — | 240 / 240 | 175 |
| `scatter` | 234 (225, behind no-ecs's `water`) | 16 | — | 18 / 25 | — |
| `scene_collision` | 69 | 395 (315, behind debug's renderer) | — | 73 / 68 | 50 |
| `water` | 71 (15) | 280 | — | 365 / 134 | 1,268 |
| `engine_view` in `msvc-release` | 1,941 in release (children) | | | 2,147 in release | |

**The answer: serial CTest.** `msvc-debug` was the Windows half's last suite, and 5,755 s of its
6,765 s was its own tests one after another; the lock added the rest. The lock was not saturated:
two or more suites were inside a GPU-labelled test in 93 of the 113 minutes, and the waits the
suites' own processes printed came to 2,007 s across all four — a test's holds are one device's
life, a case or an app run, and the other suites got the card between them. What the lock did cost
was concentrated where the same heavy test of two suites met: `gfx` against `gfx` (285 to 989 s for
the same cases), the renderer against the renderer (465 s of debug's renderer parts were waiting),
`scatter` behind `water`, `scene_collision` behind the renderer. So the order is the limit, and the
lock decides how far jobs can take it: inside one invocation the GPU-labelled tests still run one at
a time (below), so a suite can shrink to its GPU chain and no further.

### Jobs

**Windows: a `-Jobs <n>` on `tools/dev.ps1 test`, which the gate passes (3), and no `jobs` in the
Windows test presets**, so an author's `test -Filter` and `test -Affected` run one test at a time as
they always have. **Linux: `execution.jobs` in the test presets** — 8 for `linux-clang-debug`,
`linux-gcc-release`, `linux-clang-minimal` and `linux-clang-no-ecs`, 4 for `linux-server` and
`linux-server-debug` — because the container and the server run `ctest --preset <p>` inside
`tools/linux-build.ps1` and `tools/remote-build.ps1`, which pass no `-j`. A `-j` on the command line
wins over the preset.

What makes a parallel run safe, set in `cmake/EngineTesting.cmake`'s finalize by label and property,
so a test registered anywhere — by a module, an app, a tool, or a file added since — is held to it:

- **Two tests that can open a GPU device never run at once in one invocation.** Every test labelled
  `gpu` (`engine_test_waits_for_gpu`: the test or a process it starts can open a device and so wait
  for the machine-wide lock) gets `RESOURCE_LOCK gpu_device`. Without it the second would wait in
  the GPU lock, inside the half-hour budget of its own deadline, and a wait that ran out is a skip,
  which at the gate is a failure. With it the second waits in CTest, where nothing runs out.
  `tools.game_sdk`, which starts `engine-view`, carries the label since T77.
- **End-to-end tests keep `RESOURCE_LOCK e2e_apps`** (windows and input devices; `engine_app`),
  parts included.
- **Tests that assert an upper bound on wall-clock time run alone** (`RUN_SERIAL`, and the label
  `wall_clock`): `tileset` (one height lookup beside a building window under 50 ms) and
  `scene_collision` (an activation and a prefetch wait under 120 ms). Every other timing check in the
  tree was read and is a lower bound (`elapsed >= x`), a synthetic clock (the flythrough's injected
  30 ms host turn), or a ratio; the `bench.*` smoke runs make one iteration and assert nothing about
  time, so they share cores.
- Scratch space was already per test (`TempDir`, the lint's `test-temp-path`), which is why a test
  can share a run with another at all.

**What a resource lock does not do: order two invocations.** The gate's four Windows suites are four
`ctest` processes, and their GPU tests still meet in the GPU lock as they did in b57 — now more
often, since each suite's GPU tests run back to back. Each wait is bounded by the other holder's
current hold (one case's device, one app run: the lock is handed over at case granularity, `gfx`
against `gfx` traded it every few seconds), and a test's budget is half an hour of waiting from its
own start. The tests that ran longest were the most exposed — `engine_view` in release, 1,941 s
in one test — which is one reason it is four parts now; the renderer's part 3 on the server (3,306 s)
has no second suite to wait for there. The off-configurations' GPU cases leaving (below) take the
heaviest pairs, `gfx` against `gfx` and the renderer against the renderer, out of the Windows half.

**What was rejected.** CTest's `RESOURCE_GROUPS` with a `gpus` pool would let a machine say how many
GPU tests it takes at once (one here, eight in a container that has no GPU), but CTest ignores the
property when no resource specification file is given, so a hand-run `ctest -j` would lose the
protection; `RESOURCE_LOCK` holds in every invocation. Jobs in the Windows test presets would have
made every author's filtered run parallel, which nothing asked for. A lock only on the tests that
open a device for certain (`protocol`, `audio` and `engine_content` carry `gpu` and open none) would
shorten the GPU chain by their minutes and is left for when the label can say so, since a test
wrongly left out queues in the lock.

### Parts dealt by cost

`engine_module_tests(... PARTS <n>)` and now `engine_app(... E2E_PARTS <n>)` register one executable
as `<name>.1` to `<name>.<n>`, each running its own source files' cases through doctest's
`--source-file=` filter. `engine_view` has four parts and `gfx` two (with `renderer`'s four). The
deal (`_engine_test_parts`, `cmake/EngineModule.cmake`):

- runs at the end of the declaring `CMakeLists.txt` (`cmake_language(DEFER)`) and reads the test
  target's whole `SOURCES`, because `gfx` adds most of its test files with `target_sources` after
  `engine_module_tests`;
- deals the files `cmake/test-costs.txt` gives seconds for first, longest first, each to the part
  with the least so far, and the rest in name order round the parts — so an empty table is the
  name-order deal the renderer's parts had (its four parts are file for file the ones gate b57 ran);
- anchors each file's filter at the separator before its name (`*/x.cpp` and `*\x.cpp`), so `gfx`'s
  `deform_tests.cpp` does not also run `flora_deform_tests.cpp`.

**The table is empty, and why.** Gate b57's logs hold seconds per CTest test only — no case or file
durations — so the renderer is not re-dealt yet (its part 3 still holds half the server's renderer
time, and part 2 over half of Windows'). Every module and end-to-end test now runs with
`--duration=true`, one line of seconds a case in the log, and `tools/test-costs.ps1 -Log <LastTest.log>
-Tests renderer,gfx,engine_view -BuildDir build/msvc-debug` sums them per file into the table (the
case-to-file map from the executable's own `--list-test-cases --reporters=xml`; tested by
`tools.test_costs`). The slowest card is the binding one, so the rows come from the GPU server's
log (`remote-build.ps1 -Fetch`) where it has one: a part that fits there fits on the RTX 5090.
**A case's seconds include its wait for the GPU lock**, and on this shared desktop that can be most
of them: `gfx.1`, run by filter on 2026-11-10 behind three other agents' measurements, took 1,765 s
of which 1,725 were one case waiting (its own work: 50 holds, 35 s held, on the RTX 5090 in
`msvc-debug`). The process prints the wait on stderr, which reaches the log ahead of the buffered
case lines, so the tool takes each printed wait off the shortest case at least that long — the
only cases it can have been in; an end-to-end test's children wait out of sight, which is one more
reason to read the server's log, where nothing else holds the card.

The check that a deal lost nothing: each part's `--list-test-cases --no-skip` with its own filter,
summed, against the executable's whole list (2026-11-09, `msvc-debug`): renderer 111 + 69 + 98 + 71
= 349, gfx 54 + 92 = 146, engine_view 53 + 48 + 50 + 19 = 170, every name once.

### The off-configurations' GPU cases

`msvc-minimal`, `msvc-no-ecs`, `linux-clang-minimal` and `linux-clang-no-ecs` exist to prove the tree
builds and runs without a capability. They still build everything and run every CPU test and every
app test; what they no longer need to run is the GPU cases of a library that is the same code in
every configuration, which the all-on suite runs. **`tools/dev.ps1 test -SkipInvariantGpu`**, which
the gate passes to the two Windows off-configurations: in a tree whose `modules.json` says `minimal`
or names a disabled capability, the tests labelled `config_invariant` run first with no Vulkan driver
to find (`VK_DRIVER_FILES` and `VK_ICD_FILENAMES` pointed at a file that does not exist, and the GPU
lock off for them), so their CPU cases run and their GPU cases skip exactly as on a hosted runner
with no GPU, and then everything else runs as usual. In an all-on tree the switch changes nothing,
and an author's run without it is what it was. The containers have no GPU at all, so there the two
Linux off-configurations already skip every GPU case.

`config_invariant` is `engine_module_tests(... CONFIG_INVARIANT)`, on the renderer and `gfx`: neither
is a capability, `engine_module` refuses a non-capability linking one, and so the one way either
library could still differ between configurations is a capability conditional of its own. Two
checks hold that, and land with the exemption:

- **at configure time**, `_engine_check_config_invariant` reads the module's `CMakeLists.txt`: inside
  an `if()` on `TARGET engine::<x>`, `ENGINE_WITH_<X>` or `ENGINE_MINIMAL`, every line must be about
  the tests (`_tests`, `tests/`), and a capability switch named anywhere else stops the configure —
  the renderer's conditionals all add test files or test definitions, which only ever drop cases
  from an off-configuration;
- **in the lint**, `config-invariant-conditional`: a preprocessor conditional on `ENGINE_WITH_*` or
  `ENGINE_MINIMAL` in any file of a directory whose `CMakeLists.txt` says `CONFIG_INVARIANT` (marker
  `allow-config-conditional`; `tools.lint` tests it).

Another module joins with the same line once its `CMakeLists.txt` passes the configure check. What
it saves on gate b57's numbers: `msvc-no-ecs`'s renderer parts and `gfx`, 1,041 s of GPU tests, and
`msvc-minimal`'s, 1,013 s — and with them the two pairs that waited longest on the lock against
`msvc-debug`. The decision is reversible by dropping the switch from the gate's command line
(review 2026-11-09, decision 5).
