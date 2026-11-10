# landform capability (ADR-0027; docs/subsystems/landform.md, roadmap W95): an offline landform
# solver. A region sketch (the lattice, each edge open, closed or sea, outlets, uplift, the rock's
# resistance, a seed, the time and step, an optional starting heightfield) becomes an eroded ground:
# an authored heightfield the tile set builds as it is, and the fields the later solvers read
# (drainage area, receiver, slope, erosion rate, fill). Priority-Flood routing, the stream-power law
# solved implicitly along the flow tree, hillslope diffusion with a critical slope.
#
# OPTIONAL creates ENGINE_WITH_LANDFORM (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema types, its tests and its bench, and the rest of
# the tree still builds and passes; `engine-content landform` then says the capability is absent.
# It registers nothing (no generator, no scheduler entry, no tunable), so it needs no
# WHOLE_ARCHIVE: a host that wants a landform calls `landform::solve`.
#
# Pure CPU arithmetic in the domain layer: `jobs` for the loops whose points do not read each
# other, `io` and `json` for the sketch and the files, `hash` for the grids' hashes. It does not
# depend on the tile set (a capability may not depend on another): it writes and reads the
# heightfield's header by its documented shape. No GPU, no ECS, no renderer.
engine_schema_library(NAME landform_schemas SCHEMAS schemas/landform.schema CAPABILITY landform)

engine_module(NAME landform LAYER domain OPTIONAL
  DEPS base containers math hash json schema io jobs landform_schemas
  SOURCES src/landform.cpp src/files.cpp)

engine_module_tests(NAME landform
  SOURCES tests/landform_tests.cpp tests/determinism_tests.cpp tests/size_table.cpp)

engine_module_bench(NAME landform
  SOURCES bench/landform_bench.cpp)
