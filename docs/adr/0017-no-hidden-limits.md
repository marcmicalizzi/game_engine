# ADR-0017: No hidden limits: checked compact types, tile-relative positions, 64-bit time, configurable budgets

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/11-performance-principles.md §11.6, docs/plan/02-architecture.md §2.7

## Context

Shipped engines have failed at 11520×2160 by not rendering past 8192 pixels or losing their UI past 4096, because of hard-coded render-target caps and 12- or 16-bit packed screen coordinates. Float32 world positions jitter beyond roughly 10 km; float seconds drift over long sessions. The engine must not place such limits on games built with it.

## Decision

No hard-coded maximum exists for resolution, view count, lights, bones, entities, instances, texture size, or coordinate range; every limit is a configurable budget with a validator. Compact screen-space or index representations are permitted only through a checked type constructed from the render configuration, exposing `CanRepresent(config)` and falling back to the wider layout rather than clamping, wrapping, or truncating; the wide layout is the default and the compact one must earn its place with cache-density measurements. All render targets, atlases, tile grids, and screen-space buffers are sized from the render configuration at startup. World positions are (tile index, float local offset) with camera-relative rendering. Time is 64-bit integer ticks. IDs are 128-bit. CI renders at 11520×2160, 7680×4320, 1080×3840, and 5120×1440 nightly with UI enabled.

## Consequences

A class of real-world failures is closed by rule and test. Some structures cost a few more bytes than a fixed-size design would.

## Revisit when

Never.
