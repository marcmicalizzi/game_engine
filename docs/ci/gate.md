# The gate script

The merge gate as a script of the tree (roadmap T217): what it runs, in what order, how it reads a verdict, and the step after it. When to run what, and why the gate is shaped round the GPU lock, is [what to run, and when](what-to-run.md); this page is the script.


Until batch 73 the coordinator gated every batch with scripts written again each time in a scratch
folder — a gate, a re-gate, a tail, a Linux leg, a server leg, a suite runner, an after-step — and
each batch's copy carried the lessons of the one before as comments. T217 made them one script of the
tree, so a lesson is a line and a test here rather than a comment in a file that is gone next week.

```powershell
pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73 -Logs <dir> -Server <ssh alias>      # a batch's gate
pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73b -Logs <dir> -Reach '<regex>' -Skip listeners,budgets,weaker-card   # a follow-up
pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73c -Logs <dir> -Completes <dir>/b73b-gate.verdict.json   # completes a verdict of the same tip (T220)
pwsh tools/after-gate.ps1 -Gate <gate worktree> -Main <primary checkout> -Verdict <dir>/b73-gate.verdict.json -CopyTo <dir> [-Push]
```

## Flags and exit codes

| `tools/gate.ps1` | |
|---|---|
| `-Root <dir>` | the gate worktree; required. It must be **clean at its HEAD**: no tracked change, no untracked file git does not ignore, no unfinished merge, cherry-pick or revert. The verdict names that commit, so a tree that is anything else is refused |
| `-Batch <name>` | names every log and the verdict, `<Logs>/<Batch>-gate.verdict.json`; required |
| `-Logs <dir>` | where everything is kept; required, and outside `-Root` (checked before the directory is made) |
| `-Reach <regex>` | a follow-up: wave one runs `^(<regex>)$`; release keeps the apps' set |
| `-Skip <names>` | any of `linux`, `listeners`, `budgets`, `weaker-card`, comma-separated or as a list; any other name is refused |
| `-Server <ssh alias>` | starts the GPU server leg, detached |
| `-Jobs <n>` | each suite's `tools/dev.ps1 test -Jobs` (default 3) |
| `-WeakerCard <profile>` | the weaker card's `ENGINE_GFX_TEST_DEVICE` (default `titanxp`) |
| `-MemoryWaitMinutes <n>` | the longest wait for memory before a configure or a build attempt (default 15); a timeout proceeds and is kept in the verdict (`memory_wait_timeouts`) |
| `-BudgetWaitQuiet <s>` | the frame budgets' `-WaitQuiet`, the longest wait for a quiet machine before each run is flown (default 120) |
| `-Completes <verdict>` | a follow-up that completes an earlier verdict of the same tip: it runs no check or suite, only the earned steps that verdict lacks or holds inconclusive, after the release build when one of them flies its binaries (below); no `-Reach` with it, and never the verdict this run would write |
| `-SamplesFrom <dir>` | when `content/samples/Lantern/Lantern.glb` is missing, the entries of `<dir>` are copied into `<Root>/content/samples` (git-ignored) before the budgets: **the one write the gate itself makes inside `-Root`** |

Exit **0** green; **1** red (the summary names every reason); **4** nothing red and a step inconclusive: the budgets over only on a busy machine twice, or a first flight that exits 6 (a run compared with nothing for its session state) whether or not a busy run was flown again, or a step a follow-up `-Skip`s that its earlier verdict holds inconclusive; the summary names every step that is not green or skipped, by state, and `-Completes` completes it; **2** refused, before anything runs:
an unknown `-Skip` name, a `-Root` that is not a git worktree or is not clean at its HEAD (the
refusal lists what is in the way), a `-Logs` inside `-Root`, a `-Completes` verdict that cannot be
completed ([below](#a-follow-up-that-completes-a-verdict)), a `-Completes` naming the verdict this run would write (refused before the set-aside below, so that file keeps its name: give the follow-up a `-Batch` of its own). A run that is refused or stops
part-way leaves no verdict of that batch name: the last `<Batch>-gate.verdict.json` in `-Logs` is
renamed `<Batch>-gate.verdict.previous-<stamp>.json` before anything else can fail, so no older
green outlives it where `tools/after-gate.ps1` would read it.

| `tools/after-gate.ps1` | |
|---|---|
| `-Gate <dir>` | the gate worktree; required. Clean at its HEAD (as above), and its HEAD the verdict's commit |
| `-Main <dir>` | the primary checkout; required, on `main`, and an ancestor of the gated tip (or already holding it, a second run) |
| `-Verdict <file>` | the gate's `<batch>-gate.verdict.json`; required, and green |
| `-CopyTo <dir>` | where `build/<Preset>/bin` is copied whole; required. A directory this script wrote before (it holds `.after-gate-copy`) is cleared first; one that holds anything else, or overlaps the gate worktree or main, is refused as a failed step, and nothing in it is touched |
| `-Install <dir>` | default `<local folder>/bin-interactive` |
| `-Sdk <dir>` | default `<local folder>/sdk/<short sha>` |
| `-Preset <name>` | default `msvc-release` |
| `-Trailer <line>` | appended to the fold's commit message |
| `-Push` | pushes main, and only when every step before it did what it says |

Exit **0** every step did what it says; **1** one did not (each named, and `-Push` not taken);
**2** refused, nothing done: no verdict file, a verdict that is not green (the refusal names the red list and every step that is neither green nor skipped, by state), a gate worktree that is not clean or
not at the verdict's commit, a main not on `main`, a main the gated tip does not descend from.

## What it runs

Every step is a process of its own with its whole output in `-Logs`, and the gate starts nothing
through `cmd` or a Unix shell. In order:

1. **The Linux leg and the server leg start first.** The Linux leg runs the three containers
   (`linux-gcc-release`, `linux-clang-debug`, `linux-clang-minimal`) one after another, beside
   everything below, and is waited for at the end. The server leg (`-Server`, `remote-build.ps1
   -Test`) starts detached, writes `<batch>-server.verdict.json` itself when it ends, and never holds
   the gate's verdict: the summary prints its result if it has one, and where to read it if not.
   **It inherits no handle of the gate** (T231). Until 2026-11-25 it was started with its log
   redirected, and a redirect makes Start-Process create the child with handle inheritance on, so
   the leg and the ssh it runs held the launching shell's redirected output, and a shell waiting on
   that redirection waited three hours past a verdict already written (batch 75). A hidden window
   or `-NoNewWindow` changes nothing; on Windows, `Start-GateProcess -Detached` starts a pwsh of its
   own through `Start-Process` with no redirect and no `-NoNewWindow`, which is ShellExecuteEx and
   creates it with inheritance off, and that pwsh starts the leg with the two log files and waits
   for it, so the id the gate prints lives as long as the leg. `tools.gate` holds it: a stub leg
   that sleeps 20 s, started by a launcher whose output is a pipe, and the pipe ends while the stub
   still runs (measured with a 12 s stub: the pipe ended at 3.6 s detached, and at 18.7 s, with the
   stub, started the inheriting `-NoWait` way). The gate's
   last line ends `| verdict written: <file>` and, while the leg runs on, `| the server leg trails
   (<id>), its verdict will be <file>`, so a reader of the output knows the gate is done. WMI's
   `Win32_Process.Create` would detach as well, but its process takes the user's default
   environment rather than the gate's, and a `cmd /c` with redirection would have to quote every
   argument for cmd's metacharacters. On Linux the leg is started as `-NoWait` is.
2. **The documentation check and the lint**, then **the four Windows builds** (configured when a
   preset has no build tree; three attempts each, each after the short wait for memory described in
   [how long things take](what-to-run.md#how-long-things-take)). A failed check or build runs no suite.
   **Every attempt is in the verdict** with its exit code and its log (`steps.builds.<preset>`), and
   a build that passed on a retry is named (`passed_on_retry`, `retried_builds`, and its console
   line): still green, never folded into a plain pass.
3. **The suites in two waves**, each through the tree's own `tools/dev.ps1 test -Jobs 3`, the two
   off-configurations with `-SkipInvariantGpu` ([the gate's wall clock](what-to-run.md#the-gates-wall-clock-t77)):
   `msvc-debug`, `msvc-minimal` and `msvc-no-ecs` together, then `msvc-release`'s end-to-end set
   alone. On batch 72 four suites at once put `engine_view`'s long parts past the GPU lock's
   30-minute deadline, twice. `-Reach <regex>` is a follow-up's: wave one runs `^(<regex>)$`, and
   release keeps the apps' set, which is what it is in the gate for.
4. **Second looks, once and alone, after the whole wave has ended**: a test that gave up waiting for
   the card (exit 75) runs again by name, and `engine_editor` does when it is a suite's only failure
   (its end-to-end cases depend on which window the desktop has in front, A40). Nothing else is run
   again: a failure is a verdict. Each second look is named in the suite's line and the verdict file.
5. **Only when every suite is green**: the listener check, the frame budgets, and the weaker card's
   leg (the renderer's parts, `water` and `engine_cli.gpu` as a Titan Xp, T162). `-Skip` leaves any
   of them, or the Linux leg, out by name, and the summary says what was skipped. Each that fails
   is red by its name (`listeners`, `budgets`, `weaker card msvc-debug`). **The budgets have three
   states** (T220): `tools/frame-budget.ps1 -WaitQuiet <-BudgetWaitQuiet> -Out <Logs>/<batch>-budgets-1`
   exits 0 (green), 1 (a run over on a quiet machine, a failed run: red), 4 (over only on a busy
   machine) or 6 (a run compared with nothing for its session state: inconclusive). On a 4 the runs
   its `report.json` holds busy are flown **once more, by themselves** (`-Run <ids>`, into
   `-budgets-2`): within budget then is green, busy again (4 or 6) is **inconclusive**, over on a
   quiet machine is red. A 4 whose report names no busy run is red (the code and the report
   disagree). A quiet over is never flown again: a check is not retried into passing. Every flight
   is in the verdict (`budgets.flights`). The frame thread's `cpu_ms` median is judged whole; each
   run's line also says the flight's slow share and its two modes' medians, which decide nothing
   (T221: a judge by the fast mode alone was tried and refused as a check made weaker;
   [bench](../subsystems/bench.md), "The frame thread's two modes"). Each flight's frame thread is
   pinned to one logical CPU by `tools/frame-budget.ps1` (`-FrameCpu auto`), which takes the slow
   mode out of what is measured; a pin that cannot be applied or does not hold makes the run busy,
   flown again like any busy run ([bench](../subsystems/bench.md), "The frame thread pinned").
6. **The integrity read, the litter check, the tree check, the summary**, and
   `<batch>-gate.verdict.json`: green, the tip it judged, each leg's verdict, every build attempt,
   every second look, every fault, and **each step's state and source** (`states`, `sources`,
   `inconclusive`): `checks`, `builds`, `suites`, `integrity`, `tree`, `listeners`, `budgets`,
   `weaker-card` and `linux`, each `green`, `red`, `inconclusive`, `skipped` (by `-Skip`) or
   `not run` (not earned). The Linux leg, which runs beside everything, is red or green by what it
   did even when the suites are red, and `not run` only when it did not run. The verdict is green
   only when every state is green or skipped.

## A follow-up that completes a verdict

Batch 73's two follow-up gates were green in everything but the frame budgets, which exited 4 while
another project's build ran (2026-11-23); flying them again meant the whole release set again, and
`after-gate.ps1` took only a green verdict, so the merge was taken by hand. `-Completes <verdict>`
is the form for that:

- It names an earlier verdict of **the same HEAD on a clean tree**: refused (exit 2) when the
  verdict is for another commit, when its tree changed or gained litter while it ran, when it
  records no step states (a verdict from before T220), and when the tree is not clean now (as any
  gate). Nothing folds across two HEADs or a dirty tree.
- **A red step is a verdict**: a verdict holding any step red, or whose own `red` or `no_verdict`
  list is not empty, is refused, whatever the step, so a budget over on a quiet machine stays red
  and no follow-up turns it green. Its checks, builds,
  suites, integrity and tree must be green, since a follow-up runs none of them.
- It runs only the earned steps (`listeners`, `budgets`, `weaker-card`, `linux`) the verdict holds
  inconclusive, not run, skipped or lacks, less what its own `-Skip` names, and is refused when that
  leaves nothing. A step it `-Skip`s keeps an earlier `inconclusive` (never promoted, so the folded
  verdict exits 4) and is otherwise recorded `skipped`, from this run, as a plain gate records a
  skipped step. A step it runs that comes back red makes the folded verdict red (exit 1), and
  `after-gate.ps1` refuses it.
- **It flies the tip's binaries, and proves it**: before the first step that runs a release binary
  (the listener check, the budgets) it runs the release build the plain gate runs
  (`tools/dev.ps1 build -Preset msvc-release`, configured if need be, three attempts), a no-op when
  `build/msvc-release` is current and a rebuild of this commit when it is not (a gate worktree moved
  to another tip, built and moved back passes the HEAD check, and nothing else would see it). The
  build is in the verdict (`steps.follow_up_build`) and in `builds`' sources; a build that fails is
  red (`build msvc-release`, exit 1) and nothing earned is flown over what it left. The weaker
  card's leg builds through `tools/dev.ps1 test` and the Linux leg in its containers, as in a plain
  gate.
- It writes one **folded verdict** under its own batch name: the earlier verdict's record of what it
  does not run again, this run's of what it does, `completes` naming the earlier file, the earlier
  verdict's `reach` (its suites ran under it, and the start line says so), and
  `sources.<step>`, a list of `{verdict, batch, started, ended}` for each step (integrity and tree
  name both runs, since both read their own logs and tree). It is green only when every step is.
  `tools/after-gate.ps1` reads it like any other and prints where each step came from; it refuses an
  inconclusive one and names what `-Completes` can complete.

## Verdicts that cannot lie

Each rule is held by a case of `tools/gate.Tests.ps1`, which runs the real script over a throwaway
repository whose `tools/` are stand-ins:

- **A suite's verdict is read from the CTest logs it kept**, never from a console. `tools/dev.ps1`
  keeps each pass's `LastTest.log` under a name of its own (`tools/lib/CTestLogs.psm1`); the gate
  copies the ones a run added into `-Logs` and reads them record by record (`tools/lib/Gate.psm1`).
  A console that said "100% tests passed" over a failed record is red, and so is a run whose exit
  code its own logs do not explain: an 8 or a 1 over passed records, a 1 with no log, and **a 75
  whose logs hold no record that gave up waiting for the card** (`dev.ps1` exits 75 for any skipped
  test, and a skipped record whose give-up line is missing reads as passed below, so a 75 taken on
  trust would be a test that never ran read as a pass). b71's weaker-card leg read "NO VERDICT"
  beside a green run because a `*>` in the gate's own process caught `dev.ps1`'s one line and none
  of CTest's.
- **CTest's footer is not a verdict for a skipped test**: CTest writes "Test Passed." for a test its
  console calls ***Skipped (2026-11-21). A record that says "gave up waiting for the GPU lock" and
  carries no doctest summary reads as gave-up whatever its footer says.
- **The footer has two spellings**: a test judged by its exit code ends "Test Passed." or "Test
  Failed.", and one judged by a regular expression (`PASS_REGULAR_EXPRESSION`: the four hosts'
  `.generators` tests) ends "Test Pass Reason:" or "Test Fail Reason:" with the reason on the next
  line, and never the first form. The read knows both. Batch 73's first gate knew one, and called
  those four tests cut off in three suites that had passed them (2026-11-23).
- **A leg that left no verdict fails the summary**: a suite that kept no CTest log, a Linux leg that
  did not finish its verdict file (one that died part-way never writes `done`; a half-written file
  reads as none) or reported other than three containers (`Read-LegVerdict`). A container is green
  only on exit 0 with CTest's own "0 tests failed out of N", N above none, in its transcript.
- **The integrity read**, over every kept log in the order written: a record that gave up with no
  summary needs a later record of the same test in the same preset with a verdict of its own; a
  `[doctest] test cases: 0` record is a fault except the three that are so by design
  (`engine_cli.npc_replay` in `msvc-minimal` and `msvc-no-ecs`, `engine_view.frame_loop` in
  `msvc-minimal`); a record cut off before its footer is a fault.
- **The verdict's commit is the tree it tested.** The gate refuses a `-Root` that is not clean at
  its HEAD, and at the end a tracked file changed, a merge begun or HEAD moved during the run is
  red (`tree changed`); `tools/after-gate.ps1` refuses a gate worktree that is not clean or not at
  the verdict's commit. Before this, a tracked change or an unfinished merge in the gate worktree
  was invisible to both scripts and "green at `<tip>`" could name a commit whose tree it did not
  test; the primary checkout stood in exactly that state (two unmerged pages) on the morning of
  batch 73.
- **The gate worktree gains nothing**: its root's entries and its untracked files are listed before
  and after, and anything new other than `build/` and `ddc/` is named as LITTER and fails the gate.
  A `git status` that fails is a fault (the tree could not be read), never an empty list.
  Batch 72's gate left a file named `$null` at the gate worktree's root once. No line of that
  batch's scripts writes one (every `2>$null` in them, and in `tools/`, is PowerShell's own, in a
  PowerShell process); their logs say the worktree was clean at 14:24 on 2026-11-22, so it came
  after: from a step running with the worktree as its directory (the after-step's SDK export ran
  there then, and every `tools.*` test runs with the source root as its working directory) or from
  a command typed in another shell (`>$null` in `cmd`, or `>\$null` in bash, makes exactly that
  file). Now it is caught, by name, on the gate that makes it, and the SDK export runs elsewhere
  (below).

## After the gate

`tools/after-gate.ps1` refuses (exit 2) as the table says. Then main is fast-forwarded (never a
merge commit), the release binaries are copied out whole (the next batch's "main" side of a pair,
into a `-CopyTo` that holds this copy and nothing older), installed into the local folder's
`bin-interactive` (a file in use by a running session is reported as NOT INSTALLED and left;
nothing is forced and no process is stopped; each installed file is compared with its source by
hash), the SDK is exported from the gate worktree **with `<verdict's folder>/<batch>-sdk-export.cwd`
as its working directory**, never the gate worktree (whatever a relative write of it leaves is
named there and joins no gate's baseline), and the structure raises are folded on main with
`tools/structure-check.ps1 -Rebaseline` and committed. **It pushes only with `-Push`, and only when
every step before it did what it says.**

## The two CTest tests

`tools/gate.Tests.ps1 -Part unit` is CTest `tools.gate`, in every configuration: the module's
functions over fixture logs and verdict files, and the refusal that starts one process (2.4 s on
the run that mended it). `-Part scenarios` is `tools.gate_scenarios`, registered in `msvc-debug`
only (`cmake/EngineTesting.cmake`): the scripts are the same in every configuration, and the
scenarios are the time — eleven gate runs over the stand-in repository (each a few dozen pwsh
processes), eight refusals and eleven after-gate runs: 228 s on 2026-11-23 with T220's four runs
(127 s and 202 s before them), the machine shared with other rows' builds each time. With neither, the file runs both. The wave case proves
side by side with a barrier, not a clock: each wave-one stand-in ends only once all three have
started (a deadline of 60 s, logged as `barrier-timeout` if it passes), so a loaded machine slows it
and cannot fail it.

## Known limits

- **A follow-up whose reach exists only where a capability is on** gives the off-configuration
  suites no records, so they read "no verdict" and the gate cannot be green; `-Skip` takes no suite.
  Fail-safe (never a false green): gate such a follow-up with a reach that every configuration has,
  or read the two suites' "no verdict" lines against the reach by hand.
- **No leg has a deadline**: a hung step (a suite, a container, the Linux leg) hangs the gate, which
  then never writes a verdict. Fail-safe (never a false green); a gate that has not ended in twice
  [how long things take](what-to-run.md#how-long-things-take)'s figure is looked at by hand.
- **A test whose own output holds a line that begins `N/M Testing: `** (a tool test printing a CTest
  log) splits its record when the gate reads it: the real record loses its footer and reads as cut
  off. Fail-safe (a false red).
- **The budgets' quiet reading is two looks** (T230): other programs' share of the machine is read
  at a flight's start and end, and 4 to 5 % of the GPU in the desktop's hands reads quiet while it
  puts 0.3 to 0.7 ms on the p99 of the passes that run beside it, which is the whole p99 allowance.
  Batch 75's follow-up gate was red by this alone (2026-11-25: one run over on three p99s, its
  medians on the baseline, and the same tail in the previous batch's binary flown back to back).
  Fail-safe (a false red). The step is still never flown again into a pass: a red budget is read
  against the previous gated binary flown beside the tip, both in the record, and a merge past it
  is the owner's to take.

**What the scripts deliberately do not do**: take or wait on the GPU lock (every test takes it,
ADR-0049); stop a process, theirs included; reset, stack or cherry-pick the gate worktree (the
coordinator's, before the gate); decide a merge; retry a failed test; read a pass from a console;
write inside the gate worktree other than through the tree's own tools and the `-SamplesFrom` copy;
push without being told to.
