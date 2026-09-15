# File access: status-returning native file operations, the mount-table VFS, and asynchronous
# reads on the job system's Efficiency pool.
engine_module(NAME io LAYER foundation
  DEPS base containers jobs time
  SOURCES src/vfs.cpp)

engine_module_tests(NAME io
  SOURCES tests/vfs_tests.cpp)
