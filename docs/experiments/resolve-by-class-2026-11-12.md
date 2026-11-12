# The material resolve drawn by pixel class (R151, 2026-11-12)

Roadmap R151. [R148](resolve-cost-2026-11-11.md) found that a large part of the resolve at the
owner's settings (3840 × 2160, traced primaries and shadows, the bounce) is the **shape of the
pass**, one fullscreen draw of one shader that holds every kind of pixel: about 1.1 ms of a 2.8 ms
resolve under the elder grove belongs to no class of pixel alone, a scene with two ponds pays
0.74 ms for the water module on a frame with no water in view, and the one resolve pipeline is 87
to 94 % of a cold start. This page sorts the picture's pixels by what they are before they are
shaded, measures what that returns, and says what it would take to return the rest. What was
built is described where it lives, [renderer](../subsystems/renderer.md#the-resolve-by-pixel-class).

**The answer.**

1. **The split as the brief drew it — a class written as depth, the same resolve drawn once per
   class with a depth-equal test, nothing changed inside `fs_resolve` — is exact and is not a gain
   by itself.** It draws the one draw's bytes (0 differing pixels on seven poses of three scenes).
   Where classes meet along lines it costs what the one draw costs plus a classify pass of 0.055 to
   0.08 ms; where they interleave at the pixel's scale, under leaves, it is **dearer**: the elder
   grove's resolve goes from 2.75 to 3.18 ms.
2. **Why: the depth test rejects quads, not pixels.** A 2 × 2 quad the test keeps runs all four
   lanes, and a lane whose pixel failed the test runs its own pixel's whole path as a helper. The
   sky's draw under the grove costs 1.04 ms, of which the sky's pixels are 0.05: the rest is leaf
   and branch pixels shaded a second time, one or two to a shading group.
3. **One test at the top of the resolve's entry point turns it into the gain R148 pointed at.** A
   lane whose pixel cannot be of the class its draw shades ends before it shades (the class exit,
   committed in the second pass). The elder grove's resolve is **2.24 ms against 2.76 (−18 %),
   the frame 0.46 ms shorter**, the same bytes. That is half of the 1.1 ms; the rest is mixing
   inside a class's own draw, which only fuller groups can return (the compute route, below).
4. **Drawing the pixels that cannot touch water with the module that has none returns the water
   module's weight**: 0.6 ms of a 2.2 ms resolve at oasis-pool's pool and of 1.8 ms looking away
   from it, 0.55 to 0.6 ms of 2.5 to 2.9 ms on the layered ground with its ponds out of view,
   and 0.2 ms at the two poses that are mostly pond and bank, where it was dearer until the wet
   class asked the records' levels (the second pass: 75 to 98 % of the class was ground above a
   pond's fringe). It is not exact to the byte: the two modules differ by one code on 2 pixels of
   8.3 million at one pose of seven, in the resolve's tail and not in the material.
5. **What the default is, and why, is "The default" in the second pass.** `auto` draws by class
   where the scene holds leaves and leaves every other scene to the one draw; `modules` is a
   lever until the two pixels are closed.

The page keeps the first pass as it was measured (to "What the frame budgets say") and the
second after it; where the two give a figure for the same thing, the second's is from the
committed tree.

Everything below is `msvc-release` on the RTX 5090 (driver 617.42), 3840 × 2160 offscreen,
`--shadows rt`, `renderer.gi.mode=bounce` unless a line says the bounce is off, one binary with
`renderer.resolve.classes` alternated run by run (off, draws, modules, twice or three times
round), each run a flight of a path that holds each pose 300 frames; a figure is the median of a
pose's frames 40 to 300 from the GPU's timers. The machine was shared all morning (another agent's
forest flights and tests on the same GPU, builds on the CPU: other processes at 11 to 100 % of the
CPU, the harness's `quiet` false in every run), and it was restarted by a Windows update at 03:27,
between the first measurements and the rest. A loaded frame reads up to 0.4 ms dearer in `resolve`
than a quiet one of the same run; where a run's median sat among loaded frames the table gives the
run's tenth percentile as well, which is the quiet frames'. Every difference is read inside one
lease of the GPU lock.

## What was published, and how far it was read

An hour, not a night; none of it was read in full.

| Source | Read | What it says about sorting |
|---|---|---|
| Burns and Hunt, "The Visibility Buffer: A Cache-Friendly Approach to Deferred Shading", JCGT 2(2), 2013 | The abstract and what later writers say of it; the paper's PDF was fetched and could not be opened here | Visibility stored as primitive ids so that shading reads only what the visible sample needs. Nothing read of how it groups samples before shading |
| Karis, Stubbe and Wihlidal, "Nanite: a deep dive", SIGGRAPH 2021 course | Not opened; its material pass as restated in the notes below | The material's id written as depth; each material drawn as screen-covering geometry with a depth-equal test; later a grid of 64 × 64 pixel tiles so that a material's draw covers only the tiles it touches |
| Wihlidal, "Nanite GPU-driven materials", GDC 2024 | One reader's notes on the slides, whole ([notes](https://scthe-github-io.vercel.app/blog/nanite-materials-notes)); the slides themselves not opened | Unreal 5.4 moved the shading to compute: pixels counted per **shading bin** (a unique material, in the simplest definition), offsets reserved, pixel locations scattered into one list a bin, and a dispatch a bin over its list |
| Epic's knowledge-base article on Nanite compute materials | The teaser only (the article's body did not load) | Names a "1x1 shade binning mode" beside the quad modes |
| Engel, "Triangle visibility buffer", 2018 | A summary of the post | The Forge's buffer is shaded by **one** screen-space pass for every material; only the fill is split, opaque from alpha-tested |
| The Forge, release 1.57 notes (visibility buffer 2) | The release note's paragraph | The fill moved to compute; nothing on shading by class |

**What do they sort by?** By material: Nanite's 2021 pass by the material's id as depth, its 2024
pass by shading bin, and Eidos Montreal's Dawn engine (in the same notes) by a material id copied
into a 16-bit depth buffer with one rectangle a material. The Forge does not sort. Nobody sorts by
*class* in this page's sense, a handful of kinds; the engines above have thousands of materials
each with its own shader, where this engine has one resolve shader and a table.

**What did the depth-equal route cost that made them leave it?** By the notes: draws issued for
materials whose pixels the GPU had already culled, each a pipeline and descriptor change "for
nothing" (3,075 of 3,779 bins empty in the talk's scene, and skipping them "saved nearly 1 ms");
and fullscreen and tile triangles "bound by 2x2 quads", with context rolls. A naive compute pass
was slower than the 2021 raster route until the binning was added. This page's measurement is the
second of those costs on this engine, with a number: what a quad that straddles two classes costs.

## Step 2: the same resolve, once per class

A classify pass writes each pixel's class as depth; the resolve's own module and entry point are
drawn once per class at that depth, depth-equal (`renderer.resolve.classes=draws`).

Resolve milliseconds, the classify pass's own, and the frame's sum of timed passes; traced
primaries, then `--raster hw`:

| scene and pose | one draw | split | classify | frame, one draw | frame, split |
|---|---:|---:|---:|---:|---:|
| elder-grove (before the restart, the quietest pair) | 2.751, 2.748 | 3.176, 3.178 | 0.055 | 5.82 | 6.32 |
| — after it, three flights each; median (tenth percentile) | 3.13 to 3.21 (2.74) | 3.60 to 3.65 (3.17) | 0.055 | 6.48 to 6.61 (5.77) | 7.06 to 7.21 (6.28) |
| — `--raster hw` | 2.732, 2.734 | 3.149, 3.148 | 0.055 | 4.94 | 5.41 |
| oasis-pool, the pool | 2.160, 2.161 | 2.171, 2.375 | 0.067 | 3.65 | 3.76 to 3.86 |
| — looking away from it | 1.775, 1.778 | 1.770, 1.772 | 0.071 | 3.04 | 2.94 to 3.09 |
| — `--raster hw`, pool / away | 2.165 / 1.772 | 2.155 to 2.386 / 1.774 | 0.068 / 0.072 | 2.79 to 2.99 / 2.35 | 3.04 to 3.10 / 2.61 |
| layered ground with ponds, sandy shore | 2.409, 2.406 | 2.396, 2.649 | 0.064 | 3.98 to 4.01 | 4.11 to 4.25 |
| — trail | 3.214, 2.898 | 2.936, 2.954 | 0.079 | 4.67 to 4.76 | 4.60 to 4.63 |
| — mountain | 2.558, 2.562 | 2.345, 2.551 | 0.070 | 4.12 to 4.13 | 3.92 to 4.01 |
| — boggy pond | 2.248, 2.246 | 2.394, 2.628 | 0.073 | 3.79 | 3.98 to 4.12 |

The layered ground's flights were taken beside another client of the GPU (it read 20 to 91 % busy
at their starts) and their second figures are loaded ones; its `--raster hw` column was flown only
with the lines of "The class exit" below. Read: **under leaves the split costs 0.43 ms of resolve
and 0.5 ms of frame; elsewhere it costs the classify pass and between nothing and 0.15 ms.**

**The picture.** The bounce off (under it the same binary does not draw the same bytes twice),
one capture a pose at 3840 × 2160, the PNG files' hashes and, where they differ, the pixels
counted: split against one draw, **0 pixels differ** at the grove, at both of oasis-pool's poses
and at all four of the layered ground's.

**What the classes are on screen**, from the classify pass's image read back (the census,
`renderer.resolve.class_census`): pixels by class, and the 2 × 2 quads that hold more than one.

| pose | sky | rigid | ground | masked | deformed | water | wet | mixed quads |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| elder-grove | 49.8 % | 29.3 % | | 10.6 % | 10.3 % | | | 132,557 of 2,073,600 (6.4 %) |
| oasis-pool, the pool | 33.4 % | 33.6 % | | | | 2.9 % | 30.1 % | 4,992 (0.24 %) |
| — away | 37.8 % | 62.2 % | | | | | | 1,059 (0.05 %) |
| ground, sandy shore | 34.7 % | | 7.4 % | | | 20.6 % | 37.3 % | 3,681 (0.18 %) |
| — trail | 27.8 % | | 72.2 % | | | | | 1,458 (0.07 %) |
| — mountain | 51.1 % | | 48.9 % | | | | | 1,418 (0.07 %) |
| — boggy pond | 34.1 % | | 8.7 % | | | 16.9 % | 40.2 % | 3,752 (0.18 %) |

The grove's shares are R148's census to a tenth of a percent, from a different pass.

### What the driver did with the early test

It rejects before shading, and it rejects **quads**. Under the grove, each class's draw alone
(`renderer.resolve.class_mask`, the other classes' pixels left the clear), resolve ms:

| draw | as built | with every surface pixel ended after its decode (`stop_after=decode`) | with the class exit (below) |
|---|---:|---:|---:|
| the one draw | 2.75 | 0.087 | 2.76 |
| every class | 3.18 | 0.077 | 2.25 |
| sky alone (49.8 % of the pixels) | **1.04** | 0.038 | 0.055 |
| rigid alone (29.3 %) | 0.573 | | 0.582 |
| masked alone (10.6 %) | 0.997 | 0.016 | 0.944 |
| deformed alone (10.3 %) | 0.873 | | 0.803 |
| every class but the sky | 2.26 | 0.042 | 2.17 |

- **Not five fullscreen passes**: the five draws cost 3.18 ms, not five times 2.75. A class no
  pixel holds costs 0.002 to 0.008 ms a draw.
- **Not pixels either.** The sky's pixels do nothing (they read their visibility word and are
  discarded), yet the sky's draw costs 1.04 ms, and 0.038 when the stop word makes every surface
  pixel's path short. So the sky's draw was running surface pixels' paths: the lanes of the quads
  that hold a sky pixel and a leaf or a branch. 132,557 quads hold more than one class, and the
  five draws run 2,211,362 quads where the one draw runs 2,073,600 (6.6 % more); those extra runs
  cost 0.43 ms because the lanes in them are the dearest in the frame (the crown's edge, where the
  sun's ray is asked through the leaves) and sit one or two to a group of thirty-two.
- **The surface classes by themselves are cheaper apart than together**: 2.26 ms for the three
  drawn as three against 2.75 for the one draw less the sky's 0.05 to 0.16. That 0.4 ms is the
  mixing R148 measured, returned; the sky's draw then takes it back twice over.
- **Asking for the early tests changes nothing.** A scratch module with `[earlydepthstencil]` on
  an entry point that calls the resolve's: every class 3.41 to 3.59 against 3.43 to 3.55 in the
  same lease, the sky alone 1.052 against 1.046. The driver already tests early; what it does not
  do is drop a lane.
- **The shader holds no derivative.** The resolve's SPIR-V (`visibility_resolve_rt`, 396,084
  bytes) has no implicit-level sample and no `OpDPdx`, `OpDPdy` or `OpFwidth`: it takes every
  gradient from its triangle. So the lanes are not kept for a derivative the shader asks for; the
  hardware shades a quad whole.

### The class exit: the lines that are not in the tree

A lane whose pixel is not of the class its draw shades can end itself: the draw's class is the
fragment's own depth, and the pixel's class is `classify_pixel`, the classify pass's function. A
scratch build put that test at the top of `fs_resolve` (the one draw's triangle is at depth 0,
which is no class's, so it shades everything as before):

```slang
  if (in.position.z != 0.0) {
    const uint2 class_pixel = uint2(in.position.xy) - uint2(params->view_x, params->view_y);
    if (resolve_class_depth(classify_pixel(params, class_pixel)) != in.position.z) {
      discard;
      return float4(0.0, 0.0, 0.0, 0.0);
    }
  }
```

with `#include "resolve_class.slang"` in front of the entry point. The lines are inside the
resolve's entry point, which the indirect light's row and the running water's held this morning,
so they are **not committed**: the patch is in the row's hand-back, and it recompiles the sixteen
variants. Measured from that build (the binaries copied out of the tree, the tree put back), the
same flights:

| scene and pose | one draw | split (`draws`) | split (`modules`) | classify |
|---|---:|---:|---:|---:|
| elder-grove; median (tenth percentile), three flights | 2.76 to 3.17 (2.74) | **2.25 to 2.27 (2.23)** | | 0.055 |
| — frame | 5.80 to 6.62 (5.77) | 5.89 to 5.96 (5.31) | | |
| — `--raster hw` | 2.760, 2.762 | **2.231, 2.237** | | 0.056 |
| oasis-pool, the pool | 2.199, 2.197 | 2.246, 2.210 | **1.814, 1.835** | 0.067 |
| — away | 1.826, 1.815 | 1.903, 1.898 | **1.205, 1.200** | 0.071 |
| — `--raster hw`, pool / away | 2.195 / 1.809 | 2.207 / 1.905 | 1.806 / 1.197 | 0.068 / 0.072 |
| layered ground, sandy shore | 2.456, 2.458 | 2.494, 2.471 | 2.399, 2.400 | 0.064 |
| — trail | 2.937, 2.943 | 3.015, 2.794 | **2.212, 2.213** | 0.079 |
| — mountain | 2.587, 2.586 | 2.419, 2.424 | **2.014, 2.014** | 0.070 |
| — boggy pond | 2.299, 2.297 | 2.409, 2.387 | 2.309, 2.328 | 0.072 |
| — `--raster hw`, shore / trail / mountain / bog | 2.461 / 2.944 / 2.584 / 2.289 | 2.476 / 2.799 / 2.425 / 2.392 | 2.404 / 2.219 / 2.016 / 2.315 | 0.065 / 0.079 / 0.070 / 0.072 |

The grove's one-draw medians of 3.1 are flights taken while another client used the GPU; its
split is 2.25 to 2.27 in every flight, quiet or not, and both tenth percentiles agree with the
quiet pair before the restart (2.75). **Under leaves: −0.50 ms of resolve (−18 %), −0.45 ms of
frame with the classify pass counted.** The picture is the one draw's bytes at the grove and at
all six other poses under `draws`.

Two smaller things the table shows. The lines cost the *one* draw 0.01 to 0.05 ms (it tests its
depth, and the compiler keeps a little more alive). And on a scene with no mixing, `draws` with
the lines is 0.03 to 0.11 ms dearer than the one draw at four poses and 0.17 cheaper at one: every
fragment now decodes its class before it shades, the water variants walk the shore records for it,
and there is no mixing to return. The gain there is `modules`'.

## Step 3: water and not-water

`renderer.resolve.classes=modules`: the sky, rigid, ground, masked and deformed classes drawn by
the same combination's module with no water in it, and the water and wet classes by the scene's
own. The figures as committed (no class exit) are beside those with it:

| scene and pose | one draw | `modules`, as committed | `modules`, with the class exit | classify |
|---|---:|---:|---:|---:|
| oasis-pool, the pool | 2.16 | 1.778, 1.786 | 1.814, 1.835 | 0.067 |
| — away (no water in view) | 1.78 | **1.185, 1.181** | **1.205, 1.200** | 0.071 |
| layered ground, sandy shore | 2.41 to 2.46 | 2.337, 2.339 | 2.399, 2.400 | 0.064 |
| — trail (no water in view) | 2.90 to 2.94 | 2.509, 2.513 | **2.212, 2.213** | 0.079 |
| — mountain (no water in view) | 2.56 to 2.59 | 2.061, 2.314 | **2.014, 2.014** | 0.070 |
| — boggy pond | 2.25 to 2.30 | 2.202, 2.251 | 2.309, 2.328 | 0.072 |

**The target was the 0.7 ms back at the trail, and no pose dearer.** With the class exit the trail
returns 0.73 ms of resolve (0.65 with the classify pass counted) and the mountain 0.57 (0.50);
oasis-pool looking away returns 0.6 of 1.8 either way. **The boggy pond is dearer by 0.09 ms**
(resolve +0.02, classify 0.07) and the sandy shore the same within 0.01: there the wet class is
37 to 40 % of the frame and the water another 17 to 21 %, so two thirds of the surface pixels are
drawn by the water module as before, and the frame pays to learn it.

**What the wet class over-claims.** Drawing the wet class by the plain module
(`renderer.resolve.class_wet_plain`, a measurement switch) leaves the pixels the water module wets
as the ones that differ from the picture:

| pose | wet class, pixels | wetted (differ under the plain module) | over-claimed |
|---|---:|---:|---:|
| oasis-pool, the pool | 2,495,967 | 46,493 (largest difference 62 codes) | 98 % of the class, 29.5 % of the frame |
| layered ground, sandy shore | 3,095,536 | 771,445 (87 codes) | 75 %, 28.0 % |
| — boggy pond | 3,337,229 | 654,284 (14 codes) | 80 %, 32.3 % |
| every pose with no water in view | 0 | 0 | none |

The class is a shore record's square, grown; the resolve goes on to the record's level range (a
body wets from its level up to its fringe) and a bank's own width. Testing the level in the
classify pass would take most of the over-claim at the pond poses and is the first thing to do to
the class (it has to be as conservative as the square is: the pixel's height from its depth word,
grown).

**The picture under `modules`**, the bounce off: 0 pixels differ at both of oasis-pool's poses and
at the layered ground's shore, trail and boggy pond; **at its mountain 2 pixels of 8,294,400
differ by one code**, the same two in the build with the class exit and the build without. Both
are ground with no shore record near: the wet class is empty at that pose, and drawing it by the
plain module changes nothing there. It is the two modules: the same source compiled twice by the
driver, whose compiler may contract or reorder a float expression differently beside different
code. `draws` cannot differ this way (one module) and does not. The test that holds `modules`
(`systems/water/tests/resolve_class_water_tests.cpp`, the shore test's basin at 480 × 320) allows
it a code on one pixel in twenty thousand and measured none at either of its poses.

## What it costs to build

From the runs' `load.pipelines` (R149), the driver's compile of each pipeline, cold:

| pipeline | SPIR-V | cold |
|---|---:|---:|
| the resolve, `visibility_resolve_rt_water_terrain` (the layered ground's) | 4,220,740 | 261 s |
| the same module again with the depth-equal state, in the next run | | 0.064 s |
| its plain twin, `visibility_resolve_rt_terrain` | 589,992 | 17.1 s |
| the resolve, `visibility_resolve_rt_water` (oasis-pool's) | 2,473,952 | 73.8 s |
| the same again, depth-equal | | 0.023 s |
| the classify pass, `resolve_classify_water` | 22,952 | 0.066 s |

A second pipeline of a module the driver has compiled costs nothing: the depth state is not part
of what it compiles. **Not measured: a cold start that asks for the one draw's pipeline and the
split's in one batch**, where the driver may compile the module twice side by side; a renderer
whose split is on should not build the one draw's pipeline at all, and today it does. The driver's
time grows faster than the module: 29 s a megabyte at 0.59 MB, 30 at 2.47, 62 at 4.22.

## What the frame budgets say

`tools/frame-budget.ps1` over a copy of the budget file with
`--tunable renderer.resolve.classes=modules` added to every run (the tool takes no tunable, and
the budget file is nobody's to edit), the committed tree's `msvc-release` binaries copied out,
`-WaitQuiet 60`, 2026-11-12 at 10:42 to 10:59 with other agents' flights between the runs. None of
the four scenes flown has water, so `modules` is `draws` there:

| run | verdict | resolve, median (budget) | classify, median | total, median / p99 (budget) | machine |
|---|---|---:|---:|---:|---|
| overlook-1080p | **not flown** | | | | its scene reads Khronos samples this worktree does not have, and a row may not download them |
| erg-walk-1080p-rt | pass | 0.203 (0.200) | 0.012 | 0.755 / 1.320 (0.724 / 1.214) | quiet |
| erg-walk-surround3-rt | busy, inconclusive | 2.405 (2.530) | 0.144 | 3.877 / 5.273 (3.771 / 4.127) | the GPU 99 % busy at its end |
| endless-1080p-csm | pass | 0.188 (0.491) | 0.016 | 1.147 / 1.962 (3.161 / 5.545) | busy |
| endless-surround3-rt | pass | 2.101 (2.100) | 0.142 | 4.330 / 5.810 (4.392 / 5.287) | busy |

The one pass over its limit is not the resolve's: `clas` at the p99 on the surround's erg, 0.709 ms
against a limit of 0.681, on a run the tool calls busy. **At 11520 × 2160 the classify pass is
0.14 ms** for the three views, and the per-class draws of ground and sky cost the one draw or
less (2.405 against a budget of 2.530 on the erg's surround; the endless desert's the same to a
thousandth). No budget names the classify pass's bucket, so its 0.14 ms shows only in `total`,
inside the tolerance. The csm run's budget is far above what either draw costs today and says
nothing of the split. No run was flown with the split off beside these: they are read against the
file's budgets, which were taken on a quiet machine.

## The second pass (the afternoon of 2026-11-12)

Everything above this heading is the first pass, as it was measured that morning; its statements
that the class exit is not in the tree and that the default waits for it are of that morning.
The second pass committed the exit, changed what it asks, gave the wet class the records' levels,
looked for the two pixels, and built what a default needs. The machine was busier than in the
morning (eight rows of two coordinators and the owner's forest session shared the GPU through
the lock's line, ten to thirteen waiting for most of the afternoon), so every difference below is
read **inside one process**: `renderer.resolve.classes=cycle`, a measurement choice added for
this, makes every pipeline and draws frame f by (f / 16) % 3 — off, draws, modules — so the three
modes are sixteen frames apart under the same load, and a figure is the median of a mode's frames
in a pose's hold less the first 48 of the hold and two at each block's start (70 to 82 frames a
mode and pose; the tenth percentile in brackets where it is given). A forest flight is four
minutes of load for seconds of frames, so the choice is also what makes a forest pair one load
instead of four.

### The class exit, committed

Commit 85082c20 is the first pass's patch as it was. From that tree's `msvc-release` binaries
copied out, as alternated runs (off, draws, modules; three times round on the grove, twice
elsewhere), resolve ms and the frame's sum of timed passes:

| scene and pose | one draw | `draws` | `modules` | classify | frame: one draw / `draws` / `modules` |
|---|---:|---:|---:|---:|---:|
| elder-grove | 2.755, 2.757, 2.756 | **2.243, 2.244, 2.243** | | 0.055 | 5.77 / **5.31** |
| — `--raster hw` | 2.739, 2.738 | 2.227, 2.226 | | 0.055 | 4.91 / 4.45 |
| oasis-pool, the pool | 2.194, 2.194 | 2.208, 2.207 | 1.821, 1.820 | 0.067 | 3.46 / 3.54 / 3.16 |
| — away | 1.811, 1.810 | 1.900, 1.899 | 1.211, 1.211 | 0.071 | 2.87 / 3.03 / 2.34 |
| — `--raster hw`, pool / away | 2.195 / 1.811 | 2.206 / 1.899 | 1.805 / 1.200 | 0.068 / 0.071 | |

The grove is the first pass's scratch build to a hundredth: **−0.51 ms of resolve and −0.46 ms of
frame under leaves**. The picture, the bounce off, at all seven poses: `draws` 0 pixels of
8,294,400 differ; `modules` 0 at six and the same 2 at the layered ground's mountain. The layered
ground's timings were taken on the next build, below.

### What the exit asks: integers, and why

As committed first the exit called `classify_pixel`, so every lane of every class's draw walked
the shore records again, in the resolve's module. Two things were wrong with that.

- **It cost what the classify pass costs, a second time, inside the dearest shader**: `draws` at
  oasis-pool looking away was 0.09 ms dearer in `resolve` than the one draw (1.900 against 1.811)
  before the classify pass's own 0.07.
- **Two shaders asked the same floats.** The classify pass's module decides a pixel is wet and
  writes the wet class's depth; the resolve's module, in the wet class's draw, asks again. If the
  driver rounds one comparison of `classify_wet` otherwise in the second module — and this page
  has two pixels that say two modules of one source do not always agree — the lane discards
  itself in the one draw whose depth test it passes, and the pixel is left the clear. None was
  seen in any capture; a wet class with a boundary thousands of pixels long is where one would be.

So the exit now asks **integers only** (`resolve_class_may_draw`): the pixel's class by its
material's flags, or "could be wet" by the resolve's own ground test, compared with the draw's
class. The classify pass's module is the one place the floats are asked, and the depth test holds
every draw to its answer. What it leaves running is a lane of plain ground in the wet class's
draw, and the reverse, where the two meet inside a quad. Measured on the final build
(`cycle`, two flights; resolve ms):

| scene and pose | one draw | `draws` | `modules` | classify | frame: one draw / `draws` / `modules` |
|---|---:|---:|---:|---:|---:|
| elder-grove | 2.756, 2.756 | **2.243, 2.242** | | 0.055 | 5.78 / **5.31** |
| — `--raster hw` | 2.741 | 2.222 | | 0.055 | 4.92 / 4.45 |
| oasis-pool, the pool | 2.156, 2.180 | 2.157, 2.170 | **1.544, 1.554** | 0.077 | 3.43 to 3.46 / 3.50 to 3.52 / **2.89 to 2.91** |
| — away | 1.773, 1.787 | 1.826, 1.839 | **1.199, 1.209** | 0.081 | 2.83 to 2.85 / 2.96 to 2.98 / **2.34 to 2.35** |
| — `--raster hw`, pool / away | 2.179 / 1.774 | 2.167 / 1.828 | 1.555 / 1.201 | 0.078 / 0.082 | 2.79 / 2.86 / 2.25; 2.33 / 2.46 / 1.84 |
| layered ground, sandy shore | (2.391, 2.392) | (2.389, 2.393) | **(2.170, 2.177)** | 0.078 | (3.68 / 3.75 / **3.53**) |
| — trail | (2.762, 2.893) | (2.639, 2.819) | **(2.196, 2.343)** | 0.096 | (4.14 to 4.26 / 4.07 to 4.28 / **3.66 to 3.81**) |
| — mountain | (2.537, 2.484) | (2.311, 2.476) | **(2.001, 2.152)** | 0.083 | (3.71 to 3.76 / 3.62 to 3.78 / **3.26 to 3.43**) |
| — boggy pond | (2.243, 2.259) | (2.285, 2.291) | **(2.028, 2.051)** | 0.085 | (3.49 to 3.50 / 3.63 to 3.64 / **3.37 to 3.39**) |
| — `--raster hw`, shore / trail / mountain / bog | (2.397 / 2.759 / 2.488 / 2.254) | (2.388 / 2.639 / 2.314 / 2.286) | (2.181 / 2.196 / 2.005 / 2.034) | 0.078 / 0.096 / 0.083 / 0.085 | |

The layered ground's figures are in brackets because they are the **tenth percentile** of each
mode's frames, not the median: its lease came while this row's own test suite was running on
the CPU and the medians of its three flights spread by up to 0.5 ms; the quiet frames agree
between flights to 0.02 ms at the shore and the bog and to 0.15 at the other two.

`draws` on a scene with no leaves is now within 0.05 ms of the one draw in `resolve` (it was
0.09 at the worst pose) and still costs the classify pass, 0.055 to 0.10 ms at 3840 × 2160, for
nothing it can return: that is what `auto` is for. **`modules` is now cheaper than the one draw
at every pose with the classify pass counted**, the two pond poses included (the shore 0.14 ms
of frame, the boggy pond 0.12), which is the wet class's doing, below. The one draw itself is 0.02 to 0.04 ms cheaper
than with the first exit (it keeps less alive). The classify pass is 0.01 ms dearer on the scenes
with water than in the morning (0.077 against 0.067 at the pool): the level's arithmetic, below.

### The wet class by the record's level

`classify_wet` now asks, of every record whose square holds the pixel, the heights that record
can wet: the resolve's own ranges (a still body's fringe above its level; where a swash's front
and a tide can reach; a bank node's level range; a stretch from two metres under its surface to
its fringe, within its half width and the fringe's run). The pixel's point is from its depth
word, so the range is grown. **The first slack tried was the squares' own** (half a metre and two
thousandths of the distance along the ray, times the ray's slope of height) and it returned
almost nothing on the layered ground:

| pose | wet class, squares alone | by level, the squares' slack | by level, the final slack | wetted |
|---|---:|---:|---:|---:|
| oasis-pool, the pool | 2,495,967 | 67,069 | **65,177** | 46,493 |
| layered ground, sandy shore | 3,095,536 | 3,037,400 | **1,167,603** | 771,445 |
| — boggy pond | 3,337,229 | 3,274,698 | **1,070,010** | 654,284 |

Over-claimed, of the class: 98 %, 75 % and 80 % by the squares alone; **29 %, 34 % and 39 %**
now, which is the slack and the fringe's last centimetres, where the saturation moves no code.

The oasis's pool lies in a basin metres deep, and its class fell from 30 % of the frame to under
1 %. The ponds lie in a plain: the body's fringe (`wet_m`, the schema's default) is **0.3 m**,
the camera stands 4 m above a bank that falls a few decimetres to the water over ten metres, and
half a metre of slack along a ray that looks down at 25° is 0.2 to 0.4 m of height, more than
the fringe itself. So the heights have a slack of their own, from what the two points can
actually differ by: both lie on the pixel's ray, apart by what moves the surface sideways (a step
of the receiver's 16-bit grid, since a traced primary's depth is on the float triangle and the
resolve's point on the grid's; a 256th of a pixel, since a rasterizer snaps its corners) over the
tangent of the angle the ray meets the surface at. **A hundred grid steps along the ray (a
centimetre at least) and two thousandths of the distance** holds down to half a degree; times the
ray's slope of height, with four grid steps, a two-thousandth of the distance and two centimetres
beside it for the point itself, it is 0.06 to 0.12 m at these poses. What it does not prove is a
surface seen within half a degree of edge-on down a steep ray, less than a pixel wide; the
squares keep their half metre.

The wetted counts are the morning's to the pixel under every slack (the pixels that differ when
no wet class is written, `renderer.resolve.class_wet_plain`, which since the exit works through
a flag `classify_pixel` reads; the plan's own wet bit could not survive the exit, since the plain
module has no wet class to keep). On the test's basin at 480 × 320 (a metre of fringe; final
slack): across it from 14 m up the class is 100,189 pixels by the squares and 30,234 by level
with 20,502 wetted (42,240 under the squares' slack); looking away over the plain 21,102 and
**none**; from the rim at a walker's height 16,651 with 13,894 wetted; from thirty metres 1,479
with 1,171. `modules` draws the one draw's bytes at all four.

### The two pixels

Two pixels of 8,294,400 at the layered ground's mountain differ by one code between `modules`
and the one draw: (2556, 1499), blue 12 against 13, and (2948, 1749), red 15 against 14, both
dark ground (codes 12 to 15) no shore record reaches, the same two in the morning's two builds
and in this pass's three. What they are not: **the material, the normal, the sun's shadow or the
sun's lobe.** The same pose and the scene's three others, drawn in the albedo, normals, shadow
and sun-specular views by the one draw and by `modules` (eight runs, the bounce off): 0 pixels
differ in any view at any pose, 8.3 million pixels each, where an albedo one unit in the last
place apart on every ground pixel would move some tens of codes. So the two modules' ground
blends, their reconstruction and their shadow rays agree to the bit, and the difference enters
after them: in the sum of the lit terms, the sky's ambient, the air between the eye and the
surface, the exposure or the output encode — a chain of multiplies and adds on the radiance,
which the driver may fuse or order differently when the surrounding code differs (the water
module calls the air's and the encode's functions from three more places, so what the driver
inlines where is not the plain module's). It is not `terrain_wet`'s dead `pores` sum, which was
the first guess: that is in the albedo, and the albedo is the same.

**Not closed.** Closing it means the tail of `fs_resolve` (and `sky_aerial`, `display_output`)
compiled so that a multiply and an add cannot be fused — `precise` there, or the modules built
with the compiler's precise mode — in files this row did not hold, with a cost nobody has
measured; and no tool on this machine disassembles SPIR-V, so the two modules' instruction
streams for that tail were not compared (installing one is not a row's to do). The stop words
that would bisect the tail are a measurement's and draw every class by one module. What stands:
`modules` is exact on six poses of seven and at every pose of its test, differs by one code on
two pixels at the seventh, the same two every run, and is therefore a lever a host may choose
and not what `auto` picks. The test's bound is unchanged (a code, one pixel in twenty thousand;
none measured at four poses).

### The forest

**Not flown when this was written.** A binary whose resolve shaders are new needs 66 GB of
commit free when its lease comes (the warm-up's rule, from the load that died past 59 GB), and
the three leases that came, at 12:46, 13:27 and 14:49, found 58, 58 and 48 GB and were given
back at once; a fourth was in the GPU's line behind sixteen others, an hour's load of the
owner's forest session among them, when the row handed back. Nothing
on this page says what the split returns on the forest, whose frame is leaves over a layered
ground beside water and whose resolve is 9.8 ms at the lake.

The driver is in the row's scratch and is one lease: a six-frame warm-up of the new resolve
shaders under `off` with the micromaps off (under `cycle` the forest's 5 MB module would be
compiled twice side by side, as the cold starts below show, and the warm-up already peaks at
53 GB), then one flight under `cycle` of the four cost poses the earlier pairs flew (the lake's
south shore, the cascade, two more by the lake) held 384 frames each at 15 fps, with a capture
in each mode at every pose for the census.

### A cold start, each way

oasis-pool, 1920 × 1080 offscreen, four frames, traced, the bounce, both caches cold (the
engine's pipeline cache off, the driver's own disk cache off and pointed at an empty directory;
[gfx](../subsystems/gfx.md)), one run each in one lease at 12:47 to 12:52, others' work beside
them (free commit 50 to 70 GB at their starts), from the runs' `load.pipelines`:

| made under | pipelines | the resolve's module (2,477,840 bytes) | the batch's wall | the process |
|---|---:|---|---:|---:|
| `off`: the one draw | 35 | compiled once, 86.4 s | 86.5 s | 93.6 s |
| `draws`: the split, **no one draw** | 36 | compiled once, 96.6 s | 96.7 s | 99.1 s |
| `cycle`: both | 38 | **compiled twice side by side, 107.9 s and 107.6 s**; the plain twin 10.4 s | 108.1 s | 111.9 s |

The morning's guess was right: two pipelines of one module in one batch are two compiles, 244 s
of CPU where one is 86 to 97 (the batch hides most of it in wall time on an idle core, and
none of it in memory, which is what the forest's cold load runs out of). So a renderer made
under `draws` or `modules` does not build the one draw's pipeline (`resolve_one_draw_built`),
`off` asked of it later draws as `draws`, and only `cycle`, a measurement's, builds both. The
10 s between the first two rows is one run each on a loaded machine, not the split's: the split
adds the classify pass's module, 0.07 s.

### The default

**`off`, until the forest pair is flown; then `auto`.** The rule the row was given is that the
split goes on if it is exact and no pose is dearer than the one draw, the classify pass counted,
on the engine's seven poses and the forest's two, and that the forest's flight decides. `draws`
by itself does not pass it: it is exact, and it is dearer at four of the seven (oasis-pool's
two, the sandy shore and the boggy pond), by its classify pass. `auto` passes it on the seven. The forest's two are not measured, so the tunable's default
is still `off`, and making it `auto` is one constant (`resolve_classes` in `settings.cpp`), to
be changed with the forest's table beside it and the GPU suites run under it. `modules` is not a
candidate while two pixels differ.

What `auto` is on the scenes measured, frame ms with the classify pass counted, against the one
draw: the elder grove `draws`, **−0.46**; oasis-pool and the layered ground, which hold no
alpha-tested material, the one draw, 0 (where `draws` would cost up to 0.13 and `modules`
return 0.12 to 0.55). No pose of the seven is dearer under `auto`, and one is cheaper. The rule
is the scene's and static — "holds alpha-tested materials" is known when the pipelines are made
— because what says how much a frame mixes is the census, a read-back. A scene with a few leaves
and little else pays the classify pass for less than it returns; a scene of grass blades with no
alpha test (deformed, opaque) mixes and is left to the one draw. Both are the rule's to refine
when a scene shows them.

### The frame budgets

`tools/frame-budget.ps1` over a copy of the budget file with
`--tunable renderer.resolve.classes=auto` in every run, the final build's `msvc-release`
binaries copied out, `-WaitQuiet 60`, from 13:02, each run waiting its turn in the GPU's line
(the overlook's Khronos samples copied into the worktree from the gate's, read only):

| run | verdict | total, median / p99 (budget) | machine |
|---|---|---:|---|
| overlook-1080p | pass | 0.159 / 0.203 (0.155 / 0.358) | busy: others at 91 % of the CPU at its start |
| erg-walk-1080p-rt | pass | 0.747 / 1.123 (0.724 / 1.214) | busy: others at 47 % |
| erg-walk-surround3-rt | busy, inconclusive | 3.703 / 4.965 (3.771 / 4.127) | busy: others at 73 % at its start, 58 % at its end; the median is under its budget and the p99 over, as in the morning's run |
| endless-1080p-csm, endless-surround3-rt | in the GPU's line when this was written (eighteen waiting) | | |

None of these scenes holds an alpha-tested material (the overlook's classify bucket is zero), so
under `auto` every one is the one draw, and what the runs hold is the one draw with the class
exit's test at its top: inside the budgets on the two the tool could judge, on a busy machine,
and no verdict on the third. The
morning's table, above, is the same scenes with every class drawn.

## What stands between `modules` and a default

1. **The two pixels**, above: the resolve's tail compiled so that two modules of it cannot differ.
2. **A forest pair after it**, since the forest is where a module without water returns most.

## The design that follows

As the first pass wrote it, in the order of what each returns at 3840 × 2160, with the
measurement behind it. **Since the second pass**: (a) and (b) are built. (b)'s estimate from the
shares, 0.2 to 0.3 ms at the pond poses, measured 0.21 to 0.22 ms of resolve at both. (c)'s
argument for the cold start has its measurement: two pipelines of one module in one batch are
two compiles (108 s twice over where one is 86 to 97), so per-class modules are what lets a
batch compile side by side, and a class's second pipeline of the *same* module is not. Three
rows the second pass adds, smallest first: **the classify pass's own cost on a scene with
water** (0.078 to 0.096 ms where the morning's was 0.064 to 0.079: the level's slack is computed
before any record's square is asked, and the pixel's facts carry the whole instance; both can
wait until a square holds the pixel); **the resolve's tail compiled so that two modules of it
cannot differ**, which is what stands between `modules` and `auto`; and **a class exit that
reads instead of deciding** — the lane's own post-depth coverage or helper state, where the
device reports it, would end a rejected lane with no decode at all, so that `draws` cost only
the classify pass where nothing mixes and could be on for every scene.

**(a) The class exit, then the default** (S). Returns 0.5 ms of 2.75 under the grove and makes
`modules` worth 0.57 to 0.73 ms on a layered ground with ponds out of view; unblocks the forest's
pair. The lines above, the sixteen variants rebuilt, the test's cases run again, a forest pair at
the lake and the cascade, and the default by what that pair says.

**(b) The wet class by the level, and a frame's classes by what the scene holds** (S). The pond
poses stop paying for wet ground that is not wet: of 37 to 40 % of the frame, three quarters. With
it `modules` should return at those poses what it returns at the trail in proportion to the pixels
it moves, of the order of 0.2 to 0.3 ms; an estimate from the shares, not a measurement.

**(c) Per-class modules: each class's draw by a module that holds only that class's code** (M to
L). This is the cold start's and the forest's memory answer more than the frame's. From R148's
build table and the modules' includes, what each would be:

| module | holds | SPIR-V, estimated | basis |
|---|---|---:|---|
| sky | the decode, the sky's table, the output encode | 0.05 to 0.1 MB | the classify pass with the wet walk is 0.023 MB; `sky.slang`'s reachable part is the rest |
| rigid, masked, deformed | the reconstruction, the material's maps, the lighting, the shadow query, the indirect read | 0.40 MB each traced (0.44 stochastic), or one module for the three | the plain traced module is 396,084 bytes and is these three classes and the sky |
| ground | the same with the layered ground's function | 0.59 MB (0.70 stochastic) | `visibility_resolve_rt_terrain`, 589,992 |
| water | the water's block with its rays, and the bottom under it | 2.1 MB, or 3.6 with a layered bottom | R148: the water's own block is 2.1 MB, the ground adds 1.75 MB to a traced water module |
| wet | the ground's module with the wet lookup | 0.65 to 0.75 MB | the ground's, and the lookup's records |

Against one module of 4.22 MB (5.08 for the forest's) that the driver compiles in 261 s, the
largest piece becomes the water's at 2.1 to 3.6 MB, about 65 to 190 s by the rates above, and the
rest compile beside it in seconds under R149's batch; the forest's 12.7 GB of `slangc` and its
cold load past 59 GB were one module's. In the frame it returns what `modules` returns for water,
for every class: the registers a leaf pixel carries for the ground's five layer bodies, and the
reverse. R148's water figure (0.74 ms at the trail) is the only measurement of such a weight; the
ground's function is a tenth of the water's size, so the other weights are likely tenths of a
millisecond together, not measured. The cost is in the source: `fs_resolve` becomes a function a
class instantiates, a variant per class and combination replaces the sixteen, and a new stage is
added to each class that needs it.

**(d) The compute route: pixels gathered per class and shaded at full group occupancy** (L). What
the raster route cannot return is the mixing inside a class's own draw and the quads' dead lanes:
with the class exit the grove's three surface classes still cost 2.17 ms drawn apart, where their
lower bounds in R148's cycle (each class's cost with the others taken out of the *same* draw) sum
to 1.64 ms, 1.72 with the floor. A compute pass over a list of one class's pixels fills every group of
thirty-two with that class, has no quads, and needs no depth image; it adds the three passes Unreal
5.4's binning has (count, reserve, scatter: of the order of the classify pass each at these
sizes, so 0.15 to 0.25 ms) and writes the colour through a storage image, which the frame passes'
linear image already is. So the most it could return under the grove beyond (a) is about 0.45 ms
less 0.2: **of the order of 0.25 ms at 3840 × 2160 on that scene, where (a) returns 0.5**. Leaves
vary within their class by what the sun's ray meets, so even a full group of leaf pixels mixes; a
bin by "asks the sun" would be the next split, and is the reason to measure before building. It is
the right route for the forest only if a forest pair under (a) shows the leaves' own draw still
dominating.

**(e) What a new material kind costs to add, under each.** Today: a flag, a branch in
`fs_resolve`, and every pixel of every scene that has one pays its registers (the water's 0.7 ms).
Under the split as built: a class id, a line in `classify_pixel`, a bit in the plan; the kind's
pixels are drawn apart, and with a module of its own (the `modules` pattern) nobody else pays for
it. Under (c): a module, a pipeline and a class, and its compile is its own. Under (d): the same
and a bin. The classes are a table of seven with room for fifteen in the depth's sixteenths.

## What was not done

- The forest was not flown, by the brief.
- The layered ground's `--raster hw` column was flown only with the class exit.
- The overlook's budget run (the worktree has no Khronos samples), and a quiet flight of the
  surround's erg.
- No pose with a Panini view or an open sea was measured: there the plan draws every class by the
  scene's module and `modules` is `draws`.
