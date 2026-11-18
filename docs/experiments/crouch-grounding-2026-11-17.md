# Crouching: where the numbers come from (2026-11-17, roadmap P54)

The walker's crouch ([apps](../subsystems/apps.md#crouching)) and the body's crouch layer ([apps](../subsystems/apps.md#the-crouch-layer)) stand for a person walking bent under a low roof. This page is what the numbers they pin rest on, how far each source was read, and which numbers are choices.

**How far the sources were read.** None of the papers below was read whole. Each was found by a search and read through its abstract and the secondary descriptions a search returned (two publisher pages refused the fetch, and the one PDF that came back could not be rendered on this machine). Every number here is therefore an abstract's or a citing review's, to be checked against the full text before anyone leans on it harder than a default.

## The sources

| Source | What it measured | Read as | What it gave |
|---|---|---|---|
| Hodgskiss & Zschernack 2013, *Ergonomics SA* (ajol.info, "Main Article Content" 124239) | treadmill walking at 3.5 km/h under vertical restrictions of 85% and 70% of each subject's stature; muscle activity and perceived exertion | abstract via a search summary | the restriction (70% of stature) and the speed held under it (3.5 km/h, 0.97 m/s); the most local exertion at the lowest restriction, the neck, lower back and hamstrings |
| Gallagher and colleagues, NIOSH mining research (stacks.cdc.gov, cdc.gov "cogir") | gait and posture in restricted vertical space in low-seam coal mines | search summaries | upright walking is not an option below about 1.2 m of seam height: workers stoop-walk or crawl, at a lower top speed and a higher metabolic cost |
| Friedrich et al. 2000, *Spine* | spinal angles of 22 sewage workers walking at five headrooms down to 1.05 m | search summary | lumbar flexion (the trunk's forward lean) and cervical and thoracic extension (the head kept up) grow as the headroom drops |
| Grasso, Zago & Lacquaniti 2000, *J. Neurophysiol.* 83(1): 288–300 | five healthy adults walking erect, knee-flexed, and knee- and trunk-flexed, at several speeds; kinematics, ground reaction forces, EMG | abstract via a search summary | bent walking's mechanical efficiency falls below erect walking's past 1 m/s; the limb segments keep their planar covariation bent (the gait's shape survives the crouch) |
| Davis et al., *Safety Science* (2018; doi 10.1016/j.ssci.2017.12.032), "Characterizing posture and associated physiological demand during evacuation" | posture and demand of stooped and crouched movement in restricted heights | title and journal only (the PDF did not render) | not used for a number; the next source to read |
| Evacuation reviews (Li et al., *Physica A* 2022; a 2023 PMC review) | stoop-walking and crawling speeds against upright | search summaries | stoop-walking slows people much less than crawling (crawling 0.5–0.87 m/s); stoop-walking flow 26% under normal walking |

## The numbers chosen

| Number | Value | From |
|---|---|---|
| `view.walk.crouch_height` | 1.25 m | 70% of the 1.8 m standing capsule, the lower restriction Hodgskiss & Zschernack walked subjects under, and about Gallagher's 1.2 m below which miners cannot walk upright |
| `view.walk.crouch_eye_height` | 1.1 m | the standing eye's 0.15 m under the crown, kept: the crouch bends the body and the head stays the head (Friedrich: the neck extends to keep the head up as the trunk flexes) |
| `view.walk.crouch_speed` | 1.0 m/s | 3.5 km/h (0.97 m/s) held under the 70% restriction; Grasso et al.'s efficiency falling past 1 m/s says faster costs more than it is worth; well above crawling's 0.5–0.87 m/s, as the reviews say stoop-walking is |
| `view.walk.crouch_time_s` | 0.25 s | **no source**: the eye's travel between the heights is a knob of feel; a stand-to-crouch transition time was not found |
| `view.player.crouch_trunk_deg` | 30° | **a choice inside Friedrich's trend**, not a measured value: the trunk leans more as the room drops, and 30° at 70% of stature leaves the legs a knee flexion of about 120° on a body with its hips at 0.95 m |

## What the layer simplifies

- **The legs are two bones in the sagittal plane** (the knee forward, the sole level); the hip's abduction, a knee's valgus and the ankle's own dorsiflexion limit are not modelled. On the test body a full crouch bends the knee about 120°, deep for a walk; a rig whose eye sits lower or whose trunk leans further bends it less.
- **The head is not counter-extended**: Friedrich's workers extended the neck as the trunk flexed; here the look layer (P58) decides where the head points, from the camera's pitch.
- **The gait is not changed**: the walk clip plays on the bent legs at its own stride. A crouched gait (shorter steps, a longer stance) is the next layer, roadmap P56's stride matching.
