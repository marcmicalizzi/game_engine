# gfx: the RHI (ADR-0006). Device discovery, instance and device creation with the feature
# chain the renderer needs, VMA-backed resources, and one-shot command submission. The render
# graph and the renderer systems build on this.
engine_module(NAME gfx LAYER domain
  DEPS base containers memory log schemas
  EXTERNAL_DEPS volk::volk GPUOpen::VulkanMemoryAllocator
  SOURCES src/adapter.cpp src/device.cpp src/vma.cpp)
# Third-party headers: the engine's warning policy applies to engine code only.
target_include_directories(engine_gfx SYSTEM PUBLIC
  $<TARGET_PROPERTY:Vulkan::Headers,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:volk,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:VulkanMemoryAllocator,INTERFACE_INCLUDE_DIRECTORIES>)

engine_module_tests(NAME gfx
  SOURCES tests/adapter_tests.cpp tests/device_tests.cpp)

# Shaders used by the RHI's own tests: the first .slang through the toolchain.
engine_shaders(NAME gfx_tests SOURCES shaders/fill.slang)
if(TARGET engine_gfx_tests)
  target_sources(engine_gfx_tests PRIVATE tests/compute_tests.cpp)
  target_link_libraries(engine_gfx_tests PRIVATE engine_shaders_gfx_tests)
endif()
