# The wind near the ground, and the sea it raises (W136)

Roadmap W136 (2026-11-16). The owner: "I'd also like to add in variable wind if we didn't already, because then we can exercise some of the larger waves, as well as extra calm water too." This page holds the published models the scene's wind rests on, the numbers chosen from them, and where each one is read; [gfx](../subsystems/gfx.md#the-wind-a-scene-names) says what was built and what it guarantees.

## What the tree had before (2026-11-16)

Read before anything was designed, by directory:

- **`schemas/`**: a water body's own `wind_m_s` and `fetch_km` (`WaterBody` version 3; S93, S94, S100), fixed for the scene; `wind_follow`, which turned a body's waves to **the ground's wind**; the dunes' record (`Terrain.sand_flux`, the storms, the day's cycle and veer: `domain/terrain/wind.h`, ADR-0043), whose strength is a share of the record's mean, never metres a second, and which only the "dunes" ground provider has.
- **Readers**: the water (`systems/renderer/src/scene_water.cpp`) made its sixteen-wave JONSWAP table **once, at load**, from the body's wind and fetch (the ground's wind gave an open body its speed and every following body its direction, at the scene's load time only: "a wind that veers through the day does not turn the waves until the scene is read again"); the shore's train, run-up, arrival and energy-flux fields were built from it at load too. The trees (`GpuScene::flora_frame`) swayed in the host's wind, else the dunes' record at the sky's hour, else the `renderer.flora.wind_*` tunables. The sand's ripples and the volumetrics' dust read the dunes' record.
- **In time**: only the dunes' record moved, and nothing that reads water read it in time. **No scene could say a wind in metres a second that changes, and no sea grew or calmed.** So the row was built, not proved.

## The wind near the ground

**The log profile.** In a neutral surface layer the mean wind rises as U(z) = (u\* / κ) ln(z / z₀), κ = 0.40 (von Kármán's constant), with z₀ the aerodynamic roughness length of the ground upwind (Stull 1988, *An Introduction to Boundary Layer Meteorology*, §9.7; Wieringa 1993, "Representative roughness parameters for homogeneous terrain", *Boundary-Layer Meteorology* 63). Wieringa's classes, the numbers `Wind.roughness_m` names: open sea 0.0002 m, mud flats and snow 0.0005 to 0.001, open grassland 0.03, low crops 0.05 to 0.1, scattered trees and hedges 0.25, forest and towns 0.5 to 1 and more. **The speed the scene says is the ten-minute mean at 10 m**, the WMO's standard height and averaging (WMO No. 8, *Guide to Instruments and Methods of Observation*, ch. 5), because that is the speed every wave-growth law below is written in and the Beaufort scale is defined at.

**How strong.** Long-term mean 10 m wind speeds: 3 to 5 m/s over most land, 6 to 9 m/s over the open ocean and on exposed coasts (the ocean's from scatterometer climatologies, e.g. Risien and Chelton 2008, *J. Phys. Oceanogr.* 38; land from station climatologies). The trees' sway calls a land site's mean day 1: **`k_flora_wind_mean_m_s` = 5 m/s**.

**How it varies over days.** The distribution of hourly or ten-minute means at a site is close to **Weibull**, P(U > u) = exp(−(u/λ)^k), with k about 2 (the Rayleigh case) at most sites, 1.5 at gusty continental ones and up to 3 or more in trade winds (Justus et al. 1978, "Methods for estimating wind speed frequency distributions", *J. Appl. Meteor.* 17; Seguro and Lambert 2000). λ = mean / Γ(1 + 1/k). The climate's `weibull_k` defaults to 2: the standard deviation is then 0.523 of the mean.

**The spectral gap.** Van der Hoven's spectrum of the horizontal wind speed (1957, "Power spectrum of horizontal wind speed in the frequency range from 0.0007 to 900 cycles per hour", *J. Meteor.* 14) has two humps: a **synoptic** one at about four days (the passage of fronts and highs) with a second at a day (the diurnal cycle), and a **turbulent** one at about a minute, with a **gap** between them at periods of ten minutes to two hours that holds little energy. This is what makes a ten-minute mean meaningful, and what splits the model in two: a mean that moves on the weather's scale (hours to days) and gusts on the turbulence's (seconds to minutes) with nothing between. The climate's knots are `synoptic_h` = 48 h apart by default (a mid-latitude coast's weather changes every two to five days); the gusts' sines span 4 minutes to 2 seconds.

**The day.** Over land the mean wind near the ground peaks in the early-to-mid afternoon, when convection mixes the faster air aloft down, and falls near calm at night as the surface layer stabilises; the amplitude is often a third of the daily mean or more, and on a coast the **sea breeze** adds an onshore wind of 3 to 7 m/s through the afternoon (Stull 1988, §1.4 and §14.2; Miller et al. 2003, "Sea breeze: structure, forecasting, and impacts", *Rev. Geophys.* 41). The climate's `diurnal` = 0.3 and `peak_hour` = 15 are those; a sea breeze as its own onshore term was **not built** (it needs the coast's direction: a row below).

**Gusts.** The longitudinal turbulence's spectrum is **Kaimal's**, written in IEC 61400-1 (ed. 3, 2005, annex B) as S(f) = 4 σ² L / U / (1 + 6 f L / U)^(5/3) with L = 8.1 Λ and the turbulence scale Λ = 0.7 z below 60 m (Kaimal et al. 1972, *Q. J. R. Meteorol. Soc.* 98): **L = 56.7 m at 10 m**. Its intensity σ / U over flat ground follows the log law, **I = 1 / ln(z / z₀)** (Wieringa 1973; ESDU 85020 gives the same within the scatter): 0.09 over the sea, 0.17 over grassland, 0.4 over a forest's top. Taylor's frozen-turbulence hypothesis carries the field down the wind at the mean speed, so a gust crosses a scene as a front (Taylor 1938). Eight sines on a fixed logarithmic grid of [0.004, 0.5] Hz carry 0.77 of Kaimal's variance at 10 m/s (the rest is slower than four minutes, inside the gap, or faster than two seconds, which no sway shows); the test measures 0.15 of the mean against I √0.77 = 0.151.

## How the sea answers

**Not at once.** A sea grows with the **fetch** the wind has blown over and the **duration** it has blown, whichever limits it, up to a **fully developed** sea past which a longer fetch or a longer blow adds nothing; when the wind drops or turns, the waves it raised do not stop but leave as **swell**.

**Fetch-limited growth: JONSWAP** (Hasselmann et al. 1973, *Dtsch. Hydrogr. Z.* A8(12)). The water already draws its table from JONSWAP's laws ([ocean surface](ocean-surface.md#the-published-models)): ω_p = 22 (g² / (U₁₀ F))^(1/3), α = 0.076 (U₁₀² / (F g))^0.22, γ = 3.3. Integrating the spectrum gives the sea's variance as α J / ω_p⁴ with J one constant of the shape (the same midpoint integral over [0.3, 6] ω_p that `swash_wave` takes), so **the height goes as U^0.887 F^0.557**: a closed form that inverts exactly, which is what the equivalent-fetch step needs. At 10 m/s over 100 km it is 2.14 m at a 6.2 s peak; at 20 m/s over the beach scene's 60 km, 2.97 m at 6.6 s.

**Full development.** The fetch laws stop at g F / U² = 2.2 × 10⁴, where JONSWAP's peak meets Pierson and Moskowitz's fully developed peak f_p U / g = 0.13 (Pierson and Moskowitz 1964, *J. Geophys. Res.* 69; the water's `k_water_developed_fetch`, S100). **A finding, not changed here:** at that fetch the integrated JONSWAP height is about 0.33 U² / g (3.35 m at 8.2 s at 10 m/s), where Pierson–Moskowitz's own fully developed height is 0.21 U₁₉.₅² / g ≈ 0.24 U₁₀² / g (2.5 m at 10 m/s): the engine's developed sea is **about a third higher than Pierson–Moskowitz's**, because α's fetch law is carried to its end with γ = 3.3 while a developed sea's peak enhancement falls to 1. Every scene drawn since S93 has it; a row below asks for γ to relax to 1 towards full development.

**Duration-limited growth.** The coastal engineering manual's law for the time a sea takes to grow over a fetch, **t = 77.23 x^0.67 / (u^0.34 g^0.33)** (SI; USACE 2002, *Coastal Engineering Manual*, EM 1110-2-1100, Part II, ch. 2, eq. II-2-35), is consistent with JONSWAP's peak law: integrating the peak's group speed g / (2 ω_p) over the fetch gives 66 x^(2/3) / (u g)^(1/3), the same exponents and a sixth less time (the manual's is an empirical fit; it is the one taken). At 20 m/s it takes 5.8 h to grow the sea of a 60 km fetch, and 13 h for 200 km.

**Changing winds: the equivalent-duration method** (the same manual, §II-2-2.e; Shore Protection Manual 1984, §3-V). A sea grown under one wind is, under the next, the sea that wind would raise over an *equivalent fetch* (or duration) — the one at which its height is the same — and it goes on growing from there. A wind that turned by more than 45° starts a new sea and leaves the old as swell (the manual's rule). Stepped over the wind's history, this gives the sea at any time from the winds before it. The engine steps half-hours of a fixed grid of the clock over the 72 hours before the time asked (a gale over 1,000 km grows in 33 h), each at the wind of its middle, the last step partial: **a function of the time alone**, so a capture at t is the same picture whatever frames came before (`gfx::sea_state`; the test holds it continuous across the steps).

**The wind that raises nothing.** Under about 1 m/s at 10 m no wind waves form: the first wavelets appear at about that speed (Kahma and Donelan 1988, "A laboratory study of the minimum wind speed for wind wave generation", *J. Fluid Mech.* 192), and the smallest gravity-capillary wave travels at 0.23 m/s at 1.7 cm (Lamb 1932; [shore mechanics](shore-mechanics.md)). `k_sea_min_wind_m_s` = 1: below it the table is glass, and a sea that was there becomes swell.

**Decay to swell.** When the wind drops the short waves stop being forced and are lost within the hour to whitecapping and the non-linear transfer, and the long waves run on at their group speed, c_g = g T / 4π (Snodgrass et al. 1966, "Propagation of ocean swell across the Pacific", *Phil. Trans. R. Soc.* A259; Ardhuin et al. 2009, *J. Phys. Oceanogr.* 39, measure the open ocean's swell losing energy over 3,000 to 20,000 km). Locally, in the generation area, what decides how long a swell stays is that it **leaves**: the engine keeps the energy the wind can no longer hold at the period it had, and drains it with an e-folding time of fetch / c_g — 2.7 h for an 8 s swell over 60 km, eleven minutes for a lake's 1.5 s waves over 900 m. The ocean's dissipation over a scene's distances is negligible beside it and is left out.

**A lake.** Its fetch is its own reach along the wind (the water's `closed_fetch_m`, S100), a few hundred metres to kilometres: a 7 m/s wind over 900 m raises 0.11 m at 1.15 s and is grown in half an hour, so a lake follows its wind within the hour and calms in minutes.

## The check table: the Beaufort scale's sea

WMO's sea descriptions with the open ocean's probable heights (WMO No. 558, *Manual on Marine Meteorological Services*, and the Met Office's *Marine Observer's Handbook*); a sea with a short fetch or duration is lower. The pictures are read against the words, never against the heights alone.

| Force | m/s at 10 m | The sea | Probable height, m |
|---|---|---|---|
| 0 calm | < 0.3 | like a mirror | 0 |
| 1 light air | 0.3–1.5 | ripples with the appearance of scales, no foam crests | 0.1 |
| 2 light breeze | 1.6–3.3 | small wavelets, crests of glassy appearance, not breaking | 0.2 |
| 3 gentle breeze | 3.4–5.4 | large wavelets, crests begin to break, scattered white horses | 0.6 |
| 4 moderate breeze | 5.5–7.9 | small waves becoming longer, fairly frequent white horses | 1 |
| 5 fresh breeze | 8.0–10.7 | moderate waves taking a more pronounced long form, many white horses, chance of spray | 2 |
| 6 strong breeze | 10.8–13.8 | large waves forming, white foam crests more extensive, probably some spray | 3 |
| 7 near gale | 13.9–17.1 | sea heaps up, white foam from breaking waves blown in streaks along the wind | 4 |
| 8 gale | 17.2–20.7 | moderately high waves of greater length, edges of crests break into spindrift, foam in well-marked streaks | 5.5 |
| 9 strong gale | 20.8–24.4 | high waves, dense streaks of foam, crests topple, tumble and roll over, spray may affect visibility | 7 |
| 10 storm | 24.5–28.4 | very high waves with long overhanging crests, the sea takes a white appearance | 9 |

## The numbers chosen, and where they are read

| Number | Value | Source | Read in |
|---|---|---|---|
| Height the wind is said at | 10 m, 10-min mean | WMO No. 8 | `Wind.speed_m_s`, every law below |
| Turbulence intensity | 1 / ln(10 m / z₀) | Wieringa 1973, 1993 | `gfx::wind_turbulence` |
| Kaimal length | 8.1 × 0.7 × 10 = 56.7 m | IEC 61400-1 ed. 3 | `k_wind_kaimal_length_m` |
| Gusts' band | 0.004 to 0.5 Hz, 8 sines | the spectral gap (Van der Hoven 1957) | `wind_gust` |
| Weibull shape | 2 | Justus et al. 1978 | `WindClimate.weibull_k` |
| Synoptic knots | 48 h | Van der Hoven's 4-day hump | `WindClimate.synoptic_h` |
| Day's swing, peak | 0.3, 15:00 | Stull 1988 | `WindClimate.diurnal`, `peak_hour` |
| Trees' mean day | 5 m/s | land climatologies | `k_flora_wind_mean_m_s` |
| Growth with fetch | JONSWAP's α, ω_p, γ 3.3 | Hasselmann et al. 1973 | `sea_height`, `water_spectrum_on_grid` |
| Full development | g F / U² = 2.2 × 10⁴ | Hasselmann et al. 1973; Pierson and Moskowitz 1964 | `water_grown_fetch` |
| Growth with duration | t = 77.23 x^0.67 / (u^0.34 g^0.33) | CEM II-2-35 | `sea_duration_s` |
| A new sea | the wind turned past 45° | CEM II-2 | `k_sea_turn_cos` |
| No waves under | 1 m/s | Kahma and Donelan 1988 | `k_sea_min_wind_m_s` |
| Swell's drain | e-folds over fetch / c_g | group speed; Snodgrass et al. 1966 | `sea_step` |
| History | 72 h in 30 min steps | a gale's growth over 1,000 km is 33 h | `k_sea_history_s`, `k_sea_step_s` |

## Not grounded here, and why

The wind's **direction near a coast** (the sea breeze, a land breeze at night), **terrain's speed-up** over a hill and its lee (Jackson and Hunt 1975), a forest's **sheltering** of the ground and a lake within it, and **squalls** (a gust front's minutes of a doubled wind) are each a model of the wind at a place, where everything built here is uniform over the scene but the gusts' travel; they are rows below, and the interface (`wind(place, time)`) is shaped to take them.
