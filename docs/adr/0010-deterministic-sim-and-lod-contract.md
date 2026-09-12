# ADR-0010: Fixed-step deterministic simulation; LOD as a system property; the materialization contract

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/05-simulation.md §5.1–5.5, §5.10, docs/plan/02-architecture.md §2.4

## Context

Replay, automated playtesting, regression testing, persistent worlds with large NPC populations, and eventual multiplayer all require a simulation that is deterministic and that scales work to what can matter.

## Decision

The simulation runs at a fixed step (60 Hz default), seeded, with no wall-clock reads, replayable from the input and event logs; rendering is decoupled and interpolates. Tick counters and game time are 64-bit integers. LOD is a property of systems, not entities: each system declares the tiers it runs at and implements `Materialize`, `Promote`, `Demote`, `Dematerialize`, and (for LOD2/3) `SummarizeInterval` for fast-forward. Tier assignment is a function over a set of observers with hysteresis. Temporal LOD is a hierarchical timing wheel over game time.

## Consequences

CI asserts identical state hashes across replays. Tile activation reconciles elapsed time through summary functions with bounded cost. Aggregate systems must be written in time proportional to aggregates, not NPC count.

## Revisit when

Never for determinism. Tier count and tile size are per-game configuration, not ADR matters.
