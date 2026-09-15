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
