# gfx: the RHI (ADR-0006). Device discovery, instance and device creation with the feature
# chain the renderer needs, VMA-backed resources, and one-shot command submission. The render
# graph and the renderer systems build on this.
# Shaders every consumer of the RHI may draw with (the cluster path); test-only shaders stay in
# engine_shaders_gfx_tests below.
# No DEPENDS: engine_shaders() walks each source's #include graph itself, so brdf.slang,
# scene.slang, and visibility_resolve.slang (which visibility_resolve_rt.slang includes with the
# shadow rays defined in) reach exactly the shaders that include them (cmake/EngineShaders.cmake).
engine_shaders(NAME gfx SOURCES shaders/cluster_mesh.slang shaders/cluster_cull.slang
                             shaders/cluster_sw_raster.slang shaders/visibility_resolve.slang
                             shaders/visibility_resolve_rt.slang
                             shaders/hiz_build.slang shaders/cluster_vertex.slang
                             shaders/cluster_vertex_indexed.slang shaders/vertex_expand.slang
                             shaders/ray_visibility.slang shaders/clas_records.slang
                             shaders/deform.slang shaders/deform_alloc.slang
                             shaders/path_trace.slang)
engine_module(NAME gfx LAYER domain
  DEPS base containers memory hash json log math platform schemas io
  EXTERNAL_DEPS volk::volk GPUOpen::VulkanMemoryAllocator
  SOURCES src/adapter.cpp src/device.cpp src/device_caps.cpp src/requirements.cpp src/vma.cpp
          src/frame.cpp src/render_graph.cpp
          src/bindless.cpp src/swapchain.cpp src/capture.cpp src/shader_library.cpp
          src/gpu_timer.cpp src/acceleration.cpp src/cluster_acceleration.cpp)
target_link_libraries(engine_gfx PUBLIC engine_shaders_gfx)
# Third-party headers: the engine's warning policy applies to engine code only.
target_include_directories(engine_gfx SYSTEM PUBLIC
  $<TARGET_PROPERTY:Vulkan::Headers,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:volk,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:VulkanMemoryAllocator,INTERFACE_INCLUDE_DIRECTORIES>)

engine_module_tests(NAME gfx
  SOURCES tests/adapter_tests.cpp tests/device_tests.cpp tests/requirements_tests.cpp
          tests/size_table.cpp)

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
                                          tests/visibility_tests.cpp tests/occlusion_tests.cpp
                                          tests/shading_tests.cpp tests/attributes_tests.cpp
                                          tests/vertex_path_tests.cpp tests/cone_tests.cpp
                                          tests/rt_tests.cpp tests/rt_cluster_tests.cpp
                                          tests/clas_records_tests.cpp tests/scene_tests.cpp
                                          tests/deform_tests.cpp tests/skin_tests.cpp tests/morph_deform_tests.cpp
                                          tests/sampling_tests.cpp tests/stream_tests.cpp)
  # ENGINE_SOURCE_DIR is where the morph test looks for the Khronos samples tools/fetch-samples.ps1
  # fetches into content/samples (through tests/support/test_paths.h, so a bundle made with
  # -WithSamples finds its own copy); without them that case says so and passes.
  target_compile_definitions(engine_gfx_tests PRIVATE
    ENGINE_SHADER_MANIFEST="${CMAKE_BINARY_DIR}/shaders/manifest.json"
    ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  # engine::anim is the CPU reference the skinning test holds `deform.slang` to; engine::assets
  # imports the morph samples for the chain's real-file case. Tests only: gfx itself reads neither.
  target_link_libraries(engine_gfx_tests PRIVATE engine_shaders_gfx_tests engine::geometry
                                                 engine::anim engine::assets engine::window
                                                 engine::image)
endif()
