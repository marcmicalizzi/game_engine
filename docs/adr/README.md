# Architecture Decision Records

Numbered, dated, and immutable once accepted. To change a decision, write a new ADR that supersedes the old one and mark the old one `Superseded by ADR-NNNN`. Use [0000-template.md](0000-template.md).

| ADR | Title | Status |
|---|---|---|
| [0001](0001-headless-engine-server.md) | Headless engine server with a versioned protocol; the editor is a client | Accepted |
| [0002](0002-text-layered-authoring-document.md) | Text-serialized, layered, stable-ID authoring document in canonical JSON | Accepted |
| [0003](0003-event-sourced-persistent-state.md) | Event-sourced persistent world state in SQLite; non-determinism is logged | Accepted |
| [0004](0004-dependency-substrate.md) | One incremental dependency substrate with derived and authored nodes | Accepted |
| [0005](0005-cluster-geometry-hybrid-renderer.md) | The cluster as the universal geometry unit; hybrid renderer; reference path tracer from day one | Accepted |
| [0006](0006-vulkan-primary-graphics-api.md) | Vulkan primary behind a thin API-neutral RHI; Windows first, Linux second; D3D12 deferred | Accepted |
| [0007](0007-schema-code-generation.md) | Schema code generation; no hand-written serialization | Accepted |
| [0008](0008-slang-shaders.md) | Slang for all shaders | Accepted |
| [0009](0009-multi-view-and-hud-coordinates.md) | Multi-view rendering as a first-class concept; the HUD coordinate model | Accepted |
| [0010](0010-deterministic-sim-and-lod-contract.md) | Fixed-step deterministic simulation; LOD as a system property; the materialization contract | Accepted |
| [0011](0011-tunables-before-calibration.md) | Tunables registry now; calibration runner only with evidence | Accepted |
| [0012](0012-adopted-libraries.md) | Adopted libraries: Jolt, Recast, SDL3, Tracy, mimalloc, SQLite, meshoptimizer; flecs pending E6 | Accepted |
| [0013](0013-cpp20-only.md) | C++20 as the only core language | Accepted |
| [0014](0014-apache-2-license-and-dependency-policy.md) | Apache-2.0 engine license and the dependency policy that follows | Accepted |
| [0015](0015-ai-usage-policy.md) | AI usage policy: agent outputs are content and code, never training data | Accepted |
| [0016](0016-multiplayer-readiness.md) | Multiplayer-readiness rules enforced from the start | Accepted |
| [0017](0017-no-hidden-limits.md) | No hidden limits: checked compact types, tile-relative positions, 64-bit time, configurable budgets | Accepted |
| [0018](0018-frame-budgets-as-merge-gates.md) | Frame budgets are merge gates from the first renderer milestone | Accepted |
| [0019](0019-efficiency-first-class.md) | Efficiency as a first-class metric: size table, engine container set, banned containers, fast-path APIs | Accepted |
| [0020](0020-physically-reactive-world-by-default.md) | The world reacts physically by default: destruction on, deformation layer, soft bodies | Accepted |
| [0021](0021-build-system-and-tooling.md) | CMake + Ninja, `engine_module()` manifests, PowerShell and C++ tooling, no Python | Accepted |
| [0022](0022-consumer-game-order.md) | Desert Survival is the first consumer game; Island City the second | Accepted |
