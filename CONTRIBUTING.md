# Contributing

A contribution to this tree is expected to be made with a model's help, so this page is written for the model and for the person directing it alike. Read [AGENTS.md](AGENTS.md) first: it is the entry point every session reads, and it links the plan, the decisions, the subsystem pages and the rules the build checks. This page adds what a change from outside has to carry so that its value can be judged before anyone spends an hour reading it.

## What a pull request says

A pull request is judged from its description first and its diff second. The description is written so that a maintainer, or a model reviewing for one, can decide whether the change is worth a review without opening a file. It has these sections, in this order; [.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md) prefills them.

1. **The problem, with a number.** What is wrong or missing, where it shows (a scene, a path, a test, a build), and the measurement that shows it: a frame's milliseconds per pass, a build's seconds, a file's bytes, a test's count. A problem without a number is a claim; a problem with one is a baseline.
2. **The change, in a paragraph.** What was done and why that way, naming the modules touched and the plan section, ADR or page the design follows. A choice between approaches says what the others were and why they lost. Anything that stands for something real names the research it starts from ([AGENTS.md](AGENTS.md), "Rules that are reviewed").
3. **The numbers after.** The same measurements, taken the same way on the same machine, with the machine's state quoted (`--require-quiet`, `--wait-quiet`, or the `machine_state` the harness recorded; [bench](docs/subsystems/bench.md), "Measuring on a shared machine"). Before and after side by side, in a table. A change to a hot path quotes its bench rows before and after (`tools/dev.ps1 bench -Filter <module>*`); a change to the renderer quotes a flythrough (`engine-view --benchmark`) or the frame budgets (`tools/frame-budget.ps1`); a change to the content build quotes a build's time and its section hashes; a change to a size-tabled type says the old and the new size and why.
4. **What it reaches.** Every public header, file format, GPU-mirrored struct, CLI flag, build switch, protocol method and test expectation the change touches, each with the page updated in the same commit ([AGENTS.md](AGENTS.md), "Documentation moves with the code"). "Docs: none" states that nothing on that list changed; it is not an omission.
5. **Refused by a check.** Every test, lint, golden, size-table entry or budget the change failed on the way, and what was done about it. A check is answered by fixing the code; a relaxed check, a re-pinned golden or a widened tolerance is named here with its reason, and it is the first thing a reviewer reads.
6. **Behavior changes.** What draws, computes or serializes differently after the change, and for whom. A change that is byte-identical says so and names the test that proves it.
7. **What was run.** `tools/dev.ps1 test -Affected` in `msvc-debug`, and for C++ the Linux container with the same modules (`tools/linux-build.ps1 -Test -Filter <regex>`), with their results. The whole suites run at the merge, not on the branch ([what to run, and why](docs/ci/what-to-run.md)).

A description that quantifies nothing is sent back for the numbers before the diff is read. A description can be wrong, of course; the merge gate runs everything again, and a number the gate contradicts ends the review.

## What the change itself carries

- The rules [AGENTS.md](AGENTS.md) lists under "Rules that are checked" are checked by the build, the lint and the documentation check; a pull request that fails them is not reviewed until it passes. `tools/dev.ps1 lint` and `tools/dev.ps1 docs` run them locally.
- A new piece of work has a row in [docs/roadmap.md](docs/roadmap.md) before it starts and names its commit when it is done. A decision a future contributor would be surprised by gets an ADR (`docs/adr/0000-template.md`).
- Commit messages: an imperative subject under 72 characters and a body that says why. A commit that touches `include/`, `shaders/`, `schemas/`, an app's `main.cpp`, `cmake/` or `.github/` touches a `.md` too, or carries a `Docs: unchanged (<reason>)` trailer.
- No binary assets, derived data or generated images in the tree; `tools/fetch-samples.ps1` and the derived-data cache are the pattern.
- A dependency is permissive-licensed and recorded in `third_party/LICENSES.md` ([ADR-0014](docs/adr/0014-apache-2-license-and-dependency-policy.md)).
- Nothing in a change listens on a non-loopback address, opens a dialog on failure, or names the system temp directory from a test ([AGENTS.md](AGENTS.md), the checked rules).

## Models

Contributions made with a model are expected and welcome. The project's policy for models at development and at run time is [docs/plan/12-ai-usage-policy.md](docs/plan/12-ai-usage-policy.md): a model's output is content and code under the contributor's name and the project's license, never training data, and the contributor answers for it as for their own. Naming the model that did the work is welcome; the description is held to the same standard either way.

## License

The project is Apache-2.0 ([LICENSE](LICENSE)), and a contribution is made under it.
