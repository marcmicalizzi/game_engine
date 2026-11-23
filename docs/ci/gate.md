# The gate script

The merge gate as a script of the tree (roadmap T217): what it runs, in what order, how it reads a verdict, and the step after it. When to run what, and why the gate is shaped round the GPU lock, is [what to run, and when](what-to-run.md); this page is the script.


Until batch 73 the coordinator gated every batch with scripts written again each time in a scratch
folder — a gate, a re-gate, a tail, a Linux leg, a server leg, a suite runner, an after-step — and
each batch's copy carried the lessons of the one before as comments. T217 made them one script of the
tree, so a lesson is a line and a test here rather than a comment in a file that is gone next week.

```powershell
pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73 -Logs <dir> -Server <ssh alias>      # a batch's gate
pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73b -Logs <dir> -Reach '<regex>' -Skip listeners,budgets,weaker-card   # a follow-up
pwsh tools/after-gate.ps1 -Gate <gate worktree> -Main <primary checkout> -Verdict <dir>/b73-gate.verdict.json -CopyTo <dir> [-Push]
```

`-Logs` must be outside the gate worktree (the script refuses one inside it, exit 2). Every step is a
process of its own with its whole output in `-Logs`, and the gate starts nothing through `cmd` or a
Unix shell. In order:

1. **The Linux leg and the server leg start first.** The Linux leg runs the three containers
   (`linux-gcc-release`, `linux-clang-debug`, `linux-clang-minimal`) one after another, beside
   everything below, and is waited for at the end. The server leg (`-Server`, `remote-build.ps1
   -Test`) starts detached, writes `<batch>-server.verdict.json` itself when it ends, and never holds
   the gate's verdict: the summary prints its result if it has one, and where to read it if not.
2. **The documentation check and the lint**, then **the four Windows builds** (configured when a
   preset has no build tree; three attempts each, each after the short wait for memory described in
   [how long things take](what-to-run.md#how-long-things-take)). A failed check or build runs no suite.
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
   of them, or the Linux leg, out by name, and the summary says what was skipped.
6. **The integrity read, the litter check, the summary**, and `<batch>-gate.verdict.json`: green,
   the tip it judged, each leg's verdict, every second look, every fault.

**Verdicts that cannot lie**, each held by a case of `tools/gate.Tests.ps1` (CTest `tools.gate`),
which runs the real script over a throwaway repository whose `tools/` are stand-ins:

- **A suite's verdict is read from the CTest logs it kept**, never from a console. `tools/dev.ps1`
  keeps each pass's `LastTest.log` under a name of its own (`tools/lib/CTestLogs.psm1`); the gate
  copies the ones a run added into `-Logs` and reads them record by record (`tools/lib/Gate.psm1`).
  A console that said "100% tests passed" over a failed record is red, and so is a run whose exit
  code its own logs do not explain (an 8 over passed records, a 1 with no log). b71's weaker-card
  leg read "NO VERDICT" beside a green run because a `*>` in the gate's own process caught
  `dev.ps1`'s one line and none of CTest's.
- **CTest's footer is not a verdict for a skipped test**: CTest writes "Test Passed." for a test its
  console calls ***Skipped (2026-11-21). A record that says "gave up waiting for the GPU lock" and
  carries no doctest summary reads as gave-up whatever its footer says.
- **A leg that left no verdict fails the summary**: a suite that kept no CTest log, a Linux leg that
  did not finish its verdict file or reported fewer than three containers. A container is green only
  on exit 0 with CTest's own "0 tests failed out of N", N above none, in its transcript.
- **The integrity read**, over every kept log in the order written: a record that gave up with no
  summary needs a later record of the same test in the same preset with a verdict of its own; a
  `[doctest] test cases: 0` record is a fault except the three that are so by design
  (`engine_cli.npc_replay` in `msvc-minimal` and `msvc-no-ecs`, `engine_view.frame_loop` in
  `msvc-minimal`); a record cut off before its footer is a fault.
- **The gate worktree gains nothing**: its root's entries and its untracked files are listed before
  and after, and anything new other than `build/` and `ddc/` is named as LITTER and fails the gate.
  Batch 72's gate left a file named `$null` at the gate worktree's root once. No line of that
  batch's scripts writes one (every `2>$null` in them, and in `tools/`, is PowerShell's own, in a
  PowerShell process); their logs say the worktree was clean at 14:24 on 2026-11-22, so it came
  after: from a step running with the worktree as its directory (the after-step's SDK export ran
  there, and every `tools.*` test runs with the source root as its working directory) or from a
  command typed in another shell (`>$null` in `cmd`, or `>\$null` in bash, makes exactly that
  file). Now it is caught, by name, on the gate that makes it.

**After the gate** (`tools/after-gate.ps1`): it refuses (exit 2) unless the verdict file says green
and names the commit the gate worktree is at, and main can fast-forward to it. Then main is
fast-forwarded (never a merge commit), the release binaries are copied out whole (the next batch's
"main" side of a pair), installed into the local folder's `bin-interactive` (a file in use by a
running session is reported as NOT INSTALLED and left; nothing is forced and no process is
stopped; each installed file is compared with its source by hash), the SDK is exported from the gate
worktree, and the structure raises are folded on main with `tools/structure-check.ps1 -Rebaseline`
and committed. **It pushes only with `-Push`.**

**What the scripts deliberately do not do**: take or wait on the GPU lock (every test takes it,
ADR-0049); stop a process, theirs included; reset, stack or cherry-pick the gate worktree (the
coordinator's, before the gate); decide a merge; retry a failed test; read a pass from a console;
write inside the gate worktree other than through the tree's own tools; push without being told to.
