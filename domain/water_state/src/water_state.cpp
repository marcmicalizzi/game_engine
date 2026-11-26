#include <core/base/assert.h>
#include <core/jobs/job_system.h>
#include <core/math/deterministic.h>
#include <domain/water_state/water_state.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

// The scheme, with the paper and equation each part comes from (docs/subsystems/water_state.md,
// "The step"):
//   - the update U' = U - dt/dx (F(i+1/2, left side) - F(i-1/2, right side)), the bed's height at a
//     face the larger of its two cells', each side's depth reconstructed hydrostatically against
//     it, and the normal momentum's flux corrected on each side by g/2 (h^2 - h*^2): the
//     hydrostatic reconstruction, Audusse, Bouchut, Bristeau and Sainte-Marie (2016), (3.1)-(3.5),
//     after Audusse et al. (2004);
//   - the flux of the system without a bed between the two reconstructed states: the
//     central-upwind flux of Bollermann, Chen, Kurganov and Noelle (2014), (2.8), with its
//     one-sided speeds (2.13)-(2.14), applied across x faces and y faces in turn;
//   - a velocity of zero below the dry depth (the same paper, section 2, eps = 1e-9);
//   - Manning's friction, g n^2 |u| u / h^(1/3) (the grounding page's form), integrated linearly
//     implicitly so it can stop a flow and never reverse it (derived on the page);
//   - the ground's take by Green and Ampt's rate, integrated implicitly (derived on the page);
//   - the dissipation as the residual of each cell's energy balance, with the same flux formula
//     carrying the energy across the faces (derived on the page).

namespace engine::water_state {

namespace {

struct Flux {
  f64 h;  // mass, m^2/s
  f64 n;  // normal momentum, m^3/s^2
  f64 t;  // transverse momentum, m^3/s^2
  f64 g;  // energy over the density, per metre of face, m^4/s^3
};

inline f64 pressure(f64 g, f64 h) noexcept { return 0.5 * g * h * h; }

inline f64 max3(f64 a, f64 b, f64 c) noexcept {
  const f64 m = a > b ? a : b;
  return m > c ? m : c;
}

inline f64 min3(f64 a, f64 b, f64 c) noexcept {
  const f64 m = a < b ? a : b;
  return m < c ? m : c;
}

// Energy over the density, per area, of a state on a bed z: h |u|^2 / 2 + g h^2 / 2 + g h z.
inline f64 energy(f64 g, f64 h, f64 un, f64 ut, f64 z) noexcept {
  return 0.5 * h * (un * un + ut * ut) + pressure(g, h) + g * h * z;
}

// Its flux across a face: u_n (h |u|^2 / 2 + g h^2 + g h z).
inline f64 energy_flux(f64 g, f64 h, f64 un, f64 ut, f64 z) noexcept {
  return un * (0.5 * h * (un * un + ut * ut) + 2.0 * pressure(g, h) + g * h * z);
}

// The flux of the system without a bed between two states, each a depth, a normal velocity and a
// transverse velocity, on the face's bed z. Bollermann et al. (2014), (2.8), with the one-sided
// local speeds of (2.13)-(2.14); the transverse momentum and the energy are the same formula's
// further components. Two identical states give the exact flux of that state, which the formula
// equals in exact arithmetic and which keeps a lake at rest exactly at rest in floating point (the
// flux and the reconstruction's correction then cancel to the bit).
inline Flux face_flux(f64 g, f64 z, f64 hl, f64 ul, f64 vl, f64 hr, f64 ur, f64 vr) noexcept {
  if (hl == hr && ul == ur && vl == vr) {
    const f64 q = hl * ul;
    return {q, q * ul + pressure(g, hl), q * vl, energy_flux(g, hl, ul, vl, z)};
  }
  const f64 cl = std::sqrt(g * hl);
  const f64 cr = std::sqrt(g * hr);
  const f64 ap = max3(ur + cr, ul + cl, 0.0);
  const f64 am = min3(ur - cr, ul - cl, 0.0);
  const f64 span = ap - am;
  if (!(span > 0.0)) return {0.0, 0.0, 0.0, 0.0};
  const f64 ql = hl * ul;
  const f64 qr = hr * ur;
  const f64 apam = ap * am;
  const f64 fh = (ap * ql - am * qr + apam * (hr - hl)) / span;
  const f64 fn =
      (ap * (ql * ul + pressure(g, hl)) - am * (qr * ur + pressure(g, hr)) + apam * (qr - ql)) /
      span;
  const f64 ft = (ap * (ql * vl) - am * (qr * vr) + apam * (hr * vr - hl * vl)) / span;
  const f64 fg = (ap * energy_flux(g, hl, ul, vl, z) - am * energy_flux(g, hr, ur, vr, z) +
                  apam * (energy(g, hr, ur, vr, z) - energy(g, hl, ul, vl, z))) /
                 span;
  return {fh, fn, ft, fg};
}

// h^(1/3) from core/math's deterministic log and exp: the C library's cbrt and pow differ in the
// last bit by library, and the friction decides the momenta.
inline f64 cube_root(f64 h) noexcept { return det::exp(det::log(h) / 3.0); }

constexpr u64 k_fnv_offset = 0xcbf29ce484222325ull;
constexpr u64 k_fnv_prime = 0x100000001b3ull;

u64 fnv_bytes(u64 hash, const f64* data, usize count) noexcept {
  for (usize i = 0; i < count; ++i) {
    u64 bits = 0;
    std::memcpy(&bits, data + i, sizeof bits);
    for (u32 b = 0; b < 8; ++b) {
      hash ^= (bits >> (8 * b)) & 0xffu;
      hash *= k_fnv_prime;
    }
  }
  return hash;
}

}  // namespace

Solver::Solver(const Config& config) : config_(config) {
  ENGINE_ASSERT(config_.carried <= k_max_carried, "water_state: too many carried quantities");
  if (config_.carried > k_max_carried) config_.carried = k_max_carried;
  if (config_.rows_per_job == 0) config_.rows_per_job = 1;
  cells_ = config_.nx * config_.ny;
  const u32 nx = config_.nx;
  const u32 ny = config_.ny;
  const u32 carried = config_.carried;
  h_.resize(cells_, 0.0);
  hu_.resize(cells_, 0.0);
  hv_.resize(cells_, 0.0);
  ground_.resize(cells_, 0.0);
  carried_.resize(carried * cells_, 0.0);
  speed_.resize(cells_, 0.0);
  tau_x_.resize(cells_, 0.0);
  tau_y_.resize(cells_, 0.0);
  dissipation_.resize(cells_, 0.0);
  u_.resize(cells_, 0.0);
  v_.resize(cells_, 0.0);
  row_a_.resize(ny, 0.0);
  row_b_.resize(ny, 0.0);
  rows_.resize(ny);
  const u32 x_faces = (nx + 1) * ny;
  const u32 y_faces = nx * (ny + 1);
  fx_h_.resize(x_faces, 0.0);
  fx_nl_.resize(x_faces, 0.0);
  fx_nr_.resize(x_faces, 0.0);
  fx_t_.resize(x_faces, 0.0);
  fx_g_.resize(x_faces, 0.0);
  fx_c_.resize(carried * x_faces, 0.0);
  fy_h_.resize(y_faces, 0.0);
  fy_nl_.resize(y_faces, 0.0);
  fy_nr_.resize(y_faces, 0.0);
  fy_t_.resize(y_faces, 0.0);
  fy_g_.resize(y_faces, 0.0);
  fy_c_.resize(carried * y_faces, 0.0);
}

bool Solver::inputs_fit(const Inputs& in) const noexcept {
  const auto fits = [this](std::span<const f64> s) { return s.empty() || s.size() == cells_; };
  if (cells_ == 0) return false;
  if (in.bed_m.size() != cells_) return false;
  if (!fits(in.manning) || !fits(in.source_m_per_s)) return false;
  const bool k = !in.ground_conductivity_m_per_s.empty();
  const bool s = !in.ground_suction_m.empty();
  const bool c = !in.ground_capacity_m.empty();
  if (k != s || k != c) return false;
  if (!fits(in.ground_conductivity_m_per_s) || !fits(in.ground_suction_m) ||
      !fits(in.ground_capacity_m))
    return false;
  for (u32 q = 0; q < k_max_carried; ++q) {
    if (!fits(in.carried_source[q]) || !fits(in.carried_exchange[q])) return false;
    if (q >= config_.carried && (!in.carried_source[q].empty() || !in.carried_exchange[q].empty()))
      return false;
  }
  return true;
}

template <class F>
void Solver::for_rows(jobs::JobSystem* jobs, u32 rows, F&& body) {
  if (jobs == nullptr) {
    body(0u, rows);
    return;
  }
  // jobs::parallel_for keeps its chunks in a small vector of 64 and allocates past that, so the
  // rows are dealt in at most 64 chunks: a step allocates nothing at any grid (the bytes do not
  // depend on the chunking).
  const u32 fewest = (rows + 63) / 64;
  const u32 grain = config_.rows_per_job > fewest ? config_.rows_per_job : fewest;
  jobs->parallel_for(jobs::Pool::Performance, rows, grain, std::forward<F>(body));
}

void Solver::velocity_rows(u32 begin, u32 end) noexcept {
  const u32 nx = config_.nx;
  const f64 g = config_.gravity;
  const f64 dry = config_.dry_depth_m;
  for (u32 y = begin; y < end; ++y) {
    f64 a = 0.0;
    f64 b = 0.0;
    for (u32 x = 0; x < nx; ++x) {
      const u32 i = y * nx + x;
      const f64 h = h_[i];
      const bool wet = h >= dry;
      const f64 u = wet ? hu_[i] / h : 0.0;
      const f64 v = wet ? hv_[i] / h : 0.0;
      u_[i] = u;
      v_[i] = v;
      const f64 c = std::sqrt(g * h);
      const f64 au = std::fabs(u) + c;
      const f64 av = std::fabs(v) + c;
      a = au > a ? au : a;
      b = av > b ? av : b;
    }
    row_a_[y] = a;
    row_b_[y] = b;
  }
}

f64 Solver::stable_step(jobs::JobSystem* jobs) {
  if (cells_ == 0) return std::numeric_limits<f64>::infinity();
  for_rows(jobs, config_.ny, [this](u32 begin, u32 end) { velocity_rows(begin, end); });
  // A maximum is the same whatever order it is taken in.
  f64 a = 0.0;
  f64 b = 0.0;
  for (u32 y = 0; y < config_.ny; ++y) {
    a = row_a_[y] > a ? row_a_[y] : a;
    b = row_b_[y] > b ? row_b_[y] : b;
  }
  const f64 speed = a + b;
  if (!(speed > 0.0)) return std::numeric_limits<f64>::infinity();
  return config_.cfl * config_.cell_m / speed;
}

// One face between a left (or south) cell l and a right (or north) cell r; l == r for an edge's
// ghost. `ln`/`rn` are the cells' velocities normal to the face (negated for a closed edge's
// mirrored ghost), `lt`/`rt` along it. Writes the face's mass flux, its normal momentum's flux less
// each side's reconstructed pressure, its transverse momentum's flux, its energy's flux, and each
// carried quantity's flux (the mass flux times the concentration of the cell it comes from).
void Solver::face(const f64* bed, u32 l, u32 r, f64 ln, f64 lt, f64 rn, f64 rt, const Faces& out,
                  u32 index) noexcept {
  const f64 g = config_.gravity;
  const f64 zl = bed[l];
  const f64 zr = bed[r];
  const f64 zf = zl > zr ? zl : zr;
  const f64 el = h_[l] + zl - zf;
  const f64 er = h_[r] + zr - zf;
  const f64 hl = el > 0.0 ? el : 0.0;
  const f64 hr = er > 0.0 ? er : 0.0;
  const Flux f = face_flux(g, zf, hl, ln, lt, hr, rn, rt);
  out.h[index] = f.h;
  out.nl[index] = f.n - pressure(g, hl);
  out.nr[index] = f.n - pressure(g, hr);
  out.t[index] = f.t;
  out.g[index] = f.g;
  const u32 carried = config_.carried;
  if (carried != 0) {
    const u32 up = f.h > 0.0 ? l : r;
    const f64 h_up = h_[up];
    const bool wet = h_up >= config_.dry_depth_m;
    for (u32 k = 0; k < carried; ++k) {
      const f64 conc = wet ? carried_[k * cells_ + up] / h_up : 0.0;
      out.c[k * out.count + index] = f.h * conc;
    }
  }
}

void Solver::x_face_rows(const Inputs& in, u32 begin, u32 end) noexcept {
  const u32 nx = config_.nx;
  const Faces out{fx_h_.data(), fx_nl_.data(), fx_nr_.data(),        fx_t_.data(),
                  fx_g_.data(), fx_c_.data(),  (nx + 1) * config_.ny};
  const f64* bed = in.bed_m.data();
  const f64 west = config_.edges.west == Edge::closed ? -1.0 : 1.0;
  const f64 east = config_.edges.east == Edge::closed ? -1.0 : 1.0;
  for (u32 y = begin; y < end; ++y) {
    const u32 row = y * nx;
    const u32 frow = y * (nx + 1);
    const u32 first = row;  // the west edge: a ghost of cell 0
    face(bed, first, first, west * u_[first], v_[first], u_[first], v_[first], out, frow);
    for (u32 f = 1; f < nx; ++f) {
      const u32 l = row + f - 1;
      const u32 r = row + f;
      face(bed, l, r, u_[l], v_[l], u_[r], v_[r], out, frow + f);
    }
    const u32 last = row + nx - 1;  // the east edge: a ghost of the last cell
    face(bed, last, last, u_[last], v_[last], east * u_[last], v_[last], out, frow + nx);
  }
}

// Face row r lies south of cell row r; rows 0 and ny are the south and north edges.
void Solver::y_face_rows(const Inputs& in, u32 begin, u32 end) noexcept {
  const u32 nx = config_.nx;
  const u32 ny = config_.ny;
  const Faces out{fy_h_.data(), fy_nl_.data(), fy_nr_.data(), fy_t_.data(),
                  fy_g_.data(), fy_c_.data(),  nx * (ny + 1)};
  const f64* bed = in.bed_m.data();
  const f64 south = config_.edges.south == Edge::closed ? -1.0 : 1.0;
  const f64 north = config_.edges.north == Edge::closed ? -1.0 : 1.0;
  for (u32 r = begin; r < end; ++r) {
    const u32 frow = r * nx;
    if (r == 0) {
      for (u32 x = 0; x < nx; ++x)
        face(bed, x, x, south * v_[x], u_[x], v_[x], u_[x], out, frow + x);
    } else if (r == ny) {
      for (u32 x = 0; x < nx; ++x) {
        const u32 c = (ny - 1) * nx + x;
        face(bed, c, c, v_[c], u_[c], north * v_[c], u_[c], out, frow + x);
      }
    } else {
      for (u32 x = 0; x < nx; ++x) {
        const u32 s = (r - 1) * nx + x;
        const u32 n = r * nx + x;
        face(bed, s, n, v_[s], u_[s], v_[n], u_[n], out, frow + x);
      }
    }
  }
}

void Solver::update_rows(const Inputs& in, f64 dt, u32 begin, u32 end) noexcept {
  const u32 nx = config_.nx;
  const u32 ny = config_.ny;
  const u32 x_faces = (nx + 1) * ny;
  const u32 y_faces = nx * (ny + 1);
  const f64 g = config_.gravity;
  const f64 dry = config_.dry_depth_m;
  const f64 rho = config_.water_density;
  const f64 area = cell_area_m2();
  const f64 lam = dt / config_.cell_m;
  const f64 edge_volume = dt * config_.cell_m;  // a face's flux times this is m^3
  const u32 carried = config_.carried;
  const f64* bed = in.bed_m.data();
  const f64* manning = in.manning.empty() ? nullptr : in.manning.data();
  const f64* source = in.source_m_per_s.empty() ? nullptr : in.source_m_per_s.data();
  const f64* ground_k =
      in.ground_conductivity_m_per_s.empty() ? nullptr : in.ground_conductivity_m_per_s.data();
  const f64* ground_s = in.ground_suction_m.data();
  const f64* ground_cap = in.ground_capacity_m.data();
  // Each quantity's decay over this step, the same factor in every cell.
  std::array<f64, k_max_carried> keep{};
  for (u32 k = 0; k < carried; ++k) {
    const f64 tau = config_.carried_config[k].decay_s;
    keep[k] = tau > 0.0 ? det::exp(-dt / tau) : 1.0;
  }
  for (u32 y = begin; y < end; ++y) {
    RowSums sums{};
    const u32 frow = y * (nx + 1);
    if (config_.edges.west == Edge::open) {
      sums.open -= fx_h_[frow] * edge_volume;
      for (u32 k = 0; k < carried; ++k)
        sums.carried[k].open_edge_out -= fx_c_[k * x_faces + frow] * edge_volume;
    }
    if (config_.edges.east == Edge::open) {
      sums.open += fx_h_[frow + nx] * edge_volume;
      for (u32 k = 0; k < carried; ++k)
        sums.carried[k].open_edge_out += fx_c_[k * x_faces + frow + nx] * edge_volume;
    }
    if (y == 0 && config_.edges.south == Edge::open) {
      for (u32 x = 0; x < nx; ++x) {
        sums.open -= fy_h_[x] * edge_volume;
        for (u32 k = 0; k < carried; ++k)
          sums.carried[k].open_edge_out -= fy_c_[k * y_faces + x] * edge_volume;
      }
    }
    if (y + 1 == ny && config_.edges.north == Edge::open) {
      for (u32 x = 0; x < nx; ++x) {
        sums.open += fy_h_[ny * nx + x] * edge_volume;
        for (u32 k = 0; k < carried; ++k)
          sums.carried[k].open_edge_out += fy_c_[k * y_faces + ny * nx + x] * edge_volume;
      }
    }
    for (u32 x = 0; x < nx; ++x) {
      const u32 i = y * nx + x;
      const u32 w = frow + x;
      const u32 e = w + 1;
      const u32 s = y * nx + x;
      const u32 n = s + nx;
      const f64 z = bed[i];
      const f64 h0 = h_[i];
      const f64 energy0 = energy(g, h0, u_[i], v_[i], z);
      f64 h = h0 - lam * ((fx_h_[e] - fx_h_[w]) + (fy_h_[n] - fy_h_[s]));
      f64 hu = hu_[i] - lam * ((fx_nl_[e] - fx_nr_[w]) + (fy_t_[n] - fy_t_[s]));
      f64 hv = hv_[i] - lam * ((fx_t_[e] - fx_t_[w]) + (fy_nl_[n] - fy_nr_[s]));
      for (u32 k = 0; k < carried; ++k) {
        const u32 xo = k * x_faces;
        const u32 yo = k * y_faces;
        f64& q = carried_[k * cells_ + i];
        q = q - lam * ((fx_c_[xo + e] - fx_c_[xo + w]) + (fy_c_[yo + n] - fy_c_[yo + s]));
      }
      // The dissipation: what the cell's energy lost over the transport that the energy's flux
      // through its faces does not account for (docs/subsystems/water_state.md, "The dissipation").
      f64 scheme_loss = 0.0;
      {
        const bool wet = h >= dry;
        const f64 u1 = wet ? hu / h : 0.0;
        const f64 v1 = wet ? hv / h : 0.0;
        const f64 energy1 = energy(g, h > 0.0 ? h : 0.0, u1, v1, z);
        const f64 through = (fx_g_[e] - fx_g_[w]) + (fy_g_[n] - fy_g_[s]);
        scheme_loss = -rho * ((energy1 - energy0) / dt + through / config_.cell_m);
      }
      sums.scheme_loss += scheme_loss * area * dt;
      // Under the step's limit the depth cannot go negative in exact arithmetic
      // (docs/subsystems/water_state.md, "The step's length"); what rounding leaves below zero is
      // clamped and counted.
      if (h < 0.0) {
        sums.roundoff -= h * area;
        h = 0.0;
      }
      if (source != nullptr) {
        const f64 added = source[i] * dt;
        if (added >= 0.0) {
          h += added;
          sums.source += added * area;
        } else {
          const f64 taken = -added < h ? -added : h;
          h -= taken;
          sums.source -= taken * area;
        }
      }
      if (ground_k != nullptr && h > 0.0) {
        // Green and Ampt's rate f = K (1 + psi dtheta / F) integrated implicitly over the step:
        // F' = F + dt K (1 + s / F') has the positive root below, finite from F = 0 on; the take is
        // capped by the water there and the room the store has left.
        const f64 stored = ground_[i];
        const f64 room = ground_cap[i] - stored;
        if (room > 0.0) {
          const f64 kdt = ground_k[i] * dt;
          const f64 b = stored + kdt;
          const f64 next = 0.5 * (b + std::sqrt(b * b + 4.0 * kdt * ground_s[i]));
          f64 take = next - stored;
          take = take < h ? take : h;
          take = take < room ? take : room;
          if (take > 0.0) {
            h -= take;
            ground_[i] = stored + take;
            sums.ground += take * area;
          }
        }
      }
      f64 speed = 0.0;
      f64 tau_x = 0.0;
      f64 tau_y = 0.0;
      f64 friction = 0.0;
      if (h >= dry) {
        if (manning != nullptr) {
          // Manning's friction, the momenta's source -g n^2 |u| u / h^(1/3), taken implicitly in
          // |u|'s coefficient: (hu)' = (hu)* / (1 + dt g n^2 |u*| / h^(4/3)).
          const f64 nn = manning[i];
          const f64 u = hu / h;
          const f64 v = hv / h;
          const f64 sp = std::sqrt(u * u + v * v);
          const f64 cb = cube_root(h);
          const f64 k = g * nn * nn * sp / (h * cb);
          const f64 denom = 1.0 + dt * k;
          hu = hu / denom;
          hv = hv / denom;
          const f64 u1 = hu / h;
          const f64 v1 = hv / h;
          speed = std::sqrt(u1 * u1 + v1 * v1);
          const f64 coeff = rho * g * nn * nn * speed / cb;
          tau_x = coeff * u1;
          tau_y = coeff * v1;
          friction = coeff * speed * speed;
        } else {
          const f64 u1 = hu / h;
          const f64 v1 = hv / h;
          speed = std::sqrt(u1 * u1 + v1 * v1);
        }
      } else {
        hu = 0.0;
        hv = 0.0;
      }
      sums.friction += friction * area * dt;
      const f64 dissipation = (scheme_loss > 0.0 ? scheme_loss : 0.0) + friction;
      for (u32 k = 0; k < carried; ++k) {
        const CarriedConfig& cc = config_.carried_config[k];
        CarriedLedger& account = sums.carried[k];
        f64& q = carried_[k * cells_ + i];
        if (cc.per_joule > 0.0 && speed >= cc.min_speed_m_per_s) {
          const f64 made = cc.per_joule * dissipation * dt;
          q += made;
          account.from_dissipation += made * area;
        }
        if (!in.carried_source[k].empty()) {
          const f64 added = in.carried_source[k][i] * dt;
          q += added;
          account.source += added * area;
        }
        if (!in.carried_exchange[k].empty()) {
          f64 moved = in.carried_exchange[k][i] * dt;
          if (moved < 0.0 && -moved > q) moved = -q;  // only what the water holds settles
          q += moved;
          account.exchange += moved * area;
        }
        if (keep[k] != 1.0) {
          const f64 kept = q * keep[k];
          account.decayed += (q - kept) * area;
          q = kept;
        }
      }
      h_[i] = h;
      hu_[i] = hu;
      hv_[i] = hv;
      speed_[i] = speed;
      tau_x_[i] = tau_x;
      tau_y_[i] = tau_y;
      dissipation_[i] = dissipation;
    }
    rows_[y] = sums;
  }
}

f64 Solver::step(const Inputs& in, f64 max_dt, jobs::JobSystem* jobs) {
  if (!inputs_fit(in) || !(max_dt > 0.0)) return 0.0;
  const f64 stable = stable_step(jobs);  // also fills the velocities the faces read
  const f64 dt = stable < max_dt ? stable : max_dt;
  for_rows(jobs, config_.ny, [this, &in](u32 begin, u32 end) { x_face_rows(in, begin, end); });
  for_rows(jobs, config_.ny + 1, [this, &in](u32 begin, u32 end) { y_face_rows(in, begin, end); });
  for_rows(jobs, config_.ny,
           [this, &in, dt](u32 begin, u32 end) { update_rows(in, dt, begin, end); });
  // The rows' sums, added in row order: the same bytes at any thread count.
  for (u32 y = 0; y < config_.ny; ++y) {
    const RowSums& r = rows_[y];
    ledger_.source_m3 += r.source;
    ledger_.open_edge_out_m3 += r.open;
    ledger_.roundoff_m3 += r.roundoff;
    ledger_.ground_m3 += r.ground;
    ledger_.scheme_loss_j += r.scheme_loss;
    ledger_.friction_work_j += r.friction;
    for (u32 k = 0; k < config_.carried; ++k) {
      CarriedLedger& to = ledger_.carried[k];
      const CarriedLedger& from = r.carried[k];
      to.source += from.source;
      to.from_dissipation += from.from_dissipation;
      to.exchange += from.exchange;
      to.decayed += from.decayed;
      to.open_edge_out += from.open_edge_out;
    }
  }
  ledger_.time_s += dt;
  ledger_.steps += 1;
  return dt;
}

u64 Solver::advance(const Inputs& in, f64 duration_s, jobs::JobSystem* jobs) {
  if (!inputs_fit(in)) return 0;
  u64 steps = 0;
  f64 remaining = duration_s;
  while (remaining > 0.0) {
    const f64 dt = step(in, remaining, jobs);
    if (!(dt > 0.0)) break;
    ++steps;
    if (dt == remaining) break;
    remaining -= dt;
  }
  return steps;
}

f64 Solver::surface_volume_m3() const noexcept {
  f64 sum = 0.0;
  for (u32 i = 0; i < cells_; ++i)
    sum += h_[i];
  return sum * cell_area_m2();
}

f64 Solver::ground_volume_m3() const noexcept {
  f64 sum = 0.0;
  for (u32 i = 0; i < cells_; ++i)
    sum += ground_[i];
  return sum * cell_area_m2();
}

f64 Solver::carried_total(u32 k) const noexcept {
  f64 sum = 0.0;
  for (u32 i = 0; i < cells_; ++i)
    sum += carried_[k * cells_ + i];
  return sum * cell_area_m2();
}

u64 Solver::state_hash() const noexcept {
  u64 hash = k_fnv_offset;
  hash = fnv_bytes(hash, h_.data(), cells_);
  hash = fnv_bytes(hash, hu_.data(), cells_);
  hash = fnv_bytes(hash, hv_.data(), cells_);
  hash = fnv_bytes(hash, ground_.data(), cells_);
  hash = fnv_bytes(hash, carried_.data(), usize{config_.carried} * cells_);
  return hash;
}

}  // namespace engine::water_state
