// The reference's cases on the card (docs/subsystems/water_live.md, "Invariants"): each runs the
// same case through domain/water_state's double-precision reference and through the step on the
// device, and holds the card to the reference with a bound stated beside it and the reason for it.
//
// The bounds come from one argument. Single precision's unit roundoff is 2^-24 against double's
// 2^-53, 2^29 times larger. A first-order monotone step under its CFL limit does not amplify a
// perturbation (it is contractive in L1), so what single precision adds is at most about one ulp
// of relative error per step, and after S steps at most S x 2^-23 of the water involved; where a
// reference bound is a count of double's ulps it is scaled by 2^29.
#include <domain/gfx/device.h>
#include <domain/water_state/water_state.h>
#include <systems/water_live/water_live.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace engine;
using water_live::DeviceStep;
using water_live::Ops;
using water_live::StepOptions;
using water_state::Config;
using water_state::Edge;
using water_state::Inputs;
using water_state::Solver;

namespace {

constexpr f64 k_g = 9.81;
constexpr f64 k_ulp_f32 = 1.0 / 16777216.0;  // 2^-24
constexpr f64 k_precision_ratio = 536870912.0;  // 2^29
constexpr f32 k_no_limit = std::numeric_limits<f32>::infinity();

std::string ops_name(Ops ops) { return ops == Ops::fixed ? "fixed" : "native"; }

// A device for one case; a machine with none skips it.
bool open_device(gfx::Device& device) {
  std::string error;
  if (device.create(gfx::DeviceOptions{}, &error)) return true;
  MESSAGE("no Vulkan device: " << error);
  return false;
}

u32 mix(u32 x, u32 y) {
  u32 h = x * 0x9e3779b1u ^ (y + 0x7f4a7c15u) * 0x85ebca77u;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  return h;
}

// ---- the dam breaks' closed forms (the reference's, SWASHES 4.1.1 and 4.1.2) ------------------

constexpr f64 k_dam_length = 10.0;
constexpr f64 k_dam_x0 = 5.0;
constexpr f64 k_dam_hl = 0.005;
constexpr f64 k_dam_time = 6.0;

f64 stoker_cm(f64 hl, f64 hr) {
  const f64 sl = std::sqrt(k_g * hl);
  auto f = [&](f64 c) {
    const f64 a = sl - c;
    const f64 b = c * c - k_g * hr;
    return -8.0 * k_g * hr * c * c * a * a + b * b * (c * c + k_g * hr);
  };
  f64 lo = std::sqrt(k_g * hr);
  f64 hi = sl;
  const f64 flo = f(lo);
  for (int i = 0; i < 200; ++i) {
    const f64 mid = 0.5 * (lo + hi);
    if ((f(mid) > 0.0) == (flo > 0.0))
      lo = mid;
    else
      hi = mid;
  }
  return 0.5 * (lo + hi);
}

f64 dam_depth(f64 x, f64 t, f64 hl, f64 hr) {
  const f64 sl = std::sqrt(k_g * hl);
  const f64 xa = k_dam_x0 - t * sl;
  if (x <= xa) return hl;
  const f64 fan =
      4.0 / (9.0 * k_g) * (sl - (x - k_dam_x0) / (2.0 * t)) * (sl - (x - k_dam_x0) / (2.0 * t));
  if (hr == 0.0) {
    const f64 xb = k_dam_x0 + 2.0 * t * sl;
    return x <= xb ? fan : 0.0;
  }
  const f64 cm = stoker_cm(hl, hr);
  const f64 xb = k_dam_x0 + t * (2.0 * sl - 3.0 * cm);
  const f64 xc = k_dam_x0 + t * 2.0 * cm * cm * (sl - cm) / (cm * cm - k_g * hr);
  if (x <= xb) return fan;
  if (x <= xc) return cm * cm / k_g;
  return hr;
}

struct DamResult {
  f64 l1_relative = 0.0;   // the card against the closed form
  f64 l1_vs_reference = 0.0;  // the card against the reference, sum |h - h_ref| / sum h_ref
  f64 linf_vs_reference = 0.0;  // the largest |h - h_ref| of a cell, m
  f64 min_depth = 0.0;
  f64 volume_error = 0.0;
  u64 steps = 0;
};

DamResult run_dam(gfx::Device& device, Ops ops, u32 nx, f64 hr) {
  Config config;
  config.nx = nx;
  config.ny = 1;
  config.cell_m = k_dam_length / nx;
  config.gravity = k_g;
  std::vector<f64> bed(nx, 0.0), h(nx), zero(nx, 0.0);
  for (u32 x = 0; x < nx; ++x)
    h[x] = (x + 0.5) * config.cell_m <= k_dam_x0 ? k_dam_hl : hr;
  Solver reference(config);
  std::copy(h.begin(), h.end(), reference.depth().begin());
  Inputs in;
  in.bed_m = bed;
  reference.advance(in, k_dam_time);

  DeviceStep step;
  std::string error;
  StepOptions options;
  options.ops = ops;
  REQUIRE_MESSAGE(step.create(device, config, options, &error), error);
  REQUIRE_MESSAGE(step.set_bed(bed, 0.0, &error), error);
  REQUIRE_MESSAGE(step.write_state(h, zero, zero, &error), error);
  f64 before = 0.0;
  for (u32 x = 0; x < nx; ++x)
    before += static_cast<f64>(static_cast<f32>(h[x]));
  DamResult r;
  r.steps = step.advance(k_dam_time, &error);
  REQUIRE_MESSAGE(r.steps > 0, error);
  std::vector<f64> got(nx), hu(nx), hv(nx);
  REQUIRE_MESSAGE(step.read_state(got, hu, hv, &error), error);
  f64 err = 0.0, norm = 0.0, diff = 0.0, ref_norm = 0.0, after = 0.0;
  r.min_depth = got[0];
  for (u32 x = 0; x < nx; ++x) {
    const f64 exact = dam_depth((x + 0.5) * config.cell_m, k_dam_time, k_dam_hl, hr);
    err += std::fabs(got[x] - exact);
    norm += exact;
    diff += std::fabs(got[x] - reference.depth()[x]);
    r.linf_vs_reference = std::max(r.linf_vs_reference, std::fabs(got[x] - reference.depth()[x]));
    ref_norm += reference.depth()[x];
    r.min_depth = std::min(r.min_depth, got[x]);
    after += got[x];
  }
  r.l1_relative = err / norm;
  r.l1_vs_reference = diff / ref_norm;
  r.volume_error = std::fabs(after - before) / before;
  return r;
}

void check_dam(gfx::Device& device, f64 hr, f64 finest_bound, const std::string& name) {
  for (const Ops ops : {Ops::native, Ops::fixed}) {
    CAPTURE(ops_name(ops));
    const DamResult r200 = run_dam(device, ops, 200, hr);
    const DamResult r400 = run_dam(device, ops, 400, hr);
    const DamResult r800 = run_dam(device, ops, 800, hr);
    const f64 order_a = std::log2(r200.l1_relative / r400.l1_relative);
    const f64 order_b = std::log2(r400.l1_relative / r800.l1_relative);
    MESSAGE(name << " on the card (" << ops_name(ops) << "): L1 " << r200.l1_relative << ", "
                 << r400.l1_relative << ", " << r800.l1_relative << "; orders " << order_a << ", "
                 << order_b << "; against the reference " << r200.l1_vs_reference << ", "
                 << r400.l1_vs_reference << ", " << r800.l1_vs_reference << "; largest cell "
                 << r200.linf_vs_reference << ", " << r400.linf_vs_reference << ", "
                 << r800.linf_vs_reference << " m; volume "
                 << r800.volume_error << " over " << r800.steps << " steps");
    // The reference's own bounds on the scheme.
    CHECK(order_a >= 0.5);
    CHECK(order_b >= 0.5);
    CHECK(r800.l1_relative <= finest_bound);
    CHECK(r800.min_depth >= 0.0);
    // Single precision's: S x 2^-23 of the water against the reference, S x 2^-24 of the volume
    // (each step's update rounds each cell's depth by half an ulp at most).
    // And cell by cell, where an L1 over all the water could hide a front placed a cell off: eight
    // sqrt(S) ulps of the deepest water (h_l, ulp 2^-31 m), rounding that walks at random.
    const f64 deep_ulp = 1.0 / 2147483648.0;
    for (const DamResult* r : {&r200, &r400, &r800}) {
      CHECK(r->linf_vs_reference <= 8.0 * std::sqrt(static_cast<f64>(r->steps)) * deep_ulp);
      CHECK(r->l1_vs_reference <= static_cast<f64>(r->steps) * 2.0 * k_ulp_f32);
      CHECK(r->volume_error <= static_cast<f64>(r->steps) * k_ulp_f32);
    }
  }
}

// ---- the lakes at rest ------------------------------------------------------------------------

struct Lake {
  Config config;
  std::vector<f64> bed, h;
};

Lake exact_lake() {
  Lake l;
  l.config.nx = 64;
  l.config.ny = 48;
  l.config.cell_m = 0.5;
  const u32 cells = l.config.nx * l.config.ny;
  l.bed.resize(cells);
  l.h.resize(cells);
  for (u32 y = 0; y < l.config.ny; ++y) {
    for (u32 x = 0; x < l.config.nx; ++x) {
      f64 z = static_cast<f64>((x * 7 + y * 13) % 17) / 64.0;
      const bool island = (x / 8 + y / 8) % 3 == 0 && (x % 8) > 1 && (y % 8) > 2;
      if (island) z = 1.0 + static_cast<f64>(mix(x, y) % 16) / 64.0;
      const u32 i = y * l.config.nx + x;
      l.bed[i] = z;
      l.h[i] = std::max(1.0 - z, 0.0);
    }
  }
  return l;
}

Lake inexact_lake() {
  Lake l;
  l.config.nx = 64;
  l.config.ny = 48;
  l.config.cell_m = 0.5;
  const u32 cells = l.config.nx * l.config.ny;
  l.bed.resize(cells);
  l.h.resize(cells);
  for (u32 y = 0; y < l.config.ny; ++y) {
    for (u32 x = 0; x < l.config.nx; ++x) {
      f64 z = 0.731 * static_cast<f64>(mix(x, y) % 1001) / 1001.0;
      const bool island = (x / 8 + y / 8) % 3 == 0 && (x % 8) > 1 && (y % 8) > 2;
      if (island) z = 0.9 + 0.37 * static_cast<f64>(mix(y, x) % 101) / 101.0;
      const u32 i = y * l.config.nx + x;
      l.bed[i] = z;
      l.h[i] = std::max(0.9 - z, 0.0);
    }
  }
  return l;
}

struct LakeRun {
  u64 steps = 0;
  u32 moved = 0;          // cells whose depth or momenta moved by a bit
  f64 surface_drift = 0;  // m, the wet cells' surface against where it started, as the card holds it
  f64 fastest = 0;        // m/s
  f64 roundoff = 0;
  u64 hash = 0;
};

// The lake raised by `raise_m` in the world, handed to the card with the bed's datum `datum_m`.
LakeRun run_lake(gfx::Device& device, const Lake& lake, Ops ops, f64 raise_m, f64 datum_m) {
  DeviceStep step;
  std::string error;
  StepOptions options;
  options.ops = ops;
  REQUIRE_MESSAGE(step.create(device, lake.config, options, &error), error);
  const u32 cells = step.cell_count();
  std::vector<f64> world(cells), zero(cells, 0.0);
  for (u32 i = 0; i < cells; ++i)
    world[i] = lake.bed[i] + raise_m;
  REQUIRE_MESSAGE(step.set_bed(world, datum_m, &error), error);
  REQUIRE_MESSAGE(step.write_state(lake.h, zero, zero, &error), error);
  std::vector<f64> h0(cells), hu(cells), hv(cells);
  REQUIRE(step.read_state(h0, hu, hv, &error));
  LakeRun r;
  r.steps = step.advance(20.0, &error);
  REQUIRE_MESSAGE(r.steps > 0, error);
  std::vector<f64> h(cells);
  REQUIRE(step.read_state(h, hu, hv, &error));
  for (u32 i = 0; i < cells; ++i) {
    if (h[i] != h0[i] || hu[i] != 0.0 || hv[i] != 0.0) ++r.moved;
    // The surface as the card holds it: its own float bed under its own depth.
    const f64 z = static_cast<f64>(static_cast<f32>(world[i] - datum_m));
    const f64 z0 = z + static_cast<f64>(static_cast<f32>(h0[i]));
    if (h[i] > 0.0) r.surface_drift = std::max(r.surface_drift, std::fabs(h[i] + z - z0));
    if (h[i] > 0.0) {
      const f64 u = hu[i] / h[i];
      const f64 v = hv[i] / h[i];
      r.fastest = std::max(r.fastest, std::sqrt(u * u + v * v));
    }
  }
  water_live::Ledger ledger;
  REQUIRE(step.read_ledger(ledger, &error));
  r.roundoff = ledger.roundoff_m3;
  r.hash = step.state_hash(&error);
  return r;
}

// ---- the count of water (the reference's case) ------------------------------------------------

struct Slope {
  Config config;
  std::vector<f64> bed, manning, source, h;
};

Slope slope_case(bool all_open = false) {
  Slope s;
  s.config.nx = 48;
  s.config.ny = 40;
  s.config.cell_m = 0.5;
  s.config.edges.east = Edge::open;
  if (all_open) {
    s.config.edges.west = Edge::open;
    s.config.edges.south = Edge::open;
    s.config.edges.north = Edge::open;
  }
  const u32 cells = s.config.nx * s.config.ny;
  s.bed.resize(cells);
  s.manning.assign(cells, 0.03);
  s.source.assign(cells, 0.0);
  s.h.assign(cells, 0.0);
  for (u32 y = 0; y < s.config.ny; ++y) {
    for (u32 x = 0; x < s.config.nx; ++x) {
      const u32 i = y * s.config.nx + x;
      const f64 bump = 0.08 * static_cast<f64>(mix(x / 3, y / 3) % 97) / 97.0;
      s.bed[i] = 0.6 - 0.02 * static_cast<f64>(x) * s.config.cell_m + bump;
      if (x < 12) s.h[i] = 0.3;
      if (x >= 20 && x < 24 && y >= 6 && y < 10) s.source[i] = 2e-3;
      if (x >= 30 && x < 34 && y >= 28 && y < 32) s.source[i] = -5e-3;
    }
  }
  return s;
}

void load_slope(DeviceStep& step, gfx::Device& device, const Slope& s, const StepOptions& options) {
  std::string error;
  REQUIRE_MESSAGE(step.create(device, s.config, options, &error), error);
  const std::vector<f64> zero(s.h.size(), 0.0);
  REQUIRE_MESSAGE(step.set_bed(s.bed, 0.0, &error), error);
  REQUIRE_MESSAGE(step.set_manning(s.manning, &error), error);
  REQUIRE_MESSAGE(step.set_source(s.source, 1, false, &error), error);
  REQUIRE_MESSAGE(step.write_state(s.h, zero, zero, &error), error);
}

}  // namespace

TEST_CASE("water_live: a lake at rest over a bed with dry islands stays exactly at rest, at any datum") {
  // The reference's exact lake: every height a multiple of 1/64 under 2, so every sum and
  // difference the reconstruction takes is exact in single precision too, and the bound is the
  // reference's, zero. It stands at a datum of zero and 2,000 m up with the grid's datum at 2,000 m:
  // the card holds the bed from the datum, so the two are the same bytes.
  gfx::Device opened;
  if (!open_device(opened)) return;
  gfx::Device* device = &opened;
  const Lake lake = exact_lake();
  for (const Ops ops : {Ops::native, Ops::fixed}) {
    CAPTURE(ops_name(ops));
    const LakeRun low = run_lake(*device, lake, ops, 0.0, 0.0);
    const LakeRun high = run_lake(*device, lake, ops, 2000.0, 2000.0);
    MESSAGE("exact lake (" << ops_name(ops) << "): " << low.steps << " steps, moved " << low.moved
                           << " and " << high.moved << ", hash " << low.hash << " and "
                           << high.hash);
    CHECK(low.steps > 500);
    CHECK(low.moved == 0u);
    CHECK(high.moved == 0u);
    CHECK(low.roundoff == 0.0);
    CHECK(low.hash == high.hash);
  }
}

TEST_CASE("water_live: a lake at rest over an inexact bed stays at rest to single precision's round-off") {
  // The reference's inexact lake. Its bounds (the surface within 1e-13 m, no speed past
  // 1e-11 m/s, a thousand times double's ulp of the surface) scaled by 2^29: 5.4e-5 m and
  // 5.4e-3 m/s. At 2,000 m with the datum there it must be the same bytes as at zero; handed to the
  // card in world heights (a datum of zero) the surface's ulp is 2^-13 m and the balance is lost,
  // which is measured and reported, not held.
  gfx::Device opened;
  if (!open_device(opened)) return;
  gfx::Device* device = &opened;
  const Lake lake = inexact_lake();
  const f64 drift_bound = 1e-13 * k_precision_ratio;
  const f64 speed_bound = 1e-11 * k_precision_ratio;
  for (const Ops ops : {Ops::native, Ops::fixed}) {
    CAPTURE(ops_name(ops));
    const LakeRun low = run_lake(*device, lake, ops, 0.0, 0.0);
    const LakeRun high = run_lake(*device, lake, ops, 2000.0, 2000.0);
    const LakeRun world = run_lake(*device, lake, ops, 2000.0, 0.0);
    MESSAGE("inexact lake (" << ops_name(ops) << "): surface drift " << low.surface_drift
                             << " m, fastest " << low.fastest << " m/s; 2,000 m up with no datum: "
                             << world.surface_drift << " m, " << world.fastest << " m/s, moved "
                             << world.moved << " cells");
    CHECK(low.surface_drift <= drift_bound);
    CHECK(low.fastest <= speed_bound);
    CHECK(low.hash == high.hash);
    // The datum is what holds it: in world heights at 2,000 m the surface's ulp is 2^-13 m and the
    // lake's surface moves by at least half of one, past the drift bound above, which the speed
    // bound alone is too loose to tell. (Both runs move most cells by an ulp of their own depth,
    // so the count of moved cells cannot tell them apart.)
    CHECK(world.moved > 0u);
    CHECK(world.surface_drift >= 0.5 / 8192.0);
    CHECK(world.surface_drift > drift_bound);
  }
}

TEST_CASE("water_live: a dam breaks on a wet bed as the reference does") {
  gfx::Device opened;
  if (!open_device(opened)) return;
  gfx::Device* device = &opened;
  check_dam(*device, 0.001, 0.0045, "Stoker");
}

TEST_CASE("water_live: a dam breaks on a dry bed as the reference does") {
  gfx::Device opened;
  if (!open_device(opened)) return;
  gfx::Device* device = &opened;
  check_dam(*device, 0.0, 0.0058, "Ritter");
}

TEST_CASE("water_live: water is counted to the last cell as the reference counts it") {
  // The reference's count: a block released down a rough slope with a spring and a drain, out
  // through an open east edge. The card's own balance is held to S x 2^-24 of the water involved
  // (each step rounds each cell's depth by half an ulp; the ledger's sums are pairs and add
  // nothing to that); its volume and its outflow are held to the reference's at S x 2^-23 of the
  // water involved; the clamp's round-off to the reference's bound scaled by 2^29.
  gfx::Device opened;
  if (!open_device(opened)) return;
  gfx::Device* device = &opened;
  for (const bool all_open : {false, true}) {
  CAPTURE(all_open);
  const Slope s = slope_case(all_open);
  Solver reference(s.config);
  std::copy(s.h.begin(), s.h.end(), reference.depth().begin());
  Inputs in;
  in.bed_m = s.bed;
  in.manning = s.manning;
  in.source_m_per_s = s.source;
  for (int piece = 0; piece < 6; ++piece)
    reference.advance(in, 10.0);
  const f64 ref_volume = reference.surface_volume_m3();
  const f64 ref_out = reference.ledger().open_edge_out_m3;

  for (const Ops ops : {Ops::native, Ops::fixed}) {
    CAPTURE(ops_name(ops));
    StepOptions options;
    options.ops = ops;
    DeviceStep step;
    load_slope(step, *device, s, options);
    const u32 cells = step.cell_count();
    std::string error;
    std::vector<f64> h(cells), hu(cells), hv(cells);
    REQUIRE(step.read_state(h, hu, hv, &error));
    f64 initial = 0.0;
    for (const f64 v : h)
      initial += v;
    initial *= s.config.cell_m * s.config.cell_m;
    std::vector<bool> was_wet(cells);
    for (u32 i = 0; i < cells; ++i)
      was_wet[i] = h[i] > 0.0;
    f64 lowest = 0.0;
    u64 steps = 0;
    for (int piece = 0; piece < 6; ++piece) {
      steps += step.advance(10.0, &error);
      REQUIRE(step.read_state(h, hu, hv, &error));
      lowest = std::min(lowest, *std::min_element(h.begin(), h.end()));
    }
    u32 wetted = 0, drained = 0;
    f64 volume = 0.0, fastest_flux = 0.0;
    for (u32 i = 0; i < cells; ++i) {
      if (!was_wet[i] && h[i] > 0.0) ++wetted;
      volume += h[i];
      fastest_flux = std::max(fastest_flux, std::fabs(hu[i]));
    }
    volume *= s.config.cell_m * s.config.cell_m;
    for (u32 y = 28; y < 32; ++y)
      for (u32 x = 30; x < 34; ++x)
        drained += h[y * s.config.nx + x] == 0.0 ? 1u : 0u;
    water_live::Ledger ledger;
    REQUIRE(step.read_ledger(ledger, &error));
    const f64 balance =
        volume + ledger.open_edge_out_m3 - initial - ledger.source_m3 - ledger.roundoff_m3;
    const f64 involved = initial + std::fabs(ledger.source_m3);
    MESSAGE("count on the card (" << ops_name(ops) << "): " << steps << " steps (reference "
                                  << reference.ledger().steps << "), volume " << volume
                                  << " m3 (reference " << ref_volume << "), out "
                                  << ledger.open_edge_out_m3 << " (reference " << ref_out
                                  << "), balance " << balance << ", roundoff "
                                  << ledger.roundoff_m3 << ", wetted " << wetted);
    CHECK(lowest >= 0.0);
    CHECK(wetted > 200u);
    CHECK(drained > 0u);
    CHECK(ledger.open_edge_out_m3 > 0.0);
    CHECK(std::fabs(balance) <= static_cast<f64>(steps) * k_ulp_f32 * involved);
    CHECK(ledger.roundoff_m3 <= 1e-15 * k_precision_ratio * initial);
    CHECK(std::fabs(volume - ref_volume) <= static_cast<f64>(steps) * 2.0 * k_ulp_f32 * involved);
    CHECK(std::fabs(ledger.open_edge_out_m3 - ref_out) <=
          static_cast<f64>(steps) * 2.0 * k_ulp_f32 * involved);
    CHECK(fastest_flux < 10.0);
    // Cell by cell (W175 review): L1 of each field over the reference's, held to the same
    // S x 2^-23; the largest cell difference reported beside it (a wet-dry front a cell apart is
    // a difference of a whole depth, which no rounding bound covers).
    f64 dh = 0.0, du = 0.0, dv = 0.0, nh = 0.0, nu = 0.0, nv = 0.0, mh = 0.0, mu = 0.0, mv = 0.0;
    for (u32 i = 0; i < cells; ++i) {
      const f64 eh = std::fabs(h[i] - reference.depth()[i]);
      const f64 eu = std::fabs(hu[i] - reference.momentum_x()[i]);
      const f64 ev = std::fabs(hv[i] - reference.momentum_y()[i]);
      dh += eh;
      du += eu;
      dv += ev;
      nh += std::fabs(reference.depth()[i]);
      nu += std::fabs(reference.momentum_x()[i]);
      nv += std::fabs(reference.momentum_y()[i]);
      mh = std::max(mh, eh);
      mu = std::max(mu, eu);
      mv = std::max(mv, ev);
    }
    MESSAGE("count cell by cell (" << ops_name(ops) << ", all open " << all_open << "): L1 h "
                                   << dh / nh << ", hu " << du / nu << ", hv " << dv / nv
                                   << "; largest " << mh << " m, " << mu << ", " << mv << " m2/s");
    CHECK(dh / nh <= static_cast<f64>(steps) * 2.0 * k_ulp_f32);
    CHECK(du / nu <= static_cast<f64>(steps) * 2.0 * k_ulp_f32);
  }
  }
}

TEST_CASE("water_live: the step is the same bytes twice, and a face's flux is one value on both sides") {
  // Twice on one device from the same upload: the same hash. And the two forms of the faces — each
  // cell computing its own four, every face computed once and stored — the same bytes: so the flux
  // each of a face's two cells takes through it is the same value to the bit, and what one cell
  // loses through a face its neighbour gains.
  gfx::Device opened;
  if (!open_device(opened)) return;
  gfx::Device* device = &opened;
  const Slope s = slope_case();
  for (const Ops ops : {Ops::native, Ops::fixed}) {
    CAPTURE(ops_name(ops));
    u64 hashes[3] = {};
    for (u32 run = 0; run < 3; ++run) {
      StepOptions options;
      options.ops = ops;
      options.stored_faces = run == 2;
      DeviceStep step;
      load_slope(step, *device, s, options);
      std::string error;
      REQUIRE_MESSAGE(step.run_steps(400, k_no_limit, &error), error);
      hashes[run] = step.state_hash(&error);
    }
    MESSAGE("400 steps (" << ops_name(ops) << "): gather " << hashes[0] << ", " << hashes[1]
                          << "; stored faces " << hashes[2]);
    CHECK(hashes[0] != 0u);
    CHECK(hashes[0] == hashes[1]);
    CHECK(hashes[0] == hashes[2]);
  }
}

TEST_CASE("water_live: a closed basin keeps its water to the update's own rounding") {
  // A 2-D dam break sloshing in a closed basin for 1,000 steps. A face's flux is one value on both
  // sides (the case above), so every flux telescopes and what moves the volume is only each cell's
  // own update rounding its depth: at most half an ulp of the deepest water a cell a step, the
  // bound S x cells x 2^-24 x h_max x area. That is as conserved as a float depth can be; the
  // ledger's clamp is held to zero roundoff beside it.
  gfx::Device opened;
  if (!open_device(opened)) return;
  gfx::Device* device = &opened;
  Config config;
  config.nx = 96;
  config.ny = 64;
  config.cell_m = 0.25;
  const u32 cells = config.nx * config.ny;
  std::vector<f64> bed(cells), h(cells), zero(cells, 0.0);
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      const u32 i = y * config.nx + x;
      bed[i] = 0.05 * static_cast<f64>(mix(x / 4, y / 4) % 13) / 13.0;
      h[i] = (x < 30 ? 0.8 : 0.2) - bed[i];
    }
  }
  for (const Ops ops : {Ops::native, Ops::fixed}) {
    CAPTURE(ops_name(ops));
    StepOptions options;
    options.ops = ops;
    DeviceStep step;
    std::string error;
    REQUIRE_MESSAGE(step.create(*device, config, options, &error), error);
    REQUIRE(step.set_bed(bed, 0.0, &error));
    REQUIRE(step.write_state(h, zero, zero, &error));
    std::vector<f64> got(cells), hu(cells), hv(cells);
    REQUIRE(step.read_state(got, hu, hv, &error));
    f64 before = 0.0;
    for (const f64 v : got)
      before += v;
    REQUIRE(step.run_steps(1000, k_no_limit, &error));
    REQUIRE(step.read_state(got, hu, hv, &error));
    f64 after = 0.0, moving = 0.0;
    for (u32 i = 0; i < cells; ++i) {
      after += got[i];
      moving = std::max(moving, std::fabs(hu[i]));
    }
    const f64 area = config.cell_m * config.cell_m;
    const f64 bound = 1000.0 * cells * k_ulp_f32 * 0.8 * area;
    MESSAGE("closed basin (" << ops_name(ops) << "): volume moved " << (after - before) * area
                             << " m3 of " << before * area << " (bound " << bound << ")");
    CHECK(moving > 0.01);  // the water did move
    CHECK(std::fabs(after - before) * area <= bound);
  }
}

TEST_CASE("water_live: an exchange under the depth's last bit is kept by the compensated sum") {
  // A still pond 1 m deep losing 3.7e-6 m/s (13.2 mm/h, a loam's take) for 600 s at one step a
  // 60 Hz frame must lose 2.22 mm. One step's take, 6.2e-8 m, is about the depth's ulp below 1 m
  // (2^-24 = 6.0e-8), so each step's subtraction rounds it to a whole ulp. Three forms: the plain
  // step; the sink on a clock eight and sixty-four times coarser; the remainder kept per cell. The
  // compensated form is held to two ulps of the depth; the others are measured.
  gfx::Device opened;
  if (!open_device(opened)) return;
  gfx::Device* device = &opened;
  Config config;
  config.nx = 32;
  config.ny = 32;
  config.cell_m = 0.5;
  const u32 cells = config.nx * config.ny;
  std::vector<f64> bed(cells, 0.0), h(cells, 1.0), zero(cells, 0.0), sink(cells, -3.7e-6);
  constexpr f32 k_frame = 1.0f / 60.0f;
  constexpr u32 k_steps = 36000;
  const f64 want = 3.7e-6 * static_cast<f64>(k_frame) * k_steps;
  struct Form {
    const char* name;
    u32 every;
    bool compensate;
  };
  const Form forms[] = {{"plain", 1, false}, {"every 8", 8, false}, {"every 64", 64, false},
                        {"compensated", 1, true}};
  for (const Form& form : forms) {
    CAPTURE(form.name);
    DeviceStep step;
    std::string error;
    REQUIRE_MESSAGE(step.create(*device, config, StepOptions{}, &error), error);
    REQUIRE(step.set_bed(bed, 0.0, &error));
    REQUIRE(step.set_source(sink, form.every, form.compensate, &error));
    REQUIRE(step.write_state(h, zero, zero, &error));
    // A coarser clock counts steps on the host, so advance() refuses it (water_live.h).
    if (form.every > 1) CHECK(step.advance(1.0, &error) == 0u);
    for (u32 batch = 0; batch < k_steps / 1000; ++batch)
      REQUIRE_MESSAGE(step.run_steps(1000, k_frame, &error), error);
    std::vector<f64> got(cells), hu(cells), hv(cells);
    REQUIRE(step.read_state(got, hu, hv, &error));
    const f64 lost = 1.0 - got[cells / 2];
    MESSAGE("slow sink, " << std::string(form.name) << ": lost " << lost * 1e3 << " mm of " << want * 1e3
                          << " mm (" << (lost - want) / want * 100.0 << " %)");
    if (form.compensate) CHECK(std::fabs(lost - want) <= 2.0 * k_ulp_f32);
  }
}
