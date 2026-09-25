# 06 — Agent and Developer-Tool Architecture

## 6.1 Principles

- The engine is a server. All clients are equal. Editor gestures compile to the same commands agents send.
- Everything is schema-typed and versioned; nothing is inferred from screenshots that the engine already knows.
- Every mutation is a transaction with attribution and an inverse.
- Agents are stateless workers; state lives in the document, the persistent store, and the task ledger.
- Tools return *structured results plus resource URIs*; bulk data never flows through MCP tool results.

## 6.2 Engine Protocol

- JSON-RPC 2.0 over WebSocket (editor, remote agents) and stdio (CI); a separate **bulk channel** (shared memory locally, raw TCP remotely) for captures, mesh uploads, and profiles.
- Methods and types are generated from `schemas/protocol/`. Versioned; the server advertises capabilities; clients negotiate.
- Sessions: `open_session(document, mode)`; several sessions per host; each has its own transaction log and runtime world.
- Subscriptions: sim events, validation results, build progress via server push.

## 6.3 MCP bridge

- Curated tools with rich descriptions and JSON Schema inputs, each mapping to one or more protocol calls. The bridge is where good error messages, pagination, and detail-level parameters live.
- Rule of thumb: **MCP is the cognitive interface** (semantically meaningful, coarse-grained, documented); **the native protocol is the mechanical interface** (fine-grained, high-volume, used by the editor, tests, and the bridge). If an agent needs a fine-grained operation repeatedly, add a compound command server-side rather than exposing the fine-grained one over MCP.
- Bulk results (captures, meshes, profiles) are written to a workspace directory and returned as file URIs the agent reads with its own tools.

**Status (2026-09-24): the bridge exists, as `engine-mcp`** ([apps](../subsystems/apps.md#engine-mcp-the-mcp-bridge)). It is an MCP server over stdio that starts one engine-host of its own and offers 33 curated tools over the protocol: sessions, layers, reading and editing objects (every edit an undoable transaction whose attribution must carry a rationale), undo, redo, the journal, diff, merge, validation, schema discovery, capture, benchmark, compare, evaluate, the log and the GPU report — and, since the day-one operations landed (§6.9), the content build, events, budgets, headless runs and the engine's own checks. It is a client like engine-cli rather than a second front end on the dispatcher because `render.*` lives in engine-host over the renderer, so only the host has every method; and one host per bridge keeps every session and scene id the agent sees owned by one process. The restartable-bridge-over-a-long-lived-engine shape of 02 waits for a host that outlives its clients. **The input schemas are generated**: at startup the bridge reads `engine.methods` and walks each tool's params type with `schema.describe` — which now reports every field's default for this — so field names, types, documentation, enums and defaults all come from the IDL through the host; the bridge adds only which fields it hides or requires, its own fields (`load`, `cursor`, `limit`, `detail`, `name`), and prose. That covers every params type the tools use, `doc.Command` and the whole of `RenderSettings` included. Pagination, detail levels and the error hints are the bridge's, as this section asks; bulk results are files under `--workspace` returned as `file://` URIs with a short summary, never inlined. What is not there yet is in the apps page: resources, progress notifications, a watchdog for a host that hangs, and leases and roles, which wait for §6.5.

## 6.4 Transactions, checkpoints, rollback

`begin_transaction → commands → validate → commit | rollback`; `checkpoint(label)` is a git commit; `rollback_to(checkpoint)` restores. Every commit records actor, role, task ID, and rationale. `diff(a, b)` returns structural diffs with deterministic semantic summaries ("12 props added in tile (3,7); quest q17 stage 2 precondition changed").

## 6.5 Permissions, concurrency, and leases

- **Roles** are configuration: allowed tool set, allowed layers, allowed tiles and object types, and whether commits need review. Director, designer, environment, QA, and performance are role configs, not code.
- **Leases**: an agent acquires a lease on (layer, tile set) or (layer, object-type set) before editing. Leases are time-bounded and visible; conflicts fail fast at acquisition rather than at merge.
- **Proposal layers**: agents write to their own proposal layer; acceptance promotes the layer's opinions into the target feature layer via structural merge; validators run on the merged result before promotion.
- Read access is unrestricted. Read-only context from neighbors is the point.

## 6.6 Introspection

- `describe(object | tile | region, detail: summary | standard | full)` returns schema-typed data plus a natural-language summary generated deterministically from templates (not a model), so it is stable and cheap.
- `query(sql | structured)` over the persistent store (SQL) and over document indices (a structured filter language).
- `capture(view, camera, channels)` returns color plus any of: depth, normals, material ID, **entity ID buffer**, motion vectors, lighting components. The entity ID buffer accompanies every color capture so the agent can map pixels to objects. A contact-sheet mode renders N cameras into one image. **Every capture is made under the session's view policy, and records it** ([ADR-0033](../adr/0033-content-classes-and-view-policies.md)): the policy **caps each content category separately** — `nudity`, `sexual` and `violence` at least, plus any a game adds — and names the substitution applied above each cap, and the substitution happens when the scene is instantiated for that view rather than as a post-process over finished pixels. The policy belongs to the session and is set by the host operator's configuration; a client cannot raise any of its caps, and a request above one returns a structured error naming the policy and the category rather than the content. The policy is a field of the capture's provenance beside the camera and the channels, because a restricted capture that does not say it is restricted will later be read as evidence that something is absent from the world when it is only absent from the view. Diagnostic channels obey the same policy as the color channel: a wireframe or weight view of classified geometry is still that geometry.
- `profile(scene, duration)` returns a structured frame-time breakdown per pass and per system, GPU counters, memory by tag, and a Tracy capture URI.
- `validate(scope)` runs module invariants and content validators: unreachable regions, nav islands, collision leaks, floating or interpenetrating objects, missing references, budget violations, style-guide violations, stale canon dependencies.
- `events(since, filter)` returns gameplay and world events; `telemetry(run_id)` returns playtest telemetry.

## 6.7 Edit context and world mips

The design problem: an agent editing one part of a world needs enough global context to place its decisions in the whole, and exact knowledge of what borders its edit region so the result stays continuous at full detail, all inside a bounded token budget. Two mechanisms, both from the brief:

**The mipmapped world.** The tile quadtree `L0..L4` ([03 §3.7](03-data-model.md#37-spatial-partition)) carries, at each level, summaries of its children in three families:

- **Semantic mips**: terrain classes, biome, settlement types, roads and connections, watershed, faction control, narrative significance, landmarks, sightlines. Produced by deterministic aggregation plus optional LLM summarization *cached as a derived artifact* whose inputs are hashed, so it invalidates correctly.
- **Visual mips**: automated capture cameras per tile (orthographic top-down, four obliques, notable vistas) rendered on content change into the derived-data cache and composited into neighborhood, district, and region maps.
- **Design-density mips**: narrative, combat, exploration, secret, traversal difficulty, visual complexity, emotional intensity as per-tile scalar layers with aggregation. Director agents read them as heatmaps.

**The edit cell with immutable overlap.** An edit lease covers a cell (one or more L0 tiles). The engine returns the cell at full detail and writable, plus an **overlap ring** (configurable width: one tile, or N meters) at full detail and **read-only for the duration of the lease**: terrain heights, placed objects, roads, splines, materials, nav, lighting. The ring is what the agent matches against. Because it cannot change during the edit, continuity validators (terrain slope across the seam, road tangents, river cross-sections, material families, nav connectivity) are checked deterministically at commit. Above the ring, the agent receives progressively coarser mips: `standard` detail for the containing L1, `summary` for L2 through L4, plus the boundary contracts ([§6.12](#612-hierarchical-world-generation)).

**Token budgeting.** Every context package is assembled to a declared token budget: full detail for the cell and the ring first, then the mip levels compressed from the bottom up until the budget fits. The package lists what was dropped so the agent can request more on demand. The package is a `derived` node, so it is cached and invalidates exactly when any input changes.

## 6.8 Long-running work

- A **task ledger** in the repo (`work/tasks/*.json`): goal, acceptance criteria, owner role, lease, status, links to checkpoints and review items. Agents resume from the ledger and the document, never from conversation memory.
- Review items are ledger entries created by the invalidation substrate or by review agents.
- Every accepted proposal records its rationale next to the change so later agents can find *why*.

## 6.9 Day-one operations

The minimum set for the first agent loop, all via the protocol and exposed through MCP:

```
open_session, checkpoint, rollback_to, diff
begin_transaction, commit, rollback
create_object, delete_object, set_property, reparent, place_asset, instantiate_template
describe, query, list_schema
capture, run_headless(seconds | until predicate), validate, profile, run_tests, build_content
events, get_budgets, get_logs
benchmark(scene_set, resolution_set) -> {frame_times, counters, image_error_vs_reference}
```

**Status (2026-09-17): `capture`, `benchmark`, and `profile` — two of three.** `capture` and `benchmark` are `render.capture` and `render.benchmark` on engine-host, over `systems/renderer` ([renderer](../subsystems/renderer.md), [apps](../subsystems/apps.md)); `render.load` is the scene-set half of `benchmark(scene_set, resolution_set)` (one scene and one resolution per call for now, and a caller loops), and `render.compare` is the `image_error_vs_reference` half, which the list above did not name separately because it assumed a reference renderer that does not exist yet ([04 §4.8](04-renderer.md#48-reference-renderer-and-objective-optimization)). `capture` returns more than a picture: the entity-ID buffer beside the color, depth, and normal channels, which is what turns "look at the frame" into "which instance is under this pixel". The `counters` half of `benchmark` is the GPU milliseconds per pass and the visible-pair counts; allocation and draw counters arrive with the frame budgets of ADR-0018. **`profile` is not there**: Tracy zones compile into the release presets but nothing exposes a trace over the protocol, and it stays a Phase 2 item beside the ray budget scheduler it exists to measure. The rest of the list — `run_headless`, `run_tests`, `build_content`, `events`, `get_budgets` — is untouched.

**Status (2026-09-24): "exposed through MCP" is true for what the protocol has.** engine-mcp ([§6.3](#63-mcp-bridge), [apps](../subsystems/apps.md#engine-mcp-the-mcp-bridge)) reaches, of the list above: `open_session`; `diff` (paged in the bridge); `create_object`, `delete_object`, `set_property` and `reparent`, each one `doc.apply` transaction with attribution, plus `apply` for a batch of any of the document's commands; `describe` of a schema type or a protocol method (not yet of an object, tile or region at a detail level, which is §6.6's `describe`) and `list_schema`; `capture` (`render.load` then `render.capture`, the channels written into the workspace); `validate` (the document's schema validation — `doc.validate` — not yet §6.6's content validators); `get_logs` (`log.tail`); and `benchmark` (`render.benchmark`, with `compare` and `evaluate` beside it as the `image_error_vs_reference` half). Beyond the list it has `close_session`, `list_sessions`, `layers`, `add_layer`, `set_edit_layer`, `objects`, `get`, `undo`, `redo`, `journal`, `merge_layers`, `adapters` and `host_info`. `checkpoint`, `rollback_to`, the explicit `begin_transaction`/`commit`/`rollback` (every tool call is one transaction; undo is the rollback there is), `place_asset`, `instantiate_template`, `query`, `profile`, `run_headless`, `run_tests`, `build_content`, `events` and `get_budgets` are reachable over MCP exactly when they exist in the protocol, and none does yet.

**Status (2026-09-24, later): `build_content`, `events`, `get_budgets`, `run_headless` and `run_tests` exist**, as engine-host's `content.build`, `session.events`, `engine.budgets`, `session.run_headless` and `engine.run_tests`, schema-typed like everything else and reachable over MCP as `build_content`, `events`, `budgets`, `run_headless` and `run_tests` ([protocol](../subsystems/protocol.md#the-day-one-operations), [apps](../subsystems/apps.md#the-tools)). `content.build` is engine-content's build in the host's process — the build moved into a module, `domain/content_build`, rather than being copied — with the cache, the job pool, the identity skip and the texture step. `events` is the journal and, when the store is built and the document has a `world.db`, the world's event log, paged by a cursor that is also what to poll with. `get_budgets` names the renderer's, audio's, memory's and each GPU's budgets with their limit, use, unit and source. `run_tests` is the document, tissue and content validators, and spawns nothing: running the test executables stays a CI job. **`run_headless` runs, but over a world the document does not populate — the materialization gap.** The session's world is a flecs world with the animation and audio emitter systems installed (the engine's own scheduler without the ECS capability), stepped at 60 Hz for game seconds or until a predicate holds; but no document record becomes an entity, because four things [03 §3.4](03-data-model.md#34-the-runtime-world) needs are not built — a mapping from a document type to the components it materializes into, a driver that walks a document's (or a tile's) records through `sim::MaterializationHooks` (the only `TileStore`s are tests' fakes), one executor (the hooks and tiers are `sim::SimScheduler`'s while the systems tick in flecs' pipeline, which is [ADR-0028](../adr/0028-ecs-and-persistent-store.md) decision 7, still open), and a write-back from the world to the document or the store. So the predicate's document terms read a document the run cannot change, and the terms that change today are the run's own clock. **Closed the same day, in all four parts** ([protocol](../subsystems/protocol.md#sessionrun_headless-the-materialized-world)): a `materialize` declaration in the IDL is the mapping, `sim::Materializer` is the driver, the engine's scheduler owns the tick ([ADR-0038](../adr/0038-the-scheduler-owns-the-tick.md), proposed), and `@writeback` fields come back as commits attributed to `system` and events in the store's log. `run_headless` materializes the document before its first tick, its predicate reads live components as well as document properties, and `session.materialize` reports what mapped and what was skipped and why. A `TileStore` over `foundation/store` is still not built. What is left of the list is `checkpoint`, `rollback_to`, the explicit transactions, `place_asset`, `instantiate_template`, `query`, `profile`, and `describe` of an object, tile or region at a detail level.

## 6.10 Multi-agent roles, review, and the human director

- Start with **one agent, one loop**: checkpoint → edit → build → validate → capture → review → accept or rollback. Add roles once this works.
- Review is a role config with read access plus the ability to file review items and block promotion; the director role adjudicates by promoting or rejecting proposal layers.
- **Human director surface** (in the editor client): a review queue of proposals with side-by-side captures (before/after, same cameras), structural diffs with semantic summaries, validator results, budget deltas, and one-click accept, reject, or annotate. This is the human's primary interface; design it early.
- **Review item kinds** are data, and one of them exists because agents deliberately cannot see everything. A **`classified content review`** item is raised where a check cannot appropriately be made by an agent — the content is above the session's view policy ([ADR-0033](../adr/0033-content-classes-and-view-policies.md)) — and carries the numeric validator report ([09 §9.7](09-testing-profiling.md#97-character-validators)), the identity and version of exactly what is to be reviewed, and no picture. A human opens it at full policy. It is the designed counterpart of the rule that classified content is verified by numbers and never by agent inspection: the validators do most of the work, and what is left is a person's. The measure of the validator set is how rarely one of these has to be raised.

## 6.11 Automated playtesting

- Bots drive the game through the **same input path** as players (recorded input events), so replays and telemetry are identical between bots and humans.
- Two control modes: scripted/utility bots (fast, headless, thousands of runs) and LLM-driven bots (slow, use captures plus structured state, dozens of runs) for subjective evaluation.
- Player profiles (new, explorer, completionist, speedrunner, cautious, aggressive, adversarial) are parameterizations of the utility bot plus prompt variants for the LLM bot.
- Telemetry per run: path, deaths, objective timeline, view-direction samples, encounter outcomes, nav failures, stuck detection, time per region, quest and world-state snapshots, performance. Stored in a run database, queryable, aggregated into design-density mips.
- Headless `sim` mode runs at maximum speed with no rendering; `offscreen` mode adds periodic captures for the LLM bot.
- Fuzzing: random destruction sequences, random input, random save/load points, with invariants checked after each.

## 6.12 Hierarchical world generation

- Stages as in the brief (thesis → world vision → narrative spine → coarse topology → main-path regions → expansion → secondary content), each producing **authored nodes** in the canon graph and document, each with acceptance criteria and validators.
- **Boundary contracts**: when an L(n) node is elaborated into L(n-1) children, the parent first writes a contract per shared edge: terrain profile along the edge, road/river/path crossings (position, width, type), biome and style tags, sightline requirements, faction control. Children must satisfy the contract; a validator checks conformance. Contracts are the coordination mechanism between adjacent agents and turn adjacency into a *data* problem rather than a negotiation.
- **Upward proposals**: a child may file a `CanonProposal` targeting a higher-level node. It enters the review queue; acceptance changes the parent, and the invalidation substrate marks dependents stale with review items.
- **Coherence mechanisms**: style guides as structured constraints (palettes, material families, architectural grammars, naming rules) with validators; visual consistency checks via image-embedding distance to the approved reference board; the canonicity rule (only accepted canon may be depended on); review roles; human gates at the bible and spine levels.
- **Derive optional content from existing state**: secondary-content agents query the world for "unexplained" objects (structures without history, factions without conflicts, regions without landmarks) and elaborate those rather than inventing unattached filler.

## 6.13 Human developer tooling

The editor is a complete development environment, not an agent review console. A team that never uses an agent must be able to build a game with it, and a team that does must be able to open anything an agent produced and adjust it by hand.

Scope: a viewport with gizmos and snapping; scene outliner and property panels generated from schemas; an asset browser over the derived-data cache with validation results inline; material graph, terrain, spline, scatter, and placement tools; animation and state-machine editors; quest and dialogue graph editors; profiler and capture views; the HUD-safe-region editor; the review queue ([§6.10](#610-multi-agent-roles-review-and-the-human-director)).

Because every gesture compiles to a command, human edits are attributed, undoable, and diffable exactly like agent edits, and humans and agents can work in the same document under the same lease rules. The editor never has a private path into the engine; if a feature is only reachable from the GUI, that is a protocol bug.
