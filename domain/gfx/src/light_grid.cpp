#include <domain/gfx/light_grid.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace engine::gfx {

namespace {

bool directional(const ResolveLight& light) noexcept { return light.direction_type.w >= 1.5f; }

// The reach a light's cells are found for, before the cell's share of the margin: the radius the
// falloff itself implies. `brdf_distance_attenuation` divides by `max(r^2, 1e-8)`, so its window is
// above zero wherever d^2 < max(r^2, 1e-8): a light of radius zero (or under a tenth of a
// millimetre) still lights the points within 1e-4 m of it, at an attenuation near 1e8 when the
// intensity is not zero, and the builder must not skip it.
inline constexpr f64 k_least_reach = 1e-4;
f64 base_reach(const ResolveLight& light) noexcept {
  return std::max(static_cast<f64>(light.position_radius.w), k_least_reach);
}

bool finite(const ResolveLight& light) noexcept {
  return std::isfinite(light.position_radius.x) && std::isfinite(light.position_radius.y) &&
         std::isfinite(light.position_radius.z) && std::isfinite(light.position_radius.w);
}

}  // namespace

bool build_light_grid(std::span<const ResolveLight> lights, std::span<u32> cells,
                      LightGrid& header) noexcept {
  header = LightGrid{};
  const u32 n = static_cast<u32>(lights.size());
  u32 first = 0;
  while (first < n && directional(lights[first]))
    ++first;
  if (first == n) return false;
  for (u32 i = first; i < n; ++i) {
    if (directional(lights[i])) return false;  // a light with no reach behind one with a reach
    // A position or a reach that is not a finite number has no cell to be found in: no grid, and
    // the readers walk the whole array (main's loop), before any of it reaches a cast below.
    if (!finite(lights[i])) return false;
  }
  const u32 words = (n - first + 31) / 32;
  const u64 max_cells = cells.size() / words;
  if (max_cells == 0) return false;

  // The box of every grown sphere, before the cell's share of the margin. Every light past `first`
  // has a reach of at least `k_least_reach`, so the box is never empty.
  f64 lo[3] = {0.0, 0.0, 0.0};
  f64 hi[3] = {0.0, 0.0, 0.0};
  f64 smallest = 0.0;
  for (u32 i = first; i < n; ++i) {
    const f64 r = base_reach(lights[i]);
    const f64 grown = light_grid_margin(r, 0.0);
    const f64 p[3] = {static_cast<f64>(lights[i].position_radius.x),
                      static_cast<f64>(lights[i].position_radius.y),
                      static_cast<f64>(lights[i].position_radius.z)};
    const bool start = i == first;
    for (int a = 0; a < 3; ++a) {
      lo[a] = start ? p[a] - r - grown : std::min(lo[a], p[a] - r - grown);
      hi[a] = start ? p[a] + r + grown : std::max(hi[a], p[a] + r + grown);
    }
    smallest = start ? r : std::min(smallest, r);
  }

  // The cell's edge: the cube root of the box's volume over the room, never under the floor, grown
  // until the box padded by the cell's margin fits in the room. Finite lights make a finite box;
  // the room test is taken on the f64 quotient, so a box too long for any u64 count of cells grows
  // the edge rather than reaching a cast it would overflow.
  const f64 extent[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
  for (int a = 0; a < 3; ++a) {
    if (!std::isfinite(extent[a])) return false;
  }
  f64 edge = std::cbrt(extent[0] * extent[1] * extent[2] / static_cast<f64>(max_cells));
  edge = std::max(edge, static_cast<f64>(k_light_grid_cell_floor) * smallest);
  if (!(edge > 0.0) || !std::isfinite(edge)) return false;
  f32 inv_cell = 0.0f;
  f64 cell_edge = 0.0;
  Vec3 origin{};
  u64 dims[3] = {1, 1, 1};
  for (;;) {
    inv_cell = static_cast<f32>(1.0 / edge);
    if (!(inv_cell > 0.0f)) return false;  // an edge past f32's range: no cell a reader could find
    cell_edge = 1.0 / static_cast<f64>(inv_cell);  // the edge the reader's f32 multiply implies
    const f64 pad = cell_edge / 64.0;
    origin = Vec3{static_cast<f32>(lo[0] - pad), static_cast<f32>(lo[1] - pad),
                  static_cast<f32>(lo[2] - pad)};
    const f64 corner[3] = {static_cast<f64>(origin.x), static_cast<f64>(origin.y),
                           static_cast<f64>(origin.z)};
    f64 total = 1.0;
    for (int a = 0; a < 3; ++a) {
      const f64 span = std::max(1.0, std::ceil((hi[a] + pad - corner[a]) / cell_edge));
      total *= span;
      dims[a] = total <= static_cast<f64>(max_cells) ? static_cast<u64>(span) : 0;
    }
    if (total <= static_cast<f64>(max_cells)) break;
    edge *= 1.0625;
  }

  header.origin_inv_cell = Vec4{origin, inv_cell};
  header.dims_x = static_cast<u32>(dims[0]);
  header.dims_y = static_cast<u32>(dims[1]);
  header.dims_z = static_cast<u32>(dims[2]);
  header.words = words;
  header.first = first;
  header.count = n;
  header.cells = static_cast<u32>(dims[0] * dims[1] * dims[2]);
  std::memset(cells.data(), 0, sizeof(u32) * u64{header.cells} * words);

  const f64 o[3] = {static_cast<f64>(origin.x), static_cast<f64>(origin.y),
                     static_cast<f64>(origin.z)};
  u32 refs = 0;
  for (u32 i = first; i < n; ++i) {
    const f64 r = base_reach(lights[i]);
    const f64 reach = r + light_grid_margin(r, cell_edge);
    const f64 p[3] = {static_cast<f64>(lights[i].position_radius.x),
                      static_cast<f64>(lights[i].position_radius.y),
                      static_cast<f64>(lights[i].position_radius.z)};
    u64 c0[3];
    u64 c1[3];
    for (int a = 0; a < 3; ++a) {
      const f64 a0 = std::floor((p[a] - reach - o[a]) / cell_edge);
      const f64 a1 = std::floor((p[a] + reach - o[a]) / cell_edge);
      const f64 top = static_cast<f64>(dims[a] - 1);
      c0[a] = static_cast<u64>(std::clamp(a0, 0.0, top));
      c1[a] = static_cast<u64>(std::clamp(a1, 0.0, top));
    }
    const u32 bit = i - first;
    const u32 word = bit / 32;
    const u32 mask = 1u << (bit % 32);
    // A row of cells along x at once: with the row's distance from the light in y and z, the
    // sphere covers x within ±h of the light, and the cells that interval meets are exactly the
    // cells of the row whose box is within the reach — the per-cell box test, solved for x.
    const f64 top_x = static_cast<f64>(dims[0] - 1);
    for (u64 z = c0[2]; z <= c1[2]; ++z) {
      const f64 bz0 = o[2] + static_cast<f64>(z) * cell_edge;
      const f64 dz = std::max(0.0, std::max(bz0 - p[2], p[2] - (bz0 + cell_edge)));
      for (u64 y = c0[1]; y <= c1[1]; ++y) {
        const f64 by0 = o[1] + static_cast<f64>(y) * cell_edge;
        const f64 dy = std::max(0.0, std::max(by0 - p[1], p[1] - (by0 + cell_edge)));
        const f64 rest = reach * reach - dy * dy - dz * dz;
        if (rest < 0.0) continue;
        const f64 h = std::sqrt(rest);
        const u64 x0 =
            static_cast<u64>(std::clamp(std::floor((p[0] - h - o[0]) / cell_edge), 0.0, top_x));
        const u64 x1 =
            static_cast<u64>(std::clamp(std::floor((p[0] + h - o[0]) / cell_edge), 0.0, top_x));
        u32* row = cells.data() + ((z * dims[1] + y) * dims[0]) * words + word;
        for (u64 x = x0; x <= x1; ++x) row[x * words] |= mask;
        refs += static_cast<u32>(x1 - x0 + 1);
      }
    }
  }
  header.refs = refs;
  return true;
}

bool light_grid_cell(const LightGrid& grid, Vec3 position, u32& cell) noexcept {
  const f32 inv = grid.origin_inv_cell.w;
  const f32 c[3] = {(position.x - grid.origin_inv_cell.x) * inv,
                    (position.y - grid.origin_inv_cell.y) * inv,
                    (position.z - grid.origin_inv_cell.z) * inv};
  const u32 dims[3] = {grid.dims_x, grid.dims_y, grid.dims_z};
  u32 i[3];
  for (int a = 0; a < 3; ++a) {
    if (!(c[a] >= 0.0f) || !(c[a] < static_cast<f32>(dims[a]))) return false;
    i[a] = std::min(static_cast<u32>(c[a]), dims[a] - 1);
  }
  cell = (i[2] * dims[1] + i[1]) * dims[0] + i[0];
  return true;
}

bool light_grid_culls_at(const LightGrid& grid, std::span<const u32> cells,
                         Vec3 position) noexcept {
  u32 cell = 0;
  if (!light_grid_cell(grid, position, cell)) return grid.count > grid.first;
  const u64 base = u64{cell} * grid.words;
  if (base + grid.words > cells.size()) return true;
  u32 set = 0;
  for (u32 w = 0; w < grid.words; ++w)
    set += static_cast<u32>(std::popcount(cells[base + w]));
  return set < grid.count - grid.first;
}

bool light_grid_reaches_all(std::span<const ResolveLight> lights, Vec3 position) noexcept {
  const u32 n = static_cast<u32>(lights.size());
  u32 first = 0;
  while (first < n && directional(lights[first]))
    ++first;
  if (first == n) return false;
  const f64 at[3] = {static_cast<f64>(position.x), static_cast<f64>(position.y),
                     static_cast<f64>(position.z)};
  for (u32 i = first; i < n; ++i) {
    if (directional(lights[i]) || !finite(lights[i])) return false;
    // The builder's own reach and arithmetic: a cell is marked when its box is within the reach
    // and a margin of the light, and the box holding `position` is no farther than `position` is.
    const f64 r = base_reach(lights[i]);
    const f64 d[3] = {static_cast<f64>(lights[i].position_radius.x) - at[0],
                      static_cast<f64>(lights[i].position_radius.y) - at[1],
                      static_cast<f64>(lights[i].position_radius.z) - at[2]};
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > r * r) return false;
  }
  return true;
}

}  // namespace engine::gfx
