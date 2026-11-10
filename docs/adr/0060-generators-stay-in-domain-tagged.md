# ADR-0060: Generators stay in `domain/`, tagged

- **Status:** Proposed
- **Date:** 2026-11-09
- **Plan references:** docs/plan/02-architecture.md §2.3, §2.8; docs/plan/08-toolchain.md §8.5
- **Docs touched:** [scene_gen](../subsystems/scene_gen.md#which-generators-a-host-carries), [08 §8.5](../plan/08-toolchain.md#85-build-system-and-ci), [roadmap](../roadmap.md) (T79), `AGENTS.md` (Layering)

## Context

The tree has seventeen generator capabilities: modules a host links for their registrars alone and
a scene reaches by name, never by a call ([ADR-0046](0046-scene-generators-register-themselves.md)
for the scene-generator registry, the mesh generators' registry beside it). Between them they make
26 registrations: grounds (`dunes`, `tiles`), placement generators (`ruins`, `city`, `scatter`,
`cliffs`, `deposits`, `sward`, ...), a sky (`earth`) and mesh generators (`flora`, `rock`, `cave`,
`soil`, ...). Fifteen of the seventeen sit in `domain/` and two (`scatter`, `water`) in `systems/`.

The plan describes the layers otherwise. Its layer table (02 §2.3) lists for L2 the wrappers and
runtimes — gfx, physics, nav, audio, ecs, doc, protocol, ddc, geometry — and puts "worldgen" in L3;
it names no generator in `domain/`. The 2026-11-09 review counted twenty generator directories in a
layer whose description does not mention them, found that one of them depends on `gfx`
(`rocks -> gfx`), and found the hosts' three hand-written lists of them drifted (the editor lacked
`caves` and `soil`). It put two answers to the owner (`REVIEW.md`, decision 6; `A5`, L-a):

- **A layer of their own**: a `world` layer between `domain` and `systems` holding `scene_gen` and
  every generator, `scatter` and `water` included, whose modules may depend on core, foundation and
  the domain modules that are not GPU (`geometry`, `assets`, `materials`, `texture`, `image`), and
  never on `gfx` or `systems/`. That makes the plan's L2 list true again and `rocks -> gfx` a refused
  edge, and costs twenty directory moves and every path in the docs that names one.
- **Keep the directories, and tag them**: each generator says what it is on its `engine_module()`
  line, the tag drives the hosts' one list, and the rule about `gfx` and `systems/` is a rule over
  tagged modules rather than over a layer. No directory moves.

Roadmap T79 built the tag (`GENERATOR <kind>:<name>...`, `cmake/EngineGenerators.cmake`) because
the drift had to stop whichever answer the owner gives: the tag is what joins the hosts' list and
what the run-time check compares with the registries. This ADR records the second answer as the
proposal, so that the tag is not mistaken for the decision.

## Decision

Proposed, for the owner's decision 6:

1. **Generators stay where they are.** A generator is a capability in the layer its dependencies put
   it in — `domain/` for fifteen, `systems/` for `scatter` and `water` — and the layer table gains a
   status note saying that L2 also holds generator capabilities, tagged, with this ADR's reason.
2. **The tag is what makes a module a generator.** `engine_module(... OPTIONAL WHOLE_ARCHIVE
   GENERATOR <kind>:<name>...)`: a capability (`OPTIONAL`), linked whole (`WHOLE_ARCHIVE`, or the
   linker drops its registrars), naming every registration its sources make. `modules.json` records
   it (`"generator"` on the module's row, `"generators"` for the build). The hosts that read a scene
   (`ENGINE_GENERATOR_HOSTS`: engine-view, engine-host, engine-editor) link the tagged list through
   `engine_host_generators()`, a host leaves one out only with a written reason, and the configure
   stops when they differ. Each host's test executable checks at run time that every tagged
   registration is in its registries and that nothing untagged is (`<host>.generators`).
3. **The rule the layer would have enforced is a rule over the tag.** A tagged module may not depend,
   directly or through another module, on `gfx` or on a module in `systems/`. It is **not enforced
   yet**: on 2026-11-09 it would refuse `rocks` (`rocks -> gfx`, directly), `soil` (through `rocks`),
   and `scatter` and `water` (in `systems/` themselves — the rule would move them to `domain/`, which
   their dependencies allow). Enforcing it is the step after the owner accepts this, once `rocks`
   no longer needs `gfx`; until then it is reported, not checked.

Only decision 2's tag and check are built. Nothing in the build depends on decisions 1 and 3 being
accepted: if the owner chooses the layer instead, the tag stays as the list's source and the layer
check replaces decision 3.

## Consequences

- One list. A new generator gains its tag and is in every host; a host cannot lose one without the
  configure saying which, or the run-time check saying which registration is missing.
- The tag carries the registrations' names, which the sources also carry. They are kept true by the
  run-time check's second half: a registrar added without its name on the tag fails `<host>.generators`.
  That is a second place to edit when a generator registers something new — the price of a check the
  configure cannot make, since it cannot see a registrar.
- `domain/` keeps meaning two things — the wrappers and runtimes the plan lists, and the generator
  capabilities — and the plan's layer table has to say so. A reader who expects the table to be the
  directory listing is surprised once, by a status note instead of by the tree.
- `rocks -> gfx` stays, unrefused, until decision 3 is enforced: a generator that reaches the GPU
  layer is not a function of its entry alone in the sense ADR-0046 meant, and the review's finding
  (S3-4) stays open with it.
- No directory moves, no doc paths change, no history is split by a rename.

## Revisit when

- The owner decides decision 6: accepted, this becomes the record and decision 3 gets its check; the
  layer chosen instead, this is superseded by the ADR that adds the layer, and the tag stays.
- `rocks` stops depending on `gfx` (or a second generator starts to): decision 3 can be enforced, or
  has to be.
- A generator appears that is not a capability (every executable needs it, as the renderer's own
  `waves` ground): the tag requires `OPTIONAL` and would have to say what such a module is.
- A third registry joins the two the tag's kinds name (`ground`, `placement`, `sky`, `mesh`).
