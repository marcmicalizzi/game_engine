#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/stats.h>
#include <domain/terrain/tile.h>

#include <algorithm>

namespace engine::terrain {

namespace {

using namespace fx;

// tan^2 of each bin's upper edge, as the shortest decimal that reads back to the double (Python's
// repr of math.tan(radians(d))**2): constants, so no toolchain's `tan` decides a bin.
constexpr f64 k_tan2_edge[k_slope_bins - 1] = {
    0.007654266245552346,  // 5 degrees
    0.03109120412576338,   // 10
    0.07179676972449082,   // 15
    0.1324743314317942,    // 20
    0.21744283205399903,   // 25
    0.3333333333333333,    // 30
    0.4549617392929703,    // 34, the angle of repose
    0.5278640450004206,    // 36
};

constexpr u32 k_block = 64;

// A point of a crest (a barchan's brink) in the world at the gather's time, mm: `t` from -1 to 1
// along it (Q16), on the side its slip face is.
void crest_point(const Primitive& p, i64 t_q16, i64 dx, i64 dz, i64& x, i64& z) noexcept {
  if (p.kind == static_cast<u8>(PrimitiveKind::transverse)) {
    const i64 along = (static_cast<i64>(p.half_length) * ((t_q16 * 58982) >> 16)) >> 16;
    const i64 sa = abs_i64((t_q16 * 58982) >> 16);
    i64 across = (p.bend * ((sa * sa) >> 16)) >> 16;
    if (p.side_q16 < 0) across = -across;
    if (p.meander != 0) {
      const u32 angle = static_cast<u32>((along * p.inv_meander) >> 16) + p.meander_phase;
      across += (static_cast<i64>(p.meander) * sin_q15(angle)) >> 15;
    }
    x = p.cx + ((p.ax * along + p.az * across) >> 14) + dx;
    z = p.cz + ((p.az * along - p.ax * across) >> 14) + dz;
    return;
  }
  const i64 h_mm = p.height / 1000;
  const i64 scoop_r = (h_mm * 7) / 2;
  const i64 scoop_x = scoop_r + (h_mm * 3) / 10;
  // 180 degrees +- 70: the upwind arc of the scoop, horn to horn.
  const u32 angle = static_cast<u32>(32768 + ((12743 * t_q16) >> 16));
  const i64 lx = scoop_x + ((scoop_r * cos_q15(angle)) >> 15);
  const i64 ly = (scoop_r * sin_q15(angle)) >> 15;
  x = p.cx + ((p.ax * lx - p.az * ly) >> 14) + dx;
  z = p.cz + ((p.az * lx + p.ax * ly) >> 14) + dz;
}

}  // namespace

u32 slope_bin(i64 dx_um, i64 dz_um, i64 spacing_mm) noexcept {
  const f64 gx = static_cast<f64>(dx_um);
  const f64 gz = static_cast<f64>(dz_um);
  const f64 run = static_cast<f64>(2 * spacing_mm) * 1000.0;
  const f64 t2 = (gx * gx + gz * gz) / (run * run);
  u32 bin = 0;
  while (bin < k_slope_bins - 1 && t2 >= k_tan2_edge[bin])
    ++bin;
  return bin;
}

u64 FieldStats::hash() const noexcept {
  u64 h = hash_bytes("engine.terrain.stats", 20);
  const u64 head[] = {static_cast<u64>(x0),
                      static_cast<u64>(z0),
                      nx,
                      nz,
                      static_cast<u64>(spacing_mm),
                      static_cast<u64>(time_us),
                      vertices,
                      sand_vertices,
                      flat,
                      bands,
                      static_cast<u64>(tallest_um),
                      tallest_band,
                      static_cast<u64>(tallest_spacing_mm)};
  for (const u64 v : head)
    h = hash_combine(h, v);
  for (const i64 v : above_floor_um)
    h = hash_combine(h, static_cast<u64>(v));
  for (const u64 v : slope)
    h = hash_combine(h, v);
  for (u32 b = 0; b < bands; ++b) {
    h = hash_combine(h, band[b].primitives);
    h = hash_combine(h, static_cast<u64>(band[b].crest_mm));
    h = hash_combine(h, static_cast<u64>(band[b].tallest_um));
  }
  return h;
}

void evaluate_grid(const DuneField& field, i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm,
                   i64 time_us, Detail detail, const LagField* lag, jobs::JobSystem* jobs,
                   Vector<i64>& heights_um) {
  heights_um.resize(nx * nz);
  const u32 bx = (nx + k_block - 1) / k_block;
  const u32 bz = (nz + k_block - 1) / k_block;
  const auto run = [&](u32 begin, u32 end) {
    Gather gather;
    for (u32 block = begin; block < end; ++block) {
      const u32 i0 = (block % bx) * k_block;
      const u32 j0 = (block / bx) * k_block;
      const u32 i1 = std::min(nx, i0 + k_block);
      const u32 j1 = std::min(nz, j0 + k_block);
      field.gather(x0 + i0 * spacing_mm, z0 + j0 * spacing_mm, x0 + (i1 - 1) * spacing_mm,
                   z0 + (j1 - 1) * spacing_mm, time_us, lag, gather);
      for (u32 j = j0; j < j1; ++j) {
        for (u32 i = i0; i < i1; ++i) {
          heights_um[j * nx + i] =
              field.height_um(gather, x0 + i * spacing_mm, z0 + j * spacing_mm, detail);
        }
      }
    }
  };
  const u32 blocks = bx * bz;
  if (jobs == nullptr || blocks < 2) {
    run(0, blocks);
    return;
  }
  jobs->parallel_for(jobs::Pool::Performance, blocks, 1,
                     [&](u32 begin, u32 end) { run(begin, end); });
}

void field_stats(const DuneField& field, i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm,
                 i64 time_us, Detail detail, const LagField* lag, jobs::JobSystem* jobs,
                 FieldStats& out) {
  out = FieldStats{};
  out.x0 = x0;
  out.z0 = z0;
  out.nx = nx;
  out.nz = nz;
  out.spacing_mm = spacing_mm;
  out.time_us = time_us;
  out.bands = field.band_count();
  // Heights with a one-vertex apron, so every vertex's slope is a central difference.
  const u32 ax = nx + 2;
  const u32 az = nz + 2;
  Vector<i64> heights;
  evaluate_grid(field, x0 - spacing_mm, z0 - spacing_mm, ax, az, spacing_mm, time_us, detail, lag,
                jobs, heights);
  Vector<i64> above;
  above.resize(nx * nz);
  for (u32 j = 0; j < nz; ++j) {
    for (u32 i = 0; i < nx; ++i) {
      const i64 x = x0 + i * spacing_mm;
      const i64 z = z0 + j * spacing_mm;
      const i64 h = heights[(j + 1) * ax + (i + 1)];
      const i64 over = h - field.floor_um(x, z);
      above[j * nx + i] = over;
      out.flat += over <= k_flat_um ? 1u : 0u;
      ++out.vertices;
      if (material_at(field, x, z) == 1) continue;  // rock: not sand
      ++out.sand_vertices;
      const i64 gx = heights[(j + 1) * ax + (i + 2)] - heights[(j + 1) * ax + i];
      const i64 gz = heights[(j + 2) * ax + (i + 1)] - heights[j * ax + (i + 1)];
      ++out.slope[slope_bin(gx, gz, spacing_mm)];
    }
  }
  std::sort(above.begin(), above.end());
  const u64 n = above.size();
  if (n > 0) {
    const u64 at[4] = {n / 10, n / 2, (n * 9) / 10, (n * 99) / 100};
    for (u32 k = 0; k < 4; ++k)
      out.above_floor_um[k] = above[static_cast<u32>(std::min<u64>(at[k], n - 1))];
    out.above_floor_um[4] = above[static_cast<u32>(n - 1)];
  }

  // The primitives centred in the region, and their crest lines inside it.
  const i64 x1 = x0 + static_cast<i64>(nx - 1) * spacing_mm;
  const i64 z1 = z0 + static_cast<i64>(nz - 1) * spacing_mm;
  Gather gather;
  field.gather(x0, z0, x1, z1, time_us, lag, gather);
  const auto inside = [&](i64 x, i64 z) { return x >= x0 && x <= x1 && z >= z0 && z <= z1; };
  constexpr i64 k_steps = 32;
  for (u32 b = 0; b < gather.bands; ++b) {
    BandStats& bs = out.band[b];
    for (u32 k = gather.band_begin[b]; k < gather.band_begin[b + 1]; ++k) {
      const Primitive& p = gather.primitives[k];
      const i64 cx = p.cx + gather.dx[b];
      const i64 cz = p.cz + gather.dz[b];
      // A primitive counts where its band may stand: a crest coupled to the flanks is not there
      // on a floor, and its line is not crest.
      if (inside(cx, cz) && field.band_weight_q16(gather, b, cx, cz) >= k_one_q16 / 2) {
        ++bs.primitives;
        bs.tallest_um = max_i64(bs.tallest_um, p.height);
        if (p.height > out.tallest_um) {
          out.tallest_um = p.height;
          out.tallest_band = b;
        }
      }
      i64 px = 0, pz = 0;
      crest_point(p, -k_one_q16, gather.dx[b], gather.dz[b], px, pz);
      for (i64 s = 1; s <= k_steps; ++s) {
        i64 qx = 0, qz = 0;
        crest_point(p, -k_one_q16 + (2 * k_one_q16 * s) / k_steps, gather.dx[b], gather.dz[b], qx,
                    qz);
        const i64 mx = (px + qx) / 2;
        const i64 mz = (pz + qz) / 2;
        if (inside(mx, mz) && field.band_weight_q16(gather, b, mx, mz) >= k_one_q16 / 2)
          bs.crest_mm += length(qx - px, qz - pz);
        px = qx;
        pz = qz;
      }
    }
  }
  // The tallest band's spacing: each of its primitives in the region to its nearest neighbour
  // (anywhere in the gather), averaged.
  if (out.tallest_um > 0) {
    const u32 b = out.tallest_band;
    i64 sum = 0;
    i64 count = 0;
    for (u32 k = gather.band_begin[b]; k < gather.band_begin[b + 1]; ++k) {
      const Primitive& p = gather.primitives[k];
      if (!inside(p.cx + gather.dx[b], p.cz + gather.dz[b]) ||
          field.band_weight_q16(gather, b, p.cx + gather.dx[b], p.cz + gather.dz[b]) <
              k_one_q16 / 2)
        continue;
      i64 best = -1;
      for (u32 m = gather.band_begin[b]; m < gather.band_begin[b + 1]; ++m) {
        if (m == k) continue;
        const Primitive& q = gather.primitives[m];
        const i64 d = length(q.cx - p.cx, q.cz - p.cz);
        if (best < 0 || d < best) best = d;
      }
      if (best >= 0) {
        sum += best;
        ++count;
      }
    }
    out.tallest_spacing_mm = count > 0 ? sum / count : 0;
  }
}

}  // namespace engine::terrain
