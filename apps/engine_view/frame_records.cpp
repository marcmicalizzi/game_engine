#include "frame_records.h"

#include <core/profiling/startup_trace.h>  // thread_cpu_ns: the frame thread's own CPU

namespace engine::view {

i64 frame_started(PendingFrame (&pending)[k_pending_frames], u64 submissions, i64 start,
                  f64 tail_pace_ms) {
  const i64 cpu = profiling::thread_cpu_ns();
  if (submissions > 0) {
    PendingFrame& before = pending[(submissions - 1) % k_pending_frames];
    if (before.submission == submissions - 1) {
      before.spans.next_start = start;
      before.spans.pace_ms = tail_pace_ms;
      before.next_cpu = cpu;
    }
  }
  return cpu;
}

void fill_record(const PendingFrame& p, scene::FrameRecord& record) {
  record.cpu_ms = p.cpu_ms;
  record.ticks = p.ticks;
  record.wait_ms = p.wait_ms;
  record.acquire_ms = p.acquire_ms;
  record.present_ms = p.present_ms;
  record.pace_ms = p.pace_ms;
  record.pace_depth = p.pace_depth;
  record.submit_ms = p.submit_ms;
  record.pose_position = p.pose.position;  // a worldpos since FrameRecord version 9
  record.pose_yaw = p.pose.yaw;
  record.pose_pitch = p.pose.pitch;
  record.pose_time = p.pose_time;
  record.terrain = p.terrain;
  record.walk = p.walk;
  record.world = p.world;
  record.wind = p.wind;
  if (p.spans.next_start != 0) {
    renderer::FrameSpans spans = p.spans;
    spans.slot_wait_ms = record.cpu.wait;
    spans.resize_ms = record.cpu.resize;
    if (p.start_cpu > 0 && p.next_cpu >= p.start_cpu)
      spans.thread_cpu_ms = static_cast<f64>(p.next_cpu - p.start_cpu) / 1.0e6;
    record.sections = renderer::frame_sections(spans);
    record.frame_ms = record.sections->wall_ms;  // its own interval (version 25, W155)
  }
}

}  // namespace engine::view
