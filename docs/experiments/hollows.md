# A stream's line never leaves a hollow uphill: the grounding (W122)

(2026-11-14, roadmap W122; the owner's walk of 2026-11-13: "we have water going up hill, we have
stream beds with no water"; and their answer on the basins the same night: "probably a mix ...
pick something that seems like what could happen naturally, maybe one dried out over time with no
glacier or well feeding it".) What the channels planner's hollows rest on: the published methods
for a depression in a height field, what is taken from each, the numbers chosen, and **how far
each source was read**. The rules that follow are on
[channels](../subsystems/channels.md#hollows).

**How the sources were read.** One pass over the web in the night of 2026-11-13. Two papers were
read in full text through a page reader (which summarises: the last sixth of each was not
reached); the rest refused the fetch (Wiley, Taylor & Francis, Springer) or came as a PDF this
machine could not render, and are known here through the manual of the software that implements
them, an abstract, or a search summary. Each row says which. Nothing is pinned from a source that
was not opened: where the literature leaves a number to the user, the page says so and says where
the default comes from instead.

## Sources

| Source | What is taken | How far read |
|---|---|---|
| Barnes, Lehman & Mulla (2014), "Priority-Flood: an optimal depression-filling and watershed-labeling algorithm for digital elevation models", *Computers & Geosciences* 62, 117-127 | The fill the planner already ran: flood inward from the edge through a priority queue, raising each cell to the level it is reached at, with an increment so a flat still drains; and that the same pass labels watersheds, which is what labelling the pits leans on | The abstract (arXiv 1511.04463) |
| Barnes, Callaghan & Wickert (2020), "Computing water flow through complex landscapes, part 2: finding hierarchies in depressions and their merge and spill", *Earth Surface Dynamics* 8, 431-445 | **What a depression is**: an inward-draining region; its outlet is the lowest cell of its rim and its spill elevation "the elevation to which water must rise in order to flow out". Leaves merge into parents at their lowest outlets, so the flat fill's pits are the top of a hierarchy and a pit that is drained uncovers its children. **The ocean is seeded first** (cells under sea level, or the grid's edge) and is one depression with any number of children: the planner's sinks. Section 6.3: small or shallow depressions are "typically" artefacts and are filtered by area or volume (30 cells in their figure 9); real ones (lakes, prairie potholes, wetlands) are kept, and no general rule tells the two apart | Full text through a reader, sections 1 to 6.4; the last 15 % not reached. Their volume formula came through garbled and is not used: the planner sums the level less the ground |
| Barnes, Callaghan & Wickert (2021), "Computing water flow through complex landscapes, part 3: Fill-Spill-Merge", *Earth Surface Dynamics* 9, 105-121 | **Where water stands given how much arrives**: water is routed to pits, a depression that holds more than its volume overflows to its neighbour and the two merge when both are full, and a partly filled depression's level is found by growing a priority queue of cells from its pit until they hold the water (their equation 7: z_w = (V_w + sum z_i) / N). At unlimited water every depression is full, which is the flat fill. It carries **no evaporation, infiltration or loss**: the balance below is not theirs | Full text through a reader, sections 1 to 4.3; the last 13 % not reached |
| Lindsay (2016), "Efficient hybrid breaching-filling sink removal methods for flow path enforcement in digital elevation models", *Hydrological Processes* 30, 846-857 | **Breach where the cost is small, fill the rest**: complete, selective (a threshold sink depth) and constrained breaching; breaching fits the causes of artefact depressions better than filling and changes the terrain less; breaching alone answers a large sink with a long, deeply incised trench, which is why the hybrid exists | NOT read in full (the PDF did not render; the publisher refused). Read: the WhiteboxTools manual's BreachDepressions, which implements it ("breach-first, fill-second"; `max_depth` and `max_length`, **both infinite by default**), and a search summary of the abstract |
| Lindsay & Dhun (2015), "Modelling surface drainage patterns in altered landscapes using LiDAR", *Int. J. Geographical Information Science* 29, 397-411 | The least-cost breach: from a pit, the path to a lower cell that lowers the least elevation in all; left unbreached past a maximum cost | NOT read (publisher refused). Read: the manual's BreachDepressionsLeastCost, "loosely based on" it: the cost is the elevation lowered along the channel, searched within `dist` cells, `max_cost` infinite by default, the rest filled; "there are instances when a breaching solution is inappropriate" (an open pit: a long deep breach), to be limited by cost and then filled |
| Lindsay & Creed (2005), "Removal of artifact depressions from digital elevation models: towards a minimum impact approach", *Hydrological Processes* 19, 3113-3126 | Four ways compared: filling, breaching, breaching constrained to two cells with filling after, and an impact reduction approach that takes the lesser change a depression. Filling changes terrain attributes most; large depressions sit on flat landscapes | The abstract, through a search summary |
| Jenson & Domingue (1988); Planchon & Darboux (2001); Wang & Liu (2006) | The fill's lineage. Nothing is pinned from them | Not opened |
| Mason, Guzkowska, Rapley & Street-Perrott (1994), "The response of lake levels and areas to climatic change", *Climatic Change* 27, 161-197 | Nothing pinned. A closed lake is a low-pass filter of its climate with an equilibrium response time of 1.5 to 350 years | Not read (behind a login); the response times from a search snippet. The balance below is conservation of mass and is stated as that |
| Lake evaporation: NOAA's 1981-2010 Great Lakes climatology; a Minnesota open-water study (1964-2005); USGS SIR 2024-5040 (Lake Okeechobee, 2013-16); an Okanagan Lake study | **Evaporation from open water**, millimetres a year: 504 (Michigan-Huron) to 896 (Erie); 580 to 747 over six Minnesota sites; 725 and 835 at Okanagan; about 1,825 at Okeechobee | Secondary, search snippets |
| *Hydrological Atlas of Switzerland*, plate 4.1, "Mean annual actual evaporation 1973-1992" (the plate's text; the text names no author, and its model is the TRAIN model of Menzel 1997, its reference 1) | **Evaporation by surface in a humid mountain climate**, from the TRAIN model on a 1 km grid with daily data over twenty years: **open water 901 mm a year**, forest 616, farmland and mountain pasture 436, rock 234, ice and firn 156, the whole country 484; land evaporation is about 560 mm up to 700 m and falls steadily to about 230 mm at 3,000 m | The plate's text in full (four pages, 2026-11-14); the map and its figures not seen |
| Rosenberry, Lewandowski, Meinikmann & Nützmann (2015), "Groundwater - the disregarded component in lake water and nutrient budgets. Part 1: effects of groundwater on hydrology", *Hydrological Processes* 29, 2895-2921 | **What a lake's bed passes**: over the lakes of the review, the median loss of lake water to the ground is **0.60 cm a day** (2,190 mm a year) and the median gain from it 0.74 cm a day; reported rates "span more than five orders of magnitude"; the ground takes a median 35 % of a lake's losses and gives a median 25 % of its inputs, and matters less as lakes pass 100 ha | The abstract, on the survey's publication page (2026-11-14; the publisher's page refused). The table behind the medians not seen, so which settings sit at which end is not known here |
| A water balance of a proglacial lake in the French Alps (*Annals of Glaciology*) | **What a moraine passes**: a lake with no open outlet that takes all the melt of a 250 ha glacial catchment and loses about 5 x 10^6 m3 a year by seepage, 0.16 m3/s | A search snippet |
| A sedimentation study of an Eastern Alps lake (EGU 2019 abstract), and a review line on high alpine lakes | **How long a lake lasts**: 25,000 m3 a year on the bottom and 100,000 m3 a year in the delta, filled "within the next 25-30 years"; lifetimes of high alpine lakes "between a few decades and many centuries to thousands of years"; infilling is "a natural stage in the succession of a freshwater system" | Search snippets |
| Sinks in talus, rock glaciers and karst (ponors, swallow holes) | Nothing found: the search returned moraine dams only. That a basin floored with blocks or karst drains underground is common knowledge and is given to the author as a number (`leak_m3s`), not derived | Not found |

## What follows from them

**The shape is the literature's; the limit is the terrain's.** Every source that removes
depressions says the same thing in its own words: a small or shallow one is breached (the stream,
or the DEM's error, has cut its lip), a large one is real and is left as water. None gives the
limit. Lindsay's tools take a depth and a length from the user with no default; Lindsay and Creed
used two cells; Barnes et al. filter by a cell count. So `breach_max_m` is a scene number, and its
default, 1 m, is the plan's own scale for a feature it keeps (`fall_min_m`: a step under a metre
is not a knickpoint either). `basin_min_area_m2` (200 m2) is a dozen cells of the default
lattice, the same kind of filter as the hierarchy's.

**A hollow's water is a balance, and the balance is conservation of mass.** At the year's mean,
with the water standing over an area A:

    inflow  = runoff x (catchment - A) + the named sources that drain into it
    losses  = (evaporation net of rain + seepage) x A + the basin's leak

A lake fed by rivers and losing only to the air stands where the two are equal, A = inflow / (E -
P): the closed (endorheic) lake. If the losses at the spill's area are still under the inflow the
lake spills and its outlet carries the difference; if they pass it, the lake stands lower with no
outlet; if the leak alone takes the inflow nothing stands. The level below the spill is found as
Fill-Spill-Merge finds it, cells from the floor up until the balance closes, and is coarser than
theirs: one level a hollow, the inflow not divided between a hollow's sub-basins.

**The numbers a scene sets** (channels.md has the fields): the runoff is W120's (30 l/s/km2, 946
mm a year, a humid temperate mountain's). `basin_evaporation_mm_y` is net of the rain on the
water; in the climate the default runoff describes the rain passes a lake's evaporation (500 to
900 mm measured on cold temperate lakes against well over 946 mm of rain), so the default is 0
and every hollow fills unless its floor leaks. A semi-arid scene sets runoff near 3 l/s/km2 (95
mm) and a net evaporation of 1,000 to 1,500 mm, and a hollow then needs a catchment ten to
sixteen times its water to fill. `basin_seepage_mm_y` is what the bed passes over its whole area;
a basin's `leak_m3s` is what one floor passes whatever the water's area (the French lake's
moraine: 0.16 m3/s from a 250 ha catchment, as much as that catchment's whole runoff).

**The losses by climate, in place of zero** (2026-11-14, for the forest's mix). The default
runoff, 946 mm a year, with the Swiss atlas's 484 mm of evaporation from the land is a rain of
about 1,430 mm, which is that country's own. Open water there loses 901 mm, so **the rain on a
lake passes its evaporation by about 530 mm**: `basin_evaporation_mm_y`, which is net of the rain
and cannot be negative, is rightly 0 in this climate, and the balance then understates a lake's
inflow by what falls on it (530 mm against the 946 mm the same area would send as land: 7 l/s on
a 43 ha lake fed 135). A drier scene takes its own two numbers: open water less rain, and the
runoff. `basin_seepage_mm_y` has no climate; it is the ground's. The one central figure opened
is the review's median, **2,190 mm a year**, taken over lakes that were studied because the
ground mattered to them, so it is a high reading for a basin in rock and a fair one for till or
talus; the range is five orders wide and a scene that knows its ground sets its own. On the
forest it is used as the bed's loss everywhere, so that a lake stands or falls by its catchment
and a `leak_m3s` is left to say only what no rate over the bed can: one floor that passes more.

**A dry or a former lake** is as natural as a full one: alpine lakes last decades to thousands of
years before their inflow's sediment fills them, and a floor of talus or moraine can pass all a
small catchment sends. The plan can say a hollow is dry (the author names it, or its leak takes
its inflow); what its floor then looks like is not the channels' to make, and is designed on
[channels](../subsystems/channels.md#hollows), "What a dry hollow's floor is".

## The forest's hollows

The ground the forest proof of concept hands the engine holds 693 pits at the planner's 4 m
lattice (sized by the stream solver's row on the plan of 2026-11-12, before this row: the scratch
note `hollows.txt`). By depth: 494 under 0.25 m and 176 from 0.25 to 1 m (11.8 ha between them:
breached at the default); 12 from 1 to 5 m (25.5 ha); 2 from 5 to 20 m; 9 over 20 m, of which one
is the sea's own floor. The eight closed mountain basins, 28 to 134 m deep and 2.9 to 43 ha, held
no water body and carried whole networks across their filled flats: 110 wet lines climbed out of
them, and 26,395 of the 28,999 stations the solver flagged as standing over a bank were in a
hollow of their own line. A ninth, lowland basin holds an authored lake at -2 m whose basin spills
at 3.49 m. What this row's plan makes of each, measured, is in channels.md's "Hollows".

## What was not grounded

- The breach limit's default and the notch are not from a source. The page says what each stands
  for.
- No published rule was found for which mountain basins hold lakes; the plan's answer is the
  balance and the author's.
- Karst and talus sinks: no source was found, and the plan derives none.
