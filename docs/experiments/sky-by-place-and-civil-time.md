# The sky by place and civil time: sources and accuracy (W185, 2026-11-25)

What the sky provider "earth" computes when a scene gives its sky by a place and a moment on the real calendar instead of a latitude, a day of no year and a moon's age typed by hand ([sky](../subsystems/sky.md#the-sky-by-place-and-civil-time)). The gap was met by the Toronto proof of concept: a photograph's place and clock time (Toronto, EDT) had to be turned into a local solar hour and a moon's age outside the engine before a capture could be set beside it.

## The use, and the accuracy aimed at

A capture set beside a photograph: the sun's direction within **a few tenths of a degree** (its disc is half a degree across), the moon's phase within a few hundredths of its lit fraction. That bound decides what is modelled and what is left out.

## Sources, how far each was read, and their terms

| Source | What it gives here | Read | Terms |
|---|---|---|---|
| The Astronomical Almanac's low-precision solar formula (*The Astronomical Almanac for the Year 2010*, p. C5), as the encyclopaedia quotes it and the tree already used it (`ephemeris.cpp` since 2026-09-30) | the sun's mean longitude 280.460° + 0.9856474° n, mean anomaly 357.528° + 0.9856003° n, the equation of centre's two terms (1.915°, 0.020°), the distance, the obliquity 23.439° − 0.0000004° n, with n the days from J2000.0; apparent coordinates to about 0.01° for 1950–2050, by the encyclopaedia's statement of the Almanac's | the constants in the code are the ones the English Wikipedia article "Position of the Sun" prints, read 2026-11-25 with its attribution to the 2010 Almanac; the Almanac itself was not read; the USNO's "Approximate Solar Coordinates" page, which carries similar equations for 1800–2200 and may differ in the last digits, refused the connection on 2026-11-25 and was not read | the article's text is CC BY-SA 4.0, and only the formula, a fact, is taken; the Almanac is a United States and United Kingdom government publication |
| Meeus, *Astronomical Algorithms*, the moon's main terms, as the tree already used them | the moon's mean longitude, anomaly and argument of latitude in days from J2000.0, the equation of centre (6.289°), the evection (1.274°), the variation (0.658°), the latitude's first term (5.128°), the distance's two largest; a few tenths of a degree | through the tree's own page; the book was not read for W185 | the book is under copyright; its formulae are facts, and nothing was taken from code |
| NOAA Global Monitoring Laboratory, "General Solar Position Calculations" and the solar calculator's notes (gml.noaa.gov/grad/solcalc/calcdetails.html) | that NOAA's calculator rests on Meeus; that its approximations hold best between 1800 and 2100 and its spreadsheets between 1901 and 2099; that its elevations take a piecewise refraction (none above 85°, a tan formula from 5° to 85°, a polynomial below) | the page itself | no copyright or licence statement; "for research and recreational use only" |
| USNO, "Approximate Sidereal Time" (aa.usno.navy.mil/faq/GAST) | Greenwich mean sidereal time GMST = 18.697374558 h + 24.06570982441908 h · D, D the UT days from J2000.0, losing about 0.1 s of time a century | through Hroch's page below and a search engine's summary of the USNO page; the USNO page itself refused the connection | a United States government work |
| F. Hroch, "Approximations of sidereal time" (Masaryk University, integral.physics.muni.cz) | the same GMST formula with both constants written out, attributed to the USNO page, tested against GAST over 2000–2050 | the page itself | © 1997–2022, no licence stated; only a published formula is taken, checked below against an ephemeris |
| JPL Horizons (Solar System Dynamics Group, ssd.jpl.nasa.gov), observer tables from DE441 | the test's reference values: the sun's and the moon's airless apparent topocentric azimuth and elevation, and the moon's illuminated fraction (Illu%), at three places and times | the API's text output itself, 2026-11-25 (queries below) | no terms of use stated on the service's page; values of an ephemeris are facts |

W185 asked for NOAA's or the USNO's almanac values. NOAA's calculator runs in the browser and gives no table a fetch can read, and every USNO page and its API refused the connection on 2026-11-25. So the reference values are **JPL Horizons'**, the planetary ephemeris (DE441) that the almanacs' own tables come from, asked for **airless** values so that they hold the geometry the engine draws, with no refraction to subtract.

## What the civil form computes

- **The moment**: a Gregorian date, a clock time and the clock's offset from UTC give the UT days from J2000.0 (2000-01-01 12:00): `days_from_civil(y, m, d) − 0.5 + (time − offset) / 24`. The date count is the Gregorian leap rule over a March-based year (a leap day the year's last; 400 years are 146,097 days), checked against JD 2440587.5 for 1970-01-01 and round-tripped over 80,000 days. Game time adds to it in UT, so the date advances as the clock passes midnight.
- **The sun and the moon**: the same two formulae the solar form uses, at the moment's own n instead of 2000's run on from a day of the year; the moon at its own mean longitude, so its age is the real one and no `moon_age_days` is given.
- **The sky's turn**: the local sidereal time is GMST plus the longitude (east positive), where the solar form takes it from the hour as the sun's right ascension plus its hour angle. The sun's hour angle follows from it, and the reported hour is the local apparent solar time.

## What is left out, and what each costs

| Left out | Size | Why it can wait |
|---|---|---|
| ΔT (TT − UT, about 69 s in 2025) | 0.0008° of the sun, 0.01° of the moon | both formulae are run in UT |
| Nutation and the equation of the equinoxes (GMST for GAST) | up to 0.005° in longitude, 1.1 s of time (0.005°) in the sky's turn | under the formulae's own error |
| Refraction | 0.57° at the horizon, about 0.16° at 5° up, 0.09° at 10°, 0.02° at 45° (the scale NOAA's piecewise model gives) | the engine draws no refraction anywhere; an elevation it reports is the geometric one, so a photograph's sun within a few degrees of the horizon stands that much higher than the drawn one |
| The moon's parallax | up to about 1° in its place | the place is not a test case; its phase moves by under 0.01 |
| The moon's smaller terms (the annual equation 0.186°, 0.214° of 2M′, 0.114° of 2F, …) | a few tenths of a degree together | under the bound; a phase moves by under 0.01 of its lit fraction |
| Years outside 1900–2100 | the Almanac formula is stated to 0.01° for 1950–2050 only; at 1900 and 2100, half a century past that, its error grows with the drift of the elements it truncates (a few hundredths of a degree is what to expect; not measured here) | refused with a sentence; the span rests on NOAA's statement that its own Meeus-based approximations are very good between 1800 and 2100 and its spreadsheets valid 1901–2099, not on a statement about the Almanac's formula |

## Measured against Horizons

`domain/sky/tests/sky_tests.cpp`, "the civil form's sun and moon against an almanac at three places", with Horizons' values for each moment (`CENTER='coord@399'`, `COORD_TYPE='GEODETIC'`, `QUANTITIES='4'` or `'4,10'`, `APPARENT='AIRLESS'`, the times in UT). Differences are the engine's less Horizons'; the azimuth's is also shown on the sky (times the cosine of the elevation), which is what the test holds to 0.05°; the elevation is held to 0.05° and the lit fraction to 0.02. The lit fraction moves by at most about 0.0087 per degree of elongation (at quadrature), so 0.02 is about 2.3° of elongation and does not check the moon's place: the next table does.

| Place and moment | UT | Horizons sun az / el (°) | engine − Horizons: el, az (°) | Horizons moon lit | engine − Horizons |
|---|---|---|---|---|---|
| Toronto, 43.65 N 79.38 W, 2025-07-15 15:00 EDT (−4) | 2025-07-15 19:00 | 229.673850 / 60.076466 | −0.0003, −0.0060 | 0.7483 | −0.0071 |
| Sydney, 33.87 S 151.21 E, 2025-03-10 10:00 AEDT (+11) | 2025-03-09 23:00 | 65.825220 / 37.687365 | −0.0027, +0.0015 | 0.8235 | −0.0009 |
| Tromsø, 69.65 N 18.96 E, 2025-06-21 23:30 CEST (+2), the midnight sun | 2025-06-21 21:30 | 342.550866 / 4.084453 | −0.0002, −0.0050 | 0.1728 | −0.0019 |

**The moon's place** is held separately, since its lit fraction cannot show it. Horizons' geocentric apparent right ascension and declination of date (`CENTER='500@399'`, `QUANTITIES='2'`) are held against the engine's celestial direction of the moon, within **1.0°**: about three times the largest residual measured, and inside the "few tenths of a degree" the tree's page states for the three-term series. Its topocentric airless azimuth and elevation (the same queries as the sun's, `QUANTITIES='4,10'`) are held against the drawn moon within **2.0°**: that 1.0° plus the moon's horizontal parallax, which the engine does not draw, asin(6,378 km / distance) = 0.90° at apogee to 1.03° at perigee.

| Place | Horizons moon RA / Dec (°) | geocentric angle off | Horizons moon az / el (°) | topocentric angle off |
|---|---|---|---|---|
| Toronto | 354.80019 / −1.78727 | 0.287° | 313.105847 / −38.479259 | 1.039° |
| Sydney | 123.22740 / 24.49926 | 0.152° | 175.172734 / −80.747231 | 0.022° |
| Tromsø | 38.22378 / 19.05876 | 0.198° | 31.522782 / 0.849299 | 1.171° |

The topocentric residuals are the parallax, largest where the moon is low (Tromsø, at the horizon; Toronto, 38° under it) and nearly nothing where it stands near the nadir (Sydney, 81° under the horizon, where parallax shifts it along the line of sight).
The sun is within a hundredth of a degree at all three, a fifth of the bound and a fiftieth of its own disc; the moon's lit fraction within 0.007, which is about what its topocentric parallax and its missing terms predict. Sydney's case crosses the date line (its UT is the day before its clock's date) and is in the southern hemisphere; Tromsø's is near the June solstice at a high latitude, the sun four degrees over the northern horizon at local midnight.

The pins are tolerances, not bits, so no toolchain has to reproduce a number; the sines are the C library's, as the solar form's always were.
