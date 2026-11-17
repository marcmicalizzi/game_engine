# A stream's water from its discharge: the grounding (W120)

(2026-11-13, roadmap W120; the owner's proposal after walking batch 63, and the forest proof of
concept's solver needs, section 3.15.) What the channels capability's discharge and profile rest on:
the published models, the numbers taken from each, and **how far each source was read**. The rules
that follow from them are on [channels](../subsystems/channels.md#the-waters-description).

**How the sources were read.** One research pass over the web on 2026-11-13. The publishers' pages
of most primary papers refused the fetch (Wiley, ASCE, the USGS's PDF of Professional Paper 252), so
most numbers below were confirmed against a **secondary** source that quotes the original, and a few
were not confirmed at all. Each row says which. A number that was not confirmed is not pinned in the
code: the design says what stands in its place.

## Sources

| Source | What is taken | How far read |
|---|---|---|
| Chow (1959), *Open-Channel Hydraulics* | The standard step method (energy between two sections, the friction slope averaged); the direct step method (section 10-3), which the fixture integrates with; critical depth as the least specific energy and the least specific force; the sequent depth from the specific force | From memory of the text; the book was not opened. The equations are the textbook's and are restated on this page so they can be checked |
| Henderson (1966), *Open Channel Flow* | Controls: a crest and a free overfall pass critical depth; a mild reach above a pool is an M1 curve, above a drop an M2 | From memory; not opened |
| HEC-RAS Hydraulic Reference Manual (US Army Corps of Engineers, v6.5), "Mixed Flow Regime Calculations", "Computation Procedure" | **The mixed regime**: a subcritical pass from the downstream boundary, a supercritical pass from the upstream boundary or from a section that defaulted to critical depth, and at every section with both answers "whichever answer has the greater specific force is considered to be the correct solution"; if the subcritical one, a jump stands between that section and the one before. The standard step's tolerance is 0.003 m and it stops at 20 iterations | The official pages were fetched and summarised (not read as PDF) |
| Jarrett (1984), "Hydraulics of high-gradient streams", *J. Hydraulic Engineering* 110(11), 1519-1539 | **The resistance of a steep bed**: n = 0.39 S^0.38 R^-0.16 with R in feet, which is n = 0.32 S^0.38 R^-0.16 with R in metres (0.39 x 3.2808^-0.16 = 0.3225). 75 measurements on 21 Colorado streams, slopes over 0.002, average standard error 28 % | Abstract (ASCE); the equation from two secondary quotations (a HEC-RAS practitioners' note, a Utah paper). The range is quoted two ways: slopes 0.002 to 0.09 and R 0.15 to 2.1 m, or 0.002 to 0.04 and 0.15 to 1.68 m |
| Rickenmann and Recking (2011), *Water Resources Research* 47, W07538 | Nothing pinned. The paper's point — in steep, shallow flow the speed goes with about the 0.6 power of the unit discharge and a fixed n of 0.04 is far too smooth — is what Jarrett's equation also says | Not opened; the constants of its fitted equations could not be confirmed against any page and are not used |
| Rand (1955), "Flow geometry at straight drop spillways", *Proc. ASCE* 81, as Chow (1959) quotes it | **Below a free fall** of height h, with the drop number D = q^2 / (g h^3) = (y_c / h)^3: the depth where the surface resumes y_1 / h = 0.54 D^0.425, the distance to it L_d / h = 4.30 D^0.27, the pool under the nappe y_p / h = 1.00 D^0.22, the sequent depth y_2 / h = 1.66 D^0.27 | Secondary: a US Army coastal hydraulics manual page that cites Chow and gives all four |
| Rouse (1936), "Discharge characteristics of the free overfall" | The brink depth is 0.715 of critical depth, and critical depth stands three to four critical depths upstream of the brink | Secondary (an IAHR media library page); S104's cook already uses 0.715 |
| Smetana (1934), in tables of jump-length formulas; Bradley and Peterka (1957) | **A jump's length** L = 6 (y_2 - y_1). The USBR curve levels at about 6 y_2 for Froude numbers of 5 to 13; a fit to it quoted from Chow, L = 220 y_1 tanh((F_1 - 1) / 22), gives L / y_2 of about 4.2, 5.3, 5.9 and 6.1 at F_1 of 2, 3, 4.5 and 6, and Smetana's rule gives 3.5, 4.4, 5.0 and 5.3 at the same: within a fifth, shorter | Secondary (a table of formulas, lecture notes); the USBR chart itself was not read |
| Leopold and Maddock (1953), USGS Professional Paper 252 | At a station, width, depth and speed go as Q^0.26, Q^0.40 and Q^0.34; downstream as Q^0.5, Q^0.4, Q^0.1 | Secondary (a review that quotes both sets) |
| Leopold, Wolman and Miller (1964) and later, as quoted | Bankfull discharge recurs about every 1.5 years (1.0 to 2.5) | Secondary, snippets. **No published ratio of the mean annual to the bankfull discharge was found** |
| Hydrological Atlas of Switzerland, plate 5.4 (1961-1980) | **Specific runoff**: a national mean of about 32 l/s/km2 (1,009 mm a year), basins from 8 to 73, half of them between 21 and 42, alpine catchments 40 and more; 1 l/s/km2 is 31.5 mm a year | Secondary, a snippet |
| USGS Circular 52 | The extremes: over 2,000 mm a year (64 l/s/km2) in the Olympic Mountains, under 6 mm in the Arizona deserts | Secondary, a snippet |
| Montgomery and Dietrich (1988, 1992) | Where channels begin (the planner's threshold, already on [channels](channels.md)); a channel head is not where perennial flow begins | Not opened again for this row |

## What follows

**The profile is the textbook's.** Between two sections a distance dx apart the energy is carried:
z_1 + y_1 + V_1^2 / 2g = z_2 + y_2 + V_2^2 / 2g + S_f dx, the friction slope the mean of the two
sections'. A section is whatever the ground is across the line: its area A, wetted perimeter P and
top width T against the level are tabulated from the ground's own heights, never from a described
shape. Critical depth is where Q^2 T = g A^3; the specific force is M = Q^2 / (g A) + A ybar. The
subcritical profile is marched upstream from each control and the supercritical one downstream, and
where both stand the one with the greater specific force is the water (HEC-RAS's rule); where the
choice changes from supercritical to subcritical going downstream there is a jump, drawn as a pair
of stations a jump's length apart.

**Resistance is Jarrett's, by the bed's own slope.** n = 0.32 S^0.38 R^-0.16, S the bed's fall over
the reach about the station and R the section's hydraulic radius at the depth in hand, each held to
the range of Jarrett's data (S 0.002 to 0.09, R 0.15 to 2.1 m; above a slope of 0.04 one of the two
quotations of his range has no measurements, so that end is an extrapolation and the page says so).
(W137, 2026-11-16: Jarrett's own report, USGS WRI 85-4004, read since, sets the law's range at S
0.002 to 0.04 and R 0.5 to 7 ft: the 0.09 is past it. The engine's default is now Ferguson's law,
[stream-speed](stream-speed.md); this one stays as `--resistance jarrett`.)
In uniform flow this is his equation exactly, and it gives V = 3.1 R^0.83 S^0.12: on a steep bed the
speed barely rises with the slope, where n = 0.04 would have it rise with the square root. The slope
in n is the **bed's** and not the surface's, because the roughness is the bed's: a backwater does
not smooth the gravel under it. A line its author wrote may hold one n instead.

**A fall.** Where the bed drops by three critical depths or more (S104's own test, at the solved
critical depth), the profile ends at the lip in critical depth and resumes at the foot by Rand's
drop: the foot stands L_d from the lip and the water there has the speed Rand's y_1 gives. For a
section that is not a rectangle the critical depth in his ratios is read as the hydraulic depth at
critical, A_c / T_c, and the foot's depth as the area 0.54 (D_c / h)^0.275 A_c: the same ratio of
speeds, which is what his flume measured. That reading is this page's and not Rand's.

**Discharge.** Rain over the catchment as a specific runoff (a scene number; the default 30 l/s/km2
is the Swiss atlas's middle, a humid temperate mountain), plus the point sources a scene names,
summed down the links. One scene number multiplies it for the day: 1 is the mean annual flow.
Because no ratio of the mean to the bankfull flow was found, none is assumed: the cook measures,
for every link, today's discharge against what its cut carries bank-full, and reports it.

## Numbers pinned, each against its source

| Number | Value | Source |
|---|---|---|
| g | 9.81 m/s2 | the falls' constant (`k_fall_gravity`) |
| Rand's foot | 0.54, exponent 0.275 on y_c / h | 0.54 D^0.425 with D = (y_c / h)^3, over y_c / h: (y_c / h)^(1.275 - 1) |
| Rand's length | 4.30, exponent 0.81 on y_c / h | 4.30 D^0.27 |
| A jump's length | 6 (y_2 - y_1) | Smetana |
| Jarrett's n | 0.32 S^0.38 R^-0.16, R in metres | Jarrett (1984), converted from feet |
| The fall test | three critical depths, a critical depth of 1 cm | S104 ([waterfalls](waterfalls.md)) |
| Specific runoff | 30 l/s/km2 by default | the Swiss atlas's mean of 32, rounded |
