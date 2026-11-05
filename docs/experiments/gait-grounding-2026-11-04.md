# The procedural gait's grounding (2026-11-04)

Roadmap C60. What the walk and the run of `systems/animation/gait.h` are built from: the sources,
the numbers taken from them, how the curves were fitted and how closely, and what was left out.
The rules that follow from these numbers are on [animation](../subsystems/animation.md#the-procedural-gait)
and [apps](../subsystems/apps.md#the-procedural-gait).

## Sources

| Quantity | Source | What was taken |
|---|---|---|
| Sagittal hip, knee, ankle, walking at natural cadence | Winter, *Biomechanics and Motor Control of Human Movement*, 4th ed. (Wiley, 2009), the normative kinematics; Perry & Burnfield, *Gait Analysis: Normal and Pathological Function*, 2nd ed. (Slack, 2010), the phases | the peaks at their phases (table below) |
| Gait phases | Perry & Burnfield 2010 | initial contact 0%, loading response to 12%, mid stance to 31%, terminal stance to 50%, pre-swing to 62% (toe-off), initial swing to 75%, mid swing to 87%, terminal swing to 100% |
| Pelvis rotation, obliquity, tilt | Kadaba, Ramakrishnan & Wootten 1990, *J Orthop Res* 8:383, and Perry | amplitudes and phases (below) |
| Trunk counter-rotation | Perry | the thorax turns against the pelvis, about 3° to its 4° |
| Arm swing | Murray, Sepic & Barnard 1967, *Phys Ther* 47:272 | shoulder −24° to +8°, elbow 10° to 30°, free speed |
| Pelvis excursion | Perry; Saunders, Inman & Eberhart 1953, *J Bone Joint Surg* 35-A:543 | vertical ±2.5 cm (lowest in double support), lateral ±2 cm toward the stance limb |
| Walking stride against speed | Grieve 1968 (*Physiotherapy* 54:452), as reanalysed in [arXiv:1608.03583](https://arxiv.org/pdf/1608.03583); Grieve & Gear 1966, *Ergonomics* 9:379 | step length ∝ speed^0.42 |
| The walk's calibration | Perry & Burnfield 2010 | free walk 82 m/min (1.37 m/s), stride 1.41 m, cadence 113 ± 9 steps/min |
| Leg length | Winter 2009, Fig. 4.1 (segment proportions) | greater trochanter 0.530 and ankle 0.039 of the stature |
| Running curves and flight | Novacheck 1998, *Gait & Posture* 7:77 | the running condition (3.2 m/s): toe-off at 39%, so flight 39–50% and 89–100%; the peaks below |
| Running stride | Cavanagh & Kram 1989, *Med Sci Sports Exerc* 21:467 | stride frequency near 1.4 Hz, changing little with speed over the distance-running range; speed is gained by stride length |
| The walk-to-run transition | Thorstensson & Roberthson 1987, *Acta Physiol Scand* 131:211 | 1.88–2.2 m/s with leg length; the engine's walk (1.5 m/s) is below it and its sprint (5 m/s) far past it |

**How far the numbers were checked.** The sagittal peaks are the values these textbooks give and
that every gait course reproduces; the originals' tables were not opened in this session (no access
to the books), and a search returned the same values from secondary sources ([a summary table of
sagittal ranges by phase](https://pmc.ncbi.nlm.nih.gov/articles/PMC555760/table/T3)): hip about 25°
flexed at contact and about 10° extended in terminal stance, knee about 20° in loading and about
60° in early swing, ankle about 10° dorsiflexed in mid stance and about 20° plantarflexed at
toe-off. The arm-swing, pelvis and running values are those papers' figures as commonly reproduced,
not re-read from the originals; they are the numbers to check first if the walk reads wrong.
Grieve's exponent was checked against the reanalysis above.

## The keypoints and the fits

Each curve is a periodic shape-preserving cubic through the keypoints (zero slope at a peak,
Catmull-Rom elsewhere), sampled 400 times and fitted by least squares with five harmonics (one for
the arms, which are given by two extremes). The fit script is a PowerShell least-squares solve kept
outside the repository; its output is the table in `gait.cpp`. Residuals are the fit less the
keypoint at the keypoints, and the largest gap to the cubic anywhere.

| Curve | Keypoints (phase %, degrees) | At the keypoints | Worst vs the cubic |
|---|---|---|---|
| walk hip | 0: 25, 12: 21, 30: 6, 52: −10, 62: 0, 75: 20, 87: 28 | within 0.25 | 0.38 |
| walk knee | 0: 5, 15: 18, 40: 4, 62: 40, 73: 60, 88: 25, 97: 3 | within 1.68 (97%) | 1.98 |
| walk ankle | 0: 0, 7: −6, 30: 5, 48: 10, 62: −18, 80: 0 | within 1.18 | 1.43 |
| walk shoulder | 0: −24, 50: 8 | 0.23 | 0.23 |
| walk elbow | 0: 10, 50: 30 | 0.14 | 0.14 |
| run hip | 0: 40, 20: 20, 39: −8, 60: 20, 85: 55 | within 0.13 | 0.33 |
| run knee | 0: 20, 15: 42, 39: 18, 70: 92, 92: 30 | within 0.19 | 0.44 |
| run ankle | 0: 3, 15: 20, 45: −22, 70: 5 | within 0.16 | 0.57 |
| run shoulder | 0: −35, 50: 20 | 0.40 | 0.40 |
| run elbow | 0: 75, 50: 105 | 0.22 | 0.22 |

The walk's knee and ankle are the hardest: the knee's terminal-swing extension (from 25° at 88% to
3° at 97%) and the ankle's push-off (10° at 48% to −18° at 62%) are steeper than five harmonics
follow exactly. Both stay inside the 2° the tests hold every peak to; a sixth harmonic would halve
them and was not needed for a picture.

The pelvis and trunk, one sinusoid each (degrees; the right side's sense):

| | walk | run |
|---|---|---|
| rotation (right side forward) | ±4, peak at 0% | ±5, peak at 0% |
| obliquity (right side up) | ±4, peak at 17% | ±6, peak at 12% |
| tilt (anterior), twice a cycle | ±1.5, peaks at 45%, 95% | ±2, peaks at 30%, 80% |
| trunk relative to the pelvis | ∓7 (thorax −3 against pelvis +4) | ∓10 |
| rise, twice a cycle | ±2.5 cm, lowest at 5%, 55% | ±3.5 cm, lowest at 20%, 70% (mid stance), highest in flight |
| sway toward the stance limb | ±2 cm, right most at 30% | ±1 cm, right most at 20% |

## The stride

Walk: `stride/L = A (v / √(g L))^0.42`, with A fixed by Perry's free walk on a 0.93 m leg
(0.530 × 1.75 m): A = (1.41/0.93) / (1.37/√(9.81 × 0.93))^0.42 = 2.11. On that leg it gives 1.24 m
at 1.0 m/s (97 steps/min), 1.43 m at 1.4 m/s (117) and 1.58 m at 1.8 m/s (137) — inside the
normative adult ranges at each speed (Perry's 113 ± 9 at 1.37 m/s). The Froude form is the
dynamic-similarity argument: two bodies walk alike at the same `v/√(gL)`, so a 1.9 m body (leg
1.01 m) strides further and steps more slowly than a 1.6 m one (0.85 m) at one speed.

Run: stride = v / f with f = 1.40 Hz × √(0.93/L): 3.57 m at 5 m/s on the reference leg, 168
steps a minute.

**RiggedFigure** at its own units (the gait tests, `metres_per_unit` 1): leg 0.571, so the walk at
1.37 m/s strides 0.959 and cycles in 0.700 s (171 steps/min — a small figure walks quickly), and
the run at 3.2 m/s strides 1.79 in 0.560 s. Worn by the player at 1.8 m it is scaled about 1.24
(its standing height 1.45 through its joints), so its leg is about 0.71 m.

## Stated simplifications

- Amplitudes fixed per band at the band's reference speed; the speed changes the stride and the
  cycle only (in life the hip's and knee's ranges grow with speed).
- No foot IK: the feet stay put because the playhead is driven by the distance walked, and only as
  far as the stride relation is right for this body; a slope, a turn or a speed far outside the
  band slides.
- No turning adjustment, no slope adjustment, no stochastic variation, no jump, no crouch.
- One mean adult; no age, sex or load variation.
- The body's axes are read from the legs at rest, so a rig whose rest stance is far from upright
  swings about tilted axes.
