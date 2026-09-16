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

# SDL3 (zlib): windowing, input, and Vulkan surface creation (ADR-0012, plan 08 §8.3). Static,
# video and events only for now; audio goes through miniaudio/Steam Audio, rendering through
# our own Vulkan path. Joystick/haptic stay on for gamepads. Without X11/Wayland headers on a
# Linux build machine SDL simply has no video driver and window::init() reports that.
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
FetchContent_MakeAvailable(sdl3)
