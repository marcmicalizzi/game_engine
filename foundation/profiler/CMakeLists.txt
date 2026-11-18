# The profiling service (roadmap T44; docs/subsystems/profiler.md): core/profiling's frame capture
# read as a frame report (per pass and per zone, GPU and CPU milliseconds, median and tail over a
# window) and written as a trace a person opens in Perfetto. CPU only; it names no renderer type,
# so whoever measures a time (the renderer's GPU timer, a frame loop's spans) records it into the
# capture and this module reads it back.
engine_module(NAME profiler LAYER foundation
  DEPS base containers json profiling
  SOURCES src/frame_report.cpp)

engine_module_tests(NAME profiler
  SOURCES tests/frame_report_tests.cpp)
