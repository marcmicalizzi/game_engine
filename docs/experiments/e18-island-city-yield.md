# E18: the yield of a hierarchical building grammar

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), [13 §13.6](../plan/13-reference-consumer-games.md#136-additional-experiments)):** what fraction of generated buildings, and of their units, pass the validators — reachability, egress, daylight, fit, services, structure, site — **without repair**, across the first grammar's archetypes; which rules fail, and what that says about the grammar, the validator set and the agent review loop of [13 §13.2](../plan/13-reference-consumer-games.md#132-consumer-b--island-city). And what a plan, a building and a district's proxies cost on the CPU.
- **Date:** 2026-09-25. **Machine:** a cloud container — Intel Xeon (x86-64, "@ 2.10GHz", model not reported), 4 logical CPUs, 15 GB, Ubuntu 24.04, Linux 6.18; **no GPU**. Not the development machine. **Build:** `linux-clang-debug` (Clang 18) for the yields, `linux-gcc-release` (GCC 13) for the costs; the yields are the same numbers from both, since every decision is an integer.
- **Machine state:** the yields are counts and do not depend on it. The costs: `--require-quiet`, the harness's `machine_state` 0% of the CPU in other processes at the start and the end of both runs, no GPU, nothing else running in the container.
- **Decision:** [ADR-0044](../adr/0044-island-city-plan-and-building-grammar.md) (proposed): the plan is a whole-island derived node, a building is a description generated per lot, validators categorize and never repair; the grammar is constructive, and the yield on the default rules is its regression floor.

## Setup

The `city` capability ([city](../subsystems/city.md)): the default island's parameters (a 2 km radius, the default district mix, styles and building rules) at seed 2026, the golden seed of the tests. E18's 200 buildings are `choose_yield_lots(plan, 200)`: building lots taken from each of the seven archetypes in turn in the order the seed ranks them — 29 apartment towers, 29 mid-rise over shops, 29 offices, 29 terraced houses, 28 detached houses, 28 warehouses, 28 civic shells — each generated to the `Rooms` stage and validated. A building passes when the validators find nothing; a unit passes when nothing is found on it, its rooms, or its building's building-wide rules.

```
engine-content city params island.json                       # then set "seed" to 2026 in it
engine-content city plan island.json --out plan
engine-content city yield --plan plan --buildings 200 --jobs 4
```

The stressed sets change one number of `building` (e.g. `{"seed": 2026, "building": {"max_span": 7}}`) and repeat the same three commands. The sweep plans islands of 900 m, 2 km and 3.5 km at seeds 1 to 7 and runs `yield --buildings 1000000`, which takes every building lot. The costs are `engine_city_bench` (`city.plan`, `city.building`, `city.validate`, `city.fragment.district`) at `linux-gcc-release`.

## Results

### The default island, 200 buildings

| archetype | buildings passing | units passing | reachability | egress | daylight | fit | services | structure | site |
|---|---|---|---|---|---|---|---|---|---|
| apartment tower | 29 / 29 | 1,799 / 1,799 | 29 | 29 | 29 | 29 | 29 | 29 | 29 |
| mid-rise over shops | 29 / 29 | 322 / 322 | 29 | 29 | 29 | 29 | 29 | 29 | 29 |
| office | 29 / 29 | 1,619 / 1,619 | 29 | 29 | 29 | 29 | 29 | 29 | 29 |
| terraced house | 29 / 29 | 29 / 29 | 29 | 29 | 29 | 29 | 29 | 29 | 29 |
| detached house | 28 / 28 | 28 / 28 | 28 | 28 | 28 | 28 | 28 | 28 | 28 |
| warehouse | 28 / 28 | 28 / 28 | 28 | 28 | 28 | 28 | 28 | 28 | 28 |
| civic shell | 28 / 28 | 1,013 / 1,013 | 28 | 28 | 28 | 28 | 28 | 28 | 28 |
| **all** | **200 / 200** | **4,838 / 4,838** | | | | | | | |

The validator columns count buildings passing that validator. **The yield is 1.0**, in buildings and in units.

### Every building of 21 islands

Islands of 900 m, 2 km and 3.5 km at seeds 1 to 7, every building lot of each generated to `Rooms` and validated (`linux-gcc-release`):

| radius | buildings (7 islands) | units | passing |
|---|---|---|---|
| 900 m | 5,620 | 142,891 | all |
| 2 km | 56,376 | 501,774 | all |
| 3.5 km | 193,761 | 1,643,042 | all |
| **all 21** | **255,757** | **2,287,707** | **all** |

Every plan's own validators (connectivity, dead-end arterials, block sizes, park share and distance, frontage) pass too.

### One limit changed at a time

The same island and the same 200 lots, with one building rule changed. "Reads it" says whether the grammar consults the rule when it decides.

| change | grammar reads it | buildings passing | units passing | what failed |
|---|---|---|---|---|
| none | — | 200 / 200 | 4,838 / 4,838 | — |
| `max_span` 9 → 7 m | no (a bar's depth comes from `unit_depth_max`, 8.8 m) | 90 / 200 | 158 / 4,838 | `structure.span` on 110: every tower and civic shell, 28 of 29 offices, 25 of 29 mid-rise; no house or warehouse |
| `bed_clearance` 0.6 → 1.5 m (an accessible bedroom's turning space) | no (a bar's bedroom is cut to the minimum width) | 136 / 200 | 3,697 / 4,838 | `fit.bed` on 1,344 bedrooms: every tower, 20 of 29 mid-rise, 8 terraced and 7 detached houses |
| `max_travel` 30 → 12 m | yes (more stairs) | 200 / 200 | 4,788 / 4,788 | — (the extra stair cores took 50 units' worth of floor) |
| `two_stair_floors` 4 → 3 | yes | 200 / 200 | 4,828 / 4,828 | — |
| `units_per_elevator` 60 → 20 | yes (more cars) | 200 / 200 | 3,641 / 3,641 | — but the towers hold 671 units where they held 1,799: wider cores no longer fit their bars at full height, and the core rule keeps the tallest floor count that fits |
| bedroom minimum 2.6 × 2.8 → 3.0 × 3.2 m | yes (unit widths and house depths come from the minimums) | 200 / 200 | 4,789 / 4,789 | — |

### CPU cost

`engine_city_bench`, `linux-gcc-release`, two quiet runs (medians of the first; the second within 7%); the default island at seed 2026, 7,990 building lots:

| what | median |
|---|---|
| the whole plan, one thread | 7.46 ms |
| the same, lots cut on 4 workers and the caller | 7.90 ms |
| one building at `Massing` / `Floors` / `Rooms` | 0.105 µs / 2.8 µs / 127 µs |
| the validators over one building | 9.2 µs |
| a 46-building district's proxies at `Floors` | 0.86 ms |
| E18's 200 buildings generated and validated on 4 workers (`city yield`) | 56 ms (debug build) |

A building at `Rooms` is 45 times one at `Floors`; validating it adds 7%. The release CLI makes a 794-building district's proxies (45,263 instances) in 10.2 ms on one thread, and a 370-building district's (70,551) in 15.2 ms.

## What surprised me

- **The first run was not 100%.** Fourteen mid-rise buildings downtown failed `daylight.window` on a shop floor: a 6 m shop front whose 1.4 m entrance sat at the centre left no pier wide enough for a window on either side. It is exactly the kind of finding the loop exists for — one rule of the grammar (where an entrance goes) broke a rule the grammar did not think about (a habitable room needs a window) on one proportion (a narrow shop) — and the fix is in the rule: a shop's entrance goes at one end of its front. Everything else passed from the start, because the grammar is **constructive**: each stage satisfies the rules it knows (units no narrower than their rooms' minimums, stairs where the travel distance and the height ask for them, windows on every exposed wall of a habitable room, doors along the walls rooms share), so a default-rules failure is a rule it does not know.
- **So the yield on the default rules is a regression floor, not a quality measure.** The plan's guess was a grammar that fails some fraction of its output for a validator loop to drive down. A constructive grammar starts at 1.0 and the interesting number is how it responds to rules it does not read, which is what the stressed sets show: a shorter span or a larger clearance fails whole archetypes at once, by one rule, and says which stage to teach. That is still the review loop 13 §13.2 describes — agents change the grammar's rules and the validators say what broke — with the failures coming from changed requirements instead of from noise.
- **A rule the grammar does read can still cost something the validators do not see.** Asking for an elevator per 20 units left every building passing and took 63% of the towers' units away: the elevators widen the main core past what a slab tower's bays hold, and the grammar's answer is fewer floors. No validator counts units lost; the yield table's unit column shows it only because the same lots were measured twice. A density validator (units per lot area against the district's) would make it a finding.
- **A parameters file that set one room minimum used to zero the rest**, because a non-empty table replaced the defaults: the grammar then cut meeting rooms of no width, and 3,520 rooms failed `daylight.window` and 2,518 `reach.room`. Tables keyed by kind now merge with the defaults kind by kind, and a meeting room is never narrower than a door's opening. The validators found a parameter-handling bug that no unit test of the parameters had.

## What it decides

- The grammar's rules are the validators' rules plus a layout; a new validator is a new question to the grammar, and the stressed sets are how to ask it. The next rules to teach the grammar, by this table: read `max_span` when choosing a bar's depth, read the bed clearance when cutting bedrooms.
- The validator set as it stands: reachability, egress, daylight, fit, services, structure and site cover what 13 §13.2 lists (reachability, furniture fit, light, egress) and three more; E18 adds one missing check to it, density.
- The yield test (`validate_tests.cpp`) holds the default island's 200 buildings at 1.0: any failure there is a grammar change that broke one of its own rules.

It does **not** decide whether the buildings look right, whether the proportions are the owner's, or whether the default island is the right size (13 §13.8): nothing here has drawn a picture, and the owner's fly-through over the proxy fragment is the first look. Nor does it measure the materialization tiers (E16) or anything on a GPU.

## Caveats

- One grammar, one set of defaults, and validators written alongside the grammar by the same author: a validator that shares the grammar's blind spot passes what it should not. The fixtures in `validate_tests.cpp` prove each validator can fail; they do not prove the set is complete. An agent or person writing validators against the grammar, rather than beside it, is the stronger test.
- The civic shell is validated as a bar of departments without room graphs, so its daylight, fit and reachability checks are coarser than a dwelling's.
- The costs are one cloud container's, not the development machine's, and a four-core one; they are for orders of magnitude.
