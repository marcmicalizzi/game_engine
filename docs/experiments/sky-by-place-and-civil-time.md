# The sky by place and civil time: sources and accuracy (W185, 2026-11-25)

What the sky provider "earth" computes when a scene gives its sky by a place and a moment on the real calendar instead of a latitude, a day of no year and a moon's age typed by hand ([sky](../subsystems/sky.md#the-sky-by-place-and-civil-time)). The gap was met by the Toronto proof of concept: a photograph's place and clock time (Toronto, EDT) had to be turned into a local solar hour and a moon's age outside the engine before a capture could be set beside it.

## The use, and the accuracy aimed at

A capture set beside a photograph: the sun's direction within **a few tenths of a degree** (its disc is half a degree across), the moon's phase within a few hundredths of its lit fraction. That bound decides what is modelled and what is left out.

## Sources, how far each was read, and their terms

| Source | What it gives here | Read | Terms |
|---|---|---|---|
| The Astronomical Almanac's low-precision solar formula, as the tree already used it (`ephemeris.cpp` since 2026-09-30) | the sun's mean longitude 280.460° + 0.9856474° n, mean anomaly 357.528° + 0.9856003° n, the equation of centre's two terms (1.915°, 0.020°), the distance, the obliquity 23.439° − 0.0000004° n, with n the days from J2000.0; about 0.01° near 2000 | through the tree's own page ([sky](../subsystems/sky.md#where-the-sun-and-the-moon-are)); the USNO's "Approximate Solar Coordinates" page that publishes it (aa.usno.navy.mil/faq/sun_approx) refused the connection from here on 2026-11-25 | a United States government work; formulae are facts |
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
| Years outside 1900–2100 | the Almanac formula's error grows with the centuries | refused with a sentence |

## Measured against Horizons

`domain/sky/tests/sky_tests.cpp`, "the civil form's sun and moon against an almanac at three places", with Horizons' values for each moment (`CENTER='coord@399'`, `COORD_TYPE='GEODETIC'`, `QUANTITIES='4'` or `'4,10'`, `APPARENT='AIRLESS'`, the times in UT). Differences are the engine's less Horizons'; the azimuth's is also shown on the sky (times the cosine of the elevation), which is what the test holds to 0.05°; the elevation is held to 0.05° and the lit fraction to 0.02.

| Place and moment | UT | Horizons sun az / el (°) | engine − Horizons: el, az (°) | Horizons moon lit | engine − Horizons |
|---|---|---|---|---|---|
| Toronto, 43.65 N 79.38 W, 2025-07-15 15:00 EDT (−4) | 2025-07-15 19:00 | 229.673850 / 60.076466 | −0.0003, −0.0060 | 0.7483 | −0.0071 |
| Sydney, 33.87 S 151.21 E, 2025-03-10 10:00 AEDT (+11) | 2025-03-09 23:00 | 65.825220 / 37.687365 | −0.0027, +0.0015 | 0.8235 | −0.0009 |
| Tromsø, 69.65 N 18.96 E, 2025-06-21 23:30 CEST (+2), the midnight sun | 2025-06-21 21:30 | 342.550866 / 4.084453 | −0.0002, −0.0050 | 0.1728 | −0.0019 |

The sun is within a hundredth of a degree at all three, a fiftieth of the bound and of its own disc; the moon's lit fraction within 0.007, which is about what its topocentric parallax and its missing terms predict. Sydney's case crosses the date line (its UT is the day before its clock's date) and is in the southern hemisphere; Tromsø's is near the June solstice at a high latitude, the sun four degrees over the northern horizon at local midnight.

The pins are tolerances, not bits, so no toolchain has to reproduce a number; the sines are the C library's, as the solar form's always were.
