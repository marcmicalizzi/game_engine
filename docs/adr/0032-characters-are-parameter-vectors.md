# ADR-0032: Characters are parameter vectors over a canonical topology

- **Status:** Accepted
- **Date:** 2026-09-20
- **Plan references:** docs/plan/07-content-pipeline.md §7.7 and §7.11, docs/plan/05-simulation.md §5.4, §5.11, §5.14, §5.15 and §5.16, docs/plan/04-renderer.md §4.3, docs/plan/09-testing-profiling.md §9.1 and §9.7, docs/plan/10-roadmap-risks.md §10.2 (Phase 5), §10.4 and §10.5 (E27–E29), docs/plan/13-reference-consumer-games.md §13.1, §13.2 and §13.4, docs/plan/12-ai-usage-policy.md §12.6. Builds on [ADR-0004](0004-dependency-substrate.md) (derived data), [ADR-0010](0010-deterministic-sim-and-lod-contract.md) (LOD is a system property), [ADR-0014](0014-apache-2-license-and-dependency-policy.md) (licence policy), [ADR-0026](0026-deformable-volumes-first-class.md) (cage separate from the render mesh), [ADR-0027](0027-additive-capabilities.md) (capabilities are additive). Content classes are [ADR-0033](0033-content-classes-and-view-policies.md). Supersedes nothing.
- **Docs touched:** `docs/plan/07-content-pipeline.md` §7.7 and §7.11, `docs/plan/05-simulation.md` §5.15 and §5.16, `docs/plan/06-agent-tooling.md` §6.6 and §6.10, `docs/plan/09-testing-profiling.md` §9.1 and §9.7, `docs/plan/10-roadmap-risks.md` §10.2 and §10.5, `docs/plan/12-ai-usage-policy.md` §12.6, `docs/plan/13-reference-consumer-games.md` §13.1, §13.2 and §13.4, `docs/publishing.md`

## Context

[07 §7.7](../plan/07-content-pipeline.md#77-ml-asset-generation-novelty-content) already decided the strategy — *standardize first*: a standard skeleton, a parametric body, modular clothing and hair slots, a face-rig standard, and generation that fills slots rather than producing whole rigged characters. Half of that exists. The standard skeleton is built ([anim](../subsystems/anim.md)); "a parametric body" was a phrase with no design behind it.

Three consumers turn it into a decision now rather than in Phase 5, because what it settles is a *format* and formats are what cannot be retrofitted once content exists. Island City's persistent population ([13 §13.2](../plan/13-reference-consumer-games.md#132-consumer-b--island-city)) needs a character to cost a vector rather than a mesh, at a population of a city. Desert Survival wants an optional cosmetic creator on one base ([13 §13.1](../plan/13-reference-consumer-games.md#131-consumer-a--desert-survival)). And the "deformable cage authoring cost" risk ([10 §10.4](../plan/10-roadmap-risks.md#104-major-technical-risks)) is already mitigated by the sentence "one cage per body archetype is retargeted rather than authored per character", which is only true if a body archetype exists to hang a cage on.

Engines and character systems get this wrong in five recognizable ways, and the decisions below exist to close those doors first:

- **Two base meshes**, one per sex, which doubles every garment, every skin weight set, every texture layer, every deformation cage and every LOD structure downstream, forever, to express what is a set of continuous axes.
- **Per-character geometry and per-character rigs**, which make a hand-authored hero affordable and a population impossible.
- **Sliders assumed to be independent additive morphs**, which is false exactly where it is most visible: the strongly interacting macro axes.
- **An engine that knows what a module depicts**, which turns every new kind of body part into an engine change instead of content.
- **A standard that stops being optional**, so that a game wanting one hand-authored character pays for a system built for a population. This one is the reason decision 1 is decision 1.

**Prior art, and the licence caution that comes with it.** The macro-target scheme below is not novel and should not be reinvented in ignorance. As this project currently understands it — **to be verified against the actual licence text before any asset, file or line is taken** — MakeHuman's base mesh and targets are CC0 while its *code* is AGPL-3.0 and therefore excluded by [ADR-0014](0014-apache-2-license-and-dependency-policy.md); SMPL-family body models are published under non-commercial research licences and are excluded outright, weights and code alike. What this ADR takes from either is **published ideas** — a linear shape space, a joint regressor over vertices, pose-dependent corrective shapes — which are not the licensed artifact. Nothing is imported, adapted or vendored without reading the licence and recording it in the dependency manifest ([08 §8.8](../plan/08-toolchain.md#88-engine-license-and-dependency-policy)) and the asset's provenance ([07 §7.5](../plan/07-content-pipeline.md#75-provenance-and-licensing-metadata)).

## Decision

### 1. The standard is opt-in, per asset and per game

Everything below this line is **optional data**. A character may be a plain authored asset — a static mesh, or a skinned glTF with a skeleton and clips and no parameter vector, no morph channels, no modules, no appearance component — and it loads, streams, animates and renders exactly as any mesh does today, with no involvement from any of this. A game that never enables the character capability pays nothing for it: no bake step, no extra vertex stream, no deform stage, no compositor, no component, no pass ([11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)), and `ENGINE_MINIMAL=ON` proves it at link time exactly as it does for every other capability ([ADR-0027](0027-additive-capabilities.md)).

The optionality is per *piece*, not only per game. The morph stream is optional per mesh the way the skin stream already is ([04 §4.3](../plan/04-renderer.md#43-geometry)); each stage of the deform chain of decision 7 is per instance, and an instance that declares none is a rigid instance drawing off the quantized grid; the appearance component, the module set, the parameter rig and the material compositor are each absent by default. A base that has corner sculpts and no modules is a complete character system; a game with one hand-authored hero and no creator never meets any of this.

*Why this is decision 1 rather than a footnote:* a character standard is exactly the kind of system that quietly becomes mandatory — first in the asset format, then in the renderer's fast path, then in the documentation's tone, until a small game that wants a static crab is paying for a parameter rig it never uses. The engine's structure must not push a simple game toward deformation, and neither must these documents. Most games that use the engine will never enable any of this, and that is the expected case, not a degraded one.

### 2. One canonical topology per base family

A **base** is one mesh, in a neutral pose, with one vertex ordering, one UV layout, edge flow built for deformation, and one LOD structure — **unified across sex characteristics**, which are continuous axes of the parameter space and not two meshes. Everything authored against a base is authored **once** and fits every character derived from it: skin weights, garments, texture layers, hair and accessory attachment, the deformation cage of [ADR-0026](0026-deformable-volumes-first-class.md), and the cluster DAG.

A generated or scanned body is never a base. It becomes a **preset** by conforming the canonical body to it (a wrap/shrink-to-fit followed by morph transfer), which keeps the vertex ordering and therefore keeps everything authored against the base valid. A second base is a second *family* — a non-humanoid creature, a stylization the same edge flow cannot express — and is a deliberate act with its own garments and its own cage, not something a character creator produces.

*Why:* every asset class downstream is indexed by the base's vertex ordering and UV layout. One topology is what makes "authored once, fits every character" a property of the format rather than a hope.

### 3. Three layers of parameters, with a declarative rig between them

```
semantic parameters  ->  parameter rig  ->  deformation channels
(schema-typed,           (a DAG evaluated      (corner weights, detail
 named, versioned,        at bake time)         channel gains, joint
 game-defined)                                  regressor inputs, material
                                                layer weights)
```

- **Semantic parameters** are what a creator UI, a random generator, a save file, a replay header and an agent see. They are schema types ([ADR-0007](0007-schema-code-generation.md)), named, versioned, and **defined by the game**, not by the engine.
- The **parameter rig** is a declarative DAG: linear maps, curves, products, min/max, clamps, remaps, and conditions on module presence. It is **data, not scripting** — no loops, no side effects, no host calls — so it serializes, diffs, migrates, and evaluates identically everywhere. It is evaluated at bake time, never per frame.
- **Deformation channels** are the engine-side outputs. Nothing above the rig knows they exist.

*Why:* the alternative — semantic parameters wired straight to morph channels — makes every UI change an asset change and every asset change a save-compatibility problem. The rig is the shear layer, and keeping it declarative is what keeps a character deterministic and reviewable.

### 4. How channels combine: multilinear macro corners, region-masked detail

Not every parameter composes the same way, and pretending otherwise is the single most common failure in this design.

**The macro corner set is three axes: overall mass, muscularity, and the sex-characteristic axis — eight corner sculpts per base.** These three interpolate **multilinearly across sculpted corners** because their surface shapes genuinely do not add: *a heavy muscular body is not the sum of a heavy delta and a muscular delta*, the two interact over the whole surface, and adding them produces a shape that is neither.

Everything else is carried more cheaply, and the reason is one rule: **every corner axis doubles the hand sculpting per base, so an axis earns a corner only when no cheaper carrier represents it.** Three axes are eight sculpts; six would be sixty-four.

- **Height and limb, torso and shoulder proportion are skeleton parameters**, not corner axes: bone scales and offsets applied through the joint regressor of decision 5, with at most small region-masked correctives where scaling alone looks wrong (a shoulder that needs its deltoid reshaped, not just moved). A scale costs nothing; a corner costs a sculpt and a doubling.
- **Age within the adult range is not a corner axis either**: it is a set of **region-masked detail channels** plus a **skin-layer set in the compositor** (decision 9), gain-scheduled by the macro context like any other detail. Ageing is mostly surface and material, and where it is shape it is local.
- **Detail channels are additive, but region-masked and gain-scheduled by the macro context.** A local channel writes through a mask and its gain is a function of where the three macro axes are, so a detail authored at one end of the macro space does not misbehave at the other.
- **Combination correctives** exist for known bad pairs: a sculpted delta keyed on a product of channels, added only where the pair actually misbehaves. They are a repair mechanism with a cost, not a design tool.
- **Pose-space correctives** run at run time, keyed on joint rotation, and are **scaled by shape** — an elbow's crease depends on how much arm there is. They are the one corrective family that is not baked.

### 5. Joints follow shape through a sparse joint regressor

Each base carries one **joint regressor**: a sparse linear map from a small set of surface vertices to each joint's position, evaluated on the *baked* shape. Per-channel joint deltas are forbidden. A module may add **extension bones** (a tail, a tool, an extra limb); the core is the standard skeleton of `domain/anim` ([anim](../subsystems/anim.md#the-standard-skeleton-first-cut)), so the clip library and the retarget keep working for every character of every base.

**This is also where the skeleton parameters live.** Height and limb, torso and shoulder proportion are **bone scales and offsets** composed with the regressed joints, not shape channels: the rig drives them, the regressor places the rest, and the retarget's hip-height translation scale ([anim](../subsystems/anim.md#the-standard-skeleton-first-cut)) already means a clip authored for one height plays correctly on another. The stated rule is the general one: **any axis a bone scale can carry belongs here rather than in the corner set.**

*Why:* per-channel joint deltas are O(channels × joints) hand-authored numbers that go stale the moment a channel is re-sculpted, and they compose no better than additive morphs do. A regressor is authored once, is a function of the shape actually produced, and is therefore correct for combinations nobody authored.

### 6. Graft modules: a module replaces a region of a body

A **module** is a mesh that replaces a region of the base. Two mechanisms, both of which already have a reason to exist:

- The base's region is hidden by the **section-visibility mask** — the same per-section mask a garment uses to hide the skin it covers, which every character system needs anyway to stop skin poking through clothing.
- The module's mesh attaches along a **boundary loop** that matches the base's at that section: same vertex count, same order, same UV positions, same skin weights.

A module brings its own channels (driven by the same parameter rig), its own material layers, optional extension bones, optional soft-tissue regions ([05 §5.14](../plan/05-simulation.md#514-deformable-volumes)), and its own LOD policy — below a screen-size threshold the module is dropped and the base's neutral region is shown instead, which is why the base must be complete and presentable on its own.

**The engine does not know what a module depicts.** A prosthetic arm, a creature part, a wound, a piece of equipment that replaces geometry rather than covering it, and anatomical detail are one mechanism and one code path. The only thing the engine reads about a module's content is its **content classes** — a level in each category that applies ([ADR-0033](0033-content-classes-and-view-policies.md)) — which are numbers, not meanings.

*Why:* the alternative is a taxonomy of body-part kinds in engine code, which is an engine change per content idea and a policy argument per taxonomy entry. One mechanism with one declared class is both more general and easier to govern.

### 7. Shape is a static deformation of shared clusters

The cluster DAG and the streaming pages ([04 §4.3](../plan/04-renderer.md#43-geometry)) are built **once per base and once per module**, never per character. A character is:

- a **parameter vector**, plus
- a **cached position/normal (and tangent) stream** keyed in the derived-data cache ([ADR-0004](0004-dependency-substrate.md)) by the hash of that vector, the base and module versions, and the bake function version.

The **deform chain order is static**: static shape → pose correctives → skinning → cage or soft-tissue displacement. Static shape is a bake, not a per-frame pass; the rest is the per-frame deformed-vertex pool that already exists ([04 §4.3](../plan/04-renderer.md#43-geometry)).

**LOD error bounds must hold over the parameter space.** One DAG serves every character of a base, so the bounds are established against the sampled extremes of that space (corners plus low-discrepancy samples) and inflated to cover it, rather than measured on the neutral body alone. Whether one DAG can hold its bounds across a base's whole parameter space at an acceptable inflation is **E27**; if it cannot, the fallback is per-preset DAGs for the presets that break it, not per-character DAGs.

At population scale, **only materialized characters hold a baked stream**. Farther simulation tiers draw **archetype presets** — a small set of pre-baked bodies — with joint scales applied, which is the same trade the materialization contract makes everywhere else ([05 §5.4](../plan/05-simulation.md#54-lod-tier-assignment), [03 §3.4](../plan/03-data-model.md#34-the-runtime-world)).

### 8. Body parameters are cosmetic by default

A body parameter changes what a character looks like and nothing else. A game may **opt in** to let composition feed soft-tissue mass, compliance and secondary motion ([05 §5.14](../plan/05-simulation.md#514-deformable-volumes)), and that is the only coupling the engine offers by default. Body shape never feeds collision, never feeds locomotion, and never feeds stats unless a game deliberately wires it.

**Desert Survival does not opt in**, because its runs are seeded and scored and must stay comparable between players ([13 §13.1](../plan/13-reference-consumer-games.md#131-consumer-a--desert-survival)): a body shape that changed a hitbox or a walk speed would make a leaderboard a character-builder metagame. Clothing effects come from **item stats**, never from the shape of the body under them.

*Why:* the opposite default makes every appearance change a balance change and a replay-compatibility hazard, and it is very hard to take back once content depends on it.

### 9. Materials: a layered compositor, baked once per character

Character materials are a **layer stack** composited at bake time into the character's texture set: a neutral base layer, then tinted, masked and parameter-driven layers (complexion, ageing, marks, dirt, damage, makeup, module-supplied layers). Parameters drive layer weights and masks through the same rig.

Two authoring rules follow and are enforced by validation: **the base texture is authored neutral**, and **all classified detail lives in tagged layers** carrying their content classes ([ADR-0033](0033-content-classes-and-view-policies.md)). A view policy can then drop or substitute a layer *before the composite runs*, which is the only way a restricted view can be produced without post-processing finished pixels.

### 10. Serialization: names, modules, and a seed

A character serializes as **semantic parameters by name**, the **module set**, and the **seed** that produced it if it was generated. Not channel values, not vertex data. It is schema-versioned with migrations ([03 §3.8](../plan/03-data-model.md#38-schema-evolution)), quantized (16 bits per normalized parameter; nobody can see a 1/65536 change in a body), and canonical, so the **hash of the canonical form is the bake cache key**. The vector travels in a **replay header**, so a run replays with the character it was played with rather than with whatever the creator holds today.

### 11. Random generation is game-defined, seeded, and validated

The engine provides the machinery: distributions and correlations declared per base as data, sampling from a seed, deterministic and reproducible, with **rejection through the validators of decision 12**. The distributions themselves are the game's content. A generator that cannot produce a valid character is a content error that names the failing constraint, not a character that ships broken.

### 12. Validation is numeric and sweeps the parameter space

A character system is validated by **sampling the parameter space**, not by looking at a character. The sweep is the corners of the macro space plus a low-discrepancy sample of the interior, times the module set, times a pose set, through validators that produce numbers:

inverted or degenerate triangles; self-intersection and interpenetration depth; boundary-loop continuity where a module meets the base (position, normal angle, skin-weight agreement); volume and proportion bounds; joint-regressor sanity (joints inside the surface, no crossed or inverted chains); skin-weight normalization and influence counts; texel-density ratio across the character and against garments; LOD attribute error (`geometry::measure_lod_attribute_error`, [geometry](../subsystems/geometry.md#what-the-simplifier-is-given-and-why)); garment penetration; and cage fit for a character that carries a deformable volume ([07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets)).

Every one of these is a number an agent can compute and compare **without looking at anything**, which is the property that makes the system developable under the rules of plan 12 §12.6 and the view policies of [ADR-0033](0033-content-classes-and-view-policies.md). The list is carried in plan 09 §9.7 and the sweep is a gate on the content build.

### 13. Characters that can carry nudity- or sexual-classified detail are adults, and the engine enforces it

A base definition that admits any module or texture layer above `none` in the **`nudity` or `sexual`** category ([ADR-0033](0033-content-classes-and-view-policies.md)) **declares an adult age range**. Four enforcement points, all inside the engine and none of them configurable by a game:

1. the **parameter rig clamps** the age axis and any proportion axis bound to it into that range;
2. the **validators reject** a parameter vector outside it, as a content error with the axis named;
3. the **random generator cannot sample** outside it;
4. the **bake refuses** a character that violates it, whatever a game's UI allows, because a UI is game code and this is the engine's floor.

A base that declares no adult age range simply cannot carry `nudity` or `sexual` above `none` — the module or layer fails validation at the base, before any character exists.

**This rule is not part of the view policy and is not configurable by anyone.** A view policy decides what an *output* may contain and is set per output by whoever owns it ([ADR-0033](0033-content-classes-and-view-policies.md)); this is a property of the *content* and of the bake, it has no cap to raise, no operator setting, and no game override. The two mechanisms are deliberately separate: one is configuration, this is not.

**The engine places no other age restriction on characters, and this rule binds to those two categories only.** A game whose bodies carry nothing above `none` in `nudity` or `sexual` may have characters of any age with the whole parametric system — every axis, every module, every material layer — and how it handles violence, injury or damage across ages is the game's decision under its own rating regime. An engine rule that generalized past those categories would be making a game's design and rating decisions for it, which is exactly why [ADR-0033](0033-content-classes-and-view-policies.md) keeps the categories independent.

### 14. Engine, optional reference capability, game

| Layer | What it owns |
|---|---|
| **Engine core** | The morph vertex stream and the deform chain; the joint regressor evaluation; section-visibility masks and graft attachment; the parameter-rig evaluator; the layered material compositor; content classes and view policies ([ADR-0033](0033-content-classes-and-view-policies.md)) |
| **Optional reference capability** ([ADR-0027](0027-additive-capabilities.md)) | The appearance component and its schema, the bake step as a `derived` node, and the tools: conform, morph transfer, corner-set build, validator sweep |
| **Game** | The semantic parameter set and its schema; base meshes and corner sculpts; modules; distributions; slot definitions; the creator UI |
| **Outside this repository** | A game's private or experimental content packs and capabilities, which are additive by [ADR-0027](0027-additive-capabilities.md) and live in the overlay of [docs/publishing.md](../publishing.md) |

The split is the usual one for this project: the engine owns mechanisms and formats, the reference capability owns a working implementation that can be switched off (`ENGINE_MINIMAL=ON` must still build), and the game owns everything that carries meaning.

## Consequences

**What now has to exist.** A morph vertex stream and a staged deform chain in the renderer (**in progress**, Phase 1, beside the deformed-vertex pool of [04 §4.3](../plan/04-renderer.md#43-geometry)); a parameter-rig evaluator and its schema; a joint regressor format and its evaluation; a section-visibility mask on the mesh format and boundary-loop metadata on modules; a bake step as a `derived` node with the parameter hash as its cache key; a layered material compositor; the validator set of decision 12 with rule IDs in the structured diagnostics of [07 §7.3](../plan/07-content-pipeline.md#73-content-build-the-derived-data-graph); and the content classes of [ADR-0033](0033-content-classes-and-view-policies.md) on modules, sections and layers.

**What becomes easier.** One clip library, one garment set, one cage archetype and one cluster DAG per base rather than per character. A character in a save, a replay, a diff and an agent's context is a short named record. A population costs vectors plus a handful of archetype bakes. Generation fills slots against a standard, which is what [07 §7.7](../plan/07-content-pipeline.md#77-ml-asset-generation-novelty-content) asked for and what makes E10's pass-rate question answerable for characters at all.

**What becomes harder, and what it costs.** Corner sculpts are hand work that grows as 2^axes, so the macro set is capped at the three axes of decision 4 — eight sculpts per base — and a fourth is a decision with a doubling attached. A conform step stands between any generated or scanned body and the pipeline. A bake is a cost per distinct character — time and bytes — which **E28** measures and which decides how many materialized characters a tier can hold. One DAG per base is an assumption **E27** tests. Module boundary continuity across LOD levels is **E29**, and the honest possibility it tests is that modules must drop before the first simplified level rather than survive into it.

**What is now forbidden.** A second base mesh to express a sex characteristic. A per-character skeleton or a per-character cluster DAG. Treating the macro axes as independent additive morphs. Per-channel joint deltas. A module whose placement requires the engine to know what it depicts. A character baked from anything other than a parameter vector that exists in the document (a shipped character must be reproducible from its record). Classified detail in a base texture rather than in a tagged layer. A base that admits `nudity` or `sexual` above `none` without declaring an adult age range; treating that rule as a view policy that something can raise; and, equally, an engine-level age rule that reaches past those two categories. And, from decision 1, any of it becoming mandatory: an asset format, a renderer path, a validator or a document that assumes every character is a parameter vector.

## Revisit when

The macro axis count outgrows what can be sculpted — at which point the answer is a different shape model, not more corners. A learned shape space becomes a credible replacement for sculpted corners, which is a licensing review, a determinism review, and a review against [12 §12.2](../plan/12-ai-usage-policy.md#122-rules) before it is an engineering question. Or E28 says a baked position stream per materialized character is too expensive at population scale, at which point the archetype-preset tier moves nearer and the parameter space is quantized to a preset grid for everything but heroes.
