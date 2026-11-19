# The authoring document model (ADR-0002): layers, composition with a composed index, commands,
# transactions, structural diff and three-way merge, canonical layer files, tile-partitioned
# layer files, and schema validation; content-addressed blobs beside a document (blob_store.h) and
# its checkpoints as commits of its own git repository, through the git on PATH (checkpoint.h).
engine_module(NAME doc LAYER domain
  DEPS base containers hash ids json schema time schemas io profiling platform
  SOURCES src/document.cpp src/document_store.cpp src/merge.cpp src/partition.cpp
          src/blob_store.cpp src/checkpoint.cpp)

# Object types the partition tests place in a world: a type with a `position`, one with a
# `transform`, and one with neither, which is how a type opts into (or out of) a tile grid.
engine_schema_library(NAME doc_test_types
  SCHEMAS tests/doc_test_types.schema)

engine_module_tests(NAME doc
  SOURCES tests/document_tests.cpp tests/document_store_tests.cpp tests/merge_tests.cpp
          tests/index_tests.cpp tests/partition_tests.cpp tests/change_feed_tests.cpp
          tests/save_tests.cpp tests/blob_tests.cpp tests/checkpoint_tests.cpp tests/size_table.cpp
          tests/rename_tests.cpp tests/hostile_store_tests.cpp tests/json_text_property_tests.cpp
  DEPS doc_test_types)
# The migration corpus's renamed document (content/migration-corpus/documents/renamed, roadmap D28).
target_compile_definitions(engine_doc_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

engine_module_bench(NAME doc SOURCES bench/doc_bench.cpp)
