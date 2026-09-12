# ADR-0003: Event-sourced persistent world state in SQLite; non-determinism is logged

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/03-data-model.md §3.5, §3.10

## Context

Persistent worlds, sim LOD, save games, replay, and agent introspection all need one authoritative record of what has happened since the world was authored, independent of the per-frame runtime representation.

## Decision

Persistent state is an append-only log of schema-typed `WorldEvent`s (with sim tick, game time, subject, causal parent, depth, tile, origin) plus projections of current state and periodic snapshots, stored in SQLite in WAL mode. Systems read projections during play; nothing scans the log per frame. Every non-deterministic input (player input, LLM output, external generator output, live agent edits, wall-clock reads) is recorded as an event with its origin and is replayed from the log, never re-executed.

## Consequences

Save game = snapshot + tail. Replays are exact. Agents and tests query world state with SQL. Schema evolution uses versioned events with upcasters and a migration corpus. The runtime world is a materialization of document + persistent state and is never the source of truth.

## Revisit when

E6 shows SQLite write throughput or query latency limiting the sim at target NPC counts.
