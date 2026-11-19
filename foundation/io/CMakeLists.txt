# File access: status-returning native file operations, the mount-table VFS, and asynchronous
# reads on the job system's Efficiency pool, and files mapped read-only (`mapped_file.h`, T182).
engine_module(NAME io LAYER foundation
  DEPS base containers jobs time
  SOURCES src/vfs.cpp src/mapped_file.cpp src/roots.cpp)

engine_module_tests(NAME io
  SOURCES tests/vfs_tests.cpp tests/mapped_file_tests.cpp tests/roots_tests.cpp)
