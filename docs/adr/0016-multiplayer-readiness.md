# ADR-0016: Multiplayer-readiness rules enforced from the start

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/05-simulation.md §5.12, docs/plan/02-architecture.md §2.7

## Context

The first game is single-player, but multiplayer is a stated future option, and the engine carries most of the machinery (transport, replication, authority, interest management, prediction). Certain habits make multiplayer a rewrite if they take root.

## Decision

Multiplayer transport, replication, and prediction are not built now. The following rules are enforced from Phase 3 so they never need unpicking: no wall-clock reads in gameplay; no "the player" singleton (there is a set of players, possibly of size one, and a set of observers); no gameplay logic in render code; all state mutations go through commands or events; physics and gameplay run on sim ticks and every event and input carries its tick number; components declared in the schema reserve replication annotations (`replicated`, `owner_only`, `reliable`, `interpolated`) for later code generation. The headless `sim` mode is the dedicated server.

## Consequences

Lockstep and rollback models are supported by the deterministic sim (ADR-0010) directly. A persistent destructible world plus multiplayer is acknowledged as a hard combination to be scoped as its own project.

## Revisit when

Multiplayer is scheduled; that ADR chooses the model and transport.
