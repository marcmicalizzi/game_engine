# 09 — Testing and Profiling Architecture

## 9.1 Test taxonomy

| Kind | Location | Gate |
|---|---|---|
| Unit and property tests (module invariants, schema round trips, migrations) | per-module `tests/` | every change |
| Determinism and replay (sim hash after N ticks from seed + input log) | `systems/simulation/tests` | every change |
| Renderer golden images (FLIP against goldens per scene × resolution × GPU class) | `content/golden` | subset per change; full nightly including 11520×2160, 7680×4320, 1080×3840, 5120×1440 |
| Reference-vs-real-time error (FLIP against converged path trace) | nightly | per-scene threshold |
| Performance regression (frame-time medians over K runs, per pass, per-machine baseline) | nightly on GPU runners | fail on a significant regression above 3% |
| Memory budgets (per-tag high-water marks) | nightly | budget file per target config |
| Hot-type size table (`static_assert`) and per-frame allocation count | build; every headless run | every change |
| Banned-container and hot-path lint (clang-tidy project checks) | build | every change |
| World-state invariants (conservation, no orphan references, required regions reachable) | after every headless run | every change |
| Content validation (all rules in [07 §7.4](07-content-pipeline.md#74-validation-rules-automatic)) | content build | blocks the build |
| Fuzzing (destruction sequences, input, save/load points, protocol messages) | nightly | crash or invariant failure |
| Playtest bots (utility bots × profiles × seeds) | nightly | completion rate, stuck rate, budgets |
| Agent-content validation (canon, style, boundary contracts) | on proposal | blocks promotion |
| Protocol and MCP contract tests (schema conformance, error messages) | every change | |
| Migration corpus | every change | |

## 9.2 Determinism infrastructure

- A sim hash per tick covering persistent state and RNG streams.
- A divergence bisector: find the first tick where two runs differ and dump both states.
- A "record everything" mode for bug reports: input, events, and LLM outputs.
- A replay viewer in the editor with scrubbing and state inspection.

## 9.3 Profiling infrastructure (before optimizing anything)

- Tracy instrumentation in every system; GPU zones via timestamp queries; capture export as part of the `profile` tool result.
- Per-pass GPU timings and counters (occupancy, bandwidth via vendor APIs where available) in a structured frame report.
- Memory by tag; allocation counts per frame (should approach zero in steady state); page-residency statistics.
- Streaming statistics: requests, hits, latency, evictions.
- Simulation statistics: per-system time, event rates, LOD tier populations, scheduler queue depth.
- A frame-budget file per target configuration; violations are telemetry events agents can query.

## 9.4 Benchmark scene corpus

Representative: interior, urban street, forest vista, destruction sequence. Pathological: 10^6 emitters, a mirror room, a foliage wall, 10^5 instances, ultra-thin geometry, a 48:9 wide-FOV view, 10^5 NPCs at LOD2, 10^4 debris bodies. All versioned in `content/test-scenes/`, each with a converged reference image set.

## 9.5 Agent-facing test surface

`run_tests(filter)`, `benchmark(scene_set, resolution_set)`, `validate(scope)`, `replay(run_id, until)`, `bisect_divergence(run_a, run_b)`. All return structured results with artifact URIs, and every failure includes the minimal reproduction command.
