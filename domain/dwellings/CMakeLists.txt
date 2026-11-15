# dwellings capability (ADR-0027): a house's interior as a plan — rooms as a graph, inner walls,
# doorways, an enclosed stair, hearths, where the doors and windows sit — the rural filler that
# chooses a house for a plot and a household, and the building description a plan becomes
# (docs/subsystems/dwellings.md; plan 14 §14.3, roadmap G66 and G72).
#
# OPTIONAL creates ENGINE_WITH_DWELLINGS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module and its tests from the build, and the rest of the tree
# still builds and passes. That is the proof that the capability is additive.
#
# Layer: domain, beside the other generators. It writes the structures capability's description
# and so requires it: with ENGINE_WITH_STRUCTURES off this capability is off too.

engine_capability_requires(dwellings structures)

engine_module(NAME dwellings LAYER domain OPTIONAL
  DEPS base containers hash math structures structures_schemas
  SOURCES src/interiors.cpp src/rural_filler.cpp src/tour.cpp src/furnish.cpp src/describe.cpp)

engine_module_tests(NAME dwellings
  SOURCES tests/interiors_tests.cpp tests/rural_filler_tests.cpp tests/tour_tests.cpp
          tests/furnish_tests.cpp tests/describe_tests.cpp tests/determinism_tests.cpp
          tests/size_table.cpp)
