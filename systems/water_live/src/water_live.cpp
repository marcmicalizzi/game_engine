#include <systems/water_live/water_live.h>

#include <core/containers/vector.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/render_graph.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <shaders/water_step.spv.h>

// The host half of the step on the card (docs/subsystems/water_live.md, "Running it"). Every pass
// goes through a render graph, so no barrier is written by hand (gfx.md, "Render graph v0"); the
// graph here is the water's own, built, submitted and waited for by the calls that move data, or
// handed in by a caller through record().

namespace engine::water_live {

namespace {

constexpr u64 k_fnv_offset = 0xcbf29ce484222325ull;
constexpr u64 k_fnv_prime = 0x100000001b3ull;

constexpr gfx::BufferUsage k_state_usage = gfx::BufferUsage::Storage |
                                           gfx::BufferUsage::ShaderDeviceAddress |
                                           gfx::BufferUsage::TransferSrc |
                                           gfx::BufferUsage::TransferDst;

f32 infinity() noexcept { return std::numeric_limits<f32>::infinity(); }

// The buffers as one graph sees them.
struct Imports {
  gfx::RgBuffer state[2];
  gfx::RgBuffer bed, manning, source, partials, ledger, staging, faces_x, faces_y;
};

Imports import_all(gfx::RenderGraph& graph, const gfx::BufferResource (&state)[2],
                   const gfx::BufferResource& bed, const gfx::BufferResource& manning,
                   const gfx::BufferResource& source, const gfx::BufferResource& partials,
                   const gfx::BufferResource& ledger, const gfx::BufferResource& staging,
                   const gfx::BufferResource& faces_x, const gfx::BufferResource& faces_y) {
  // Every buffer was last written by an earlier submission that has been waited for; the graph is
  // told so, and orders its first use behind it.
  constexpr gfx::PipelineStage stage = gfx::PipelineStage::ComputeShader;
  constexpr gfx::MemoryAccess access = gfx::MemoryAccess::ShaderWrite;
  Imports im;
  im.state[0] = graph.import_buffer("water.state.0", state[0], stage, access);
  im.state[1] = graph.import_buffer("water.state.1", state[1], stage, access);
  im.bed = graph.import_buffer("water.bed", bed, stage, access);
  im.manning = graph.import_buffer("water.manning", manning, stage, access);
  im.source = graph.import_buffer("water.source", source, stage, access);
  im.partials = graph.import_buffer("water.partials", partials, stage, access);
  im.ledger = graph.import_buffer("water.ledger", ledger, stage, access);
  im.staging = graph.import_buffer("water.staging", staging, stage, access);
  im.faces_x = graph.import_buffer("water.faces.x", faces_x, stage, access);
  im.faces_y = graph.import_buffer("water.faces.y", faces_y, stage, access);
  return im;
}

template <class Build>
bool run_graph(const gfx::Device& device, Build&& build, std::string* error) {
  gfx::RenderGraph graph(device);
  build(graph);
  if (!graph.compile(error)) return false;
  return gfx::submit_immediate(device, [&](gfx::CommandList commands) { graph.execute(commands); },
                               error);
}

void add_copy(gfx::RenderGraph& graph, gfx::RgBuffer from, gfx::RgBuffer to, u64 bytes) {
  graph.add_pass(
      "water.copy", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(from, gfx::Access::TransferRead);
        b.write(to, gfx::Access::TransferWrite);
      },
      [from, to, bytes](gfx::CommandList commands, gfx::RenderGraph& g) {
        commands.copy_buffer(g.buffer(from).buffer, g.buffer(to).buffer,
                             gfx::BufferCopy{0, 0, bytes});
      });
}

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

}  // namespace

u64 hash_floats(std::span<const f32> values) noexcept {
  u64 hash = k_fnv_offset;
  for (const f32 v : values) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    for (u32 b = 0; b < 4; ++b) {
      hash ^= (bits >> (8 * b)) & 0xffu;
      hash *= k_fnv_prime;
    }
  }
  return hash;
}

DeviceStep::~DeviceStep() { destroy(); }

bool DeviceStep::create(const gfx::Device& device, const water_state::Config& config,
                        const StepOptions& options, std::string* error) {
  destroy();
  if (config.nx == 0 || config.ny == 0) return fail(error, "water_live: an empty grid");
  config_ = config;
  options_ = options;
  source_every_ = 1;
  source_phase_ = 0;
  cells_ = config.nx * config.ny;
  groups_x_ = (config.nx + k_group_side - 1) / k_group_side;
  groups_y_ = (config.ny + k_group_side - 1) / k_group_side;
  flags_ = 0;
  if (config.edges.west == water_state::Edge::open) flags_ |= k_flag_west_open;
  if (config.edges.east == water_state::Edge::open) flags_ |= k_flag_east_open;
  if (config.edges.south == water_state::Edge::open) flags_ |= k_flag_south_open;
  if (config.edges.north == water_state::Edge::open) flags_ |= k_flag_north_open;
  if (options.stored_faces) flags_ |= k_flag_stored_faces;
  current_ = 0;
  device_ = &device;

  module_ = gfx::create_shader_module(device, shaders::k_water_step_spirv,
                                      shaders::k_water_step_spirv_size, error);
  if (!module_.valid()) {
    destroy();
    return false;
  }
  gfx::Specialization spec;
  spec.set(0, options.ops == Ops::fixed ? 1u : 0u);
  const u32 push = sizeof(StepParams);
  if (!gfx::create_compute_pipeline(device, module_, "prime_main", {}, push, spec, prime_,
                                    error) ||
      !gfx::create_compute_pipeline(device, module_, "dt_main", {}, push, spec, dt_, error) ||
      !gfx::create_compute_pipeline(device, module_, "update_main", {}, push, spec, update_,
                                    error) ||
      !gfx::create_compute_pipeline(device, module_, "faces_x_main", {}, push, spec, faces_x_,
                                    error) ||
      !gfx::create_compute_pipeline(device, module_, "faces_y_main", {}, push, spec, faces_y_,
                                    error)) {
    destroy();
    return false;
  }
  const u64 plane = u64{cells_} * sizeof(f32);
  const u64 group_bytes = u64{groups_x_} * groups_y_ * sizeof(WaterPartial);
  const gfx::BufferUsage input_usage = gfx::BufferUsage::Storage |
                                       gfx::BufferUsage::ShaderDeviceAddress |
                                       gfx::BufferUsage::TransferDst;
  const u64 planes = u64{k_state_planes} * plane;
  // A face is four floats; without stored faces the buffers are a token 16 bytes.
  const u64 x_faces = options.stored_faces ? u64{config.nx + 1} * config.ny * 16 : 16;
  const u64 y_faces = options.stored_faces ? u64{config.nx} * (config.ny + 1) * 16 : 16;
  if (!gfx::create_buffer(device, planes, k_state_usage, false, state_[0], error) ||
      !gfx::create_buffer(device, planes, k_state_usage, false, state_[1], error) ||
      !gfx::create_buffer(device, x_faces, k_state_usage, false, faces_x_buffer_, error) ||
      !gfx::create_buffer(device, y_faces, k_state_usage, false, faces_y_buffer_, error) ||
      !gfx::create_buffer(device, plane, input_usage, false, bed_, error) ||
      !gfx::create_buffer(device, plane, input_usage, false, manning_, error) ||
      !gfx::create_buffer(device, plane, input_usage, false, source_, error) ||
      !gfx::create_buffer(device, group_bytes, k_state_usage, false, partials_, error) ||
      !gfx::create_buffer(device, sizeof(WaterLedger), k_state_usage, false, ledger_, error) ||
      !gfx::create_buffer(device, planes < 256 ? 256 : planes,
                          gfx::BufferUsage::TransferSrc | gfx::BufferUsage::TransferDst, true,
                          staging_, error)) {
    destroy();
    return false;
  }
  // A dry grid over a bed of zero, an empty ledger, and the records of that state.
  WaterLedger empty;
  empty.remaining_s = infinity();
  return run_graph(
      device,
      [&](gfx::RenderGraph& graph) {
        const Imports im = import_all(graph, state_, bed_, manning_, source_, partials_, ledger_,
                                      staging_, faces_x_buffer_, faces_y_buffer_);
        graph.add_pass(
            "water.clear", gfx::PassKind::Transfer,
            [&](gfx::PassBuilder& b) {
              b.write(im.state[0], gfx::Access::TransferWrite);
              b.write(im.state[1], gfx::Access::TransferWrite);
              b.write(im.bed, gfx::Access::TransferWrite);
              b.write(im.manning, gfx::Access::TransferWrite);
              b.write(im.source, gfx::Access::TransferWrite);
              b.write(im.ledger, gfx::Access::TransferWrite);
            },
            [im, empty](gfx::CommandList commands, gfx::RenderGraph& g) {
              commands.fill_buffer(g.buffer(im.state[0]).buffer, 0, gfx::k_whole_size, 0);
              commands.fill_buffer(g.buffer(im.state[1]).buffer, 0, gfx::k_whole_size, 0);
              commands.fill_buffer(g.buffer(im.bed).buffer, 0, gfx::k_whole_size, 0);
              commands.fill_buffer(g.buffer(im.manning).buffer, 0, gfx::k_whole_size, 0);
              commands.fill_buffer(g.buffer(im.source).buffer, 0, gfx::k_whole_size, 0);
              commands.update_buffer(g.buffer(im.ledger).buffer, 0, sizeof empty, &empty);
            });
        const StepParams p = params(0);
        graph.add_pass(
            "water.prime", gfx::PassKind::Compute,
            [&](gfx::PassBuilder& b) {
              b.read(im.state[0], gfx::Access::ComputeRead);
              b.write(im.partials, gfx::Access::ComputeWrite);
            },
            [this, p](gfx::CommandList commands, gfx::RenderGraph&) {
              commands.bind_pipeline(gfx::BindPoint::Compute, prime_.pipeline);
              commands.push_constants(prime_.layout, gfx::ShaderStage::Compute, 0, sizeof p, &p);
              commands.dispatch(groups_x_, groups_y_, 1);
            });
      },
      error);
}

void DeviceStep::destroy() noexcept {
  if (device_ == nullptr) return;
  for (gfx::BufferResource* b :
       {&state_[0], &state_[1], &bed_, &manning_, &source_, &partials_, &ledger_, &staging_,
        &faces_x_buffer_, &faces_y_buffer_}) {
    if (b->buffer.valid()) gfx::destroy_buffer(*device_, *b);
    *b = {};
  }
  if (faces_y_.pipeline.valid()) gfx::destroy_compute_pipeline(*device_, faces_y_);
  if (faces_x_.pipeline.valid()) gfx::destroy_compute_pipeline(*device_, faces_x_);
  if (update_.pipeline.valid()) gfx::destroy_compute_pipeline(*device_, update_);
  if (dt_.pipeline.valid()) gfx::destroy_compute_pipeline(*device_, dt_);
  if (prime_.pipeline.valid()) gfx::destroy_compute_pipeline(*device_, prime_);
  update_ = dt_ = prime_ = faces_x_ = faces_y_ = {};
  if (module_.valid()) gfx::destroy_shader_module(*device_, module_);
  module_ = {};
  device_ = nullptr;
}

StepParams DeviceStep::params(u32 from) const noexcept {
  StepParams p;
  p.state_in = state_[from].address;
  p.state_out = state_[from ^ 1u].address;
  p.bed = bed_.address;
  p.manning = manning_.address;
  p.source = source_.address;
  p.partials = partials_.address;
  p.ledger = ledger_.address;
  p.nx = config_.nx;
  p.ny = config_.ny;
  p.groups_x = groups_x_;
  p.group_count = groups_x_ * groups_y_;
  p.flags = flags_;
  p.cell_m = static_cast<f32>(config_.cell_m);
  p.gravity = static_cast<f32>(config_.gravity);
  p.cfl = static_cast<f32>(config_.cfl);
  p.dry_depth_m = static_cast<f32>(config_.dry_depth_m);
  p.max_dt = 0.0f;
  p.source_scale = 1.0f;
  p.faces_x = faces_x_buffer_.address;
  p.faces_y = faces_y_buffer_.address;
  return p;
}

bool DeviceStep::upload(const gfx::BufferResource& target, const void* data, u64 bytes,
                        std::string* error) {
  std::memcpy(staging_.mapped, data, static_cast<usize>(bytes));
  return run_graph(
      *device_,
      [&](gfx::RenderGraph& graph) {
        const gfx::RgBuffer from = graph.import_buffer("water.staging", staging_);
        const gfx::RgBuffer to = graph.import_buffer("water.input", target,
                                                     gfx::PipelineStage::ComputeShader,
                                                     gfx::MemoryAccess::ShaderRead);
        add_copy(graph, from, to, bytes);
      },
      error);
}

bool DeviceStep::write_state(std::span<const f64> depth, std::span<const f64> momentum_x,
                             std::span<const f64> momentum_y, std::string* error) {
  if (!valid()) return fail(error, "water_live: no device");
  if (depth.size() != cells_ || momentum_x.size() != cells_ || momentum_y.size() != cells_)
    return fail(error, "water_live: a state that does not fit the grid");
  auto* planes = static_cast<f32*>(staging_.mapped);
  for (u32 i = 0; i < cells_; ++i) {
    planes[i] = static_cast<f32>(depth[i]);
    planes[cells_ + i] = static_cast<f32>(momentum_x[i]);
    planes[2 * cells_ + i] = static_cast<f32>(momentum_y[i]);
    planes[3 * cells_ + i] = 0.0f;
  }
  source_phase_ = 0;
  WaterLedger empty;
  empty.remaining_s = infinity();
  const u64 bytes = u64{k_state_planes} * cells_ * sizeof(f32);
  return run_graph(
      *device_,
      [&](gfx::RenderGraph& graph) {
        const Imports im = import_all(graph, state_, bed_, manning_, source_, partials_, ledger_,
                                      staging_, faces_x_buffer_, faces_y_buffer_);
        const u32 cur = current_;
        add_copy(graph, im.staging, im.state[cur], bytes);
        graph.add_pass(
            "water.ledger.clear", gfx::PassKind::Transfer,
            [&](gfx::PassBuilder& b) { b.write(im.ledger, gfx::Access::TransferWrite); },
            [im, empty](gfx::CommandList commands, gfx::RenderGraph& g) {
              commands.update_buffer(g.buffer(im.ledger).buffer, 0, sizeof empty, &empty);
            });
        const StepParams p = params(cur);
        graph.add_pass(
            "water.prime", gfx::PassKind::Compute,
            [&](gfx::PassBuilder& b) {
              b.read(im.state[cur], gfx::Access::ComputeRead);
              b.write(im.partials, gfx::Access::ComputeWrite);
            },
            [this, p](gfx::CommandList commands, gfx::RenderGraph&) {
              commands.bind_pipeline(gfx::BindPoint::Compute, prime_.pipeline);
              commands.push_constants(prime_.layout, gfx::ShaderStage::Compute, 0, sizeof p, &p);
              commands.dispatch(groups_x_, groups_y_, 1);
            });
      },
      error);
}

bool DeviceStep::read_state_bits(std::span<f32> planes, std::string* error) {
  if (!valid()) return fail(error, "water_live: no device");
  if (planes.size() != usize{k_state_planes} * cells_)
    return fail(error, "water_live: planes of the wrong size");
  const u64 bytes = u64{k_state_planes} * cells_ * sizeof(f32);
  const bool ok = run_graph(
      *device_,
      [&](gfx::RenderGraph& graph) {
        const Imports im = import_all(graph, state_, bed_, manning_, source_, partials_, ledger_,
                                      staging_, faces_x_buffer_, faces_y_buffer_);
        add_copy(graph, im.state[current_], im.staging, bytes);
      },
      error);
  if (!ok) return false;
  std::memcpy(planes.data(), staging_.mapped, static_cast<usize>(bytes));
  return true;
}

bool DeviceStep::read_state(std::span<f64> depth, std::span<f64> momentum_x,
                            std::span<f64> momentum_y, std::string* error) {
  if (depth.size() != cells_ || momentum_x.size() != cells_ || momentum_y.size() != cells_)
    return fail(error, "water_live: a state that does not fit the grid");
  Vector<f32> planes;
  planes.resize(usize{k_state_planes} * cells_, 0.0f);
  if (!read_state_bits({planes.data(), planes.size()}, error)) return false;
  for (u32 i = 0; i < cells_; ++i) {
    depth[i] = static_cast<f64>(planes[i]);
    momentum_x[i] = static_cast<f64>(planes[cells_ + i]);
    momentum_y[i] = static_cast<f64>(planes[2 * cells_ + i]);
  }
  return true;
}

bool DeviceStep::read_ledger(Ledger& out, std::string* error) {
  if (!valid()) return fail(error, "water_live: no device");
  const bool ok = run_graph(
      *device_,
      [&](gfx::RenderGraph& graph) {
        const Imports im = import_all(graph, state_, bed_, manning_, source_, partials_, ledger_,
                                      staging_, faces_x_buffer_, faces_y_buffer_);
        const StepParams p = params(current_);  // max_dt 0: a flush
        graph.add_pass(
            "water.flush", gfx::PassKind::Compute,
            [&](gfx::PassBuilder& b) {
              b.write(im.partials, gfx::Access::ComputeReadWrite);
              b.write(im.ledger, gfx::Access::ComputeReadWrite);
            },
            [this, p](gfx::CommandList commands, gfx::RenderGraph&) {
              commands.bind_pipeline(gfx::BindPoint::Compute, dt_.pipeline);
              commands.push_constants(dt_.layout, gfx::ShaderStage::Compute, 0, sizeof p, &p);
              commands.dispatch(1, 1, 1);
            });
        add_copy(graph, im.ledger, im.staging, sizeof(WaterLedger));
      },
      error);
  if (!ok) return false;
  WaterLedger l;
  std::memcpy(&l, staging_.mapped, sizeof l);
  out.source_m3 = static_cast<f64>(l.source_hi) + static_cast<f64>(l.source_lo);
  out.open_edge_out_m3 = static_cast<f64>(l.open_hi) + static_cast<f64>(l.open_lo);
  out.roundoff_m3 = static_cast<f64>(l.roundoff_hi) + static_cast<f64>(l.roundoff_lo);
  out.time_s = static_cast<f64>(l.time_hi) + static_cast<f64>(l.time_lo);
  out.remaining_s = static_cast<f64>(l.remaining_s);
  out.steps = l.steps;
  return true;
}

bool DeviceStep::set_bed(std::span<const f64> bed_m, f64 datum_m, std::string* error) {
  if (!valid()) return fail(error, "water_live: no device");
  if (bed_m.size() != cells_) return fail(error, "water_live: a bed that does not fit the grid");
  Vector<f32> local;
  local.resize(cells_, 0.0f);
  for (u32 i = 0; i < cells_; ++i)
    local[i] = static_cast<f32>(bed_m[i] - datum_m);
  return upload(bed_, local.data(), u64{cells_} * sizeof(f32), error);
}

bool DeviceStep::set_manning(std::span<const f64> manning, std::string* error) {
  if (!valid()) return fail(error, "water_live: no device");
  if (manning.empty()) {
    flags_ &= ~k_flag_manning;
    return true;
  }
  if (manning.size() != cells_) return fail(error, "water_live: a Manning's n that does not fit");
  Vector<f32> values;
  values.resize(cells_, 0.0f);
  for (u32 i = 0; i < cells_; ++i)
    values[i] = static_cast<f32>(manning[i]);
  if (!upload(manning_, values.data(), u64{cells_} * sizeof(f32), error)) return false;
  flags_ |= k_flag_manning;
  return true;
}

bool DeviceStep::set_source(std::span<const f64> source_m_per_s, u32 every, bool compensate,
                            std::string* error) {
  if (!valid()) return fail(error, "water_live: no device");
  source_every_ = every == 0 ? 1 : every;
  source_phase_ = 0;
  if (compensate)
    flags_ |= k_flag_compensate;
  else
    flags_ &= ~k_flag_compensate;
  if (source_m_per_s.empty()) {
    flags_ &= ~k_flag_source;
    return true;
  }
  if (source_m_per_s.size() != cells_) return fail(error, "water_live: a source that does not fit");
  Vector<f32> values;
  values.resize(cells_, 0.0f);
  for (u32 i = 0; i < cells_; ++i)
    values[i] = static_cast<f32>(source_m_per_s[i]);
  if (!upload(source_, values.data(), u64{cells_} * sizeof(f32), error)) return false;
  flags_ |= k_flag_source;
  return true;
}

void DeviceStep::record(gfx::RenderGraph& graph, u32 count, f32 max_dt, gfx::GpuTimer* timer) {
  const Imports im =
      import_all(graph, state_, bed_, manning_, source_, partials_, ledger_, staging_,
                 faces_x_buffer_, faces_y_buffer_);
  for (u32 s = 0; s < count; ++s) {
    const u32 from = current_;
    const u32 to = from ^ 1u;
    StepParams p = params(from);
    p.max_dt = max_dt;
    // The source's clock: on one step in `every`, `every` times the step's amount.
    if ((flags_ & k_flag_source) != 0u && source_every_ > 1) {
      source_phase_ += 1;
      if (source_phase_ == source_every_) {
        source_phase_ = 0;
        p.source_scale = static_cast<f32>(source_every_);
      } else {
        p.flags &= ~k_flag_source;
      }
    }
    graph.add_pass(
        "water.dt", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.write(im.partials, gfx::Access::ComputeReadWrite);
          b.write(im.ledger, gfx::Access::ComputeReadWrite);
        },
        [this, p, timer](gfx::CommandList commands, gfx::RenderGraph&) {
          if (timer != nullptr) timer->begin(commands, "water.dt");
          commands.bind_pipeline(gfx::BindPoint::Compute, dt_.pipeline);
          commands.push_constants(dt_.layout, gfx::ShaderStage::Compute, 0, sizeof p, &p);
          commands.dispatch(1, 1, 1);
          if (timer != nullptr) timer->end(commands);
        });
    const bool stored = (flags_ & k_flag_stored_faces) != 0u;
    if (stored) {
      const u32 gx = (config_.nx + 1 + k_group_side - 1) / k_group_side;
      const u32 gy = (config_.ny + 1 + k_group_side - 1) / k_group_side;
      graph.add_pass(
          "water.faces", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(im.state[from], gfx::Access::ComputeRead);
            b.read(im.bed, gfx::Access::ComputeRead);
            b.read(im.ledger, gfx::Access::ComputeRead);
            b.write(im.faces_x, gfx::Access::ComputeWrite);
            b.write(im.faces_y, gfx::Access::ComputeWrite);
          },
          [this, p, timer, gx, gy](gfx::CommandList commands, gfx::RenderGraph&) {
            if (timer != nullptr) timer->begin(commands, "water.faces");
            commands.bind_pipeline(gfx::BindPoint::Compute, faces_x_.pipeline);
            commands.push_constants(faces_x_.layout, gfx::ShaderStage::Compute, 0, sizeof p, &p);
            commands.dispatch(gx, groups_y_, 1);
            commands.bind_pipeline(gfx::BindPoint::Compute, faces_y_.pipeline);
            commands.push_constants(faces_y_.layout, gfx::ShaderStage::Compute, 0, sizeof p, &p);
            commands.dispatch(groups_x_, gy, 1);
            if (timer != nullptr) timer->end(commands);
          });
    }
    graph.add_pass(
        "water.update", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          if (stored) {
            b.read(im.faces_x, gfx::Access::ComputeRead);
            b.read(im.faces_y, gfx::Access::ComputeRead);
          }
          b.read(im.state[from], gfx::Access::ComputeRead);
          b.read(im.bed, gfx::Access::ComputeRead);
          b.read(im.manning, gfx::Access::ComputeRead);
          b.read(im.source, gfx::Access::ComputeRead);
          b.read(im.ledger, gfx::Access::ComputeRead);
          b.write(im.state[to], gfx::Access::ComputeWrite);
          b.write(im.partials, gfx::Access::ComputeWrite);
        },
        [this, p, timer](gfx::CommandList commands, gfx::RenderGraph&) {
          if (timer != nullptr) timer->begin(commands, "water.update");
          commands.bind_pipeline(gfx::BindPoint::Compute, update_.pipeline);
          commands.push_constants(update_.layout, gfx::ShaderStage::Compute, 0, sizeof p, &p);
          commands.dispatch(groups_x_, groups_y_, 1);
          if (timer != nullptr) timer->end(commands);
        });
    current_ = to;
  }
}

bool DeviceStep::run_steps(u32 count, f32 max_dt, std::string* error) {
  if (!valid()) return fail(error, "water_live: no device");
  return run_graph(
      *device_, [&](gfx::RenderGraph& graph) { record(graph, count, max_dt); }, error);
}

bool DeviceStep::set_remaining(f32 remaining_s, std::string* error) {
  return run_graph(
      *device_,
      [&](gfx::RenderGraph& graph) {
        const gfx::RgBuffer ledger = graph.import_buffer(
            "water.ledger", ledger_, gfx::PipelineStage::ComputeShader,
            gfx::MemoryAccess::ShaderWrite);
        graph.add_pass(
            "water.remaining", gfx::PassKind::Transfer,
            [&](gfx::PassBuilder& b) { b.write(ledger, gfx::Access::TransferWrite); },
            [ledger, remaining_s](gfx::CommandList commands, gfx::RenderGraph& g) {
              commands.update_buffer(g.buffer(ledger).buffer, offsetof(WaterLedger, remaining_s),
                                     sizeof remaining_s, &remaining_s);
            });
      },
      error);
}

u64 DeviceStep::advance(f64 duration_s, std::string* error) {
  if (!valid()) {
    fail(error, "water_live: no device");
    return 0;
  }
  if (!(duration_s > 0.0)) return 0;
  // The steps a batch records; those after the time runs out choose a length of zero and leave the
  // state as it is (update_main copies it).
  constexpr u32 k_batch = 64;
  constexpr u32 k_most_batches = 1u << 16;
  Ledger start;
  if (!read_ledger(start, error)) return 0;
  if (!set_remaining(static_cast<f32>(duration_s), error)) return 0;
  bool done = false;
  for (u32 batch = 0; batch < k_most_batches && !done; ++batch) {
    const bool ok = run_graph(
        *device_,
        [&](gfx::RenderGraph& graph) { record(graph, k_batch, infinity()); },
        error);
    if (!ok) return 0;
    Ledger now;
    if (!read_ledger(now, error)) return 0;
    done = !(now.remaining_s > 0.0);
  }
  Ledger end;
  if (!read_ledger(end, error) || !set_remaining(infinity(), error)) return 0;
  if (!done) {
    fail(error, "water_live: the advance did not finish");
    return 0;
  }
  return end.steps - start.steps;
}

u64 DeviceStep::state_hash(std::string* error) {
  Vector<f32> planes;
  planes.resize(usize{k_state_planes} * cells_, 0.0f);
  if (!read_state_bits({planes.data(), planes.size()}, error)) return 0;
  return hash_floats({planes.data(), planes.size()});
}

}  // namespace engine::water_live
