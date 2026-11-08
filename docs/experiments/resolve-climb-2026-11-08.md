# The resolve's climb on the endless desert, and where it started (R138, 2026-11-08)

Roadmap R138. The batch 54 gate read `endless-surround3-rt`'s resolve median at 2.166 ms against a budget of 1.885 pinned on 2026-10-12 ([frame budgets](../subsystems/bench.md#frame-budgets)); the budget was re-pinned to main's 2.100 in 58288431 with hw kept. This page dates the step by building and flying the run at commits of main, and says what a compile-time test of the likeliest cause recovered.

**The run** is `endless-surround3-rt` as `tools/frame-budget.ps1 -Preset msvc-release -Run endless-surround3-rt -WaitQuiet 600` flies it (desert-endless, 11520×2160, three surround views, `--shadows rt`), on the RTX 5090, one release build per commit, each flight under the GPU lock the script takes itself.

## 1. What was measured

The machine was never quiet for the whole of a flight tonight (eight agents building, and the GPU lock queue was 30 to 50 minutes a run); each row states the harness's `machine_state` at its start and end. The GPU's own load was 0% at every start, and the claims below lean on pairs, not on one number.

| commit | what it is | resolve median (ms) | resolve p99 | hw | rt | sky | total | machine state (others CPU, start / end) |
|---|---|---|---|---|---|---|---|---|
| 4e442d76 | before batch 47 | 2.015 | 4.655 | 0.444 | 0.877 | 0.055 | 4.276 | busy (100% / 100%): an upper bound, to be re-measured |
| f28cd69f | R114 | 2.001 | 2.363 | 0.430 | 0.866 | 0.054 | 4.188 | quiet (9% / 8%) |
| 5e2d6ee6 | R115 stage 1, the resolve split by stage | 2.109 | 2.483 | 0.325 | 0.861 | 0.054 | 4.259 | busy (5% / 31%) |
| 3666b106 | R113 | 2.108 | not read | 0.332 | 0.861 | 0.054 | 4.263 | busy (8% / 10%, GPU 23% at the end) |
| 58288431 | main as it stood at this batch's base | 2.182 | 2.560 | 0.312 | 0.866 | 0.054 | 4.487 | busy (10% / 20%) |
| 6d67ab8a (not kept) | 58288431 with the stage stop and the ray counting compiled out of the picture's modules | 2.154 | 2.534 | 0.311 | 0.866 | 0.055 | 4.442 | busy (9% / 26%, GPU 24% at the end) |

Not measured, for the lock's queue: 693b1f63 (W82 stage 1), f13475a4, 43344b54 (R136), a3c96208 and 1b17ddfa (F3). Batch 54's share of the climb (about 0.08 ms by the gate's own results) is therefore **not bisected**; main's reading of 58288431 beside 3666b106 (2.182 against 2.108, 0.07 ms, over the same module's later commits) is the only evidence, and it is the size the gate reported.

## 2. The attribution

- **R115 stage 1 (5e2d6ee6) is the step of batch 47: +0.108 ms** (2.001 quiet at f28cd69f, 2.109 at 5e2d6ee6; R113 after it reads 2.108, so nothing between added or took anything). hw and rt do not move, so the cost is the resolve's own.
- The stop test (`stop_skips`/`g_stop`, uniform per draw) and the ray counters (`g_count_sun`, an increment guarded in the sun ray's loop) are **not** most of it. Folding them to constants in the picture's modules read 2.154 against 2.182 for the same tree with them in: 0.028 ms, inside the 0.05 ms that two readings of one commit differ by tonight. The experiment was therefore not kept: it would have cost a second family of sixteen `_measure` modules (a dimension beside stochastic, terrain, water and ray shadows) and the flights that use the stop, for a gain a rerun cannot separate from noise.
- What else 5e2d6ee6 changed in the picture's path: `light_tolerance`'s test (`weigh`, `rest`: a luminance dot over the pixel's lit colour, computed before the lights loop for every pixel even when `light_count` is zero), the lights loop moved from before the hemisphere term to after it (the sun's `color` and the indirect now live across the sky and occlusion reads, which raises what the compiler holds), and the 32 bytes added to `ResolveParams` (`resolve_stop`, `light_tolerance`). The desert scene has no analytic lights, so the loop never iterates; the likely cost is register pressure and scheduling around code that only moved. That is a hypothesis: it was not bisected inside the commit.

## 3. What stands

Nothing was taken back and the budget file is unchanged: a fix this tonight could not be separated from noise (§2), and re-pinning to a number measured on a busy machine would loosen the gate (the script refuses it; `-AllowBusy` was not used). The next step, for whoever takes it, is the cheap one: on a quiet machine, 5e2d6ee6 against its parent with (a) the `weigh`/`rest` pair guarded by `params->light_count != 0` and (b) the lights loop restored to its place before the hemisphere term, each alone, three flights each and the median of medians quoted. The picture must not change: both are the order of independent additions, and the terrain identity tests and a first-frame capture compared byte for byte hold it.

Batch 54's 0.08 ms stays an open question: R136's `terrain_hex_sample` (a hex lattice's three taps and blend per covered pixel) and F3's aerial lookup past 32 km are the candidates the commits suggest, and the two pairs to measure are `f13475a4` against `43344b54`, and `a3c96208` against `1b17ddfa`.

## 4. The candidates inside R115 stage 1, tried (R139's agent, batch 56, 2026-11-08)

Main at 86b543cb, the same run (`tools/frame-budget.ps1 -Preset msvc-release -Run endless-surround3-rt -WaitQuiet 600`, each binary frozen in the agent's scratch and named with `-Exe`), the three trees flown in turn under the GPU lock. The machine was never quiet (others 16–45 % of the CPU at the flights' ends, the GPU 0–31 %), so each reading is an upper bound; the pairs are what is compared.

| tree | resolve median, flight 1 / flight 2 (ms) | resolve p99, flight 1 | hw median, flight 1 | machine, flight 1 (others CPU start / end, GPU end) |
|---|---|---|---|---|
| main 86b543cb | 2.092 / 2.093 | 2.481 | 0.309 | busy (38 % / 25 %, GPU 31 %) |
| main + R139 (its rule off, the block 560 bytes) | 2.083 / 2.086 | 2.462 | 0.308 | busy (30 % / 16 %, GPU 7 %) |
| that + (a), `weigh`/`rest` guarded by `light_count != 0` | 2.098 / 2.099 | 2.479 | — | busy (25 % / 45 %, GPU 4 %) |

- **(a) does not measure**: 2.098 and 2.099 against 2.083–2.086 without it and main's 2.092–2.093, inside the 0.05 ms that two readings of one commit differ by. It was committed and reverted (a7e8b48f, af3c2ac6), as this row's rule asks.
- **(b), the lights loop back before the hemisphere term, was not tried alone**: since R115 stage 1 the loop's tolerance test reads `rest`, which is the pixel's other light *including the hemisphere and occlusion term*, so moving the loop back in front of that term changes what `light_tolerance` means rather than only where the code sits. Without the tolerance the order is the sum's and the picture's bytes would hold; with it, it is a different rule.
- **R139's 16-byte row costs nothing measurable**: 2.083 against main's 2.092.
- **Main reads under the budget** (2.092 against 2.100) where the batch 55 gate read 2.195 on a busier machine, so the gate's step is the machine's and not the resolve's, as far as one busy flight can say; the budget file is unchanged.
- Batch 54's pairs (f13475a4 against 43344b54, a3c96208 against 1b17ddfa) were not flown: the GPU lock's queue held each flight 30–60 minutes tonight.

What stands: R115 stage 1's +0.108 ms (§2) is not in `light_tolerance`'s weighing; the remaining suspects are the loop's move (a rule change, above) and the register pressure of the stop and count words that §2 could not separate from noise. Main's resolve is under its budget today.
