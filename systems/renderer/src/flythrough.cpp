#include <core/time/time.h>
#include <systems/renderer/flythrough.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace engine::renderer {

namespace {

// The median of a handful of repeats: the middle one, or the mean of the middle two.
f64 median_of(Vector<f64>& values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const u32 n = values.size();
  return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

f64 rank(const Vector<f64>& sorted, f64 p) {
  if (sorted.empty()) return 0.0;
  const f64 position = p * static_cast<f64>(sorted.size() - 1);
  const u32 lo = static_cast<u32>(std::floor(position));
  const u32 hi = std::min<u32>(lo + 1, sorted.size() - 1);
  const f64 t = position - static_cast<f64>(lo);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * t;
}

}  // namespace

scene::FrameRecord frame_record(const FrameStats& stats, u32 repeat, u32 frame, f64 time) {
  scene::FrameRecord out;
  out.repeat = repeat;
  out.frame = frame;
  out.time = time;
  out.visible_hw = stats.visible_hw;
  out.visible_pass2 = stats.visible_pass2;
  out.visible_sw = stats.visible_sw;
  out.visible_pairs = stats.visible_pairs();
  out.shadow_casters = stats.shadow_casters;
  out.shadow_pairs = stats.shadow_pairs;
  out.gpu_ms.cull = stats.gpu_cull;
  out.gpu_ms.hw = stats.gpu_hw;
  out.gpu_ms.sw = stats.gpu_sw;
  out.gpu_ms.hiz = stats.gpu_hiz;
  out.gpu_ms.resolve = stats.gpu_resolve;
  out.gpu_ms.rt = stats.gpu_rt;
  out.gpu_ms.clas = stats.gpu_clas;
  out.gpu_ms.blas = stats.gpu_blas;
  out.gpu_ms.deform = stats.gpu_deform;
  out.gpu_ms.deform_alloc = stats.gpu_deform_alloc;
  out.gpu_ms.trace = stats.gpu_trace;
  out.gpu_ms.shadow = stats.gpu_shadow;
  out.gpu_ms.shadow_cull = stats.gpu_shadow_cull;
  out.gpu_ms.total = stats.gpu_total;
  out.gpu_ms.tables = stats.gpu_tables;
  out.gpu_ms.sky = stats.gpu_sky;
  out.uploads = stats.uploads;
  out.upload_bytes = stats.upload_bytes;
  out.evictions = stats.evictions;
  out.requests = stats.requests;
  out.pages_resident = stats.pages_resident;
  out.pending = stats.pending;
  out.loads_in_flight = stats.loads_in_flight;
  out.pool_pages = stats.pool_pages;
  out.rt_built = stats.rt_built;
  out.rt_wanted = stats.rt_wanted;
  out.rt_capacity = stats.rt_capacity;
  out.instances = stats.instances;
  out.pairs = stats.pairs;
  out.table_slots = stats.table_slots;
  out.table_pairs = stats.table_pairs;
  out.hole_pairs = stats.hole_pairs;
  out.cpu.wait = stats.cpu.wait_ms;
  out.cpu.begin = stats.cpu.begin_ms;
  out.cpu.tables = stats.cpu.tables_ms;
  out.cpu.terrain = stats.cpu.terrain_ms;
  out.cpu.passes = stats.cpu.passes_ms;
  out.cpu.compile = stats.cpu.compile_ms;
  out.cpu.record = stats.cpu.record_ms;
  out.cpu.submit = stats.cpu.submit_ms;
  out.cpu.slowest_pass = stats.cpu.slowest_pass != nullptr ? stats.cpu.slowest_pass : "";
  out.cpu.slowest_pass_ms = stats.cpu.slowest_pass_ms;
  return out;
}

void summarize_rt(const RtStats& stats, scene::FlythroughRt& out) {
  out.capacity = stats.capacity;
  out.peak_capacity = stats.peak_capacity;
  out.limit = stats.limit;
  out.union_clusters = stats.union_clusters;
  out.bytes = stats.bytes;
  out.peak_bytes = stats.peak_bytes;
  out.grows = stats.grows;
  out.shrinks = stats.shrinks;
  out.overflow_frames = stats.overflow_frames;
  out.dropped_instances = stats.dropped_instances;
  out.dropped_caster_instances = stats.dropped_caster_instances;
}

void summarize_textures(const GpuScene& scene, scene::FlythroughTextures& out) {
  out.built = scene.textures_built();
  out.decoded = scene.textures_decoded();
  out.shared = scene.textures_shared();
  out.bytes = scene.texture_bytes();
  out.bytes_saved = scene.texture_bytes_saved();
  out.sharing = scene.share_textures();
}

bool fly_camera_path(SceneRenderer& renderer, const CameraPath& path, const FlightOptions& options,
                     Flight& out, std::string* error) {
  out = Flight{};
  const u32 frames = options.frames != 0 ? options.frames : path.frame_count();
  const u32 repeats = options.repeats > 0 ? options.repeats : 1u;
  out.frames = frames;
  // What each submission was, by its place since the reset — the index `FrameStats::submission`
  // names — so a fold can be told from a warm-up, a path frame and the drain.
  // The CPU's milliseconds and the wall time since the previous submission are taken here, where
  // the frame is submitted, and carried until its GPU numbers fold in two frames later.
  struct Submission {
    u32 repeat = 0;
    u32 frame = 0;
    bool recorded = false;
    f64 cpu_ms = 0.0;
    f64 frame_ms = 0.0;
  };
  Vector<Submission> submissions;
  submissions.reserve((options.warmup + frames) * repeats + options.frames_in_flight);
  out.records.reserve(frames * repeats);
  renderer.reset_stats();
  u64 folded = renderer.stats().folded;
  i64 previous_start = 0;
  auto submit = [&](u32 f, bool recorded, u32 repeat, u32 warmup) {
    const i64 start = time::monotonic_ns();
    // The host's turn, between two frames: a streamed world updates its ring from this camera and
    // hands the renderer its instances (FlightOptions::before_frame). It is in the frame's wall
    // time and not in its CPU milliseconds, which are the renderer's own; the hook measures itself.
    if (options.before_frame != nullptr) {
      FlightStep step;
      step.repeat = repeat;
      step.frame = f;
      step.warmup = warmup;
      step.recorded = recorded;
      step.camera = camera_path_frame(path, f, frames);
      if (!options.before_frame(options.before_frame_context, step, error)) return false;
    }
    renderer.begin_frame();
    const Stats& stats = renderer.stats();
    if (stats.folded != folded) {
      folded = stats.folded;
      if (stats.last.submission < submissions.size()) {
        const Submission& done = submissions[static_cast<u32>(stats.last.submission)];
        if (done.recorded) {
          scene::FrameRecord record =
              frame_record(stats.last, done.repeat, done.frame,
                           camera_path_frame_time(path, done.frame, frames));
          record.cpu_ms = done.cpu_ms;
          record.frame_ms = done.frame_ms;
          out.records.push_back(std::move(record));
        }
      }
    }
    // The wait for the slot is the GPU's time, not the CPU's: the CPU's starts once it is free.
    const i64 ready = time::monotonic_ns();
    FrameDesc frame;
    frame.camera = camera_path_frame(path, f, frames);
    frame.frame_index = f;
    frame.sun_time_s = options.sun_time_s + options.sun_rate * static_cast<f64>(f) / 60.0;
    submissions.push_back(Submission{repeat, f, recorded});
    const bool ok = renderer.submit_frame(frame, error) != 0;
    Submission& mine = submissions[submissions.size() - 1];
    mine.cpu_ms = static_cast<f64>(time::monotonic_ns() - ready) / 1.0e6;
    mine.frame_ms = previous_start != 0 ? static_cast<f64>(start - previous_start) / 1.0e6 : 0.0;
    previous_start = start;
    return ok;
  };
  for (u32 r = 0; r < repeats; ++r) {
    const i64 warm_from = time::monotonic_ns();
    const i64 warm_ns = static_cast<i64>(options.warmup_seconds * 1.0e9);
    for (u32 w = 0; w < options.warmup || time::monotonic_ns() - warm_from < warm_ns; ++w) {
      if (!submit(0, false, r, w)) return false;
    }
    const i64 started = time::monotonic_ns();
    for (u32 f = 0; f < frames; ++f) {
      if (!submit(f, true, r, ~0u)) return false;
    }
    out.seconds += static_cast<f64>(time::monotonic_ns() - started) / 1.0e9;
  }
  // A frame's numbers fold in when its slot comes around, so the last path frames need that many
  // submissions behind them; these are drawn and not recorded.
  for (u32 d = 0; d < options.frames_in_flight; ++d) {
    if (!submit(frames - 1, false, repeats - 1, ~0u)) return false;
  }
  renderer.wait_idle();
  out.submitted = submissions.size();
  return true;
}

scene::Percentiles percentiles(std::span<const f64> values) {
  scene::Percentiles out;
  if (values.empty()) return out;
  Vector<f64> sorted(values.begin(), values.end());
  std::sort(sorted.begin(), sorted.end());
  f64 sum = 0.0;
  for (const f64 v : sorted)
    sum += v;
  out.median = rank(sorted, 0.50);
  out.p95 = rank(sorted, 0.95);
  out.p99 = rank(sorted, 0.99);
  out.max = sorted[sorted.size() - 1];
  out.mean = sum / static_cast<f64>(sorted.size());
  return out;
}

u32 marker_frame(const CameraPath& path, u32 marker, u32 frames) noexcept {
  const u32 native = path.frame_count();
  if (frames == native || native <= 1 || frames <= 1) return std::min(marker, frames - 1);
  const f64 t = static_cast<f64>(marker) / static_cast<f64>(native - 1);
  return static_cast<u32>(std::llround(t * static_cast<f64>(frames - 1)));
}

void summarize_frames(std::span<const scene::FrameRecord> records, u32 frames, u32 repeats,
                      const CameraPath& path, scene::FlythroughSummary& out) {
  if (frames == 0 || repeats == 0) return;
  // One slot per (frame, repeat), so the order the records came in does not matter.
  const u32 slots = frames * repeats;
  Vector<const scene::FrameRecord*> slot(slots, nullptr);
  for (const scene::FrameRecord& r : records) {
    if (r.frame < frames && r.repeat < repeats) slot[r.frame * repeats + r.repeat] = &r;
  }
  // A frame's figure is the median of its repeats; the path's are percentiles over frames.
  enum Pass : u32 {
    cull,
    hw,
    sw,
    hiz,
    resolve,
    rt,
    deform,
    trace,
    shadow,
    total,
    pairs,
    casters,
    clas,
    blas,
    rt_built,
    rt_wanted,
    shadow_pairs,
    cpu,
    wall,
    tables,
    sky,
    cpu_wait,
    cpu_begin,
    cpu_tables,
    cpu_terrain,
    cpu_passes,
    cpu_compile,
    cpu_record,
    cpu_submit,
    passes
  };
  Vector<f64> per_frame[passes];
  Vector<f64> repeats_of;
  out.deterministic = true;
  out.mismatched_frames = 0;
  out.uploads = 0;
  out.upload_bytes = 0;
  out.evictions = 0;
  out.ticks = 0;
  for (const scene::FrameRecord* r : slot) {
    if (r == nullptr) continue;
    out.uploads += r->uploads;
    out.upload_bytes += r->upload_bytes;
    out.evictions += r->evictions;
    out.ticks += r->ticks;
  }
  for (u32 f = 0; f < frames; ++f) {
    const scene::FrameRecord* first = nullptr;
    bool same = true;
    for (u32 k = 0; k < repeats; ++k) {
      const scene::FrameRecord* r = slot[f * repeats + k];
      if (r == nullptr) continue;
      if (first == nullptr) {
        first = r;
      } else if (r->visible_pairs != first->visible_pairs) {
        same = false;
      }
    }
    if (first == nullptr) continue;
    if (!same) {
      out.deterministic = false;
      ++out.mismatched_frames;
    }
    for (u32 p = 0; p < passes; ++p) {
      repeats_of.clear();
      for (u32 k = 0; k < repeats; ++k) {
        const scene::FrameRecord* r = slot[f * repeats + k];
        if (r == nullptr) continue;
        const scene::PassMs& ms = r->gpu_ms;
        const f64 values[passes] = {ms.cull,
                                    ms.hw,
                                    ms.sw,
                                    ms.hiz,
                                    ms.resolve,
                                    ms.rt,
                                    ms.deform,
                                    ms.trace,
                                    ms.shadow,
                                    ms.total,
                                    static_cast<f64>(r->visible_pairs),
                                    static_cast<f64>(r->shadow_casters),
                                    ms.clas,
                                    ms.blas,
                                    static_cast<f64>(r->rt_built),
                                    static_cast<f64>(r->rt_wanted),
                                    static_cast<f64>(r->shadow_pairs),
                                    r->cpu_ms,
                                    r->frame_ms,
                                    ms.tables,
                                    ms.sky,
                                    r->cpu.wait,
                                    r->cpu.begin,
                                    r->cpu.tables,
                                    r->cpu.terrain,
                                    r->cpu.passes,
                                    r->cpu.compile,
                                    r->cpu.record,
                                    r->cpu.submit};
        repeats_of.push_back(values[p]);
      }
      per_frame[p].push_back(median_of(repeats_of));
    }
  }
  auto over = [&](u32 p) {
    return percentiles(std::span<const f64>(per_frame[p].data(), per_frame[p].size()));
  };
  out.gpu_ms.cull = over(cull);
  out.gpu_ms.hw = over(hw);
  out.gpu_ms.sw = over(sw);
  out.gpu_ms.hiz = over(hiz);
  out.gpu_ms.resolve = over(resolve);
  out.gpu_ms.rt = over(rt);
  out.gpu_ms.deform = over(deform);
  out.gpu_ms.trace = over(trace);
  out.gpu_ms.shadow = over(shadow);
  out.gpu_ms.total = over(total);
  out.visible_pairs = over(pairs);
  out.shadow_casters = over(casters);
  out.gpu_ms.clas = over(clas);
  out.gpu_ms.blas = over(blas);
  out.rt.built = over(rt_built);
  out.rt.wanted = over(rt_wanted);
  out.shadow_pairs = over(shadow_pairs);
  out.cpu_ms = over(cpu);
  out.frame_ms = over(wall);
  out.gpu_ms.tables = over(tables);
  out.gpu_ms.sky = over(sky);
  out.cpu.wait = over(cpu_wait);
  out.cpu.begin = over(cpu_begin);
  out.cpu.tables = over(cpu_tables);
  out.cpu.terrain = over(cpu_terrain);
  out.cpu.passes = over(cpu_passes);
  out.cpu.compile = over(cpu_compile);
  out.cpu.record = over(cpu_record);
  out.cpu.submit = over(cpu_submit);
  out.cpu.over_8ms = 0;
  out.cpu.over_16ms = 0;
  for (const f64 ms : per_frame[cpu]) {
    if (ms > 8.0) ++out.cpu.over_8ms;
    if (ms > 16.0) ++out.cpu.over_16ms;
  }

  out.markers.clear();
  for (const CameraPathMarker& marker : path.markers) {
    scene::MarkerRecord m;
    m.frame = marker_frame(path, marker.frame, frames);
    m.name = marker.name;
    if (m.frame < per_frame[total].size()) {
      m.total_ms = per_frame[total][m.frame];
      m.visible_pairs = static_cast<u32>(per_frame[pairs][m.frame]);
    }
    for (u32 k = 0; k < repeats; ++k) {
      const scene::FrameRecord* r = slot[m.frame * repeats + k];
      if (r != nullptr && !r->meshes.empty()) {
        m.meshes = r->meshes;
        m.pixels = r->pixels;
        break;
      }
    }
    out.markers.push_back(std::move(m));
  }
}

VisibleCensus::~VisibleCensus() { destroy(); }

bool VisibleCensus::create(const gfx::Device& device, const GpuScene& scene, std::string* error) {
  destroy();
  device_ = &device;
  // Host-visible and as long as the whole list, so a frame's runs land in it with one submission;
  // it is created once, since a census reads every frame of a path.
  return gfx::create_buffer(device, scene.visible.size > 0 ? scene.visible.size : 8,
                            gfx::BufferUsage::TransferDst, true, staging_, error);
}

void VisibleCensus::destroy() noexcept {
  if (device_ != nullptr && staging_.buffer.valid()) gfx::destroy_buffer(*device_, staging_);
  staging_ = gfx::BufferResource{};
  device_ = nullptr;
}

bool VisibleCensus::count(const SceneData& data, const GpuScene& scene, const Stats& stats,
                          Vector<u32>& levels, Vector<u32>& meshes, std::string* error) {
  levels.assign(data.lod.level_cluster_counts.size(), 0u);
  meshes.assign(data.parts.size(), 0u);
  if (device_ == nullptr || !staging_.buffer.valid()) {
    if (error != nullptr) *error = "the census was not created";
    return false;
  }
  // A scene whose instances come and go may have made its visible list again, longer, since the
  // census was created (GpuScene::set_dynamic_instances): follow it.
  if (scene.visible.size > staging_.size) {
    gfx::destroy_buffer(*device_, staging_);
    if (!gfx::create_buffer(*device_, scene.visible.size, gfx::BufferUsage::TransferDst, true,
                            staging_, error)) {
      return false;
    }
  }
  // Every view's three runs, each only as long as the count its argument block recorded: the rest
  // of a run is whatever an earlier frame left there.
  gfx::BufferCopy regions[k_max_views * k_visible_runs];
  u32 region_count = 0;
  u64 cursor = 0;
  for (u32 v = 0; v < scene.view_count() && v < k_max_views; ++v) {
    const ViewStats& view = stats.views[v];
    const u32 counts[k_visible_runs] = {view.visible_hw, view.visible_pass2, view.visible_sw};
    for (u32 run = 0; run < k_visible_runs; ++run) {
      const u64 bytes = u64{counts[run]} * 8;
      if (bytes == 0) continue;
      const u64 source = u64{scene.visible_base(v, run)} * 8;
      if (source + bytes > scene.visible.size || cursor + bytes > staging_.size) {
        if (error != nullptr) *error = "a visible run is longer than the visible list";
        return false;
      }
      regions[region_count++] = gfx::BufferCopy{source, cursor, bytes};
      cursor += bytes;
    }
  }
  if (region_count > 0) {
    const gfx::BufferHandle from = scene.visible.buffer;
    const gfx::BufferHandle to = staging_.buffer;
    const bool ok = gfx::submit_immediate(
        *device_,
        [&](gfx::CommandList commands) {
          commands.copy_buffer(from, to, std::span<const gfx::BufferCopy>(regions, region_count));
        },
        error);
    if (!ok) return false;
  }
  const auto* entries = static_cast<const u32*>(staging_.mapped);
  const u64 entry_count = cursor / 8;
  const u32 cluster_count = data.lod.lod.size();
  for (u64 e = 0; e < entry_count; ++e) {
    const u32 instance = entries[e * 2 + 0];
    const u32 cluster = entries[e * 2 + 1];
    if (cluster < cluster_count) {
      const u32 level = data.lod.lod[cluster].level;
      if (level >= levels.size()) levels.resize(level + 1, 0u);
      ++levels[level];
    }
    // Through the GPU scene's table, which holds a streamed world's tail of instances as well as
    // the load's (`SceneData::instances` is only the latter).
    const u32 mesh = scene.instance_mesh(instance);
    if (mesh < meshes.size()) ++meshes[mesh];
  }
  return true;
}

}  // namespace engine::renderer
