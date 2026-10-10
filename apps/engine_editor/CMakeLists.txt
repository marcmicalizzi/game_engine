# engine-editor: the editor's first slice (docs/subsystems/apps.md, "engine-editor"; ADR-0054;
# roadmap A31). A window on a document: the renderer's picture of its placements, engine-view's fly
# camera, and Dear ImGui panels drawn through the renderer's overlay pass.
#
# `protocol` because the document is opened through `protocol::Session` and edited through its
# checked write path, never behind it (editor_document.h); `doc_scene` is the bridge from the
# document's placements to the renderer's scene; `imgui` is linked below, not a module.
#
# The fly camera, its input conversion and the walker it carries are engine-view's sources compiled
# in, not copied: one camera, one set of bindings, and the editor flies exactly as the viewer does.
# Without ENGINE_VIEW_WALK_PHYSICS the walker follows the scene's ground, as in the minimal build.
# When a third host wants the camera it should become a module (docs/subsystems/apps.md).
set(_editor_deps base containers hash ids math time jobs log platform window image io gfx renderer
                 json schema schemas input tunables doc protocol doc_scene)
set(_editor_camera ${CMAKE_SOURCE_DIR}/apps/engine_view/fly_camera.cpp
                   ${CMAKE_SOURCE_DIR}/apps/engine_view/window_input.cpp
                   ${CMAKE_SOURCE_DIR}/apps/engine_view/walk.cpp)
set(_editor_sources editor_document.cpp editor_viewport.cpp editor_ui.cpp)

engine_app(NAME engine_editor OUTPUT engine-editor
  SOURCES main.cpp ${_editor_sources} ${_editor_camera}
  DEPS ${_editor_deps}
  E2E_TESTS tests/editor_tests.cpp ${_editor_sources})
# engine-editor presents, so like engine-view it sees the Vulkan backend set (the swapchain and the
# surface); its viewport and its panels record through the renderer and include none of it
# (docs/subsystems/gfx.md, "The RHI surface and the backend surface").
target_link_libraries(engine_engine_editor PRIVATE engine::gfx_vulkan imgui)
if(TARGET engine_engine_editor_tests)
  target_link_libraries(engine_engine_editor_tests PRIVATE imgui)
endif()
