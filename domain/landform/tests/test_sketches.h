#pragma once

// Sketches the landform tests share (docs/subsystems/landform.md, "Testing").
#include <core/hash/hash.h>
#include <domain/landform/landform.h>

namespace landform_test {

using namespace engine;

template <class T>
u64 hash_of(const Vector<T>& v) {
  return hash_bytes(v.data(), static_cast<usize>(v.size()) * sizeof(T));
}

// A sketch that uses every part of the model: a sea edge with its shelf, a closed edge, two open
// ones at a base level above the sea, an outlet inside the lattice, a regional rate, a ridge and a
// raster, a resistant band the ground rises through, erodibility variation, and a lattice that is
// not square and not at the origin.
inline landform::Sketch golden_sketch() {
  landform::Sketch s;
  s.width = 96;
  s.height = 80;
  s.spacing_mm = 100000;
  s.origin_mm = {-3200000, 6400000};
  s.edges.west = landform::EdgeKind::Sea;
  s.edges.east = landform::EdgeKind::Closed;
  s.edges.north = landform::EdgeKind::Open;
  s.edges.south = landform::EdgeKind::Open;
  s.base_level_m = 20.0;
  landform::Sea sea;
  sea.level_m = 0.0;
  sea.shelf_m = 1500.0;
  sea.depth_m = 150.0;
  s.sea = sea;
  landform::Outlet outlet;
  outlet.at = landform::Point{-3200.0 + 6000.0, 6400.0 + 4000.0};
  outlet.level_m = 5.0;
  s.outlets.push_back(outlet);
  s.uplift.regional_mm_per_yr = 0.1;
  landform::Ridge ridge;
  ridge.points.push_back(landform::Point{-3200.0 + 2000.0, 6400.0 + 2000.0});
  ridge.points.push_back(landform::Point{-3200.0 + 5000.0, 6400.0 + 3500.0});
  ridge.points.push_back(landform::Point{-3200.0 + 7000.0, 6400.0 + 6000.0});
  ridge.rate_mm_per_yr = 2.0;
  ridge.reach_m = 2500.0;
  s.uplift.ridges.push_back(ridge);
  landform::UpliftRaster raster;
  raster.width = 3;
  raster.height = 2;
  const f64 rates[6] = {0.0, 0.2, 0.4, 0.3, 0.1, 0.0};
  for (const f64 r : rates)
    raster.rates_mm_per_yr.push_back(r);
  s.uplift.raster = raster;
  s.rock.resistance = 0.2;
  landform::Band soft;
  soft.top_m = 150.0;
  soft.resistance = 0.0;
  landform::Band hard;
  hard.top_m = 300.0;
  hard.resistance = 1.0;
  s.rock.bands.push_back(soft);
  s.rock.bands.push_back(hard);
  s.rock.variation = 0.25;
  s.rock.variation_cells = 8;
  s.erodibility_per_yr = 0.00002;
  s.diffusivity_m2_per_yr = 0.01;
  s.seed = 7;
  s.duration_yr = 400000;
  s.step_yr = 5000;
  return s;
}

// A plain square region: four open edges at zero, the defaults elsewhere.
inline landform::Sketch plain_sketch(u32 side, i64 spacing_mm) {
  landform::Sketch s;
  s.width = side;
  s.height = side;
  s.spacing_mm = spacing_mm;
  s.origin_mm = {0, 0};
  s.base_level_m = 0.0;
  return s;
}

}  // namespace landform_test
