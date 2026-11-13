// The coast (docs/subsystems/landform.md, "The coast"; docs/experiments/coast.md has the sources
// and every number's standing): the closed forms of the shoreface, the face, the closure depth,
// the breaker and the run-up; the outline's distance and the bed it starts from; and the stand of
// the sea after the last step: the waterline as segments, every point's distance to it, each
// segment's exposure by walks across the lattice's water toward the climate's directions, and the
// beach's section laid where the waves reach. Every decision is IEEE `+ - * /`, `sqrt`, `floor`,
// integers, and `det::exp`, `det::log`, `det::sin` and `det::cos` from `core/math/deterministic.h`.
#include "coast.h"

#include <core/jobs/job_system.h>
#include <core/math/deterministic.h>

#include <cmath>
#include <limits>
#include <utility>

namespace engine::landform {

namespace {

constexpr f64 k_g = 9.81;
constexpr f64 k_pi = 3.14159265358979323846;
// The breaker index, H_b / h_b (McCowan's 0.78; the CERC constants Lim, Hwang and Lee 2022 use).
constexpr f64 k_breaker_index = 0.78;
// Quartz's specific gravity, for Hallermeier's outer limit.
constexpr f64 k_quartz = 2.65;
// The stand's azimuths (docs/experiments/coast.md, "The stand", step 1) and the directions a
// field's `wave_direction` names.
constexpr u32 k_azimuths = 32;
constexpr u32 k_directions = 256;
// A walk's position in 1/65536 of a spacing: integers, so every toolchain walks the same points.
constexpr i64 k_walk_one = 65536;
// `shore_distance` is held to this, millimetres.
constexpr i64 k_shore_far_mm = 2000000;
constexpr u32 k_no_segment = 0xFFFFFFFFu;
constexpr u32 k_row_grain = 16;

template <class F>
void for_range(jobs::JobSystem* jobs, u32 count, u32 grain, F body) {
  if (count == 0) return;
  if (jobs == nullptr) {
    body(u32{0}, count);
    return;
  }
  jobs->parallel_for(jobs::Pool::Performance, count, grain, std::move(body));
}

// x^y for x > 0, through the deterministic exponential and logarithm.
f64 power(f64 x, f64 y) noexcept { return det::exp(y * det::log(x)); }

// A segment of the waterline, in lattice units (a point's i and j), with the unit normal that
// points from the land to the sea.
struct Segment {
  f64 ax = 0.0, az = 0.0, bx = 0.0, bz = 0.0;
  f64 nx = 0.0, nz = 0.0;
};

// Squared distance from (px, pz) to a segment, lattice units, and the parameter of the nearest
// point along it.
f64 distance2(const Segment& s, f64 px, f64 pz) noexcept {
  const f64 ex = s.bx - s.ax;
  const f64 ez = s.bz - s.az;
  const f64 len2 = ex * ex + ez * ez;
  f64 t = 0.0;
  if (len2 > 0.0) {
    t = ((px - s.ax) * ex + (pz - s.az) * ez) / len2;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
  }
  const f64 dx = px - (s.ax + t * ex);
  const f64 dz = pz - (s.az + t * ez);
  return dx * dx + dz * dz;
}

// The points under the level that a sea edge reaches through other such points, four-connected:
// the sea. A hollow inland below the level is not.
void find_sea(const Sketch& s, const Grid& g, const f64* z, f64 level, Vector<u8>& sea) {
  const u32 w = g.w, h = g.h;
  sea.assign(w * h, u8{0});
  Vector<u32> stack;
  const auto seed = [&](u32 i, u32 j) {
    const u32 p = j * w + i;
    if (sea[p] == 0 && z[p] < level) {
      sea[p] = 1;
      stack.push_back(p);
    }
  };
  for (u32 j = 0; j < h; ++j) {
    if (s.edges.west == EdgeKind::Sea) seed(0, j);
    if (s.edges.east == EdgeKind::Sea) seed(w - 1, j);
  }
  for (u32 i = 0; i < w; ++i) {
    if (s.edges.north == EdgeKind::Sea) seed(i, 0);
    if (s.edges.south == EdgeKind::Sea) seed(i, h - 1);
  }
  while (stack.size() > 0) {
    const u32 p = stack[stack.size() - 1];
    stack.pop_back();
    const u32 i = p % w, j = p / w;
    if (i > 0) seed(i - 1, j);
    if (i + 1 < w) seed(i + 1, j);
    if (j > 0) seed(i, j - 1);
    if (j + 1 < h) seed(i, j + 1);
  }
}

// The waterline by marching squares over the lattice's squares, in square order: a corner is sea
// (below the level, below zero) or land (at or above it, or a hollow that is not the sea). A
// square whose corners are sea and land on its two diagonals is read by its centre's mean. The
// crossings are linear along an edge, so the line of a plane is exact.
void find_segments(const Grid& g, const f64* z, const u8* sea, f64 level, Vector<Segment>& out) {
  const u32 w = g.w, h = g.h;
  out.resize(0);
  const auto value = [&](u32 p) {
    const f64 v = z[p] - level;
    if (sea[p] != 0) return v < 0.0 ? v : -1.0e-9;
    return v > 0.0 ? v : 0.0;
  };
  for (u32 j = 0; j + 1 < h; ++j)
    for (u32 i = 0; i + 1 < w; ++i) {
      const u32 pa = j * w + i, pb = pa + 1, pc = pa + w + 1, pd = pa + w;
      const u32 code = (sea[pa] != 0 ? 1u : 0u) | (sea[pb] != 0 ? 2u : 0u) |
                       (sea[pc] != 0 ? 4u : 0u) | (sea[pd] != 0 ? 8u : 0u);
      if (code == 0 || code == 15) continue;
      const f64 va = value(pa), vb = value(pb), vc = value(pc), vd = value(pd);
      const f64 x0 = static_cast<f64>(i), z0 = static_cast<f64>(j);
      // The crossing on each edge: 0 a-b (z = j), 1 b-c (x = i + 1), 2 c-d (z = j + 1), 3 d-a.
      const auto cross = [&](u32 e, f64& cx, f64& cz) {
        f64 v0 = 0.0, v1 = 0.0;
        if (e == 0) {
          v0 = va;
          v1 = vb;
        } else if (e == 1) {
          v0 = vb;
          v1 = vc;
        } else if (e == 2) {
          v0 = vc;
          v1 = vd;
        } else {
          v0 = vd;
          v1 = va;
        }
        const f64 t = v0 / (v0 - v1);
        if (e == 0) {
          cx = x0 + t;
          cz = z0;
        } else if (e == 1) {
          cx = x0 + 1.0;
          cz = z0 + t;
        } else if (e == 2) {
          cx = x0 + 1.0 - t;
          cz = z0 + 1.0;
        } else {
          cx = x0;
          cz = z0 + 1.0 - t;
        }
      };
      u32 pairs[4];
      u32 count = 0;
      const auto pair = [&](u32 e0, u32 e1) {
        pairs[count++] = e0;
        pairs[count++] = e1;
      };
      switch (code) {
        case 1:
        case 14: pair(3, 0); break;
        case 2:
        case 13: pair(0, 1); break;
        case 3:
        case 12: pair(3, 1); break;
        case 4:
        case 11: pair(1, 2); break;
        case 6:
        case 9: pair(0, 2); break;
        case 7:
        case 8: pair(2, 3); break;
        case 5: {
          const bool centre_sea = (va + vb + vc + vd) < 0.0;
          if (centre_sea) {
            pair(0, 1);
            pair(2, 3);
          } else {
            pair(3, 0);
            pair(1, 2);
          }
          break;
        }
        case 10: {
          const bool centre_sea = (va + vb + vc + vd) < 0.0;
          if (centre_sea) {
            pair(3, 0);
            pair(1, 2);
          } else {
            pair(0, 1);
            pair(2, 3);
          }
          break;
        }
        default: break;
      }
      // A sea corner of the square, to point each normal seaward.
      f64 sx = x0, sz = z0;
      if ((code & 1u) != 0) {
        sx = x0;
        sz = z0;
      } else if ((code & 2u) != 0) {
        sx = x0 + 1.0;
        sz = z0;
      } else if ((code & 4u) != 0) {
        sx = x0 + 1.0;
        sz = z0 + 1.0;
      } else {
        sx = x0;
        sz = z0 + 1.0;
      }
      for (u32 k = 0; k < count; k += 2) {
        Segment seg;
        cross(pairs[k], seg.ax, seg.az);
        cross(pairs[k + 1], seg.bx, seg.bz);
        const f64 ex = seg.bx - seg.ax, ez = seg.bz - seg.az;
        const f64 len = std::sqrt(ex * ex + ez * ez);
        if (!(len > 0.0)) continue;
        seg.nx = -ez / len;
        seg.nz = ex / len;
        // In a saddle the nearest sea corner to this segment's middle is the one it bounds.
        f64 cx = sx, cz = sz;
        if (code == 5 || code == 10) {
          const f64 mx = 0.5 * (seg.ax + seg.bx), mz = 0.5 * (seg.az + seg.bz);
          f64 best = std::numeric_limits<f64>::max();
          const u32 corner_bit[4] = {1u, 2u, 4u, 8u};
          const f64 corner_x[4] = {x0, x0 + 1.0, x0 + 1.0, x0};
          const f64 corner_z[4] = {z0, z0, z0 + 1.0, z0 + 1.0};
          for (u32 c = 0; c < 4; ++c) {
            if ((code & corner_bit[c]) == 0) continue;
            const f64 d =
                (corner_x[c] - mx) * (corner_x[c] - mx) + (corner_z[c] - mz) * (corner_z[c] - mz);
            if (d < best) {
              best = d;
              cx = corner_x[c];
              cz = corner_z[c];
            }
          }
        }
        const f64 mx = 0.5 * (seg.ax + seg.bx), mz = 0.5 * (seg.az + seg.bz);
        if (seg.nx * (cx - mx) + seg.nz * (cz - mz) < 0.0) {
          seg.nx = -seg.nx;
          seg.nz = -seg.nz;
        }
        out.push_back(seg);
      }
    }
}

// Every point's nearest segment, by a distance transform that hands each point its neighbours'
// nearest segments and keeps the closest (the exact distance to that segment, ties to the lower
// index), swept forward and back until a round changes nothing. Serial: a point reads what the
// sweep wrote before it.
void nearest_segments(const Grid& g, const Vector<Segment>& segs, Vector<u32>& nearest,
                      Vector<f64>& d2) {
  const u32 w = g.w, h = g.h, n = w * h;
  nearest.assign(n, k_no_segment);
  d2.assign(n, std::numeric_limits<f64>::max());
  const auto offer = [&](u32 p, u32 seg) {
    const f64 d = distance2(segs[seg], static_cast<f64>(p % w), static_cast<f64>(p / w));
    if (d < d2[p] || (d == d2[p] && seg < nearest[p])) {
      d2[p] = d;
      nearest[p] = seg;
      return true;
    }
    return false;
  };
  // Each segment to the four corners of its square, found from its middle.
  for (u32 k = 0; k < segs.size(); ++k) {
    const Segment& s = segs[k];
    const f64 mx = 0.5 * (s.ax + s.bx), mz = 0.5 * (s.az + s.bz);
    u32 i = static_cast<u32>(std::floor(mx));
    u32 j = static_cast<u32>(std::floor(mz));
    if (i + 1 >= w) i = w - 2;
    if (j + 1 >= h) j = h - 2;
    const u32 p = j * w + i;
    offer(p, k);
    offer(p + 1, k);
    offer(p + w, k);
    offer(p + w + 1, k);
  }
  const i32 wi = static_cast<i32>(w), hi = static_cast<i32>(h);
  const i32 fwd_dx[4] = {-1, -1, 0, 1}, fwd_dz[4] = {0, -1, -1, -1};
  for (u32 round = 0; round < 64; ++round) {
    bool changed = false;
    for (u32 pass = 0; pass < 2; ++pass) {
      const i32 sign = pass == 0 ? 1 : -1;
      for (i32 jj = 0; jj < hi; ++jj) {
        const i32 j = pass == 0 ? jj : hi - 1 - jj;
        for (i32 ii = 0; ii < wi; ++ii) {
          const i32 i = pass == 0 ? ii : wi - 1 - ii;
          const u32 p = static_cast<u32>(j * wi + i);
          for (u32 k = 0; k < 4; ++k) {
            const i32 qi = i + sign * fwd_dx[k], qj = j + sign * fwd_dz[k];
            if (qi < 0 || qj < 0 || qi >= wi || qj >= hi) continue;
            const u32 seg = nearest[static_cast<u32>(qj * wi + qi)];
            if (seg != k_no_segment && seg != nearest[p] && offer(p, seg)) changed = true;
          }
        }
      }
    }
    if (!changed) break;
  }
}

// The climate's directions, toward where the waves come from: (sin, -cos) of the compass angle.
struct Directions {
  f64 x[k_directions];
  f64 z[k_directions];
};

const Directions& directions() {
  static const Directions d = [] {
    Directions out{};
    for (u32 k = 0; k < k_directions; ++k) {
      const f64 a = 2.0 * k_pi * static_cast<f64>(k) / static_cast<f64>(k_directions);
      out.x[k] = det::sin(a);
      out.z[k] = -det::cos(a);
    }
    return out;
  }();
  return d;
}

// The azimuth of the stand's 32 a direction falls in, and that azimuth's index among the 256.
u32 azimuth_of(f64 from_deg) noexcept {
  const f64 a = std::floor(from_deg / (360.0 / static_cast<f64>(k_azimuths)) + 0.5);
  const i64 k = static_cast<i64>(a) % static_cast<i64>(k_azimuths);
  return static_cast<u32>(k < 0 ? k + k_azimuths : k);
}

// A walk from a segment's middle toward an azimuth, a spacing a step on integers: true when it
// leaves the lattice through a sea edge before it meets land.
bool walk_open(const Sketch& s, const Grid& g, const u8* sea, f64 mx, f64 mz, i64 dx, i64 dz) {
  const i64 sx = static_cast<i64>(std::floor(mx * static_cast<f64>(k_walk_one) + 0.5));
  const i64 sz = static_cast<i64>(std::floor(mz * static_cast<f64>(k_walk_one) + 0.5));
  const i64 w = g.w, h = g.h;
  for (i64 k = 1;; ++k) {
    const i64 px = (sx + k * dx + k_walk_one / 2) >> 16;
    const i64 pz = (sz + k * dz + k_walk_one / 2) >> 16;
    if (px < 0) return s.edges.west == EdgeKind::Sea;
    if (px >= w) return s.edges.east == EdgeKind::Sea;
    if (pz < 0) return s.edges.north == EdgeKind::Sea;
    if (pz >= h) return s.edges.south == EdgeKind::Sea;
    if (sea[static_cast<u32>(pz * w + px)] == 0) return false;
  }
}

// What a segment of shore takes from the climate.
struct Exposure {
  f64 breaker = 0.0;  // energy-weighted breaker height, m
  f64 deep = 0.0;     // energy-weighted deep-water height, m
  f64 period = 0.0;   // energy-weighted period, s
  u8 direction = 0;   // where the energy comes from, 256ths of a turn
  bool open = false;
};

}  // namespace

// ---- the closed forms ---------------------------------------------------------------------------

// The Coastal Engineering Manual's Table III-3-3 (EM 1110-2-1100, Part III, chapter 3, 2002):
// Dean's A, m^(1/3), from 0.10 mm to 1.09 mm in steps of 0.01 mm ("based on Dean 1978b, values
// recomputed by Dean, June 2001"), as printed.
constexpr f64 k_dean_a[100] = {
    0.063, 0.0672, 0.0714, 0.0756, 0.0798, 0.084,  0.0872, 0.0904, 0.0936, 0.0968,  // 0.1
    0.100, 0.103,  0.106,  0.109,  0.112,  0.115,  0.117,  0.119,  0.121,  0.123,   // 0.2
    0.125, 0.127,  0.129,  0.131,  0.133,  0.135,  0.137,  0.139,  0.141,  0.143,   // 0.3
    0.145, 0.1466, 0.1482, 0.1498, 0.1514, 0.153,  0.1546, 0.1562, 0.1578, 0.1594,  // 0.4
    0.161, 0.1622, 0.1634, 0.1646, 0.1658, 0.167,  0.1682, 0.1694, 0.1706, 0.1718,  // 0.5
    0.173, 0.1742, 0.1754, 0.1766, 0.1778, 0.179,  0.1802, 0.1814, 0.1826, 0.1838,  // 0.6
    0.185, 0.1859, 0.1868, 0.1877, 0.1886, 0.1895, 0.1904, 0.1913, 0.1922, 0.1931,  // 0.7
    0.194, 0.1948, 0.1956, 0.1964, 0.1972, 0.198,  0.1988, 0.1996, 0.2004, 0.2012,  // 0.8
    0.202, 0.2028, 0.2036, 0.2044, 0.2052, 0.206,  0.2068, 0.2076, 0.2084, 0.2092,  // 0.9
    0.210, 0.2108, 0.2116, 0.2124, 0.2132, 0.2140, 0.2148, 0.2156, 0.2164, 0.2172,  // 1.0
};

f64 profile_scale(f64 grain_mm) noexcept {
  // Read linearly between the table's hundredths of a millimetre; past its last row, its last
  // value carried by D^0.48, the exponent of Dean 1991's fit to the same values (authored: the
  // shoreface of shingle is outside what the table was fit to).
  const f64 at = (grain_mm - 0.10) * 100.0;
  if (!(at > 0.0)) return k_dean_a[0];
  if (at >= 99.0) return k_dean_a[99] * power(grain_mm / 1.09, 0.48);
  const f64 low = std::floor(at);
  const u32 k = static_cast<u32>(low);
  const f64 t = at - low;
  return k_dean_a[k] + t * (k_dean_a[k + 1] - k_dean_a[k]);
}

f64 face_slope(f64 grain_mm) noexcept {
  const f64 d = grain_mm - 0.125;
  f64 slope = d > 0.0 ? 0.268 - 0.154 * power(d, -0.145) : 0.0;
  if (slope < 0.01) slope = 0.01;
  if (slope > 0.2) slope = 0.2;
  return slope;
}

f64 closure_depth(f64 storm_height_m, f64 period_s) noexcept {
  const f64 hh = storm_height_m;
  const f64 d = 2.28 * hh - 68.5 * hh * hh / (k_g * period_s * period_s);
  return d > 0.0 ? d : 0.0;
}

f64 outer_closure_depth(f64 height_m, f64 period_s, f64 grain_mm) noexcept {
  const f64 d = grain_mm / 1000.0;
  return 0.018 * height_m * period_s * std::sqrt(k_g / (d * (k_quartz - 1.0)));
}

f64 breaker_height(f64 height_m, f64 period_s, f64 cos_angle) noexcept {
  if (!(cos_angle > 0.0) || !(height_m > 0.0)) return 0.0;
  // Energy flux over parallel contours, deep water to the break: H0^2 (g T / 4 pi) cos a0 =
  // Hb^2 sqrt(g Hb / gamma), the crest square to the shore at the break.
  const f64 flux =
      std::sqrt(k_breaker_index * k_g) * period_s * height_m * height_m * cos_angle / (4.0 * k_pi);
  return power(flux, 0.4);
}

f64 run_up(f64 height_m, f64 period_s, f64 slope) noexcept {
  if (!(height_m > 0.0)) return 0.0;
  const f64 l0 = k_g * period_s * period_s / (2.0 * k_pi);
  const f64 hl = std::sqrt(height_m * l0);
  return 1.1 *
         (0.35 * slope * hl + 0.5 * std::sqrt(height_m * l0 * (0.563 * slope * slope + 0.004)));
}

f64 beach_depth(f64 x_m, f64 scale, f64 slope, f64 berm_m) noexcept {
  if (x_m < 0.0) {
    const f64 up = -x_m * slope;
    return -(up < berm_m ? up : berm_m);
  }
  if (!(x_m > 0.0)) return 0.0;
  const f64 face = slope * x_m;
  const f64 curve = scale * power(x_m, 2.0 / 3.0);
  return face < curve ? face : curve;
}

namespace coast {

f64 wave_base_m(const Sketch& s) noexcept {
  if (!s.sea) return 0.0;
  if (s.sea->wave_base_m) return *s.sea->wave_base_m;
  return shoreface_depth_m(s);
}

f64 shoreface_depth_m(const Sketch& s) noexcept {
  if (!s.sea) return 0.0;
  if (s.coast && s.coast->waves.size() > 0) {
    // The year's significant height and period, energy-weighted over the climate's entries.
    f64 e = 0.0, et = 0.0, shares = 0.0;
    for (u32 k = 0; k < s.coast->waves.size(); ++k) {
      const Wave& wave = s.coast->waves[k];
      const f64 energy = wave.share * wave.height_m * wave.height_m;
      e += energy;
      et += energy * wave.period_s;
      shares += wave.share;
    }
    if (e > 0.0 && shares > 0.0) {
      f64 d = outer_closure_depth(std::sqrt(e / shares), et / e, s.coast->grain_mm);
      if (d > 200.0) d = 200.0;
      return d;
    }
  }
  return k_default_wave_base_m;
}

void outline_distance(const Sketch& s, const Grid& g, jobs::JobSystem* jobs, Vector<f64>& out) {
  out.assign(g.w * g.h, 0.0);
  if (!s.coast) return;
  f64* d = out.data();
  for_range(jobs, g.h, k_row_grain, [&](u32 j0, u32 j1) {
    for (u32 j = j0; j < j1; ++j)
      for (u32 i = 0; i < g.w; ++i) {
        const f64 px = g.origin_x_m + static_cast<f64>(i) * g.spacing_m;
        const f64 pz = g.origin_z_m + static_cast<f64>(j) * g.spacing_m;
        f64 best = std::numeric_limits<f64>::max();
        bool inside = false;
        for (u32 o = 0; o < s.coast->outline.size(); ++o) {
          const auto& pts = s.coast->outline[o].points;
          const u32 m = pts.size();
          for (u32 k = 0; k < m; ++k) {
            const Point& a = pts[k];
            const Point& b = pts[k + 1 < m ? k + 1 : 0];
            // Even-odd crossing of the ray toward +x.
            if ((a.z > pz) != (b.z > pz)) {
              const f64 at = a.x + (pz - a.z) * (b.x - a.x) / (b.z - a.z);
              if (px < at) inside = !inside;
            }
            Segment seg;
            seg.ax = a.x;
            seg.az = a.z;
            seg.bx = b.x;
            seg.bz = b.z;
            const f64 dd = distance2(seg, px, pz);
            if (dd < best) best = dd;
          }
        }
        const f64 dist = std::sqrt(best);
        d[j * g.w + i] = inside ? dist : -dist;
      }
  });
}

f64 outline_bed_depth(const Sketch& s, f64 outside_m) noexcept {
  const f64 wave_base = shoreface_depth_m(s);
  const f64 grain = s.coast ? s.coast->grain_mm : 0.3;
  const f64 scale = profile_scale(grain);
  if (!(outside_m > 0.0)) return 0.0;
  const f64 curve = scale * power(outside_m, 2.0 / 3.0);
  if (curve <= wave_base || !s.sea) return curve;
  // Past the shoreface's end the shelf falls from the wave base to the sea's depth.
  const f64 end = wave_base > 0.0 ? power(wave_base / scale, 1.5) : 0.0;
  const f64 deep = s.sea->depth_m > wave_base ? s.sea->depth_m : wave_base;
  if (!(s.sea->shelf_m > 0.0)) return deep;
  f64 t = (outside_m - end) / s.sea->shelf_m;
  if (t > 1.0) t = 1.0;
  return wave_base + (deep - wave_base) * t;
}

bool stand_runs(const Sketch& s) noexcept {
  return s.sea && s.coast && s.coast->waves.size() > 0 && s.coast->stand_yr > 0;
}

void run_stand(const Sketch& s, const Grid& g, const u8* held, f64* z, f64* sed,
               jobs::JobSystem* jobs, Stand& out) {
  const u32 w = g.w, h = g.h, n = w * h;
  out = Stand{};
  out.substrate.assign(n, u8{0});
  out.grain.assign(n, 0u);
  out.exposure.assign(n, u16{0});
  out.wave_direction.assign(n, u8{0});
  out.beach_slope.assign(n, 0);
  out.berm.assign(n, 0u);
  if (!stand_runs(s)) return;
  const Coast& c = *s.coast;
  const f64 level = s.sea->level_m;
  Vector<u8> sea;
  find_sea(s, g, z, level, sea);
  Vector<Segment> segs;
  find_segments(g, z, sea.data(), level, segs);
  out.segments = segs.size();
  if (segs.size() == 0) return;
  Vector<u32> nearest;
  Vector<f64> d2;
  nearest_segments(g, segs, nearest, d2);

  // ---- 1. exposure: each segment's walks toward the climate's azimuths ------------------------
  const Directions& dirs = directions();
  const u32 entries = c.waves.size();
  Vector<u32> entry_dir(entries);
  for (u32 e = 0; e < entries; ++e)
    entry_dir[e] = azimuth_of(c.waves[e].from_deg) * (k_directions / k_azimuths);
  Vector<Exposure> exposure(segs.size());
  for_range(jobs, segs.size(), 64, [&](u32 k0, u32 k1) {
    for (u32 k = k0; k < k1; ++k) {
      const Segment& seg = segs[k];
      const f64 mx = 0.5 * (seg.ax + seg.bx), mz = 0.5 * (seg.az + seg.bz);
      f64 eb = 0.0, ed = 0.0, et = 0.0, vx = 0.0, vz = 0.0;
      // Two entries of one azimuth share a walk.
      u32 walked_dir = k_directions;
      bool walked_open = false;
      for (u32 e = 0; e < entries; ++e) {
        const Wave& wave = c.waves[e];
        const u32 a = entry_dir[e];
        const f64 cos_angle = seg.nx * dirs.x[a] + seg.nz * dirs.z[a];
        if (!(cos_angle > 0.0) || !(wave.share > 0.0) || !(wave.height_m > 0.0)) continue;
        if (a != walked_dir) {
          walked_dir = a;
          walked_open = walk_open(
              s, g, sea.data(), mx, mz,
              static_cast<i64>(std::floor(dirs.x[a] * static_cast<f64>(k_walk_one) + 0.5)),
              static_cast<i64>(std::floor(dirs.z[a] * static_cast<f64>(k_walk_one) + 0.5)));
        }
        if (!walked_open) continue;
        const f64 hb = breaker_height(wave.height_m, wave.period_s, cos_angle);
        const f64 energy = wave.share * hb * hb;
        eb += energy;
        ed += wave.share * wave.height_m * wave.height_m;
        et += energy * wave.period_s;
        vx += energy * dirs.x[a];
        vz += energy * dirs.z[a];
      }
      Exposure& x = exposure[k];
      if (!(eb > 0.0)) continue;
      x.open = true;
      x.breaker = std::sqrt(eb);
      x.deep = std::sqrt(ed);
      x.period = et / eb;
      // The direction among the 256 nearest the energy's mean, ties to the lower index.
      f64 best = -std::numeric_limits<f64>::max();
      for (u32 d = 0; d < k_directions; ++d) {
        const f64 dot = vx * dirs.x[d] + vz * dirs.z[d];
        if (dot > best) {
          best = dot;
          x.direction = static_cast<u8>(d);
        }
      }
    }
  });
  for (u32 k = 0; k < segs.size(); ++k)
    if (exposure[k].open) ++out.exposed;

  // ---- 2. the section at every point a segment's beach reaches --------------------------------
  const f64 scale = profile_scale(c.grain_mm);
  const f64 slope = face_slope(c.grain_mm);
  const f64 high_water = 0.5 * s.sea->tide_m;
  const Substrate kind = c.grain_mm < 2.0 ? Substrate::Sand : Substrate::Shingle;
  const u32 grain_um = static_cast<u32>(std::floor(c.grain_mm * 1000.0 + 0.5));
  const i32 slope_ppm = static_cast<i32>(std::floor(slope * 1.0e6 + 0.5));
  // Each exposed segment's section: the berm's crest, the closure depth and where it is reached.
  struct Section {
    f64 berm = 0.0;
    f64 closure_x = 0.0;
    f64 land_x = 0.0;
  };
  Vector<Section> sections(segs.size());
  for (u32 k = 0; k < segs.size(); ++k) {
    const Exposure& x = exposure[k];
    if (!x.open) continue;
    Section& sec = sections[k];
    sec.berm = high_water + run_up(x.deep, x.period, slope);
    const f64 closure = closure_depth(c.storm * x.deep, x.period);
    sec.closure_x = closure > 0.0 ? power(closure / scale, 1.5) : 0.0;
    sec.land_x = sec.berm / slope + c.berm_width_m;
  }
  Vector<f64> placed_rows(h, 0.0);
  Vector<f64> taken_rows(h, 0.0);
  for_range(jobs, h, k_row_grain, [&](u32 j0, u32 j1) {
    for (u32 j = j0; j < j1; ++j) {
      f64 placed = 0.0, taken = 0.0;
      for (u32 i = 0; i < w; ++i) {
        const u32 p = j * w + i;
        const u32 k = nearest[p];
        if (k == k_no_segment || held[p] != 0 || !exposure[k].open) continue;
        const Section& sec = sections[k];
        const f64 dist = std::sqrt(d2[p]) * g.spacing_m;
        const f64 x = sea[p] != 0 ? dist : -dist;
        const f64 rock = z[p] - sed[p];
        f64 target = z[p];
        if (x >= 0.0) {
          if (x > 2.0 * sec.closure_x) continue;
          const f64 section = level - beach_depth(x, scale, slope, sec.berm);
          if (x <= sec.closure_x) {
            target = section;
          } else {
            // Between the closure depth and twice its distance the section gives way to the
            // ground, the outer shoreface the storms reach (authored: docs/subsystems/landform.md,
            // "The coast").
            const f64 t = (x - sec.closure_x) / sec.closure_x;
            target = (1.0 - t) * section + t * z[p];
          }
          // The waves move sediment and not rock: rock above the section stands.
          if (target < rock) target = rock;
        } else {
          // On land the beach is laid where it stands above the ground and nothing is taken: a
          // dune, a fan or a cliff behind the berm keeps its ground. A hollow under the level
          // that is not the sea is a lagoon, and keeps its water.
          if (-x > sec.land_x || z[p] < level) continue;
          const f64 section = level - beach_depth(x, scale, slope, sec.berm);
          if (!(section > target)) continue;
          target = section;
        }
        const f64 change = target - z[p];
        if (change > 0.0)
          placed += change;
        else
          taken -= change;
        z[p] = target;
        sed[p] = target - rock > 0.0 ? target - rock : 0.0;
        const f64 cover = sed[p];
        out.substrate[p] = static_cast<u8>(cover >= 0.01 ? kind : Substrate::Rock);
        out.grain[p] = cover >= 0.01 ? grain_um : 0u;
        const f64 mm = std::floor(exposure[k].breaker * 1000.0 + 0.5);
        out.exposure[p] = static_cast<u16>(mm > 65535.0 ? 65535.0 : mm);
        out.wave_direction[p] = exposure[k].direction;
        out.beach_slope[p] = slope_ppm;
        out.berm[p] = static_cast<u32>(std::floor(sec.berm * 1000.0 + 0.5));
      }
      placed_rows[j] = placed;
      taken_rows[j] = taken;
    }
  });
  for (u32 j = 0; j < h; ++j) {
    out.placed += placed_rows[j];
    out.taken += taken_rows[j];
  }
}

void shore_distance(const Sketch& s, const Grid& g, const i32* mm, Vector<i32>& out) {
  const u32 n = g.w * g.h;
  out.assign(n, 0);
  if (!s.sea) return;
  Vector<f64> z(n);
  for (u32 p = 0; p < n; ++p)
    z[p] = static_cast<f64>(mm[p]) / 1000.0;
  // The level as the ground's own millimetres read it.
  const f64 level = std::floor(s.sea->level_m * 1000.0 + 0.5) / 1000.0;
  Vector<u8> sea;
  find_sea(s, g, z.data(), level, sea);
  Vector<Segment> segs;
  find_segments(g, z.data(), sea.data(), level, segs);
  if (segs.size() == 0) {
    for (u32 p = 0; p < n; ++p)
      out[p] = static_cast<i32>(sea[p] != 0 ? k_shore_far_mm : -k_shore_far_mm);
    return;
  }
  Vector<u32> nearest;
  Vector<f64> d2;
  nearest_segments(g, segs, nearest, d2);
  for (u32 p = 0; p < n; ++p) {
    const f64 dmm = std::floor(std::sqrt(d2[p]) * g.spacing_m * 1000.0 + 0.5);
    const i64 v = dmm > static_cast<f64>(k_shore_far_mm) ? k_shore_far_mm : static_cast<i64>(dmm);
    out[p] = static_cast<i32>(sea[p] != 0 ? v : -v);
  }
}

}  // namespace coast

}  // namespace engine::landform
