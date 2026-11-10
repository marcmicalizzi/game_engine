# landform (domain)

**Purpose.** TODO(landform): one paragraph. What this capability is, which use cases it serves, and
what it deliberately does not do.

**Why this shape.** TODO(landform): why it is built this way and not another way — the alternatives
that were on the table, the measurement or constraint that chose the data layout, the solver, and
the LOD policy, and what would have to change for a different answer to be right. A page that says
only what the code does leaves the next reader to rediscover the reasoning at the price the first
one paid (AGENTS.md, "Recording decisions": "Write the why, not only the what"). If a
decision here is one a future contributor could be surprised by, it is an ADR, and this paragraph
links it.

**Owned data.** TODO(landform): what this module is the source of truth for. Nothing else may hold or
mutate it.

**Invariants.** TODO(landform): bullet list, each one checked by a test.

**Public API.** `include/domain/landform/landform.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `io`, `jobs`, `log`, `landform_schemas`.

**Testing.** `tools/dev.ps1 test -Filter landform`. Benchmarks: `tools/dev.ps1 bench -Filter 'landform.*'`.

**Performance notes.** TODO(landform): hot paths, layout decisions, tunables, and the budget this
capability is held to (docs/plan/11-performance-principles.md §11.1).

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | this capability stands alone |
| Component and event types | `domain/landform/schemas/landform.schema` | scaffolded, TODO |
| Tick scheduler entry | `landform::LandformSystem`, phase `landform::k_phase` | TODO: register when the scheduler lands |
| Render-graph passes | none | TODO: state whether this capability draws |
| Content-build derived step | none | TODO: state whether anything is precomputed from content |
| Protocol methods | none | not needed |
| Tunables | none | TODO: every run-time parameter, never a constant |
| LOD policy | `LandformSystem::lod_tier()` | scaffolded, TODO: real tiers and hysteresis |
| Determinism | `landform::k_determinism` = `hashed` | TODO: confirm, and say why |
| Zero cost when unused | TODO: which mechanism of plan 11 §11.10 applies | TODO |
| Tests and size table | `tests/landform_tests.cpp`, `tests/size_table.cpp` | scaffolded |
| Bench | `domain/landform/bench/landform_bench.cpp` | scaffolded, TODO |
| Removal proof | `ENGINE_WITH_LANDFORM`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_LANDFORM=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. Everything else must still build and pass.
