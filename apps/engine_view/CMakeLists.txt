# engine-view: the engine's first window (windowed mode, docs/plan/02-architecture.md §2.2).
# Draws a procedural heightfield through the mesh-shader cluster path with a reversed-Z depth
# buffer and an orbiting camera; --frames N --capture out.png is how agents and CI look at it.
engine_app(NAME engine_view OUTPUT engine-view
  SOURCES main.cpp
  DEPS base containers math time log platform window image geometry gfx
  E2E_TESTS tests/view_tests.cpp)
