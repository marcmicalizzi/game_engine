# ADR-0054: The tools UI is Dear ImGui, drawn by the renderer's overlay pass

- **Status:** Proposed
- **Date:** 2026-10-10
- **Plan references:** docs/plan/06-agent-tooling.md §6.10, §6.13; docs/plan/04-renderer.md §4.7; docs/plan/10-roadmap-risks.md §10.6 (R45); docs/plan/02-architecture.md §2.2
- **Docs touched:** `docs/subsystems/apps.md` ("engine-editor"), `docs/subsystems/renderer.md` ("The overlay", "Picking", "Moving an instance"), `docs/subsystems/gfx.md` (`BlendMode`, the backend set), `docs/subsystems/doc_scene.md` (new), `docs/plan/06-agent-tooling.md` §6.10 and §6.13 status notes, `third_party/LICENSES.md`, `AGENTS.md` (the Vulkan confinement sentence)

## Context

The editor (06 §6.13) needs panels — an outliner, property panels generated from schemas, a journal, later a review queue and gizmos — and ADR-0012 adopted Dear ImGui "for tools" without saying how it reaches the screen or what it is not for. Three things constrain the answer:

- **Vulkan is confined** (AGENTS.md, gfx.md "The RHI surface and the backend surface"): only `domain/gfx`, `foundation/window` and an app that presents include the backend set; the renderer records through `gfx::CommandList`. ImGui's own Vulkan backend (`imgui_impl_vulkan`) is a second Vulkan renderer in an app, with its own descriptor pools, pipeline and barriers outside the render graph. Its SDL backend would put SDL outside `foundation/window`.
- **Rendering code lives in `systems/renderer`**, and both hosts get what it does: a pass a UI needs is the renderer's, not the editor's.
- **The game's UI is a different thing.** 04 §4.7 scopes a UI coordinate model (surface, views, HUD-safe region) and 10 §10.6 defers the game UI framework (R45). A tool's panels are not a HUD: they are developer chrome over a viewport.

The alternatives: ImGui's Vulkan backend in the editor (rejected above); a retained-mode toolkit (Qt is LGPL and a second event loop and window system; none of the permissive ones is what agents write fluently); a UI of the engine's own now (that is R45's question, and it would be answered for the wrong consumer).

## Decision

1. **The editor and every in-engine tool draw their panels with Dear ImGui** (MIT, so ADR-0014 allows it), fetched by `cmake/EngineImgui.cmake` — the docking branch's release tag, the core only, **none of ImGui's backends** — with its row in `third_party/LICENSES.md`. An immediate-mode UI is the right shape for a tool whose every widget reads live state (the session's journal, the selection, the document's revision) and whose every edit is a command built at the moment of the gesture; there is no retained widget tree to keep in step with the document. It is also what agents know how to write.
2. **It reaches the screen through the renderer's overlay pass**, never through an ImGui backend: `renderer::OverlayPass` (`systems/renderer/overlay.h`) takes 2D triangle lists in the engine's own vocabulary — a vertex is ImGui's `ImDrawVert` byte for byte, a command a scissor, a texture and a run of indices — and records them through `gfx::CommandList` as the frame's last raster pass, onto the colour image the frame was drawn into (the swapchain image, or the renderer's own target offscreen), straight-alpha blended (`gfx::BlendMode::Alpha`, the only blending pipeline). ImGui's textures are overlay textures in the scene's bindless set, made and retired as ImGui asks (`ImGuiBackendFlags_RendererHasTextures`). Nothing below the host includes ImGui. Window events reach ImGui as `window::Event`s, converted in the host.
3. **SDR only, for now.** The overlay blends in the space the output encode wrote, which is what an immediate-mode UI's colours are authored in on an SDR target. On an HDR target (PQ, scRGB) a UI has to be composited at paper white, in the output's primaries, between the curve and the encode — the **seam in `display_output`** that renderer.md ("HDR output") names. That seam is where the game UI and the tools UI will both composite once it is split into a tone pass and an encode pass; it is named here and not built, and a frame asking for an overlay on an HDR target is refused rather than drawn wrongly.
4. **The tools UI is not the game UI.** R45 stays deferred; 04 §4.7's coordinate model is the game UI's. When R45 is decided, the game UI shares the overlay's place in the frame (decision 3's seam), not ImGui.
5. **The editor hosts the engine in process and talks to it only through the protocol's own objects** — `protocol::Session` for the document, its checked `apply` with a `Caller` for every edit, its undo, redo and journal — until A2's transport lets it be a client of a separate engine-host. Its viewport is the renderer's, through `doc_scene`. An operation the protocol lacks is added as a method, never as a back door.

## Consequences

- **Easier:** a panel is a function of state and a gesture is a command, so stage 2's property edits and gizmo are a few lines each through `EditorDocument::apply`. Any host — engine-view, engine-host for a debug overlay, a game's developer console — composites a UI the same way, because the pass is the renderer's.
- **Harder:** the overlay pass has to stay compatible with what ImGui emits (20-byte vertices, 32-bit indices: `ImDrawIdx` is `unsigned int` by compile definition, so a frame's lists concatenate with no rebasing). ImGui's version is a pin, and its 1.92 texture protocol is what `editor_ui.cpp` implements; a bump re-reads it.
- **Now required:** a pass that composites anything over the picture goes through the overlay or its successor at decision 3's seam, never through a hand-written blend in another pass. `gfx::GraphicsPipelineDesc::blend` exists for it.
- **Forbidden:** ImGui's Vulkan, SDL or any other backend in the tree; ImGui types in a public header below `apps/`; an editor feature that reaches past `protocol::Session` into the document or the renderer's privates.
- **Lint:** `vulkan-backend-include` allows `apps/engine_editor` beside `apps/engine_view`, because it presents (its swapchain and surface); its viewport and panels include none of the backend.

## Revisit when

- R45 chooses a game UI framework: decide whether tools move to it or stay on ImGui at the shared seam.
- The `display_output` split is built for HDR: move the overlay to paper white there and lift the SDR-only refusal.
- A2's transport exists: the editor becomes a client of engine-host, and decision 5's in-process shape is retired.
- ImGui's texture or draw-list protocol changes incompatibly at a version the tree wants.
