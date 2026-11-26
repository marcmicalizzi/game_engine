# water_live capability (ADR-0027; docs/subsystems/water_live.md): the water's shallow-water step
# on the card, domain/water_state's reference scheme as a compute shader in single precision, held
# to the reference's cases and measured at three grids (roadmap W175).
#
# OPTIONAL creates ENGINE_WITH_WATER_LIVE (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its shader, its tests, and its bench from the build, and
# the rest of the tree still builds and passes. That is the proof that the capability is additive.
# No WHOLE_ARCHIVE: the module registers nothing statically.

# The capability graph (docs/plan/08-toolchain.md section 8.5). `water_state` is itself an optional
# capability and this one links it (its Config, and the reference the device cases are held to),
# so the edge is declared: with that capability switched off, this one is switched off too.
engine_capability_requires(water_live water_state)

# The step's shader (shaders/water_step.slang), five entries in one module. `-fp-mode precise` is
# what puts SPIR-V's NoContraction on every float operation: Slang 2026.17's `precise` alone, with
# -emit-spirv-directly, emits none (docs/experiments/water-step-on-the-card.md, "Contraction").
engine_shaders(NAME water_live SOURCES shaders/water_step.slang FLAGS -fp-mode precise)

engine_module(NAME water_live LAYER systems OPTIONAL
  DEPS base containers math gfx water_state
  SOURCES src/water_live.cpp)
if(TARGET engine_water_live AND TARGET engine_shaders_water_live)
  target_link_libraries(engine_water_live PRIVATE engine_shaders_water_live)
endif()

# DEVICE_FILES: the reference's cases on the card open a device; the stance and the refusals do not.
engine_module_tests(NAME water_live
  SOURCES tests/water_live_tests.cpp tests/water_live_device_tests.cpp tests/size_table.cpp
  DEVICE_FILES water_live_device_tests.cpp)
# The SPIR-V case reads the embedded module, and the device cases hold it to the reference.
if(TARGET engine_water_live_tests AND TARGET engine_shaders_water_live)
  target_link_libraries(engine_water_live_tests PRIVATE engine_shaders_water_live)
endif()

# The step's GPU time at three grids and three states, both forms of the arithmetic.
engine_module_bench(NAME water_live
  SOURCES bench/water_live_bench.cpp)
