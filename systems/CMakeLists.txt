# Layer L3: systems. Renderer, streaming, simulation, destruction, deformation, ui, animation,
# and the capabilities a game adds (ADR-0027, docs/plan/02-architecture.md §2.8). Declaration
# order matters: a module's dependencies are declared before it, so lower layers first and, in
# this file, dependencies first. `tools/new-capability.ps1` appends to the end of this list,
# which is always correct for a new capability because its dependencies already exist.
add_subdirectory(renderer)
add_subdirectory(animation)
