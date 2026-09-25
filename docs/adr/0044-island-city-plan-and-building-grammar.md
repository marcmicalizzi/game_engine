# ADR-0044: Island City is a whole-island plan made once, with buildings generated per lot as descriptions, proxies first

- **Status:** Proposed
- **Date:** 2026-09-25
- **Plan references:** docs/plan/13-reference-consumer-games.md §13.2, §13.4 items 1, 11 and 16, §13.6 (E18), §13.8; docs/plan/07-content-pipeline.md §7.4, §7.6; docs/plan/03-data-model.md §3.4, §3.7
- **Docs touched:** `docs/subsystems/city.md` (new), `docs/subsystems/apps.md` ("engine-content city"), `docs/experiments/e18-island-city-yield.md` (new), `docs/plan/13-reference-consumer-games.md` §13.2 and §13.6, `docs/plan/07-content-pipeline.md` §7.6, `docs/plan/10-roadmap-risks.md` §10.5, `docs/adr/README.md` (this row, and a note on ADR-0037's)

## Context

Island City ([13 §13.2](../plan/13-reference-consumer-games.md#132-consumer-b--island-city)) is finite: a dense city on an island with a mountain at one end, a harbour and a transition into wilderness, and **no facade-only buildings** — every building has a functional interior generated from semantic and structural constraints, footprint to furniture, reviewed by validators rather than by hand. The owner asked for the city's plan and its building grammar, with parks and several kinds of city rather than one downtown, deterministic and integer like the ruins ([ruins](../subsystems/ruins.md)) and the content build ([ADR-0035](0035-no-floating-point-contraction.md)), before any kit, texture or furniture exists, and with nothing here able to look at a picture: the owner's fly-through over a proxy scene is the visual review.

Three questions had to be answered, and each had an obvious wrong answer.

**Where do streets come from?** The endless desert's generators are functions of (seed, tile) ([13 §13.4](../plan/13-reference-consumer-games.md#134-requirements-the-consumers-add-or-sharpen) item 1), and so could the city's be: each tile draws its own streets, matched at its edges by a shared lattice. That fails the city's own rules. An arterial crosses dozens of tiles and must never dead-end; the street graph must be connected; a district is a region with its own pattern; "a park within walking distance of every dwelling" and "a park share per district" are questions about areas no tile can see. A per-tile generator can satisfy those only by making the city a lattice, which is the one thing the owner asked it not to be.

**What does a building come out as?** Meshes are what the renderer draws, and a building could be assembled into kit instances now, as a ruin is. But no kit exists; every consumer after the renderer — the furnisher, the navigation mesh, the NPC routines, interior streaming by floor and unit, destruction — needs to know what a room is, not what it looks like; and a mesh assembled before its semantics are settled would be thrown away with every change to the grammar.

**How is it reviewed?** Plan 13 makes validators the review loop and E18 their pass rate. The obvious shortcut is a generator that repairs its output until the validators pass, which makes the yield 100% and the validators meaningless.

## Decision

1. **The plan is a whole-island derived node.** `city::generate_plan` is a pure function of the island's parameters (`engine.city.IslandParams`): coastline (seeded, or an authored polygon), the mountain and greenbelt, an arterial grid, districts assigned from the island's shape and a district mix, each district's street pattern and lot rules from a style table, blocks, lots, parks by rule (central, district, pocket, squares, waterfront, greenbelt) and civic reservations. It is made once and cached under `ddc/city/<key>/plan.json`, keyed by the parameters' canonical JSON and `k_generator_version`, and read back by everything that needs it. Streets come in classes (arterial, collector, local, alley, pedestrian) with widths; the graph is connected and no arterial dead-ends, and the plan's validators check it.
2. **Every building is a pure function of (plan, lot), addressed per tile.** The grammar runs on one lot and reads nothing but the plan; a tile's content is the lots whose closed rectangle touches it; materializing a tile twice yields the same buildings; a tile's content is the whole plan's restricted to the tile. A building's identity is (generator version, plan key, lot), and its units and rooms are numbered from it.
3. **A building is a description.** Floors, cores, spaces with semantic kinds, units, walls as centre lines with thickness and openings, doors, windows, furniture zones, and an occupancy summary (dwellings, bedrooms, residents, workplaces by kind, opening hours) — the capability's own schema (`engine.city.BuildingFile`). The walls sit on the ruins' module grid (2 m, a parameter) so a kit can attach by the ruins' socket convention later; that attachment is not designed here.
4. **Proxies first.** Until kits exist a city is drawn as twelve unit-box meshes instanced per element and scaled to it, written as an `engine.scene.Scene` fragment by `engine-content city fragment`. **The scene reader does not link this generator**: ADR-0037 accepted one such link as an interim and named a second generator as its revisit trigger; the city is that second generator, and the registration point that replaces the link is being designed on the owner's side, so the city attaches through nothing but the fragment until it exists.
5. **Authored overrides are sparse pins by id.** An authored layer may pin a district's kind, a street's class, a block as a park (or keep the park rules off it) and a lot as a civic reservation (or keep the civic rules off its block); everything else is generated around the pins. Ids are composed from the arterial grid, which no pin changes, so a pin names the same thing whatever else is pinned; a pin naming an id the plan does not have refuses the plan with a sentence.
6. **Integers decide.** Every seeded choice is integer arithmetic on centimetres and Q16 fractions with the engine's hash; angles are a 64-step Q14 table; floats appear only in the proxy fragment's metres. Golden hashes pin a plan at a seed, a building per archetype and E18's yield table on every toolchain, and the output does not depend on the thread count.
7. **Validators categorize and never repair.** Reachability, egress, daylight, fit, services, structure and site for a building; connectivity, block sizes, parks and frontage for the plan. A failure names its rule. The grammar is constructive — each of its stages satisfies the rules it knows — so the validators are an independent check of the rules the grammar does not read, and E18's yield measures how far the two agree.

## Consequences

- A city is two artifacts with two lifetimes: one plan per parameter set (milliseconds, a few megabytes, cached), and buildings made on demand from it (microseconds to milliseconds each, never stored). A world ring can materialize a tile's buildings from the plan the way it materializes a tile's ruins, at the stage the LOD policy gives the tile's distance (`stage_for_distance`: rooms near, floors in the middle, massing far).
- Changing the plan's rules changes every building on the island (their lots move); changing the grammar changes only buildings. Both bump `k_generator_version` when they change output, and every derived plan is rebuilt rather than trusted.
- The occupancy summary is a contract with work that does not exist yet: the NPC routines will read homes and jobs from it. Its fields are the schema's, `@since`-versioned like any other.
- The validators' pass rate on the default rules is 100% by construction (E18); a yield below that on default parameters is a grammar regression, and the test holds that floor. What the yield measures in earnest is parameters: a limit the grammar does not read when it decides (E18's 7 m span, its 1.5 m bed clearance) fails the buildings it reaches, by rule.
- No world-ring consumer is added: the ruins consumer lives in `systems/world`, and a capability may not edit another. The per-tile query is what a consumer would call, and it belongs with the registration point of decision 4.
- The scene reader stays as it is; ADR-0037's revisit trigger has fired, and its replacement is the owner's to design.

## Alternatives rejected

- **A per-tile street generator with no global plan**: rejected for the reasons in Context — connectivity, arterials that never dead-end, district regions and park rules are properties of the whole island, and a lattice that satisfies them tile by tile is not a city.
- **Generating meshes now**: rejected because no kit exists to assemble them from, every consumer after the renderer needs semantics rather than geometry, and the grammar will change many times before its output is worth a mesh; the proxies give the owner a look now at no cost to that.
- **Repairing buildings until they pass**: rejected because it turns the validators into a loop condition and the yield into a constant; a failure is a finding about the grammar, and it has to reach the person who can fix the rule.
- **A third link from the scene reader**: rejected because ADR-0037 accepted one link as an interim, not a pattern, and the registration point is already being designed.

## Open questions (defaults chosen, recorded here to be decided)

13 §13.8 leaves the island's size, the city's footprint and the first archetypes open. The defaults are parameters, chosen to be defensible and not yet checked against a picture:

- **The island**: a 2 km mean radius (about 4 km across; at the default seed 9 km² of city, 9,165 buildings and 66,459 units), 30% of its extent mountain and wilderness, a 150 m greenbelt, an 800 m harbour bay. The arterial grid at 480 m and districts of about 800 m.
- **The district mix** (downtown 8%, industrial 12%, old town 7%, waterfront 9%, campus 6%, mixed use 24%, residential 34%) and each style's block, lot, setback, coverage, floor and height ranges (city.md's table): typical European dimensions, not measured against anything.
- **The park rules**: every dwelling within 400 m of a park, a 2.5% central park, district park shares of 3–12%.
- **The first archetypes**: apartment tower, mid-rise over shops, office, terraced house, detached house, warehouse, and one civic shell (a hospital's bar of departments, used for every civic kind). A tower is a slab — a bar at most twice as wide as deep — not a point tower; footprints are rectangles.
- **The building rules**: 30 m of travel to a stair, two stairs from four floors, an elevator from five floors and one per 60 units, 9 m spans, the room minimums and a double bed's clearance — building-code rules of thumb, not any one jurisdiction's.
- **The station** is a civic shell with a transport workplace kind, not a concourse.

## Revisit when

- The scene-generator registration point exists: the proxy fragment's command becomes one more thing it can call, and a world-ring consumer follows.
- A kit exists for Island City: the attachment of decision 3 is designed, and a facade's far tier with it.
- E16 (interior materialization tiers) measures what a floor or a unit costs to materialize: the LOD policy's thresholds are its to set.
- The owner's fly-through finds the proportions wrong: the defaults above change, and this ADR's open questions with them.
- An island is wanted whose streets are not an axis-aligned lattice at all (a radial old town, a coastal road that follows the shore, a district at an angle to its neighbours): the plan's street stage becomes a graph from something other than a lattice, and decision 1's ids must be recomposed.
