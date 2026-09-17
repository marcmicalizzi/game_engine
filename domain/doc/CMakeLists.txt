# The authoring document model (ADR-0002): layers, composition, commands, transactions,
# structural diff and three-way merge, canonical layer files, and schema validation.
engine_module(NAME doc LAYER domain
  DEPS base containers ids json schema time schemas io profiling
  SOURCES src/document.cpp src/document_store.cpp src/merge.cpp)

engine_module_tests(NAME doc
  SOURCES tests/document_tests.cpp tests/document_store_tests.cpp tests/merge_tests.cpp
          tests/size_table.cpp)
