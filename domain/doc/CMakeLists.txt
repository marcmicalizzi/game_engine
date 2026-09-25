# The authoring document model (ADR-0002): layers, composition with a composed index, commands,
# transactions, structural diff and three-way merge, canonical layer files, tile-partitioned
# layer files, and schema validation.
engine_module(NAME doc LAYER domain
  DEPS base containers ids json schema time schemas io profiling
  SOURCES src/document.cpp src/document_store.cpp src/merge.cpp src/partition.cpp)

# Object types the partition tests place in a world: a type with a `position`, one with a
# `transform`, and one with neither, which is how a type opts into (or out of) a tile grid.
engine_schema_library(NAME doc_test_types
  SCHEMAS tests/doc_test_types.schema)

engine_module_tests(NAME doc
  SOURCES tests/document_tests.cpp tests/document_store_tests.cpp tests/merge_tests.cpp
          tests/index_tests.cpp tests/partition_tests.cpp tests/change_feed_tests.cpp
          tests/size_table.cpp
  DEPS doc_test_types)

engine_module_bench(NAME doc SOURCES bench/doc_bench.cpp)
