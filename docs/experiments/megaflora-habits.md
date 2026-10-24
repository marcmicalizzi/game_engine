# Megaflora habits: emergent and spreading crowns, limbs a wood could carry (G79, 2026-10-22)

**Current follow-up (G85–G86):** [Shared growth rules](tree-growth-rules.md) records the landed
renewal growth and shorter automatic runs. The proportions table and village section below include
those changes; the later G79 capture/timing tables remain historical measurements.
[G86's audit](tree-profile-audit.md) records the seated segment joints and new paired captures.
Explicit platform runs remain explicit requests; adopting a different dwelling recipe requires
checking its platform positions against that recipe's structural table.

The research `engine.flora.Ancient`'s habits and its limbs' mechanical cap are grounded in, the
constants chosen from it, where the budget and the look traded against it, and what the recipes
came to. The rules that follow are on the subsystem page: [flora](../subsystems/flora.md#megaflora),
"Limbs a wood could carry". The branching below the limbs is G73's and its sources are in
[tree allometry](tree-allometry.md).

## What was wrong

The owner's walk under the elder grove's tree (2026-10-22): every major limb alike, each a straight
run out from the trunk at near level and then a limb past it that kept the run's thickness and went
on, near level, before it turned up — 24 m of run and 36 m past it on the default tree, 20 m and
28 m at 2° on the forest village's anchor tree. Real limbs fork, meander, differ one from the next
and do not reach that far that low: the wood would not carry it. The owner named two natural forms
to draw from and a third, rarer, for fantasy:

- **emergent, very tall** — a long clear bole with the branches near the top (an old Douglas-fir,
  "Big Lonely Doug"; the giant sequoia, the Grizzly Giant's huge, crooked, ascending limbs high up);
- **spreading, very wide** — shorter, many limbs going out and forking again and again into a broad
  dome (the open-grown bur oak, the monkeypod, a live oak);
- **the two together** — a clear bole carrying a spreading crown, the less common choice.

## The habits' proportions and their sources

| Habit | Number | Value chosen | Source |
|---|---|---|---|
| emergent | first large limb over the height | 0.5 | General Sherman: 83.8 m tall, first large branch 39.6 m up, 0.47 ([NPS, "The General Sherman Tree"](https://www.nps.gov/seki/learn/nature/sherman.htm)); the Grizzly Giant in the owner's photograph about the same |
| emergent | crown width over height | 0.6 asked (realised 0.58 with the leaves) | General Sherman's average crown spread 32.5 m, 0.39 of its height ([NPS](https://www.nps.gov/seki/learn/nature/sherman.htm)); asked wider than the measurement because a limb's own path is not its horizontal reach, and the recursion's leaves end inside the asked width |
| emergent | largest limb | 0.42 of the core radius where it leaves | General Sherman's largest branch 2.1 m across ([NPS](https://www.nps.gov/seki/learn/nature/sherman.htm)) on a trunk about 4.5 to 5 m across at that height |
| emergent | the trunk | excurrent, through to 0.8 to 0.9 of the height | the sequoia's and the Douglas-fir's single stem |
| spreading | where the trunk divides | 0.2 to 0.3 of the height | the open-grown oaks in the owner's photographs divide at a fifth to a quarter of their height; a monkeypod lower still |
| spreading | crown width over height | 1.5 asked | the bur oak in the owner's photograph about 1.45 and the monkeypod about 2.2 (measured off the photographs, so approximate); UK free-standing pedunculate oaks' crown diameter runs 24 to 27 times their stem diameter (Coombes et al. 2019, *Defining the allometry of stem and crown diameter of urban trees*), which on a 1.5 m stem and a 25 m tree is 1.4 to 1.6 |
| spreading | the trunk | decurrent: ends just above its highest limb | the oak's trunk dissolving into its scaffold limbs |
| spreading | forks | 4 levels, the daughters splaying near level | the oak's and the monkeypod's repeatedly forking limbs (sympodial, decurrent branching) |
| mixed | first limb, crown width | 0.55, 0.9 | an emergent's bole with a spreading crown; the tropical emergents (kapok, tualang) come closest; their proportions are not pinned to a source — this is the fantasy habit |
| all, G85 | automatic straight-run fraction | emergent 0.12; spreading/mixed 0.15 | authored reduction of G79's long straight spans, followed by the same mechanical cap; explicit run lengths override it |
| all, G85 | renewal | one upright at each run; radius 0.3 of run tip; length branch_length × (run + 0.35 × past) | authored reiteration approximation; its load enters the mechanical sum |
| all, G85 | inner segment crowns | radius 0.55 of segment, lateral length multiplier 1.2 | authored interior fullness; its load also enters the cap |

The angles (the runs' rise, the turn up past them, the fork angle and its plane's tilt, the side to
side swing) are not measured from a source; they were set against the reference photographs, capture
by capture (below), and are the look's, not the literature's.

## Limbs a wood could carry

### Village habits: an authored fantasy requirement

The wide dwelling recipe uses the spreading growth structure with nominal height 80 m, 6 m core
radius, twelve limbs from 22 to 28 m, requested crown spread 1.8, upturn 0.25, branch length 0.65,
LAI 5.5 and .30 m leaf cards under an explicit 1.5-million source budget. The taller dwelling uses
the mixed structure, fourteen limbs from 22 to 56 m, a 4.5 m core and LAI 4.5. These proportions
serve the owner's broad, full village canopy and forest-landmark requirements. They are **authored
fantasy trees**, not measurements of sequoias, oaks or tropical emergents. Natural habits retain
their own limb bands and counts.

Both leave automatic run lengths to the habit and strength cap. Renewal growth fills the
interior above supports. Where a run length is explicit, the upright stays at the trunk-side
crotch instead of blocking the reserved walking span. Its radius fraction contributes 0.09 of
the run section to the load approximation; an inner crown contributes 0.55² × 1.2 = 0.363.

G86 changes only the surface seating at the ancient's named segment joints: a small ring starts
inside the parent and smoothly grows to the child's nominal wood, without an exposed flared rim.
No structural axis, radius, run length, part ID or crown topology changes. The natural emergent,
spreading and mixed trees, original game anchor and wide village each have paired captures.
This is not a watertight union and neither solver certifies real platform loads.

### Mechanical model

**The model.** A limb system carries about its base's cross-section of wood all the way out to its
tips: the pipe model ([Shinozaki et al. 1964](tree-allometry.md#the-model-and-its-sources)),
Leonardo's rule that the sections of a fork's daughters sum to the parent's (shown consistent with a
tree resisting wind stress by Eloy 2011, *Leonardo's rule, self-similarity and wind-induced stresses
in trees*, PRL 107). So its weight per metre of path is the green wood's weight times that section,
and the bending moment at the base of each segment is that weight times its horizontal lever arm,
summed vectorially over everything the segment carries — its own wood, every daughter's, and the
crown the recursion grows past each last segment (its laterals' length times 2.5, at a third of that
on along the segment's heading). The stress at the segment's base is that moment over the section
modulus of a round section, σ = 4M / (π r³). A gravity-loaded cantilever is the standard frame
for branch mechanics (Niklas, *Plant Biomechanics*, 1992).

**The allowable stress.**

| Constant | Value | Source |
|---|---|---|
| green wood's modulus of rupture (`wood_rupture_mpa`) | 50 MPa | the white oaks' small clear specimens tested green: 7,200 psi (49.6 MPa) at the low end of a range to 18,400 psi seasoned ([AHEC species guide, strength and mechanical properties](https://www.hardwoodinfo.com/wp-content/uploads/professional/species-guide/Species_Guide_Stength_Mech_Properties.pdf), from the USDA's tests) |
| green wood's density (`wood_density`) | 1,000 kg/m³ | white oak's specific gravity 0.57 to 0.60 on green volume ([AHEC](https://www.hardwoodinfo.com/wp-content/uploads/professional/species-guide/Species_Guide_Stength_Mech_Properties.pdf)) with its water: about 0.6 × 1.7 |
| duration of load | 0.6 | wood under a load sustained for a decade breaks at about 60% of the short test's stress (the Wood Handbook's duration-of-load curve, FPL-GTR-282, ch. 5) |
| static factor of safety | 2 | a constant factor of safety of about 2 against mechanical failure in a black locust's trunk (Niklas 1997, *Annals of Botany* 79:265); the factor rises towards a crown's tips (Niklas 1999, *American Journal of Botany* 86:688, read in abstract only) |

So the allowable stress is 50 × 0.6 / 2 = 15 MPa. **A limb asked to reach further than that allows
is shortened, run and all, by one factor**: every length scales by k and every section stays, so
every moment scales by k² and k = √(1 / peak ratio) puts the worst segment on the limit; the solver
grows the limb once at its asked length, sizes it, and grows it again at the size (twice more at
most if the droop below moved the peak). `flora::ancient_limbs` reports each limb's scale and peak
stress, and the test holds every limb under its allowable stress.

**What that comes to.** For a pipe-model limb system held level, the reach the wood allows is
√(σ r / (2 ρ g)) — about 28 √r metres for r in metres at these constants, before the crown's own
load. A 0.5 m oak limb (0.25 m radius) reaches about 14 m, which is what a big open-grown oak's
scaffold limbs do; the default tree's 1.6 m radius limb about 35 m; General Sherman's 2.1 m limb,
which was more than 30 m long and broke off in January 2006 (its NPS page and the reporting of it),
is at these constants at its limit — sequoia wood is lighter and weaker than oak, and a limb fails
under wind and snow, not its weight at rest, which the factor of safety and the duration of load
are there for. **The elastic sag of a limb this thick is centimetres** (a 1.6 m limb held 30 m
level deflects about 0.4 m under its own weight at green oak's 8.6 GPa, doubled for creep), so the
droop the solver draws is not the elastic curve: it is how a heavy limb's base angle falls as it
grows and its tip recurves upward by reaction wood (Alméras and Fournier 2009, *Biomechanical design
and long-term stability of trees*, J. Theor. Biol. 256:370), drawn as a turn down over a segment's
first half by up to 14° on a level segment at its wood's limit (in proportion to its load ratio and
its levelness) and the turn back up, and beyond, over its second. That 14° is the look's number.

## Forks and meander

Past its straight run a limb is a chain of named segments: the first, then forks of two daughters
(at the first fork sometimes three) for the habit's number of levels, each daughter about 0.8 to
0.85 of its parent's length. The daughters share the parent's section by the pipe rule, the greater
taking 0.5 to 0.74 of it, and the greater one deviates the less: their sections' directions sum to
the parent's direction, the force balance Murray (1927, *A relationship between circumference and
weight in trees and its bearing on branching angles*, J. Gen. Physiol. 10:725) used for branching
angles. Each segment swings from side to side on a seeded wave and wanders a little at every step;
each turns up by the habit's ascent past the sag. The last fork's lesser daughter is a broken stub
one time in five to ten (short, blunt, bare), and an emergent's or a mixed tree's clear bole
carries a few dead stubs, the limbs it shed.

## Where the budget and the look traded

- **The structure the research says, the detail the budget allows.** Every named segment is a
  tube of its own part (rows by length, sides by radius, both coarsened down the ladder) and the
  recursion below is G73's. The default emergent tree is 58 named segments and 496 parts; the
  spreading one 231 and 1,229; the mixed one 308 and 1,376 — the recursion's part cap (2,000) is
  untouched, and all three are well inside 1.5 million triangles with no rung walked.
- **The leaves follow the footprint.** The leaf count is the leaf area index over the crown's
  footprint (G73), so a narrow emergent crown carries fewer leaves than the old wide default:
  321,954 triangles for the default tree against 1,419,335.
- **The angles are tuned by eye**, against the reference photographs; the proportions in the
  first table and the cap are from the sources.
- **A spreading tree falls short of its height.** Its limbs leave low and the wood holds them low,
  so the 50 m spreading recipe's crown tops out at 39 m: `height_m` sets where its trunk divides and
  its limbs' band, and the crown reaches about 0.8 of it.

## What the recipes come to

msvc-debug and msvc-release (the same bytes), the solver's report and `ancient_limbs`:

| Recipe | Habit | Across × tall | Lowest run | Limbs, segments, broken | Limbs the wood shortened | Triangles (budget) | Parts |
|---|---|---|---|---|---|---|---|
| default (`Ancient{}`) | emergent | 55 × 93 m | 50 m up, 7.4 m, 2.4 m thick | 7, 58, 3 | 0 (the worst at 14.2 of 15 MPa) | 321,954 (1.5 M) | 496 |
| `ancient-spreading` (50 m) | spreading | 70 × 39 m | 10 m up, 9.9 m, 2.2 m thick | 6, 231, 6 | 5 (kept 0.64 to 0.86) | 365,401 (1.5 M) | 1,389 |
| `ancient-mixed` (80 m) | mixed | 67 × 81 m | 44 m up, 9.6 m, 2.0 m thick | 8, 308, 10 | 6 (0.70 to 0.87) | 450,697 (1.5 M) | 1,632 |
| `ancient` (elder A: the old limb fields spelled out) | emergent | — | 30 m up, 15.0 m kept of 24, 2.9 m thick | 9 | — | 606,450 (1.5 M; 1,419,335 before) | 878 |
| `ancient-rising` (elder B) | emergent | — | 30 m up at 19°, 24 m, 2.7 m thick | 8 | — | 386,241 (1.5 M; 1,050,182 before) | 626 |
| the forest village's anchor A | emergent | 74 × 74 m | 18 m up, 16.1 m kept of 20, 3.3 m thick | 7, 52, 3 | 6 (the lowest kept 0.81 of 48 m) | 593,832 (1.5 M) | 704 |
| the forest village's anchor B | emergent | 69 × 71 m | 18 m up, 15.9 m kept of 20, 2.6 m thick | 7, 61, 3 | 7 (the lowest kept 0.80 of 46 m) | 469,148 (1.5 M) | 519 |

The anchor recipes (`divine-rpg`'s `content/areas/forest-village/recipes/anchor-a.flora.json`
and `anchor-b`) state their runs, reaches and limb band and leave the habit at its default: they get
an emergent crown on their stated band (18 to 48 m and 18 to 45 m), their limbs' radius the habit's
share of the trunk, and every limb shortened by its wood — the lowest keeps 39 and 37 m of the 48 and
46 asked, its run 16 m of the 20 asked. Their platforms' runs are still straight, near level and
load-bearing. What they would gain by stating it: `"habit": "spreading"` or `"mixed"` for a village
tree whose platforms want many near-level limbs low down, and either `limb_run_m` left out (the
habit's share, about 10 m) or kept at what the wood allows (the report's `scale` times the asked).

## Captures

Offscreen at 1600×1200, the side view at 1.45 times the height and the view from under the crown
from 0.35 times the height out, before (the solver at `7a235b70`) and after: the default tree and
the forest village's anchor tree before; the three habits, elder A from under its crown and the two
anchors after. They are not committed; the batch's report lists where they are.

## Not done

Wind and the limbs' motion (S66); a limb's elastic and creep sag computed rather than drawn; a broken
stub's end is a short cone, not a jagged break; the spreading habit's crown height; the habits'
growth from light and space (G74), which would make an emergent's and a spreading tree's crowns
follow from where they stand instead of a table.
