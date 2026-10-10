# Dear ImGui (MIT; ADR-0012 adopted it for tools, ADR-0054 says how it draws): the tools UI that
# engine-editor's panels are written in (docs/subsystems/apps.md, "engine-editor").
#
# The docking branch, because the editor's panels dock round its viewport; a release tag of that
# branch is the pin. Only the core is compiled — imgui.cpp, its draw lists, tables and widgets —
# and **none of ImGui's backends**: its Vulkan backend would put Vulkan in an app, which the
# confinement rule forbids (docs/subsystems/gfx.md, "The RHI surface and the backend surface"), and
# its SDL backend would put SDL outside foundation/window. The editor feeds ImGui its input from
# `window::Event`s and hands the draw lists to the renderer's overlay pass, which draws them through
# `gfx::CommandList` like every other pass (systems/renderer, `overlay.h`).
#
# `ImDrawIdx` is 32 bits so a frame's draw lists concatenate into one index buffer with no rebasing
# and no 64K-vertex limit per window, and the obsolete names are compiled out so nothing written
# against them can start depending on them. The library is created in the top-level directory's
# scope, so it inherits the CPU baseline and -ffp-contract=off like everything else built here;
# its own warnings are silenced and its include directory is SYSTEM, so they never reach our
# warnings-as-errors builds. Fetched in every configuration: the editor is an app, not a capability,
# so there is no switch to hang it on, and the minimal build compiles it like engine-view.

include(FetchContent)

set(ENGINE_IMGUI_TAG "v1.92.9b-docking" CACHE STRING "Dear ImGui tag (the docking branch)")

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        ${ENGINE_IMGUI_TAG}
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  cmake-not-used)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
target_include_directories(imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
target_compile_definitions(imgui PUBLIC
  "ImDrawIdx=unsigned int"
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS
  IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
if(MSVC)
  target_compile_options(imgui PRIVATE /W0)
else()
  target_compile_options(imgui PRIVATE -w)
endif()
add_library(engine::imgui ALIAS imgui)
