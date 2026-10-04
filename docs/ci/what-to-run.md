# What to run, and when

The rule is in [AGENTS.md](../../AGENTS.md#what-to-run-and-when); this page is why it is the rule and
what it was measured against.

## The two places a change is tested

| Where | Who | What runs | Why there |
|---|---|---|---|
| **Before a branch is handed over** | whoever wrote it | `tools/dev.ps1 test -Affected` in `msvc-debug`; `tools/dev.ps1 lint` and `docs` (both are in that run); `tools/linux-build.ps1 -Preset linux-clang-debug -Test -Filter <the same modules>` when C++ changed | It answers "did I break what I touched, and what is built from it". It does not answer "does the tree still pass", and is not asked to. |
| **At the merge** | whoever merges | the three Windows suites (`msvc-debug`, `msvc-minimal`, `msvc-no-ecs`), the GCC container, and the GPU server (`tools/remote-build.ps1 -Host titanxp -Preset linux-server -Test`), on the branch rebased onto `main` | It is the only place the integrated tree exists, the only place the capability graph's two off-configurations are exercised, and the only place a second GPU and a second compiler see the change. |

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
- Documentation selects nothing; the lint and the documentation check run in every case.

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

A first build in a fresh worktree comes on top of these, and a fresh worktree's first container
run compiles every third-party dependency ([local Linux builds](local-linux.md) has those times).
When a figure here is found wrong by more than a third, correct it in the change that found it.
