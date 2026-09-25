# ADR-0037: The scene reader links the ruins capability where it is configured, until placement generators have a registration point

- **Status:** Accepted
- **Accepted as:** an interim, on 2026-09-24. It is replaced by a scene-generator registration point the moment a second generator exists ("Revisit when").
- **Date:** 2026-09-24
- **Plan references:** docs/plan/07-content-pipeline.md §7.6 (the direction note of 2026-09-24 and its status note), docs/plan/02-architecture.md §2.8; builds on ADR-0027 (decisions 1 and 3)
- **Docs touched:** `docs/subsystems/ruins.md`, `docs/subsystems/renderer.md`, `docs/plan/07-content-pipeline.md` §7.6

## Context

The ruin assembler is an optional capability, `domain/ruins` ([ruins](../subsystems/ruins.md)). Its output has to reach a scene where every other placement is expanded: `renderer::read_scene_file`, which turns a scene file's instances, seeded scatters and terrain into the plain instances the renderer loads, and which both hosts call (engine-view's `--scene`, engine-host's `render.load`). [ADR-0027](0027-additive-capabilities.md) decision 1 says a capability never becomes a dependency of a module that is not part of it, because that module could then not be built without it; decision 3 says a capability that needs a registration point that does not exist is a plan change. The scene reader has no registration point for placement generators, and `renderer` is not a capability.

The alternatives on the table:

1. **Make `ruins` a foundation**, like `texture` and `atlas`: always built, a plain dependency. It is an engine-provided generator in 07 §7.6's list, beside the terrain, which already lives inside the renderer. Rejected because a building assembler is exactly the kind of thing a studio that is not making a desert does not want, and the brief it was built to asked for it to be removable.
2. **A caller-supplied expander** in `SceneFileOptions`, set by each host that links the capability. Rejected for now: two hosts and every test that reads a scene would each wire the same callback, and the scene reader would still have to know the `ruins` field exists in order to hand it over.
3. **A registration point**: a constant-initialized table of scene expanders, keyed by the schema type of the scene field they expand, that `read_scene_file` walks and a capability registers into. The right shape once there are several generators — but it is a new attachment surface (ADR-0027 makes those compatibility surfaces), designed from one example.
4. **Link it where it is configured.** The renderer's `CMakeLists.txt` adds `ruins` to its dependencies only when `engine::ruins` exists and defines `ENGINE_RENDERER_RUINS`; without it the renderer builds, and a scene that names ruins is refused with a sentence. This is the pattern engine-view uses for `animation` and engine-content for `tissue`, applied to the one module that reads scene files.

## Decision

Option 4. The renderer depends on `ruins` only in a configuration that has the capability; the expansion is compiled under `ENGINE_RENDERER_RUINS` in `src/scene.cpp` alone, and the `#else` branch refuses the file naming the switch. The minimal build (`msvc-minimal`, `linux-clang-minimal`) is the proof that the renderer builds and passes without the capability, exactly as it is for any capability. No other module links an optional capability this way on the strength of this record: a second case is a new ADR.

## Consequences

A scene can ask for ruins with no flag in either host, and nothing downstream of the scene reader knows a ruin from a scatter. `modules.json` shows `renderer -> ruins` in configurations that have it, and `engine_module()`'s refusal of a non-capability depending on a capability does not fire, because the edge is only declared when the capability is on — which is the point, and which a reviewer reading the renderer's `CMakeLists.txt` finds explained there. The cost is one `#if` in one file and a dependency the capability graph does not describe as a requirement.

**The renderer's link to the ruins capability is the one exception to [ADR-0027](0027-additive-capabilities.md)'s rule that a module which is not a capability never depends on one**, and it is an interim: it is replaced by a scene-generator registration point the moment a second generator exists.

What is forbidden: extending the conditional link to another module or another capability without a new ADR; letting code outside `read_scene_file`'s expansion branch on `ENGINE_RENDERER_RUINS`.

## Revisit when

**The moment a second generator exists** — the building grammar with interiors of 07 §7.6, a road network, rule-driven scatter, anything else that has to expand inside the scene reader: then the scene-generator registration point (option 3) is designed from two examples, `ruins` moves onto it, the renderer's link to the capability is removed, and this record is superseded. That is the trigger, not a review date. Also when world streaming materializes tiles outside the scene reader, which calls the assembler directly and needs no link from the renderer at all.
