# Graphics dependencies (ADR-0006, ADR-0012): the Khronos Vulkan headers and volk, the
# meta-loader that resolves Vulkan entry points at run time. Neither needs the Vulkan SDK: the
# loader library comes with the GPU driver, and volk loads it dynamically, so a machine without a
# driver builds fine and reports "no Vulkan loader" at run time. The SDK is needed only for
# validation layers and shader tooling, which arrive with the render graph and Slang.

include(FetchContent)

set(ENGINE_VULKAN_HEADERS_TAG "vulkan-sdk-1.4.357.0" CACHE STRING "Vulkan-Headers and volk tag")

FetchContent_Declare(vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG        ${ENGINE_VULKAN_HEADERS_TAG}
  GIT_SHALLOW    TRUE)
set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(vulkan_headers)

FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG        ${ENGINE_VULKAN_HEADERS_TAG}
  GIT_SHALLOW    TRUE)
set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
set(VOLK_HEADERS_ONLY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(volk)

# Vulkan Memory Allocator (MIT): header-only; the implementation is compiled once in
# domain/gfx/src/vma.cpp with dynamic entry points from volk.
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG        v3.4.0
  GIT_SHALLOW    TRUE)
set(VMA_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(vma)

# meshoptimizer (MIT): meshlet building, bounds, simplification, and the cluster-LOD DAG
# builder behind the cluster geometry format (docs/plan/04-renderer.md §4.3).
FetchContent_Declare(meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG        v1.2
  GIT_SHALLOW    TRUE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(meshoptimizer)
# Its clusterizer, sphere fit and simplifier are float code whose every decision a fused
# multiply-add can move: compiled with contraction, GCC and Clang at x86-64-v3 build a different
# LOD DAG from MSVC's out of the same bytes. The tree-wide -ffp-contract=off reaches it because it
# is added before this file is included (cmake/EngineFpContraction.cmake, ADR-0035).

# SDL3 (zlib): windowing, input, and Vulkan surface creation (ADR-0012, plan 08 §8.3). Static,
# video and events only for now; audio goes through miniaudio/Steam Audio, rendering through
# our own Vulkan path. Joystick/haptic stay on for gamepads.
#
# --- ENGINE_WINDOW_BACKENDS, and why a headless machine needs a switch at all ------------------
#
# `auto` (the default) is SDL's own detection: it finds X11 and/or Wayland and builds against
# them. `none` builds SDL with **no display backend of any kind** and is what a machine with no
# display server wants.
#
# The sentence this block used to carry — "without X11/Wayland headers SDL simply has no video
# driver and window::init() reports that" — was wrong in both directions, and both halves cost a
# build to find out (docs/ci/remote-linux.md has the transcripts):
#
#  1. **SDL's Unix configure does not "simply" do anything; it refuses.** With neither X11 nor
#     Wayland it stops with a FATAL_ERROR telling you to install the packages, and
#     `SDL_UNIX_CONSOLE_BUILD` is the documented way past it. Worse, a *partial* X11 — which is
#     what a server actually has, because something pulled libX11 in — fails earlier and harder:
#     the headless GPU server has libX11, libXext, libXi and libXfixes but no Xcursor and no
#     Xrandr, so `SDL_X11` stays ON, `CheckX11` finds the base library, and then
#     `SDL_missing_dependency(XCURSOR)` ends the configure. Detection cannot be trusted to mean
#     "this machine wants windows"; it has to be told.
#  2. **Turning X11 and Wayland off is not enough to get "no display" at run time.** SDL's video
#     bootstrap list is tried in order until one `create()` succeeds (`SDL_video.c`), and `dummy`
#     and `offscreen` are in that list and always succeed. A build with those left on reports a
#     working video subsystem on a machine with no display: `SDL_Init(SDL_INIT_VIDEO)` returns
#     true, `window::init()` succeeds, and the window and swapchain tests — which skip on "no
#     display" — would instead run against a driver that cannot make a Vulkan surface.
#
# So `none` switches off the two display backends, the two fake ones, and KMS/DRM (which wants
# libgbm, is a display backend, and is not what a headless build is for), and sets SDL's escape
# hatch so the configure stops asking for X11. `SDL_Init(SDL_INIT_VIDEO)` then fails with "No
# available video device", `window::init()` returns false with that in `error`, and the tests skip
# with the reason they already print — byte for byte what a full build does on a machine whose
# DISPLAY is unset. Events, joysticks and haptics are untouched and still work headless.
set(ENGINE_WINDOW_BACKENDS "auto" CACHE STRING
    "Display backends SDL is built with: auto (detect) or none (headless)")
set_property(CACHE ENGINE_WINDOW_BACKENDS PROPERTY STRINGS auto none)
if(NOT ENGINE_WINDOW_BACKENDS STREQUAL "auto" AND NOT ENGINE_WINDOW_BACKENDS STREQUAL "none")
  message(FATAL_ERROR
    "ENGINE_WINDOW_BACKENDS is '${ENGINE_WINDOW_BACKENDS}'; it is 'auto' (the default, SDL's own "
    "detection) or 'none' (headless: no X11, no Wayland, no KMS/DRM, and no dummy or offscreen "
    "driver standing in for a display). See docs/ci/remote-linux.md.")
endif()

set(ENGINE_SDL_TAG "release-3.4.16" CACHE STRING "SDL3 tag")
FetchContent_Declare(sdl3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        ${ENGINE_SDL_TAG}
  GIT_SHALLOW    TRUE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
if(ENGINE_WINDOW_BACKENDS STREQUAL "none")
  # The two real display backends, and KMS/DRM with them.
  set(SDL_X11 OFF CACHE BOOL "" FORCE)
  set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
  set(SDL_KMSDRM OFF CACHE BOOL "" FORCE)
  # The two that would otherwise stand in for a display and make init() succeed. This pair is the
  # whole reason `none` is a switch of ours rather than just SDL_UNIX_CONSOLE_BUILD.
  set(SDL_DUMMYVIDEO OFF CACHE BOOL "" FORCE)
  set(SDL_OFFSCREEN OFF CACHE BOOL "" FORCE)
  # SDL's own documented escape from "could not find X11 or Wayland development libraries"
  # (SDL's docs/README-cmake.md; the check is in its cmake/macros.cmake). It is read as a plain
  # variable, not declared as an option, so it is set here rather than turned off.
  set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
  message(STATUS "engine window backends: none (headless SDL: no X11, Wayland, KMS/DRM, dummy or offscreen)")
else()
  message(STATUS "engine window backends: auto (SDL detects X11/Wayland)")
endif()
FetchContent_MakeAvailable(sdl3)
