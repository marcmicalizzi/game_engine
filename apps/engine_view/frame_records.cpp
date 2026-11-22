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
  record.sections = pending_sections(p, record.cpu.wait, record.cpu.resize);
  // Its own interval (version 25, W155).
  if (record.sections.has_value()) record.frame_ms = record.sections->wall_ms;
}

std::optional<scene::FrameSections> pending_sections(const PendingFrame& p, f64 slot_wait_ms,
                                                     f64 resize_ms) {
  if (p.spans.next_start == 0) return std::nullopt;
  renderer::FrameSpans spans = p.spans;
  spans.slot_wait_ms = slot_wait_ms;
  spans.resize_ms = resize_ms;
  if (p.start_cpu > 0 && p.next_cpu >= p.start_cpu)
    spans.thread_cpu_ms = static_cast<f64>(p.next_cpu - p.start_cpu) / 1.0e6;
  return renderer::frame_sections(spans);
}

void classify_folded(const renderer::FrameStats& stats, const PendingFrame& p, LongFrames& into) {
  if (!into.enabled()) return;
  const std::optional<scene::FrameSections> sections =
      pending_sections(p, stats.cpu.wait_ms, stats.cpu.resize_ms);
  LongFrames::Input in;
  in.frame = p.frame;
  // As the record's: its own interval, and nothing for a frame no later frame ended.
  in.frame_ms = sections.has_value() ? sections->wall_ms : 0.0;
  in.sections = sections.has_value() ? &*sections : nullptr;
  in.world = p.world.has_value() ? &*p.world : nullptr;
  // `renderer::frame_record`'s `gpu_frame`, which carries the split only on a valid GPU frame.
  if (stats.gpu.valid && stats.cpu.wait_split) {
    in.wait_split = true;
    in.wait_gpu_ms = stats.cpu.wait_gpu_ms;
    in.wait_before_ms = stats.cpu.wait_before_ms;
    in.wait_late_ms = stats.cpu.wait_late_ms;
  }
  if (stats.cpu.slowest_pass != nullptr) in.slowest_pass = stats.cpu.slowest_pass;
  in.slowest_pass_ms = stats.cpu.slowest_pass_ms;
  into.add(in);
}

}  // namespace engine::view
