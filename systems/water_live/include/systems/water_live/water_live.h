#pragma once

// water_live capability (ADR-0027; docs/plan/02-architecture.md §2.8, roadmap W173 and W175).
//
// The water's shallow-water step on the card: domain/water_state's reference scheme as a compute
// shader in single precision (shaders/water_step.slang), the smallest host code that runs it on a
// device, its device cases against the reference and its bench. It is the capability that will
// later run the water's state in a scene; as of 2026-11-25 it holds no scene, draws nothing, adds no pass to
// a frame and has no scheduler entry. docs/subsystems/water_live.md is the contract; the
// measurements are docs/experiments/water-step-on-the-card.md.
//
// Scaffolded by tools/new-capability.ps1. The module is additive by construction: nothing under
// core/ or foundation/, the render graph, the scheduler, or another capability is edited to add
// it, and ENGINE_WITH_WATER_LIVE=OFF (or ENGINE_MINIMAL=ON) removes it from the build
// entirely. The list is ADR-0027 decision 2; docs/subsystems/water_live.md says why each open box
// is open.
//
//   [-] schema types      none yet: a saved state is W173's design
//   [-] scheduler entry   none yet: a caller records steps into a graph it owns
//   [-] render passes     none in a frame: the step's passes go into a graph the caller submits
//   [-] derived data      none
//   [-] protocol methods  none
//   [-] tunables          none: run-time parameters arrive in water_state::Config
//   [-] LOD policy        none: one grid at one resolution; the water's LOD is W173's design
//   [x] determinism       k_determinism below: derived (ADR-0080 decision 11)
//   [x] zero cost unused  nothing is allocated or recorded until a DeviceStep is created
//   [x] docs, tests, size table
//   [x] bench             bench/water_live_bench.cpp

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>
#include <domain/water_state/water_state.h>

#include <span>
#include <string>

namespace engine::gfx {
class GpuTimer;
class RenderGraph;
}  // namespace engine::gfx

namespace engine::water_live {

/// Determinism stance (ADR-0010, ADR-0027): "derived". ADR-0080 decision 11: the state on the card
/// is `derived` until measurement shows it can be more, and nothing `hashed` reads it. On one
/// device a step is a function of the state's bytes, the inputs' bytes and the device, in a fixed
/// order with no atomic, so the same device gives the same bytes twice (a case); that is not the
/// same bytes on every device, which "hashed" (the state enters the sim hash and replays exactly
/// everywhere) would need. Across devices it may become so only with Ops::fixed, whose open
/// questions the experiment page lists (the denormal mode above all); not yet: the fixed form also
/// differs between two cards (docs/subsystems/water_live.md, "Two cards").
inline constexpr const char* k_determinism = "derived";

/// How division, the square root and the friction's cube root are taken. `native`: the device's
/// own (Vulkan allows them 2.5 ulp and more, so two devices may differ in a last bit). `fixed`:
/// built from addition, subtraction and multiplication alone, which are correctly rounded
/// everywhere contraction is forbidden — an estimate from the operand's bits and a fixed number of
/// Newton steps (shaders/water_step.slang, "The arithmetic").
enum class Ops : u8 { native = 0, fixed = 1 };

/// The shader's push constants (`WaterStepParams` in shaders/water_step.slang): the buffers by
/// device address and the grid's constants. Pinned in tests/size_table.cpp.
struct StepParams {
  u64 state_in = 0;   // three planes of nx * ny floats: h, hu, hv
  u64 state_out = 0;  // the same, written by the update
  u64 bed = 0;        // nx * ny floats, metres from the grid's datum
  u64 manning = 0;    // nx * ny floats
  u64 source = 0;     // nx * ny floats, m/s
  u64 partials = 0;   // one WaterPartial per workgroup
  u64 ledger = 0;     // one WaterLedger
  u32 nx = 0;
  u32 ny = 0;
  u32 groups_x = 0;
  u32 group_count = 0;
  u32 flags = 0;  // k_flag_* below
  f32 cell_m = 1.0f;
  f32 gravity = 9.81f;
  f32 cfl = 0.45f;
  f32 dry_depth_m = 1e-9f;
  f32 max_dt = 0.0f;        // 0: fold the ledger's sums in and choose no step (a flush)
  f32 source_scale = 1.0f;  // this step applies source * dt * source_scale (set_source's clock)
  u32 pad = 0;
  u64 faces_x = 0;  // with k_flag_stored_faces: (nx + 1) * ny faces of four floats
  u64 faces_y = 0;  // nx * (ny + 1) faces
};

inline constexpr u32 k_flag_west_open = 1u;
inline constexpr u32 k_flag_east_open = 2u;
inline constexpr u32 k_flag_south_open = 4u;
inline constexpr u32 k_flag_north_open = 8u;
inline constexpr u32 k_flag_manning = 16u;
inline constexpr u32 k_flag_source = 32u;
inline constexpr u32 k_flag_compensate = 64u;
inline constexpr u32 k_flag_stored_faces = 128u;

/// The state's planes on the card, each nx * ny floats: h, hu, hv, and the source's remainder
/// (kept only with set_source's `compensate`). The ground's store, the carried quantities and the
/// bed's mobile layer will be planes after these (docs/subsystems/water_live.md, "The state").
inline constexpr u32 k_state_planes = 4;

/// How a step is formed (docs/subsystems/water_live.md, "Two forms of the faces").
struct StepOptions {
  Ops ops = Ops::native;
  /// false: each cell computes its own four faces (one pass); true: every face is computed once
  /// into a buffer and the update reads it (three passes). The two give the same bytes.
  bool stored_faces = false;
};

/// A workgroup's record (`WaterPartial`): the largest |u| + c and |v| + c of the state it wrote
/// and the volumes its cells added to the ledger in that step.
struct WaterPartial {
  f32 speed_x = 0.0f;
  f32 speed_y = 0.0f;
  f32 source_m3 = 0.0f;
  f32 open_m3 = 0.0f;
  f32 roundoff_m3 = 0.0f;
  f32 pad[3] = {};
};

/// The ledger on the card (`WaterLedger`): each sum a pair, hi + lo, kept by TwoSum.
struct WaterLedger {
  f32 time_hi = 0.0f;
  f32 time_lo = 0.0f;
  f32 source_hi = 0.0f;
  f32 source_lo = 0.0f;
  f32 open_hi = 0.0f;
  f32 open_lo = 0.0f;
  f32 roundoff_hi = 0.0f;
  f32 roundoff_lo = 0.0f;
  f32 remaining_s = 0.0f;  // an advance's time left; +inf between advances
  f32 dt = 0.0f;           // the last step's length
  u32 steps = 0;           // steps of non-zero length
  u32 pad = 0;
};

/// The workgroup's side: 16 x 16 cells.
inline constexpr u32 k_group_side = 16;

/// The water's account as the card kept it, in the reference's terms (water_state::Ledger).
struct Ledger {
  f64 source_m3 = 0.0;
  f64 open_edge_out_m3 = 0.0;
  f64 roundoff_m3 = 0.0;
  f64 time_s = 0.0;
  f64 remaining_s = 0.0;
  u64 steps = 0;
};

/// One rectangle of water on a device: two copies of the state (each cell gathers from one and
/// writes the other), the bed, Manning's n and the sources as buffers the step reads at every step,
/// the workgroups' records and the ledger. The grid's positions are its own: cell (x, y) at index
/// y * nx + x, and the bed is a height from the grid's datum, so a lake at rest does not depend on
/// where in the world the grid stands. Every call that moves data submits and waits; `record`
/// is how a caller puts steps into a graph of its own instead.
class DeviceStep {
 public:
  DeviceStep() noexcept = default;
  ~DeviceStep();
  ENGINE_NON_COPYABLE(DeviceStep);

  /// The grid is `config`'s (nx, ny, cell_m, gravity, cfl, dry_depth_m, edges; carried and
  /// rows_per_job are not read: depth and the momenta only, for now). The state starts dry over a
  /// bed of zero, the ledger empty.
  bool create(const gfx::Device& device, const water_state::Config& config,
              const StepOptions& options, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }

  const water_state::Config& config() const noexcept { return config_; }
  const StepOptions& options() const noexcept { return options_; }
  u32 cell_count() const noexcept { return cells_; }
  u32 group_count() const noexcept { return groups_x_ * groups_y_; }

  /// Writes the state (each span nx * ny values, rounded to float) and empties the ledger.
  bool write_state(std::span<const f64> depth, std::span<const f64> momentum_x,
                   std::span<const f64> momentum_y, std::string* error = nullptr);
  /// Reads the state back, widened to double.
  bool read_state(std::span<f64> depth, std::span<f64> momentum_x, std::span<f64> momentum_y,
                  std::string* error = nullptr);
  /// The same as the card holds it: k_state_planes * nx * ny floats, plane by plane.
  bool read_state_bits(std::span<f32> planes, std::string* error = nullptr);
  /// The ledger, after folding in the last step's sums.
  bool read_ledger(Ledger& out, std::string* error = nullptr);

  /// The bed's heights in metres, any datum; the card holds bed_m - datum_m, taken in double and
  /// rounded once. Refused (false, nothing changed) unless it has nx * ny values.
  bool set_bed(std::span<const f64> bed_m, f64 datum_m, std::string* error = nullptr);
  /// Manning's n per cell; empty turns friction off.
  bool set_manning(std::span<const f64> manning, std::string* error = nullptr);
  /// Water added per area, m/s (negative takes); empty turns sources off. `every` > 1 applies it on
  /// a coarser clock: on one step in `every`, `every` times the step's amount. `compensate` keeps
  /// what rounding left out of each cell's depth (plane 3) and adds it to the next step's amount.
  bool set_source(std::span<const f64> source_m_per_s, u32 every = 1, bool compensate = false,
                  std::string* error = nullptr);

  /// Adds `count` steps of min(stable step, max_dt) seconds to `graph`: per step a pass choosing
  /// the step's length and the ledger's sums (one workgroup) and a pass stepping every cell. Once
  /// per graph: it imports the buffers. With `timer`, each pass is a zone ("water.dt",
  /// "water.update"); the caller brackets the frame. The state's copy flips with each step.
  void record(gfx::RenderGraph& graph, u32 count, f32 max_dt, gfx::GpuTimer* timer = nullptr);
  /// `count` steps, submitted and waited for.
  bool run_steps(u32 count, f32 max_dt, std::string* error = nullptr);
  /// Steps until `duration_s` has passed, the last step shortened to land on it; returns the
  /// steps taken (0 on an error). The time left is kept on the card in float. Refused (0) while a
  /// source is on a coarser clock (set_source's `every` > 1), which holds with run_steps only.
  u64 advance(f64 duration_s, std::string* error = nullptr);

  /// FNV-1a over the state's float bytes (every plane); 0 on an error.
  u64 state_hash(std::string* error = nullptr);

 private:
  bool upload(const gfx::BufferResource& target, const void* data, u64 bytes, std::string* error);
  bool set_remaining(f32 remaining_s, std::string* error);
  StepParams params(u32 from) const noexcept;

  const gfx::Device* device_ = nullptr;
  water_state::Config config_{};
  StepOptions options_{};
  u32 source_every_ = 1;
  u32 source_phase_ = 0;  // steps since the source last applied
  u32 cells_ = 0;
  u32 groups_x_ = 0;
  u32 groups_y_ = 0;
  u32 flags_ = 0;
  u32 current_ = 0;  // the state copy that holds the state
  gfx::ShaderModuleHandle module_;
  gfx::ComputePipeline prime_;
  gfx::ComputePipeline dt_;
  gfx::ComputePipeline update_;
  gfx::ComputePipeline faces_x_;
  gfx::ComputePipeline faces_y_;
  gfx::BufferResource state_[2];
  gfx::BufferResource bed_;
  gfx::BufferResource manning_;
  gfx::BufferResource source_;
  gfx::BufferResource partials_;
  gfx::BufferResource ledger_;
  gfx::BufferResource faces_x_buffer_;
  gfx::BufferResource faces_y_buffer_;
  gfx::BufferResource staging_;  // host visible: uploads and readbacks
};

/// FNV-1a over float bytes, in order: the hash the device cases compare.
u64 hash_floats(std::span<const f32> values) noexcept;

}  // namespace engine::water_live
