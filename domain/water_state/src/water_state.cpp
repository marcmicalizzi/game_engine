#include <core/base/assert.h>
#include <core/jobs/job_system.h>
#include <core/math/deterministic.h>
#include <domain/water_state/water_state.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

// The scheme, with the paper and equation each part comes from (docs/subsystems/water_state.md,
// "The scheme"):
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
//     implicitly so it can stop a flow and never reverse it (derived on the page).

namespace engine::water_state {

namespace {

struct Flux {
  f64 h;  // mass, m^2/s
  f64 n;  // normal momentum, m^3/s^2
  f64 t;  // transverse momentum, m^3/s^2
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

// The flux of the system without a bed between two states, each a depth, a normal velocity and a
// transverse velocity. Bollermann et al. (2014), (2.8), with the one-sided local speeds of
// (2.13)-(2.14); the transverse momentum is the same formula's third component. Two identical
// states give the exact flux of that state, which the formula equals in exact arithmetic and which
// keeps a lake at rest exactly at rest in floating point (the flux and the reconstruction's
// correction then cancel to the bit).
inline Flux face_flux(f64 g, f64 hl, f64 ul, f64 vl, f64 hr, f64 ur, f64 vr) noexcept {
  if (hl == hr && ul == ur && vl == vr) {
    const f64 q = hl * ul;
    return {q, q * ul + pressure(g, hl), q * vl};
  }
  const f64 cl = std::sqrt(g * hl);
  const f64 cr = std::sqrt(g * hr);
  const f64 ap = max3(ur + cr, ul + cl, 0.0);
  const f64 am = min3(ur - cr, ul - cl, 0.0);
  const f64 span = ap - am;
  if (!(span > 0.0)) return {0.0, 0.0, 0.0};
  const f64 ql = hl * ul;
  const f64 qr = hr * ur;
  const f64 apam = ap * am;
  const f64 fh = (ap * ql - am * qr + apam * (hr - hl)) / span;
  const f64 fn =
      (ap * (ql * ul + pressure(g, hl)) - am * (qr * ur + pressure(g, hr)) + apam * (qr - ql)) /
      span;
  const f64 ft = (ap * (ql * vl) - am * (qr * vr) + apam * (hr * vr - hl * vl)) / span;
  return {fh, fn, ft};
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
  row_source_.resize(ny, 0.0);
  row_open_.resize(ny, 0.0);
  row_roundoff_.resize(ny, 0.0);
  const u32 x_faces = (nx + 1) * ny;
  const u32 y_faces = nx * (ny + 1);
  fx_h_.resize(x_faces, 0.0);
  fx_nl_.resize(x_faces, 0.0);
  fx_nr_.resize(x_faces, 0.0);
  fx_t_.resize(x_faces, 0.0);
  fx_c_.resize(carried * x_faces, 0.0);
  fy_h_.resize(y_faces, 0.0);
  fy_nl_.resize(y_faces, 0.0);
  fy_nr_.resize(y_faces, 0.0);
  fy_t_.resize(y_faces, 0.0);
  fy_c_.resize(carried * y_faces, 0.0);
}

bool Solver::inputs_fit(const Inputs& in) const noexcept {
  if (cells_ == 0) return false;
  if (in.bed_m.size() != cells_) return false;
  if (!in.manning.empty() && in.manning.size() != cells_) return false;
  if (!in.source_m_per_s.empty() && in.source_m_per_s.size() != cells_) return false;
  return true;
}

template <class F>
void Solver::for_rows(jobs::JobSystem* jobs, u32 rows, F&& body) {
  if (jobs == nullptr) {
    body(0u, rows);
    return;
  }
  jobs->parallel_for(jobs::Pool::Performance, rows, config_.rows_per_job, std::forward<F>(body));
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

// One face between a left (or south) cell and a right (or north) cell. `ln`/`rn` are the cells'
// velocities normal to the face (negated for a closed edge's mirrored ghost), `lt`/`rt` along it.
// Writes the face's mass flux, its normal momentum's flux less each side's reconstructed pressure,
// its transverse momentum's flux, and each carried quantity's flux (the mass flux times the
// concentration of the cell it comes from).
#define ENGINE_WATER_FACE(L, R, LN, LT, RN, RT, OUT_H, OUT_NL, OUT_NR, OUT_T, OUT_C, FACE, FACES) \
  do {                                                                                            \
    const f64 zl_ = bed[L];                                                                       \
    const f64 zr_ = bed[R];                                                                       \
    const f64 zf_ = zl_ > zr_ ? zl_ : zr_;                                                        \
    const f64 el_ = h_[L] + zl_ - zf_;                                                            \
    const f64 er_ = h_[R] + zr_ - zf_;                                                            \
    const f64 hl_ = el_ > 0.0 ? el_ : 0.0;                                                        \
    const f64 hr_ = er_ > 0.0 ? er_ : 0.0;                                                        \
    const Flux f_ = face_flux(g, hl_, (LN), (LT), hr_, (RN), (RT));                               \
    (OUT_H)[FACE] = f_.h;                                                                         \
    (OUT_NL)[FACE] = f_.n - pressure(g, hl_);                                                     \
    (OUT_NR)[FACE] = f_.n - pressure(g, hr_);                                                     \
    (OUT_T)[FACE] = f_.t;                                                                         \
    if (carried != 0) {                                                                           \
      const u32 up_ = f_.h > 0.0 ? (L) : (R);                                                     \
      const f64 hu_up_ = h_[up_];                                                                 \
      for (u32 k_ = 0; k_ < carried; ++k_) {                                                      \
        const f64 conc_ = hu_up_ >= dry ? carried_[k_ * cells_ + up_] / hu_up_ : 0.0;             \
        (OUT_C)[k_ * (FACES) + (FACE)] = f_.h * conc_;                                            \
      }                                                                                           \
    }                                                                                             \
  } while (false)

void Solver::x_face_rows(const Inputs& in, u32 begin, u32 end) noexcept {
  const u32 nx = config_.nx;
  const u32 faces = (nx + 1) * config_.ny;
  const f64 g = config_.gravity;
  const f64 dry = config_.dry_depth_m;
  const u32 carried = config_.carried;
  const f64* bed = in.bed_m.data();
  const f64 west = config_.edges.west == Edge::closed ? -1.0 : 1.0;
  const f64 east = config_.edges.east == Edge::closed ? -1.0 : 1.0;
  for (u32 y = begin; y < end; ++y) {
    const u32 row = y * nx;
    const u32 frow = y * (nx + 1);
    {
      const u32 c = row;  // the west edge: a ghost of cell 0
      ENGINE_WATER_FACE(c, c, west * u_[c], v_[c], u_[c], v_[c], fx_h_, fx_nl_, fx_nr_, fx_t_,
                        fx_c_, frow, faces);
    }
    for (u32 f = 1; f < nx; ++f) {
      const u32 l = row + f - 1;
      const u32 r = row + f;
      ENGINE_WATER_FACE(l, r, u_[l], v_[l], u_[r], v_[r], fx_h_, fx_nl_, fx_nr_, fx_t_, fx_c_,
                        frow + f, faces);
    }
    {
      const u32 c = row + nx - 1;  // the east edge: a ghost of the last cell
      ENGINE_WATER_FACE(c, c, u_[c], v_[c], east * u_[c], v_[c], fx_h_, fx_nl_, fx_nr_, fx_t_,
                        fx_c_, frow + nx, faces);
    }
  }
}

// Face row r lies south of cell row r; rows 0 and ny are the south and north edges.
void Solver::y_face_rows(const Inputs& in, u32 begin, u32 end) noexcept {
  const u32 nx = config_.nx;
  const u32 ny = config_.ny;
  const u32 faces = nx * (ny + 1);
  const f64 g = config_.gravity;
  const f64 dry = config_.dry_depth_m;
  const u32 carried = config_.carried;
  const f64* bed = in.bed_m.data();
  const f64 south = config_.edges.south == Edge::closed ? -1.0 : 1.0;
  const f64 north = config_.edges.north == Edge::closed ? -1.0 : 1.0;
  for (u32 r = begin; r < end; ++r) {
    const u32 frow = r * nx;
    if (r == 0) {
      for (u32 x = 0; x < nx; ++x) {
        const u32 c = x;
        ENGINE_WATER_FACE(c, c, south * v_[c], u_[c], v_[c], u_[c], fy_h_, fy_nl_, fy_nr_, fy_t_,
                          fy_c_, frow + x, faces);
      }
    } else if (r == ny) {
      for (u32 x = 0; x < nx; ++x) {
        const u32 c = (ny - 1) * nx + x;
        ENGINE_WATER_FACE(c, c, v_[c], u_[c], north * v_[c], u_[c], fy_h_, fy_nl_, fy_nr_, fy_t_,
                          fy_c_, frow + x, faces);
      }
    } else {
      for (u32 x = 0; x < nx; ++x) {
        const u32 s = (r - 1) * nx + x;
        const u32 n = r * nx + x;
        ENGINE_WATER_FACE(s, n, v_[s], u_[s], v_[n], u_[n], fy_h_, fy_nl_, fy_nr_, fy_t_, fy_c_,
                          frow + x, faces);
      }
    }
  }
}

#undef ENGINE_WATER_FACE

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
  const f64* manning = in.manning.empty() ? nullptr : in.manning.data();
  const f64* source = in.source_m_per_s.empty() ? nullptr : in.source_m_per_s.data();
  for (u32 y = begin; y < end; ++y) {
    f64 row_source = 0.0;
    f64 row_open = 0.0;
    f64 row_roundoff = 0.0;
    const u32 frow = y * (nx + 1);
    if (config_.edges.west == Edge::open) row_open -= fx_h_[frow] * edge_volume;
    if (config_.edges.east == Edge::open) row_open += fx_h_[frow + nx] * edge_volume;
    if (y == 0 && config_.edges.south == Edge::open) {
      for (u32 x = 0; x < nx; ++x)
        row_open -= fy_h_[x] * edge_volume;
    }
    if (y + 1 == ny && config_.edges.north == Edge::open) {
      for (u32 x = 0; x < nx; ++x)
        row_open += fy_h_[ny * nx + x] * edge_volume;
    }
    for (u32 x = 0; x < nx; ++x) {
      const u32 i = y * nx + x;
      const u32 w = frow + x;
      const u32 e = w + 1;
      const u32 s = y * nx + x;
      const u32 n = s + nx;
      f64 h = h_[i] - lam * ((fx_h_[e] - fx_h_[w]) + (fy_h_[n] - fy_h_[s]));
      f64 hu = hu_[i] - lam * ((fx_nl_[e] - fx_nr_[w]) + (fy_t_[n] - fy_t_[s]));
      f64 hv = hv_[i] - lam * ((fx_t_[e] - fx_t_[w]) + (fy_nl_[n] - fy_nr_[s]));
      for (u32 k = 0; k < carried; ++k) {
        const u32 xo = k * x_faces;
        const u32 yo = k * y_faces;
        f64& q = carried_[k * cells_ + i];
        q = q - lam * ((fx_c_[xo + e] - fx_c_[xo + w]) + (fy_c_[yo + n] - fy_c_[yo + s]));
      }
      // Under the step's limit the depth cannot go negative in exact arithmetic
      // (docs/subsystems/water_state.md, "The step's length"); what rounding leaves below zero is
      // clamped and counted.
      if (h < 0.0) {
        row_roundoff -= h * area;
        h = 0.0;
      }
      if (source != nullptr) {
        const f64 added = source[i] * dt;
        if (added >= 0.0) {
          h += added;
          row_source += added * area;
        } else {
          const f64 taken = -added < h ? -added : h;
          h -= taken;
          row_source -= taken * area;
        }
      }
      f64 speed = 0.0;
      f64 tau_x = 0.0;
      f64 tau_y = 0.0;
      f64 dissipation = 0.0;
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
          dissipation = coeff * speed * speed;
        } else {
          const f64 u1 = hu / h;
          const f64 v1 = hv / h;
          speed = std::sqrt(u1 * u1 + v1 * v1);
        }
      } else {
        hu = 0.0;
        hv = 0.0;
      }
      h_[i] = h;
      hu_[i] = hu;
      hv_[i] = hv;
      speed_[i] = speed;
      tau_x_[i] = tau_x;
      tau_y_[i] = tau_y;
      dissipation_[i] = dissipation;
    }
    row_source_[y] = row_source;
    row_open_[y] = row_open;
    row_roundoff_[y] = row_roundoff;
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
    ledger_.source_m3 += row_source_[y];
    ledger_.open_edge_out_m3 += row_open_[y];
    ledger_.roundoff_m3 += row_roundoff_[y];
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
