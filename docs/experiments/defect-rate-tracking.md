# Defect-rate tracking per phase (roadmap T14): a design

Status: designed, nothing built (2026-11-19). The definition below is the owner's to accept; the tally it ends in is a proposed roadmap row, unnumbered until it is taken.

## Why it exists

The plan names agent-written C++ as a risk to everything ([10 §10.4](../plan/10-roadmap-risks.md#104-major-technical-risks): "measure per phase; Rust as the fallback") and asks for one number: sanitizer findings per thousand lines of agent-written code per phase ([08 §8.1](../plan/08-toolchain.md#81-language)). The number exists to answer one question, whether the mitigations (sanitizers, fuzzing, codegen, small modules, strict layering) are holding or whether the fallback has to be weighed, and nobody has kept it: there is no tally in the tree.

## What a defect is here

A defect is something the tree promised and did not do, found after the change that caused it was committed. Four kinds reach a record today:

| Kind | What counts | Where it is recorded today |
|---|---|---|
| A found defect | a row in the roadmap's F area, "defects that no plan section schedules" | [roadmap F](../roadmap.md#f--found-along-the-way): one row each, `done` with what closed it; 26 rows on 2026-11-19, 21 done and 5 open |
| A fix row | a red gate leg opens "a fix row at the top of the next batch" ([AGENTS.md](../../AGENTS.md), "What to run, and when") | an ordinary roadmap row in the area of the code it mends; nothing marks it as a fix, so it cannot be counted from the board |
| A red gate leg | a suite, a container or the GPU server failing at a merge | not in the repository: the gate's verdicts live in the coordinator's notes outside it; the tree holds only the fix row a leg opened |
| A re-pin | a golden image, a hash or a measured expectation taken again because the old one no longer held | the commit message, which must say why (AGENTS.md, the re-pin attribution); not on a page |

Sanitizer findings, the plan's own measure, are a subset of the first two: a sanitizer leg is a gate leg (`linux-clang-asan`, `msvc-asan`, roadmap T11), and what it finds becomes an F row or a fix row like any other failure. Counting them apart needs nothing new beyond a word in the row.

**What is not a defect.** A test made stricter, a case added for a promise that was never made, a measurement that moved a plan (that is an estimate, recorded in [10 §10.8](../plan/10-roadmap-risks.md#108-calibrating-the-estimates)), and a driver change the engine did not cause (the R184 kind, written down as a lesson). The line is whether the tree had already claimed the behaviour.

## Per phase, read as per month for now

The plan's phases run concurrently: the ledger's Phase 3 ran "alongside Phase 1", and Phase 1 has not exited ([10 §10.8](../plan/10-roadmap-risks.md#actuals)). A defect cannot be given a phase by its date, and a row's area (R, W, D, ...) is a part of the engine, not a phase. So the tally is kept **by calendar month of the row's closing date and by area**, and a phase's figure is the months it spanned once the ledger gives a phase both its dates. That is a reading of "per phase" the owner may refuse; the alternative, a phase column on every row, is a field nobody can fill honestly while five phases are open at once.

**Per thousand lines** is left out of the first tally: "agent-written" is every line in this tree, and lines counted from `git ls-files` move with generated and vendored files unless the count names its roots. The rate is the count over the month's added lines once the count is trusted, a second step.

## The smallest tally that `tools/docs-check.ps1` keeps

The board already holds the F rows, and docs-check already parses every row of it (ids, states, dependencies, done notes). The smallest change that makes a tally exist and keeps it honest without a hand count:

1. **A fix row says so.** A row opened by a red gate leg or by a re-pin carries `fix:` at the start of its title cell (`fix: the swash band case on the Titan Xp`). docs-check refuses nothing new; it only counts.
2. **A tally table at the top of the F section**, months by kind (F rows, `fix:` rows, of which a sanitizer leg found), written by whoever closes such a row, and **checked by docs-check against the board**: a written count that differs from the rows fails with the right numbers in the message, as the structure check does for a ceiling. Kept by the check, never by hand alone, which is what the row asks.
3. **Red gate legs and re-pins stay off the page** until they have a home in the tree: both are in the coordinator's notes and in commit messages, and a tally that reads git history from docs-check would make the documentation check depend on the history a clone carries.

**Proposed row (unnumbered):** docs-check counts the F area and the `fix:` rows by the month of their closing note and holds a tally table in the F section to that count; the fix rows already on the board are found by reading and marked once.

## What this does not answer

Whether the rate is high. There is no baseline: the tally starts the record, and a comparison waits for two phases with both dates in the ledger.
