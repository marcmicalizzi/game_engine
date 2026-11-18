# events capability (ADR-0027): the world event bus of docs/plan/05-simulation.md §5.7 — events as
# schema types, raised by systems, named sources and the document's change feed, delivered inside
# the scheduler's phases in an order that is a function of the world, under runaway guards.
#
# OPTIONAL creates ENGINE_WITH_EVENTS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive: the
# scheduler (domain/sim) knows nothing of it, and it attaches through the scheduler's table.
# WHOLE_ARCHIVE keeps the module's static registration objects — its tunables and schema types —
# from being dropped by the linker because nothing references them.

# The bus's own event types (what the document's feed raises). CAPABILITY ties them to the same
# switch, so they leave the build with it.
engine_schema_library(NAME events_schemas SCHEMAS schemas/events.schema CAPABILITY events)

engine_module(NAME events LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers hash ids log time schema json sim doc tunables events_schemas io
  SOURCES src/events.cpp src/external_recording.cpp)

# Event types for the tests and the bench.
engine_schema_library(NAME events_test_types SCHEMAS tests/events_test.schema CAPABILITY events)

engine_module_tests(NAME events
  SOURCES tests/events_tests.cpp tests/guard_tests.cpp tests/size_table.cpp
  DEPS jobs memory events_test_types)

# Delivery's cost per event at 10^3 and 10^5 events a tick, in order and from a parallel wave
# (docs/subsystems/events.md, "Performance").
engine_module_bench(NAME events
  SOURCES bench/events_bench.cpp
  DEPS jobs events_test_types)
