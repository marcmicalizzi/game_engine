#include "long_frames.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace engine::view {

// ---- the long frames by cause (T212) ----------------------------------------------------------

namespace {

struct Part {
  const char* name;
  f64 ms;
};

// The largest of `parts`, the first on a tie.
template <usize N>
Part largest(const Part (&parts)[N]) noexcept {
  Part best = parts[0];
  for (usize i = 1; i < N; ++i)
    if (parts[i].ms > best.ms) best = parts[i];
  return best;
}

void set_part(LongFrames::Frame& f, std::string_view name, f64 ms) noexcept {
  const usize n = std::min<usize>(name.size(), sizeof(f.part) - 1);
  std::memcpy(f.part, name.data(), n);
  f.part[n] = '\0';
  f.part_ms = ms;
}

// The world's largest named part, down the update and the placements while one part holds at least
// half of the one above it (the rule `renderer::slowest_span` reads a flight's records by).
void world_part(const scene::FrameWorld& w, LongFrames::Frame& f) {
  const Part top[] = {{"update", w.update_ms}, {"ground", w.ground_ms},
                      {"props", w.props_ms},   {"ticks", w.ticks_ms - w.game_ms},
                      {"input", w.input_ms},   {"other", w.other_ms}};
  Part at = largest(top);
  char name[40];
  std::snprintf(name, sizeof(name), "%s", at.name);
  if (std::string_view(at.name) == "update") {
    const Part update[] = {{"ring", w.ring_ms},
                           {"drawn", w.drawn_ms},
                           {"placements", w.placements_ms},
                           {"restand", w.restand_ms}};
    const Part next = largest(update);
    if (next.ms >= 0.5 * at.ms && next.ms > 0.0) {
      at = next;
      std::snprintf(name, sizeof(name), "update.%s", next.name);
      if (std::string_view(next.name) == "placements") {
        const Part placements[] = {{"events", w.events_ms},     {"due", w.due_ms},
                                   {"meshes", w.meshes_ms},     {"cells", w.cells_ms},
                                   {"admit", w.admit_ms},       {"commit", w.commit_ms},
                                   {"handover", w.handover_ms}, {"tiles", w.tiles_ms}};
        const Part leaf = largest(placements);
        if (leaf.ms >= 0.5 * at.ms && leaf.ms > 0.0) {
          at = leaf;
          std::snprintf(name, sizeof(name), "update.placements.%s", leaf.name);
        }
      }
    }
  }
  set_part(f, name, at.ms);
}

}  // namespace

const char* LongFrames::cause_name(Cause cause) noexcept {
  switch (cause) {
    case gpu: return "gpu";
    case gpu_elsewhere: return "gpu_elsewhere";
    case late_wake: return "late_wake";
    case slot_wait: return "slot_wait";
    case world: return "world";
    case module: return "module";
    case present: return "present";
    case recording: return "recording";
    case other: return "other";
    case descheduled: return "descheduled";
    case k_causes: break;
  }
  return "other";
}

LongFrames::Frame LongFrames::classify(const scene::FrameRecord& r) noexcept {
  Frame f;
  f.frame = r.frame;
  if (!r.sections.has_value()) {
    // A record with no sections (`render.benchmark`'s) names nothing: its interval and "other".
    f.wall_ms = r.frame_ms;
    f.other_ms = r.frame_ms;
    f.cause = other;
    set_part(f, "unmeasured", r.frame_ms);
    return f;
  }
  const scene::FrameSections& s = *r.sections;
  f.wall_ms = s.wall_ms;
  f.slot_wait_ms = s.slot_wait_ms;
  f.module_ms = r.world.has_value() ? r.world->game_ms : 0.0;
  // The host's turn, the fold and the ticks are the world's (FrameWorld::total_ms covers them in
  // both loops); with no `world` block they are the host's turn alone.
  f.world_ms = r.world.has_value() ? std::max(0.0, r.world->total_ms - f.module_ms)
                                   : s.host_ms + s.fold_ms + s.ticks_ms;
  f.present_ms = s.acquire_ms + s.present_ms + s.pace_ms;
  f.recording_ms = s.begin_ms + s.submit_ms + s.after_ms;
  f.other_ms = std::max(0.0, s.tail_ms - s.pace_ms) + std::max(0.0, s.other_ms);
  if (s.thread_cpu_ms > 0.0 || s.off_cpu_ms > 0.0) {
    f.thread_cpu_ms = s.thread_cpu_ms;
    f.off_cpu_ms = s.off_cpu_ms;
  }
  const Part parts[] = {{"slot_wait", f.slot_wait_ms}, {"world", f.world_ms},
                        {"module", f.module_ms},       {"present", f.present_ms},
                        {"recording", f.recording_ms}, {"other", f.other_ms}};
  const Part top = largest(parts);
  const std::string_view name(top.name);
  if (name == "slot_wait") {
    if (r.gpu_frame.has_value() && r.gpu_frame->wait_split) {
      const scene::FrameGpu& g = *r.gpu_frame;
      f.wait_gpu_ms = g.wait_gpu_ms;
      f.wait_before_ms = g.wait_before_ms;
      f.wait_late_ms = g.wait_late_ms;
      const Part split[] = {{"wait_gpu", g.wait_gpu_ms},
                            {"wait_before", g.wait_before_ms},
                            {"wait_late", g.wait_late_ms}};
      const Part which = largest(split);
      const std::string_view w(which.name);
      f.cause = w == "wait_gpu" ? gpu : w == "wait_before" ? gpu_elsewhere : late_wake;
      set_part(f, which.name, which.ms);
    } else {
      f.cause = slot_wait;
      set_part(f, "slot_wait", top.ms);
    }
    return f;
  }
  if (name == "present") {
    const Part present_parts[] = {
        {"acquire", s.acquire_ms}, {"present", s.present_ms}, {"pace", s.pace_ms}};
    const Part which = largest(present_parts);
    f.cause = present;
    set_part(f, which.name, which.ms);
    return f;
  }
  if (name == "world") {
    f.cause = world;
    if (r.world.has_value()) {
      world_part(*r.world, f);
    } else {
      set_part(f, "host", f.world_ms);
    }
  } else if (name == "module") {
    f.cause = module;
    set_part(f, "game", f.module_ms);
  } else if (name == "recording") {
    f.cause = recording;
    if (!r.cpu.slowest_pass.empty() && r.cpu.slowest_pass_ms >= 0.5 * top.ms) {
      char text[40];
      std::snprintf(text, sizeof(text), "pass:%s", r.cpu.slowest_pass.c_str());
      set_part(f, text, r.cpu.slowest_pass_ms);
    } else {
      const Part rec[] = {{"begin", s.begin_ms}, {"submit", s.submit_ms}, {"after", s.after_ms}};
      const Part which = largest(rec);
      set_part(f, which.name, which.ms);
    }
  } else {
    f.cause = other;
    set_part(f, "other", top.ms);
  }
  // The thread off the processor for most of that part: its waits for a slot and for the present
  // path are its own and are taken out first.
  if (f.thread_cpu_ms >= 0.0) {
    const f64 off_beyond_waits = s.off_cpu_ms - (f.slot_wait_ms + f.present_ms);
    if (off_beyond_waits >= 0.5 * top.ms) f.cause = descheduled;
  }
  return f;
}

void LongFrames::add(const scene::FrameRecord& record) noexcept {
  if (!enabled_) return;
  ++frames_;
  const f64 wall = record.sections.has_value() ? record.sections->wall_ms : record.frame_ms;
  if (wall <= k_limit_ms[0]) return;
  const Frame f = classify(record);
  for (u32 l = 0; l < k_limits; ++l)
    if (f.wall_ms > k_limit_ms[l]) ++counts_[l][f.cause];
  // The ten longest, longest first: insert in place.
  u32 at = listed_;
  while (at > 0 && longest_[at - 1].wall_ms < f.wall_ms)
    --at;
  if (at >= k_listed) return;
  const u32 last = listed_ < k_listed ? listed_ : k_listed - 1;
  for (u32 i = last; i > at; --i)
    longest_[i] = longest_[i - 1];
  longest_[at] = f;
  if (listed_ < k_listed) ++listed_;
}

u64 LongFrames::over(u32 limit) const noexcept {
  u64 n = 0;
  for (u32 c = 0; c < k_causes; ++c)
    n += counts_[limit][c];
  return n;
}

JsonValue LongFrames::json() const {
  if (!enabled_) return JsonValue();
  JsonValue out = JsonValue::object();
  out.set("frames", frames_);
  for (u32 l = 0; l < k_limits; ++l) {
    JsonValue band = JsonValue::object();
    band.set("frames", over(l));
    JsonValue causes = JsonValue::object();
    for (u32 c = 0; c < k_causes; ++c)
      causes.set(cause_name(static_cast<Cause>(c)), counts_[l][c]);
    band.set("by_cause", std::move(causes));
    char key[16];
    std::snprintf(key, sizeof(key), "over_%u", static_cast<u32>(k_limit_ms[l]));
    out.set(key, std::move(band));
  }
  JsonValue list = JsonValue::array();
  for (u32 i = 0; i < listed_; ++i) {
    const Frame& f = longest_[i];
    JsonValue row = JsonValue::object();
    row.set("frame", f.frame);
    row.set("wall_ms", f.wall_ms);
    row.set("cause", cause_name(f.cause));
    row.set("part", std::string_view(f.part));
    row.set("part_ms", f.part_ms);
    row.set("slot_wait_ms", f.slot_wait_ms);
    row.set("wait_gpu_ms", f.wait_gpu_ms);
    row.set("wait_before_ms", f.wait_before_ms);
    row.set("wait_late_ms", f.wait_late_ms);
    row.set("world_ms", f.world_ms);
    row.set("module_ms", f.module_ms);
    row.set("present_ms", f.present_ms);
    row.set("recording_ms", f.recording_ms);
    row.set("other_ms", f.other_ms);
    row.set("thread_cpu_ms", f.thread_cpu_ms >= 0.0 ? JsonValue(f.thread_cpu_ms) : JsonValue());
    row.set("off_cpu_ms", f.thread_cpu_ms >= 0.0 ? JsonValue(f.off_cpu_ms) : JsonValue());
    list.push_back(std::move(row));
  }
  out.set("longest", std::move(list));
  return out;
}

}  // namespace engine::view
