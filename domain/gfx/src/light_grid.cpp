#include <domain/gfx/light_grid.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::gfx {

namespace {

bool directional(const ResolveLight& light) noexcept { return light.direction_type.w >= 1.5f; }

// The reach a light's cells are found for, before the cell's share of the margin: zero for a light
// that reaches nothing (`brdf_distance_attenuation` is zero everywhere at a radius of zero).
f64 base_reach(const ResolveLight& light) noexcept {
  const f64 r = light.position_radius.w;
  return r > 0.0 ? r : 0.0;
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
  }
  const u32 words = (n - first + 31) / 32;
  const u64 max_cells = cells.size() / words;
  if (max_cells == 0) return false;

  // The box of every grown sphere, before the cell's share of the margin.
  f64 lo[3] = {0.0, 0.0, 0.0};
  f64 hi[3] = {0.0, 0.0, 0.0};
  f64 smallest = 0.0;
  bool any = false;
  for (u32 i = first; i < n; ++i) {
    const f64 r = base_reach(lights[i]);
    if (r <= 0.0) continue;
    const f64 grown = light_grid_margin(r, 0.0);
    const f64 p[3] = {lights[i].position_radius.x, lights[i].position_radius.y,
                      lights[i].position_radius.z};
    for (int a = 0; a < 3; ++a) {
      lo[a] = any ? std::min(lo[a], p[a] - r - grown) : p[a] - r - grown;
      hi[a] = any ? std::max(hi[a], p[a] + r + grown) : p[a] + r + grown;
    }
    smallest = any ? std::min(smallest, r) : r;
    any = true;
  }
  if (!any) {  // no light reaches anything: one empty cell, and the walk past `first` is empty
    header.origin_inv_cell = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    header.dims_x = header.dims_y = header.dims_z = 1;
    header.words = words;
    header.first = first;
    header.count = n;
    header.cells = 1;
    std::memset(cells.data(), 0, sizeof(u32) * words);
    return true;
  }

  // The cell's edge: the cube root of the box's volume over the room, never under the floor, grown
  // until the box padded by the cell's margin fits in the room.
  const f64 extent[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
  f64 edge = std::cbrt(extent[0] * extent[1] * extent[2] / static_cast<f64>(max_cells));
  edge = std::max(edge, static_cast<f64>(k_light_grid_cell_floor) * smallest);
  f32 inv_cell = 0.0f;
  f64 cell_edge = 0.0;
  Vec3 origin{};
  u64 dims[3] = {1, 1, 1};
  for (;;) {
    inv_cell = static_cast<f32>(1.0 / edge);
    cell_edge = 1.0 / static_cast<f64>(inv_cell);  // the edge the reader's f32 multiply implies
    const f64 pad = cell_edge / 64.0;
    origin = Vec3{static_cast<f32>(lo[0] - pad), static_cast<f32>(lo[1] - pad),
                  static_cast<f32>(lo[2] - pad)};
    const f64 corner[3] = {origin.x, origin.y, origin.z};
    u64 total = 1;
    for (int a = 0; a < 3; ++a) {
      dims[a] =
          std::max<u64>(1, static_cast<u64>(std::ceil((hi[a] + pad - corner[a]) / cell_edge)));
      total *= dims[a];
    }
    if (total <= max_cells) break;
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

  const f64 o[3] = {origin.x, origin.y, origin.z};
  u32 refs = 0;
  for (u32 i = first; i < n; ++i) {
    const f64 r = base_reach(lights[i]);
    if (r <= 0.0) continue;
    const f64 reach = r + light_grid_margin(r, cell_edge);
    const f64 p[3] = {lights[i].position_radius.x, lights[i].position_radius.y,
                      lights[i].position_radius.z};
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
    for (u64 z = c0[2]; z <= c1[2]; ++z) {
      const f64 bz0 = o[2] + static_cast<f64>(z) * cell_edge;
      const f64 dz = std::max({0.0, bz0 - p[2], p[2] - (bz0 + cell_edge)});
      for (u64 y = c0[1]; y <= c1[1]; ++y) {
        const f64 by0 = o[1] + static_cast<f64>(y) * cell_edge;
        const f64 dy = std::max({0.0, by0 - p[1], p[1] - (by0 + cell_edge)});
        for (u64 x = c0[0]; x <= c1[0]; ++x) {
          const f64 bx0 = o[0] + static_cast<f64>(x) * cell_edge;
          const f64 dx = std::max({0.0, bx0 - p[0], p[0] - (bx0 + cell_edge)});
          if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
          cells[((z * dims[1] + y) * dims[0] + x) * words + word] |= mask;
          ++refs;
        }
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

}  // namespace engine::gfx
