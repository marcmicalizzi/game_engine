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
**slot worktrees**, `.claude/worktrees/slot-1` to `slot-N` (ten since 2026-11-18, `slot.ps1 add`
makes another; the pool grows with the agents two accounts run), each with a warm
`build/msvc-debug`, and `build/msvc-release` where a row flies or measures, and **assigns one to each
heavy agent by its brief**. The slots exist for warmer builds and for fewer bytes written: a fresh
worktree writes a whole build tree before its first useful edit, a warm slot writes what its change
touched ([what a build writes](#what-a-build-writes)). The coordinator claims the slot for the row
before the agent starts and the agent never runs `slot.ps1`; it works in the slot on a branch of its
own (`b57-<row>`), and the coordinator releases the slot and resets it to `main` after the merge
(`reset` refuses a claimed slot: release first). A slot has no branch of its own: between agents it
is detached at a commit of `main`, so no two worktrees ever hold one branch.

**A slot agent's shell starts in the primary checkout, not in its slot**, because the harness starts
it at the repository root. So every command names the slot (`git -C <slot>`,
`pwsh -WorkingDirectory <slot>`) and every path is absolute: a relative path writes into the primary
checkout, which no agent writes (four stray writes landed there in batch 68). A slot builds
`msvc-debug` unless its row flies or measures. A slot's build trees are about 21 GB for `msvc-debug`
and 20 GB for `msvc-release`, about 20,000 files each, so `slot.ps1 list` prints each tree's size and
last use and the pool's total; a new slot's first build is whole, from the compiler cache.

`tools/slot.ps1` keeps that honest (exit 0 done, 1 refused with the reason, 2 usage):

| Command | What it does |
|---|---|
| `slot.ps1 list` | each slot's path, HEAD, branch or detached, dirty count, its build directories with the time of the newest file under each (its last use), size and file count, and its owner; then the pool's total |
| `slot.ps1 add -Slot 11 [-To <sha\|main>]` | `git worktree add --detach` at the commit in the slots directory; refuses a slot that exists and an unknown commit; no build: warm it |
| `slot.ps1 claim -Slot 2 -Owner S100` / `release -Slot 2` | writes or removes `build/slot-owner.json` (owner, time, branch; git-ignored with `build/`); a second owner is refused |
| `slot.ps1 reset -Slot 2 [-To <sha\|main>] [-Force]` | `git checkout --detach`, `build/` left in place; refuses a dirty tree, a process whose executable is under the slot (best effort: a compiler, a test run, an engine binary), and a claimed slot unless `-Force` |
| `slot.ps1 warm -Slot 2 [-Preset msvc-debug,msvc-release] [-Base <sha>]` | `tools/dev.ps1 build` for each preset inside the slot, `-Affected -Base` when a base is given; honours `CMAKE_BUILD_PARALLEL_LEVEL` |

`tools/slot.Tests.ps1` (CTest `tools.slot`) drives every command over a throwaway repository with two
worktrees (and a third that `add` makes), including the refusals. A slot is not an agent's own checkout to leave dirty: whoever
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

### What a build writes

Drive wear is a cost the owner watches (2026-11-18), so the pool is also judged by the bytes a build
writes ([T185's measurements](../experiments/build-writes-2026-11-18.md), 2026-11-18, `msvc-debug`):

- **A fresh worktree's first build writes the whole tree, about 18.5 GB** (17.5 GB in 21,000 files:
  `.pdb` 5.8, `.ilk` 4.1, objects 3.4, executables 1.8, libraries 1.4; plus 0.9 GB of dependency
  sources), **where a warm slot writes about 0.4 GB for a one-line change**: one object, its module's
  `.lib` archived again whole (the renderer's 136 MB, the largest single write), and incremental
  relinks that change a few percent of their outputs' pages. That difference is what the pool saves.
- **ccache's `hard_link` stays off**: it would save the objects' 3.4 GB of a cold build and nothing
  of an incremental one, but a linked object is stored uncompressed, and these `/Z7` objects compress
  5.1 to 1, so the 30 GB cache would hold about two hours of use instead of a day; the page has the
  rest.

To watch the machine as a whole over a window, `tools/disk-writes.ps1` (read-only, no elevation)
prints the bytes written per physical disk and per volume, and the processes that asked to write the
most, from the performance counters; `-Out` keeps the same as JSON. A process that lives less than
one sample (a compiler) and a mapped file's writes do not show in its process list, which is why the
per-build figures were taken with a job object instead. It is not run by the gate.

## What "affected" means

`tools/dev.ps1 test -Affected [-Base main]` (the rule is `tools/lib/Affected.psm1`, tested by
`tools.affected`; `tools/dev.ps1 affected` prints the set without running it):

- A file that differs from the base — committed since the merge base, staged, unstaged or
  untracked — belongs to the module whose directory holds it (`build/<preset>/modules.json`).
- **A module's tests run when what they link or start holds a changed module** (the test edges,
  roadmap T31, below). Each row of `modules.json` carries `test_reach`: the closure of the
  module's test executable, its bench and, for an app, the app itself, over what they link, what
  they build first (`add_dependencies`, which is how a test says it starts an executable) and the
  executables they are compiled with the path of (`$<TARGET_FILE:...>`), walked at configure time
  by `cmake/EngineTestReach.cmake`. It holds every module the module depends on, and what the
  module graph alone could not say: `renderer`'s tests link `sky` and `terrain`, `water`'s and
  `scatter`'s link the renderer, `engine_cli`'s start engine-view, engine-host and
  engine-content, `gpu_lock`'s start a probe that links `gfx`. A module is in it too when its
  directory holds a source a target of the closure compiles (engine-editor builds engine-view's
  camera), an include directory one reads (`water`'s tests include the renderer's test headers),
  or the `CMakeLists.txt` that declared a custom target in it (engine-host's copy of the shell).
  It also holds the sources a test target compiles from outside every module
  (`tests/support/test_main.cpp`, `apps/tests/`) and the directories of targets that are no
  module (the game modules under `game/`). A path under a layer directory that no module holds
  and nothing of the closure reaches — `apps/engine_shell`, which engine-host copies by a glob no
  target lists — runs everything, unless its directory is named like a capability the preset
  switched off.
- **What a test reads at run time is found from the text** when the rule runs
  (`Get-AffectedMentions`, `git grep` over the working tree), so an edit that adds a reference
  counts without a configure. Comments (`//`, `#`) name nothing. The grep is started with its
  arguments as a list, never through `& git`, because PowerShell on Linux expands a wildcard in a
  native command's argument against the current directory: `*CMakeLists.txt` reached git as the
  root's `CMakeLists.txt`, and a `cmake/` helper's callers went unfound in every container
  (2026-11-18).
  - `content/<dir>/...` reaches the modules whose files name `content/<dir>`, and every content
    directory whose files refer to that one (a scene's `../../flora/oak.flora.json`) brings its
    readers too; a file that builds a content path out of pieces (`"content"` joined to a name:
    `materials`) reads every directory. A file at the top of `content/` runs everything.
  - A script under `tools/` (`.ps1`, `.psm1`, `.sh`, `.cmd`, or a `.json` or `.txt` one reads)
    runs the tests whose command names it or names a file of `tools/` that names it, through any
    number of others (`affected.Tests.ps1` imports `lib/Affected.psm1`; `no-driver.Tests.ps1` runs
    `dev.ps1`, which imports it too) — the commands are CTest's own (`ctest --show-only=json-v1`),
    so a module's test that runs a script (`mcp_bridge.mcp_check`) is selected by its name; a
    script no test runs selects no tools test, and the run says so (roadmap T183: the tools' own
    tests had been about 1,100 s of a 1,839 s run for any change there). Anything else under
    `tools/` (the shader meter's C++), a tree with no CTest file, and `templates/` still run every
    tools test; any of them builds the apps. It also reaches the modules whose files run a script
    from the tree (`"tools/...`, `${CMAKE_SOURCE_DIR}/tools/...`: `gpu_lock`, `mcp_bridge`,
    `engine_host`, `engine_view`).
  - A `tests/support` header reaches the files that include it, through the support headers that
    do; a support source no test target lists runs everything.
  - A shader is also a change to every module whose shaders include it, by relative path.
  - `cmake/<file>.cmake` reaches the modules whose `CMakeLists.txt` call what it defines, when all
    it holds is function and macro definitions and every caller is a module's `CMakeLists.txt`
    (today `EngineSchema.cmake`); anything else — a helper that sets a variable, includes another,
    fetches a dependency, or is called from another helper — runs everything.
  - A reference in a module's `tests/` or `bench/` source reaches that module's tests; one
    anywhere else in a module (its sources, its headers, a test header another module's tests may
    include, its `CMakeLists.txt`) is a change to the module and reaches what links it.
- **A changed file in a module's `tests/` or `bench/` runs that module's tests alone** (roadmap
  T183): it is compiled into, or read by, that module's test and bench executables, which nothing
  links. A header there also runs the tests whose files include it, through the headers that do
  (`water`'s tests include gfx's `water_reference.h`), and is a change to a module whose own source
  includes it; a shader there is a shader. Until 2026-11-18 such a file was a change to the module,
  and three test sources of the renderer ran the fifteen modules whose tests link it.
- **It never selects too little**: the build system's core (`CMakeLists.txt`, `CMakePresets.json`,
  `cmake/*.txt`), vendored code, the schema compiler, and a reference from a file no module holds
  run everything, and the run says why. `-AffectedRule modules` (or `ENGINE_AFFECTED_RULE=modules`)
  is the older rule — the changed modules, their dependents, and every module above a changed
  capability's layer — and the rule whenever `modules.json` has no test edges (a tree configured
  before them); `-AffectedRule all` selects everything.
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
  `domain/flora`, a capability and so (under the rule of the day) everything above its layer, asks for 1,146 of the tree's 1,542
  compiles and 44 of its 128 links; one in `foundation/input` for 1,028 and 21; one in
  `apps/engine_cli`, whose tests start engine-host, engine-view and engine-content, for 954 and 7.
  The floor under all of them is the third-party code and `core/`, which everything links. In a
  tree that is already built, the saving is whatever else is out of date — main's changes after a
  rebase, say — which a whole build compiles and this does not; the change's own files are
  recompiled either way. What takes the floor out of a fresh worktree is
  [the compiler cache](local-linux.md#the-compiler-cache).

**The test edges, measured** (roadmap T31, 2026-11-17). Batch 67's sixteen merged branches replayed
through both rules (each merge's own diff, the edges of the tree at 0073605c, the text references
read at the branch's tip; 170 tests in `msvc-debug`). Minutes are the sum of each selected test's
fastest Windows run that night under load — a GPU case's fastest is its skip, so it stands for the
no-driver form agents run — and are serial seconds, not the wall clock of a `-Jobs` run:

| | Tests selected (16 branches) | Branches that selected everything | Minutes |
|---|---|---|---|
| the module graph (`modules`) | 2,205 | 9 | 586 |
| the test edges (`edges`) | 1,800 | 3 (each changed `cmake/EngineModule.cmake`) | 501 |

Five branches that changed committed content and one a support header now select 89 to 144 tests
instead of 170; a branch in a leaf capability selects fewer by the modules above its layer that do
not link it (G203: 79 to 58). The saving in minutes is smaller than in tests because the costly
tests — the renderer's and the viewer's parts, `engine_cli`, `engine_content`, `flora`, `rocks` —
link most of the tree and are reached either way; the remaining waste is the tools' own tests,
which a change under `tools/` still runs all of. **Every test that failed on the stack or held a
defect the review found that night is still selected** (renderer.3 for R171, renderer.4 for T164,
R170 and R164, `engine_content` for R164, gfx and water for W136's header, the scripted GPU tests
for T89). The edges also select tests the module graph missed: `water`'s and `scatter`'s tests
link the renderer, `engine_cli`'s start engine-view, `gpu_lock`'s probe links `gfx`, and a change
to a game module (`game/walk_poc`, which `engine_view`'s tests start) selected nothing at all.
Reading the targets once each keeps the configure where it was (182 s against 194 s for this
worktree's first, both under load); reading them once per closure had cost 300 s more. Finding
the text references takes about 20 s of `git grep`.

**A module's test files and the tools' scripts, measured** (roadmap T183, 2026-11-18). Batch 68's
41 merged branches replayed the same way through the base's rule and this one (the edges of the
tree at 5d0634bb, the tools tests' commands from its CTest file, 175 tests; minutes from the same
fastest-run table): **3,809 tests and 1,603 minutes become 3,286 and 1,442**. A raise file under
`tools/structure-raises/` now runs `structure_check` and `tools.structure_check` rather than all 22
tools tests (23 branches), and leg G's one renderer test source 9 tests rather than 37 (5.7
minutes rather than 20.8). Every test that failed that night on a branch's own change is still
selected (the renderer's parts for R173, R158, R168, R171-5 and -6, R173-fix, the banding mend, G2
and leg G; water for R177, R177-fix and W147; `protocol` for A12; `village` for G219;
`tools.affected` for T31). The one it drops is the swash case that failed on the tree where R181
was joined to W147: R181's diff kept was a renderer test source, a raise and pages, the case had
passed on W147's tip and passed again on the same code an hour later, and W147's own merge selects
`water`.

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

**The session's state decides which budget a flight meets** (T161; [bench](../subsystems/bench.md#frame-budgets),
"Which session a budget holds"). A locked Windows session idles the card, so every run holds a
`locked` and an `unlocked` budget side by side and each flight is compared only with its own
state's. A gate flown in a state a run has no budget for exits **6** on that run (compared with
nothing) rather than passing or failing it: the gate's report says so per run. Read the gate's
summary line, not only its exit code: 6 with every run `nobaseline` is a state with no baselines
yet, and the first gate flown in it takes them (`-Rebaseline -Reason "..."`, quiet, session in that
state), which writes that state's set and leaves the other alone.

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

**`linux-clang-tsan`, races** (2026-11-17, batch 68). `linux-clang-debug` with ThreadSanitizer (`ENGINE_TSAN`, `cmake/EngineOptions.cmake`; refused with `ENGINE_ASAN`, whose shadow memory it would share, and on MSVC, which has none): a data race between two threads, reported with both accesses and both stacks, which no other build here sees. It exists because batch 67's row that made the scene generators' `tile` reentrant (a generator declaring `tile_reentrant`, built on workers ahead of the walker's ring) could run no race check. It is not in ci.yml or in `tools/linux-build.ps1 -Preset all`: TSan costs a test five to fifteen times its time and the modules worth it are few, so it is run by hand with a filter, `tools/linux-build.ps1 -Preset linux-clang-tsan -Test -Filter '^(jobs|scene_gen|scene_collision|store)(\.|$)'` — the job system (`core/jobs`, its pools and their queues), the generators' tiles (`scene_gen`), the walker's collision built ahead on workers (`scene_collision`), and the persistent store (`foundation/store`, whose writer and readers are on different threads) — after a change to any of them. Its test preset's `TSAN_OPTIONS` keep TSan's defaults otherwise: every report printed, and the process's exit 66 when there was any, so a test with a race fails. The container needs nothing the ASan preset did not: the sanitizer runtime is `libclang-rt-18-dev`, already in the image. The platform module's fault probe already treats a TSan build as it does an ASan one (`ENGINE_FAULT_UNDER_SANITIZER` names `__SANITIZE_THREAD__`), so a whole-suite run would not stop there.

**What its first run found** (2026-11-17, the local container, `-Filter '^(jobs|scene_gen|scene_collision|store|audio)(\.|$)'`; the build 28 minutes cold, every object compiled anew since TSan's flags share nothing with the cache, which stood at its 30 GB limit). The first build stopped at one link: `domain/audio`'s `realtime_tests.cpp` replaces the global allocation functions to count the audio thread's, and TSan's runtime links its own whole, so the two collide; replacing the rest of the set does not help. Under TSan the test now leaves them alone, says it cannot count `operator new`, and counts the engine's allocations and the log's records as before. Then **`store` and `scene_gen` passed clean** — the generators' reentrant tiles reported nothing — and `jobs`, `audio` and `scene_collision` failed on reports alone (every doctest case passed: `scene_collision` 47 of 47, in 739 s under TSan), 138 of them in `jobs`, all in the job system:

- **`WorkDeque` publishes a pushed job by a release fence and a relaxed store** (`work_deque.h`, `push`: `atomic_thread_fence(release)` then `bottom_.store(relaxed)`), which C++ orders but TSan does not model, and `steal` copies the slot before its CAS decides whether it may — Chase–Lev's speculative read, a data race in the language's terms when the CAS loses. TSan therefore sees no order between a job's push and the thief that runs it, and every read of the job's data downstream is reported with it (`parallel_for`'s chunks at `job_system.h:171`, `run_job`'s counter at `job_system.cpp:272`, `Counter::signal`). `scene_collision`'s five reports and `audio`'s are this one. The mend is the deque's: a release store of `bottom_` (the same instruction on x86) and the slot read as atomic words, or the fence spelled where TSan sees it; not made here, since it is the job system's hot path and wants its own measurement.
- **`JobSystem::stats()` reads each worker's counters while the worker writes them** (`jobs_executed`, `steals_*`, `inbox_pops`, `sleeps`: plain integers, `job_system.cpp:595` against `worker_main`), a real race on numbers that are only reported; relaxed atomics would close it.

Both mended by T186 (2026-11-18; [jobs](../subsystems/jobs.md#what-a-job-may-assume)).

### The hostile-input cases

Roadmap T13 ([the harness](fuzzing.md)). A reader's fuzz target is a CPU-only doctest case, `fuzz: <target>`, in its own test executable, bounded by counts, never a time, so `-Affected` selects it with its module and nothing new is scheduled. **Budget: 10 s a target, 60 s for all, serial `msvc-debug`**, measured in the commit that sets a target's counts; beside the GPU chain on three jobs that adds about nothing to the gate's wall clock ([the design](../experiments/fuzzing-design.md#3-what-the-gate-pays)). Under `linux-clang-asan` the same cases are a sanitized fuzz run on every push. The libFuzzer preset, `linux-clang-fuzz`, is no gate step.

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

**Both machine locks are lines since 2026-11-11** (T96, [the line](../subsystems/gpu_lock.md#the-line),
[ADR-0062](../adr/0062-the-machine-locks-have-a-line.md)): the GPU lock and the container build
lock serve whoever has waited longest, a holder that releases and asks again goes to the back,
and a short ask (a test's device, a `-Minutes 2` capture) passes a long one only while the long
one has waited less than the difference between them. So a wait is the holds that were before
you, each once, and no longer a lottery a re-asker wins: there is nothing to gain by asking early
and often. (The pause GPU-LOCK.md's rule 9 asks between back-to-back holds is still what lets in
a tool that takes no ticket, the Blender agent's: with nobody in the line, a re-asker has the
lock back at once.)
`tools/gpu-lock.ps1 status` (with `-Kind linux-build` for builds) prints the
holder and the line, and a waiter's own log says where it stands. What it changes for a plan:
GPU tests beside other agents' now take turns hold by hold, so a long GPU suite run beside five
others runs at a sixth of its pace instead of all at once after a wait; and a waiter that is
killed frees its place within 32 s.

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
| The whole merge gate on gate b58 (2026-11-10; the first with three CTest jobs a Windows suite, eight in a container, the parts, and the off-configurations skipping the renderer's and `gfx`'s GPU cases; 145 tests in `msvc-debug`, 143 in a container): the Windows half (14 minutes of incremental builds, the suites side by side, the listener check, the budgets) / the three containers | 1 h 30 (64 min of suites, which is `msvc-debug`'s chain of GPU-labelled tests, 3,834 s of CTest; `msvc-release`'s app tests 31 min, `msvc-no-ecs` 27, `msvc-minimal` 9; 7 min of listener check and 6 of budgets) / 37 min ([measured](#gate-b58-measured)) |
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

### Heavy shaders share a pool

**The material resolve's variants are the build's largest single memory cost, and a Ninja job pool
caps how many compile at once** (roadmap T118, 2026-11-12). One `slangc` compile of
`visibility_resolve_rt_water_terrain_stochastic` holds 12.3 GiB of commit for about two minutes; six
of the sixteen resolve variants hold 2.4 GiB or more, the other ten under 1 GiB; the 55 shaders of
the tree sum to 49.1 GiB of peaks and 385 s of `slangc` (the table below). A build that recompiled
them at four or six jobs asked for 38 to 40 GB at once, and on 2026-11-11, with thirteen agents
building, the machine ran out of commit: builds died with C1083 on system headers, tests failed
writing their own scratch files, and a forest load died. Every agent's first build after a base
that touches a file the resolve includes paid that.

**The rule.** `cmake/shader-costs.txt` gives a shader's peak commit and seconds by its stem; a row
at or above the line, `ENGINE_SHADER_HEAVY_MB` = 1,536 MiB, makes it **heavy**, and
`engine_shaders()` gives its compile the Ninja pool `engine_shader_heavy`
(`cmake/EngineShaders.cmake`, `cmake/EngineShaderCosts.cmake`). Every other compile — the light
shaders and every C++ step — runs as parallel as the build's jobs allow, as before. The line sits
in the measured gap (light at 997 MiB and under, heavy at 2,430 and over) with room for a light
shader to grow by half before the check below asks for its row. The depth is the cache variable
`ENGINE_SHADER_HEAVY_JOBS`: a whole number, or `auto` (the default), which is **as many of the
heaviest compiles, largest first, as fit in a third of the build machine's physical memory, and
never fewer than one**. Largest first, because the pool does not choose which heavy compiles run
together, so the depth has to fit the worst pair it could pick. A third, because the same build's
C++ jobs, the other builds on a shared machine and the system need the rest. The configure prints
the depth and the heavy set (`engine shaders: 6 heavy ..., at most 2 at once`).

| Machine | A third of its memory | The largest first | `auto` depth | Heavy compiles' worst peak |
|---|---|---|---|---|
| This desktop, 64 GB (65,216 MiB) | 21,738 MiB | 12,571 + 7,089 = 19,660; + 3,883 = 23,543 | 2 | 19.2 GiB |
| A 32 GB developer machine | 10,922 MiB | 12,571 alone is over | 1 (the floor) | 12.3 GiB |
| 128 GB | 43,690 MiB | all six, 31,333 | 6 (no limit) | 30.6 GiB |

On a 32 GB machine the floor is the point: the largest variant alone is 12.3 GiB, more than a third,
and it still has to compile; one at a time is the least a build can ask. A Linux container sees the
memory its VM was given: this desktop's containers see 15,997 MiB and take depth 1, so their six
heavy compiles run one after another (about 300 s of `slangc` on the shader step's path; the first
two container runs with the pool, `linux-clang-debug` and `linux-gcc-release` with a narrow test
filter, took 9 min 25 s and 11 min 52 s end to end), where before the pool six of them at once would
have asked a 15.6 GiB VM for 30.6 GiB. A generator with no pools (anything but Ninja; every preset is Ninja) ignores
`JOB_POOL` and configures as before, and `build.shader_pool` is not registered there.

**What a heavy compile costs** (the meter's numbers in the first `msvc-debug` build of 33ff2824
with the pool, four jobs and other agents building beside it; the peaks are each compile's own and
do not move with load or preset, the seconds are an upper bound):

| Shader | Peak MiB | Seconds |
|---|---|---|
| `visibility_resolve_rt_water_terrain_stochastic` | 12,571 | 116.2 |
| `visibility_resolve_rt_water_terrain` | 7,089 | 62.9 |
| `visibility_resolve_water_terrain_stochastic` | 3,883 | 44.1 |
| `visibility_resolve_rt_water_stochastic` | 2,824 | 31.2 |
| `visibility_resolve_rt_water` | 2,536 | 26.8 |
| `visibility_resolve_water_terrain` | 2,430 | 22.1 |
| the six together | 31,333 | 303.3 |
| the other 49 shaders | 18,943 summed; 997 the largest | 82.0 |

The water branch and not the terrain is what makes a variant heavy: every heavy variant carries
water, and the terrain variants without it peak at 805 MiB and under.

**Measured, as a pair** (2026-11-12, `msvc-debug`, a clean rebuild of the shaders alone: every
`.spv` and `.cost` removed, then `ninja -j6` over the twelve `engine_shaders_*_build` targets; another
session's `engine-view` held 49.8 GiB of commit throughout, so free commit swung between 45 and 5 GiB
under the run and the machine never had the 80 GiB a pool-less run at six jobs needs, which is why
the "before" side is a model). The peak is the largest sum of the meter's peaks over the compiles
`.ninja_log` shows running at once, an upper bound since two compiles' peaks need not coincide; a
sampler reading the `slangc` processes' private bytes every five seconds saw at most 11.6 GiB. The
model replays the measured run's order and each compile's measured time through `-j6` with and
without the pool; it reproduces the measured run exactly (164.4 s, 19,659 MiB), and its pool-less
side is a lower bound on time, since six heavy compiles at once would contend more than two do.

| Run | Wall time of the shader step | Heavy compiles at once | Peak commit of the compiles |
|---|---|---|---|
| before: no pool, `-j6` (model) | 136.3 s | 6 | 31,323 MiB |
| before: no pool, `-j4` (model) | 157.7 s | 4 | 25,981 MiB |
| **after: pool depth 2, `-j6` (measured)** | **164.4 s** of `slangc` (174 s with ninja's start and the embeds) | 2 | **19,659 MiB** |
| after: pool depth 1, `-j6` (model) | 315.7 s | 1 | 12,571 MiB |

So at this desktop's default the pool takes 11.4 GiB off the shader step's peak for 28 s of wall
time on a rebuild of the shaders alone; in a whole build the C++ jobs fill the slots the pool holds
back. The 38 to 40 GB quoted on 2026-11-11 is close to the sixteen resolve variants' peaks summed
(37,594 MiB on 33ff2824), which one build reaches only with all sixteen running at once; at six jobs
one build's worst is the six heavy compiles together, the first row, and the machine's worst was
several builds' worst at once.

**The table cannot go stale quietly.** Every compile runs under `tools/shader_meter`
(`shader-meter <cost file> <slangc> <arguments...>`, a host tool, standard library and the operating
system only), which writes the compile's peak private commit (Windows) or peak resident set
(Linux, `wait4`'s `ru_maxrss`) and its wall seconds to `<stem>.cost` beside the SPIR-V, for the price
of one process start; the compiler's diagnostics reach the build's log through the meter's own
handles, its exit code is the meter's, and a failed compile leaves no measurement. `build.shader_pool` (`cmake/CheckShaderCosts.cmake`, CTest label `build`,
Ninja builds) reads `build/<preset>/shaders/pool.txt`, which the configure writes, `build.ninja` and
`CMakeFiles/rules.ninja`, and the cost files, and fails when a heavy compile's build statement does
not carry the pool or a light one's does, when the declared depth is not the configure's, when a
shader measured at or above the line has no heavy row (it grew or is new: the failure prints the
row to add), when a heavy row's shader measured under half the line, and when a SPIR-V has no
measurement beside it. The half-line hysteresis keeps a shader near the line from changing class
between a Windows build's commit and a Linux build's resident set. A row that names no `.slang`
source in the tree stops the configure; a source that exists but is not compiled in this
configuration (a capability switched off) is not an unknown name. `tools.shader_costs` is the same
script's self-test: the reader, the refusals in a child `cmake -P`, the depth arithmetic above, and
the classification over fixture builds, each way the build can disagree with the table.

To change the table: build, read the row the check prints (or `<stem>.cost`), edit the row, and
reconfigure (the table is a configure dependency, so `tools/dev.ps1 build` does it). The pool is per
build tree: two agents recompiling the resolve at once each hold their own two, which is what a
machine-wide limit would have to answer.

**Checking the release stamp.** The build-time stamp is regenerated from Git on every build (`cmake/EngineBuildStamp.cmake`), and the build relinks the applications when it changes. `engine-view`, `engine-editor`, `engine-content`, `engine-image` and `engine-mcp` expose it through `--version`. The host, CLI and input probe currently have no `--version` switch; verify their completed relink against the clean generated stamp and retain their file hashes with an install, rather than treating an unsupported flag as a stale binary.

### The shader cache

**A compile some checkout already made is copied, not made again** (roadmap T76, 2026-11-17).
About twenty agents build this tree in fresh worktrees every night. ccache shares their C++ objects
([the compiler cache](local-linux.md#the-compiler-cache)), but `slangc` is not a compiler ccache
wraps, so until T76 every fresh worktree compiled every shader again, the six heavy resolve
variants included. Measured on batch 68's base (0073605c), a fresh worktree's first `msvc-debug`
build at `-j6` on a machine at 100% CPU with ccache warm for the C++: **63 shader compiles, 1,411 s
of Ninja time between them (1,362 s of `slangc` by the meter), and at least one of them running for
604 s of the build's 2,530** (24%; `domain/gfx` 56 compiles and 1,354 s, `systems/renderer` 5 and
51 s, `volumetrics` and `scene_collision` one each; the longest single step,
`visibility_resolve_rt_water_terrain_stochastic`, 489 s under that load). And almost all of it was
repeated work: hashing the SPIR-V of the twenty other build trees on the machine that evening,
**929 of the 1,146 shaders they shared a name with were byte-identical to this one's**, and every
tree at the same commit whose author had not edited a shader matched all 63 — the gate's
`msvc-release` included, so the bytes name neither the checkout nor the preset.

**What it is.** The meter every compile already runs under (`tools/shader_meter`, the section above)
is also the cache, so nothing else stands between Ninja and `slangc`. `engine_shaders()` gives it
the store (`ENGINE_SHADER_CACHE_DIR`), the compiler's identity, and which arguments are the source
and the output; the compile's own arguments are unchanged, and the embedded header and the
manifest the hot-reload path reads are made exactly as before. A lookup is two levels, as ccache's
direct mode is ([`shader_cache.h`](../../tools/shader_meter/src/shader_cache.h)):

| Level | What goes into it |
|---|---|
| The manifest key (SHA-256) | the cache's format version; **the compiler**: a SHA-256 over the Slang release's version and every file of its `bin/` and `lib/` (slangc, the compiler library, glslang, the LLVM backend), taken at configure; the source's **file name** and **bytes**; **every argument** of the compile in order (target, profile, `-O2`, `-warnings-disable`, every `FLAGS` entry and define), with the source, the output and each `-I` directory written relative to the source's directory, so the same compile in two checkouts has one key |
| An entry of the manifest | **every file `slangc` read**, from its own `-depfile` (so no `DEPENDS` list or configure-time scan can be short, and an include reached through a macro counts), each with its SHA-256; and **every place an `#include "..."` of those files could have been found and was not** — beside the including file, in each `-I` directory, beside the shader, in the compile's working directory — each of which must still not exist, because a file created there later would be read instead (a shadow) |

An entry is served only when every file it lists still has its hash and every shadow is still
absent; a manifest keeps the sixteen newest entries, so the branches of a batch that each changed
one included file each keep theirs. What is not in the key, and why that is safe: the absolute
paths (the SPIR-V carries none — checked on every store: **an output that contains the directory it
was compiled in is compiled and never stored**, which is what `-g` produces); the environment
(`slangc` reads none of the engine's); the preset (it changes no shader argument, and the bytes
match across presets above).

**Writing is safe beside other builds.** Every file goes to a temporary name in the store's own
`tmp/` and is renamed into place. A result (the SPIR-V, the cost lines the meter measured, what
`slangc` printed) is written once and never replaced, SPIR-V last, so a result whose SPIR-V exists
is whole; two builds storing one key at once both succeed with the same bytes. A manifest is
replaced whole, and two builds adding to one at once lose at most one entry, which is a later miss.
A file `slangc` read that was written at or after the compile started may have changed under it, so
that compile is not stored (ccache's "include file too new"); the source's key is taken again after
the compile for the same reason.

**What a build sees.** A hit copies the SPIR-V, writes the stored cost lines and `cache hit` as the
cost file's third line (`build.shader_pool` reads the first two, which are the compile's own
measurement), and prints what `slangc` printed when it compiled, so a hit carries the warnings. A
miss runs `slangc` with `-depfile` added, passes its output on, and writes `cache stored` or `cache
not stored: <why>`. So `Select-String 'cache hit' build/<preset>/**/*.cost` counts a build's hits.

**Where it is on**, by the compiler cache's rules ([`CompilerCache.psm1`](../../tools/lib/CompilerCache.psm1),
`Get-ShaderCachePlan`): `tools/dev.ps1 configure` sets the store in an agent's worktree and the
gate's, `<main checkout>_local\shader-cache` beside ccache's (or `$env:ENGINE_SHADER_CACHE_DIR`), and
leaves it unset in the primary checkout and any other worktree unless `ENGINE_SHADER_CACHE=1`;
`ENGINE_SHADER_CACHE=0` turns it off everywhere. Nothing about debugging keeps it out of the owner's
checkout, since a served SPIR-V is the compile's own bytes; the rule is ccache's so that the owner's
checkout changes only when asked. In the Linux containers the store is `/ccache/engine-shaders`, a
directory of the compiler cache's volume (ccache keeps to its own sixteen level directories), on the
configure line of every preset, and `ENGINE_SHADER_CACHE=0` reaches the meter there as
`CCACHE_DISABLE` does. At build time `ENGINE_SHADER_CACHE=0` makes the meter compile without reading
or writing the store, with no configure; changing the setting through `dev.ps1` reconfigures, which
changes every shader's command and compiles them once. The store holds itself under 4 GB
(`ENGINE_SHADER_CACHE_MAX_MB`): at most every ten minutes a store walks it and removes the least
recently used files down to nine tenths, a hit touching what it served; a result the walk left in
part (its SPIR-V without its log or cost) misses once and is stored whole again. One commit's 63 shaders are
30 MB, so 4 GB is over a hundred generations of every shader. Deleting the directory empties it
for the machine; the next build of each checkout compiles once.

**The proof.** `tools.shader_cache` (CTest label `tools`, `cmake/CheckShaderCache.cmake`) compiles a
small shader tree in two copies with this build's `slangc` into a store of its own and holds every
ingredient: the same compile from the other copy's path is served the bytes `slangc` makes with no
cache; an edit to an included file, an edit to the source, a changed flag, a changed compiler and a
file that now shadows an include each miss (and the shadow's compile shows `slangc` did read it);
`ENGINE_SHADER_CACHE=0` serves and stores nothing; a failing compile is never stored or served; a
`-g` compile names its directory and is not stored; two compiles of one key at once both succeed
with one SPIR-V; a store over its limit is emptied down to it; the depfile reader and SHA-256 against
FIPS 180-4's vectors.

**Measured** (2026-11-17, one worktree at 0073605c, `msvc-debug`, `-j6`, the machine at 100% CPU
throughout with some twenty other agents building; Ninja's own log, each compile counted once):

| Run | Shader steps | Their Ninja time, summed | Wall time with one running | SPIR-V against the uncached build |
|---|---|---|---|---|
| before: the fresh worktree's first build, no cache | 63 compiles | 1,411 s | 604 s of a 2,530 s build | (the reference) |
| the cache on, the store empty: every compile a miss and stored | 63 compiles | 952 s | 335 s | 63 of 63 byte-identical |
| the cache on, the store warm: every `.spv` and `.cost` removed and built again | 63 hits | 138 s | **35 s** | 63 of 63 byte-identical |

A hit is a process start and some file reads: 0.2 to 1.5 s each here (one by hand: 175 ms when the
files were warm, 1.4 s when they were not), against 1.5 s for the cheapest compile and 490 s for
the dearest under that load. So a fresh worktree whose shaders match a commit any checkout on the
machine has built — every agent's worktree at the batch's base, until it edits a shader — spends
about half a minute of its first build on shaders instead of ten, and none of the heavy compiles'
memory: a hit holds no `slangc` (its pool slot for a moment), so the 12 GiB of the largest variant is
paid once a machine per change to what the resolve includes, not once a worktree. The miss path
costs a depfile and the store's writes, inside the noise of the compiles beside it.

### The release preset's code generation (T193)

`msvc-release` is CMake's `RelWithDebInfo`, kept for its PDBs (the sampler and Tracy need them),
which by CMake's default compiles `/O2 /Ob1` (inline only what is marked so) and links `/debug
/INCREMENTAL` (every call through a thunk, no `/OPT:REF,ICF`). Since T193 one switch,
`ENGINE_MSVC_RELEASE_CODEGEN`, can build the engine's own targets `/Ob2` and link them
`/INCREMENTAL:NO /OPT:REF /OPT:ICF`, debug information kept (`full`; `inline` for `/Ob2` alone;
`cmake`, **the default**, for CMake's own). The default stays `cmake`: batch 69's two failures under `full` are mended and every pin holds under it, but MSVC's fault is still in the compiler and the canary in `core/base` fails on it under `/Ob2` by design, until a workaround or a mended compiler is in ([the run](../experiments/release-codegen.md#full-run-whole-t197-2-batch-70)). The switch is set in `cmake/EngineReleaseCodegen.cmake` and applied through
`engine_apply_warnings()`, so no module opts in alone. Third-party targets and the host tools keep
`/Ob1` (Jolt has its own `/Ob2`, Recast measured none). Measured
([the experiment](../experiments/release-codegen.md)): the binaries half their size (173
executables 1,337 to 644 MB, `engine-view` 62.9 to 43.5 MB, 3.8 GB of `.ilk` gone, the tree 24.5
to 21.4 GB), no compile-time cost found, the 13 determinism and state-hash tests its author ran unmoved, and the bench rows the
experiment lists. **Switching a tree to `full` recompiles the whole release preset** (the flags are
in ccache's key), once.

### The miscompile scan (T197)

`tools/miscompile-scan.ps1 [-Level Ob1,Ob2] [-Filter <regex>] [-Jobs 8] [-Keep]` looks in the
compiler's own listings for the MSVC fault that `rocks`' pin found
([the page](../experiments/msvc-ob2-avx2-miscompile.md#the-search-as-a-tool) has what it reads, what
a candidate is and what a run costs). **It runs weekly, and before and after a change of compiler
or of the release flags** (`/Ob`, `/arch`, `ENGINE_MSVC_RELEASE_CODEGEN`'s default), on a built
`msvc-release` tree; it is not a test and no gate waits for it. Its matcher is tested in every
suite (`tools.miscompile_scan`).
## The gate's wall clock (T77)

The owner, 2026-11-09: five to ten hours a day waiting for gates is too high. Until T77 every leg of
the gate ran CTest one test at a time — no test preset set `jobs` and `tools/dev.ps1 test` passed
none — and nobody knew whether the Windows half, four suites side by side, was waiting on that or
on the one machine-wide GPU lock they share.

What this section settled is [the gate script](gate.md)'s now, with batch 72's change: two waves of
suites, since four at once put long parts past the GPU lock's 30-minute deadline.

### Gate b57, measured

From gate b57's second run (2026-11-09, `ge-gate\build\<preset>\Testing\Temporary\LastTest.log`
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

- **No more than two tests that can open a GPU device run at once in one invocation** (one until
  T89; [two GPU chains](#two-gpu-chains), below). Every test labelled `gpu`
  (`engine_test_waits_for_gpu`: the test or a process it starts can open a device and so wait for
  the machine-wide lock) gets one chain's `RESOURCE_LOCK`, `gpu_device.1` or `gpu_device.2`, dealt
  by cost. Without a chain every such test would start at once and wait in the GPU lock, inside the
  half-hour budget of its own deadline, and a wait that ran out is a skip, which at the gate is a
  failure. With two, a test waits in the lock for at most the other chain's current hold, as two
  suites side by side already did. `tools.game_sdk`, which starts `engine-view`, carries the label
  since T77.
- **End-to-end tests that can open a window keep `RESOURCE_LOCK e2e_apps`** (windows and input
  devices; the label `desktop`, which `engine_app` gives a test whose app reaches `window`), parts
  included. A headless app's end-to-end test (`engine_cli`, `engine_content`, `engine_host`,
  `mcp_bridge`, `engine_image`, the `.generators` tests) runs beside them since T89.
- **Tests that assert an upper bound on wall-clock time run alone**: a test registered
  `engine_module_tests(... WALL_CLOCK)` carries the label `wall_clock`, and the finalize gives every
  test with the label `RUN_SERIAL` (T87): `tileset` (one height lookup
  beside a building window under 50 ms). Every other timing check in the tree was read (again on
  2026-11-11: every test file that reads a clock) and is a lower bound (`elapsed >= x`), a
  synthetic clock (the flythrough's injected 30 ms host turn, a game session's ticks), or a figure
  printed and not held (the cut, channel, ledge, cone and meadow costs); the `bench.*` smoke runs
  make one iteration and assert nothing about time, so they share cores. A new test that bounds a
  wall clock says `WALL_CLOCK` where it is registered, though `RUN_SERIAL` orders one `ctest`, not
  the gate's three (T199).
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
deal (`_engine_test_parts`, `cmake/EngineModule.cmake`, calling `engine_deal_test_files` in
`cmake/EngineTestDeal.cmake`, which needs no project, so `tools/test-costs.ps1 -Predict` and
`tools.test_costs` run the configure's own deal in script mode rather than a copy of it):

- runs at the end of the declaring `CMakeLists.txt` (`cmake_language(DEFER)`) and reads the test
  target's whole `SOURCES`, because `gfx` adds most of its test files with `target_sources` after
  `engine_module_tests`;
- deals the files `cmake/test-costs.txt` gives seconds for first, longest first (equal costs by
  name), each to the part with the least so far (the lowest-numbered of equals), and the rest in
  name order round the parts from the first — so an empty table is the name-order deal the
  renderer's parts had (its four parts are file for file the ones gate b57 ran);
- anchors each file's filter at the separator before its name (`*/x.cpp` and `*\x.cpp`), so `gfx`'s
  `deform_tests.cpp` does not also run `flora_deform_tests.cpp`.

**The table, and how it is refreshed.** Every module and end-to-end test runs with
`--duration=true`, one line of seconds a case in the log (since T77), and `tools/test-costs.ps1`
sums them per file into the table (T88; the case-to-file map from the executable's own
`--list-test-cases --reporters=xml`; tested by `tools.test_costs`). The slowest card is the
binding one — a part that fits on the GPU server fits on the RTX 5090 — so the tool reads several
logs and keeps, a file, the seconds of the log that took longest over it, with that log's label
as the row's comment (`renderer shadow_map_tests.cpp 666  # b58-linux-server`). After a gate:

1. collect the logs: the server's `LastTest.log` (`remote-build.ps1 -Fetch <dir>`), and
   `msvc-release`'s for the apps' tests (`engine_view`'s parts run there and not on a CPU-only
   container); `tools/dev.ps1 test` keeps every pass's log as
   `build/<preset>/Testing/Temporary/LastTest-<time>-<pass>.log`, so a rerun no longer overwrites
   the suite's (gate b58 lost `msvc-debug`'s that way);
2. `tools/test-costs.ps1 -Log <server log>,<msvc-release log> -Label <gate>-linux-server,<gate>-msvc-release
   -Tests renderer,gfx,engine_view -BuildDir build/msvc-debug -Out <scratch>/test-costs.txt`, in a
   build of the gate's commit (a case renamed since is reported and left out);
3. `tools/test-costs.ps1 -Predict -BuildDir build/msvc-debug -Table <scratch>/test-costs.txt`:
   each part's seconds as the build deals them now and as the configure would deal them by the
   new table — the configure's own `engine_deal_test_files` in script mode, no build needed;
4. copy it over `cmake/test-costs.txt` and commit it with the prediction in the message; the next
   configure deals by it (the file is a configure dependency). Reading the table and dealing costs
   the configure about 70 ms (390 rows and three deals of 130 files, `cmake -P`, 2026-11-11, host
   CPU at 48% beside other agents' builds: an upper bound).

A file the table does not name — one added since the gate — is dealt round the parts after the
costed ones and counts nothing, so a table a few days old still deals well; refresh it when a
part's measured seconds drift from the prediction by more than a quarter. A log line from another
thread can land inside a duration line and cut the case's name (seven cases in gate b58's server
log); the tool mends a cut name when exactly one case of the test begins with what is left, and
reports the rest.

**The table in use: gate b60's logs** (T113, 2026-11-11; a prediction from one gate's seconds,
not a measurement until the next gate runs the deal). Gate b60 ran the four Windows suites at once
on `be3826d9` from 17:55, and fetched no server log, so the rows are `msvc-debug`'s whole suite
(`LastTest-20261111-175517-all.log`) for every file, with the slowest of `msvc-release`'s,
`msvc-no-ecs`'s and `msvc-minimal`'s runs of `engine_view` beside it (the two off-configurations'
`all-rest` logs; their `all-invariant-no-driver` logs ran `renderer` and `gfx` with every GPU case
skipped and are the wrong log for a cost). **The renderer's and gfx's seconds are therefore the
RTX 5090's in `msvc-debug`, not the Titan Xp's**: refresh those rows from the server's log when a
gate has one. One case, 0.5 s of `hdr_output_tests.cpp`, was cut by a log line too early to mend
and is left out; ten were mended.

| Test | Measured at the gate, seconds | Parts as dealt by name, by the table | By the table | Longest part |
|---|---|---|---|---|
| `renderer` (56 files) | 273, 523, 44, 87 | 273, 419, 45, 89 | 207, 207, 206, 206 | 419 to 207 |
| `gfx` (47 files, 2 not costed) | 24, 123 | 30, 133 | 82, 81 | 133 to 82 |
| `engine_view` (41 sources, 8 with no cases) | debug 86, 623, 212, 262; release 82, 844, 154, 327 | 108, 935, 215, 421 | 420, 420, 420, 419 | 935 to 420 |

The renderer's middle column reproduces its measured parts once `renderer.2`'s own printed waits
for the GPU lock (99 s over twenty devices) come off, which is the check that every case was placed;
gfx's runs over by the whole second every file of under half a second rounds up to. `engine_view`'s
middle column is the slowest machine's a file, so it is above every one run. The `PARTS` stay four,
two and four: by the table the longest `engine_view` part is 420 s, under an end-to-end test's
600 s of work (`ENGINE_TEST_WORK_TIMEOUT_S`) and under `flora`'s 547 s, the gate's longest unparted
suite, where `engine_view.2` took 844 s in `msvc-release`; and the renderer's four on the server
were about 1,450 s each at gate b58, so fewer would bind there whatever the 5090 says. Gate b58's
own table (the server's seconds, predicting the renderer's longest part 2,383 s to 1,453 s) was
never committed, because the merged tree of 2026-11-11 added files it did not know.

**A case's seconds include its wait for the GPU lock**, and on this shared desktop that can be most
of them: `gfx.1`, run by filter on 2026-11-10 behind three other agents' measurements, took 1,765 s
of which 1,725 were one case waiting (its own work: 50 holds, 35 s held, on the RTX 5090 in
`msvc-debug`). The process prints the wait on stderr, which reaches the log ahead of the buffered
case lines, so the tool takes each printed wait off the shortest case at least that long — the
only cases it can have been in; an end-to-end test's children wait out of sight, which is one more
reason to read the server's log, where nothing else holds the card. Gate b60's three largest
`engine_view` rows — `mesh_view_tests.cpp` 308 s, `authored_view_tests.cpp` 288 s and
`interactive_tests.cpp` 128 s — are `msvc-release`'s, run beside the other three suites, so some of
each may be a child's wait.

The check that a deal lost nothing: each part's `--list-test-cases --no-skip` with its own filter,
summed, against the executable's whole list. On 2026-11-09 (`msvc-debug`): renderer 111 + 69 + 98 +
71 = 349, gfx 54 + 92 = 146, engine_view 53 + 48 + 50 + 19 = 170. On `be3826d9` before and after
T113's table (`msvc-debug`): renderer 131 + 74 + 88 + 77 and then 80 + 76 + 98 + 116, both 370 of
370; gfx 60 + 105 and then 68 + 97, both 165 of 165; engine_view 46 + 59 + 43 + 29 and then 41 + 34
+ 64 + 38, both 177 of 179, the other two being `engine_view.generators`' own. Every name once, the
same 712 names in the parts before and after, and `ctest -N` the same 158 tests.

### The no-driver form

**`tools/dev.ps1 test -NoDriver`** (T177) is how a change that needs no GPU is tested: `test
-Affected -Base <base> -NoDriver`, in any configuration. It sets four variables for the whole run
and puts them back when the run ends, failed or not (`tools/lib/NoDriver.psm1`, the one function
the `-SkipInvariantGpu` pass below uses too; `tools.no_driver` tests it): `VK_DRIVER_FILES` and
`VK_ICD_FILENAMES` at a file in the build directory that does not exist and `VK_ADD_DRIVER_FILES`
removed, so the Vulkan loader finds no driver and every GPU case skips as on a hosted runner with
no GPU; and `ENGINE_GPU_LOCK_ON_DEVICE=0`, so nothing in the run asks for the machine-wide GPU lock
([gpu_lock](../subsystems/gpu_lock.md)). It prints one line saying so, and refuses any command but
`test`. It cannot tell from a `-Filter` whether the tests named are wanted for their GPU cases (a
test labelled `gpu` holds CPU cases too), so it does not refuse one: what it runs with a filter is
those tests' CPU cases.

**Why the loader's three set by hand still wait.** With the driver hidden, `gfx::Device::create`
fails at the loader, before the hold it takes just before `vkCreateDevice` (ADR-0049), and asks for
nothing. A Vulkan window does not get that far: it joins the process's hold *before it appears*
([gpu_lock](../subsystems/gpu_lock.md#windows)), where nothing yet says a device cannot follow, so
with the switch at the test main's `1` every windowed `engine-view` an end-to-end test starts takes
its turn in the GPU's line before it finds there is no device. On the evening of 2026-11-17 runs
that set the three and not the switch had `engine_view`'s parts each wait out their half-hour
budget, in a line 12 to 21 deep, and skip with 75: about two hours of a run spent learning there
was no GPU. The switch at `0` alone would be wrong the other way, since a run that forgot the three
would open devices with no lock. So the four go together, from one switch. Measured on
2026-11-18 in `msvc-debug` with the machine's lock held by others and 10 to 12 waiting:
`engine_view.1` with the three alone skipped after 1,805 s (its `engine-view` seventh of ten in
the line), and with `-NoDriver` passed in 25 s; the whole `test -Affected -NoDriver -Jobs 3` on
this branch, which reached everything, passed 171 of 171 in 2,311 s, and no process in it waited
for the machine's lock.

### The off-configurations' GPU cases

`msvc-minimal`, `msvc-no-ecs`, `linux-clang-minimal` and `linux-clang-no-ecs` exist to prove the tree
builds and runs without a capability. They still build everything and run every CPU test and every
app test; what they no longer need to run is the GPU cases of a library that is the same code in
every configuration, which the all-on suite runs. **`tools/dev.ps1 test -SkipInvariantGpu`**, which
the gate passes to the two Windows off-configurations: in a tree whose `modules.json` says `minimal`
or names a disabled capability, the tests labelled `config_invariant` run first with no Vulkan driver
to find (the four variables of [the no-driver form](#the-no-driver-form), for that pass alone), so their CPU cases run and their GPU cases skip exactly as on a hosted runner
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

### The renderer as a weaker card

The GPU server leg (the TITAN Xp: no mesh shaders, no ray queries, no cluster acceleration
structures) was red on batches 65 and 66, each time on a new renderer test that took the RTX 5090
for granted, and it trails the merge, so a fault of that class reached `main` twice. Since T162
every renderer test file opens its device through `systems/renderer/tests/test_device.h`, which
honours `ENGINE_GFX_TEST_DEVICE` ([renderer](../subsystems/renderer.md), "Running a case as a
weaker card"), so the Windows gate runs the renderer's suite once more as that card, beside the
`msvc-debug` suite and in the same build:

```powershell
$env:ENGINE_GFX_TEST_DEVICE = 'titanxp'; $env:ENGINE_GPU_LOCK_LOG = '<gate log dir>\lock-titanxp.jsonl'
pwsh -NoProfile -File tools/dev.ps1 test -Preset msvc-debug -Filter '^renderer\.' -Jobs 3
Remove-Item Env:ENGINE_GFX_TEST_DEVICE
```

Since T181 (2026-11-17) `engine-view` takes the same profile under a test's main
([apps](../subsystems/apps.md#running-as-a-weaker-card)), so the leg can reach the app's end-to-end
tests too, `-Filter '^(renderer|engine_view)\.'`; whether the gate widens its filter is the
coordinator's to decide, and no gate has run it yet. `engine-host` took no profile until R193
(2026-11-19), so `engine_cli`'s cases that hold the viewer's picture to the host's were red on such
a leg by construction, two devices compared as one; it takes the same profile now, and those cases
check which device each host opened before they compare a byte ([gfx](../subsystems/gfx.md), "A
test's device profile reaches every host it starts").

`titanxp` rather than `baseline`: it removes everything `baseline` removes (mesh shaders) and the
ray-tracing rows besides, so it covers both of the faults the server leg found. A fault this leg
reports is a test's, not the card's: the case skips before it builds what the device lacks
(T153's pattern). **What it costs** (2026-11-16, `msvc-debug`, the RTX 5090, the session
unlocked): the first part, `renderer.1`, ran 1,025 s of wall clock of which 696 s was waiting for
the GPU lock behind other agents' work (60 device opens, 276 s holding the card), so about 330 s
of its own; the whole suite's own time is in the T162 hand-back's log, measured the same way. The
lock wait is the line's and not the leg's: on a gate that runs nothing else on the GPU at that
moment it is near zero, and beside the three other Windows suites it is whatever they hold.

### Gate b58, measured

The first gate run with everything above (2026-11-10, ten rows on `75fa25da`, beside the three
containers and the GPU server; the per-suite logs and the lock's own record of holds,
`ENGINE_GPU_LOCK_LOG`, one file a suite).

| Leg | Tests | Wall (CTest real) | On gate b57 | The card: holds, held, waited |
|---|---|---|---|---|
| `msvc-debug` | 145 | 3,834 s (02:35–03:39) | 6,765 s | 750, 1,824 s, 241 s |
| `msvc-no-ecs` | 6 with no driver, then 128 | 319 + 1,295 s | 5,848 s | 28, 2 s, 240 s |
| `msvc-minimal` | 6 with no driver, then 86 | 119 + 416 s | 2,015 s | 20, 6 s, 135 s |
| `msvc-release`, the app tests | 8 | 1,846 s | 2,738 s | 293, 520 s, 195 s |
| three containers | 143, 143, 91 | 37 min (15 m 09 s, 16 m 00 s, 5 m 34 s) | 58 min | none |

From the gate's start to the Windows verdict: 13 min 42 s of four incremental builds and 64 minutes
of suites, 78 minutes against b57's two hours; the listener check (121 executables, 6 min 42 s, its
first run on `msvc-release`, clean) and the five budget flights (5 min 42 s, five of five) after
them. No test waited more than 120 s for the card, and none was skipped.

**What bounds it now: one chain of GPU-labelled tests.** `msvc-debug`'s 32 `gpu`-labelled tests
summed 3,815 s of its 3,834 s wall — the suite is its `gpu_device` resource lock end to end, as the
section on jobs said it would be — while the card was held for 1,824 s of it. The other half is
those tests' work with no device open: a debug build's scene loads, the content builds inside
`engine_cli` (605 s) and `engine_content` (269 s), an app test starting its processes. Everything
without the label finished inside the chain's time, `flora`'s 652 s included. So the next lever is
not more jobs but **two chains**: the GPU-labelled tests dealt by cost over two resource locks, the
machine-wide lock ordering their holds as it does between suites, where this gate's longest wait
was two minutes against a budget of thirty (roadmap T89). The release app tests are the same shape:
1,846 s of wall, 520 s on the card.

**The one failure, and what it changes for the stack.** `tools.lint` failed on every suite and every
container, and nothing else did: three "passes" cases of T77's `config-invariant-conditional` rule
exited non-zero because their fixture modules have no size table, which T80's `size-table` rule,
landed in the same batch, holds over every module of a fixture. Each row's test passed on its own
branch. The fixtures carry a table now, the test was rerun alone on both platforms, and the
listener check and the budgets, which the failure had cut off, ran after it. **A stack's pre-gate
check runs `tools/dev.ps1 test -Filter '^tools\.'`** beside the lint, the documentation check and
the debug build (about six minutes, no GPU): rows that change the tools meet in the tools' own
tests, and a gate is a slow place to learn it.

## The gate script

A batch's gate is one command of the tree since T217, and a follow-up's is the same command with
the tests its change can reach; what each step does, how a verdict is read and the step after the
gate are [the gate script](gate.md)'s page.

```powershell
pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73 -Logs <dir> -Server <ssh alias>
pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73b -Logs <dir> -Reach '<regex>' -Skip listeners,budgets,weaker-card
pwsh tools/after-gate.ps1 -Gate <gate worktree> -Main <primary checkout> -Verdict <dir>/b73-gate.verdict.json -CopyTo <dir> [-Push]
```
## Two GPU chains

**Where the chain's time went, gate b66** (2026-11-16, `msvc-debug` on d6534963 at three jobs,
06:12 to 07:27, beside the other three Windows suites and the containers; its console log, its
CTest log and its hold log, `ENGINE_GPU_LOCK_LOG`). The suite's wall was 4,496 s and its 34
`gpu`-labelled tests summed 4,480 s, so the suite was still the one chain. A test's run, the
holds' times laid over the CTest order (each hold is one process's device, from its `started` and
`held_ms`, and is the test's whose executable it is, or for a child process the end-to-end test
running at that moment):

| Of the chain's 4,480 s | Seconds | What it is |
|---|---|---|
| A device held | 2,036 | the test's cases on the card, and the CPU work between them inside one device's life |
| Waiting for the GPU lock | 348 | other suites' and other agents' holds |
| No device open | 2,096 | scene loads and content builds in a debug build, a process starting, and the tests labelled `gpu` that opened no device that run: `rocks` 331, `engine_content` 279, `landform` 47, `world` 37, `caves` 30 and seven smaller |

And inside the holds of the module tests (952 s; their own output is in the CTest log, where an
end-to-end test's children's is not): 534 devices, whose instance and device creation took 225 s
of which the processes printed 113 s as waits for the lock — about 0.2 s a device — and 21,131
pipelines compiled in 24.6 s, the driver's own cache being warm. So creation and compilation are
about a seventh of a hold, and the rest is the cases' work; sharing a device across a test's cases
would have bought tens of seconds, at the price of holding the card across a test's CPU work, which
is the opposite of what the lock is for. **The time the chain could give back was the 2,096 s with
no device open**, which a second chain overlaps with the first's holds.

**What the configure does.** Every test labelled `gpu` takes one of `ENGINE_TEST_GPU_CHAINS`
resource locks, `gpu_device.1` to `gpu_device.<n>`, so `n` of them run at once in one `ctest` with
jobs, and the machine-wide lock orders their devices' holds a hold at a time, as it does between
two suites: a test waits for at most the other chain's current hold. `ENGINE_TEST_GPU_CHAINS` is
`auto` unless set, and `auto` is worked out at every configure: 2 where the machine-wide lock's
directory exists (the lock is in use, [gpu_lock](../subsystems/gpu_lock.md#elsewhere-and-the-limits))
and a test's work fits twice into its lock wait, and 1 otherwise — where the directory does not
exist because nothing would stop two devices being open at once, and where the work does not fit
for the reason [below](#one-chain-where-a-test-outruns-its-deadline); a bundle
(`tools/package-tests.ps1`) writes every chain's lock as one `gpu_device` for the first reason. The
chains are dealt in `_engine_finalize_tests` (`cmake/EngineTesting.cmake`) by
`engine_deal_gpu_chains` (`cmake/EngineTestDeal.cmake`), which is the parts' deal — longest first,
each to the chain with the least so far, the unmeasured after them round the chains — over:

- **a part's seconds**: the files the parts' deal gave it, by the table's file rows;
- **any other test's seconds**: its whole-test row in `cmake/test-costs.txt`, `<test> * <seconds>`,
  written by `tools/test-costs.ps1 -Whole -Log <msvc-debug LastTest.log> -Label <gate>-msvc-debug
  -BuildDir build/msvc-debug` from the gate's `msvc-debug` suite — every test labelled `gpu` that
  is not a part, its `Test time` less the lock waits its own process printed;
- **tests that share another resource lock as one item**, their seconds summed: the desktop's
  tests (below) run one at a time whatever chain holds them, so splitting them over two chains
  would hold a chain idle behind the other.

The configure prints the deal, `GPU chains: 2 over 34 gpu-labelled tests (gpu_device.1 17 tests
2247 s, gpu_device.2 17 tests 2245 s by cmake/test-costs.txt; 0 unmeasured)` on d6534963.

**The desktop's lock is the desktop's alone.** Until T89 every end-to-end test held `e2e_apps`,
"windows, GPU devices and input devices, one of each". The GPU device is the machine-wide lock's
to order, so what the lock still guards is a window — the editor's cases post clicks and keys and
fail when another window takes the front (roadmap A40) — and an input device. `engine_app` labels
a test `desktop` when its app reaches `window`, and only that label takes the lock; `engine_cli`
(whose `engine-view` runs are all `--offscreen`), `engine_content`, `engine_host`, `mcp_bridge`,
`engine_image` and the `.generators` tests run beside the desktop's. A headless app's test that
starts an app with a window adds the label beside the line that names it, as `tools.game_sdk`
does. It is what lets the release app tests, all end-to-end, run as two chains at all: on gate b66
they were 2,384 s one after another, `engine_content` 504 s and `engine_cli` 353 s of it headless.

**What it gives back, replayed.** A replay of gate b66's two Windows suites that have the chain
(each test's measured seconds as its holds, one after another with its CPU work between them,
CTest's scheduler taking the longest test whose locks are free, one card shared first come first
served by the suite alone, so the 348 s and 560 s the suites waited for others are left out;
`simulate.py` in T89's scratch, 2026-11-16) — a prediction from one gate's seconds, which the next
gate measures:

| Suite | One chain (replayed / measured less its waits) | Two chains | The card's floor |
|---|---|---|---|
| `msvc-debug`, three or four jobs | 4,132 / 4,132 s | **2,695 s** | 2,036 s held |
| `msvc-release`'s app tests | 1,827 / 1,822 s | **1,017 s** | 524 s held |
| `msvc-debug` with the renderer's four parts run again as a weaker card (T162) | 5,173 s | **3,368 s** | 2,751 s held |

The one-chain replay reproduces the measured run to the second once its waits are taken off, which
is the check that the model holds. Jobs do not bind: two chains and the 2,155 s of CPU tests
finish in the same time at three jobs as at five, so the gate's three stand. With the chains the
card is the next bound — held 75% of the replayed debug suite, 82% with T162's run in it. (Once
T170 took 1,170 s of tests that open no device out of the chains, three jobs do bind; below.)

### One chain where a test outruns its deadline

**What gate b67's server leg showed** (2026-11-17, `linux-server` on the Titan Xp, 11:29 to 15:05,
two chains): `renderer.2`, `.3`, `.4` and `engine_view.3` skipped after 1,800 to 1,839 s, every
one on a give-up line reading `gave up waiting for the GPU lock after 0 s` — the wait had not
lasted, the **deadline** had passed. Every wait in a test's tree gives up at the test's start plus
`ENGINE_TEST_GPU_LOCK_WAIT_S` (1,800 s; [gpu_lock](../subsystems/gpu_lock.md#how-a-hold-works)),
an absolute time the test's children inherit, and it assumes a test's own run is short beside it.
On that card it is not: one chain at a time on gate b66b, `renderer.2` ran 1,882 s and `renderer.4`
1,918 s alone, which is why the server's presets give a test 7,200 s of work. With one chain a test
that runs past its deadline never waits after it, because nothing of its own run holds the card
beside it; with two, the other chain holds it about half the time, every test's holds take turns
with that chain's, and a test still running past its deadline gives up at its next ask. Beside the
renderer's parts `engine_view.3` went from 483 s to past its 1,800, so the two that skipped first
were each other's cause, and `water` ran 2,009 s against 1,624 alone.

**One chain was not enough by itself.** Batch 68's first run of the leg with one chain (T171,
2026-11-17 21:00 onwards) had `renderer.3` give up 2,141 s in: alone, a test that runs past its
deadline needs only one other holder at one of its asks after it — on that server another user of
the card, on this machine any agent — and gives up behind it however short its hold. So T171 has
two halves.

**The deadline is the queue's** ([gpu_lock](../subsystems/gpu_lock.md#how-a-hold-works)): each
process moves the deadline it inherited later by the time it has itself held the lock, which is the
test's work and never its queue. A test now gives up when its time off the card — its waits and
the CPU work between its holds — reaches the budget, wherever its own run has got to. Children
start from the deadline they inherit, so an end-to-end test's fifty children still share one
budget, and CTest's `TIMEOUT` (work + queue) still covers a give-up: the deadline moves only by
work.

**And the configure deals one chain where a second would spend the queue**: beside `n - 1` other
chains on a card that bounds the suite, a test's holds take turns with theirs, so it waits about
`(n - 1)` times its own card time, and with its CPU work between holds its time off the card is at
most `(n - 1) * card + cpu <= n * (card + cpu) <= n * WORK_S`. `auto` deals two chains only where
`n * WORK_S <= WAIT_S` holds for `n` = 2. With the defaults (600 and 1,800 s) every preset keeps its
two; the `linux-server` presets' 7,200 s of work give one, which is the one chain the server ran
green on until T89. The number is known at configure time because the presets already declare how
long a test there may work, and that declaration is what the card's speed decides; neither the card
(no device is open at configure time) nor the cost table (measured on the RTX 5090) could say it. A
whole number given for `ENGINE_TEST_GPU_CHAINS` overrides `auto`, with a warning when it breaks the
rule. A build tree configured before T171 has T89's number in its cache, which nobody chose, and its
first configure since reads it as `auto`. A second chain would buy the server little anyway: its
suite is the card's, as the one-chain legs show (11,737 s on b66b with every GPU test run).

## Tests that open no device

**The label `gpu` follows the link graph, not a device** (roadmap T170). `engine_module_tests` and
`engine_app` give it to every test whose module, test dependency or app reaches `gfx` or `window`,
because such a test *can* open a device and so wait for the machine-wide lock; and since T89 the
label is also a seat in a GPU chain, behind every other GPU test of the run. On gate b66 about
830 s of the `msvc-debug` chain was tests that never opened one (the hold log has none for them):
`rocks` reaches `gfx` for its formation packing, `engine_content` links the renderer for its scene
reader, `world` reads scenes with the renderer's CPU half. They waited in line for a card they
never asked for.

**Every `gpu`-labelled test of d6534963, and whether anything in its tree can open a device**, read
from gate b66's hold log (`ENGINE_GPU_LOCK_LOG`), its CTest log's per-case seconds attributed to
files, and every test file and app: the only places a device is made are `gfx::Device::create`'s
callers, which outside `domain/gfx` are the renderer's and the systems' GPU test files,
`engine-view`, `engine-editor` and `engine-host`'s `render.*` and `engine.budgets` (which opens the
render session's device for its memory row):

| Test | b66 s | Holds | Opens a device? | Now |
|---|---|---|---|---|
| `gfx.1`, `gfx.2`, `renderer.1`–`.4` | 82, 71, 405, 259, 392, 152 | 49, 45, 81, 50, 92, 133 | yes | `gpu`, parts |
| `engine_view.1`–`.4`, `.frame_loop`, `engine_editor`, `tools.game_sdk` | 246, 368, 347, 333, 213, 56, 68 | many | yes (`engine-view`, the editor) | `gpu`, desktop |
| `engine_host`, `mcp_bridge`, `mcp_bridge.mcp_check`, `.mcp_check_connect` | 67, 18, 11, 5 | 3, 1, 0, 0 | yes: `render.*` through `engine-host` (one file each; the checks' capture and benchmark) | `gpu` |
| `water` | 119 | 47 | nine of its sixteen files; 22 s of cases do not | `gpu`, whole (below) |
| `doc_scene`, `volumetrics`, `scatter` | 27, 11, 10 | 4, 6, 2 | one or two files each; 0.5, 0.2 and 5 s of cases do not | `gpu`, whole |
| `engine_cli` | 386 | 12 | `render_tests.cpp`, `console_tests.cpp`'s captured frame, `ops_tests.cpp`'s `engine.budgets`: 77 s of cases | split: `engine_cli.gpu`; `engine_cli` (308 s, `npc_replay_tests.cpp` 285 of it) in the CPU pool; since T184 `npc_replay_tests.cpp` is `engine_cli.npc_replay` (below) |
| `scene_collision` | 75 | 1 | `collision_draw_tests.cpp`: 0.8 s | split: `scene_collision.gpu`; `scene_collision` (72 s) in the CPU pool |
| `rocks`, `landform`, `world`, `caves`, `soil`, `protocol`, `audio`, `audio_system` | 331, 47, 37, 30, 7, 6, 4, 2 | 0 | no (`protocol`'s `gpu.adapters` enumerates adapters, which opens no device) | `NO_DEVICE` |
| `window` | 9 | 0 | no: its one Vulkan window holds a lock file in its own scratch directory | `NO_DEVICE` |
| `engine_content`, `engine_input` | 279, 11 | 0 | no: `engine-content` opens none and starts nothing that does; `engine-input`'s one window is hidden and not Vulkan | `E2E_NO_DEVICE` |

(`engine_input`'s two holds in the b66 timeline were another end-to-end test's children, attributed
by the time they ran; `engine-input` makes no `gfx::Device`.)

**Where it is said: the declaration.** `engine_module_tests(... NO_DEVICE)` and
`engine_app(... E2E_NO_DEVICE)` keep the label off a test that reaches `gfx` without opening a
device, so it runs in the CPU pool beside everything else. `DEVICE_FILES <basename>...` (and
`E2E_DEVICE_FILES`) splits a mixed test by file rather than dropping or skipping a case: the one
executable is two CTest tests, `<name>` with doctest's `--source-file-exclude` over those files and
no label, and `<name>.gpu` with `--source-file` over them and the label, the parts' anchored filter
both ways, so every case runs in exactly one. `water` was left whole: 22 s of its 116 s of cases
open no device, and its test list is another row's tonight; `doc_scene`, `volumetrics` and
`scatter` have under 5 s of such cases each, less than a split's second start-up is worth to read.
Not with `PARTS`; a name that is not a file in the directory's `tests/` stops the configure.

**Kept true both ways**, at run time, because a label is a claim about every process in a test's
tree and only the run sees the tree. The finalize (`cmake/EngineTesting.cmake`) gives every test
`ENGINE_TEST_DEVICES`, which its children inherit, and `gfx::Device::create` calls
`gpu_lock::note_device_open()` first, before it looks for an adapter
(`foundation/gpu_lock/device_hold.h`):

- **A test without the label that opens a device fails.** `forbidden`: the process prints
  `engine-test: <exe> opened a GPU device in a test that is not labelled gpu` and ends with 70. A
  test main was the wrong place for this half — a child (`engine-host` under `engine_cli`) is not a
  test executable — and a lint cannot see what a process started at run time does; the
  environment reaches both. Because it is checked at the call and not at a device, it holds on a
  machine with no driver too: the no-driver `test -Affected` of the branch ran every moved test
  through it.
- **A `gpu`-labelled test that never tried is reported.** `report`: the test main
  (`tests/support/test_main.cpp`) turns it into a marker file in a scratch directory of its own,
  `note_device_open` appends to it from any process of the tree, and a run that passed and left
  no file prints `engine-test: labelled gpu and opened no device`. A report and not a failure: a
  part may hold only CPU cases, a script test (`tools.game_sdk`, the MCP checks) has no test main
  to make the file, and a case may skip on a weaker card before it asks. The line is in the CTest
  log, and **`tools/test-costs.ps1 -NoDevice -Log <a gate's LastTest logs>`** collects it (batch
  68): one row a test that reported at least once, its parts folded in, with how many of its
  passing runs reported and in which logs, and `label can come off` when every one did or `mixed`
  when some did — so a label no longer needed shows up without anyone reading the logs. It reads
  only what the logs say: a failed run and a test the logs never ran say nothing. Gate b67's
  whole-suite `msvc-debug` log (2026-11-17, 166 passing tests) holds no such line.

**What it gives back, replayed** (`replay.py` in T170's scratch: T89's model unchanged over gate
b66's seconds — each test's holds with its CPU work between them, CTest's scheduler, one card
first come first served, the waits for other suites left out — the split tests' seconds taken from
gate b66's per-case lines; 2026-11-17, nothing flown):

| `msvc-debug` | Wall | `gpu_device.1` | `gpu_device.2` |
|---|---|---|---|
| One chain (before T89) | 4,132 s | 34 tests, ends 4,132 s | — |
| Two chains (T89) | 2,695 s | 17 tests, 1,861 s, ends 2,059 s | 17 tests, 2,619 s, ends 2,695 s |
| T170, three jobs | **2,405 s** | 7 tests, 1,630 s, ends 1,877 s | 16 tests, 1,707 s, ends 2,378 s |
| T170, four to six jobs | **2,262 s** | ends 1,875 s | ends 2,262 s |

The configure's deal is now `gpu_device.1 7 tests 2016 s, gpu_device.2 16 tests 1335 s`: the
desktop's tests are one item and fill a chain alone. At three jobs the two chains hold two slots
and the CPU pool, now 1,170 s larger, finishes last; at four the card's own floor (2,036 s held)
and the second chain's queue for it bind. So the gate's `msvc-debug` suite should run at
`-Jobs 4` once this merges, which the next gate measures.

**On a machine with no GPU and in a bundle.** Nothing that moved opens a device, so taking its
chain lock away can let no two devices meet, on a machine with the lock or without it; a split
runs every case once wherever it runs. With no driver `note_device_open` still runs (it is the
first line of `Device::create`), so `forbidden` fails a mislabelled test there as it would here,
and `report` sees the try even though no device follows. A bundle (`tools/package-tests.ps1`)
carries each test's arguments, so the split's filters travel, and not its environment: there
`ENGINE_TEST_DEVICES` is unset and both checks do nothing, which is right for a machine whose run
has no chains to protect. Outside CTest — an author running a test executable by hand — the same.

## Checks on events, not seconds

A check that passes alone and fails in a full run is a race or a clock until shown otherwise
(roadmap T184), and the mend is to wait for the event the assertion is about, never to lengthen the
sleep or loosen the assertion. A deadline may stay, to end a case that would otherwise hang; it is
never what the case measures. What batch 68's runs found, with each check before and after:

| Check | Before | After |
|---|---|---|
| `tools.slot`, "reset refuses while a process runs from under the slot" | the sleeper given 800 ms to start | waits until slot.ps1's own question (a process whose executable is under the slot) finds it; the same refusal, the same unmoved HEAD |
| `tools.slot`, "and resets once it has ended" | kill, then at most 5 s for the process to end | waits until that question finds nothing; the same reset, the same new HEAD |
| `tools.machine_lock`, "a build that releases and asks again is served after the build that was waiting" | the first hold 3 s, in which a second pwsh had to start and stand in the line; every hold seen by a look every 20 ms at holds of 0.3 s | the first hold ends only once the second build stands in the line (`linux-build.ps1 -LockOnlyUntil <file>`), and every hold ends only once the test has seen it; the same order, by pid and by hold |
| `tools.machine_lock`, "and are served in that order" (three builds) | holds of 0.31 to 0.33 s against the same 20 ms look | each hold ends once seen; the same order, by pid and by hold |

**The slot reset's own race was in the script, not the test.** `slot.ps1` asked
`$_.Path -and $_.Path.StartsWith(...)` of every process, and `Path` is a script property
(`$this.MainModule.FileName`) that asks the process again on each read: one that exits between the
two reads answers null the second time, the call on null stops the script, and `reset` exits 1. On
this machine with the rows' compilers starting and ending, the two-read form threw on 1 of 60
enumerations of the process table and the one-read form on none (2026-11-18, about 1.9 s an
enumeration). It reads `Path` once now, and so does the test's own question.

`mcp_bridge`'s "a host that stops answering is stopped at the deadline and replaced" held every
quick call to the one-second deadline it tests, a fresh debug host's first `engine.info` included,
which at 100 % CPU took longer and replaced the host before the case began. The hung method is still
held to that second (the same message, `timed_out`, waited 0.9 s and under 30); a quick call the
deadline stopped on a host still starting is asked again and counted, and the generations add each
such replacement in.

**`engine_cli`'s CPU half was over its limit on a loaded machine, and the time is two cases.**
`npc_replay_tests.cpp`'s 10^5 residents and its 2 * 10^4 crossing the simulated edge, each
generated, streamed, saved, loaded and replayed in three hosts, took 467 s and 115 s of the half's
645 s here on 2026-11-18 at 100 % CPU (the rest 63 s; the 10^5 world's generation 25 s, its save
77 MB), and 594 s and 220 s in other rows' runs that day, against the 600 s every end-to-end test
has. Shortening them is an edit to cases other rows held that day (three hosts that could be two at
once: the replay from tick 0 needs nothing from the continuous run), so the file is a test of its
own, `engine_cli.npc_replay` (`engine_app(... E2E_OWN_FILES <file> E2E_OWN_TIMEOUT <s>)`, beside
the device split), whose limit is twice the worst seen, 1,650 s; `engine_cli` keeps the other 22
cases at 600 s, and the three tests hold the suite's 55 cases between them, each once.

`scene_collision`'s wall-clock check is another row's (T184 left it).
