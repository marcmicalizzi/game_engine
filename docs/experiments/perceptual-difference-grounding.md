# Measures of a visible difference: what is published about telling two pictures apart, still and in motion (T232, stage 0)

(2026-11-25, roadmap T232, stage 0; pages only, nothing built or run.) A shortcut's evidence today
is a count of pixels past 2 of 255 on one still frame, which says neither whether an eye would
notice the difference nor what it does in motion, where most of a shortcut's faults live (flicker,
crawl, a pop at a hand-off). The owner asked for two tools: a perceptual difference between two
captures, and a flight of one path under two settings compared frame for frame. **Neither is a
judge**: a measure says where to look, the owner's eye decides, and a build is judged standing
alone first. This page is what those tools will be built from: for each published measure what it
claims to predict, at what viewing distance, pixel density, luminance and field of view, what it
was validated on, what it costs as published, **under what licence its paper and its reference
code stand, apart**, and whether it carries trained weights. **Section 5 reasons from each model
about a shortcut already shipped (R213's cap) and is marked as reasoning throughout; section 6 is
a reading for the design and is kept apart from the sources**; nothing in either is a measurement
or a decision.

## What the engine has today

Not repeated here, linked:

- **`render.compare` and `engine-image compare`** ([apps](../subsystems/apps.md), [image](../subsystems/image.md)):
  LDR-FLIP written from its paper (its constants checked against NVIDIA's BSD-3-Clause reference,
  [LICENSES](../../third_party/LICENSES.md)), PSNR, and SSIM on Rec. 601 luma, with a mean, a
  percentile, a maximum and a centre-weighted mean, a pixel density defaulting to FLIP's 67.02
  pixels a degree, and a heat map. 4K pair: 3.04 to 3.19 s on one CPU thread. HDR-FLIP and a
  temporal measure are on that page's "not yet" list.
- **`render.evaluate`** compares a real-time frame with the reference path tracer's; per-scene FLIP
  thresholds in `tools/ci/reference-compare.ps1` ([plan 04 §4.8](../plan/04-renderer.md#48-reference-renderer-and-objective-optimization)).
- **R171's measure of the change between frames** ([renderer](../subsystems/renderer.md#the-change-between-frames-as-the-eye-sees-it),
  `engine-view --temporal`): every pixel of consecutive frames of one flight, its change of displayed
  luma in codes and of its light relative to its level, histograms by luma band, and the share of
  pixels any one channel of which moved a code or three. A count over one sequence, not a
  comparison of two, and no model of what is visible.
- **The 2-of-255 count**: the largest channel difference of the 8-bit picture, as R213's table
  counts it ([lights by reach](lights-by-reach.md#the-cap)).
- **What a capture holds**: an 8-bit PNG of the SDR picture, whose encode is a 2.2 power, and with
  `--capture-channels light` the picture's light as an EXR, linear Rec. 709 with 1.0 at the
  picture's white and the white's luminance in the file's `whiteLuminance` for HDR
  ([renderer](../subsystems/renderer.md), "The capture: a picture's light, in an EXR"); `--capture-every <n>`
  writes a flight's frames ([apps](../subsystems/apps.md#flythroughs)). An offscreen flight stands
  frame f at f / 60 real seconds and its world at the world rate times that, so two flights of one
  path have the same world time at every frame, fires' flicker included
  ([ADR-0056](../adr/0056-one-world-clock-and-the-days-length.md), renderer, "Who sets the clocks").

## How the sources were read

One pass over the web on the evening of 2026-11-25, through the fetch and search tools only, as
text; nothing was saved or opened from disk. **No PDF was read**: FLIP's paper was past the fetch's
size limit and stelaCSF's came back as compressed bytes, so a paper is read here only where an HTML
page carries it — an arXiv or ar5iv rendering, an abstract page, a repository's README or licence
file, a project page, a manual. Each source in [the list](#the-sources-and-how-far-each-was-read)
says how far it was read: **read** (the HTML text, for what is taken), **abstract** (an abstract or
project page and nothing past it), **secondary** (a page restating it), **search summary only**
(**not read**; no number of such a source is used). ar5iv's long renderings were read to the
fetch's first 100,000 characters; where a figure lies past that the page says so.

**Licences are read before a number is taken (T229).** Two licences are recorded per source, the
text's and the reference code's, because they decide different things: the engine builds a model
from its paper's equations and constants, as `foundation/image` built FLIP, and takes a number from
a source's code only where the code's licence is one [ADR-0014](../adr/0014-apache-2-license-and-dependency-policy.md)
admits (permissive; no GPL, LGPL or non-commercial terms). A constant that exists only in excluded
code is read from the paper or measured again here. Trained weights are a third licence, and
[plan 12](../plan/12-ai-usage-policy.md#122-rules) rule 3 governs them: only weights whose licence
permits redistribution and commercial use.

## 1. Still images

### The FLIP family

**LDR-FLIP** (Andersson, Nilsson, Akenine-Möller, Oskarsson, Åström, Fairchild, HPG 2020,
*Proc. ACM Comput. Graph. Interact. Tech.* 3(2), DOI 10.1145/3406183). **Claims** a per-pixel map
that approximates the difference a person perceives **when alternating between two images**
(abstract, read); in [0, 1], about 0.1 where a person starts to notice ([image](../subsystems/image.md),
written from the paper). **Viewing geometry**: a pixel density, the paper's reference condition
67.02 pixels a degree, a 3840-pixel screen 0.7 m wide seen from 0.7 m (the image page); **no
absolute luminance** (sRGB in, differences in a colour space relative to the display's white), **no
eccentricity** (every pixel filtered by one contrast-sensitivity Gaussian set), **no time** beyond
the alternation the observer is assumed to make. **Validated**: over a thousand image pairs
inspected by the authors and a user study in which it did substantially better on average than the
other algorithms (abstract; the study's design was not read). **Cost**: the engine's own CPU form
above. **Licences**: paper under ACM's copyright (personal and classroom copies; NVIDIA's page,
read); code BSD-3-Clause (the repository, read); no weights.

**HDR-FLIP** (Andersson, Nilsson, Shirley, Akenine-Möller, Eurographics 2021 short papers,
pp. 25–28, DOI 10.2312/egs.20211015). **Claims** error maps for high-dynamic-range renders that
predict errors substantially better than the metrics rendering used before, by a composite over
LDR-FLIP maps of exposure-compensated, tone-mapped pairs (abstract, read). The exposure range is
computed or given; the reference tool's example sweeps 14 exposures from −12.54 to 0.94 stops under
ACES (the C++ README, BSD-3-Clause, read). The sweep stands in for a display it does not know: it
still models **no absolute luminance and no eccentricity**. **Licences**: paper under Eurographics'
copyright notice (personal and classroom; NVIDIA's page, read); code BSD-3-Clause; no weights.

### The structural measures, and why rendering moved past them

**SSIM** (Wang, Bovik, Sheikh, Simoncelli, *IEEE TIP* 13(4), 600–612, 2004) compares local means,
variances and covariance in windows and claims structural similarity as a quality measure. Its
authors' page (read) says the right scale depends on the image's resolution and the viewing
distance, and for a viewer at 3 to 5 image heights or widths prescribes averaging and downsampling
by F = max(1, round(N / 256)) for N rows before the index — **F = 8 for 2,160 rows** (*derived*);
the engine's SSIM runs at full resolution. **MS-SSIM** (Wang, Simoncelli, Bovik, Asilomar 2003)
evaluates it over scales to lessen that dependence: named on the same page, **not read**. Neither
knows the display's luminance, its pixel density, colour (the engine's runs on luma) or time.
**Licences**: the paper not read for its licence; the authors' MATLAB code may be downloaded,
modified and used anywhere with a citation (the page, read), which is a permission notice and not
a named licence; the engine took nothing from it.

**Why rendering moved past them**, in the numbers read: in LPIPS's study SSIM agreed with human
two-alternative choices 63.1 % of the time against the humans' own 73.9 % (Zhang et al., Sec. 4,
read); on CGVQM's dataset of rendering artefacts SSIM's Pearson correlation was the lowest of the
measures tried, 0.639, though its rank correlation, 0.805, was second only to CGVQM's own
(Jindal et al., Table 1, read) — it orders the sequences well and places them badly; FLIP's
abstract claims a substantial margin over the older algorithms. What the structural measures cannot see by their form: that a difference
of one code is a different contrast at different levels, whether a pixel is near or far in degrees,
and anything between frames.

### The visible-difference predictors: HDR-VDP's line

Daly's VDP (1993) is the line's root: **named only**. **HDR-VDP-2** (Mantiuk et al., 2011):
**named only**; HDR-VDP-3's paper says it keeps HDR-VDP-2's pipeline.

**HDR-VDP-3** (Mantiuk, Hammou, Hanji, arXiv 2304.13625, 2023; read through ar5iv). One visual model
with five tasks: **quality** (one score in just-objectionable differences, JOD, 10 the best),
**side-by-side** and **flicker** (maps of the proportion of viewers who would notice a difference,
the flicker task's images swapped every 0.5 s), **detection** (one probability) and contrast
distortion. **Input**: pictures in absolute luminance through a display model. It models the
eye's ageing (glare, the lens, the pupil) and **not eccentricity**; its CSF was refitted to Kim et
al. (2013). Calibrated: quality on UPIQ (over 4,000 SDR and HDR images on a JOD scale),
side-by-side on Wolski et al. (2018) and Ye et al. (2019), flicker on a small unpublished set,
detection on Gabors, gratings and discs; no correlation figure was in what was read. **Licences**:
the arXiv text CC BY 4.0 (arXiv, read); **the code, HDR-VDP 3.0.7 for MATLAB and Octave, is
GPL-2.0** (SourceForge, read): **excluded — no constant is taken from it**, only from the paper.
No weights.

### The learned measures, and what plan 12 says of them

**LPIPS** (Zhang, Isola, Efros, Shechtman, Wang, CVPR 2018, arXiv 1801.03924; read through ar5iv).
Distances between the activations of image classifiers (AlexNet, VGG, SqueezeNet trained on
ImageNet), with learned linear weights per channel. Its dataset, BAPPS, is **64 × 64 patches**
under traditional, network-made and real algorithms' distortions (super-resolution, frame
interpolation, deblurring, colourisation); agreement with human two-alternative choices: humans
73.9 %, AlexNet 68.9 %, SqueezeNet 68.6 %, VGG 67.0 %, against SSIM 63.1 % and L2 63.2 %; the
choices and the JND judgements (patches shown a second with 250 ms between) were collected on
Mechanical Turk, and **no display, distance or luminance** is stated. It claims a similarity, not a
threshold. **Licences**: arXiv's non-exclusive distribution licence (arXiv, read); code
BSD-2-Clause and its linear weights in the repository under it (read); **the backbones' weights**
come from ImageNet classifiers for which torchvision states no licence, and a library maintainer
holds that ImageNet's non-commercial research terms reach them (forum threads, secondary);
BAPPS's licence not stated. DISTS and PieAPP are its kin: **named only**.

**What [plan 12](../plan/12-ai-usage-policy.md#122-rules) says**: a learned measure is a model, and
model weights are dependencies with licences, admitted only where the licence permits
redistribution and commercial use (rule 3); a runtime model is third-party open weights used as
published (rule 2, §12.4); no output of an Anthropic model may go into training,
evaluation-for-training or a dataset for any model (rule 1), so calibrating a learned measure's
weights on pictures from this project's sessions is out. Running published weights as published at
development time trains nothing; whether a given set's terms admit it is rule 3's question, and for
LPIPS's backbones it has no answer in what was read. The repository also takes no Python, so a
learned measure here means a network's inference written in C++ and its weights carried as a
dependency.

## 2. In motion

### What the video predictors rest on: spatio-temporal contrast sensitivity

- **Kelly (1979)**, the spatio-temporal threshold surface: **named only**. **Watson and Ahumada's
  pyramid of visibility (2016)**: log sensitivity linear in spatial frequency, temporal frequency
  and log luminance near the high-frequency limits (**search summary only**; no number used).
- **stelaCSF** (Mantiuk, Ashraf, Chapiro, *ACM TOG* 41(4) 145, 2022): sensitivity over spatial and
  temporal frequency, eccentricity, luminance and area, fitted to 11 papers' data (search summary
  of its project page; **the PDF did not parse**). Code a MATLAB class; its licence not read.
- **castleCSF** (Ashraf, Mantiuk, Chapiro, Wuerger, *J. Vision* 24(4):5, 2024): stelaCSF's
  dimensions plus colour direction and the background's chromaticity; one parameter set fits 18
  studies' data with an error of 3.59 dB (project page, read). The article itself refused the
  fetch (PubMed Central and the journal), so its text's licence and its equations are **not read**.
  Code (a MATLAB class) and the consolidated data on GitHub under the **MIT** licence, Graphics and
  Displays group, University of Cambridge, 2023 (the LICENSE file, read).
- **elaTCSF** (Cai, Bozorgian, Ashraf, Wanat, Mantiuk, SIGGRAPH Asia 2024, DOI 10.1145/3680528.3687586;
  arXiv 2503.16759, **CC BY 4.0**, read). A temporal CSF for **flicker detection** at low spatial
  frequency over temporal frequency, luminance, eccentricity and area. Eccentricity scales
  sensitivity by **S_ecc(e) = 10^(−k₁ e), k₁ = 0.0330933 per degree (Eq. 4, Table 2)** and
  stretches the temporal-frequency axis with it: **Eq. 5 divides (ω − ω_p) by (1 + k₂ e), k₂ =
  0.0341811 per degree, ω_p = −2 Hz, and adds ω_p back**, so at a given temporal frequency the
  periphery is read further down the curve, its high-frequency sensitivity falls less than its
  low-frequency one, which is how critical flicker frequency (CFF) can peak off the fovea; area enters
  by probability summation (Eqs. 7 to 10, β = 3.80022). Against eight datasets (Table 1) its error
  is 3.50 dB in sensitivity and 8.95 Hz in CFF, against stelaCSF's 6.13 dB / 11.47 Hz, castleCSF's
  5.45 / 13.07, Barten's CSF's 5.79 / 15.36 and the paper's Barten's CSF (HTF) row's 4.58 / 9.06
  (Table 3); the authors call the eccentricity of the CFF's
  peak a debate they contribute to rather than settle. Code licence not stated.
- **Krajancich, Kellnhofer, Wetzstein (2021)**, *ACM TOG* 40(4) 47, arXiv 2104.13514 (read through
  ar5iv; the text under arXiv's non-exclusive distribution licence 1.0, read on its abstract page): CFF measured on a high-speed display at 380 cd/m² (and 23.9 and 3.0
  for the luminance extension) from 0 to 56.8° of eccentricity at 0 to 2 cycles a degree, fitted as
  Eq. 2 with Table 3's constants (R² 0.889 to 0.938) and extended by luminance (Eqs. 5–6). **Temporal
  sensitivity is anti-foveated**: at the higher spatial frequencies the CFF rises from the fovea to a
  peak between about 10° and 30° and falls again. Code licence not stated.

### FovVideoVDP

Mantiuk, Denes, Chapiro, Kaplanyan, Rufo, Bachy, Lian, Patney, *ACM TOG* 40(4) 49, 2021. A visible
difference predictor **for wide-field video**: spatio-temporal contrast sensitivity, cortical
magnification and masking, from the display's luminance, size and resolution and the viewing
distance; heat maps show blur, compression, 30 Hz flicker and noise fading with eccentricity
(project page, read). Validated on a foveated-rendering dataset of its own, three independent
foveated video datasets and an image dataset, best of the metrics compared (project page; the
numbers were not read). It outputs JOD and difference maps (repository, read). **Licences**: the
paper not read (its PDF), its copyright ACM's; **the code, PyTorch and MATLAB, is CC BY-NC 4.0**
(the LICENSE file, read): **excluded**. Its constants could come only from its paper, which this
pass did not read.

### ColorVideoVDP

Mantiuk, Hanji, Ashraf, Asano, Chapiro, SIGGRAPH 2024 (*ACM TOG*, article 129, DOI 10.1145/3658144;
arXiv 2401.11485, **CC BY 4.0**, read through ar5iv: its first 100,000 characters, which in that
rendering run from the abstract to the start of Sec. 6.2 and so hold Secs. 3.11 and 5.1, and the rest
read separately for Secs. 6.2 to 7; section numbers below are ar5iv's, which arXiv's own HTML may
number otherwise).

- **Claims** the visibility of distortions between a test and a reference video or image: one
  quality in JOD (Eq. 16, 10 for identical), a per-pixel distortion map as a video (Sec. 3.10,
  Eq. 18), and a "distogram" of distortion by channel and band over time.
- **Input**: a display model (Sec. 3.1) — the display's width, horizontal resolution and the
  viewing distance give pixels a degree (Eq. 1); peak and black luminance, the ambient illuminance
  and the screen's reflectivity (Eqs. 2–3); the EOTF (sRGB or PQ) and the primaries.
- **Vision**: castleCSF (Sec. 3.4) as look-up tables, **with eccentricity fixed at 0** and the area
  at π·1.5² square degrees; the paper says it does not model foveated vision, unlike FovVideoVDP
  (Sec. 2). Four channels — achromatic sustained (low-pass) and transient (band-pass), red-green,
  violet-yellow — through temporal filters derived from castleCSF with 250 ms of support (Sec. 3.3).
- **Pooling** (Eq. 14): over pixels with an exponent of 2, bands 4, channels 4, frames 2, then to JOD.
- **Validated**: trained on XR-DAVID (336 videos, the authors' own, eight AR/VR display artefacts and
  streaming distortions) and UPIQ; tested on their held-out parts and LIVE HDR, LIVE VQA and
  KADID-10k; better than the second best, VMAF (Sec. 5.1); the correlations themselves are in a
  figure and a report not read.
- **Cost**: PyTorch on the GPU, as many frames in memory as fit, castleCSF precomputed as tables;
  processing times "comparable" to other metrics, with the timings in the supplementary document
  (Sec. 3.11, "Implementation details"); **the supplementary was not read**, so no time is taken.
- **Licences**: the text CC BY 4.0; **the code MIT** (Graphics and Displays group, 2024; the LICENSE
  file, read), PyTorch. Its display models (`display_models.json`, read): **`standard_4k` a 30-inch
  3840 × 2160 at 0.7472 m, 200 cd/m², contrast 1000, 250 lux**, which is **75.4 pixels a degree at
  the screen's centre** (*derived*: a 0.664 m width over 3,840 pixels against 13.04 mm for a degree
  at 0.7472 m; the README's sample output prints 75.4); `standard_hdr_pq` the same 30-inch
  3840 × 2160 at 0.7472 m, but BT.2020 with the PQ EOTF, 1,500 cd/m², contrast 1,000,000 and 10 lux. Machine-learned variants (`cvvdp-ml-saliency`, `cvvdp-ml-transformer`) were added in
  v0.5.0; their weights' terms were not read.

### The flicker and temporal-artefact measures graphics has published

- **tOF and tLP** (Chu et al., *ACM TOG* 2020, "TecoGAN", arXiv 1811.09393;
  read through ar5iv): **tOF = ‖OF(b_{t−1}, b_t) − OF(g_{t−1}, g_t)‖₁** with Farnebäck's flow and
  **tLP = ‖LP(b_{t−1}, b_t) − LP(g_{t−1}, g_t)‖₁** with LPIPS (Eq. 2), b the reference frames and g
  the generated: how differently two sequences change between frames, not how they differ. The
  authors report that the two followed their user studies (Table 3's caption). tLP inherits LPIPS's
  weights. Licences not read.
- **The motion-quality model** (Denes, Jindal, Mikhailiuk, Mantiuk, *ACM TOG* 2020): perceived
  quality from judder and from blur by the eye's pursuit, the refresh rate and the resolution,
  fitted on psychophysics from 50 to 165 Hz; Jindal et al. (2021) add the texture's content
  (**search summaries only**).
- **HDR-VDP-3's flicker task** (above): a still pair viewed alternating, not a sequence.
- **CGVQM** (Jindal, Sadaka, Thomas, Sochenov, Kaplanyan, *Computer Graphics Forum* (HPG 2025);
  arXiv 2506.11546, **CC BY 4.0**, read to its first 100,000 characters). A full-reference video
  measure for **rendering's** distortions: a 3D ResNet-18 pretrained on Kinetics-400 (human actions),
  frozen, with about 1,027 channel weights calibrated on GamingVideoSET, LIVE Livestream and the
  authors' CG-VQD (Sec. 4.1); a global score (the minimum over patches for a large frame) and error
  maps. CG-VQD is 80 sequences of path tracing (noise, fireflies, shimmer), neural denoising and
  supersampling (flicker and ghosting deliberately boosted), Gaussian splatting, frame interpolation
  and variable-rate shading, rated remotely on participants' own screens of at least 1080p at 60 Hz,
  stimuli 1024 × 1024 at 30 fps (60 for two scenes); **viewing distance, pixel density and luminance
  are not controlled** (Sec. 3.3). On CG-VQD (Table 1, Pearson / Spearman): **CGVQM-5 0.871 / 0.877**,
  FovVideoVDP 0.755 / 0.741, ColorVideoVDP 0.744 / 0.775, VMAF 0.715 / 0.790, LPIPS 0.696 / 0.748,
  FLIP 0.693 / 0.728, SSIM 0.639 / 0.805; the authors note ColorVideoVDP and FLIP over-estimating
  quality at high distortion. Cost: on 32 × 512 × 512 patches on an A100, CGVQM-2 27 % faster than
  CGVQM-5 (no absolute time read). **Licences**: code Apache-2.0 (the repository, read); a `weights`
  folder in the repository whose origin and terms were not read; the backbone's Kinetics-400
  pretraining's terms not read.

### VMAF and its kin

Netflix's VMAF fuses per-frame image measures — VIF at four scales, a detail-loss measure (DLM or
ADM), and a motion feature that is a frame difference of luminance — by a support-vector regressor
(**secondary**: later papers' descriptions; the 2016 post refused the fetch), and secondary sources
note its weakness on high-motion content because of that simple feature. Its models (`models_v0.md`,
read): **the default a 1080p HDTV in a living room at three screen heights**, the 4K model at 1.5
heights, the phone model at whatever distance its subjects chose; the default trained on lab ratings
of ten-second catalogue clips at **six resolutions crossed with three encodings — compression and
scaling, nothing else**; "NEG" variants switch off the gain an enhancement buys. Licence
**BSD+Patent** since February 2020, Apache-2.0 before (the repository, read). It claims nothing
about a rendering fault: none of its training is one, and its one temporal input does not compare
the two sequences' changes. In CGVQM's table it reads 0.715 / 0.790, between the image measures and
the video predictors.

## 3. The surround

**What is known of the owner's display**: three 4K monitors as one Surround display, 11,520 × 2,160
at 82 Hz ([second interactive session](second-interactive-session-2026-09-25.md)), driven at 10
bits ([ADR-0052](../adr/0052-the-picture-is-quantized-once-with-dither.md)), about 150° across by
[plan 01](../plan/01-critique.md)'s estimate, side monitors that may be turned inward (`--side-yaw`).
**Not recorded anywhere in the tree**: each screen's width, the eye's distance, the peak and black
luminance, the room's light. The bench's captures carry a resolution and the camera's field of view
(lantern-street's path: 75° vertical at 3840 × 2160), and neither is the display's geometry.

**What each measure says about eccentricity**:

| measure | eccentricity | what geometry it needs |
|---|---|---|
| LDR-FLIP, HDR-FLIP | none: every pixel foveal | pixels a degree |
| SSIM, MS-SSIM | none | a scale for the distance (Wang's F) |
| HDR-VDP-3 | none (the eye's age instead) | display model in absolute luminance, pixels a degree |
| LPIPS, CGVQM | none | none stated |
| VMAF | none | a distance in screen heights, fixed per model |
| ColorVideoVDP | none: castleCSF at e = 0 | display model, distance, frame rate |
| FovVideoVDP | yes: CSF and cortical magnification by distance from the gaze | display model, distance, **a gaze point** |
| castleCSF, stelaCSF | a dimension of the function | — (a component, not a measure) |
| elaTCSF, Krajancich et al. | flicker over eccentricity | — (components) |

**What the engine would have to state** to use any of them over the surround, per screen: its width
in metres, its resolution, the eye's distance to it and its angle to the eye (the yaw), the peak and
black luminance and the ambient light, and a gaze (a fixed point, or none and every pixel foveal).
The geometry is not the camera's: the frustum-correct eye for a 4K picture drawn at 75° vertical
sees **24.6 pixels a degree at its centre**, at the engine's 55° default **36.2** (*derived*:
1,080 / tan(fov / 2) pixels a radian), where FLIP's reference condition, 67.02, puts the eye where a
0.7 m screen spans 53.1°; a picture drawn 107.5° across (75° vertical at 16:9, *derived*) seen at
that density is seen from far behind its projection centre. A measure reads the physical geometry;
the camera's field of view says only what the picture holds.

**The periphery is not one number.** If the three screens share 150° equally and the eye is at their
centre, the side screens' centres are 50° out and their outer edges 75° (*derived*, an assumption on
plan 01's figure). Over that range spatial sensitivity falls steeply (the reason FovVideoVDP exists),
while temporal sensitivity does not fall with it: elaTCSF's factor alone would be 10^(−0.0331 · 25)
= 0.15 at 25° and 0.022 at 50° at low temporal frequency (*derived* from Eq. 4), but its Eq. 5
stretches the temporal-frequency axis with eccentricity, so high temporal frequencies lose less, and Krajancich et al. measured the CFF of the higher
spatial frequencies peaking between 10° and 30°. So a weighting that turns the periphery down —
`render.compare`'s centre weight, which [plan 04 §4.6](../plan/04-renderer.md#46-extreme-displays)
asked for — turns down the side screens' **flicker and pops** with their blur, which by these two
papers is the wrong way for the temporal faults (a reading of the read papers, not a measurement).

## 4. What a shortcut's faults look like to each, by its own model

Reasoning from each measure's published form, not a measurement. "Sees" means the model has a term
that responds to the fault; how strongly is stage 1's to measure.

| fault | FLIP | SSIM | HDR-VDP-3 | LPIPS | ColorVideoVDP | FovVideoVDP | CGVQM | VMAF |
|---|---|---|---|---|---|---|---|---|
| a few pixels far past threshold (a capped light's worst) | sees each in the map; the mean hides them, the max and p95 do not | barely: a window's statistics move little for a few pixels | sees, at the display's real contrast | trained on patches; a few pixels in 64 × 64 move little | sees in the map; pooling with exponent 2 over pixels dilutes | as ColorVideoVDP, less off the gaze | its minimum over patches is decided by the worst patch | hides them in VIF's and DLM's means |
| a low-amplitude error over a large area (an interpolated cache) | sees about uniformly, relative to white, whatever the light level | its luminance term only, near 1 | weighs it by the level's real sensitivity, which falls in the dark | uncertain: small amplitudes are near its features' noise | as HDR-VDP-3, per frame and pooled | as ColorVideoVDP, less in the periphery spatially | trained on such errors (denoising, VRS) | not its training |
| flicker | only when the two stills differ; a sequence it cannot read | no | its flicker task is alternation of a pair, not a sequence | no (tLP does, as a change of LPIPS) | the transient channel | the transient channel, **kept in the periphery** | trained on flicker | its frame difference is not a comparison of two changes |
| crawl on an edge | each frame's edge moved: the feature term per frame; not the motion | per frame | per frame | per frame | transient channel at the edge's frequency | the same, by eccentricity | trained on shimmer and aliasing | no |
| a pop at a hand-off between LODs | one frame's difference if that frame is compared | one frame | one frame | one frame | the transient channel; frame pooling (exponent 2) dilutes one frame over a clip, the distogram and the map keep it | the same, and a pop at the side screen is not turned down | likely, in time; pooling not read for single events | diluted |

The pattern: **the still measures see a fault as each frame's difference and nothing of its course
in time**; of the published video predictors only FovVideoVDP carries the periphery, and its code is
non-commercial; ColorVideoVDP carries time, luminance and colour under MIT and is foveal everywhere;
CGVQM was trained on rendering's own faults and has no geometry and weights whose terms are open.

## 5. How each would read R213's cap — reasoning from each model, not a measurement

**What R213 measured** ([lights by reach](lights-by-reach.md#the-cap),
[renderer](../subsystems/renderer.md#a-cap-on-the-shadow-rays-a-pixel-traces)): lantern-street's 32
fires at night, 3840 × 2160, traced primaries and shadows, the bounce and the accumulation off, frame
300 of the held path with the cap on against every ray traced: **3,009,497 of 8,294,400 pixels
differ (36.3 %, *derived*), 71,531 past 2 of 255 (0.86 %), the worst 12**. The capped pixels, 3.2 to
3.4 million, are the street's floor and walls; a pixel's error is bounded by the untraced fires'
unshadowed light, and the estimate is worst where the traced and the untraced lights' visibilities
differ, which the walls make common. Where on the frame the 71,531 lie the page does not record beyond
that. The cut reads nothing of the eye, so a point's traced set does not change as the eye moves;
**the fires flicker** (R85's flicker on the fixture), and the ranking is by their unshadowed
irradiance.

**What a code is on the display** (*derived*): under the picture's 2.2 power one code at code c
changes the light by about 2.2 / c of itself — **11 % at code 20, 4.4 % at 50, 2.2 % at 100** — and
in CIE L* by **about 0.38 to 0.40 a code at codes 20 and 128 alike**. At a 200 cd/m² white
(ColorVideoVDP's `standard_4k`, an assumption; the owner's display is not recorded) code 20 is
0.74 cd/m² and code 50 5.6. A night street lies low in the codes; the capture's `codes.json` would
say how low, and was not kept. So "2 of 255" is about one step of lightness everywhere, a large
relative contrast in the dark, and at a light level where the eye's contrast sensitivity is lower
than at a day's: two effects that pull opposite ways, which **only a measure with absolute
luminance (HDR-VDP-3, ColorVideoVDP, FovVideoVDP) can weigh**.

**By each model, at the bench's geometry as it stands (no display recorded; FLIP's 67 ppd default):**

- **FLIP**: its colour difference is in a lightness-like space relative to white, so the one or two
  code differences over a third of the frame read about as the engine's own test of a one-level flat
  change does, **about 0.03** a pixel (the image page's pinned 0.0299, two mid greys 0.394 of L* apart;
  a step from code 20 to 21 is about 0.38 in L*, *derived* under the 2.2 power: about the same, not
  the same), under the 0.1 the image page names; the pooled mean, mostly zeros and 0.03s, of
  the order of 0.01. The worst pixels, 12 codes or about 4.7 in L*, would score of the order of
  0.15 if the exponent of 0.7 were all that acted (12^0.7 ≈ 5.7 times the one-code value; the
  redistribution and the CSF filter of a small blob lower it): **at or above the 0.1 line, as
  differences an alternating viewer could find**. FLIP's alternation is the most searching way to
  look, not the owner's first look at a build standing alone. FLIP does not know it is night.
- **SSIM**: a flat one-code offset in a dark window moves only the luminance term: at means of 20
  and 21 codes it is 0.9988, at 128 and 129 0.99997 (*derived*, with the paper's C₁ = (0.01 · 255)²).
  It would call the frame nearly identical, which says more about SSIM than about the cap.
- **HDR-VDP-3**: cannot be run without a display in absolute luminance. With one, it would weigh the
  dark street's codes at their real contrast and the eye's lower sensitivity at a few cd/m²; its
  side-by-side map, the nearer of its tasks to judging a build alone, would by its model show less
  than its flicker task, and the worst pixels are where both would look.
- **LPIPS**: no threshold to read against and no display; its patches' distortions were far larger
  than a code or two. Its number would not say whether the cap is visible.
- **ColorVideoVDP**: as HDR-VDP-3 for the still pair. **In motion it can see what no still can**: the
  ranking follows the fires' flickering irradiance, so at a pixel where two fires' weights cross, the
  traced set can change between frames, and the cap's error would change with it at the fires' own
  rate in a held view — the transient channel's domain. Whether that happens on lantern-street is
  not known: R213 compared one frame.
- **FovVideoVDP**: as ColorVideoVDP, and on the surround the walls' spatial error at a side screen
  turned down by eccentricity, a temporal change there much less.
- **CGVQM**: trained on rendering noise and flicker, no geometry; its patch minimum would be the worst
  wall's; no basis to place its number against a threshold.
- **VMAF**: compression and scaling at three screen heights: nothing to say about the cap.

**Stage 1's first real run** (a reading, proposed): (a) the same pair again — frame 300, cap on
against `renderer.shadow.light_rays=0`, the bounce and accumulation off — captured with
`--capture-channels light` and its `codes.json`, through the first built measure at two stated
geometries (FLIP's 67.02 ppd reference condition and the frustum-correct 24.6 ppd of the picture's
75°), its map and pooled numbers written beside the 0.86 % and the 12; (b) the held path's frames
240 to 480 flown under both settings on one world clock, every frame kept, and the temporal change of
each side and of their difference (R171's measure on the difference sequence first), to answer
whether the fires' flicker makes the cap's error flicker.

## 6. A reading for the design, kept apart from the sources

Everything below is this page's reading of the above, written for the next stage; it decides
nothing.

### Which to build first, and from what

1. **ColorVideoVDP**, from its paper (arXiv, CC BY 4.0) with **its MIT code as the check of every
   constant** (and castleCSF beneath it, whose MIT LICENSE was read; its article was not reached, so
   its constants would come from that code until the article is read): the one published predictor that models time,
   colour, absolute luminance and the display's geometry, validated against VMAF and the image
   measures, with a permissive licence on text and code alike, so a constant may be taken from either
   and checked against both. Built from the paper as FLIP was, no Python; its still-image form
   (Eq. 17) answers tool (1) and its video form tool (2). What it lacks is the periphery.
2. **A peripheral flicker map**, from elaTCSF (CC BY 4.0, its constants in Table 2) and checked
   against Krajancich et al.'s CFF (Eq. 2, Table 3): the visibility of the **difference sequence's**
   low-spatial-frequency temporal change at each pixel's eccentricity under a stated geometry and
   gaze — small, cheap, and the one thing over the side screens that ColorVideoVDP fixes at the fovea.
   The alternative is to set castleCSF's own eccentricity per pixel inside ColorVideoVDP, which
   departs from a metric calibrated at e = 0 and would have to say so beside every number.

Not first, with the reason: **HDR-FLIP** (BSD-3 code and a paper, small, but still foveal and
timeless: worth adding to the existing FLIP as `render.compare`'s HDR form, not as the next tool);
**HDR-VDP-3** (GPL-2.0 code; ColorVideoVDP is the same group's later model for video);
**FovVideoVDP** (CC BY-NC code; its paper not read; built from the paper alone it would be the
strongest candidate for the surround, and a later stage may read it); **LPIPS, tLP, CGVQM** (models
with weights whose terms are open, Python, and plan 12's rule 3 before any of it); **VMAF** (not
rendering's faults).

### What each needs beyond two pictures

The display per screen (width, resolution, distance, yaw), its peak and black luminance and the
room's light, the EOTF the pictures were encoded with (the SDR picture's 2.2 power, which the
capture inverts), **linear light** rather than codes (the capture's EXR, 1.0 at white, times the
white's luminance), the frame rate (an offscreen flight's 60 frames a second of world time), and a
gaze for anything peripheral. A run with any of these unstated says which defaults it took, beside
its number.

### What is reported, and where

A map (the measure's heat map, as FLIP's) and one pooled number (JOD; for the flicker map the share
of pixel-frames past its threshold) beside the 2-of-255 count and `render.compare`'s FLIP, PSNR and
SSIM, and never in place of a pinned check; for a flight, the per-frame number and its worst stretch,
since frame pooling dilutes a single pop.

### The flight under two settings

Two offscreen flights of one path, frame f of each at the same world time (above), the same seed,
the same state of everything temporal (the bounce, the accumulation, the cache's memory) on both
sides, the display's dither a function of place alone; frame f of A against frame f of B. A 4K
light EXR is about 50 MB a frame in halves and a 241-frame flight about 12 GB a side (*derived*,
uncompressed), so the comparison should take frames as they are drawn rather than from disk; and
FLIP alone at today's 3.1 s a 4K pair would take about 12.5 minutes for 241 frames on one thread
(*derived*). Who holds the GPU lock while two flights and a measure run is the design's.

### Questions the design must answer, with the evidence each way

- **Which way of looking it predicts.** FLIP and HDR-VDP-3's flicker task predict alternation, the
  most searching protocol; HDR-VDP-3's side-by-side task and the video predictors' sequences are
  nearer to watching. The owner judges a build standing alone first: a measure tuned to alternation
  over-reports for that, and is still the right one for "where to look".
- **The gaze.** A fixed gaze at the centre screen turns down a side screen the owner turns to; no gaze
  (every pixel foveal) over-reports the periphery's spatial error and is right for its temporal one
  (§3). Reporting both brackets it.
- **The display's luminance.** Night scenes are where a shortcut like R213's shows, and where the
  sensitivity a model assumes depends most on the peak luminance the owner's display really has,
  which nobody has measured; ColorVideoVDP's 200 cd/m² default is a convention, not the owner's display.
- **Pooling.** ColorVideoVDP's frame exponent of 2 dilutes one bad frame over a clip; tool (2) asks
  for the worst stretch, which the per-frame map gives and the JOD does not.
- **Where it runs.** FLIP is 3 s a 4K pair on one CPU thread today; a video predictor's pyramids
  and temporal filters cost more a frame; a GPU form is a separable-filter chain. Whether the first
  build is CPU and slow or GPU and early is open.
- **Learned measures.** CGVQM correlates best with ratings of rendering's faults (0.871 against
  0.744 to 0.755 for the video predictors on its own dataset) but rests on a backbone pretrained on
  Kinetics-400 and ratings taken at no controlled geometry; whether its weights' terms admit it under
  plan 12's rule 3 is unread, and it would be a network written here in C++.

## The sources, and how far each was read

"Text" is the licence or copyright the paper's text stands under; "code" the reference
implementation's; a dash where there is none.

| source | what is taken | read | text | code | weights |
|---|---|---|---|---|---|
| Andersson et al. (2020), LDR-FLIP, HPG, DOI 10.1145/3406183 | the claim (alternation map), the validation in outline; the model through the engine's image page | abstract (NVIDIA's page); the PDF too large | ACM copyright | BSD-3-Clause (NVlabs/flip, read) | none |
| Andersson, Nilsson, Shirley, Akenine-Möller (2021), HDR-FLIP, EG short | the claim; the tool's exposure example | abstract; C++ README | Eurographics copyright | BSD-3-Clause | none |
| Wang, Bovik, Sheikh, Simoncelli (2004), SSIM, IEEE TIP 13(4) | the scale's dependence on distance; F = max(1, round(N/256)) at 3–5 heights | the authors' page | not read | permission notice: use and modify with citation | none |
| Wang, Simoncelli, Bovik (2003), MS-SSIM | named | not read | — | — | none |
| Daly (1993), VDP; Mantiuk et al. (2011), HDR-VDP-2 | named | not read | — | — | none |
| Mantiuk, Hammou, Hanji (2023), HDR-VDP-3, arXiv 2304.13625 | tasks, inputs, age not eccentricity, calibration sets | read (ar5iv) | CC BY 4.0 | **GPL-2.0** (HDR-VDP 3.0.7, SourceForge, read): excluded | none |
| Zhang, Isola, Efros, Shechtman, Wang (2018), LPIPS, arXiv 1801.03924 | BAPPS 64×64; 2AFC 73.9 / 68.9 / 63.1 %; no display | read (ar5iv) | arXiv non-exclusive licence | BSD-2-Clause (read) | linear layers in the repository; backbones' ImageNet weights: no licence stated, ImageNet's non-commercial terms argued (secondary) |
| PyTorch forum threads on torchvision's weights | that no licence is stated on the weights | secondary | — | — | — |
| DISTS, PieAPP | named | not read | — | — | — |
| Kelly (1979); Watson, Ahumada (2016), pyramid of visibility | the pyramid's form in words | not read; search summary only | — | — | — |
| Mantiuk, Ashraf, Chapiro (2022), stelaCSF, ACM TOG 41(4) | its dimensions; 11 papers | search summary of the project page; PDF unreadable | not read | MATLAB; not read | none |
| Ashraf, Mantiuk, Chapiro, Wuerger (2024), castleCSF, J. Vision 24(4):5 | dimensions; 18 studies, 3.59 dB | project page | not reached | MIT, Graphics and Displays group, University of Cambridge, 2023 (LICENSE, read) | none |
| Cai, Bozorgian, Ashraf, Wanat, Mantiuk (2024), elaTCSF, arXiv 2503.16759 | Eqs. 4–5, k₁ 0.0330933, k₂ 0.0341811, β 3.80022; Table 3's errors; the CFF debate | read (arXiv HTML) | CC BY 4.0 | not stated | none |
| Krajancich, Kellnhofer, Wetzstein (2021), arXiv 2104.13514 | 0–56.8°, 0–2 cpd, 380 cd/m²; Eq. 2, Table 3; CFF peak 10–30° | read (ar5iv) | arXiv non-exclusive distribution licence 1.0 (abstract page, read) | not stated | none |
| Mantiuk et al. (2021), FovVideoVDP, ACM TOG 40(4) 49 | what it models and needs; its validation in outline | project page; repository README | not read (ACM) | **CC BY-NC 4.0** (LICENSE, read): excluded | none |
| Mantiuk, Hanji, Ashraf, Asano, Chapiro (2024), ColorVideoVDP, arXiv 2401.11485 | Sec. 3.1–3.11, Eqs. 1–3, 14, 16–18; e = 0; training and tests (Sec. 5.1); the supplementary's timings not read | read (ar5iv, whole; section numbers as ar5iv prints them) | CC BY 4.0 | MIT (LICENSE, read); `display_models.json` read | ML variants' terms not read |
| Chu et al. (2020), TecoGAN, arXiv 1811.09393 | tOF and tLP, Eq. 2 | read (ar5iv) | not read | not read | LPIPS's |
| Denes, Jindal, Mikhailiuk, Mantiuk (2020); Jindal et al. (2021) | judder and blur against refresh rate and resolution | search summaries only | — | — | — |
| Jindal, Sadaka, Thomas, Sochenov, Kaplanyan (2025), CGVQM, arXiv 2506.11546 | architecture, CG-VQD, Table 1, viewing not controlled | read (arXiv HTML, first 100,000 characters) | CC BY 4.0 | Apache-2.0 (read) | a `weights` folder, terms not read; Kinetics-400 pretraining, terms not read |
| Netflix, VMAF repository and `models_v0.md` | licence; models' viewing conditions; training conditions | read | — | BSD+Patent (Apache-2.0 before February 2020) | the models are the repository's |
| Later papers on VMAF's features (arXiv 2310.15578 and others) | VIF at four scales, DLM/ADM, motion, SVR | secondary | — | — | — |
| Dougherty et al. (2003), cortical magnification | named as FovVideoVDP's (secondary) | not read | — | — | — |
| Not read at all | FLIP's and FovVideoVDP's papers, stelaCSF's paper, castleCSF's article, the Ray Tracing Gems II chapter on FLIP, Netflix's 2016 post on VMAF (refused), Kim et al. (2013), Wolski et al. (2018), Ye et al. (2019) | — | — | — | — |
