# engine-host: the headless engine server (ADR-0001). JSON-RPC over stdio, the document methods
# from domain/protocol, and `render.*` over systems/renderer with no window in the process
# (docs/subsystems/apps.md, renderer.md). It depends on `renderer` the way any app depends on a
# system: apps are L4 and systems are L3.
engine_app(NAME engine_host OUTPUT engine-host
  SOURCES main.cpp render_methods.cpp
  DEPS base containers json log io image tunables protocol renderer)
