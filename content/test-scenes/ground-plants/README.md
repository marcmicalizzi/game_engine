# Ground plants

G103 review scene: seven explicit recipes in a row with three rotated copies behind each.
Left to right: arching fern, autumn fern, creeping fern, broad rosette, flowering rosette,
autumn rosette and low plantain-like rosette. Geometry is procedural and uses flat material
colours unless a pack is explicitly supplied. No binary assets are committed.

The camera path inspects the row at 5, 20 and 60 m. Render offscreen from an isolated build,
under `tools/gpu-lock.ps1 run`; captures and their exact cameras belong under
`D:/workspace/game_engine_local/captures/2026-10-29/ground-plants-g103/`.
The scene deliberately omits tall trees so their detail cannot hide plant silhouettes.
A near individual view is needed to inspect broad leaves, since a 30 cm rosette is small even
at 5 m. The ordinary cluster DAG determines distance detail; these plants have no new impostor.

[Grounding and limitations](../../../docs/experiments/ground-plants.md),
[solver contract](../../../docs/subsystems/flora.md#ground-plant-forms-g103).
The forest POC is read-only for this change; adopting recipes there remains a content decision.
