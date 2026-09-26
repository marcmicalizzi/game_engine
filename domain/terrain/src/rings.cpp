#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/rings.h>
#include <domain/terrain/stats.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace engine::terrain {

using namespace fx;

namespace {

// The rings' sizes (docs/subsystems/terrain.md, "Rings"). Read when a terrain's rings are laid
// out, never per frame.
tunables::Int inner_half_m{"terrain.rings.inner_half_m", 250, 0, 100'000,
                           "Half the side of the inner terrain ring, metres (0: no inner ring)"};
tunables::Int inner_spacing_cm{"terrain.rings.inner_spacing_cm", 50, 1, 10'000,
                               "The inner terrain ring's grid spacing, centimetres"};
tunables::Int middle_half_m{"terrain.rings.middle_half_m", 1'000, 0, 100'000,
                            "Half the side of the middle terrain ring, metres (0: no middle ring)"};
tunables::Int middle_spacing_cm{"terrain.rings.middle_spacing_cm", 100, 1, 10'000,
                                "The middle terrain ring's grid spacing, centimetres"};
tunables::Int skirt_spacings{"terrain.rings.skirt_spacings", 8, 0, 1'000,
                             "How far a terrain ring's skirts hang below its border, in its own "
                             "grid spacings"};

i64 lcm_i64(i64 a, i64 b) noexcept { return a / std::gcd(a, b) * b; }
i64 round_to(i64 v, i64 step) noexcept { return floor_div(v + step / 2, step) * step; }
i64 ceil_to(i64 v, i64 step) noexcept { return -floor_div(-v, step) * step; }
i64 floor_to(i64 v, i64 step) noexcept { return floor_div(v, step) * step; }

// Ring k's centre on one axis, `want` snapped to its step and clamped so the ring stays inside the
// ring round it (whose centre is `outer_c`), on its step too.
i64 place_axis(const RingParams& params, u32 k, i64 want, i64 outer_c) noexcept {
  const i64 step = ring_snap_mm(params, k);
  const i64 room = params.ring[k + 1].half_mm - params.ring[k].half_mm;
  const i64 lo = ceil_to(outer_c - room, step);
  const i64 hi = floor_to(outer_c + room, step);
  return std::clamp(round_to(want, step), lo, hi);
}

void fill_ring(const RingParams& params, u32 k, i64 cx, i64 cz, Ring& out) noexcept {
  out.cx = cx;
  out.cz = cz;
  out.half = params.ring[k].half_mm;
  out.spacing = params.ring[k].spacing_mm;
  out.skirt = params.ring[k].skirt_mm;
  out.has_hole = false;
}

void link_holes(RingLayout& layout) noexcept {
  for (u32 k = 1; k < layout.count; ++k) {
    Ring& r = layout.ring[k];
    r.has_hole = true;
    r.hole_cx = layout.ring[k - 1].cx;
    r.hole_cz = layout.ring[k - 1].cz;
    r.hole_half = layout.ring[k - 1].half;
  }
}

}  // namespace

RingParams ring_params_from_tunables(i64 cx_mm, i64 cz_mm, i64 outer_half_mm,
                                     i64 outer_spacing_mm) {
  RingParams p;
  p.outer_cx_mm = cx_mm;
  p.outer_cz_mm = cz_mm;
  p.uv_x0_mm = cx_mm - outer_half_mm;
  p.uv_z0_mm = cz_mm - outer_half_mm;
  p.uv_size_mm = 2 * outer_half_mm;
  const i64 skirt = skirt_spacings.get();
  const i64 wanted[2][2] = {
      {inner_half_m.get() * 1000, inner_spacing_cm.get() * 10},
      {middle_half_m.get() * 1000, middle_spacing_cm.get() * 10},
  };
  for (const auto& w : wanted) {
    if (w[0] <= 0) continue;
    p.ring[p.count++] = RingSpec{w[0], w[1], w[1] * skirt};
  }
  p.ring[p.count++] = RingSpec{outer_half_mm, outer_spacing_mm, outer_spacing_mm * skirt};
  // Round each moving ring's half-side to its step, and drop one that does not fit the rules
  // (a tunable set past the next ring, say) rather than refuse the terrain.
  for (u32 k = 0; k + 1 < p.count; ++k)
    p.ring[k].half_mm =
        std::max(ring_snap_mm(p, k), round_to(p.ring[k].half_mm, ring_snap_mm(p, k)));
  while (p.count > 1 && !validate_rings(p)) {
    for (u32 k = 0; k + 1 < p.count; ++k)
      p.ring[k] = p.ring[k + 1];
    --p.count;
  }
  return p;
}

i64 ring_snap_mm(const RingParams& params, u32 k) noexcept {
  if (k + 1 >= params.count) return std::max<i64>(1, params.ring[k].spacing_mm);
  return lcm_i64(std::max<i64>(1, params.ring[k].spacing_mm),
                 std::max<i64>(1, params.ring[k + 1].spacing_mm));
}

bool validate_rings(const RingParams& params, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  if (params.count == 0 || params.count > k_max_rings)
    return fail("terrain rings: between 1 and " + std::to_string(k_max_rings) + " rings");
  if (params.uv_size_mm <= 0) return fail("terrain rings: the UV frame must have a size");
  for (u32 k = 0; k < params.count; ++k) {
    const RingSpec& r = params.ring[k];
    const std::string where = "terrain ring " + std::to_string(k);
    if (r.spacing_mm <= 0 || r.half_mm <= 0 || r.skirt_mm < 0)
      return fail(where + ": its half-side and spacing must be positive, its skirt not negative");
    const i64 step = ring_snap_mm(params, k);
    if (r.half_mm % step != 0)
      return fail(where + ": its half-side must be a multiple of " + std::to_string(step) +
                  " mm, so its edges lie on its grid and the next ring's");
    if (2 * r.half_mm / r.spacing_mm + 1 > k_ring_max_vertices)
      return fail(where + ": more than " + std::to_string(k_ring_max_vertices) +
                  " vertices a side");
    if (k + 1 < params.count) {
      const RingSpec& next = params.ring[k + 1];
      if (next.spacing_mm < r.spacing_mm) return fail(where + ": finer than the ring inside it");
      // The outer ring never moves, so the clamp keeps the last moving ring inside it; the moving
      // rings keep each other inside only if each is three times the one inside it.
      const bool outer = k + 2 == params.count;
      if (next.half_mm - r.half_mm < step || (!outer && next.half_mm < 3 * r.half_mm))
        return fail(where + ": the ring round it must be at least three times as wide");
    }
  }
  const RingSpec& outer = params.ring[params.count - 1];
  if (floor_mod(params.outer_cx_mm - outer.half_mm, outer.spacing_mm) != 0 ||
      floor_mod(params.outer_cz_mm - outer.half_mm, outer.spacing_mm) != 0)
    return fail(
        "terrain rings: the outer ring's corner must lie on its own grid, counted from the "
        "world's origin");
  return true;
}

void place_rings(const RingParams& params, i64 camera_x, i64 camera_z, RingLayout& out) {
  out.count = params.count;
  const u32 last = params.count - 1;
  fill_ring(params, last, params.outer_cx_mm, params.outer_cz_mm, out.ring[last]);
  for (u32 k = last; k-- > 0;) {
    fill_ring(params, k, place_axis(params, k, camera_x, out.ring[k + 1].cx),
              place_axis(params, k, camera_z, out.ring[k + 1].cz), out.ring[k]);
  }
  link_holes(out);
}

u32 recentre_rings(const RingParams& params, i64 camera_x, i64 camera_z, RingLayout& layout) {
  u32 moved = 0;
  const u32 last = layout.count - 1;
  for (u32 k = last; k-- > 0;) {
    Ring& r = layout.ring[k];
    const Ring& round = layout.ring[k + 1];
    const bool far = abs_i64(camera_x - r.cx) > r.half / 2 || abs_i64(camera_z - r.cz) > r.half / 2;
    i64 cx = r.cx, cz = r.cz;
    if (far) {
      cx = place_axis(params, k, camera_x, round.cx);
      cz = place_axis(params, k, camera_z, round.cz);
    } else if ((moved & (1u << (k + 1))) != 0) {
      // The ring round it moved: it stays unless it no longer fits (its centre is on its step
      // already, so snapping it again moves it only if the clamp does).
      cx = place_axis(params, k, cx, round.cx);
      cz = place_axis(params, k, cz, round.cz);
    }
    if (cx != r.cx || cz != r.cz) {
      r.cx = cx;
      r.cz = cz;
      moved |= 1u << k;
    }
  }
  if (moved == 0) return 0;
  link_holes(layout);
  // A ring that moved, and the ring whose hole it is.
  return (moved | (moved << 1)) & ((1u << layout.count) - 1);
}

i32 ring_of(const RingLayout& layout, i64 x, i64 z) noexcept {
  for (u32 k = 0; k < layout.count; ++k) {
    const Ring& r = layout.ring[k];
    if (x >= r.cx - r.half && x < r.cx + r.half && z >= r.cz - r.half && z < r.cz + r.half)
      return static_cast<i32>(k);
  }
  return -1;
}

void FieldRingHeights::heights(i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm,
                               jobs::JobSystem* jobs, Vector<i64>& out_um) const {
  evaluate_grid(field_, x0, z0, nx, nz, spacing_mm, time_us_, detail_, lag_, jobs, out_um);
}

i64 ring_chunk_mm(const Ring& ring) noexcept { return k_ring_chunk_cells * ring.spacing; }

void ring_chunks(const Ring& ring, Vector<RingChunkCoord>& out) {
  out.clear();
  const i64 c = ring_chunk_mm(ring);
  const i64 i0 = floor_div(ring.cx - ring.half, c);
  const i64 i1 = floor_div(ring.cx + ring.half - 1, c);
  const i64 j0 = floor_div(ring.cz - ring.half, c);
  const i64 j1 = floor_div(ring.cz + ring.half - 1, c);
  for (i64 j = j0; j <= j1; ++j) {
    for (i64 i = i0; i <= i1; ++i) {
      // A chunk wholly inside the hole is nothing of this ring's.
      if (ring.has_hole && i * c >= ring.hole_cx - ring.hole_half &&
          (i + 1) * c <= ring.hole_cx + ring.hole_half && j * c >= ring.hole_cz - ring.hole_half &&
          (j + 1) * c <= ring.hole_cz + ring.hole_half)
        continue;
      out.push_back(RingChunkCoord{static_cast<i32>(i), static_cast<i32>(j)});
    }
  }
}

namespace {

// A chunk's cells in world cell indices: the chunk's square clipped to the ring's, and the hole's
// cells clipped to that (empty when the hole misses the chunk).
struct ChunkCells {
  i64 i0 = 0, i1 = 0, j0 = 0, j1 = 0;          // [i0, i1) x [j0, j1)
  i64 hi0 = 0, hi1 = 0, hj0 = 0, hj1 = 0;      // the hole's, clipped; empty when hi0 >= hi1
  bool ring_left = false, ring_right = false;  // which of its sides are the ring's border
  bool ring_low = false, ring_high = false;
};

ChunkCells chunk_cells(const Ring& ring, RingChunkCoord chunk) noexcept {
  const i64 s = ring.spacing;
  const i64 cells = k_ring_chunk_cells;
  ChunkCells c;
  const i64 ri0 = (ring.cx - ring.half) / s, ri1 = (ring.cx + ring.half) / s;
  const i64 rj0 = (ring.cz - ring.half) / s, rj1 = (ring.cz + ring.half) / s;
  c.i0 = std::max<i64>(chunk.i * cells, ri0);
  c.i1 = std::min<i64>((chunk.i + 1) * cells, ri1);
  c.j0 = std::max<i64>(chunk.j * cells, rj0);
  c.j1 = std::min<i64>((chunk.j + 1) * cells, rj1);
  c.ring_left = c.i0 == ri0;
  c.ring_right = c.i1 == ri1;
  c.ring_low = c.j0 == rj0;
  c.ring_high = c.j1 == rj1;
  if (ring.has_hole) {
    c.hi0 = std::max(c.i0, (ring.hole_cx - ring.hole_half) / s);
    c.hi1 = std::min(c.i1, (ring.hole_cx + ring.hole_half) / s);
    c.hj0 = std::max(c.j0, (ring.hole_cz - ring.hole_half) / s);
    c.hj1 = std::min(c.j1, (ring.hole_cz + ring.hole_half) / s);
    if (c.hi0 >= c.hi1 || c.hj0 >= c.hj1) c.hi0 = c.hi1 = c.hj0 = c.hj1 = 0;
  }
  return c;
}

u64 lod_hash(const geometry::ClusterLodMesh& lod) noexcept {
  u64 h = hash_bytes(lod.mesh.vertices.data(), lod.mesh.vertices.size() * sizeof(Vec3));
  h = hash_combine(h,
                   hash_bytes(lod.lod.data(), lod.lod.size() * sizeof(geometry::ClusterLodDesc)));
  return hash_combine(h, hash_bytes(lod.mesh.clusters.data(),
                                    lod.mesh.clusters.size() * sizeof(geometry::ClusterDesc)));
}

}  // namespace

u64 ring_chunk_key(const Ring& ring, RingChunkCoord chunk) noexcept {
  const ChunkCells c = chunk_cells(ring, chunk);
  u64 h = hash_combine(static_cast<u64>(ring.spacing), static_cast<u64>(ring.skirt));
  for (const i64 v : {c.i0, c.i1, c.j0, c.j1, c.hi0, c.hi1, c.hj0, c.hj1})
    h = hash_combine(h, static_cast<u64>(v));
  const u64 sides = (c.ring_left ? 1u : 0u) | (c.ring_right ? 2u : 0u) | (c.ring_low ? 4u : 0u) |
                    (c.ring_high ? 8u : 0u);
  return hash_combine(h, sides);
}

void build_ring_chunk_mesh(const Ring& ring, RingChunkCoord chunk, const RingParams& params,
                           const RingHeights& source, jobs::JobSystem* jobs, RingMesh& out) {
  const i64 s = ring.spacing;
  const ChunkCells c = chunk_cells(ring, chunk);
  const u32 nx = static_cast<u32>(c.i1 - c.i0 + 1);  // vertices
  const u32 nz = static_cast<u32>(c.j1 - c.j0 + 1);
  // The heights with a one-vertex apron, for the normals.
  const u32 ax = nx + 2;
  Vector<i64> h;
  source.heights((c.i0 - 1) * s, (c.j0 - 1) * s, ax, nz + 2, s, jobs, h);
  const auto in_hole = [&](i64 wi, i64 wj) {  // a cell
    return wi >= c.hi0 && wi < c.hi1 && wj >= c.hj0 && wj < c.hj1;
  };
  // A vertex is the chunk's unless every cell round it is the hole's.
  const auto vertex_used = [&](i64 wi, i64 wj) {
    return !(wi > c.hi0 && wi < c.hi1 && wj > c.hj0 && wj < c.hj1);
  };
  const bool hole = c.hi0 < c.hi1;

  out.positions.clear();
  out.normals.clear();
  out.uvs.clear();
  out.indices.clear();
  out.locked.clear();
  Vector<u32> slot(nx * nz, ~0u);
  const f64 uv_scale = 1.0 / static_cast<f64>(params.uv_size_mm);
  for (u32 j = 0; j < nz; ++j) {
    for (u32 i = 0; i < nx; ++i) {
      const i64 wi = c.i0 + i, wj = c.j0 + j;
      if (!vertex_used(wi, wj)) continue;
      slot[j * nx + i] = out.positions.size();
      const i64 x = wi * s;
      const i64 z = wj * s;
      const i64 y = h[(j + 1) * ax + i + 1];
      out.positions.push_back(Vec3{static_cast<f32>(static_cast<f64>(x) / 1000.0),
                                   static_cast<f32>(static_cast<f64>(y) / 1e6),
                                   static_cast<f32>(static_cast<f64>(z) / 1000.0)});
      // Central differences over the apron: (-dh/dx, 1, -dh/dz), both in metres per metre.
      const f64 dx = static_cast<f64>(h[(j + 1) * ax + i + 2] - h[(j + 1) * ax + i]) /
                     (2000.0 * static_cast<f64>(s));
      const f64 dz = static_cast<f64>(h[(j + 2) * ax + i + 1] - h[j * ax + i + 1]) /
                     (2000.0 * static_cast<f64>(s));
      const f64 len = std::sqrt(dx * dx + 1.0 + dz * dz);
      out.normals.push_back(Vec3{static_cast<f32>(-dx / len), static_cast<f32>(1.0 / len),
                                 static_cast<f32>(-dz / len)});
      out.uvs.push_back(Vec2{static_cast<f32>(static_cast<f64>(x - params.uv_x0_mm) * uv_scale),
                             static_cast<f32>(static_cast<f64>(z - params.uv_z0_mm) * uv_scale)});
      // The chunk's border and the hole's edge are locked: a neighbouring chunk (or ring) holds
      // the same line, and the two DAGs meet there exactly only if neither moves it.
      const bool border = i == 0 || j == 0 || i + 1 == nx || j + 1 == nz ||
                          (hole && wi >= c.hi0 && wi <= c.hi1 && wj >= c.hj0 && wj <= c.hj1);
      out.locked.push_back(border ? u8{1} : u8{0});
    }
  }
  out.grid_vertices = out.positions.size();
  // Counter-clockwise seen from +y: the renderer's `build_terrain_mesh` winding.
  for (u32 j = 0; j + 1 < nz; ++j) {
    for (u32 i = 0; i + 1 < nx; ++i) {
      if (in_hole(c.i0 + i, c.j0 + j)) continue;
      const u32 a = slot[j * nx + i];
      const u32 b = slot[j * nx + i + 1];
      const u32 cc = slot[(j + 1) * nx + i];
      const u32 d = slot[(j + 1) * nx + i + 1];
      out.indices.push_back(a);
      out.indices.push_back(cc);
      out.indices.push_back(b);
      out.indices.push_back(b);
      out.indices.push_back(cc);
      out.indices.push_back(d);
    }
  }
  out.grid_triangles = out.indices.size() / 3;

  // The skirts: every edge of the ring's border in this chunk, a quad hanging `skirt` below it,
  // facing away from the ring's own ground — out of the outer square, into the hole.
  const f32 drop = static_cast<f32>(static_cast<f64>(ring.skirt) / 1000.0);
  Vector<u32> lowered(nx * nz, ~0u);
  const auto low = [&](u32 i, u32 j) {
    u32& l = lowered[j * nx + i];
    if (l == ~0u) {
      const u32 top = slot[j * nx + i];
      l = out.positions.size();
      const Vec3 p = out.positions[top];
      out.positions.push_back(Vec3{p.x, p.y - drop, p.z});
      out.normals.push_back(out.normals[top]);
      out.uvs.push_back(out.uvs[top]);
      out.locked.push_back(u8{1});
    }
    return l;
  };
  // An edge from chunk vertex (i0, j0) to (i1, j1) whose skirt faces along (ox, oz).
  const auto edge = [&](u32 i0, u32 j0, u32 i1, u32 j1, i32 ox, i32 oz) {
    const u32 a = slot[j0 * nx + i0];
    const u32 b = slot[j1 * nx + i1];
    const u32 al = low(i0, j0);
    const u32 bl = low(i1, j1);
    // (a, al, b) faces cross(al - a, b - a) = (-d ez, 0, d ex) for an edge e = b - a; keep it if
    // that points along the outward direction, else wind the other way.
    const i32 ex = static_cast<i32>(i1) - static_cast<i32>(i0);
    const i32 ez = static_cast<i32>(j1) - static_cast<i32>(j0);
    const bool keep = -ez * ox + ex * oz > 0;
    const u32 tri[2][3] = {{a, al, b}, {b, al, bl}};
    for (const auto& t : tri) {
      out.indices.push_back(t[0]);
      out.indices.push_back(keep ? t[1] : t[2]);
      out.indices.push_back(keep ? t[2] : t[1]);
    }
  };
  if (ring.skirt > 0) {
    const u32 last_i = nx - 1, last_j = nz - 1;
    for (u32 i = 0; i < last_i; ++i) {
      if (c.ring_low) edge(i, 0, i + 1, 0, 0, -1);
      if (c.ring_high) edge(i, last_j, i + 1, last_j, 0, 1);
    }
    for (u32 j = 0; j < last_j; ++j) {
      if (c.ring_left) edge(0, j, 0, j + 1, -1, 0);
      if (c.ring_right) edge(last_i, j, last_i, j + 1, 1, 0);
    }
    if (hole) {
      // The hole's own sides that fall in this chunk, facing into the hole: its low-z side faces
      // +z, and so on. A side clipped away by the chunk is the next chunk's.
      const i64 s0 = (ring.hole_cx - ring.hole_half) / s, s1 = (ring.hole_cx + ring.hole_half) / s;
      const i64 t0 = (ring.hole_cz - ring.hole_half) / s, t1 = (ring.hole_cz + ring.hole_half) / s;
      const u32 hi0 = static_cast<u32>(c.hi0 - c.i0), hi1 = static_cast<u32>(c.hi1 - c.i0);
      const u32 hj0 = static_cast<u32>(c.hj0 - c.j0), hj1 = static_cast<u32>(c.hj1 - c.j0);
      for (u32 i = hi0; i < hi1; ++i) {
        if (c.hj0 == t0) edge(i, hj0, i + 1, hj0, 0, 1);
        if (c.hj1 == t1) edge(i, hj1, i + 1, hj1, 0, -1);
      }
      for (u32 j = hj0; j < hj1; ++j) {
        if (c.hi0 == s0) edge(hi0, j, hi0, j + 1, 1, 0);
        if (c.hi1 == s1) edge(hi1, j, hi1, j + 1, -1, 0);
      }
    }
  }
  out.skirt_triangles = out.indices.size() / 3 - out.grid_triangles;
}

bool build_ring_chunk(const Ring& ring, RingChunkCoord chunk, const RingParams& params,
                      const RingHeights& source, const geometry::ClusterLodOptions& options,
                      RingChunk& out, std::string* error) {
  out.coord = chunk;
  out.key = ring_chunk_key(ring, chunk);
  RingMesh mesh;
  build_ring_chunk_mesh(ring, chunk, params, source, nullptr, mesh);
  out.grid_triangles = mesh.grid_triangles;
  out.skirt_triangles = mesh.skirt_triangles;
  out.lod = geometry::ClusterLodMesh{};
  if (mesh.indices.empty()) return true;
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(mesh.normals.data(), mesh.normals.size());
  attributes.uvs = std::span<const Vec2>(mesh.uvs.data(), mesh.uvs.size());
  attributes.locked = std::span<const u8>(mesh.locked.data(), mesh.locked.size());
  return geometry::build_cluster_lod(
      std::span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
      std::span<const u32>(mesh.indices.data(), mesh.indices.size()), options, out.lod, error,
      attributes);
}

bool TerrainRings::reset(const RingParams& params, i64 camera_x, i64 camera_z,
                         const RingHeights& source, const geometry::ClusterLodOptions& options,
                         jobs::JobSystem* jobs, std::string* error) {
  params_ = params;
  source_ = &source;
  options_ = options;
  for (RingState& r : rings_)
    r = RingState{};
  place_rings(params_, camera_x, camera_z, layout_);
  return rebuild((1u << layout_.count) - 1, jobs, error);
}

bool TerrainRings::update(i64 camera_x, i64 camera_z, jobs::JobSystem* jobs, u32& rebuilt,
                          std::string* error) {
  rebuilt = recentre_rings(params_, camera_x, camera_z, layout_);
  if (rebuilt == 0) return true;
  return rebuild(rebuilt, jobs, error);
}

bool TerrainRings::rebuild(u32 mask, jobs::JobSystem* jobs, std::string* error) {
  last_built_ = 0;
  last_reused_ = 0;
  for (u32 k = 0; k < layout_.count; ++k) {
    if ((mask & (1u << k)) == 0) continue;
    RingState& state = rings_[k];
    const Ring& ring = layout_.ring[k];
    Vector<RingChunkCoord> coords;
    ring_chunks(ring, coords);
    // Keep every chunk that is still the same cells, hole and border; build the rest. Both lists
    // are in chunk order, so the old one is walked once.
    Vector<RingChunk> next(coords.size());
    Vector<u32> todo;
    u32 old = 0;
    for (u32 c = 0; c < coords.size(); ++c) {
      const u64 key = ring_chunk_key(ring, coords[c]);
      const auto before = [](RingChunkCoord a, RingChunkCoord b) {
        return a.j < b.j || (a.j == b.j && a.i < b.i);
      };
      while (old < state.chunks.size() && before(state.chunks[old].coord, coords[c]))
        ++old;
      if (old < state.chunks.size() && state.chunks[old].coord.i == coords[c].i &&
          state.chunks[old].coord.j == coords[c].j && state.chunks[old].key == key) {
        next[c] = std::move(state.chunks[old++]);
      } else {
        todo.push_back(c);
      }
    }
    last_reused_ += coords.size() - todo.size();
    last_built_ += todo.size();
    Vector<u8> failed(todo.size(), u8{0});
    const auto run = [&](u32 begin, u32 end) {
      for (u32 t = begin; t < end; ++t) {
        if (!build_ring_chunk(ring, coords[todo[t]], params_, *source_, options_, next[todo[t]],
                              nullptr))
          failed[t] = 1u;
      }
    };
    if (jobs == nullptr || todo.size() < 2) {
      run(0, todo.size());
    } else {
      jobs->parallel_for(jobs::Pool::Performance, todo.size(), 1,
                         [&](u32 begin, u32 end) { run(begin, end); });
    }
    for (const u8 f : failed) {
      if (f != 0) {
        if (error != nullptr)
          *error = "terrain ring " + std::to_string(k) + ": a chunk's cluster LOD failed to build";
        return false;
      }
    }
    state.chunks = std::move(next);
    // The ring's DAG: its chunks' merged in chunk order, so the bytes are the same whichever
    // thread built which chunk and whichever chunks were kept from before.
    Vector<geometry::ClusterLodMesh> parts;
    for (const RingChunk& chunk : state.chunks) {
      if (!chunk.lod.mesh.clusters.empty()) parts.push_back(chunk.lod);
    }
    state.lod = geometry::ClusterLodMesh{};
    state.hash = 0;
    if (parts.empty()) continue;
    if (!geometry::merge_cluster_lod(
            std::span<const geometry::ClusterLodMesh>(parts.data(), parts.size()), state.lod,
            nullptr, error))
      return false;
    state.hash = lod_hash(state.lod);
  }
  return true;
}

}  // namespace engine::terrain
