# content/muscle: the muscle line tier's qualification corpus

The 39 trajectories the authoring track qualified the line tier's law and scheme against (the five original cases, an offset pulse, an active wave, and sixteen parameter corners in a step and a passive variant), with the held-control Radau reference's samples as the oracle. `systems/muscle/tests/corpus_tests.cpp` replays every case on every build from its archived static start and holds it to the D20 gates ([muscle](../../docs/subsystems/muscle.md#the-corpus), [ADR-0055](../../docs/adr/0055-the-muscle-line-tier-scheme-schedule-and-failure-semantics.md)). The corpus is also the **only qualified domain** the validator accepts: its distinct parameter-set identities are `synthetic-study-only` version 1.

## The files

- `corpus.json` — the index: the case names in the source order; `reference_model_sha256`, the reference law's identity; `qualified_domain`, its id, version and the 17 distinct `parameter_sha256` values of the 39 rows; the source files with their SHA-256; and per case its parameter identity, the gate limits (`force_N` = max(1% of the reference's peak tendon force, 0.5% of F0), `fiber_m` = 0.5% of the optimal fiber length, `work_J` = 1e-3 of the reference throughput), `reference_throughput_J`, the sample count and `held_samples_sha256`.
- `cases/<case>.json` — one case: `inputs` (the control segments, the path, the fiber and series parameters, the initial activation, the identities) and `initial_state` (the archived static start: activation, fiber length and the reference's outputs there), both copied whole from CASES.json; then `columns` and `samples`, one row per held-reference sample: `time_s`, `activation`, `fiber_length_m`, `tendon_force_N`, `passive_fiber_energy_J`, `tendon_energy_J`, and the ledger's five channels from t = 0, `external_work_J`, `active_output_work_J`, `dissipation_J`, `active_throughput_J`, `external_throughput_J`. The samples are t = 0 and every 1/480 s parent endpoint — 289 for the 0.6 s cases, 193 for `shortening` and `lengthening` (0.4 s) — and `offset_pulse` has two more at its off-tick control changes, which the nominal comparison skips.
- `expectations.json` — each case's class: `qualified` (passes all three gates), `known_limit_work` (`corner_00/02/04/06_step`: work-only misses at r009 S, order-limited at either order) or `known_limit_force` (`corner_04/06_passive`: force-only misses at the first nominal endpoint, a start-up transient that r010's first-tick schedule targets), with r009 S's force error, fiber error, work-over-limit, classification and located crossings for every case, and the provenance of the labels. A labelled case passes the gates it is labelled as passing, and its failing metric may be at most 1.5 times its r009 number; a labelled case that passes everything is allowed and printed. The labels change only by the owner's review of r010's result.

## Where it comes from

`tools/muscle-corpus.ps1 -Authoring <the authoring track's 06-muscle-activation>` writes the three generated files (not this README) from these sources, read as data; nothing in the authoring worktree is run:

| Source | SHA-256 |
|---|---|
| `r010-20261013/declarations/d001/CASES.json` (inputs, archived starts, limits, throughput, hashes) | `acb1f8bf97b5a8534459afbb208d2fae4ce817b2e2409c2ec54ca61fecd81462` |
| `r007-20261011/_handoff/corpus.json` (the held-reference samples) | `16b43ddac1bb32805970a06e183ac1cfffebc1fe5f8cf44e1d36758f66e3b3f0` |
| `r009-20261013/results/s001/summary.json` (r009 S's per-row numbers, for the labels) | `c92c3e02e7583419e0af0c4dddadff4b1acba8229f6d90f0b9d99a22b8a59e5d` |
| `r009-20261013/results/s001/MANIFEST.json` (the S result, which lists that summary) | `86f0e6e5ffa58f6bb4177e5f6dd36847b1c331c9456c7c571a65d24eb0260b3a` |

The r009 declaration the labels belong to is `a204e8cf9c94ff609e44e32534f8b3ebee1bf8fd19f5513e828aff7beaf46232` (its `DECLARATION-MANIFEST.json`, as the summary records it).

**Every number is copied as text, never recomputed**: the script writes each source number's own token, so a value here is the reference's binary64 exactly, and the corpus test checks that every number survives a parse-print-parse through `core/json` bit for bit. The script also re-derives CASES.json's three per-row hashes — `inputs_sha256`, `initial_state_sha256` and `held_samples_sha256` — from the data it copies, under the authoring side's canonical form (sorted keys, compact separators, Python's number text), and stops if any of the 117 differs: the copy is checked against the hashes the sources carry, not only against the files' hashes.

To regenerate: `tools/muscle-corpus.ps1 -Authoring <path>` (PowerShell 7, no Python); a changed source changes the hashes above, which this README records by hand.
