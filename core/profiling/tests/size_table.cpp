// Size table for core/profiling (ADR-0019). The frame capture keeps one `CaptureEvent` per zone,
// frame mark and value of every frame it records, in a buffer of `k_capture_default_capacity` of
// them (profiling.md, "The frame capture"); the startup trace's records are kept once a process
// start and written out as JSON, so they are not hot.
#include <core/base/size_table.h>
#include <core/profiling/frame_capture.h>

using namespace engine;

ENGINE_EXPECT_SIZE(48, 8, profiling::CaptureEvent);
ENGINE_EXPECT_SIZE(24, 8, profiling::CaptureZone);
