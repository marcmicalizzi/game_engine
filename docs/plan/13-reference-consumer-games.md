# 13 — Reference Consumer Games

Status: recorded 15 September 2026 from the project owner's descriptions (the second, refined version incorporates a discussion the owner had elsewhere). These are two potential reference games for the engine, deliberately complementary. Neither dictates premature engine architecture; their purpose is to give proposed capabilities concrete consumers and acceptance cases instead of developing systems in the abstract. Nothing here is implemented.

- **Consumer A, Desert Survival** stresses sparse, effectively infinite, long-distance, dynamically deforming terrain while keeping conventional asset density low.
- **Consumer B, Island City** stresses finite but extreme density, interiors, NPCs, visibility, streaming, simulation, and world consequences, and contains a continuous transition into natural terrain.

Where the desert asks how cheaply the engine can represent enormous sparse space, Island City asks how much complexity the engine can manage when almost everything nearby matters. Together they cover the risk row "engine generality without a game" ([10 §10.4](10-roadmap-risks.md#104-major-technical-risks)).

## 13.1 Consumer A — Desert Survival

### Concept

Single player. The game begins immediately after a plane crash in a procedurally generated, effectively infinite desert. There is no conventional narrative campaign; the objective is to survive as long as possible, and survival time is the primary score. It is simultaneously the engine's first substantial technical demonstration, a proof of concept for agent and procedural content generation, and a small but potentially viable niche survival game. Its deliberately small content and narrative requirements let effort concentrate on engine capabilities.

### World

Generated from a deterministic seed and continued indefinitely through world streaming: large dune fields; flatter hardpan where appropriate; occasional exposed rock and geological variation; extremely sparse ruined structures; very rare oases; sparse desert creatures; the plane wreck. Large-scale variation must keep the world from reading as repeated noise while preserving the character of a vast, mostly empty desert. The sparseness is intentional: it permits extremely long view distances without city-like asset density.

### Dynamic sand (the defining system)

- **Large-scale dune evolution.** Wind slowly transports sand and modifies dune geometry; dunes migrate over long game periods. No granular physics for whole dunes: a coarse deterministic transport model modifies the terrain representation over time. Wind direction and strength drive migration, erosion, deposition, filling of depressions, and accumulation against structures. Distant and unloaded regions evolve through coarse deterministic simulation, not detailed physics.
- **Structure and sand interaction.** Wind and structure geometry shape local deposition enough that sand accumulates against walls and partially buries buildings. A ruin found early may have an accessible doorway; returning much later may reveal sand piled against it, the doorway partially or wholly buried, another part of the structure exposed, or a new route onto the roof. Persistent environmental state gets a visible consequence without any NPC simulation.
- **Fine local deformation.** Near the player a higher-resolution deformation layer supports footprints, trails, dragged-object trails, creature tracks, displacement around objects, localized erosion, and wind gradually filling tracks. Tracks carry gameplay meaning (creature tracks indicate recent activity) and fade with wind. The fine representation is not resident at distance; coarse persistent state suffices until the tile becomes active again.
- **Sand slides.** Dune faces support localized, realistic slides. Movement or other disturbance destabilizes sand past an angle-of-repose threshold; the engine runs local relaxation and transport rather than granular simulation. Walking across a slip face causes a small avalanche beneath the player that modifies the real surface rather than being cosmetic. The same capability later carries snow and similar deformable terrain.

### Survival mechanics

Thirst is the dominant short-term pressure; hunger operates over a longer scale. Water comes primarily from rare oases, secondarily from water around ruins, supplies recovered from the wreck, and limited hydration from creatures (eating never removes the need for water). Food comes from creatures, rare supplies at ruins, initial crash supplies, and possibly sparse edible environmental resources. Temperature exposure (daytime heat, night cold) is a candidate third dimension, added only if it materially improves the game rather than adding a meter.

### Oases

Extremely rare, potentially visible from very long distances. They provide water, foliage, localized wildlife, shade, possibly food and resources. Technically they are **rendering islands**: most of the desert is cheap, and an oasis suddenly introduces dense foliage, alpha and opacity geometry, water with reflection and refraction, complex shadows, wet materials, much heavier ray-tracing traversal, and far higher local asset density. Approaching one exercises streaming from sparse into dense and back out. Far visibility is also gameplay: the player must judge whether the water they have will get them to something several kilometres away, so atmospheric rendering must make distant landmarks believable. Mirage effects are optional and must not make navigation arbitrarily frustrating.

### Ruins

Sparse enough that finding one matters. Possible functions: shelter, shade, supplies, water containers, food, tools, viewpoints, creature nests, interest. No elaborate history is needed for the first game. Their technical roles: far-distance geometry, streaming, geometry LOD, occlusion, partial visibility, dynamic terrain occlusion, sand accumulation, persistence. The key case is a ruin mostly buried behind a dune: if only the top of a tower is visible, the renderer should process only the clusters that can contribute to the image rather than treating visibility as an all-or-nothing object property, and that visible fraction changes as dunes migrate.

### Visibility and long-distance rendering

Very large view distances over a sparse world test hierarchical visibility, terrain occlusion, cluster-level culling, partial-object visibility, geometry and texture streaming, projected-error LOD, ray-tracing geometry LOD, and atmospheric rendering. Large dunes are useful coarse occluders: objects within nominal view distance may still be excluded because terrain makes them impossible to see.

### Day, night, and weather

Time progresses continuously through harsh daylight, sunset, night, and sunrise, exercising rapidly changing lighting with limited asset variety: harsh sun, subtle diffuse light, long shadows, atmospheric scattering, GI change at sunset, moonlight, stars, very dark scenes. Wind changes over time and drives the sand simulation. Further weather is a later consideration.

### Camera modes

First person, third person, and isometric, toggleable at any time; partly gameplay, partly engine qualification. First person stresses very close materials, fine sand detail, footprints directly below the player, close ruin geometry, water close-ups. Third person adds character rendering, animation, skinning, character shadows, sand interaction, camera collision, clothing. Isometric changes the workload: a much larger visible terrain area, many more tiny projected objects, aggressive LOD, streaming, visibility, different UI assumptions. Instant switching gives regression and benchmark cases.

### Seeds and scoring

Generation is deterministic from a seed. A run records survival time, distance travelled, oases found, ruins explored, creatures caught, resources consumed. Modes: random seed, daily challenge, weekly challenge, explicit seed. Shared seeds give identical geography and conditions, so scores compare meaningfully, and because a run is (seed, input log) it replays for verification ([05 §5.10](05-simulation.md#510-determinism-and-replay)).

### What Desert Survival qualifies

Scale and sparsity: effectively infinite streaming, deterministic procedural generation, extreme view distances, cluster geometry, far LOD, terrain occlusion, partial visibility, dynamic terrain, deformable surfaces, persistent environmental state, coarse offscreen simulation, ray tracing, atmosphere, water, foliage, day and night, three camera rigs, resource residency, procedural content generation, agent-driven environment development. It deliberately does not test dense urban assets, dense NPC populations, many interiors, large light and material counts, complex navigation, or large-scale human world simulation. That is Consumer B.

## 13.2 Consumer B — Island City

### Concept

A finite island holding a very dense city and a mountain and wilderness region, surrounded by an effectively infinite procedural ocean. The island is a natural world boundary without invisible walls. Its defining property is a continuous density gradient: dense downtown → urban neighbourhoods → suburbs → sparse development → foothills and forest → mountain → coast → ocean. It may initially exist as a technical environment; a gameplay concept can follow if one emerges.

### Geography and the three workloads

- **Downtown**: maximum near-field density of buildings, props, materials, lights, interiors, windows, vehicles, NPCs, foliage, reflections, shadows.
- **Mountain looking at the city**: potentially most of the city visible at once; occlusion helps little; the renderer must aggressively reduce geometry, texture, interior representation, ray-tracing geometry, light representation, and NPC representation. The test of whether runtime work follows perceptual contribution.
- **City looking at the mountain**: dense nearby geometry plus enormous distant terrain, vegetation, and atmosphere.
- **Continuous traversal** downtown → suburbs → wilderness → summit without loading screens or separate benchmark maps.

### Fully functional buildings (ambitious target)

No facade-only buildings. Every building can have a complete functional interior: houses, apartments, offices, shops, restaurants, industrial and service spaces, stairwells, elevators, corridors. Doors, stairs, and elevators work; rooms are furnished and explorable. The finite island makes completeness a property that can actually be reached.

### Procedural building generation

Thousands of interiors cannot be furnished by hand. Buildings are generated hierarchically from semantic and structural constraints:

```
footprint → structural grid → building type → floor count → vertical cores → entrances
→ stairs/elevators → service shafts → floor plans → room graphs → doors/windows
→ furniture zones → furniture/props → occupancy/use
```

A tower first establishes building-wide constraints (structure, elevator core, fire stairs, hallways, shafts, exterior windows), then subdivides the remaining area into units, then gives each unit a semantic room graph (entry → living/kitchen → bedroom, bathroom). Furnishing follows the meaning and geometry of the room rather than scattering objects where collision permits.

**Agent role.** Development agents do not furnish rooms; they design and refine building grammars, room grammars, furniture rules, architectural styles, occupancy archetypes, procedural parameters, and asset kits. The procedural system produces volume; agents sample results, inspect captures, run validators, find systemic problems, and improve the rules. ML generation supplies novel assets ([07 §7.6–7.7](07-content-pipeline.md#76-procedural-generation-volume-content)).

### Persistent interior identity, streaming, and materialization

A generated interior is stable: identity through generator version, seed, building/floor/unit, archetype, occupant state, and persistent modifications. Geometry need not stay loaded; the engine reconstructs the same apartment on demand and applies its persistent changes, so hundreds of thousands of rooms do not imply hundreds of thousands of resident rooms.

Standing outside a tower must not make every room resident at full fidelity:

```
building exterior   resident        floor 17          relevant
structural shell    resident/coarse     corridor        active
floors 1..16        unloaded            apartment 1701  unloaded
                                        apartment 1702  active
                                        apartment 1703  coarse
```

Opening a door triggers detailed materialization; visibility through a window may need a cheaper representation first. Interior state is another form of visibility-driven materialization ([05 §5.4–5.5](05-simulation.md#54-lod-tier-assignment)).

### Windows and partial interior visibility

From a street, hundreds or thousands of interiors are theoretically visible. The engine must not load and render them all at full fidelity, and must distinguish relevance to primary visibility, reflections, shadows, GI, and ray-tracing acceleration structures. Three pixels of an apartment through a distant window must not put every small object in that apartment into full-fidelity RT structures. This is the consumer for RT-specific geometry and detail policies.

### Elevators and vertical streaming

Functional elevators exercise moving enclosed spaces, doors, animation, NPC navigation, vertical partitioning, audio transitions, physics, lighting transitions, and streaming prediction: selecting floor 63 tells the streaming system what is needed shortly. Stairs are the unpredictable case and keep the architecture from using elevators as disguised loading screens.

### Dense NPC simulation

The primary consumer for large human populations: hundreds or more visible NPCs downtown; animation throughput, skinning, navigation, avoidance, perception, shadows, RT character geometry, material diversity; routines, jobs, homes, shops, transit, services, economy, factions. Most persistent NPCs are not simulated in detail. Persistent state is of the form *home: building 184, floor 17, apartment 1702; job: hospital 2, radiology; state: working; next event: shift end*. When neither the NPC nor the location is relevant to an observer, no detailed movement happens; when the region or interior activates, the NPC materializes consistently from persistent state. This is the real-world consumer for spatial and temporal simulation LOD ([05 §5.3–5.6](05-simulation.md#53-event-scheduler-temporal-lod)).

### Dense asset and renderer stress

Extremely high instance counts, many unique materials, texture residency, dense lighting, RT acceleration structures, glass, reflections, interiors, foliage mixed with architecture, shadow complexity, skinned characters, crowds, vehicles, audio, navigation, physics, simulation, shader divergence, streaming under movement. A crowded plaza or transit station is an explicit pathological benchmark ([09 §9.4](09-testing-profiling.md#94-benchmark-scene-corpus)).

### Destruction and consequences

The city is the hard version of destruction ([05 §5.8](05-simulation.md#58-destruction)): destroying part of a building affects structural support, residents, homes, jobs, elevators, stairs, utilities, roads, navigation, nearby structures, quests, sightlines, traffic, geometry, RT structures, and persistent state. The test of whether destruction is integrated into the world model rather than a visual effect. With story protection disabled, destruction may legitimately make objectives impossible.

### Mountain, wilderness, and ocean

The mountain keeps the island from being purely an urban benchmark: natural terrain, forest, long-distance landscape, wilderness streaming, foliage, potentially snow with the same deformation architecture as the desert's sand (footprints, tracks, accumulation, displacement, localized avalanches), natural occlusion, and the urban-to-natural transition as a workload in itself. The ocean is effectively infinite procedurally with very low persistent complexity: boundary, unlimited horizon, waves, reflection and refraction, shoreline interaction, atmosphere, potentially underwater rendering; boats could later exercise streaming and simulation away from land.

### What Island City qualifies

Density and complexity: dense asset rendering, massive instance counts, dense materials and lights, RT under high scene complexity, strong occlusion downtown and weak occlusion from the mountain, geometry and texture LOD, interior streaming, partial interior visibility, RT relevance LOD, procedural architecture and furnishing, vertical streaming, functional buildings, large persistent NPC populations, visible crowds, animation, navigation, routines, economy and faction consequences, destruction, wilderness and urban transitions, ocean rendering, finite but highly complete world generation.

## 13.3 What the plan already covers

| Consumer need | Where |
|---|---|
| Heightfield terrain with clipmap LOD; deformation map read by terrain displacement | [04 §4.3](04-renderer.md#43-geometry) |
| Footprints, decay toward baseline, coarse deterministic CPU grid, per-tile compressed persistence | [05 §5.13](05-simulation.md#513-deformable-surfaces-and-soft-bodies) |
| Tile grid; tile-relative positions; camera-relative rendering at any world size; interior cells behind portals | [03 §3.7](03-data-model.md#37-spatial-partition) |
| Observer set; LOD tiers; materialization contract; reconciliation on tile activation; NPC scale | [05 §5.4–5.6](05-simulation.md#54-lod-tier-assignment) |
| Integer game time; timing wheel and `SummarizeInterval` for slow processes | [02 §2.4](02-architecture.md#24-frame-and-time-model), [05 §5.3](05-simulation.md#53-event-scheduler-temporal-lod) |
| Camera rigs over one observer; multi-view `ViewSet` | [05 §5.12](05-simulation.md#512-multiplayer-readiness), [04 §4.6](04-renderer.md#46-extreme-displays) |
| Event-sourced per-tile persistent state; saves as snapshot plus tail | [03 §3.5](03-data-model.md#35-persistent-world-state) |
| Generators as `derived` nodes with document parameters; building shape grammars and kits | [07 §7.6](07-content-pipeline.md#76-procedural-generation-volume-content) |
| Cluster as the unit of raster, streaming, and acceleration structures; page streaming by GPU feedback | [04 §4.3, 4.9](04-renderer.md#43-geometry) |
| Destruction with support graphs and downstream consumers; story protection | [05 §5.8–5.9](05-simulation.md#58-destruction) |
| Run telemetry and the run database | [06 §6.11](06-agent-tooling.md#611-automated-playtesting) |
| Every limit a tunable | [11 §11.6](11-performance-principles.md#116-no-hidden-limits) |

## 13.4 Requirements the consumers add or sharpen

1. **Unbounded procedural worlds.** Terrain baseline is a function of (seed, tile); the document holds generator parameters and sparse authored overrides (the crash site as an authored layer over an infinite procedural base), never the world. Tiles generate on demand as `derived` nodes with a per-tile budget, deterministically regardless of visit order and thread count; persistent state distinguishes *never generated* from *generated then unloaded*; tile addressing is 64-bit throughout. The ocean around Island City is the same mechanism with near-zero persistent complexity.
2. **A moving terrain baseline.** Wind transport, erosion, deposition, and accumulation against structures are a slow, coarse geomorphology process on the deterministic CPU grid, driven through the timing wheel with a `SummarizeInterval` for tiles outside the active set. The heightfield is mutable persistent per-tile state, not only derived data. Sand volume conservation is a test invariant and telemetry alert. Wind is a world field other systems read.
3. **Signed deformers and granular relaxation.** Deformers add material and move displaced volume, not only write a minimum height. Angle-of-repose relaxation runs locally on the coarse grid, bounded per event by radius and per tick by budget, and the fine GPU map re-syncs from the coarse grid: the coupling is bidirectional. Snow on the Island City mountain is the second consumer.
4. **Partial-object visibility as a first-class renderer property.** A tower mostly buried behind a dune, or an apartment seen through three pixels of a window, contributes only the clusters that can affect the image. Visibility is per cluster, not per object, and the visible fraction changes with terrain and viewpoint. Terrain is a coarse occluder in the hierarchical visibility pass.
5. **RT relevance LOD.** Relevance to primary visibility, reflections, shadows, GI, and acceleration structures are distinct decisions with distinct budgets. Small objects behind a distant window never reach full-fidelity acceleration structures. This sharpens the BLAS budget manager in Phase 2.
6. **Rendering islands and density gradients.** Streaming and residency must handle a sudden transition from a cheap sparse world into a dense island (an oasis) and back, and a continuous gradient from downtown to summit, without loading screens. Both become benchmark traversals.
7. **Long view distances over low-frequency terrain.** Terrain LOD to tens of kilometres from the world-gen mips, with the horizon generated coarsely at the top quadtree levels; a physically based sky and atmosphere; view distance a tunable with a cost model.
8. **Time of day as a world input.** Sun and moon, sky radiance, and temperature derive from `GameTime` with a tunable day length; rendering stays stable under a continuously moving sun (temporal reuse in ReSTIR and the GI cache).
9. **Camera rigs including an orthographic one.** Isometric needs LOD and culling by projected size, not eye distance; the renderer assumes a perspective camera nowhere. Instant switching is a regression case.
10. **Interior cells as a materialization hierarchy.** Building → floor → unit → room, each with states *unloaded, coarse, active*, driven by observers, doors, windows, and predicted movement (an elevator destination is a streaming hint; stairs are the unpredictable case). Interior identity is (generator version, seed, building/floor/unit, archetype) plus persistent modifications, reconstructed on demand.
11. **Procedural architecture as a hierarchical generator.** Building-wide constraints before unit subdivision before room graphs before semantic furnishing, expressed as grammars and rules agents author and refine, with validators and capture sampling as the review loop.
12. **Persistent-state NPC materialization at scale.** Persistent NPC records (home, job, state, next event) as the LOD3 representation for a whole city; materialization on activation must be consistent with the record. Crowds of hundreds downtown are the LOD0 benchmark.
13. **Destruction as a world-model operation.** Structural support, residents, jobs, vertical circulation, utilities, roads, navigation, quests, and persistence all react; a benchmark scenario destroys part of an occupied tower.
14. **Scored deterministic runs.** Run = (seed, input log, schema version); the run database stores runs; leaderboards are projections; scores are verifiable by replay. Daily and weekly shared seeds.
15. **Still water and ocean.** Oasis pools with reflection, refraction, and wet shorelines; an effectively infinite ocean with waves and shoreline interaction, potentially underwater rendering. No flow simulation.

## 13.5 Mapping to the roadmap

- **Phase 1**: heightfield terrain with clipmap LOD; sky and atmosphere; a "dune field, 20 km" vista scene with a distant oasis and a moving sun; a downtown block scene as the first density benchmark; per-cluster visibility with terrain as an occluder.
- **Phase 2**: RT relevance LOD and the BLAS budget manager tested on the window case; GI stability under a moving sun.
- **Phase 3**: seeded on-demand tile generation with budgets; `GameClock` day and night; observer set with the three camera rigs; per-tile persistent state; interior cells with the unloaded/coarse/active hierarchy; persistent NPC records at LOD3; critter populations.
- **Phase 5**: the desert generator (dune-field synthesis from a wind field, ruin kits, oasis and scatter rules) and the first building grammar (footprint to furnished rooms) as `derived` nodes with document parameters.
- **Phase 6**: deformable surfaces v1 with signed deformers, granular relaxation, wind transport on the timing wheel, and `SummarizeInterval` for dune migration; destruction of an occupied tower with all downstream consumers. Added exit criteria: walk across a dune crest and it slumps; leave for a game-week and the dune has moved; destroy part of a tower and its residents' routines change.
- **Phase 7**: Desert Survival is the recommended **first shippable**, built as the Phase 6 exit demo made playable: seeded runs, score, leaderboard-ready replays. Island City is the standing density benchmark environment and the candidate setting for the narrative game.

## 13.6 Additional experiments

| ID | Question | Method | Decides |
|---|---|---|---|
| E13 | Cost of angle-of-repose relaxation on a 25 cm coarse grid for one 128 m tile on one core | Cellular-automaton prototype; cells per millisecond; worst case after a crest stamp | Coarse grid resolution; per-tick relaxation budget; whether relaxation stays CPU-side to remain deterministic |
| E14 | Does an analytic dune-migration `SummarizeInterval` match the step-by-step process well enough that a returning player sees no discontinuity | Simulate N game-days both ways from one seed; compare heightfields | Fast-forward model for slow terrain processes |
| E15 | ReSTIR and GI-cache stability under a continuously moving sun at the chosen day length | The dune vista scene; temporal error and flicker against the reference path tracer | Temporal reuse policy; day length bounds |
| E16 | Memory and time per interior at each materialization tier (unloaded record, coarse, active) for a 60-floor tower | Generate one tower; measure residency while walking floors and riding the elevator | Interior cell model; streaming hint value of elevator intent |
| E17 | RT cost of a street with 2,000 visible windows under relevance LOD versus full fidelity | Downtown block scene; acceleration-structure memory and trace time per policy | RT geometry LOD policy and BLAS budgets |
| E18 | Yield of a hierarchical building grammar: fraction of generated units passing validators (reachability, furniture fit, light, egress) without repair | 200 generated buildings across archetypes | Grammar design; validator set; agent review loop |

## 13.7 Open design questions for the owner

Desert Survival: day length; death rules (one run, permadeath); ruin interiors (interior cells) or shells; keyboard and mouse versus controller; crash site always tile (0, 0) with an authored layer and everything else procedural (recommended); whether shared-seed leaderboards ship in the first release; whether temperature exposure is in.

Island City: whether it stays a technical environment through Phase 7 or acquires a game; the island's size and the city's footprint (which set the ceiling on "every building is real"); which archetypes the first grammar covers; whether vehicles and transit are in the first density benchmark.
