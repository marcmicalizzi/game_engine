# ADR-0033: Content classes and view policies: what an output may contain, decided at instantiation

- **Status:** Accepted
- **Date:** 2026-09-20
- **Plan references:** docs/plan/06-agent-tooling.md §6.6 and §6.10, docs/plan/07-content-pipeline.md §7.4, §7.5 and §7.11, docs/plan/09-testing-profiling.md §9.7, docs/plan/12-ai-usage-policy.md §12.6, docs/plan/13-reference-consumer-games.md §13.1. Companion to [ADR-0032](0032-characters-are-parameter-vectors.md), whose modules, mesh sections and texture layers are the first things to carry a class. Builds on [ADR-0001](0001-headless-engine-server.md) (the engine is a server; clients are equal) and [ADR-0015](0015-ai-usage-policy.md). Supersedes nothing.
- **Docs touched:** `docs/plan/06-agent-tooling.md` §6.6 and §6.10, `docs/plan/07-content-pipeline.md` §7.11, `docs/plan/09-testing-profiling.md` §9.7, `docs/plan/12-ai-usage-policy.md` §12.6, `docs/plan/13-reference-consumer-games.md` §13.1

## Context

A game built with this engine will contain material that not every output of the engine should carry. The ordinary cases arrive long before any unusual one: a regional rating variant, a streamer-safe mode, a store screenshot, a trailer capture. The engine-specific case arrives immediately, because this engine is developed by agents driving it through a protocol ([ADR-0001](0001-headless-engine-server.md)) and `capture` is one of the day-one operations ([06 §6.9](../plan/06-agent-tooling.md#69-day-one-operations)) — so "what may appear in a picture the engine produces" is a question the engine has to be able to answer before Phase 5 rather than after.

Two ways this is normally done, both wrong for this engine. **A global build flag** produces two builds, two test matrices and two sets of goldens, and cannot express "this session may see less than that one". **A post-process over finished pixels** — blur, black bar, censor pass — is a filter over data that already reached the output, which means the data was in the frame, in the buffers, in the capture path and one bug away from the file. Neither survives the requirement that an agent session and a shipped build be the same engine with different policy.

What this ADR adds is a two-part vocabulary: a class on the content, a policy on the output, and substitution at the earliest point where the two meet.

## Decision

### 1. A content class is a (category, level) pair

Any module, mesh section, texture layer, material, effect or audio asset may carry **content classes**: a **level in a category**, one category at a time, as many categories as apply. The engine defines at least two, and they are independent:

| Category | Levels, in order |
|---|---|
| `nudity` | `none`, `partial`, `full` |
| `sexual` | `none`, `suggestive`, `explicit` |
| `violence` | `none`, `mild`, `graphic` |

`nudity` and `sexual` are **separate categories**, which mirrors how rating boards treat them: a body is not an act, and a regime that permits one routinely restricts the other. An asset carries `none` in every category it does not declare. A game may **add categories** — medical, horror, substance, whatever its rating regime asks about — and may refine within a level with its own metadata, but may **not reorder or reinterpret a category the engine defines**, because `level <= policy.cap[category]` is engine code and every substitution decision rests on it.

**Why pairs rather than one ordered ladder.** These categories are not comparable and are not restricted together: regions rate them independently, and most of the real variants a game ships differ in one and not the others. A single scale would force a ranking between them, and — worse — it would make an engine-level rule about one silently a rule about another, which is how an engine ends up forbidding an injury state on a young character because of a rule written about something else. That is a game's design and rating decision, not the engine's.

A class is a property of an asset, declared where the asset is declared, carried through derived data like provenance is ([07 §7.5](../plan/07-content-pipeline.md#75-provenance-and-licensing-metadata)), and inherited by anything built from it.

**One rule elsewhere is not a policy at all.** [ADR-0032](0032-characters-are-parameter-vectors.md) decision 13 — a base admitting content above `none` in `nudity` or `sexual` declares an adult age range — is a property of content and of the bake. It has no cap to raise, no operator setting and no game override, and nothing in this ADR configures it. Everything else here is configuration; that is not.

### 2. A view policy caps each category, and names a substitution per category

A **view policy** is a **cap per category** plus the **substitution** applied to anything above that cap. The substitution is named per category because the right answer differs by category: `cover` is the natural one for nudity and meaningless for a blood decal, where `omit` is. A category the policy does not mention is capped at `none`.

| Substitution | What the output gets |
|---|---|
| `omit` | The item is not instantiated; for a graft module, the base's own neutral region shows instead ([ADR-0032](0032-characters-are-parameter-vectors.md) decision 6) |
| `proxy` | An abstract stand-in shape of the right size and position, so silhouettes, collision and framing stay meaningful |
| `cover` | A mandatory garment or decal slot is filled; the output is a dressed character rather than an edited one |
| `obscure at capture` | The last resort: the output is produced and then obscured |

**Substitution happens at instantiation for that view wherever possible** — when the character is composited, when the module set is resolved, when the material stack is baked, when the scene is built for that view — so that classified data **never reaches that view's render**. `obscure at capture` exists because a few cases genuinely cannot be resolved earlier, and it is named as what it is: a filter over data that was present. A policy whose substitution is `obscure at capture` for something the pipeline could have omitted is a bug, not a configuration.

### 3. The uses, in the order most games meet them

1. **Regional and rating variants** — one build, one content set, a policy per distribution, and per-category caps because that is what regions actually differ in.
2. **Streamer mode** — a policy the player sets, applied to the game's own output.
3. **Store screenshots and marketing capture** — a policy on a capture session, so the picture cannot contain what the storefront forbids.
4. **Agent sessions** — the case this engine has first, and decision 4.

### 4. For a protocol session, the policy is a property of the session

A session's view policy is set by the **host operator's configuration** when the session is opened ([06 §6.2](../plan/06-agent-tooling.md#62-engine-protocol)). **A client cannot raise any category's cap**: there is no protocol method that widens what the session may see, and a request for content above a cap returns a structured error naming the policy and the category rather than the content. A client may narrow its own policy for one call, because narrowing is always safe.

**Every capture records the policy it was made under**, in its provenance, beside the camera and the channels. A restricted capture that does not say it is restricted is worse than no capture: it is a picture that will later be read as evidence that something is absent from the world when it is only absent from the view.

**What that enforcement is and is not.** "A client cannot raise its own policy" is enforceable against a **protocol client**, which can only ask the server for what the session allows. It is not a security boundary against an agent with **file-system access to the host**, which could edit the configuration the session was opened from — and pretending otherwise would be the worse failure, because a guard rail believed to be a wall gets leaned on. Against such an agent this is a **guard rail and an audit trail**: the policy lives in host configuration **outside the agent-editable document model**, a change to it is a human commit visible in review, and every capture carries its policy in provenance, so a session that ran wider than it should have says so afterwards. The rule for agents belongs where agents read it first, in the repository's conventions, stated as a rule rather than relied on as a mechanism: an agent does not change view-policy configuration, and a check that needs a higher policy becomes a human review item.

### 5. Diagnostic views obey the same policy

A clay, wireframe, UV, skin-weight, normal, deformation or heat-map view **of classified geometry is still that geometry**. The shading does not change what the shape is, and a policy that stopped at the lit renderer would be a policy with a hole in it shaped exactly like a debugging tool.

So an agent-visible diagnostic of a classified module is either **numeric** — validator reports, histograms, boundary-loop plots, UV-space plots, error tables — or it is the **base without the module**. Both are more useful than a picture for the failures these validators catch, which is the part of this constraint that is not a compromise.

### 6. Classified content is verified by numbers and by people

It follows from decisions 2 and 5 that classified content is verified by the **numeric validators** of [ADR-0032](0032-characters-are-parameter-vectors.md) decision 12 and by **human review items** in the review queue ([06 §6.10](../plan/06-agent-tooling.md#610-multi-agent-roles-review-and-the-human-director)) — a `classified content review` item kind, raised where a check cannot appropriately be made by an agent, carrying the validator's numbers and opened by a human at full policy.

**The complete character always remains in the document and in the store.** A policy affects outputs, never the source of truth: nothing is deleted, nothing is redacted at rest, and the same document produces the full picture under a full policy.

### 7. The default policy for an agent session, and who decides above it

The default is **`nudity: none`, `sexual: none`, `violence: mild`**, with any category a game adds capped at `none` until the operator says otherwise — the safe direction for a category the engine has never heard of.

**That default is a conservative starting point, not an engine-imposed ceiling.** The host operator configures a session's caps, and raising one is an ordinary configuration change. It has to be, because models and providers differ in what their terms allow, and **configuring a session consistently with the terms of the model in use is the operator's responsibility**. The engine supplies the mechanism and the safe default and **encodes no vendor's policy** — a hard-coded ceiling would be this project guessing at another company's terms, and would be wrong the week either changes. An operator whose agent accepts non-sexual nudity may raise `nudity` while `sexual` stays `none`; another may raise nothing at all.

**Permission is not compulsion.** A policy says what an output *may* contain, never what a model must produce or look at. An agent may decline a capture its policy allowed, and that is a **normal handled outcome**, not a failure: the check becomes a human review item ([06 §6.10](../plan/06-agent-tooling.md#610-multi-agent-roles-review-and-the-human-director)) and the work continues. A workflow never treats a refusal as an error to retry around or rephrase around — a refusal is information about the task, and the numeric validators exist precisely so that the work does not depend on a picture anybody has to look at.

The default is also **how this engine is developed**: development agents on this project work with neutral fixtures under it. Plan 12 §12.6 states that rule and its reasons; this ADR is the mechanism that makes it something the engine enforces rather than something a contributor remembers.

## Consequences

**Authoring rules.** A base texture is authored neutral and classified detail lives in tagged layers; a region that can be classified is a **section** of the mesh with its own visibility bit; a module declares its classes where it is declared. These are the same rules [ADR-0032](0032-characters-are-parameter-vectors.md) decisions 6 and 9 arrive at from the geometry side, which is the reason they are affordable: the layered compositor and the section mask exist for garments anyway, and the classes ride on them.

**What the capture API gains.** `capture` takes the session's view policy into account and records it in the capture's provenance ([06 §6.6](../plan/06-agent-tooling.md#66-introspection)); the scene built for a restricted view is built restricted; diagnostic channels are subject to the same policy as the color channel.

**What a validator must provide.** Enough numbers that human review is rare rather than routine: continuity at a module's boundary loop, penetration and self-intersection depth, volume and proportion bounds, texel density, LOD attribute error, and a stable identity for the thing being reviewed so an item can be closed against a version rather than against a memory. A validator that reports "looks wrong" has moved the work to a human; a validator that reports a number with a threshold has not.

**What is now forbidden.** A build-time switch as the mechanism for any of this; a client that can raise its own session policy; a capture whose provenance does not name its policy; a diagnostic view that bypasses the policy the color view obeys; a game that reorders or redefines a category the engine defines; collapsing the categories back into one scale; redaction of the document itself as a way of implementing a view; an agent that edits view-policy configuration; a workflow that retries or rephrases around a refusal; and treating [ADR-0032](0032-characters-are-parameter-vectors.md)'s adult rule as a policy cap that something can raise.

**What this is not.** It is not a rating system, not a legal compliance layer, and not a substitute for a storefront's own requirements. It is the mechanism a game uses to implement whatever those turn out to require.

## Revisit when

A platform's rating rules need finer levels inside a category, or a category games keep adding for themselves turns out to be common enough to belong in the engine — both are the likely triggers, and both are additive. Also if `proxy` or `cover` substitutions turn out to need content the base does not have (a proxy shape, a mandatory slot) often enough that they become an authoring obligation rather than a fallback, which would make them part of the base definition in [ADR-0032](0032-characters-are-parameter-vectors.md) decision 2.
