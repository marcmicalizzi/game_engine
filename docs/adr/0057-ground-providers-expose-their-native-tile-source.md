# ADR-0057: A ground provider exposes its native tile source

- **Status:** Proposed
- **Date:** 2026-10-23
- **Plan references:** docs/plan/02-architecture.md §2.8; docs/plan/04-renderer.md §4.3 and §4.9
- **Docs touched:** [scene_gen](../subsystems/scene_gen.md#the-tile-source), [tileset](../subsystems/tileset.md#what-the-seam-lacks), [renderer](../subsystems/renderer.md#the-ground-from-the-worlds-tiles)

## Context

ADR-0050 makes authored and procedural heights sources of the same streamed terrain levels.
Stock hosts still select those levels only for moving ground. Moreover, a still provider's
generic tile adapter calls `grid`, which cannot refuse a request: an authored set silently
answers point samples when its filtered mip is absent. A renderer dependency on the tileset
capability, or a typed accessor copied into every host, would break ADR-0046's boundary.

## Decision

Extend `scene_gen::GroundOps` with an optional native `tile_source` callback. The capability
returns its own non-owning `TileSource`; `GroundProvider::tiles()` prefers it and otherwise
keeps the existing procedural adapter. The provider's state outlives every consumer and worker
using the source. A source retains its own motion and refusal contracts independently of
`GroundOps::evaluate`. A missing supported filter or unreadable tile fails a lattice request.

Select terrain tiles from the request and the provider's millimetre capability, independently
of motion. The actual source is obtained and checked when the shared terrain layout is built.
A still source never advances its surface time or evaluates future fields. Stock engine-view,
engine-host and the editor viewport use the shared setup and camera-following path.

The authored reader keeps a bounded cache of tile data and indices, rather than an array indexed
by the entire source extent and every tile ever visited. Cache residency is a configurable
budget, with reads, failures, held bytes and evictions visible for acceptance measurements.

Terrain stitch markers use downward rest normals (`y < -0.5`, their x/z angle still naming the
level), replacing ADR-0050's horizontal encoding. An authored cliff may have an almost horizontal
surface normal; a heightfield surface never points down. The former test could interpret a cliff
as an absent level and read outside the GPU table. Ring skirts do not carry the named-level flag.

## Consequences

The renderer and collision name only `scene_gen`; the tileset remains removable. Native terrain
materials and geometry-page streaming are independent work. Terrain vertices still pass through
the existing terrain stage, preserving stitching and the shared surface used by collision.
The cache may reread evicted tiles on return; its budget bounds that memory whatever the source
extent or distance travelled. A missing tile cannot masquerade as valid zero-height terrain.

## Revisit when

Asynchronous source reads or editable source generations need a request lifetime beyond one
lattice call, or several observers require a shared cache admission policy.
