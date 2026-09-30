#pragma once

// Earth's air and Earth's stars (docs/subsystems/sky.md): the numbers the provider "earth" hands
// the renderer, and where each comes from.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/scene_gen/sky.h>

namespace engine::sky {

// The air for a Linke-style turbidity T: Bruneton's and Hillaire's Earth (Rayleigh for 680, 550 and
// 440 nm with an 8 km scale height, the ozone tent round 25 km), with an aerosol whose vertical
// optical depth at 550 nm is (T - 1) times the clean air's own, 0.108 — so T is how many clean
// atmospheres of extinction the air has, which is what the Linke factor means — over a 1.2 km scale
// height, falling off with wavelength with an Angstrom exponent of 0.8, scattering 90% of what it
// takes out (single-scattering albedo 0.9) with an asymmetry of 0.76: a dust haze. T of 1 is
// Hillaire's own faint aerosol, 2 very clear, 3 clear, 5 hazy; a desert is usually 2.5 to 4.
scene_gen::Atmosphere earth_atmosphere(f64 turbidity) noexcept;

// The star table, brightest first: `count_for_magnitude(limit)` stars whose magnitudes follow the
// whole sky's counts (log10 N(< m) = 0.68 + 0.5 m: 5 brighter than 0, 170 than 3, 4,800 than 6),
// placed at random on the celestial sphere and gathered towards the galactic plane the fainter they
// are, coloured by a B - V index drawn from the naked-eye stars' two populations (the white A stars
// and the orange K giants). **Procedural and not a catalogue**: the repository takes no binary
// asset, no catalogue here carries a licence this tree accepts without a check nobody has made, and
// a seeded table costs nothing to store. So the constellations are not the real ones; a provider
// with a catalogue replaces this function and nothing else.
Vector<scene_gen::Star> earth_stars(u32 seed, f64 magnitude_limit);
u32 star_count_for_magnitude(f64 magnitude_limit) noexcept;

// A star's irradiance in sun units from its visual magnitude: the sun's is -26.74.
f64 star_irradiance(f64 magnitude) noexcept;

}  // namespace engine::sky
