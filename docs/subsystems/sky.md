# sky (domain, capability)

**Purpose.** The **model** of an Earth sky ([ADR-0048](../adr/0048-the-sky-is-a-providers-model-drawn-by-the-renderer.md)): where the sun and the moon stand on a world's calendar at its latitude and at a game time, what the air is made of, and which stars there are. It registers the sky provider **"earth"** in the scene-generator registry ([scene_gen](scene_gen.md#sky-providers)), which is how a scene's `sky` entry reaches it; it draws nothing. The drawing — the scattering tables, the discs, the stars' spots, the air to a surface, the exposure — is `domain/gfx`'s and the renderer's ([renderer](renderer.md#the-sky), [gfx](gfx.md#the-sky)), and names no provider. Removed (`ENGINE_WITH_SKY=OFF`, and the minimal build), every scene that names no sky draws as before and one that names a sky is refused with the registry's sentence.

**Why it is a capability and not the renderer's.** An atmosphere's coefficients, an ephemeris and a star table are content: a game on another planet, with two moons or with a real catalogue replaces them and changes nothing that draws them. ADR-0027 makes such content removable and reached through a registration point; the registry ADR-0046 built for grounds and placements was the point to reach it through.

## Where the sun and the moon are

`ephemeris(Calendar, time_s, Ephemeris&)` (`include/domain/sky/ephemeris.h`). A **calendar** is the scene's latitude, the day of the year its epoch begins (game time 0 is midnight of that day) and the moon's mean age at the epoch; a **game time** is seconds since the epoch, the clock the terrain's wind reads ([terrain](terrain.md#the-days-wind)).

- **The sun** is the Astronomical Almanac's low-precision formula — its mean longitude and anomaly, the two terms of its equation of centre, the obliquity — good to about a hundredth of a degree near 2000. The world has a calendar and no year, so the elements are 2000's run on continuously from the calendar's day. Its distance moves its illuminance 3.4% over a year.
- **The game's hour is apparent solar time**: the sun is on the meridian at 12:00 every day. That is what a player and a scene's wind, whose afternoon peak is at a local hour, mean by the time of day; the equation of time moves the stars instead of the sun.
- **The moon** is Meeus' main terms — the equation of centre, the evection and the variation in longitude, the first term of its latitude, its distance's two largest — good to a few tenths of a degree. Its mean longitude at the epoch is the sun's plus its age times 360° a synodic month, so the scene's `moon_age_days` is its phase: 0 new, 7.4 first quarter, 14.8 full. It rises about fifty minutes later a night (the test measures 11 to 15.5 degrees of hour angle a day), keeps a month to a few degrees, and its declination stays within 18 to 29 degrees over a year.
- **Its light** is a Lambertian sphere's (`moon_illuminance`): albedo 0.12, its size at its distance, the phase law `((π − i) cos i + sin i) / π` — about a fifth of a lux at full (0.15–0.3 in the test), 1/π of that at a quarter, nothing new.
- **The stars turn with the sky**: the celestial frame's world axes (x the vernal equinox, z the north pole, right-handed) through the local sidereal time, `RA_sun + H_sun`, so the pole stands the latitude high in the north and a star is back where it was a sidereal day later (to 0.13°: an apparent solar day is up to half a minute off the mean) and a degree west of it a solar day later.
- **Not modelled**: the moon's parallax (up to a degree), refraction (half a degree at the horizon, which is why the sun sets a few minutes early), precession, nutation, eclipses. The world is flat for everything but the air, and its frame is y up, north −z, east +x.

## The air

`earth_atmosphere(turbidity)` (`include/domain/sky/earth.h`): Bruneton's and Hillaire's Earth — Rayleigh coefficients for 680, 550 and 440 nm (5.802, 13.558, 33.1 × 10⁻³ km⁻¹) over an 8 km scale height, the ozone layer's absorption in a tent 15 km either side of 25 km, a 6,360 km planet under 100 km of air — and an **aerosol a turbidity makes**: its vertical optical depth at 550 nm is (T − 1) times the clean air's own (0.108), so T is how many clean atmospheres of extinction the air carries, which is what a Linke turbidity factor means, never less than Hillaire's own faint aerosol at T = 1; a 1.2 km scale height; an Ångström exponent of 0.8 across the three wavelengths; nine tenths of what it takes out scattered; an asymmetry of 0.76, a dust haze's forward lobe. T of 2 is very clear, 3 clear, 5 hazy; a desert is usually 2.5–4. The night's own glow — airglow, zodiacal light, the stars too faint to list — is a uniform 1.4–1.9 × 10⁻⁹ sun units per steradian above the horizon, about 2 × 10⁻⁴ cd/m², so a moonless night is not black.

## The stars

`earth_stars(seed, magnitude_limit)`: a **procedural** table, brightest first, whose magnitudes follow the whole sky's counts (log₁₀ N(< m) = 0.68 + 0.5 m, rank by rank: 5 brighter than 0, about 170 than 3, 4,800 than 6, **8,511 to 6.5**, the naked eye's limit and the table's LOD policy, `k_star_magnitude_limit`), placed uniformly on the celestial sphere for the bright ones and gathered towards the galactic plane the fainter they are (the test counts 35% or more of the stars fainter than 5 within 15° of it, where a uniform sky would put 26%), coloured by a B − V index drawn from the naked-eye stars' two populations and turned into linear RGB of luminance 1 through a Planck spectrum (Ballesteros' temperature). **Not a catalogue, on purpose**: the repository takes no binary asset, and no star catalogue has been checked against the licences this tree accepts ([ADR-0014](../adr/0014-apache-2-license-and-dependency-policy.md)); a seeded table costs nothing to store. So the sky has a Milky Way, a Sirius and the right number of stars of each brightness, and not the real constellations. A provider with a catalogue replaces this function and nothing else. `star_irradiance(m)` is 10^(−0.4 (m + 26.74)) sun units.

## Where it attaches

`src/sky_provider.cpp` registers `SkyProviderDesc{"earth", earth_make}` with a static `Registrar`; the module is `WHOLE_ARCHIVE` so the linker keeps it. `earth_make` checks the entry — latitude within ±90°, a day within [1, 366], a moon age within a synodic month, a turbidity within [1, 10], a ground albedo within [0, 1], each refused with a sentence naming the field — and builds the air and, when the entry keeps its stars, the table (seed 1: the stars are Earth's, not a scene's). `state(time_s)` is the ephemeris at the time, handed on as floats. Hosts link it (`engine-view`, `engine-host`, and the tests that read a scene with a sky: the renderer's, the world's, the scene collision's).

## LOD policy

The sky has one observer and no tiers. The star table stops at magnitude 6.5, where a star is under a display's step on a moonless night at the exposure rule's darkest; the ephemeris is one evaluation a frame.

## Determinism

`k_determinism` = **derived**: every output is a closed function of the scene's sky entry and a game time, in `f64`, with nothing carried from one call to the next. Nothing it says is read back into gameplay today; a game that wants the sun's height for heat reads the same function at the tick's time and gets the same answer on every machine the tree builds for (contraction off, [ADR-0035](../adr/0035-no-floating-point-contraction.md)).

## Invariants

- The state is a function of (entry, time) alone; the same time is the same answer.
- The sun is on the meridian at every 12:00; the celestial pole's elevation is the latitude to 10⁻⁹°.
- The star table is the same bytes every time it is made, sorted brightest first, each colour of luminance 1.

## Public API

`include/domain/sky/sky.h` (`k_determinism`, `k_star_magnitude_limit`), `ephemeris.h` (`Calendar`, `Direction`, `Ephemeris`, `ephemeris`, `to_world`, `moon_illuminance`, the constants), `earth.h` (`earth_atmosphere`, `earth_stars`, `star_count_for_magnitude`, `star_irradiance`).

## Testing

`tools/dev.ps1 test -Filter sky`: `tests/sky_tests.cpp`, with no device — the sun at noon through the seasons (60° at 30° N on the equinox, 83.44° at the June solstice, 36.56° in December, overhead at the equator, the same height all day at a pole in its summer, in the north south of the equator), on the meridian at noon, rising in the east and north of east in June, continuous through midnight and a year's end; the celestial pole at the latitude, the frame orthonormal and right-handed, a star back after a sidereal day and a degree west after a solar one; the moon's phase from its age at full, new and a quarter, a month's return, its hour angle's daily lag, its declination's range, its light at full, half and new; the air a turbidity makes; the star table's count, order, colours, galactic crowding, determinism and irradiance; and the provider through the registry, on the world's clock, with its refusals. `tests/size_table.cpp` pins `scene_gen::Star` (28 bytes, the record there are thousands of), `Direction` and `Calendar`. The drawing's tests are the renderer's and `domain/gfx`'s.

## What is left open

A real catalogue (licence first); the planets; the moon's parallax and libration; refraction's early sunrise and late sunset; a southern-hemisphere check against an almanac rather than against the geometry; clouds, which are the participating-media field's far-field layer and not this module's.

## Capability contract (ADR-0027)

One module in `domain/`, `OPTIONAL` (`ENGINE_WITH_SKY`), no schema types of its own (it reads `engine.scene.Sky`), no tick, no pass, no protocol method, no tunable; attaches through the scene-generator registry only; LOD policy and determinism stance above; nothing unused costs anything — a scene with no sky never makes a provider, and a build without the module has no code from it.
