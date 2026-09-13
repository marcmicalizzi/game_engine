# Topology-aware job system: pinned worker pools per efficiency class, per-worker
# work-stealing deques, cache-domain-local stealing (ADR-0011, plan 11.5).
engine_module(NAME jobs LAYER core
  DEPS base memory containers platform
  SOURCES src/job_system.cpp)

engine_module_tests(NAME jobs
  SOURCES tests/job_system_tests.cpp tests/queue_tests.cpp tests/size_table.cpp)
