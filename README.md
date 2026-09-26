# Agent-Native High-Performance Game Engine

An engine built from first principles around four priorities: rendering efficiency and fidelity (cluster geometry, hybrid ray tracing, extreme resolutions), hardware-aware execution, agent-native development tooling where every developer operation is a structured API, and persistent, systemic, physically reactive worlds.

**Status** lives where it is kept current, not here: the phase ledger in [docs/plan/10-roadmap-risks.md §10.8](docs/plan/10-roadmap-risks.md#108-calibrating-the-estimates) records each phase's estimate, actual and exit criteria as they are met, and every plan page carries dated status notes beside the intent it reports on. In one line as of 2026-09-25: Phase 0 (foundations) closed on 2026-09-15; Phase 1 (renderer core) has cluster geometry with a LOD DAG, GPU culling and two-pass occlusion, a 64-bit visibility buffer with mesh-shader and vertex-shader rasterizers, a material resolve with ray-traced and cascaded-map shadows, geometry streaming, a GPU path tracer as the reference, flythrough benchmarks, and an engine-host protocol over all of it; pieces of Phases 3 and 5 exist where the renderer needed them (ECS, persistent store, sim scheduler, physics, navigation, animation, glTF import, the content build and its derived-data cache, the character standard's first data), and Phase 3's exit is met: a tile-streamed world with 10^5 scheduled NPCs saves, loads and replays to one persistent-state hash on every compiler ([10 §10.2](docs/plan/10-roadmap-risks.md)).

- **Plan**: [docs/plan/README.md](docs/plan/README.md) is the technical plan and the source of intent.
- **Decisions**: [docs/adr/README.md](docs/adr/README.md) indexes the architecture decision records.
- **Modules**: [docs/subsystems/README.md](docs/subsystems/README.md) has one page per module — purpose, owned data, invariants, public API, how to test.
- **Measurements**: [docs/experiments/README.md](docs/experiments/README.md) holds the numbers the decisions rest on.
- **Working in this repo**: [AGENTS.md](AGENTS.md) covers build, test, layering and conventions for humans and agents alike. It is the entry point every session reads first.

## Layout

Source is one directory per module, layered lowest to highest, and CMake refuses a dependency that points up:

| Directory | Layer | Holds |
|---|---|---|
| `core/` | L0 | platform, memory, containers, math, jobs, log, schema runtime, serialization, hash, time, ids |
| `foundation/` | L1 | io, image, input, window, store, scripting, tunables, bench |
| `domain/` | L2 | gfx (RHI and render graph), geometry, assets, atlas, physics, nav, anim, ecs, doc, protocol, sim, tissue, audio |
| `systems/` | L3 | renderer, animation, audio_system |
| `apps/` | L4 | the executables below |
| `game/` | L5 | per-game code (none yet) |

The executables build into `build/<preset>/bin/`: `engine-host` (the engine as a JSON-RPC 2.0 server), `engine-cli` (its command-line client), `engine-view` (renders a mesh or a scene to a window or a PNG, flies camera paths, benchmarks), `engine-content` (the content build: glTF to `.clusters`, images to block-compressed `.tex`, the derived-data cache, tissue definitions), `engine-image` (image decode and encode checks), `engine-input` (the input-device probe), and `engine-mcp` (the MCP bridge: a Model Context Protocol server for agents' clients over an engine-host of its own). `docs/subsystems/apps.md` documents each.

`schemas/` is the IDL source of truth, `tools/` holds the PowerShell 7 scripts and small C++ tools that drive everything, `content/` holds test scenes, golden images and recorded input, and `third_party/` holds vendored dependencies under permissive licenses only.

## Build

Requirements: Visual Studio 2026 with the C++ workload (bundled CMake and Ninja are used automatically), PowerShell 7. On Linux: Clang 18+ or GCC 14+, CMake 3.28+, Ninja. Every preset compiles for x86-64-v3; the `-v2` presets exist for the two older GPU test machines.

```powershell
tools/dev.ps1 configure
tools/dev.ps1 build
tools/dev.ps1 test
```

`tools/dev.ps1 lint` and `tools/dev.ps1 docs` run the checked rules; `tools/linux-build.ps1` runs the Linux presets in a container on the same machine. AGENTS.md has the rest.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). Games built with the engine owe attribution only. The engine's own reference games are licensed apart from it, in repositories of their own: Desert Survival's code under GPL-3.0 with non-commercial assets and a trademarked title, so that it stays readable and buildable as the reference implementation while a low-effort clone cannot be sold ([ADR-0047](docs/adr/0047-reference-games-are-licensed-apart-from-the-engine.md)); Island City is an engine demo under the engine's terms unless it becomes a game.
