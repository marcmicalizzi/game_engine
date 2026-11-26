#pragma once

// water_state capability (ADR-0027; docs/plan/02-architecture.md §2.8, roadmap W173 and W184).
//
// The shallow-water equations as a finite-volume step on a regular grid, on the CPU, in double:
// the reference every later form of the engine's simulated water (the card's float step, W175,
// and whatever is cut down from it) is held to. It owns one rectangle of cells and their state —
// depth, the two momenta, the ground's store, and a few carried quantities — and steps it over a
// bed the caller hands in at every step. It draws nothing, knows no world position, and depends on
// no renderer, no gfx, no ECS and no other capability. docs/subsystems/water_state.md is the
// contract: the scheme with the paper and equation each part comes from, the state's layout, the
// cases and their bounds.
//
// Scaffolded by tools/new-capability.ps1. The module is additive by construction: nothing under
// core/ or foundation/, the render graph, the scheduler, or another capability is edited to add
// it, and ENGINE_WITH_WATER_STATE=OFF (or ENGINE_MINIMAL=ON) removes it from the build
// entirely. The list is ADR-0027 decision 2; docs/subsystems/water_state.md says why each open box
// is open.
//
//   [-] schema types      none yet: the reference is driven by its cases; a saved state is W173's
//   design
//   [-] scheduler entry   none yet: a caller steps a Solver; which tick phase owns it is the
//   design's
//   [-] render passes     none: the reference draws nothing
//   [-] derived data      none
//   [-] protocol methods  none
//   [-] tunables          none: run-time parameters arrive in Config, filled by whoever owns one
//   [-] LOD policy        none: the reference has one resolution; the water's LOD is W173's design
//   [x] determinism       k_determinism below; the same bytes at any thread count and toolchain
//   [x] zero cost unused  a scene with no Solver allocates and runs nothing (plan 11 §11.10)
//   [x] docs, tests, size table
//   [x] bench             bench/water_state_bench.cpp

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>

#include <array>
#include <span>

namespace engine::jobs {
class JobSystem;
}

namespace engine::water_state {

/// Determinism stance (ADR-0010): "hashed". A step is a function of the state's bytes, the
/// inputs' bytes and the step length, the same bytes at any thread count and on MSVC, GCC and
/// Clang: only + - * / and sqrt decide anything, and the few powers the laws need come from
/// core/math/deterministic.h, never from the C library.
inline constexpr const char* k_determinism = "hashed";

/// The most carried quantities a cell holds (foam, suspended sediment, ...). The step moves each by
/// the same mass fluxes and does not know which is which.
inline constexpr u32 k_max_carried = 4;

/// What an edge of the rectangle does. `closed` is a wall (a mirrored ghost cell: no water passes);
/// `open` is a transmissive edge (a ghost that copies the edge cell), and whatever crosses it is
/// counted in Ledger::open_edge_out_m3.
enum class Edge : u8 { closed = 0, open = 1 };

struct Edges {
  Edge west = Edge::closed;   // x = 0
  Edge east = Edge::closed;   // x = nx * cell_m
  Edge south = Edge::closed;  // y = 0
  Edge north = Edge::closed;  // y = ny * cell_m
};

/// What one carried quantity does besides riding the mass fluxes. The step treats every quantity
/// alike; which is foam and which is sediment is only these numbers and the inputs it is given.
struct CarriedConfig {
  /// e-folding time of an exponential decay, s; 0: none.
  f64 decay_s = 0.0;
  /// Amount made per joule of mechanical energy the step dissipates in a cell (the scheme's loss at
  /// a bore or a jump and the bed friction's work), per area; 0: none. The foam's source.
  f64 per_joule = 0.0;
  /// The dissipation feeds the quantity only where the water is at least this fast, m/s.
  f64 min_speed_m_per_s = 0.0;
};

/// Everything fixed for a solver's life. Cells are square, `cell_m` on a side; cell (x, y) is at
/// index y * nx + x, x along +x (east), y along +y (north).
struct Config {
  u32 nx = 0;
  u32 ny = 0;
  f64 cell_m = 1.0;
  f64 gravity = 9.81;  // m/s^2
  /// The step is cfl * cell_m / (max(|u| + c) + max(|v| + c)), c = sqrt(g h). Depth stays
  /// non-negative for cfl <= 0.5 (docs/subsystems/water_state.md, "The step's length").
  f64 cfl = 0.45;
  /// Below this depth a cell's velocity is taken as zero and its momenta are set to zero after a
  /// step (Bollermann et al. 2014, section 2: 1e-9).
  f64 dry_depth_m = 1e-9;
  f64 water_density = 1000.0;  // kg/m^3, for the bed stress and the dissipation
  u32 carried = 0;             // carried quantities in use, at most k_max_carried
  std::array<CarriedConfig, k_max_carried> carried_config{};
  Edges edges{};
  u32 rows_per_job = 16;  // rows a job takes when a JobSystem is given; does not change the bytes
};

/// The inputs of one step, read and never kept: a step may find any of them changed.
/// Every non-empty span has nx * ny values in the state's index order.
struct Inputs {
  std::span<const f64> bed_m;           // the bed's height, metres; required
  std::span<const f64> manning;         // Manning's n, s m^-1/3; empty: no friction
  std::span<const f64> source_m_per_s;  // water added per area (rain, a spring); negative takes
  /// The ground's store, by Green and Ampt's rate (docs/subsystems/water_state.md, "The ground's
  /// store"): the saturated conductivity K (m/s), the suction at the wetting front times the
  /// moisture deficit (m), and the store's capacity, the soil's depth times its deficit (m). All
  /// three or none; none: the bed takes nothing.
  std::span<const f64> ground_conductivity_m_per_s;
  std::span<const f64> ground_suction_m;
  std::span<const f64> ground_capacity_m;
  /// Per carried quantity: an outside source (amount per area per second), and the exchange with
  /// the bed (positive: lifted into the water; negative: settling out of it, taken only as far as
  /// the water holds the quantity). Empty: none. The exchange's law is a later row's (W178): it
  /// fills this field from the state before each step.
  std::array<std::span<const f64>, k_max_carried> carried_source{};
  std::array<std::span<const f64>, k_max_carried> carried_exchange{};
};

/// The water's account since construction or reset_ledger(), in cubic metres, summed in a fixed
/// order. surface + ground + open_edge_out == initial + source, to round-off; roundoff_m3 is what
/// clamping a depth that came out a rounding error below zero added, reported so it can be held.
/// One carried quantity's account, in its amount (its unit times m^3), summed like the water's:
/// held + open_edge_out + decayed == initial + source + from_dissipation + exchange.
struct CarriedLedger {
  f64 source = 0.0;            // from Inputs::carried_source
  f64 from_dissipation = 0.0;  // made by the step's dissipation (CarriedConfig::per_joule)
  f64 exchange = 0.0;          // net from the bed (settling counts negative)
  f64 decayed = 0.0;
  f64 open_edge_out = 0.0;
};

struct Ledger {
  f64 source_m3 = 0.0;         // what the sources gave (a sink's take counts negative)
  f64 open_edge_out_m3 = 0.0;  // net out through open edges (in counts negative)
  f64 ground_m3 = 0.0;         // what the ground's store took
  f64 roundoff_m3 = 0.0;       // depth added by clamping a negative round-off to zero
  f64 scheme_loss_j = 0.0;     // energy the scheme dissipated, signed (docs: "The dissipation")
  f64 friction_work_j = 0.0;   // energy the bed friction took
  f64 time_s = 0.0;
  u64 steps = 0;
  std::array<CarriedLedger, k_max_carried> carried{};
};

/// One rectangle of water. Construction allocates the state, the outputs and every scratch array
/// once; step() allocates nothing.
///
/// The state (structure of arrays, nx * ny doubles each, the index order above):
///   depth          h, m
///   momentum_x/y   hu, hv, m^2/s
///   ground         water the ground's store holds, m (depth of water per area)
///   carried(k)     amount of carried quantity k per area (its unit times m); its concentration
///                  carried(k) / h is what the step keeps bounded
/// The outputs of the last step, per cell:
///   speed          |u|, m/s
///   bed_stress_x/y the stress the flow puts on the bed by the friction law, Pa
///   dissipation    mechanical energy lost per area, W/m^2: the scheme's loss (at a bore, a jump)
///                  where positive, plus the bed friction's work
class Solver {
 public:
  explicit Solver(const Config& config);
  ENGINE_NON_COPYABLE(Solver);
  Solver(Solver&&) noexcept = default;
  Solver& operator=(Solver&&) noexcept = default;

  const Config& config() const noexcept { return config_; }
  u32 cell_count() const noexcept { return cells_; }
  u32 index(u32 x, u32 y) const noexcept { return y * config_.nx + x; }
  f64 cell_area_m2() const noexcept { return config_.cell_m * config_.cell_m; }

  std::span<f64> depth() noexcept { return {h_.data(), cells_}; }
  std::span<f64> momentum_x() noexcept { return {hu_.data(), cells_}; }
  std::span<f64> momentum_y() noexcept { return {hv_.data(), cells_}; }
  std::span<f64> ground() noexcept { return {ground_.data(), cells_}; }
  std::span<f64> carried(u32 k) noexcept { return {carried_.data() + usize{k} * cells_, cells_}; }
  std::span<const f64> depth() const noexcept { return {h_.data(), cells_}; }
  std::span<const f64> momentum_x() const noexcept { return {hu_.data(), cells_}; }
  std::span<const f64> momentum_y() const noexcept { return {hv_.data(), cells_}; }
  std::span<const f64> ground() const noexcept { return {ground_.data(), cells_}; }
  std::span<const f64> carried(u32 k) const noexcept {
    return {carried_.data() + usize{k} * cells_, cells_};
  }

  std::span<const f64> speed() const noexcept { return {speed_.data(), cells_}; }
  std::span<const f64> bed_stress_x() const noexcept { return {tau_x_.data(), cells_}; }
  std::span<const f64> bed_stress_y() const noexcept { return {tau_y_.data(), cells_}; }
  std::span<const f64> dissipation() const noexcept { return {dissipation_.data(), cells_}; }

  const Ledger& ledger() const noexcept { return ledger_; }
  void reset_ledger() noexcept { ledger_ = {}; }

  /// The step the stability limit allows for the current state, seconds (infinite when no cell
  /// holds water). Reads the state only.
  f64 stable_step(jobs::JobSystem* jobs = nullptr);

  /// One explicit step of min(stable_step(), max_dt) seconds; returns the length taken, or 0 when
  /// the inputs do not fit the grid (nothing is changed then).
  f64 step(const Inputs& inputs, f64 max_dt, jobs::JobSystem* jobs = nullptr);

  /// Steps until exactly `duration_s` has passed, the last step shortened to land on it. Returns
  /// the number of steps, or 0 when the inputs do not fit the grid.
  u64 advance(const Inputs& inputs, f64 duration_s, jobs::JobSystem* jobs = nullptr);

  /// Surface water, m^3, summed in index order.
  f64 surface_volume_m3() const noexcept;
  /// The ground's store, m^3, summed in index order.
  f64 ground_volume_m3() const noexcept;
  /// Carried quantity k's amount in the water, summed in index order (its unit times m^3).
  f64 carried_total(u32 k) const noexcept;
  /// FNV-1a over the state's bytes: depth, the momenta, the ground, the carried quantities.
  u64 state_hash() const noexcept;

 private:
  /// One row's sums, added over rows in row order after a step.
  struct RowSums {
    f64 source = 0.0;
    f64 open = 0.0;
    f64 roundoff = 0.0;
    f64 ground = 0.0;
    f64 scheme_loss = 0.0;
    f64 friction = 0.0;
    std::array<CarriedLedger, k_max_carried> carried{};
  };
  /// One direction's face arrays (see the scratch below).
  struct Faces {
    f64* h;
    f64* nl;
    f64* nr;
    f64* t;
    f64* g;
    f64* c;
    u32 count;
  };

  bool inputs_fit(const Inputs& inputs) const noexcept;
  void face(const f64* bed, u32 l, u32 r, f64 ln, f64 lt, f64 rn, f64 rt, const Faces& out,
            u32 index) noexcept;
  template <class F>
  void for_rows(jobs::JobSystem* jobs, u32 rows, F&& body);
  void velocity_rows(u32 begin, u32 end) noexcept;
  void x_face_rows(const Inputs& in, u32 begin, u32 end) noexcept;
  void y_face_rows(const Inputs& in, u32 begin, u32 end) noexcept;
  void update_rows(const Inputs& in, f64 dt, u32 begin, u32 end) noexcept;

  Config config_{};
  u32 cells_ = 0;
  Ledger ledger_{};

  // The state.
  Vector<f64> h_, hu_, hv_, ground_, carried_;
  // The outputs.
  Vector<f64> speed_, tau_x_, tau_y_, dissipation_;
  // Scratch, allocated once: velocities, per-row maxima and sums, and the face fluxes. An x face
  // row has nx + 1 faces (face f west of cell f); there are ny + 1 rows of y faces (row r south of
  // cell row r). Per face: the mass flux, the normal momentum's flux less the reconstructed
  // pressure on each side (the hydrostatic reconstruction's two corrections, Audusse et al.), the
  // transverse momentum's flux, the energy's flux (for the dissipation), and each carried
  // quantity's flux.
  Vector<f64> u_, v_;
  Vector<f64> row_a_, row_b_;
  Vector<RowSums> rows_;
  Vector<f64> fx_h_, fx_nl_, fx_nr_, fx_t_, fx_g_, fx_c_;
  Vector<f64> fy_h_, fy_nl_, fy_nr_, fy_t_, fy_g_, fy_c_;
};

}  // namespace engine::water_state
