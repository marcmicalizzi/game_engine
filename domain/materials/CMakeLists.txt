# materials (docs/subsystems/materials.md): a generated mesh's tiled PBR sets — the description a
# recipe's material block and a material pack's role resolve to, its check, the pack's manifest,
# and the step that writes a set into a `.clusters` container's materials. Shared by the tree
# solver (domain/flora) and the rock solver (domain/rocks) so the two reach the renderer by one
# code path.
#
# Not a capability: the mesh-generator registry in `assets`, which is not one either, carries a
# pack in its inputs, and a module that is not a capability may not depend on one (ADR-0027). It
# sits below `assets` for the same reason.

engine_module(NAME materials LAYER domain
  DEPS base containers hash json log io geometry
  SOURCES src/materials.cpp)

engine_module_tests(NAME materials
  SOURCES tests/materials_tests.cpp)
