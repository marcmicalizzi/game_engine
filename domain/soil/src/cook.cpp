#include "detail.h"

#include <domain/soil/soil.h>

#include <limits>

namespace engine::soil {
f32 ground_height(const GroundPatch& g, f32 x, f32 z) noexcept {
  if (g.side == 0) return 0;
  const f32 fx = std::clamp((x - g.min_x_m) / g.spacing_m, 0.0f, static_cast<f32>(g.side - 1));
  const f32 fz = std::clamp((z - g.min_z_m) / g.spacing_m, 0.0f, static_cast<f32>(g.side - 1));
  const u32 ix = std::min(static_cast<u32>(fx), g.side - 2),
            iz = std::min(static_cast<u32>(fz), g.side - 2);
  const f32 u = fx - static_cast<f32>(ix), v = fz - static_cast<f32>(iz);
  const auto at = [&](u32 i, u32 j) { return g.heights[j * g.side + i]; };
  return (at(ix, iz) * (1 - u) + at(ix + 1, iz) * u) * (1 - v) +
         (at(ix, iz + 1) * (1 - u) + at(ix + 1, iz + 1) * u) * v;
}
f32 terrain_delta(const SoilFile& f, f32 x, f32 z) noexcept {
  if (f.kind == Kind::TreeThrow) {
    const f32 a = x / (f.width_m * 0.5f), b = z / (f.length_m * 0.5f);
    const f32 pit = std::max(0.0f, 1 - a * a - b * b);
    const f32 mz = (z - f.length_m * 0.75f) / (f.length_m * 0.5f);
    const f32 mound = std::max(0.0f, 1 - a * a - mz * mz);
    return -f.depth_m * pit * pit + f.mound_height_m * mound * mound;
  }
  // A source can extend far beyond this local reach. Avoid evaluating seeded
  // station indices outside it (and leave all such terrain untouched).
  if (std::abs(x) >= f.width_m * 0.5f) return 0;
  const f32 b = detail::back(f);
  z -= detail::planform(f, x);
  f32 cross_section = 0;
  if (f.kind == Kind::Gully) {
    cross_section = detail::smooth((detail::half_channel(f, x) + b - std::abs(z)) / 0.25f);
  } else {
    cross_section = detail::smooth((z + b) / 0.25f) * (1 - detail::smooth(z / f.length_m));
  }
  return -f.depth_m * detail::taper(f, x) * cross_section;
}

bool sample_ground(const scene_gen::Ground& ground, WorldPos origin, f32 radius_m, f32 spacing_m,
                   GroundPatch& out, std::string& error) {
  if (!(radius_m > 0 && radius_m <= 32 && spacing_m >= 0.025f && spacing_m <= 1) ||
      !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
      std::abs(origin.x) > 1.0e8 || std::abs(origin.z) > 1.0e8) {
    error = "soil ground sample: invalid origin, radius or spacing";
    return false;
  }
  GroundPatch next;
  next.side = 2 * static_cast<u32>(std::ceil(radius_m / spacing_m)) + 1;
  if (next.side > 513) {
    error = "soil ground sample exceeds 513 points per side";
    return false;
  }
  next.spacing_m = spacing_m;
  next.min_x_m = next.min_z_m = -static_cast<f32>(next.side / 2) * spacing_m;
  next.heights.reserve(next.side * next.side);
  for (u32 j = 0; j < next.side; ++j)
    for (u32 i = 0; i < next.side; ++i) {
      const WorldPos p{origin.x + static_cast<f64>(next.min_x_m) +
                           static_cast<f64>(i) * static_cast<f64>(spacing_m),
                       origin.y,
                       origin.z + static_cast<f64>(next.min_z_m) +
                           static_cast<f64>(j) * static_cast<f64>(spacing_m)};
      const f64 h = ground.surface(p) - origin.y;
      if (!std::isfinite(h) || std::abs(h) > 10000) {
        error = "soil ground sample is nonfinite or exceeds 10 km local height";
        return false;
      }
      next.heights.push_back(static_cast<f32>(h));
    }
  out = std::move(next);
  return true;
}

bool cook(SoilFile& file, tileset::Heightfield& terrain, WorldPos origin, std::string& error) {
  if (!in_range(file, error)) return false;
  if (terrain.width < 2 || terrain.height < 2 || terrain.spacing_mm < 25 ||
      terrain.spacing_mm > 250 ||
      terrain.data.size() != static_cast<usize>(terrain.width) * terrain.height ||
      !(terrain.vertical_mm_per_unit > 0 && std::isfinite(terrain.vertical_mm_per_unit))) {
    error = "soil cook requires a complete heightfield with 25..250 mm spacing";
    return false;
  }
  for (f32 h : terrain.data)
    if (!std::isfinite(h)) {
      error = "soil cook: nonfinite terrain";
      return false;
    }
  const f64 minx = static_cast<f64>(terrain.origin_x_mm) / 1000.0;
  const f64 minz = static_cast<f64>(terrain.origin_z_mm) / 1000.0;
  const f64 step = static_cast<f64>(terrain.spacing_mm) / 1000.0;
  const f64 r = static_cast<f64>(detail::radius(file)) + step;
  if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
      origin.x - r < minx || origin.z - r < minz ||
      origin.x + r > minx + (terrain.width - 1) * step ||
      origin.z + r > minz + (terrain.height - 1) * step) {
    error = "soil cook footprint and apron must be inside source terrain";
    return false;
  }
  struct View {
    const tileset::Heightfield& field;
  };
  View view{terrain};
  scene_gen::Ground ground;
  ground.context = &view;
  ground.surface_fn = [](const void* context, i64 xmm, i64 zmm) noexcept -> f64 {
    const auto& t = static_cast<const View*>(context)->field;
    const f64 x = static_cast<f64>(xmm - t.origin_x_mm) / static_cast<f64>(t.spacing_mm);
    const f64 z = static_cast<f64>(zmm - t.origin_z_mm) / static_cast<f64>(t.spacing_mm);
    if (x < 0 || z < 0 || x > t.width - 1 || z > t.height - 1)
      return std::numeric_limits<f64>::quiet_NaN();
    const u32 ix = std::min(static_cast<u32>(x), t.width - 2),
              iz = std::min(static_cast<u32>(z), t.height - 2);
    const f64 u = x - ix, v = z - iz;
    const auto at = [&](u32 i, u32 j) { return static_cast<f64>(t.data[j * t.width + i]); };
    return ((at(ix, iz) * (1 - u) + at(ix + 1, iz) * u) * (1 - v) +
            (at(ix, iz + 1) * (1 - u) + at(ix + 1, iz + 1) * u) * v) *
           t.vertical_mm_per_unit / 1000.0;
  };
  SoilFile next = file;
  if (!sample_ground(ground, origin, detail::radius(file), static_cast<f32>(step), next.ground,
                     error) ||
      !in_range(next, error))
    return false;
  auto edited = terrain.data;
  for (u32 j = 0; j < terrain.height; ++j)
    for (u32 i = 0; i < terrain.width; ++i) {
      const f32 x = static_cast<f32>(minx + i * step - origin.x),
                z = static_cast<f32>(minz + j * step - origin.z);
      const f64 h =
          static_cast<f64>(edited[j * terrain.width + i]) +
          static_cast<f64>(terrain_delta(next, x, z)) * 1000.0 / terrain.vertical_mm_per_unit;
      if (!std::isfinite(h) || std::abs(h) > static_cast<f64>(std::numeric_limits<f32>::max())) {
        error = "soil cook height overflow";
        return false;
      }
      edited[j * terrain.width + i] = static_cast<f32>(h);
    }
  terrain.data = std::move(edited);
  file = std::move(next);
  return true;
}
}  // namespace engine::soil
