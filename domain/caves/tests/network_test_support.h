#pragma once

// What the cave network tests share: a hillside over a valley floor with one flank cut away (and,
// when asked, a doline), and the network laid in a limestone that rises into the hill.

#include <domain/caves/network.h>

#include <cmath>

namespace engine::caves::test {

// y = max(valley, slope * x - max(0, z)^2 / flank), less a doline `depth` deep and `radius` wide
// at (doline_x, doline_z): a hill that rises toward +x, its +z side cut down by a side valley.
struct Hill {
  f64 valley = 0.0, slope = 0.36, flank = 400.0;
  f64 doline_x = 0.0, doline_z = 0.0, radius = 0.0, depth = 0.0;
};
inline f64 hill_height(const void* c, i64 x_mm, i64 z_mm) noexcept {
  const Hill& h = *static_cast<const Hill*>(c);
  const f64 x = static_cast<f64>(x_mm) * 0.001, z = static_cast<f64>(z_mm) * 0.001;
  const f64 side = z > 0.0 ? z : 0.0;
  f64 y = h.slope * x - side * side / h.flank;
  if (y < h.valley) y = h.valley;
  if (h.radius > 0.0) {
    const f64 dx = (x - h.doline_x) / h.radius, dz = (z - h.doline_z) / h.radius;
    const f64 r2 = dx * dx + dz * dz;
    if (r2 < 1.0) y -= h.depth * (1.0 - r2) * (1.0 - r2);
  }
  return y;
}
inline scene_gen::Ground ground_of(const Hill& h) {
  scene_gen::Ground g;
  g.surface_fn = hill_height;
  g.context = &h;
  return g;
}

// Two levels in 24 m of limestone over shale, the beds dipping 8 degrees toward -x (out of the
// hill), so the member rises into the hill and the upper level lies 65 m further in.
inline Network hill_network(u64 seed = 11) {
  Network n;
  n.format = std::string(k_network_format);
  n.seed = seed;
  n.origin = WorldPos{30.0, 1.0, 0.0};
  scene::Formation f;
  // The bed coordinate is 42 m at the network's origin: the middle of the limestone.
  f.origin = WorldPos{35.846, -40.59, 0.0};
  f.dip_deg = 8.0f;
  f.azimuth_deg = 180.0f;
  const f32 thick[4] = {0.9f, 0.3f, 0.6f, 0.25f};
  const f32 strong[4] = {0.85f, 0.15f, 0.7f, 0.2f};
  for (u32 i = 0; i < 4; ++i) {
    scene::FormationBed bed;
    bed.name = i % 2 == 0 ? "bed" : "parting";
    bed.thickness_m = thick[i];
    bed.resistance = strong[i];
    f.beds.push_back(bed);
  }
  scene::FormationBed shale;
  shale.name = "shale";
  shale.thickness_m = 30.0f;
  shale.resistance = 0.2f;
  scene::FormationBed limestone;
  limestone.name = "limestone";
  limestone.thickness_m = 24.0f;
  limestone.resistance = 0.8f;
  f.macro_beds.push_back(shale);
  f.macro_beds.push_back(limestone);
  n.formation = f;
  n.member = "limestone";
  n.speleothems = Speleothems{};
  return n;
}

}  // namespace engine::caves::test
