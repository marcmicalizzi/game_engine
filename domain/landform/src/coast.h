#pragma once

// The coast's parts the solve calls (docs/subsystems/landform.md, "The coast";
// docs/experiments/coast.md): the wave base the long run's sea fills to, the outline's distance
// and the bed it starts from, and the stand of the sea after the last step. Private to the module.
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/landform/landform.h>

namespace engine::jobs {
class JobSystem;
}

namespace engine::landform::coast {

/// The lattice in metres: point (i, j) at origin + (i, j) * spacing.
struct Grid {
  u32 w = 0;
  u32 h = 0;
  f64 spacing_m = 0.0;
  f64 origin_x_m = 0.0;
  f64 origin_z_m = 0.0;
};

/// The depth below the level the sea's bed may be built to by the rivers, metres: the sketch's
/// `sea.wave_base_m`, else Hallermeier's outer limit of the coast's climate, else
/// `k_default_wave_base_m`. Zero with no sea.
f64 wave_base_m(const Sketch& s) noexcept;

/// The depth the waves of the sketch's climate reach, metres: Hallermeier's outer limit of the
/// coast's climate, else `k_default_wave_base_m`, whatever `sea.wave_base_m` says. The
/// starting bed's shoreface ends there.
f64 shoreface_depth_m(const Sketch& s) noexcept;

/// Signed distance to the sketch's outline, metres, positive inside the land, for every point.
void outline_distance(const Sketch& s, const Grid& g, jobs::JobSystem* jobs, Vector<f64>& out);

/// The starting bed's depth below the level `outside_m` metres outside the outline: Dean's
/// shoreface down to `shoreface_depth_m`, then the sea's shelf falling to its depth over
/// `shelf_m`.
f64 outline_bed_depth(const Sketch& s, f64 outside_m) noexcept;

/// What the stand writes beside the ground.
struct Stand {
  Vector<u8> substrate;
  Vector<u32> grain;
  Vector<u16> exposure;
  Vector<u8> wave_direction;
  Vector<i32> beach_slope;
  Vector<u32> berm;
  f64 placed = 0.0;  // metres over one cell
  f64 taken = 0.0;
  u32 segments = 0;
  u32 exposed = 0;
};

/// Whether the sketch asks for a stand: a sea, a coast with a climate, and years to stand.
bool stand_runs(const Sketch& s) noexcept;

/// The stand of the sea (docs/subsystems/landform.md, "The coast"): the waterline of `z`, each
/// shore segment's exposure by the climate's azimuth walks, and the beach's section laid where
/// the waves reach. `z` and `sed` are metres and change in the band; held points never do.
void run_stand(const Sketch& s, const Grid& g, const u8* held, f64* z, f64* sed,
               jobs::JobSystem* jobs, Stand& out);

/// The distance from the still waterline of a ground in whole millimetres, millimetres, positive
/// over the sea, held to 2 km.
void shore_distance(const Sketch& s, const Grid& g, const i32* mm, Vector<i32>& out);

}  // namespace engine::landform::coast
