# ADR-0063: Memory is held under one budget and by travel, and fast travel has a covered form and a shown one

- **Status:** Proposed
- **Date:** 2026-11-14
- **Plan references:** docs/plan/04-renderer.md §4.9 (streaming and residency: one budget policy with per-type floors, tile predictions from observer velocity), docs/plan/03-data-model.md §3.7 (the spatial partition), docs/plan/05-simulation.md §5.4 (tiers over the observer set)
- **Docs touched:** `docs/experiments/streaming-by-travel.md` (the design, its sources and its derivations)

## Context

On 2026-11-13 the owner walked, ran and flew the forest proof of concept at 3840 × 2160 with traced primaries, traced shadows and the bounce: 75.9 GB of private commit before the first frame and 79 GB later, a working set of 22 GB, a frame-time jump every couple of seconds while running, and jumps in flight "as objects switch LOD en masse". They asked for memory managed in the background by distance and speed of travel so that walking, running, riding and driving never outrun it, a technique of its own for flight, and fast travel "with support for a loading screen or support for an immersive fast travel that doesn't involve hiding the move".

What the measurements say (row T129's accounting and row T131's tile work, both of the same night; the design page cites each figure):

- On Windows the driver backs every device-local byte with commit, so the process's commit is its host bytes plus its device bytes.
- Of the forest staging's 42 GB, about 10 GB is geometry and its companions read at load and held whole wherever the eye is (one set of device buffers, one merged mesh on the host, the micromaps, the materials), about 5 GB already follows the eye, and 7.7 GB has no owner.
- The engine has seven residencies, each under a bound of its own (pages, the mesh room, its textures, the ray tracing chain, the structure cache, the flora tail, the pair cap) and no sum.
- The tile ring scores tiles from each observer's position and reads no velocity. For the staging's rings a metre of straight travel asks for 1.58 tile builds; at the tile costs on record one build worker holds between 10.5 and 63 m/s. Three recorded bursts (577, 984 and 955 tiles due at once) each cost seconds of long frames.
- 04 §4.9 already asks for "one budget policy with per-type floors" and "tile predictions based on observer velocity". Neither is built.

The alternatives on the table, for each of the three things to decide:

**The budget's form.** (a) One number for the process, host plus device, with the engine splitting it; (b) two numbers, one for commit and one for the device; (c) the per-pool tunables as they are, with better defaults. And for the default when nothing is named: half the physical memory; the physical memory less a reserve for the rest of the machine; a share of the commit limit.

**Being outrun.** (a) The engine detects it and draws far forms until the speed falls; (b) the game declares a travel mode and the engine obeys; (c) the engine caps the observer's speed at what it can load; (d) the frame waits for the load, as Unreal's "Block on Slow Streaming" does.

**Fast travel.** (a) One covered form only, a loading screen; (b) one shown form only, sized to always fit; (c) both, with the covered one the fallback; and, for a shown move whose destination is not ready when the traversal ends: hold the traversal, cover the remainder, or arrive into far forms.

## Decision

1. **One budget, the process's.** `engine.memory.budget_mib` is the most the process may hold, host bytes plus device bytes (alternative a). The device's part is capped again by what the device reports. With nothing named it is **half the machine's physical memory**. A place has a **floor** (the descriptors every cull reads, the far tier, the frame's targets, the coarsest texture levels the far tier samples, collision under each observer, the simulated rings' state), and a budget under the floor is refused at load with the floor's bytes by kind.
2. **One governor over pools.** Every residency reports the bytes it holds on the host and on the device, the bytes it could release now without changing a pixel, its floor, and a `trim`. A governor off the frame's thread compares their sum, and the process's commit as the system reports it, against the budget; the difference between the two is counted as unaccounted, against the budget. It gives each pool a ceiling and, as the sum nears the budget, takes these steps in order: release what nothing names; stop holding behind; coarsen from the far side; withhold, nearest first. It never fails an allocation in a frame, opens a hole, goes under the floor, or trades one drawn tile for another inside the hold-off. A new residency is a pool or it is not merged.
3. **The envelope is two times, measured.** What is requested is what an observer will be near within **τ**, the measured time from wanted to drawn for each kind of thing (a tile, a mesh, a texture, a page): each observer is scored from the segment between where it is and where it will be τ from now at its velocity, in place of its point. What is behind is held for `t_back` seconds of travel while the budget has room. No distance is tuned per speed, and a world's rings stay the scene's own.
4. **Being outrun is detected, and changes what is drawn** (alternative a, with b as a hint a game may give). When a backlog's time to drain passes the time its tiles will stay in the ring that asked for them, the engine is in flight: it draws the far tier everywhere, requests a ring only while a tile will stay in it longer than its τ and a time worth showing, takes a tile's drawn form from the eye's distance in three dimensions, and defers the near field until the speed falls. No frame waits for a load, and no speed is capped.
5. **A frame takes only what fits a time allowance.** However many tiles are due, a frame's hand-over on its own thread is bounded in time, nearest first; a tile that changed form holds it for the hold-off; the layout keeps its room across a flight. A thousand tiles due at once are drawn over many frames.
6. **Fast travel is one interface with two forms** (alternative c): `travel_begin` (a destination, `cover`, how much must be settled, and for a shown move its path or duration and `on_late`), `travel_status` (a phase, a progress that never goes back, the seconds left), `travel_arrive`, `travel_cancel`; in the game module's interface table and in the protocol. `travel_begin` returns an estimate from the session's measured rates: the seconds, the peak, the least duration of a shown move, and whether the budget holds it.
   - **Covered** (`cover` true): release to the floor all the destination does not also name, load with the whole machine, arrive settled. The peak is the floor plus the larger place, never the sum. A session's first load is this call from nowhere.
   - **Shown** (`cover` false): a preload observer at the destination builds and admits without drawing; the traversal plays in the world under decision 4; the destination's tiles enter the scene at the frame's allowance as the eye approaches. If the budget does not hold both places, the origin's near field is released at departure; if it does not hold the destination beside the floor, the call is **refused with the bytes** and the covered form is the answer.
   - **Late is never hidden.** A shown move whose destination is not ready at the traversal's end holds in flight's forms by default (`on_late: hold`) and says how late; the game may ask for its cover instead. The engine never stalls a frame to finish a load.
7. **Nothing here reads a generator.** The budget is the process's; the envelope reads a scene's `world` block, observers and measured times; a scene with no world block has the budget and no envelope.

## Consequences

- The seven bounds become ceilings the governor hands out. Their tunables stay as overrides for a measurement that needs to name one; with nothing named, a pool's size is the governor's.
- A consumer sets one number and is told at load, by kind, when it is too small. A default of half the physical memory is 32 GB on the development machine, where the staging holds 42 GB today: the default is not met until the restructure's first steps are built, and until then the governor will be taking its steps on the forest. That is the point of it, and it is said here so that nobody reads the first governed walk's coarser far rings as a regression.
- The static scene has to be restructured before the budget can be honest about a place: the host's copy of the geometry released after upload, the load's meshes resident by reference (ADR-0061's first "revisit when"), geometry by page in a streamed scene, micromaps and textures arriving with their mesh. The order, and the most each can return, is on the design page; none of it is decided here beyond "in that order".
- A placement generator's `tile` must be callable from several threads at once before the envelope can promise driving speeds at the worst tile cost on record. That is a contract change in `scene_gen` over eight generators.
- In flight the near field is absent by design. A game that wants detail at speed (a low, fast pass over a village) gets it only as far as the builders supply it, nearest first.
- A shown fast travel has a least duration the engine names, and a game must be written to stretch its traversal to it or to accept a hold.
- Forbidden from here: a residency with a bound of its own that the governor does not see; a load that a frame waits for outside a covered travel; a distance ahead or behind tuned for one world's speeds; a form change of every due tile in one frame.

## Revisit when

- Stage a of the build has measured a place's near field: if two near fields fit under the default budget on the smallest machine the engine supports, decision 6's release at departure can become the exception rather than the rule.
- A read of the conference talks this row could not read (Sucker Punch's and Insomniac's on streaming, Guerrilla's on Decima) gives a rule for lead by speed that differs from decision 3's.
- The engine runs on a machine whose device memory is the host's (a unified-memory adapter): the budget's one number is then one pool, and decision 1's second cap goes.
- A world ten times the staging's size is loaded: the far tier and the plans a load expands are then themselves by region, and the floor is no longer a constant of the world.
