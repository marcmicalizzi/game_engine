// The step's cost on the card (roadmap W175; docs/experiments/water-step-on-the-card.md). Each
// variant runs batches of 16 steps of one state at one grid, every pass a gfx::GpuTimer zone, and
// prints the GPU time a step takes (median and p95 over every step measured after a warm-up), the
// batch's own span over its steps, and the step's length the state allows. The harness's own
// columns are the CPU's wall time per batch, submission and wait included, and cells a second from
// set_items. Cells are 0.5 m; the state is put back every 1,024 steps (untimed) so it stays the
// state named.
#include <foundation/bench/bench.h>
#include <systems/water_live/water_live.h>

#include <domain/gfx/device.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/render_graph.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace engine;

namespace {

enum class Kind { lake, stream, beach };
constexpr u32 k_batch = 16;
constexpr u32 k_reset_batches = 64;
constexpr f64 k_cell_m = 0.5;

struct Case {
  water_state::Config config;
  std::vector<f64> bed, manning, h, hu, hv;
};

Case make_case(Kind kind, u32 n) {
  Case c;
  c.config.nx = n;
  c.config.ny = n;
  c.config.cell_m = k_cell_m;
  const usize cells = usize{n} * n;
  c.bed.assign(cells, 0.0);
  c.manning.assign(cells, 0.025);
  c.h.assign(cells, 0.0);
  c.hu.assign(cells, 0.0);
  c.hv.assign(cells, 0.0);
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const usize i = usize{y} * n + x;
      const f64 fx = static_cast<f64>(x) / n;
      const f64 off = std::fabs(static_cast<f64>(y) - 0.5 * n) / n;  // 0 at the axis, 0.5 at a side
      switch (kind) {
        case Kind::lake:  // a still lake a metre deep over a flat bed
          c.h[i] = 1.0;
          break;
        case Kind::stream: {  // a reach falling 2 % east, a valley across it; half the cells dry
          c.bed[i] = 0.02 * (1.0 - fx) * n * k_cell_m + 0.05 * off * n * k_cell_m;
          if (off < 0.25) {
            c.h[i] = 0.1;
            c.hu[i] = 0.1;  // 1 m/s
          }
          c.manning[i] = 0.04;
          break;
        }
        case Kind::beach: {  // a plane beach from -2 m to +1 m, still at 0, a bore offshore
          c.bed[i] = -2.0 + 3.0 * fx;
          const f64 level = fx < 0.3 ? 0.5 : 0.0;
          c.h[i] = std::max(level - c.bed[i], 0.0);
          break;
        }
      }
    }
  }
  if (kind == Kind::stream) {
    c.config.edges.west = water_state::Edge::open;
    c.config.edges.east = water_state::Edge::open;
  }
  return c;
}

f64 percentile(std::vector<f64> v, f64 p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const usize k = static_cast<usize>(std::ceil(p * static_cast<f64>(v.size()))) - 1;
  return v[std::min(k, v.size() - 1)];
}

const char* kind_name(Kind kind) {
  return kind == Kind::lake ? "lake" : kind == Kind::stream ? "stream" : "beach";
}

void run_case(bench::State& state, Kind kind, water_live::StepOptions options, const char* form) {
  const u32 n = static_cast<u32>(state.arg());
  if (bench::smoke_mode()) {
    // CTest's smoke run is a test that may open no device (ENGINE_TEST_DEVICES=forbidden, and the
    // bench main takes no GPU lock); the step's device cases are water_live.gpu's.
    while (state.keep_running()) {
    }
    return;
  }
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    std::printf("water_live bench: no device (%s)\n", error.c_str());
    while (state.keep_running()) {
    }
    return;
  }
  {
    const Case c = make_case(kind, n);
    water_live::DeviceStep step;
    gfx::GpuTimer timer;
    bool ok = step.create(device, c.config, options, &error) &&
              step.set_bed(c.bed, 0.0, &error) && step.set_manning(c.manning, &error) &&
              step.write_state(c.h, c.hu, c.hv, &error) &&
              timer.create(device, 1, 4 * k_batch, &error);
    std::vector<f64> step_ms, dt_ms, faces_ms, update_ms, span_ms;
    f64 time_s = 0.0;
    u64 steps = 0;
    u32 batches = 0;
    bool measuring = false;
    // Reads the zones of the batch before: one step begins at each "water.dt".
    auto collect = [&]() {
      if (!measuring) return;
      f64 current = -1.0;
      for (const gfx::GpuTimer::Zone& z : timer.results()) {
        if (std::strcmp(z.name, "water.dt") == 0) {
          if (current >= 0.0) step_ms.push_back(current);
          current = z.ms;
          dt_ms.push_back(z.ms);
        } else {
          current += z.ms;
          (std::strcmp(z.name, "water.faces") == 0 ? faces_ms : update_ms).push_back(z.ms);
        }
      }
      if (current >= 0.0) step_ms.push_back(current);
      if (timer.frame_span().valid)
        span_ms.push_back(timer.frame_span().span_ms / static_cast<f64>(k_batch));
    };
    auto batch = [&]() {
      gfx::RenderGraph graph(device);
      step.record(graph, k_batch, std::numeric_limits<f32>::infinity(), &timer);
      if (!graph.compile(&error)) return false;
      const bool done = gfx::submit_immediate(
          device,
          [&](gfx::CommandList commands) {
            timer.begin_frame(commands, 0);
            collect();
            graph.execute(commands);
            timer.end_frame(commands);
          },
          &error);
      ++batches;
      if (batches % k_reset_batches == 0) {
        state.pause_timing();
        water_live::Ledger ledger;
        if (step.read_ledger(ledger, &error)) {
          time_s += ledger.time_s;
          steps += ledger.steps;
        }
        step.write_state(c.h, c.hu, c.hv, &error);
        state.resume_timing();
      }
      return done;
    };
    // A warm-up the zones do not count.
    for (u32 i = 0; ok && i < 4; ++i)
      ok = batch();
    measuring = true;
    while (state.keep_running()) {
      if (ok) ok = batch();
    }
    if (ok) {
      // One more frame to read the last batch's zones.
      ok = gfx::submit_immediate(
          device,
          [&](gfx::CommandList commands) {
            timer.begin_frame(commands, 0);
            collect();
            timer.end_frame(commands);
          },
          &error);
      water_live::Ledger ledger;
      if (step.read_ledger(ledger, &error)) {
        time_s += ledger.time_s;
        steps += ledger.steps;
      }
    }
    if (!ok) {
      std::printf("water_live bench: %s\n", error.c_str());
    } else if (!step_ms.empty()) {
      const f64 cells = static_cast<f64>(n) * n;
      const f64 median = percentile(step_ms, 0.5);
      const f64 mean_dt = steps > 0 ? time_s / static_cast<f64>(steps) : 0.0;
      const f64 substeps = mean_dt > 0.0 ? std::ceil((1.0 / 60.0) / mean_dt) : 0.0;
      std::printf(
          "water_live.%s.%s/%u: gpu step median %.4f ms p95 %.4f ms (dt %.4f, faces %.4f, "
          "update %.4f), span/step %.4f ms, %.0f Mcells/s, %.4f ms per Mcell, mean dt %.5f s, "
          "%.0f sub-steps a 1/60 s tick, n=%zu\n",
          form, kind_name(kind), n, median, percentile(step_ms, 0.95), percentile(dt_ms, 0.5),
          percentile(faces_ms, 0.5), percentile(update_ms, 0.5), percentile(span_ms, 0.5),
          cells / (median * 1e3), median / (cells * 1e-6), mean_dt, substeps, step_ms.size());
    }
    state.set_items(u64{k_batch} * n * n);
  }
  device.destroy();
}

water_live::StepOptions native() { return {}; }
water_live::StepOptions fixed() {
  water_live::StepOptions o;
  o.ops = water_live::Ops::fixed;
  return o;
}
water_live::StepOptions stored() {
  water_live::StepOptions o;
  o.stored_faces = true;
  return o;
}

}  // namespace

ENGINE_BENCH_ARGS(water_live_lake, "water_live.step.lake", 512, 1024, 2048) {
  run_case(state, Kind::lake, native(), "step");
}
ENGINE_BENCH_ARGS(water_live_stream, "water_live.step.stream", 512, 1024, 2048) {
  run_case(state, Kind::stream, native(), "step");
}
ENGINE_BENCH_ARGS(water_live_beach, "water_live.step.beach", 512, 1024, 2048) {
  run_case(state, Kind::beach, native(), "step");
}
ENGINE_BENCH_ARGS(water_live_fixed_lake, "water_live.fixed.lake", 512, 1024, 2048) {
  run_case(state, Kind::lake, fixed(), "fixed");
}
ENGINE_BENCH_ARGS(water_live_fixed_stream, "water_live.fixed.stream", 512, 1024, 2048) {
  run_case(state, Kind::stream, fixed(), "fixed");
}
ENGINE_BENCH_ARGS(water_live_fixed_beach, "water_live.fixed.beach", 512, 1024, 2048) {
  run_case(state, Kind::beach, fixed(), "fixed");
}
ENGINE_BENCH_ARGS(water_live_faces_lake, "water_live.faces.lake", 512, 1024, 2048) {
  run_case(state, Kind::lake, stored(), "faces");
}
ENGINE_BENCH_ARGS(water_live_faces_stream, "water_live.faces.stream", 512, 1024, 2048) {
  run_case(state, Kind::stream, stored(), "faces");
}
ENGINE_BENCH_ARGS(water_live_faces_beach, "water_live.faces.beach", 512, 1024, 2048) {
  run_case(state, Kind::beach, stored(), "faces");
}
