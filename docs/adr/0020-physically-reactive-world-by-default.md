# ADR-0020: The world reacts physically by default: destruction on, deformation layer, soft bodies

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/05-simulation.md §5.8, §5.13, docs/plan/04-renderer.md §4.5

## Context

Destruction, surfaces that deform under movement (snow, sand, mud), and non-rigid objects are usually isolated features bolted onto specific props. The owner wants them to be the default behavior of the world, with opt-out rather than opt-in.

## Decision

Destruction is on for every object whose material category defines strength and fracture behavior, disabled only by a per-object `Indestructible` flag (also set by the story-protection guard), a per-material-category override, or a global switch. Destructibles are represented as damage-state mesh variants plus a support graph plus a pre-fractured chunk hierarchy built at content-build time; runtime fracture is a later experiment. Deformable surfaces are a deformation layer over terrain: a per-tile virtual-textured deformation map stamped by a GPU pass from deformer footprints and read by terrain displacement, mirrored by a coarse deterministic CPU grid that gameplay reads, decaying toward baseline through the event scheduler and persisted with the tile. Gameplay-affecting soft bodies use Jolt's XPBD soft-body simulation at LOD0 under a per-tick budget; cosmetic cloth, hair, and foliage use GPU position-based dynamics with no gameplay effect.

## Consequences

Acceleration structures for destruction use prebuilt per-chunk BLAS and variant swaps under the BLAS budget manager. Replays never depend on the GPU deformation map. Particle fluids and finite-element deformation are out of scope for v1.

## Revisit when

Runtime fracture or fluids are scheduled.
