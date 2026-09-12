# ADR-0002: Text-serialized, layered, stable-ID authoring document in canonical JSON

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/03-data-model.md §3.1–3.3

## Context

Agents must diff, review, merge, attribute, and roll back world edits. Binary level formats make all of that impossible. OpenUSD offers the right vocabulary but its composition engine makes edits non-local and its build is heavy.

## Decision

The authoring document is a tree of schema-typed objects with 128-bit stable IDs, composed from an ordered stack of sparse layers where the strongest layer wins per (object, property). No other composition operators in v1. Serialized as canonical JSON (sorted keys, fixed number formatting) one file per tile per layer, with large blobs stored separately by content hash. Commands are the only mutation path; transactions produce structural forward and inverse patches with attribution. Diff and merge are structural, keyed by object ID and property. USD interop is import/export through a dependency-free library, not the authoring format.

## Consequences

Every edit is diffable and reviewable in git and in the editor. Prefabs are instantiate-and-record-origin rather than live composition. Experiment E4 confirms the format against LightUSD before Phase 3 depends on it.

## Revisit when

E4 shows JSON diff churn or merge quality is unacceptable for agent workflows.
