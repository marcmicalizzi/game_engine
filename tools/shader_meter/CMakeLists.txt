# shader-meter: runs each slangc compile of engine_shaders() and writes its peak memory and seconds
# beside the SPIR-V, which build.shader_pool holds against cmake/shader-costs.txt (roadmap T118;
# cmake/EngineShaders.cmake). Standard library and the operating system only, like schemac.
add_executable(engine_shader_meter src/main.cpp)
target_compile_features(engine_shader_meter PRIVATE cxx_std_20)
engine_apply_warnings(engine_shader_meter)
set_target_properties(engine_shader_meter PROPERTIES OUTPUT_NAME shader-meter)
if(WIN32)
  target_link_libraries(engine_shader_meter PRIVATE psapi)
endif()
# A host tool (ADR-0034): the build runs it on the build machine and it never ships.
engine_host_tool(engine_shader_meter)
