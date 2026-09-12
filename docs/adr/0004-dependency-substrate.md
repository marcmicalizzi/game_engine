# ADR-0004: One incremental dependency substrate with derived and authored nodes

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/03-data-model.md §3.6, docs/plan/07-content-pipeline.md §7.3

## Context

The asset pipeline (mesh to clusters, tile to navmesh) and world-generation canon (a change to world history invalidating faction motivations and quests) are the same problem: derived values with declared inputs that must be invalidated when inputs change. Retrofitting dependency tracking onto a pipeline is a rewrite.

## Decision

A single content-addressed substrate identifies every node by hash(function identity + version, input hashes). `derived` nodes are pure functions recomputed automatically in parallel and cached. `authored` nodes hold human or agent judgment; on input change they are marked stale, keep their old value live, and enqueue a review item. Build functions read inputs through tracked accessors so dependencies are recorded, not maintained by hand; undeclared reads are blocked in the build sandbox. Storage is a content-addressed file store with an SQLite index.

## Consequences

Asset builds, semantic and visual world mips, validation caches, playtest result caches, and canon invalidation share one implementation. Nothing stale is used silently; nothing authored is regenerated without a decision.

## Revisit when

Never for the principle. Storage backend may change with a new ADR.
