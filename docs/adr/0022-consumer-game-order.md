# ADR-0022: Desert Survival is the first consumer game; Island City the second

- **Status:** Accepted
- **Date:** 2026-09-15
- **Plan references:** docs/plan/13-reference-consumer-games.md, docs/plan/10-roadmap-risks.md §10.2 and §10.4

## Context

Two reference consumer games are recorded in plan 13. Desert Survival stresses sparse, effectively infinite, dynamically deforming terrain at extreme view distances with a small asset set. Island City stresses finite but extreme density: functional procedural interiors, crowds, partial interior visibility, vertical streaming, destruction with consequences, and an urban-to-wilderness transition. Both are needed: they exercise opposite ends of what the engine must handle, and neither alone would surface the other's failure modes.

## Decision

Desert Survival is built first, as the Phase 6 exit demo made playable and the first shippable. Island City follows as the density benchmark environment and the candidate setting for the narrative game. Desert Survival is not simple, but it is far simpler than Island City and needs far fewer assets to reach a playable proof of concept, so it exposes engine bugs and design gaps earlier and more cheaply, before the dense scenario multiplies their cost. Systems are still designed with both consumers in view (plan 13 §13.4); the order governs when each is exercised end to end, not what is designed for.

## Consequences

Terrain, deformation, procedural generation, streaming of unbounded worlds, day and night, the camera rigs, and scored deterministic runs get their first real consumer in Phase 6. Interiors as a materialization hierarchy, RT relevance LOD through windows, procedural architecture, and dense persistent NPC populations are designed for from Phase 3 but first exercised end to end after Desert Survival ships. Benchmark scenes for both exist from Phase 1 (the dune vista and a downtown block) so renderer work is measured against both regimes throughout.

## Revisit when

Desert Survival slips badly enough that a smaller density benchmark would surface renderer problems sooner, or a gameplay concept for Island City emerges that changes what the first shippable should be.
