# engine-editor: the engine's window on a document (docs/subsystems/apps.md, "engine-editor";
# ADR-0054; roadmap A34, A37). The web shell (apps/engine_shell) in the platform's web view beside
# the renderer's live viewport in a child window, both clients of the protocol listener the editor
# runs in process; engine-view's fly camera, picking, a translate gizmo, undo/redo/delete, and the
# viewport's own overlay (the selection's box, the gizmo) in Dear ImGui drawn through the
# renderer's overlay pass. `viewport_math.cpp` is the pointer's maths, `editor_input.cpp` what it
# does with it, and `editor_methods.cpp` the three methods the shell and the viewport share.
#
# `protocol` because the document is opened through `protocol::Session` and edited through its
# checked write path, never behind it (editor_document.h), and because its listener and dispatcher
# serve the shell; `net` for the listener's loopback address; `doc_scene` is the bridge from the
# document's placements to the renderer's scene; `imgui` is linked below, not a module.
#
# The fly camera, its input conversion and the walker it carries are engine-view's sources compiled
# in, not copied: one camera, one set of bindings, and the editor flies exactly as the viewer does.
# Without ENGINE_VIEW_WALK_PHYSICS the walker follows the scene's ground, as in the minimal build.
# When a third host wants the camera it should become a module (docs/subsystems/apps.md).
set(_editor_deps base containers hash ids math time jobs log platform window image io gfx renderer
                 json schema schemas input tunables doc protocol doc_scene net assets geometry)
set(_editor_defs)
# The web view is a capability (ENGINE_WITH_WEBVIEW; foundation/webview): linked when this
# configuration has it, as engine-host links its optional modules. Without it — the minimal build,
# every platform but Windows — the window is the viewport alone and says where the shell is.
if(TARGET engine::webview)
  list(APPEND _editor_deps webview)
  list(APPEND _editor_defs ENGINE_EDITOR_WEBVIEW=1)
endif()
# The scene generators this configuration has (ADR-0046; docs/subsystems/scene_gen.md), linked for
# their registrars exactly as engine-view and engine-host link them: the viewport draws the document
# scene through the renderer's scene reader, which finds a terrain's ground provider (the dunes, or
# the tile set's `tiles` for a Heightfield record) and a placement's generator by the name the scene
# gives, and refuses a name this executable does not carry. Nothing here calls one.
foreach(_gen authored city cover deadwood flora rocks ruins scatter sky terrain tileset water wetland)
  if(TARGET engine::${_gen})
    list(APPEND _editor_deps ${_gen})
  endif()
endforeach()
set(_editor_camera ${CMAKE_SOURCE_DIR}/apps/engine_view/fly_camera.cpp
                   ${CMAKE_SOURCE_DIR}/apps/engine_view/window_input.cpp
                   ${CMAKE_SOURCE_DIR}/apps/engine_view/walk.cpp)
set(_editor_sources editor_document.cpp editor_viewport.cpp editor_ui.cpp editor_input.cpp editor_import.cpp
                    editor_methods.cpp viewport_math.cpp)

engine_app(NAME engine_editor OUTPUT engine-editor
  SOURCES main.cpp ${_editor_sources} ${_editor_camera}
  DEPS ${_editor_deps}
  E2E_TESTS tests/editor_tests.cpp ${_editor_sources})
if(_editor_defs)
  target_compile_definitions(engine_engine_editor PRIVATE ${_editor_defs})
endif()
# engine-editor presents, so like engine-view it sees the Vulkan backend set (the swapchain and the
# surface); its viewport and its overlay record through the renderer and include none of it
# (docs/subsystems/gfx.md, "The RHI surface and the backend surface").
target_link_libraries(engine_engine_editor PRIVATE engine::gfx_vulkan imgui)
# The shell's files, which engine-host's CMakeLists copies to build/<preset>/shell/ and the editor's
# listener serves from <bin>/../shell.
if(TARGET engine_shell_files)
  add_dependencies(engine_engine_editor engine_shell_files)
endif()
if(TARGET engine_engine_editor_tests)
  target_link_libraries(engine_engine_editor_tests PRIVATE imgui)
  # The sample import finds content/samples through the source tree CMake names (or the test
  # bundle's copy: tests/support/test_paths.h), never by walking up from __FILE__, which the
  # compiler cache makes relative to the build directory (docs/ci/local-linux.md).
  target_compile_definitions(engine_engine_editor_tests PRIVATE
    ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
