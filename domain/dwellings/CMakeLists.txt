# dwellings capability (ADR-0027): a house's interior as a plan — rooms as a graph, inner walls,
# doorways, an enclosed stair, hearths, where the doors and windows sit — and the rural filler
# that chooses a house for a plot and a household (docs/subsystems/dwellings.md; plan 14 §14.3,
# roadmap G66 and G72).
#
# OPTIONAL creates ENGINE_WITH_DWELLINGS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module and its tests from the build, and the rest of the tree
# still builds and passes. That is the proof that the capability is additive.
#
# Layer: domain, beside the other generators. The plan itself needs nothing but core: integers,
# the containers and the engine's hash.

engine_module(NAME dwellings LAYER domain OPTIONAL
  DEPS base containers hash
  SOURCES src/interiors.cpp src/rural_filler.cpp)

engine_module_tests(NAME dwellings
  SOURCES tests/interiors_tests.cpp tests/rural_filler_tests.cpp tests/determinism_tests.cpp
          tests/size_table.cpp)
