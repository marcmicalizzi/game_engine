# gfx: the RHI (ADR-0006). Device discovery, instance and device creation with the feature
# chain the renderer needs, VMA-backed resources, and one-shot command submission. The render
# graph and the renderer systems build on this.
# Shaders every consumer of the RHI may draw with (the cluster path); test-only shaders stay in
# engine_shaders_gfx_tests below.
engine_shaders(NAME gfx SOURCES shaders/cluster_mesh.slang shaders/cluster_cull.slang
                             shaders/cluster_sw_raster.slang shaders/visibility_resolve.slang)
engine_module(NAME gfx LAYER domain
  DEPS base containers memory hash json log math platform schemas io
  EXTERNAL_DEPS volk::volk GPUOpen::VulkanMemoryAllocator
  SOURCES src/adapter.cpp src/device.cpp src/vma.cpp src/frame.cpp src/render_graph.cpp
          src/bindless.cpp src/swapchain.cpp src/capture.cpp src/shader_library.cpp
          src/gpu_timer.cpp)
target_link_libraries(engine_gfx PUBLIC engine_shaders_gfx)
# Third-party headers: the engine's warning policy applies to engine code only.
target_include_directories(engine_gfx SYSTEM PUBLIC
  $<TARGET_PROPERTY:Vulkan::Headers,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:volk,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:VulkanMemoryAllocator,INTERFACE_INCLUDE_DIRECTORIES>)

engine_module_tests(NAME gfx
  SOURCES tests/adapter_tests.cpp tests/device_tests.cpp)

# Shaders used by the RHI's own tests: the first .slang through the toolchain.
engine_shaders(NAME gfx_tests
  SOURCES shaders/fill.slang shaders/scale.slang shaders/bindless_copy.slang shaders/bda_fill.slang
          shaders/triangle.slang)
if(TARGET engine_gfx_tests)
  target_sources(engine_gfx_tests PRIVATE tests/compute_tests.cpp tests/frame_tests.cpp
                                          tests/render_graph_tests.cpp tests/bindless_tests.cpp
                                          tests/raster_tests.cpp tests/mesh_shader_tests.cpp
                                          tests/capture_tests.cpp tests/swapchain_tests.cpp
                                          tests/shader_library_tests.cpp tests/cull_tests.cpp
                                          tests/visibility_tests.cpp)
  target_compile_definitions(engine_gfx_tests PRIVATE
    ENGINE_SHADER_MANIFEST="${CMAKE_BINARY_DIR}/shaders/manifest.json")
  target_link_libraries(engine_gfx_tests PRIVATE engine_shaders_gfx_tests engine::geometry
                                                 engine::window engine::image)
endif()
