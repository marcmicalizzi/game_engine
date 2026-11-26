# water_live (systems)

**Purpose.** The water's shallow-water step on the card (roadmap W175, under W173): the scheme of [water_state](water_state.md) — the CPU reference in double precision — written as a compute shader in single precision, the smallest host code that runs it on a device, device cases that hold it to the reference's own cases, and a bench that says what it costs a frame. It is the capability that will later run the water's state in a scene. Today it holds no scene, draws nothing, adds no pass to a frame and has no scheduler entry: those are a later row's, and the measurements this row exists for are on [water-step-on-the-card](../experiments/water-step-on-the-card.md).

**Why this shape.** The design of the simulated water (W173 and its decision record) leans on a number nobody had: what a well-balanced finite-volume step costs a frame at the grids a lake, a stream's reach and a beach need. A step that is not the reference's scheme would measure something else, so the shader is the reference line for line — the same expressions in the same operand order, `precise` on every one — and is held to the reference's cases rather than to cases of its own. It is a capability of its own in `systems/` (ADR-0027) because the state's later home is a scene, which the renderer and the world own, and nothing in `domain/water_state` may depend on a device.

- **Each cell gathers.** A thread is a cell: it reads its own state and its four neighbours', computes the flux through each of its four faces, and writes its new state into the second copy. No scatter, no atomic, one fixed order, so the same device gives the same bytes twice. A face is computed twice, once by each of its cells, from the same arguments in the same order; a second form computes every face once into a buffer and the update reads it (`StepOptions::stored_faces`), and a case holds the two to the same bytes, which is the proof that a face's flux is one value on both sides ("Two forms of the faces", below).
- **The bed is a height from the grid's own datum.** `set_bed` takes world heights and a datum in double and gives the card `bed - datum`, rounded once. A grid's positions are its own (cell (x, y) at index y·nx + x; [ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md) puts no world position on the card), and a lake at rest at 2,000 m is the same bytes as at zero. In world heights it is not: single precision's ulp at 2,000 m is 2⁻¹³ m, and the surface's rounding becomes a force (the inexact lake's case measures it). The bed is a buffer the step reads at every step and may find changed.
- **The step's length is chosen on the card.** Each update writes, per workgroup of 16 × 16 cells, the largest |u| + c and |v| + c of the state it wrote and the volumes its cells added to the ledger; a one-workgroup pass reduces those records in a fixed tree, takes dt = cfl·dx / (max(|u| + c) + max(|v| + c)) as the reference does, and folds the volumes into the ledger. So a run of steps needs nothing from the host, and the frame does not wait for a readback to know its step.
- **Single precision throughout.** Every field that changes is a 32-bit float; nothing is 16 or 8 bits.

**The state.** Two copies, each `k_state_planes` (4) planes of nx·ny floats: h (m), hu and hv (m²/s), and the source's remainder (m, kept only when `set_source(..., compensate)` asks). The planes the design adds come after these, in the same layout: the ground's store, the carried quantities (one plane each, up to the reference's `k_max_carried`), the bed's mobile layer; the bed, Manning's n and the sources are buffers of their own that the host may rewrite between steps. The workgroups' records (`WaterPartial`, 32 bytes) and the ledger (`WaterLedger`, 48 bytes: each sum a pair hi + lo kept by TwoSum, so 10⁵ steps add in about twice single precision) are mirrored structs, pinned with the push constants (`StepParams`, 120 bytes) in `tests/size_table.cpp`.

**The step.** `shaders/water_step.slang`, five entries in one module: `prime_main` (the records of a state the host just wrote), `dt_main` (the step's length and the ledger), `update_main` (the step), and `faces_x_main`/`faces_y_main` (the faces, stored). The scheme is the reference's at its first milestone: first order; the hydrostatic reconstruction (Audusse, Bouchut, Bristeau and Sainte-Marie 2016, (3.1)–(3.5)) over the central-upwind flux (Bollermann, Chen, Kurganov and Noelle 2014, (2.8), (2.13)–(2.14)); the face's bed max(z_L, z_R); identical states given their exact flux, which keeps a lake at rest to the bit; a velocity of zero below the dry depth; Manning's friction implicit, (hu)' = (hu)* / (1 + dt g n² |u*| / h^(4/3)); sources and sinks; closed edges as mirrored ghosts and open ones as copied ghosts; a depth rounded below zero clamped and counted. A step of zero length (an advance that has landed) copies the state unchanged.

**The arithmetic.** Every arithmetic line is `precise`, which Slang emits as SPIR-V's NoContraction: no driver may fuse a multiply and an add, so addition, subtraction and multiplication are correctly rounded on every conformant device. Division and the square root are not: Vulkan allows them 2.5 ulp and more, so two devices may differ in a last bit. With `Ops::fixed` (specialization constant 0) they and the friction's cube root are built from the first three alone: an estimate from the operand's bits, which integer arithmetic makes exactly, and a fixed number of Newton steps (four for 1/x, three for 1/√x, four for ∛x). What each form costs and gives is on the experiment page.

**An exchange under the last bit.** A slow source or sink — a loam's take, 3.7e-6 m/s, is 6.2e-8 m in a 60 Hz step — is about the last bit of a depth near a metre (2⁻²⁴ m below 1 m), so a plain step rounds each step's take to a whole ulp and the pond's loss comes out wrong by a few percent. `set_source(values, every, compensate)` offers the two remedies the design weighed: applying the source on a coarser clock (on one step in `every`, `every` times the amount), and keeping each cell's remainder (plane 3): the target is what the step owes plus what earlier steps' rounding left out, what reaches the depth is h' − h (exact by Sterbenz's lemma while the step is small against the depth), and the rest waits for the next step. The case and the numbers are below and on the experiment page.

**Two forms of the faces.** `StepOptions::stored_faces = false` (the default): one pass, each cell computing its four faces. `true`: a faces pass computes each of the (nx + 1)·ny x faces and nx·(ny + 1) y faces once into a buffer of four floats a face, and the update reads them. The two are the same bytes on this card (a case), so the gather's two computations of a face agree to the bit; the default is the gather because it moves fewer bytes (the experiment page has both costs).

**Running it.** `DeviceStep::create(device, water_state::Config, StepOptions)` makes the buffers and pipelines; `set_bed`, `set_manning`, `set_source` and `write_state` upload; `run_steps(count, max_dt)` and `advance(duration)` submit and wait; `read_state`, `read_ledger` and `state_hash` read back. `record(graph, count, max_dt, timer)` puts steps into a render graph the caller owns — every pass declares what it reads and writes and the graph derives the barriers — and with a `gfx::GpuTimer` each pass is a zone (`water.dt`, `water.faces`, `water.update`). An advance keeps its time left on the card in float: the last step lands on it, and the steps after it in the same batch choose a length of zero.

**Owned data.** Nothing outside a `DeviceStep`: the state, its inputs and its ledger on the device. The reference (`water_state::Solver`) stays the source of truth for the scheme; the card's form follows it.

**Invariants.** Checked by `tests/water_live_device_tests.cpp` on the device and `tests/water_live_tests.cpp` without one. Each bound comes from one argument: single precision's unit roundoff is 2²⁹ times double's, and a first-order monotone step under its CFL limit does not amplify a perturbation, so the card may differ from the reference by about one ulp of the water involved a step, S × 2⁻²³ after S steps; a reference bound that is a count of double's ulps is scaled by 2²⁹.
- A lake at rest over a bed of binary fractions with dry islands stays exactly at rest for 20 s (no bit of depth or momentum moves, no round-off is clamped), at a datum of zero and at 2,000 m with the datum there, the two the same bytes; in both forms of the arithmetic.
- A lake at rest over an inexact bed stays within 5.4e-5 m of its surface and under 5.4e-3 m/s (the reference's 1e-13 m and 1e-11 m/s times 2²⁹); at 2,000 m with the datum there it is the same bytes as at zero.
- Stoker's and Ritter's dam breaks at 200, 400 and 800 cells: L1 orders at least 0.5 and the finest grid within the reference's own bound (0.0045, 0.0058); the card within S × 2⁻²³ of the reference in L1 and its volume within S × 2⁻²⁴.
- The reference's count of water (wetting, drying, a spring, a drain, an open edge, friction): the card's own balance within S × 2⁻²⁴ of the water involved, its volume and outflow within S × 2⁻²³ of the reference's.
- The same bytes twice on one device, and the gather and the stored faces the same bytes.
- A closed basin keeps its water to the update's own rounding: the volume moves by no more than S × cells × 2⁻²⁴ × h_max × area.
- A still pond's slow sink kept by the compensated sum loses what it owes to two ulps of the depth.
- The SPIR-V carries NoContraction and no GLSL.std.450 Fma; the mirrored structs keep the shader's offsets and sizes.

**Public API.** `include/systems/water_live/water_live.h`.

**Depends on.** `base`, `containers`, `math`, `gfx`, `water_state`.

**Testing.** `tools/dev.ps1 test -Filter water_live` (`water_live` without a device, `water_live.gpu` with one). Benchmarks: `build/msvc-release/systems/water_live/engine_water_live_bench.exe --filter=water_live.* --gpu-lock --wait-quiet=<s>`; each variant prints its GPU step times on a line of its own beside the harness's table.

**Performance notes.** The step is memory-bound by design: a cell reads its three planes, its bed and its Manning's n and writes three planes, 32 bytes, its neighbours' reads served from cache. The measured costs, the bytes against the card's bandwidth and what a 60 Hz frame keeps are on [water-step-on-the-card](../experiments/water-step-on-the-card.md).

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(water_live water_state)` | declared; off when water_state is off |
| Component and event types | none | not yet: a saved state is W173's design |
| Tick scheduler entry | none | not yet: a caller records steps into a graph it owns; which phase owns the water is W173's design |
| Render-graph passes | `DeviceStep::record` adds the step's passes to a graph the caller submits | none in a frame yet |
| Content-build derived step | none | nothing is precomputed |
| Protocol methods | none | not needed |
| Tunables | none | run-time parameters arrive in `water_state::Config` and `StepOptions` |
| LOD policy | none | one grid at one resolution; the water's LOD is W173's design |
| Determinism | `water_live::k_determinism` = `hashed` | the same bytes twice on one device (a case); across devices only with `Ops::fixed`, whose open questions the experiment page lists |
| Zero cost when unused | nothing is allocated, compiled into a pipeline or recorded until a `DeviceStep` is created | plan 11 §11.10's first mechanism |
| Tests and size table | `tests/water_live_tests.cpp`, `tests/water_live_device_tests.cpp`, `tests/size_table.cpp` | the reference's cases on the card |
| Bench | `systems/water_live/bench/water_live_bench.cpp` | three states, three grids, three forms |
| Removal proof | `ENGINE_WITH_WATER_LIVE`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_WATER_LIVE=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. Everything else must still build and pass.
