#include <core/base/assert.h>
#include <core/json/json.h>
#include <core/memory/memory.h>
#include <core/profiling/startup_trace.h>  // thread_cpu_ns: the frame thread's own CPU (W155)
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <core/time/time.h>
#include <domain/gfx/resources.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/flythrough.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <renderer_log.h>
#include <string>
#include <utility>

namespace engine::renderer {

namespace {

// The chain's census as a log line a counted frame (flythrough.h, `VisibleCensus::count`; R152).
tunables::Bool census_log{"renderer.rt.census_log", false,
                          "Log every census frame's visible and caster entries by what the ray "
                          "tracing chain does with them (built by the frame, cached, static) and "
                          "by ring (engine-view --census)"};
// **What a placement draws against what it holds** (R152; docs/experiments/rt-chain-walk-2026-11-12
// .md, "What a far placement draws"): on one census frame, per ring, the meshes that draw the most
// entries, each with its cluster count (the pairs an instance of it holds), the instances of it
// the frame drew anything of, the entries they drew and the most one instance drew.
tunables::Int census_mesh_frame{"renderer.rt.census_mesh_frame", -1, -1, 1'000'000'000,
                                "The census frame on which to log, per ring, the meshes that "
                                "draw the most entries and how many clusters an instance of each "
                                "draws against how many it holds (-1: never)"};

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

// What this thread's `FlightBookkeeping` scopes left out of the frame-loop count, and how deep
// they are nested (only the outermost counts, so a scope inside another is not left out twice).
thread_local u64 t_left_out_allocations = 0;
thread_local u64 t_left_out_buffers = 0;
thread_local u32 t_bookkeeping_depth = 0;
#if ENGINE_DEBUG
thread_local bool t_capture_was_paused = false;
#endif

}  // namespace

FlightBookkeeping::FlightBookkeeping() noexcept : outermost_(t_bookkeeping_depth++ == 0) {
  if (outermost_) {
#if ENGINE_DEBUG
    t_capture_was_paused = mem::pause_allocation_capture(true);
#endif
    allocations_ = mem::allocations_on_thread();
    buffers_ = gfx::buffers_created_on_thread();
  }
}

FlightBookkeeping::~FlightBookkeeping() {
  --t_bookkeeping_depth;
  if (outermost_) {
#if ENGINE_DEBUG
    (void)mem::pause_allocation_capture(t_capture_was_paused);
#endif
    t_left_out_allocations += mem::allocations_on_thread() - allocations_;
    t_left_out_buffers += gfx::buffers_created_on_thread() - buffers_;
  }
}

u64 FlightBookkeeping::allocations_left_out() noexcept { return t_left_out_allocations; }
u64 FlightBookkeeping::buffers_left_out() noexcept { return t_left_out_buffers; }

scene::Reconstruction reconstruction_block(const Reconstruction& state) {
  scene::Reconstruction out;
  out.provider = state.provider == ReconstructionProvider::Registered && state.registered != nullptr
                     ? state.registered
                     : reconstruction_provider_name(state.provider);
  // The ratio's name once there is one (roadmap R160); until then a frame is the output's size.
  out.mode =
      state.internal_width == state.output_width && state.internal_height == state.output_height
          ? "native"
          : "ratio";
  out.inputs = state.inputs;
  out.internal_width = state.internal_width;
  out.internal_height = state.internal_height;
  out.output_width = state.output_width;
  out.output_height = state.output_height;
  return out;
}

MeasurementKeys measurement_keys(const Reconstruction& state) {
  MeasurementKeys keys;
  keys.open =
      "\"reconstruction\":" +
      write_json(schema::to_json(reconstruction_block(state)), JsonWriteOptions{.pretty = false}) +
      (state.native() ? ",\"gpu_ms\":" : ",\"gpu_ms\":null,\"reconstructed\":{\"gpu_ms\":");
  keys.close = state.native() ? "" : "}";
  return keys;
}

const scene::PassMs* record_pass_ms(const scene::FrameRecord& record) noexcept {
  if (record.gpu_ms.has_value()) return &*record.gpu_ms;
  if (record.reconstructed.has_value()) return &record.reconstructed->gpu_ms;
  return nullptr;
}

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
  out.resolve_stop = stats.resolve_stop;
  out.sun_ray_rule = stats.sun_ray_rule;
  out.light_cap_pixels = stats.light_cap_pixels;  // R213
  out.surface_reflect_rays = stats.surface_reflect_rays;  // R71
  // Every bucket of the pass list (scene_renderer.h, T85) into the record's field of its name.
  scene::PassMs ms;
#define ENGINE_RENDERER_PASS_RECORD(name, top) ms.name = stats.gpu_##name;
  ENGINE_RENDERER_PASS_BUCKETS(ENGINE_RENDERER_PASS_RECORD)
#undef ENGINE_RENDERER_PASS_RECORD
  // **The measurement rule** (ADR-0064; renderer.md, "Sampling over time: the measurement rule"):
  // the frame's times are a cost only when nothing reconstructed it and it was drawn at the
  // output's size. Any other frame's go under `reconstructed`, and `gpu_ms` stays null, so a
  // reader of a cost finds none.
  out.reconstruction = reconstruction_block(stats.reconstruction);
  if (stats.reconstruction.native()) {
    out.gpu_ms = ms;
  } else {
    out.reconstructed = scene::ReconstructedMs{.gpu_ms = ms};
  }
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
  out.rt_built_capacity = stats.rt_built_capacity;
  out.rt_references = stats.rt_references;
  out.rt_cache_built = stats.rt_cache_built;
  out.rt_micromaps = stats.rt_micromaps;
  out.rt_cache_evicted = stats.rt_cache_evicted;
  out.water_reflect = stats.water_reflect;
  out.water_reflect_views = stats.water_reflect_views;
  out.water_reflect_level_m = stats.water_reflect_level_m;
  out.water_refract = stats.water_refract;
  out.water_refract_views = stats.water_refract_views;
  out.water_refract_floor_m = stats.water_refract_floor_m;
  out.instances = stats.instances;
  out.pairs = stats.pairs;
  out.table_slots = stats.table_slots;
  out.table_pairs = stats.table_pairs;
  out.hole_pairs = stats.hole_pairs;
  out.history_pixels = stats.history_pixels;
  out.history_taken = stats.history_taken;
  out.history_trusted = stats.history_trusted;
  out.history_fresh = stats.history_fresh;
  out.history_runs = stats.history_runs;
  out.history_whole = stats.history_whole;
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
  out.cpu.resize = stats.cpu.resize_ms;
  // The frame on the GPU's own clock and its wait by what the GPU was doing (W155).
  if (stats.gpu.valid) {
    scene::FrameGpu gpu;
    gpu.span_ms = stats.gpu.span_ms;
    gpu.untimed_ms = std::max(0.0, stats.gpu.span_ms - stats.gpu_total);
    gpu.calibrated = stats.gpu.calibrated;
    gpu.queued_ms = stats.gpu.queued_ms;
    gpu.elsewhere_ms = stats.gpu.elsewhere_ms;
    gpu.wait_split = stats.cpu.wait_split;
    gpu.wait_gpu_ms = stats.cpu.wait_gpu_ms;
    gpu.wait_before_ms = stats.cpu.wait_before_ms;
    gpu.wait_late_ms = stats.cpu.wait_late_ms;
    out.gpu_frame = gpu;
  }
  return out;
}

scene::FrameSections frame_sections(const FrameSpans& s) {
  scene::FrameSections out;
  out.wall_ms = static_cast<f64>(s.next_start - s.start) / 1.0e6;
  out.host_ms = s.host_ms;
  // The renderer's wait is measured inside the call, so it can never exceed it.
  out.slot_wait_ms = std::min(s.slot_wait_ms, s.begin_call_ms);
  out.begin_ms = s.begin_call_ms - out.slot_wait_ms;
  out.resize_ms = s.resize_ms;
  out.fold_ms = s.fold_ms;
  out.acquire_ms = s.acquire_ms;
  out.ticks_ms = s.ticks_ms;
  out.submit_ms = s.submit_ms;
  out.after_ms = s.after_ms;
  out.present_ms = s.present_ms;
  out.tail_ms = s.next_start > s.end ? static_cast<f64>(s.next_start - s.end) / 1.0e6 : 0.0;
  out.pace_ms = std::min(s.pace_ms, out.tail_ms);  // a part of the tail, never more than it
  out.other_ms =
      out.wall_ms - (out.host_ms + out.slot_wait_ms + out.begin_ms + out.fold_ms + out.acquire_ms +
                     out.ticks_ms + out.submit_ms + out.after_ms + out.present_ms + out.tail_ms);
  if (s.thread_cpu_ms >= 0.0) {
    out.thread_cpu_ms = s.thread_cpu_ms;
    out.off_cpu_ms = std::max(0.0, out.wall_ms - s.thread_cpu_ms);
  }
  return out;
}

void summarize_rt(const RtStats& stats, scene::FlythroughRt& out) {
  out.caster_index_fallbacks = stats.caster_index_fallbacks;
  out.frame_estimate = stats.frame_estimate;
  out.bytes_per_cluster = stats.bytes_per_cluster;
  out.budget_mib = stats.budget_mib;
  out.budget_source = rt_budget_source_name(stats.budget_source);
  out.geometry_index_need = stats.geometry_index_max;  // version 3's compatibility spelling
  out.geometry_index_max = stats.geometry_index_max;
  out.geometry_index_required = stats.geometry_index_required;
  out.geometry_index_limit = stats.geometry_index_limit;
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
  out.static_forms = stats.static_forms;
  out.static_clusters = stats.static_clusters;
  out.static_bytes = stats.static_bytes;
  out.static_instances = stats.static_instances;
  out.static_pairs = stats.static_pairs;
  out.horizon_casters = stats.horizon_casters;
  out.cache_meshes = stats.cache_meshes;
  out.cache_clusters = stats.cache_clusters;
  out.cache_slots = stats.cache_slots;
  out.cache_slot_bytes = stats.cache_slot_bytes;
  out.cache_bytes = stats.cache_bytes;
  out.cache_occupied = stats.cache_occupied;
  out.cache_built_total = stats.cache_built_total;
  out.cache_build_frames = stats.cache_build_frames;
  out.cache_overflow = stats.cache_overflow;
  out.cache_unbuilt_frames = stats.cache_unbuilt_frames;
  out.cache_evictions = stats.cache_evictions;
  out.peak_references_wanted = stats.peak_references_wanted;
  out.reference_capacity = stats.reference_capacity;
  out.reference_limit = stats.reference_limit;
  out.bytes_per_reference = stats.bytes_per_reference;
  out.reference_grows = stats.reference_grows;
  out.reference_shrinks = stats.reference_shrinks;
  out.blas_built = stats.blas_built;
  out.peak_blas_built = stats.peak_blas_built;
  out.instance_slots = stats.instance_slots;
  out.static_reserved_bytes = stats.static_reserved_bytes;
  out.first_drop_bound = stats.first_drop_bound != 0 ? rt_bound_name(stats.first_drop_bound) : "";
  out.last_drop_bound = stats.last_drop_bound != 0 ? rt_bound_name(stats.last_drop_bound) : "";
}

void summarize_textures(const GpuScene& scene, scene::FlythroughTextures& out) {
  out.built = scene.textures_built();
  out.decoded = scene.textures_decoded();
  out.shared = scene.textures_shared();
  out.bytes = scene.texture_bytes();
  out.bytes_saved = scene.texture_bytes_saved();
  out.sharing = scene.share_textures();
  out.normal_variance = scene.normal_variance_textures();
  out.normal_variance_bytes = scene.normal_variance_bytes();
  const TextureBoundUse& bound = scene.texture_bound_use();  // R172 stage 1
  out.bound_textures = bound.managed;
  out.bound_resident_bytes = bound.resident_bytes;
  out.bound_peak_bytes = bound.peak_bytes;
  out.bound_promotions = bound.promotions;
  out.bound_late = bound.late;
  out.bound_worst_admit_ms = bound.worst_admit_ms;
  out.bound_worst_settle_ms = bound.worst_settle_ms;
  out.residency_ceiling = bound.ceiling;  // R172 stage 2
  out.residency_demotions = bound.demotions;
  out.residency_released_bytes = bound.released_bytes;
  out.residency_pressure_frames = bound.pressure_frames;
  out.residency_refused_in_hold = bound.refused_in_hold;
  out.residency_asked_late = bound.asked_late;
  out.residency_feedback_ms =
      bound.feedback_frames == 0
          ? 0.0
          : bound.feedback_ms_total / static_cast<f64>(bound.feedback_frames);
  out.residency_feedback_ms_max = bound.feedback_ms_max;
  out.residency_worst_demote_ms = bound.worst_demote_ms;
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
    u64 allocations = 0;
    u64 buffers = 0;
    scene::FrameWorld world{};  // with `measure_world`: the host's turn before it (W82)
    // The frame's sections (R130, `scene::FrameSections`): its start, the host's turn, the
    // `begin_frame` call whole, the fold and setup after it, the submission, and when the call
    // returned; the slot's wait and the resize inside `begin_frame` come from the renderer's own
    // record of the frame when it folds, and the tail from the next submission's start.
    i64 start = 0;
    i64 start_cpu = 0;  // the frame thread's CPU time at `start` (W155)
    i64 end = 0;
    f64 host_ms = 0.0;
    f64 begin_call_ms = 0.0;
    f64 fold_ms = 0.0;
    f64 submit_ms = 0.0;
  };
  // The caller's counts and the path's, multiplied in 64 bits (T187): in `u32` a long path or many
  // repeats wrapped the reservation, and a flight that cannot be counted in a `u32` is refused.
  const u64 flight_frames =
      (u64{options.warmup} + frames) * repeats + u64{options.frames_in_flight};
  if (flight_frames > std::numeric_limits<u32>::max()) {
    if (error != nullptr) {
      *error = "a flight of " + std::to_string(frames) + " frames, " +
               std::to_string(options.warmup) + " warm-up frames and " + std::to_string(repeats) +
               " repeats submits more frames than a flight counts";
    }
    return false;
  }
  Vector<Submission> submissions;
  submissions.reserve(static_cast<u32>(flight_frames));
  out.records.reserve(frames * repeats);
  renderer.reset_stats();
  u64 folded = renderer.stats().folded;
  // The frame loop's allocations on this thread (AGENTS.md, "No allocations in the frame loop in
  // steady state"): the host's turn, `begin_frame` and `submit_frame`, and not this loop's own
  // bookkeeping between them — the submissions list, which a warm-up by wall time can grow past its
  // reserve, and the records, which carry the frame's numbers out — nor what the host's hook
  // records for the benchmark inside a `FlightBookkeeping`.
  auto counted = [] {
    return std::pair<u64, u64>{
        mem::allocations_on_thread() - FlightBookkeeping::allocations_left_out(),
        gfx::buffers_created_on_thread() - FlightBookkeeping::buffers_left_out()};
  };
  auto submit = [&](u32 f, bool recorded, u32 repeat, u32 warmup) {
#if ENGINE_DEBUG
    mem::AllocationSite sites[64];
    mem::begin_allocation_capture(sites, 64);
    // End even on a refused host turn. The storage belongs to this submission's stack.
    struct EndCapture {
      ~EndCapture() { (void)mem::end_allocation_capture(); }
    } end_capture;
#endif
    const i64 start = time::monotonic_ns();
    const i64 start_cpu = profiling::thread_cpu_ns();
    const std::pair<u64, u64> turn = counted();
    // The host's turn, between two frames: a streamed world updates its ring from this camera and
    // hands the renderer its instances (FlightOptions::before_frame). It is in the frame's wall
    // time and not in its CPU milliseconds, which are the renderer's own; the record's `world`
    // names it (W82), its whole measured here and its parts by the hook.
    scene::FrameWorld world;
    if (options.before_frame != nullptr) {
      FlightStep step;
      step.repeat = repeat;
      step.frame = f;
      step.warmup = warmup;
      step.recorded = recorded;
      step.camera = camera_path_frame(path, f, frames);
      step.world = options.measure_world ? &world : nullptr;
      const i64 hook_started = time::monotonic_ns();
      if (!options.before_frame(options.before_frame_context, step, error)) return false;
      world.total_ms = static_cast<f64>(time::monotonic_ns() - hook_started) / 1.0e6;
      world.other_ms = world.total_ms - (world.update_ms + world.ground_ms + world.props_ms +
                                         world.ticks_ms + world.input_ms);
    }
    const i64 before_begin = time::monotonic_ns();
    renderer.begin_frame();
    const i64 after_begin = time::monotonic_ns();
    const std::pair<u64, u64> begun = counted();
#if ENGINE_DEBUG
    (void)mem::pause_allocation_capture(true);
#endif
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
          record.allocations = static_cast<u32>(std::min<u64>(done.allocations, ~0u));
          record.buffers = static_cast<u32>(std::min<u64>(done.buffers, ~0u));
          if (options.measure_world) record.world = done.world;
          // Its interval ends where the next submission began: that one's start, or this call's
          // when this call is the next (one frame in flight).
          const u64 index = stats.last.submission;
          const i64 next_start = index + 1 < submissions.size()
                                     ? submissions[static_cast<u32>(index + 1)].start
                                     : start;
          const i64 next_cpu = index + 1 < submissions.size()
                                   ? submissions[static_cast<u32>(index + 1)].start_cpu
                                   : start_cpu;
          FrameSpans spans;
          spans.start = done.start;
          spans.end = done.end;
          spans.next_start = next_start;
          spans.host_ms = done.host_ms;
          spans.begin_call_ms = done.begin_call_ms;
          spans.slot_wait_ms = record.cpu.wait;
          spans.resize_ms = record.cpu.resize;
          spans.fold_ms = done.fold_ms;
          spans.submit_ms = done.submit_ms;
          if (done.start_cpu > 0 && next_cpu > 0)
            spans.thread_cpu_ms = static_cast<f64>(next_cpu - done.start_cpu) / 1.0e6;
          record.sections = frame_sections(spans);
          // The frame's own interval (FrameRecord version 25, W155): until then the interval
          // before it, so a long frame's parts sat in the record before the one that showed it.
          record.frame_ms = record.sections->wall_ms;
          out.records.push_back(std::move(record));
        }
      }
    }
    // The wait for the slot is the GPU's time, not the CPU's: the CPU's starts once it is free.
    const i64 ready = time::monotonic_ns();
    FrameDesc frame;
    frame.camera = camera_path_frame(path, f, frames);
    frame.frame_index = f;
    frame.world_time_s = options.world_rate * static_cast<f64>(f) / 60.0;
    frame.sky_time_s = options.sky_start_s + options.sun_rate * static_cast<f64>(f) / 60.0;
    // A repeat's first path frame starts sampling over time's history again (ADR-0064;
    // renderer.md, "Sampling over time: the inputs"), so with the inputs on frame f of every
    // repeat is drawn at the same sub-pixel phase and draws the same cut, as `deterministic` asks.
    // Not its first warm-up frame: a warm-up that runs for `warmup_seconds` is a different number
    // of frames each repeat, and so would leave each repeat's path at another phase.
    frame.history_reset = recorded && f == 0;
    submissions.push_back(Submission{repeat, f, recorded});
    submissions[submissions.size() - 1].world = world;
    const std::pair<u64, u64> before_submit = counted();
#if ENGINE_DEBUG
    (void)mem::pause_allocation_capture(false);
#endif
    const i64 before_submit_ns = time::monotonic_ns();
    const bool ok = renderer.submit_frame(frame, error) != 0;
    const i64 after_submit_ns = time::monotonic_ns();
#if ENGINE_DEBUG
    (void)mem::pause_allocation_capture(true);
#endif
    const std::pair<u64, u64> submitted = counted();
    Submission& mine = submissions[submissions.size() - 1];
    mine.start = start;
    mine.start_cpu = start_cpu;
    mine.end = after_submit_ns;
    mine.host_ms = static_cast<f64>(before_begin - start) / 1.0e6;
    mine.begin_call_ms = static_cast<f64>(after_begin - before_begin) / 1.0e6;
    mine.fold_ms = static_cast<f64>(before_submit_ns - after_begin) / 1.0e6;
    mine.submit_ms = static_cast<f64>(after_submit_ns - before_submit_ns) / 1.0e6;
    mine.allocations = (begun.first - turn.first) + (submitted.first - before_submit.first);
    mine.buffers = (begun.second - turn.second) + (submitted.second - before_submit.second);
    mine.cpu_ms = static_cast<f64>(time::monotonic_ns() - ready) / 1.0e6;
#if ENGINE_DEBUG
    if (recorded && mine.allocations != 0) {
      std::fprintf(stderr, "frame allocation sites repeat=%u frame=%u allocations=%llu\n", repeat,
                   f, static_cast<unsigned long long>(mine.allocations));
      mem::print_allocation_capture();
    }
#endif
    return ok;
  };
  // A refused frame returns from inside the loops; the renderer counts its frames again after.
  struct Counting {
    SceneRenderer& renderer;
    ~Counting() { renderer.count_frames(true); }
  } counting{renderer};
  for (u32 r = 0; r < repeats; ++r) {
    const i64 warm_from = time::monotonic_ns();
    const i64 warm_ns = static_cast<i64>(options.warmup_seconds * 1.0e9);
    // The warm-up is drawn and not measured: it stays out of the run's drop sums (G133).
    renderer.count_frames(false);
    for (u32 w = 0; w < options.warmup || time::monotonic_ns() - warm_from < warm_ns; ++w) {
      if (!submit(0, false, r, w)) return false;
    }
    renderer.count_frames(true);
    const i64 started = time::monotonic_ns();
    for (u32 f = 0; f < frames; ++f) {
      if (!submit(f, true, r, ~0u)) return false;
    }
    out.seconds += static_cast<f64>(time::monotonic_ns() - started) / 1.0e9;
  }
  // A frame's numbers fold in when its slot comes around, so the last path frames need that many
  // submissions behind them; these are drawn and not recorded.
  renderer.count_frames(false);
  for (u32 d = 0; d < options.frames_in_flight; ++d) {
    if (!submit(frames - 1, false, repeats - 1, ~0u)) return false;
  }
  renderer.count_frames(true);
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
  //
  // **The GPU's pass buckets are the frame record's own** (T78): every field of `scene::PassMs`,
  // walked by its schema descriptor and written into the `scene::PassPercentiles` field of the same
  // name, so the summary cannot leave out a bucket the record has. Until T78 they were a list here
  // that had drifted to 14 of the record's 29. The figures beside them stay the list below.
  const schema::TypeInfo& bucket_type = schema::type_of<scene::PassMs>();
  const schema::TypeInfo& summary_type = schema::type_of<scene::PassPercentiles>();
  const u32 buckets = static_cast<u32>(bucket_type.fields.size());
  u32 total = buckets;  // the bucket a marker's `total_ms` reads
  for (u32 b = 0; b < buckets; ++b) {
    ENGINE_ASSERT(bucket_type.fields[b].type.kind == schema::Kind::F64,
                  "a PassMs bucket is f64 milliseconds");
    if (std::strcmp(bucket_type.fields[b].name, "total") == 0) total = b;
  }
  ENGINE_ASSERT(total < buckets, "PassMs has a total");
  Vector<Vector<f64>> per_bucket(buckets);
  enum Figure : u32 {
    pairs,
    casters,
    rt_built,
    rt_wanted,
    shadow_pairs,
    cpu,
    wall,
    cpu_wait,
    cpu_begin,
    cpu_tables,
    cpu_terrain,
    cpu_passes,
    cpu_compile,
    cpu_record,
    cpu_submit,
    rt_references,
    rt_cache_built,
    figures
  };
  Vector<f64> per_frame[figures];
  Vector<f64> repeats_of;
  out.deterministic = true;
  out.mismatched_frames = 0;
  out.uploads = 0;
  out.upload_bytes = 0;
  out.evictions = 0;
  out.ticks = 0;
  out.frame_loop = scene::FrameLoopAllocations{};
  for (const scene::FrameRecord* r : slot) {
    if (r == nullptr) continue;
    out.uploads += r->uploads;
    out.upload_bytes += r->upload_bytes;
    out.evictions += r->evictions;
    out.ticks += r->ticks;
    out.frame_loop.allocations += r->allocations;
    out.frame_loop.buffers += r->buffers;
    out.frame_loop.allocating_frames += r->allocations > 0 ? 1u : 0u;
    out.frame_loop.buffer_frames += r->buffers > 0 ? 1u : 0u;
    if (r->allocations > 0 || r->buffers > 0) {
      out.frame_loop.last_frame = std::max<i64>(out.frame_loop.last_frame, r->frame);
    }
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
    for (u32 b = 0; b < buckets; ++b) {
      const u32 offset = bucket_type.fields[b].offset;
      repeats_of.clear();
      for (u32 k = 0; k < repeats; ++k) {
        const scene::FrameRecord* r = slot[f * repeats + k];
        if (r == nullptr) continue;
        // A record's times wherever it carries them (ADR-0064): its `gpu_ms`, or under
        // `reconstructed` when it was not a native measurement.
        const scene::PassMs* passes = record_pass_ms(*r);
        if (passes == nullptr) continue;
        f64 ms = 0.0;
        std::memcpy(&ms, reinterpret_cast<const u8*>(passes) + offset, sizeof(ms));
        repeats_of.push_back(ms);
      }
      per_bucket[b].push_back(median_of(repeats_of));
    }
    for (u32 p = 0; p < figures; ++p) {
      repeats_of.clear();
      for (u32 k = 0; k < repeats; ++k) {
        const scene::FrameRecord* r = slot[f * repeats + k];
        if (r == nullptr) continue;
        const f64 values[figures] = {static_cast<f64>(r->visible_pairs),
                                     static_cast<f64>(r->shadow_casters),
                                     static_cast<f64>(r->rt_built),
                                     static_cast<f64>(r->rt_wanted),
                                     static_cast<f64>(r->shadow_pairs),
                                     r->cpu_ms,
                                     r->frame_ms,
                                     r->cpu.wait,
                                     r->cpu.begin,
                                     r->cpu.tables,
                                     r->cpu.terrain,
                                     r->cpu.passes,
                                     r->cpu.compile,
                                     r->cpu.record,
                                     r->cpu.submit,
                                     static_cast<f64>(r->rt_references),
                                     static_cast<f64>(r->rt_cache_built)};
        repeats_of.push_back(values[p]);
      }
      per_frame[p].push_back(median_of(repeats_of));
    }
  }
  auto over = [&](u32 p) {
    return percentiles(std::span<const f64>(per_frame[p].data(), per_frame[p].size()));
  };
  // **The measurement rule** (ADR-0064): the summary's distribution is a cost only when every
  // record was a native measurement. One record that was not puts the whole distribution under
  // `reconstructed` and leaves `gpu_ms` null, and the block is that record's, so the summary says
  // what a reader must not take for a cost.
  bool native = true;
  bool block_set = false;
  for (const scene::FrameRecord* r : slot) {
    if (r == nullptr) continue;
    const bool record_native = r->gpu_ms.has_value() && !r->reconstructed.has_value();
    if (!block_set || (native && !record_native)) out.reconstruction = r->reconstruction;
    block_set = true;
    native = native && record_native;
  }
  scene::PassPercentiles distribution{};
  for (u32 b = 0; b < buckets; ++b) {
    const schema::FieldInfo* to = summary_type.find_field(bucket_type.fields[b].name);
    ENGINE_ASSERT(to != nullptr && to->type.type == &schema::type_of<scene::Percentiles>(),
                  "every PassMs bucket has a PassPercentiles field of its name");
    if (to == nullptr || to->type.type != &schema::type_of<scene::Percentiles>()) continue;
    *reinterpret_cast<scene::Percentiles*>(reinterpret_cast<u8*>(&distribution) + to->offset) =
        percentiles(std::span<const f64>(per_bucket[b].data(), per_bucket[b].size()));
  }
  out.gpu_ms.reset();
  out.reconstructed.reset();
  if (native) {
    out.gpu_ms = distribution;
  } else {
    out.reconstructed = scene::ReconstructedPercentiles{.gpu_ms = distribution};
  }
  out.visible_pairs = over(pairs);
  out.shadow_casters = over(casters);
  out.rt.built = over(rt_built);
  out.rt.wanted = over(rt_wanted);
  out.rt.references = over(rt_references);
  out.rt.cache_built = over(rt_cache_built);
  out.shadow_pairs = over(shadow_pairs);
  out.cpu_ms = over(cpu);
  out.frame_ms = over(wall);
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
    if (total < buckets && m.frame < per_bucket[total].size()) {
      // A marker's time is a cost too: zero in a summary that is not a native measurement.
      m.total_ms = native ? per_bucket[total][m.frame] : 0.0;
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
  summarize_slow(records, k_slow_frame_ms, out);
}

namespace {

struct Part {
  const char* name;
  f64 ms;
};

Part largest(std::span<const Part> parts) noexcept {
  Part best = parts[0];
  for (const Part& p : parts) {
    if (p.ms > best.ms) best = p;
  }
  return best;
}

}  // namespace

// The largest part at each step down, for as long as one part holds at least half of the one
// above it: past that the time is spread, and the span above is the honest answer.
scene::SlowFrame slowest_span(const scene::FrameRecord& r) {
  scene::SlowFrame out;
  out.frame = r.frame;
  out.repeat = r.repeat;
  if (!r.sections.has_value()) return out;
  const scene::FrameSections& s = *r.sections;
  out.wall_ms = s.wall_ms;
  const Part sections[] = {
      {"host", s.host_ms},     {"slot_wait", s.slot_wait_ms}, {"begin", s.begin_ms},
      {"fold", s.fold_ms},     {"acquire", s.acquire_ms},     {"ticks", s.ticks_ms},
      {"submit", s.submit_ms}, {"after", s.after_ms},         {"present", s.present_ms},
      {"tail", s.tail_ms},     {"other", s.other_ms}};
  Part at = largest(sections);
  std::string span = at.name;
  const auto down = [&](std::span<const Part> parts) {
    const Part p = largest(parts);
    if (p.ms * 2.0 < at.ms) return false;
    span += '.';
    span += p.name;
    at = p;
    return true;
  };
  if (span == "host" && r.world.has_value()) {
    const scene::FrameWorld& w = *r.world;
    const Part host[] = {{"update", w.update_ms},
                         {"ground", w.ground_ms},
                         {"props", w.props_ms},
                         {"ticks", w.ticks_ms},
                         {"input", w.input_ms}};
    if (down(host)) {
      if (std::strcmp(at.name, "update") == 0) {
        // `FrameWorld` version 2 (W155): the update by who spent it, then the placements' calls
        // by part; a version 1 record names the placements' and the renderer's parts at once.
        const bool by_caller = w.placements_ms > 0.0 || w.ring_ms > 0.0 || w.drawn_ms > 0.0;
        const Part callers[] = {{"ring", w.ring_ms},
                                {"drawn", w.drawn_ms},
                                {"placements", w.placements_ms},
                                {"restand", w.restand_ms}};
        const Part placements[] = {{"events", w.events_ms},   {"due", w.due_ms},
                                   {"meshes", w.meshes_ms},   {"cells", w.cells_ms},
                                   {"admit", w.admit_ms},     {"commit", w.commit_ms},
                                   {"handover", w.handover_ms}};
        const Part update[] = {{"tiles", w.tiles_ms},
                               {"admit", w.admit_ms},
                               {"commit", w.commit_ms},
                               {"handover", w.handover_ms}};
        const bool stepped =
            by_caller ? down(callers) && std::strcmp(at.name, "placements") == 0 && down(placements)
                      : down(update);
        if (stepped && std::strcmp(at.name, "handover") == 0) {
          const Part handover[] = {{"compare", w.compare_ms},  {"layout", w.layout_ms},
                                   {"write", w.write_ms},      {"after", w.after_ms},
                                   {"fires", w.fires_ms},      {"far", w.far_ms},
                                   {"relayout", w.relayout_ms}};
          (void)down(handover);
        }
      } else if (std::strcmp(at.name, "ground") == 0 && r.terrain.has_value()) {
        const Part ground[] = {{"wait", r.terrain->wait_ms}};
        (void)down(ground);
      }
    }
  } else if (span == "submit") {
    const Part submit[] = {{"tables", r.cpu.tables}, {"terrain", r.cpu.terrain},
                           {"passes", r.cpu.passes}, {"compile", r.cpu.compile},
                           {"record", r.cpu.record}, {"submit", r.cpu.submit}};
    (void)down(submit);
  } else if (span == "slot_wait" && r.gpu_frame.has_value() && r.gpu_frame->wait_split) {
    // The wait by what the GPU was doing (W155): inside the awaited frame, before it had started
    // it, or after it, until this thread woke.
    const scene::FrameGpu& g = *r.gpu_frame;
    const Part wait[] = {
        {"gpu", g.wait_gpu_ms}, {"before", g.wait_before_ms}, {"late", g.wait_late_ms}};
    (void)down(wait);
  }
  out.span = std::move(span);
  out.span_ms = at.ms;
  return out;
}

void summarize_slow(std::span<const scene::FrameRecord> records, f64 limit_ms,
                    scene::FlythroughSummary& out) {
  out.slow.reset();
  Vector<f64> walls;
  Vector<const scene::FrameRecord*> over;
  for (const scene::FrameRecord& r : records) {
    // The first frame of a repeat is the load's or the path's jump back to its start.
    if (!r.sections.has_value() || r.frame == 0) continue;
    walls.push_back(r.sections->wall_ms);
    if (r.sections->wall_ms > limit_ms) over.push_back(&r);
  }
  if (walls.empty()) return;
  scene::FlythroughSlow slow;
  slow.limit_ms = limit_ms;
  slow.wall_ms = percentiles(std::span<const f64>(walls.data(), walls.size()));
  std::sort(walls.begin(), walls.end());
  const u32 at999 = static_cast<u32>(std::ceil(0.999 * static_cast<f64>(walls.size()))) - 1u;
  slow.p999_ms = walls[std::min<u32>(at999, walls.size() - 1u)];
  for (const f64 ms : walls) {
    slow.over_33 += ms > 33.0 ? 1u : 0u;
    slow.over_50 += ms > 50.0 ? 1u : 0u;
    slow.over_100 += ms > 100.0 ? 1u : 0u;
  }
  std::sort(over.begin(), over.end(), [](const scene::FrameRecord* a, const scene::FrameRecord* b) {
    if (a->sections->wall_ms != b->sections->wall_ms)
      return a->sections->wall_ms > b->sections->wall_ms;
    return a->repeat != b->repeat ? a->repeat < b->repeat : a->frame < b->frame;
  });
  for (const scene::FrameRecord* r : over) {
    scene::SlowFrame frame = slowest_span(*r);
    slow.ground_waits += frame.span == "host.ground.wait" ? 1u : 0u;
    if (slow.worst.size() < k_slow_frames_named) slow.worst.push_back(std::move(frame));
  }
  out.slow = std::move(slow);
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
  u64 entry_count = 0;
  if (!copy_runs(scene, stats, false, entry_count, error)) return false;
  const auto* entries = static_cast<const u32*>(staging_.mapped);
  const u32 cluster_count = data.lod.lod.size();
  std::memset(chain_, 0, sizeof(chain_));
  // The chain's census (flythrough.h): an entry by its instance's kind and rank.
  const auto tally = [this, &scene](u32 instance, u32 caster) {
    const u32 kind = scene.rt_instance_kind(instance);
    if (kind >= k_chain_kinds) return;
    const u32 rank = std::min(scene.rt_instance_rank(instance), k_chain_ranks - 1);
    ++chain_[(caster * k_chain_ranks + rank) * k_chain_kinds + kind];
  };
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
    tally(instance, 0);
  }
  // The shadow casters' run, which nothing draws: what the cone test kept out of the picture and
  // the volumes round the eye, the water's mirror and its floor added (gpu_scene.h).
  u64 caster_count = 0;
  if (!copy_runs(scene, stats, true, caster_count, error)) return false;
  for (u64 e = 0; e < caster_count; ++e)
    tally(entries[e * 2 + 0], 1);
  // The trees' motion (R206): the casters now, the drawn run below (the staging holds one at once).
  if (!scene.flora_motion().empty()) {
    count_motion(scene, entries, caster_count, 1);
    if (!copy_runs(scene, stats, false, entry_count, error)) return false;
    count_motion(scene, entries, entry_count, 0);
    if ((counted_ + 1) % 120 == 0) log_motion();
  }
  if (census_mesh_frame.get() == static_cast<i64>(counted_)) {
    // The drawn run again (the staging buffer holds the casters now, or the drawn run when the
    // motion was counted above; copied either way): entries per instance, then
    // per (rank, mesh) the instances seen, their entries and the most one drew. Cold: one frame.
    if (!copy_runs(scene, stats, false, entry_count, error)) return false;
    u32 instance_end = 0;
    for (u64 e = 0; e < entry_count; ++e)
      instance_end = std::max(instance_end, entries[e * 2 + 0] + 1u);
    Vector<u32> per_instance(instance_end, 0u);
    for (u64 e = 0; e < entry_count; ++e)
      ++per_instance[entries[e * 2 + 0]];
    struct Row {
      u32 instances = 0;
      u32 drawn = 0;
      u32 most = 0;
    };
    Vector<Row> rows(u64{k_chain_ranks} * data.parts.size());
    for (u32 i = 0; i < per_instance.size(); ++i) {
      if (per_instance[i] == 0) continue;
      const u32 mesh = scene.instance_mesh(i);
      if (mesh >= data.parts.size()) continue;
      const u32 rank = std::min(scene.rt_instance_rank(i), k_chain_ranks - 1);
      Row& row = rows[u64{rank} * data.parts.size() + mesh];
      ++row.instances;
      row.drawn += per_instance[i];
      row.most = std::max(row.most, per_instance[i]);
    }
    for (u32 rank = 0; rank < k_chain_ranks; ++rank) {
      for (u32 top = 0; top < 10; ++top) {
        u32 best = ~0u;
        for (u32 m = 0; m < data.parts.size(); ++m) {
          const Row& row = rows[u64{rank} * data.parts.size() + m];
          if (row.drawn == 0) continue;
          if (best == ~0u || row.drawn > rows[u64{rank} * data.parts.size() + best].drawn) best = m;
        }
        if (best == ~0u) break;
        Row& row = rows[u64{rank} * data.parts.size() + best];
        ENGINE_LOG_INFO(log_renderer, "census mesh", log::field("frame", counted_),
                        log::field("rank", rank), log::field("mesh", best),
                        log::field("clusters", data.parts[best].cluster_count),
                        log::field("instances_drawn", row.instances),
                        log::field("entries", row.drawn), log::field("most", row.most));
        row.drawn = 0;  // taken
      }
    }
  }
  ++counted_;
  if (census_log.get()) {
    // Summed over the ranks by kind, and the frame's own builds by rank: kinds 0 and 1 are built
    // every frame under any picture, 4 and 5 (untraced) under a traced one alone.
    u32 kinds[2][k_chain_kinds] = {};
    u32 own[2][k_chain_ranks] = {};
    u32 refs[k_chain_ranks] = {};
    for (u32 c = 0; c < 2; ++c) {
      for (u32 r = 0; r < k_chain_ranks; ++r) {
        for (u32 k = 0; k < k_chain_kinds; ++k) {
          const u32 n = chain_[(c * k_chain_ranks + r) * k_chain_kinds + k];
          kinds[c][k] += n;
          if (k == 0 || k == 1 || k == 4 || k == 5) own[c][r] += n;
          // What references the cache, by rank: a rigid instance's entries and, since the far
          // field rests (R152), a field instance's past the distance — traced (2) or not (6).
          if (c == 0 && (k == 2 || k == 6)) refs[r] += n;
        }
      }
    }
    ENGINE_LOG_INFO(
        log_renderer, "chain census", log::field("frame", counted_ - 1),
        log::field("drawn", static_cast<u32>(entry_count)),
        log::field("casters", static_cast<u32>(caster_count)), log::field("deform", kinds[0][0]),
        log::field("rigid", kinds[0][1]), log::field("cached", kinds[0][2]),
        log::field("static", kinds[0][3]), log::field("untraced_deform", kinds[0][4]),
        log::field("untraced_rigid", kinds[0][5]), log::field("untraced_static", kinds[0][7]),
        log::field("untraced_cached", kinds[0][6]), log::field("ref_r0", refs[0]),
        log::field("ref_r1", refs[1]), log::field("ref_r2", refs[2]), log::field("ref_r3", refs[3]),
        log::field("ref_r4", refs[4]), log::field("own_r0", own[0][0]),
        log::field("own_r1", own[0][1]), log::field("own_r2", own[0][2]),
        log::field("own_r3", own[0][3]), log::field("own_r4", own[0][4]),
        log::field("own_r5", own[0][5]), log::field("c_deform", kinds[1][0]),
        log::field("c_rigid", kinds[1][1]), log::field("c_cached", kinds[1][2]),
        log::field("c_static", kinds[1][3]), log::field("c_untraced", kinds[1][4] + kinds[1][5]),
        log::field("c_own_r0", own[1][0]), log::field("c_own_r1", own[1][1]),
        log::field("c_own_r2", own[1][2]), log::field("c_own_r3", own[1][3]),
        log::field("c_own_r4", own[1][4]), log::field("c_own_r5", own[1][5]));
  }
  return true;
}

// The swaying trees' entries by the crown's distance and the tips' motion a frame (flythrough.h;
// R206). A pixel is 2 tan(27.5 degrees) / 2160 radians, 4.82e-4: the owner's 3840 x 2160 at 55.
void VisibleCensus::count_motion(const GpuScene& scene, const u32* entries, u64 count, u32 caster) {
  static constexpr f32 k_distance_m[k_motion_bins - 1] = {10, 20, 40, 80, 160, 320, 640};
  static constexpr f32 k_px[k_motion_bins - 1] = {0.25f, 0.5f, 1, 2, 4, 8, 16};
  static constexpr f32 k_pixel_rad = 4.8199e-4f;
  const std::span<const Vec2> motion = scene.flora_motion();
  if (caster == 0 && trees_seen_.size() < motion.size())
    trees_seen_.resize(static_cast<u32>(motion.size()), 0u);
  const auto bin = [](const f32* edges, f32 v) {
    u32 b = 0;
    while (b < k_motion_bins - 1 && !(v < edges[b]))
      ++b;
    return b;
  };
  for (u64 e = 0; e < count; ++e) {
    const u32 instance = entries[e * 2 + 0];
    if (instance >= motion.size() || !(motion[instance].y > 0.0f)) continue;  // not a swaying tree
    if (scene.rt_instance_kind(instance) != 0) continue;  // built from the pool this frame
    const u32 d = bin(k_distance_m, motion[instance].y);
    const u32 px = bin(k_px, motion[instance].x / motion[instance].y / k_pixel_rad);
    ++motion_[(caster * k_motion_bins + d) * k_motion_bins + px];
    if (caster == 0 && trees_seen_[instance] != counted_ + 1) {
      trees_seen_[instance] = counted_ + 1;
      ++motion_[2 * k_motion_bins * k_motion_bins + d];
    }
  }
}

bool VisibleCensus::pairs(const GpuScene& scene, const Stats& stats, Vector<u64>& out,
                          std::string* error) {
  out.clear();
  if (device_ == nullptr || !staging_.buffer.valid() || scene.visible.size > staging_.size) {
    if (error != nullptr) *error = "the census was not created for this visible list";
    return false;
  }
  u64 entry_count = 0;
  if (!copy_runs(scene, stats, false, entry_count, error)) return false;
  const auto* entries = static_cast<const u32*>(staging_.mapped);
  out.resize(static_cast<u32>(entry_count));
  for (u64 e = 0; e < entry_count; ++e)
    out[static_cast<u32>(e)] = u64{entries[e * 2 + 0]} << 32 | entries[e * 2 + 1];
  std::sort(out.begin(), out.end());
  return true;
}

void VisibleCensus::log_motion() const {
  for (u32 c = 0; c < 2; ++c) {
    for (u32 d = 0; d < k_motion_bins; ++d) {
      const u64* row = motion_ + (c * k_motion_bins + d) * k_motion_bins;
      ENGINE_LOG_INFO(
          log_renderer, "census motion", log::field("frames", counted_ + 1),
          log::field("caster", c), log::field("distance_bin", d),
          log::field("trees", c == 0 ? motion_[2 * k_motion_bins * k_motion_bins + d] : 0),
          log::field("px0", row[0]), log::field("px1", row[1]), log::field("px2", row[2]),
          log::field("px3", row[3]), log::field("px4", row[4]), log::field("px5", row[5]),
          log::field("px6", row[6]), log::field("px7", row[7]));
    }
  }
}

// Every view's drawn runs — or, with `casters`, its shadow casters' — into the staging buffer, each
// only as long as the count the frame's statistics hold: the rest of a run is whatever an earlier
// frame left there.
bool VisibleCensus::copy_runs(const GpuScene& scene, const Stats& stats, bool casters, u64& entries,
                              std::string* error) {
  gfx::BufferCopy regions[k_max_views * k_visible_runs];
  u32 region_count = 0;
  u64 cursor = 0;
  for (u32 v = 0; v < scene.view_count() && v < k_max_views; ++v) {
    const ViewStats& view = stats.views[v];
    u32 counts[k_visible_runs] = {view.visible_hw, view.visible_pass2, view.visible_sw};
    if (casters) {
      for (u32 run = 0; run < k_visible_runs; ++run)
        counts[run] = run == gfx::k_caster_run ? view.shadow_casters : 0u;
    }
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
  entries = cursor / 8;
  return true;
}

}  // namespace engine::renderer
