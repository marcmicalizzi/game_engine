# gfx: the RHI (ADR-0006). Phase 1 begins with device discovery: enumerate Vulkan physical
# devices and their capabilities without an SDK, so experiment E2 has data from day one.
engine_module(NAME gfx LAYER domain
  DEPS base containers log schemas
  EXTERNAL_DEPS volk::volk
  SOURCES src/adapter.cpp)
# The Vulkan headers are third-party code: the engine's warning policy applies to engine code.
target_include_directories(engine_gfx SYSTEM PUBLIC
  $<TARGET_PROPERTY:Vulkan::Headers,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:volk,INTERFACE_INCLUDE_DIRECTORIES>)

engine_module_tests(NAME gfx
  SOURCES tests/adapter_tests.cpp)
