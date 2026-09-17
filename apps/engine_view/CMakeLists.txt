# engine-view: the engine's first window (windowed mode, docs/plan/02-architecture.md §2.2).
# Draws a procedural heightfield through the mesh-shader cluster path with a reversed-Z depth
# buffer and an orbiting camera; --frames N --capture out.png is how agents and CI look at it.
engine_app(NAME engine_view OUTPUT engine-view
  SOURCES main.cpp
  DEPS base containers math time log platform window image io geometry assets gfx
  E2E_TESTS tests/view_tests.cpp tests/mesh_view_tests.cpp)
# The mesh end-to-end test also drives engine-content; apps/engine_content/CMakeLists.txt adds
# the build dependency, since that target is declared after this one.
