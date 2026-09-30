# The machine-wide GPU lock in C++ (docs/subsystems/gpu_lock.md, ADR-0049): the protocol's file,
# the benchmark harness's lease, and the hold a process keeps while it has a GPU device open,
# which gfx::Device takes when the test environment's switch is on. PowerShell has the other
# implementation, tools/lib/MachineLock.psm1.
engine_module(NAME gpu_lock LAYER foundation
  DEPS base containers json platform time
  SOURCES src/gpu_lock.cpp src/device_hold.cpp)

engine_module_tests(NAME gpu_lock
  SOURCES tests/gpu_lock_tests.cpp tests/device_hold_tests.cpp)

# A process that holds the lock the way a process with a device does, for the cases that need a
# parent and a child, a crash, or an exit code. Named engine_* so tools/package-tests.ps1 carries
# it into a bundle's bin/ like every other executable a test starts.
if(TARGET engine_gpu_lock_tests)
  add_executable(engine_gpu_lock_probe tests/probe_main.cpp)
  target_link_libraries(engine_gpu_lock_probe PRIVATE engine::gpu_lock engine::platform)
  engine_apply_warnings(engine_gpu_lock_probe)
  add_dependencies(engine_gpu_lock_tests engine_gpu_lock_probe)
  target_compile_definitions(engine_gpu_lock_tests PRIVATE
    ENGINE_GPU_LOCK_PROBE_PATH="$<TARGET_FILE:engine_gpu_lock_probe>"
    ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
