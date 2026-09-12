# ADR-0009: Multi-view rendering as a first-class concept; the HUD coordinate model

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/04-renderer.md §4.6–4.7, docs/plan/01-critique.md §1.4

## Context

The reference extreme display is 11520×2160 across three monitors. A single planar projection at that field of view is distorted at the edges; uniform quality at 25 megapixels is unaffordable; and surround players treat side monitors as peripheral vision. Real-world engines have failed at this resolution by not rendering past 8192 pixels or losing UI past 4096.

## Decision

A `ViewSet` (N views sharing one scene, culling hierarchy, TLAS, and residency budget) is a first-class renderer concept covering surround, split-screen, portrait, and later VR. A user-configurable attention region (default: the center monitor) drives per-view internal resolution, shading rate, ray budget, and effect quality; quality outside it drops aggressively by design. UI elements declare a `UiSpace` (Surface, View, HudSafe, WorldProjected, Reading) and anchor; a `HudSafeRegion` defaults to the center 16:9 in surround and is user-editable. No 16-bit screen coordinates exist anywhere; all screen-space sizes derive from the render configuration; CI renders at 11520×2160, 7680×4320, 1080×3840, and 5120×1440 nightly.

## Consequences

Screen-space passes run per view; presentation supports both driver surround and multi-window. Correct ultrawide behavior is the default for UI developers.

## Revisit when

Never for the principle. Projection choice (per-monitor frusta vs Panini) is settled by E9.
