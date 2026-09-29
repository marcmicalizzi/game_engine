# Walking the erg in time-lapse (2026-09-29)

**Question.** The owner walked the erg (`content/test-scenes/desert-erg`, his `fly-erg.json` is the same scene) on 2026-09-29 with `walk-erg.ps1` — `engine-view --interactive --walk --width 11520 --height 2160 --views surround3 --start 140,60,-60 0,0 --shadows csm`, recording his input and `--benchmark` frames — and changed the dunes' rate with `,` and `.`. He reported three defects: (1) at 600 game seconds a real second the ground round his start changed smoothly for about a second and then jumped in height, over and over, for about the first 20,000 ticks, and then stopped or slowed sharply; (2) standing where the ground rose under a fast time-lapse, he fell through the bottom as the dune passed over him; (3) a session started with `--walk` ignored W A S D until he pressed F to fly, flew, and pressed F again to walk. What caused each, what fixes it, and what do the numbers say before and after?

**Answer.** (1) Not the drawing. The drawn sand moved smoothly; the camera jumped, because the camera is the walker's eye, and the walker stood on a collision ground that followed the drawn sand in 5 cm steps. What changed at 20,000 ticks was the wind: the record's next two game days are calm, the renderer's next pair spanned 2.2 of them, and the sand slowed about thirty-fold. Fixed in `scene_collision`: the ground under the walker now follows the drawn sand to a millimetre and carries a standing walker with it. (2) The same collision closed over a standing walker: a heightfield's contact is one-sided, and the character's slope hold zeroes the backend's penetration recovery, so rising sand buried him at slow rates and dropped him through at fast ones. Fixed by the rule **a walker is never below the collision ground after a refresh**: lifted onto it before the step, left standing with no vertical speed. (3) Not reproduced: in every recording of the day the walker moved at walking speed from the first tick W was held, and his start replayed with W held from the first tick walks; two things his runs did that look like it are described below, one of them fixed by (2).

- **Date:** 2026-09-29. **Machine:** the owner's desktop (i9-10980XE, 18 cores, Windows 11); **GPU:** RTX 5090. **Build:** `msvc-release` of `3ccc59b` (before) and of this change (after) for the end-to-end runs, `msvc-debug` for the CPU models.
- **Machine state:** the end-to-end runs under the GPU lock, other processes at 27.4% (before) and 10.1% (after) of the CPU and the GPU 7% busy, WARNING raised — they measure motion, not time, so the load moves nothing quoted; the bench below `--wait-quiet=600` with others at 12.1% and a repeat at 19.3%, both WARNING raised, upper bounds. The CPU models are a function of their inputs.
- **Decision:** the rule in [scene_collision](../subsystems/scene_collision.md#the-walker-goes-with-the-ground) ("The ground moves", "The walker goes with the ground"); no ADR, since nothing the time-lapse or the walk promises changed — the walk's promise is now kept on moving sand.

## Setup

- **His records.** Five sessions in `D:\workspace\game_engine_local\flythrough\erg-walk-2026-09-29T*` (two at `d7f7185` in the morning, three at `3ccc59b` in the afternoon): each an input log and a `--benchmark` frame log whose records carry the pose the frame drew and a `terrain` object (the rate, the surface's time, its pair's times, the blend, the largest vertex move that frame). Read with a script outside the repository: a jump is a frame whose largest move is many times its neighbours'.
- **The renderer's CPU model of his run** (`systems/renderer/tests/terrain_clock_tests.cpp`, "the owner's walk at 600 on the erg"): his window's grid level with the surface clock, the keep-up rule and one worker at 59 frames an evaluation (his latency of 708 game seconds at 600 is 1.2 times that), his rate changes at his frames, and the committed erg's own fields over a 192 m window round his start, blended as the pool pass blends them.
- **The collision's CPU model** (`systems/scene_collision/tests/time_lapse_tests.cpp`): the committed erg's dunes through the registry, the renderer's cadence and `terrain_blend_frame`, a `physics::CharacterBody` standing still, four 240 Hz ticks a frame, the collision refreshed round it before each step as the walker's host does. The rising points were found by a scan of the erg every 25 m over a kilometre either side of the origin, from the committed time, for the largest rise over ten seconds at each rung: 600 at (−250, 975) run a minute, 3,600 at (−500, 900), 8,640 at (−750, −575), 86,400 at (−500, −825) and 604,800 at (−400, −1000), with the pairs timed as a window times them (the keep-up rule at a 0.8 s turnaround). And `scene_collision_tests.cpp`'s ground rising 0.375 m a frame, the renderer's own bound.
- **The walker the window runs** (`apps/engine_view/tests/walk_tests.cpp`): `view::Walker` on the committed erg at his start, handed the ground the renderer's model draws a frame at a time.
- **End to end**: `engine-view` release on the committed erg whole, his flags but the size — `--interactive --walk --start 140,60,-60 0,0 --views surround3 --shadows csm --time-rate 600 --width 480 --height 96 --no-vsync` (under the 120 Hz present ceiling) — standing still for 4,800 injected ticks, with `--benchmark`; the before binary is `msvc-release` of `3ccc59b`. And the same start holding W from tick 1.

## Results

### 1. The ground's jump at 600

**His records** (`T1556`, 15:56). Rate 600 from frame 1,495; he stood at (140, −60). From there to frame 6,814 his eye dropped **4.65 cm in one frame 45 times**, 0.61 to 3.59 s apart (0.6–0.8 s during the windy pairs at 53–69 s), and stood exactly still between. The drawn sand's largest move a frame over those frames was 0.20 to 1.29 mm, and over all 14,934 frames at 600 no frame's move was more than **1.81** times the mean of the ten round it (the largest ratio, at the handover to the calm pair, a drop and not a rise). `T1550` shows the same beat at 600, 0.68 to 4.8 s. The suspects the brief named, in order: the one-second floor on the interval between fields (`min_step_s`) — at 600 his pairs were 24,708, 17,707, 5,322, 4,112, 4,668 and 12,290 game seconds long, the displacement cadence's, and the floor decided none; a level installing a field that was not its blend's b — every handover exact in the model; the live rate change leaving a field timed at the old rate — he changed it once, at frame 1,495, and the steps kept their beat for a minute after; the sand detail's wind — a shading input that turns over two game hours, twelve real seconds at 600; the walker's collision ground being refreshed — **this one**, because it moves the camera: the walker stands on the collision ground, the collision ground stood still while the drawn sand sank until it was 5 cm off (`walk.collision.ground_error_m`), and then its tile was rebuilt and the walker snapped down onto it in a frame.

**What changed at 20,000 ticks.** At frame 6,814 (84.3 s, 20,230 ticks) his grid took a pair spanning **193,353 game seconds**: the wind record's days 1,096 and 1,097 are calm — the waves, the fastest band, travel 2.52 m on day 1,095, **0 m on each of the next two**, and 3.14 m on day 1,098 — so the cadence put the next field 2.2 days out, the sand's largest move fell from 0.42 to 0.015 mm a frame, and he stood 66 s more without a step. The renderer's model of his run reproduces it: 7 pairs (the shortest 4,112 game seconds, none timed late), the calm pair taken at frame 6,798 (his 6,814), the largest move 1.133 mm a frame before it and 0.0153 mm after, no jump, every handover exact.

**Before and after** (the collision's model standing at his start for 1,200 frames; the walker the window runs; end to end):

| | before | after |
|---|---|---|
| Model: the feet's largest move a frame | 46.5 mm | 1.03 mm |
| Model: jumps (over 1 cm and 4× the sand's move) | 3 | 0 |
| Model: the feet over the drawn sand, at most | 46.6 mm | 1.0 mm |
| Model: tile rebuilds | 21 | 189 |
| Walker: the eye's largest move a frame | 46.5 mm | 1.03 mm |
| Walker: the eye's move against the sand's, at most | 46.4 mm | 0.89 mm |
| End to end: the eye's largest step a frame (2,368 / 2,361 frames) | 46.5 mm, 7 steps | 1.08 mm, none over 5 mm |
| End to end: the drawn sand's largest move a frame | 0.215 mm | 0.215 mm |
| End to end: collision rebuilds; stalest under the walker | 46; 50.1 mm | 366; 1.19 mm |

End to end before, the steps came every 324 frames, 2.7 s at the ceiling's 120 Hz: a frame is a sixtieth of a second of the time-lapse's clock ([apps](../subsystems/apps.md#--time-rate-the-dunes-in-time-lapse)), so the window ran at twice the rate in real time, as his did at 80 frames a second at 1.33 times.

### 2. Falling through rising sand

**His records** (`T1606`, rate 604,800 from 5.3 s). At 26.9 s, standing at (−17.3, −315.5), the walker fell 9.1 m in 1.3 s and stopped; a second later he flew up and dropped back, and the drop found the sand 20.8 m above where the feet had stood.

**The model** (standing still where the erg's sand rises, the pairs timed as a window times them):

| Rate | the sand under the feet rose | before | after |
|---|---|---|---|
| 600 (60 s) | 0.16 m | buried 15.4 cm, the feet never moved | never under the collision ground; 198 carries |
| 3,600 | 0.20 m | buried 18.2 cm | never under; 165 carries |
| 8,640 | 0.57 m | fell through, 0.86 m under | never under; 386 carries |
| 86,400 | 1.56 m | fell through, 95 m under | never under; 600 carries |
| 604,800 | 4.80 m | fell through, 132 m under | never under; 462 carries |

After, the feet are never more than 0.8 mm under the drawn ground at a frame's end and never jump. At the renderer's own bound — a plane rising 0.375 m a frame for three game seconds and falling as fast for three — before, the walker fell through on the first rising frame and ended 28.1 m under; after, it rose 22.5 m with the ground and came down again, never under the collision ground, within 0.87 mm of the drawn plane, each frame's move within 0.5 µm of the ground's, and a jump from it at 22.5 m/s of rising sand was caught and lifted 0.31 m.

**The rule.** A walker is never below the collision ground after a refresh: before the step, feet the ground has risen past are lifted onto it, at the collision's height there — the ground provider's drawn pair blended on the collision's lattice — and a walker standing on the ground is carried by the change of the ground under its feet, keeping the offset it stood at. Both are `physics::CharacterBody::teleport`: it stands where it is put with no velocity, so a fall or a jump the sand caught ends as a landing does (its vertical speed dropped), and its horizontal speed is the input's, which every step sets anew.

**Cost** (`scene_collision.follow`, `msvc-release`): a frame of moving sand under a standing walker — the blend on by 2 mm, the walker's tile rebuilt, the walker carried, the other tiles rebuilt as the 5 cm rule comes due — **121 µs median, 119 best**; a repeat 130 and 120. Upper bounds (machine state above).

### 3. W A S D at a `--walk` start

**Not reproduced.** In his five recordings the walker moved at walking speed from the first tick W was held — `T0834` from tick 708 (1.3 m in the first second), `T1550` from 599, `T1606` from 514 — and the one that pressed F first (`T0835`, at tick 315) had pressed no W before it. His start with every flag but the size (`--walk --start 140,60,-60 0,0 --views surround3 --shadows csm`), W held from tick 1, walked **2.99 m in two seconds** on the committed erg whole (`msvc-release` of `3ccc59b`) and 1.50 m by the end of the first second on the test's 512 m window of it, which `walk_view_tests.cpp` now holds. The erg's size and its first collision ring (filled unbudgeted by the drop, before the first tick), the 60 m start (the drop's ray finds the floor 44 m down) and the three views made no difference.

**What his runs did that can look like it.**

- **A walker under the sand.** At a fast rate a standing walker fell through rising sand (2, above) and walked, with W, in the dark under the dune, where nothing he could see moved; F flew him out and F dropped him on top, where W worked — his `T1606` does that dance at a week a second. Fixed by (2).
- **The first captured pointer motion.** A live window takes the pointer when it gains focus, and the first motion it then reports can carry the cursor's whole distance to the window's centre: at tick 1 of `T1556` the log records (−2,746, −541) pixels, which turned the camera 346° and pitched it 68° up — a view of nothing but sky, in which W moves the walker and nothing on screen changes — and `T0835` recorded (−2,931, 97), a turn of 369° and a pitch 12° down; the other three recorded (6, −11), (−41, 32) and nothing at tick 1. Not changed here: an injected session never takes the real pointer, and reproducing it means moving the owner's own cursor on his desktop. The fix is small (drop the motion reported before relative mode is on, or the first poll after it), and it wants a session at his desk to confirm.

## What surprised me

- All five of the renderer's suspects were innocent; the camera is a physics body, and the collision's 5 cm, chosen as an error the walker could tolerate, was a step the eye could see.
- The character's slope hold — zeroing a standing character's velocity on a walkable contact, so it does not creep down a dune — also zeroes the backend's penetration recovery, so a rising heightfield buries a standing character even when the rise is millimetres a frame.
- The wind decides what the time-lapse looks like more than the rate does: two calm days turned "a jump every second" into "nothing for a minute" at the same 600.

## What it decides

That the ground under a walker follows the drawn sand to a millimetre, and that the walker goes with it: carried standing, lifted when overtaken, in `scene_collision::SceneCollision::follow`, which engine-view's walker calls before every step. It does not decide the pointer's first motion (above), and it does not make a walk over moving sand replay bit for bit — the sand still follows a live window's clock.

## Caveats

- One machine and one scene; the rising points are where a scan found the most rise, not where the sand rises fastest.
- The models time the fields as a window does or wait for them as offscreen does; a live window's own clock, with its latency, was measured end to end at 600 only.
- The cost is a plain-arithmetic ground; the dunes' field evaluations on a pair change are the terrain capability's and come on top, a pair at a time.
