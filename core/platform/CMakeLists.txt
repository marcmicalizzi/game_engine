# OS and CPU facts the rest of the engine builds on: processor topology (cores, SMT, cache
# domains, NUMA, efficiency classes), CPUID feature detection, the ADR-0031 startup check, no
# dialog and one line on stderr for every fatal fault, and thread pinning/naming.
engine_module(NAME platform LAYER core
  DEPS base memory containers
  SOURCES src/topology.cpp src/cpu_features.cpp src/cpu_baseline.cpp src/thread.cpp
          src/process.cpp src/error_dialogs.cpp src/fault_report.cpp)

# This module is compiled at the floor baseline, whatever the rest of the tree is built for
# (ADR-0031). It is the module that decides whether the rest of the binary may run: a
# `cpu_baseline.cpp` compiled with /arch:AVX2 would be a VEX-encoded instruction executed before
# the check that exists to say the CPU has none. Nothing here is a hot path — CPUID, the
# topology walk and thread pinning happen once at start-up — so the baseline costs nothing to
# give up, and giving it up for the whole module rather than for one file makes the rule one
# sentence instead of a list somebody has to keep.
engine_strip_cpu_baseline(engine_platform)

engine_module_tests(NAME platform
  SOURCES tests/topology_tests.cpp tests/cpu_features_tests.cpp tests/cpu_baseline_tests.cpp
          tests/process_tests.cpp tests/error_dialogs_tests.cpp tests/fault_report_tests.cpp
          tests/size_table.cpp)

# A process that fails on request — an access violation, a stack overflow, abort() and the rest,
# one argument per kind — for the test that reads the line its death prints
# (docs/subsystems/platform.md, "Every fatal fault prints one line"). Named engine_* so
# tools/package-tests.ps1 carries it into a bundle's bin/ like every other executable a test starts.
if(TARGET engine_platform_tests)
  add_executable(engine_platform_fault_probe tests/fault_probe_main.cpp)
  target_link_libraries(engine_platform_fault_probe PRIVATE engine::platform)
  engine_apply_warnings(engine_platform_fault_probe)
  add_dependencies(engine_platform_tests engine_platform_fault_probe)
  target_compile_definitions(engine_platform_tests PRIVATE
    ENGINE_PLATFORM_FAULT_PROBE_PATH="$<TARGET_FILE:engine_platform_fault_probe>")
endif()
