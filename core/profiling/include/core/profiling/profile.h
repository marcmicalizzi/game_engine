#pragma once

// Profiling hooks (docs/plan/09-testing-profiling.md §9.3, ADR-0012). With ENGINE_PROFILING
// (CMake option ENGINE_TRACY) the macros are Tracy zones, frame marks, plots, and messages;
// without it they expand to nothing, so instrumentation is free to leave in place.
//
//     void Renderer::cull(const View& view) {
//       ENGINE_PROFILE_ZONE();                       // named after the function
//       ...
//       ENGINE_PROFILE_PLOT("culled instances", count);
//     }
//
// Rules: one ENGINE_PROFILE_ZONE per scope (it declares a local); a zone per system and per
// pass, not per element; plots for counts the frame budget tracks (11 §11.8). Worker threads
// name themselves with ENGINE_PROFILE_THREAD so captures read well.
//
// **A zone is also the frame capture's, in every build** (frame_capture.h; profiling.md, "The
// frame capture"): while a host has the capture on, every zone scope records its interval and
// every named thread its name, Tracy or not, which is what `profile.*` and a frame report read.
// Off, that is one relaxed atomic load per zone. Frame marks for the capture carry the frame
// loop's own index, so a frame loop calls `capture_frame(index)`; ENGINE_PROFILE_FRAME stays
// Tracy's.

#include <core/base/macros.h>
#include <core/profiling/frame_capture.h>

#ifndef ENGINE_PROFILING
#define ENGINE_PROFILING 0
#endif

#define ENGINE_PROFILE_CAPTURE_ZONE_(name) \
  ::engine::profiling::CaptureZone ENGINE_CONCAT(engine_capture_zone_, __LINE__)(name)

#if ENGINE_PROFILING

#include <tracy/Tracy.hpp>

#define ENGINE_PROFILE_ZONE() \
  ZoneScoped;                 \
  ENGINE_PROFILE_CAPTURE_ZONE_(__func__)
#define ENGINE_PROFILE_ZONE_NAMED(name) \
  ZoneScopedN(name);                    \
  ENGINE_PROFILE_CAPTURE_ZONE_(name)
#define ENGINE_PROFILE_ZONE_TEXT(text, size) ZoneText(text, size)
#define ENGINE_PROFILE_FRAME() FrameMark
#define ENGINE_PROFILE_FRAME_NAMED(name) FrameMarkNamed(name)
#define ENGINE_PROFILE_THREAD(name) \
  (::tracy::SetThreadName(name), ::engine::profiling::capture_name_thread(name))
#define ENGINE_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define ENGINE_PROFILE_MESSAGE(text, size) TracyMessage(text, size)
#define ENGINE_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define ENGINE_PROFILE_FREE(ptr) TracyFree(ptr)

#else

#define ENGINE_PROFILE_ZONE() ENGINE_PROFILE_CAPTURE_ZONE_(__func__)
#define ENGINE_PROFILE_ZONE_NAMED(name) ENGINE_PROFILE_CAPTURE_ZONE_(name)
#define ENGINE_PROFILE_ZONE_TEXT(text, size) ((void)(text), (void)(size))
#define ENGINE_PROFILE_FRAME() ((void)0)
#define ENGINE_PROFILE_FRAME_NAMED(name) ((void)(name))
#define ENGINE_PROFILE_THREAD(name) ::engine::profiling::capture_name_thread(name)
#define ENGINE_PROFILE_PLOT(name, value) ((void)(name), (void)(value))
#define ENGINE_PROFILE_MESSAGE(text, size) ((void)(text), (void)(size))
#define ENGINE_PROFILE_ALLOC(ptr, size) ((void)(ptr), (void)(size))
#define ENGINE_PROFILE_FREE(ptr) ((void)(ptr))

#endif

namespace engine::profiling {

constexpr bool enabled() noexcept { return ENGINE_PROFILING != 0; }

// True while a profiler client is connected; always false when profiling is compiled out.
inline bool connected() noexcept {
#if ENGINE_PROFILING
  return TracyIsConnected;
#else
  return false;
#endif
}

}  // namespace engine::profiling
