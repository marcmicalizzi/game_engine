# engine-host: the headless engine server (ADR-0001). Phase 0: sim mode, JSON-RPC over stdio.
engine_app(NAME engine_host OUTPUT engine-host
  SOURCES main.cpp
  DEPS base containers json log io tunables protocol)
