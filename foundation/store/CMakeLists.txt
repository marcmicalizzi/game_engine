# store: the persistent world state of docs/plan/03-data-model.md §3.5 (ADR-0003) on SQLite —
# a Status-returning connection with a prepared-statement cache, RAII transactions and
# user_version migrations, and the event log, projections, and per-tile snapshots on top of it.
#
# OPTIONAL (ADR-0027): ENGINE_WITH_STORE=OFF, and the minimal build, take the module, its tests,
# its bench, and the SQLite fetch with it. The dependency pin lives in cmake/EngineStore.cmake.
engine_module(NAME store LAYER foundation
  OPTIONAL CAPABILITY store
  DEPS base containers ids log
  EXTERNAL_DEPS engine::sqlite3
  SOURCES src/database.cpp src/event_log.cpp)

engine_module_tests(NAME store
  SOURCES tests/database_tests.cpp tests/event_log_tests.cpp tests/size_table.cpp)

# The E6 half that measures the store: 10^6 LOD3 projections, batched inserts, point and range
# reads, hourly summarization, snapshots, WAL against a rollback journal, and event-log append
# rate (docs/experiments/e6-ecs-store.md).
engine_module_bench(NAME store
  SOURCES bench/store_bench.cpp
  DEPS platform time)
