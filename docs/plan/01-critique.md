# 01 — Critique of the Brief

This section challenges the planning brief before anything is designed around it. It covers what the brief gets right, where the proposed mechanism is heavier than the problem needs, what is coupled incorrectly, what is missing entirely, and what is unnecessary complexity.

The short version: the four priorities are sound and the agent-native pillar is genuinely differentiating. The motivations behind the two most heavily worded sections (microarchitectural optimization and extreme-resolution robustness) are correct and come from real experience, and they are adopted as engine coding standards in [11-performance-principles](11-performance-principles.md). But the brief's *mechanisms* for them (a general hardware-calibration system, a family of compact screen-space types) are heavier than what solves the problem, and the brief is comparatively quiet on the three decisions that will actually determine whether agents can build a game with this engine: the **authoring data model**, the **incremental dependency/invalidation substrate**, and the **engine-as-server protocol**. Those three are where the architecture must be right from day one because they are the hardest to reverse.

---

## 1.1 What the brief gets right

- **Agent accessibility as an architectural requirement.** This is the correct framing and the thing no existing engine has. It forces "editor as thin client over a structured protocol," which is a good architecture even for humans, and it makes human tooling and agent tooling the same code path.
- **Separating source-asset complexity from runtime complexity.** Cluster-based virtualized geometry is the proven path and the brief correctly identifies the perceptual criterion (projected error, not object polygon budgets).
- **Reference renderer early, objective image metrics, automated accept/reject.** Cheap to build, enormous long-term payoff, and uniquely suited to agent-driven optimization. This should be in the first renderer milestone, not "eventually."
- **Performance as a first-class goal, not a polish phase.** The observation behind the brief is right: many current high-end engines ship features at unplayable frame rates and then downgrade to reach consumers, while well-optimized titles with less nominal technology run several times faster at the same resolution. This engine treats frame budgets as merge gates from the first renderer milestone ([11 §11.1](11-performance-principles.md#111-culture-budgets-are-gates)).
- **Sim LOD in space and time, event-driven scheduling.** Correct, well understood, and the brief's back-of-envelope reasoning holds up (see [05 §5.6](05-simulation.md#56-what-npc-scale-is-realistic)).
- **"What happens" vs. "how is it expressed."** The single best idea in the runtime-LLM section. It keeps the deterministic core authoritative and makes Tier 0 a hard requirement rather than a fallback.
- **Author the common cases as content during development.** Recurring runtime situations become authored, deterministic game content in the next build. This is caching at the content level, and it is the only way runtime LLM use stays cheap. No model is trained on anything in this process; see [12-ai-usage-policy](12-ai-usage-policy.md).
- **Hierarchical progressive elaboration with bidirectional constraint propagation.** Correct model for generation. The implementation consequence (structured state + explicit dependencies + invalidation) is the same substrate the asset pipeline needs, which is a significant simplification the brief does not quite draw out.
- **The mipmapped world plus an edit cell with immutable overlap.** This is the right answer to "how does an agent get full context for a local decision without an enormous token count": coarse global context from the mips, exact local context from a read-only ring around the edit region. It is adopted as the core of the agent context design ([06 §6.7](06-agent-tooling.md#67-edit-context-and-world-mips)).
- **Not starting with a 100-hour open world.** A polished 1–3 hour game is the right first target.

---

## 1.2 Re-scoped: hardware characterization and compile-time specialization

The experience behind these sections is real and the idioms are correct: hot paths with no branches, selected through a dispatch table of `constexpr`-specialized variants; iteration order that cooperates with the hardware prefetcher (a 20–30× win on a real 4K video-analysis kernel came from that alone); thread pools pinned by cache domain so data stays near the core that produced it. All of that is engine coding standard ([11-performance-principles](11-performance-principles.md)). What this section re-scopes is *which dimensions get specialized* and *when the machine gets measured*.

**Two kinds of specialization, and only one needs the hardware measured.**

| Dimension type | Examples | Right mechanism |
|---|---|---|
| **Data/config-selected** (changes what the inner loop does) | pixel format, material model, feature flags, instance type, LOD mode, SIMD width | `template<Config>` kernels behind a dispatch table, chosen once per batch from the data. Always right. No hardware measurement involved. |
| **Hardware-selected** (changes how fast the same loop runs) | prefetch distance, batch/tile size, thread count per pool, GPU workgroup size, stealing policy | Runtime parameters read once outside the loop and held in registers, defaulted from topology detection. Worth *calibrating* only when machines demonstrably disagree. |

The brief's template example mixes the two. Prefetch mode and locality mode are hardware-selected and cost nothing as hoisted runtime parameters; SIMD mode and batch layout change the loop body and belong in the template. Keeping the distinction is what prevents the Cartesian explosion: the compiled variant count grows only with data configurations, which are finite and known.

**What varies between machines and matters.**

| Dimension | Typical impact | Mechanism |
|---|---|---|
| SIMD ISA level (SSE4 / AVX2 / AVX-512) | 1.5–3× on vectorizable kernels | Function multi-versioning or ISPC multi-target; solved problem |
| Thread topology (P/E cores, CCDs, SMT, NUMA) | 10–40% on job-heavy frames | Topology-aware pinned pools built from CPUID/OS data. Day one. No benchmarking needed |
| GPU workgroup size / occupancy per kernel | 5–30% per kernel | Per-kernel tunable with a small candidate set |
| Memory budgets (VRAM, RAM) | Determines residency policy | Capability detection |
| Upscaler / RT / mesh-shader availability | Determines render path | Capability detection |
| Prefetch distance, batch sizes | 0–15%, kernel-specific | Runtime parameter; occasionally worth calibrating |

Cache-line utilization, stride, false sharing, TLB behavior, and prefetcher-friendly access order are *design-time* concerns solved once with good data layout, and the solution is good on every machine. You do not need to measure the machine to know that a 16-byte stride beats a 17-byte stride, or that a column walk through row-major data fights the prefetcher.

**Overfitting risk.** Synthetic microbenchmarks measure the microbenchmark, and real kernels cannot be measured until they exist. So: build the **tunables registry** and **in-engine benchmark harness** early (cheap, useful for humans and agents alike), but build the **automatic calibration runner** only after profiling shows that the best value for some tunable differs across at least two machines you own. Until then, calibration is speculative infrastructure.

**Recommendation.**

1. Day one: capability and topology detection; pinned, cache-domain-local worker pools; NUMA-aware arenas; per-kernel tunables with defaults derived from topology.
2. Renderer milestone: `constexpr` dispatch tables for config-selected kernels; ISPC for CPU SIMD; per-kernel GPU workgroup sizes as tunables.
3. Only after two machines disagree: a calibration runner measuring real kernels on real data, storing a per-hardware-hash profile, with a regression guard.

Details in [08-toolchain §8.7](08-toolchain.md#87-hardware-characterization-scaled-to-fit) and [11-performance-principles](11-performance-principles.md).

---

## 1.3 Under-weighted: the geometry / ray-tracing collision

The brief treats "Nanite-like geometry" and "real-time path tracing" as two independent features. They are the two features in modern rendering that most fight each other, and the interaction should be the central renderer decision.

- Virtualized geometry selects a different cluster LOD every frame per screen region. Ray tracing needs a bottom-level acceleration structure (BLAS) that is built ahead of time and is expensive to rebuild. If the rasterized surface and the traced surface come from different LODs, you get self-shadowing acne, light leaks, and mismatched reflections.
- BLAS memory for source-resolution geometry is prohibitive, so RT needs its own LOD, which reintroduces the mismatch.
- Destruction makes it worse: fractured geometry means new BLAS builds under a frame budget.

Epic needed years and a bespoke cluster-BLAS streaming system to get Nanite into hardware RT. NVIDIA's cluster acceleration structure extension (the basis of "RTX Mega Geometry") exists precisely to make cluster-LOD geometry traceable by building acceleration structures from the same clusters the rasterizer selected. It is currently vendor-specific.

**Consequence.** The renderer's geometry representation must be designed so that a cluster is simultaneously the unit of rasterization, of streaming, and of acceleration-structure construction. Whether the cluster-AS path or a fallback (per-LOD static BLAS with coarser RT LOD) is used is then a backend detail. This is the highest-priority renderer experiment. See [04-renderer](04-renderer.md#44-ray-tracing).

**Also under-weighted:** the brief says "path tracing" but the realistic real-time target is a *hybrid*: rasterized primary visibility through the cluster pipeline, ray-traced secondary lighting (direct light sampling, GI, reflections, shadows) with reservoir resampling and caches, and a full path tracer only as the offline reference. Primary-visibility path tracing at 25 megapixels is not a near-term real-time target on any hardware and should not shape the architecture.

---

## 1.4 Extreme displays: the data-type concern is real, and it is also a projection and budget problem

The compact-representation concern comes from real failures at 11520×2160: engines that render nothing past 8192 pixels, and UIs that vanish past 4096 pixels (Cyberpunk 2077 at launch). Those failures have specific causes. Render targets or atlases sized to a hard-coded or legacy maximum (8192 was the Direct3D 10 texture limit). UI or sprite vertex formats that pack screen coordinates into 12- or 16-bit fixed point (a 12.4 fixed-point coordinate tops out at exactly 4096). The brief's `CanRepresent` idea is the right fix, made concrete in [11 §11.6](11-performance-principles.md#116-no-hidden-limits): compact screen-space types are allowed only through a checked type constructed from the render configuration that falls back to a wider layout instead of clamping or wrapping; every render-target and atlas size derives from the render configuration; and CI renders at 11520×2160 and beyond every night with UI enabled. That closes the whole class of bugs. It is a rule and a test rather than an architecture, which is why it gets fewer words here than in the brief, not because it matters less.

What the brief does not cover, and what does need architecture at 48:9:

- **Projection.** A single planar perspective projection across three monitors at ~150° horizontal FOV is grotesquely distorted at the edges. Correct rendering requires either per-monitor frusta (three views, shared scene, shared culling hierarchy) or a non-planar projection (Panini or similar) with a post-resample. Both have consequences for screen-space techniques (TAA, SSR, denoisers assume a single planar projection). This is a real architectural decision.
- **Budget, and where to spend it.** 11520×2160 is 24.9 megapixels, 12× 1080p. No lighting technique survives that at uniform quality, and it should not try: surround players treat the side monitors as peripheral vision. Nobody looks at them, and nobody will notice whether a reflection there is exact. The architecture therefore needs a user-configurable **attention region** (default: the center monitor) with a quality gradient outside it: reduced internal resolution, coarser shading rate, fewer rays, cheaper effects. This is "foveation by monitor," it is the expected behavior rather than a compromise, and it makes the HUD-safe-region concept a *rendering* concept, not only a UI concept.
- **Presentation.** Driver-level surround (one giant swapchain) versus three windows on three outputs behave differently for vsync, HDR metadata, and refresh rates.

The UI coordinate-space model in the brief is correct and should be adopted as written. Details in [04-renderer](04-renderer.md#46-extreme-displays).

---

## 1.5 Coupling errors

| What the brief couples | Why it's wrong | Decouple into |
|---|---|---|
| Hardware calibration with kernel design (templates inside every kernel) | Kernels become unreadable; calibration cannot be turned off; agents cannot reason about which variant ran | A tunables registry that kernels *read from*; templates only for data-selected dimensions |
| Story protection with the destruction system | Protection is a policy over *narrative dependencies*, and destruction is only one way to break them (NPC death, item loss, faction collapse are others) | A narrative-dependency index + policy evaluator that any world-mutating system consults |
| World-gen "mips" with the streaming tile grid | They should share a spatial partition, but semantic layers, visual summaries, and design-density layers are separate data with separate producers | One spatial index; independent layer stores keyed by it |
| Editor operations with the MCP surface | MCP is one client. If the MCP tool *is* the operation, humans get a second, divergent code path | A native engine protocol; MCP bridge and GUI editor are both clients |
| Runtime LLM with NPC dialogue | The useful abstraction is "constrained choice among valid options" for *any* system; dialogue is one consumer | An intent-resolution interface with Tier 0 = utility scoring |
| Asset pipeline with world-generation canon invalidation | Presented as separate problems; they are the same incremental-computation problem with different rebuild policies | One dependency-tracking substrate, two policies (auto-rebuild vs. flag-for-review) |

---

## 1.6 Missing subsystems and requirements

The brief was deliberately non-exhaustive: it covered what was novel, what other engines get wrong, or what the owner most wanted, and defers the rest to this plan. The following are the systems the engine needs regardless. Several are large.

**Engine subsystems entirely absent**
- **Animation.** Skeletal animation, blend graphs/state machines, IK, retargeting, procedural motion, animation compression, and the animation ↔ physics ↔ navigation coupling. This is one of the three largest systems in any engine and the hardest for asset generation (rigging). It is mentioned only as `create_animation_graph`.
- **Audio.** Spatialization, occlusion (which can share the RT scene), mixing, music systems, dialogue playback with generated speech.
- **Input** and the input → gameplay → replay path (replay is impossible without a recorded input stream).
- **Save/load with schema migration.** Asked as a question but not treated as a subsystem. It is one, and it interacts with everything.
- **Localization and text pipeline**, including generated text.
- **Asset build system** with dependency tracking (the derived-data cache). Every downstream system depends on it.
- **Reflection / schema / serialization / code generation.** C++ has no usable reflection in shipping compilers. Every agent-facing feature (introspection, RPC, diffing, migration) depends on a schema system, so it must come first.
- **Memory management and budget accounting** per system, queryable.
- **Job system** with topology awareness (mentioned as a concern, not a system).
- **Platform / windowing / VFS / async I/O.**
- **Shader build pipeline** including permutation management and hot reload.
- **Game UI framework** (as distinct from the coordinate model).
- **Networking.** Single-player ships first, but multiplayer is a stated future option and the engine carries more of that burden than game code does. The architectural rules that keep it possible must be in place now ([05 §5.12](05-simulation.md#512-multiplayer-readiness)).
- **Packaging / distribution / crash reporting.**

**Cross-cutting requirements absent**
- **Provenance and licensing metadata** for generated assets and generated text, and a policy on which generators are allowed. Mentioned once as a question; it is a hard requirement for shipping anything.
- **Content safety** for runtime-generated text, and an **AI usage policy** that keeps every use of AI clearly inside provider terms ([12-ai-usage-policy](12-ai-usage-policy.md)).
- **Human developer tooling.** The brief describes agent tooling in depth and the human editor only implicitly. Developers who do not want an agent in the loop, and developers tweaking agent output by hand, need a complete editor ([06 §6.13](06-agent-tooling.md#613-human-developer-tooling)).
- **Human review surface.** The brief describes agents reviewing agents but not how a *human* director reviews a proposal: side-by-side captures, diffs of world state, a queue.
- **Agent visual context alignment.** When an agent looks at a capture, it should also get the entity-ID buffer for that capture so vision and structured data align ("the thing at pixel (x,y) is entity E"). Cheap and high-leverage.
- **Determinism boundaries.** Which systems must be deterministic (simulation, procedural generation given a seed) versus which may not be (rendering timing, LLM inference).
- **Time model.** Game time vs. wall time vs. sim ticks vs. frame; time scaling; pausing; catch-up. Needed by the temporal-LOD design and never defined.
- **Observer model.** Sim LOD is defined relative to "the player." With cameras, automated playtesters, cutscenes, spectator views, and eventually multiple players, LOD assignment must be a function over a set of observers with importance weights.
- **Generality guarantees.** The engine should not place limits on games built with it: world scale, session length, unit counts, view configurations, modding, custom rendering. Each has a cheap architectural hook now and an expensive retrofit later ([02 §2.7](02-architecture.md#27-generality-what-the-engine-must-not-preclude)).

---

## 1.7 Unnecessary complexity (cut or defer)

- A general hardware-calibration system before there are kernels to calibrate → detection, pinning, and tunables now; the calibration runner only with evidence (§1.2).
- Compile-time specialization on hardware-selected dimensions → runtime parameters hoisted out of the loop; templates for data-selected dimensions only (§1.2).
- A family of compact screen-space representations → one checked-type rule plus nightly extreme-resolution CI (§1.4).
- Multiple agent roles, permissions, and adjudication *before* one agent can complete one edit-test-review loop. Build the loop first; roles are configuration on top of it.
- Four simulation LOD tiers *before a game exists*. Build the mechanism (tier assignment, promote/demote hooks, event scheduler) and let the first game define how many tiers it needs.
- Physical display topology detection. Read it from the OS when needed; do not design around it early.
- Dynamic runtime fracture. Pre-fractured chunk hierarchies cover most gameplay; runtime Voronoi fracture is a later experiment.
- A second graphics backend. One API (Vulkan, which covers Windows and Linux; reasons in [08 §8.3](08-toolchain.md#83-graphics-api)) behind a thin API-neutral RHI. A D3D12 backend is worth adding only when cross-vendor cluster acceleration structures ship there and matter for the target audience.
- A bespoke scripting language. Choose an existing one by experiment.
- Runtime LLM Tiers 1–4. Design the interface, ship Tier 0, and revisit after automated playtesting shows which long-tail cases actually occur.

---

## 1.8 The three decisions that must be right early

Everything above is recoverable. These are not:

1. **The authoring data model.** Text-serialized, schema-driven, stable-ID'd, layered, diffable, git-native. If the world lives in binary level files, agents cannot diff, review, merge, or attribute changes, and every agent workflow in the brief becomes impossible. See [03-data-model](03-data-model.md).
2. **The incremental dependency substrate.** Content-addressed derived data with declared inputs, used for the asset pipeline *and* for canon/world-gen invalidation. Retrofitting dependency tracking onto a pipeline that was not built for it is a rewrite. See [03-data-model](03-data-model.md#36-dependencies-and-invalidation).
3. **Engine as a headless server.** The runtime must run with no window, no GPU (sim-only), or an offscreen GPU, driven entirely over a versioned protocol, with the GUI editor as a client. An engine whose editor is in-process with privileged access can never be fully agent-driven, and the same headless mode is a dedicated multiplayer server later. See [06-agent-tooling](06-agent-tooling.md).

A fourth is *nearly* irreversible: the **cluster as the universal geometry unit** for raster, streaming, and acceleration structures ([04-renderer](04-renderer.md#43-geometry)).
