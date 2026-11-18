# ADR-0074: Classes travel with derived data, and a view policy is the operator's file, decided at load

- **Status:** Proposed
- **Date:** 2026-11-18
- **Plan references:** docs/plan/06-agent-tooling.md §6.6 and §6.10, docs/plan/07-content-pipeline.md §7.5 and §7.11, docs/plan/12-ai-usage-policy.md §12.6; roadmap C3 and A26. Builds on [ADR-0033](0033-content-classes-and-view-policies.md), which it does not change, on [ADR-0072](0072-the-task-ledger-beside-the-document.md) (the ledger's person's review item), and on two decisions proposed the same day and not yet merged: roadmap A4's (a connection is who its credential says, and a role a host cannot enforce fails closed) and roadmap T174's (a host reads and writes a client's paths only under the roots its operator named).
- **Docs touched:** `docs/content-classes.md` (the design), `docs/subsystems/protocol.md` ("View policies on captures, reads and review items"), `docs/subsystems/assets.md` ("Content classes"), `docs/subsystems/renderer.md` ("Content classes at load"), `docs/plan/06-agent-tooling.md` §6.10, `docs/roadmap.md` (C3, A26)

## Context

[ADR-0033](0033-content-classes-and-view-policies.md) decided the vocabulary: a content class is a (category, level) pair; a view policy caps each category and names a substitution; the substitution happens when content is instantiated for a view; a session's policy is the host operator's, and a client cannot raise it. Nothing of it is built. Today the engine knows nothing about any content's class at any of the twenty-four ways content leaves it ([content classes](../content-classes.md#every-way-content-leaves-the-engine)). The generation tools write `content_class: general` into their sidecars and nothing reads it.

Five things were left open, and each has a wrong answer that is easy to build:

1. **Where a class is written.** On the document record alone, any agent's edit could declassify an asset. On the source alone, a neutral asset could not be classified by how it is placed.
2. **How it survives derived data.** The cache is keyed by the source's bytes. A class kept beside the source and not in the key is shed the first time a build made before the class existed is found under the same key.
3. **Where a policy lives.** In the roles file, which is in the repository and edited in ordinary work, or behind a protocol method, an agent could change its own policy.
4. **What stands in for an instance above a cap, and how a frame knows what it holds.** A flag per instance on the GPU costs every frame of every scene for a feature most scenes never use.
5. **How a refusal reaches a person.** As an error alone it is retried; as a picture it is the very thing refused.

## Decision

1. **A class is declared on the asset, in its provenance sidecar.** The field is `content_classes` of `<stem>.provenance.json`, made a schema type (`engine.content.ContentClasses`: category to level, optionally per material). Absent means `none` in every category. The engine's three categories are compiled in, and a game adds its own in a categories file its content build is given. A category neither knows is refused by the build; a level a category does not have reads as the category's highest. One sidecar serves every source format, because an image has no `extras`.

2. **A document record may raise its asset's class and never lower it** (*recommended, not decided*, question 1). `MeshPlacement` version 2 carries `content_classes`, and the effective class is the per-category maximum of asset and record. `doc.place_asset` and `editor.import_mesh` copy the asset's class onto the record. **A write that lowers or removes a record's class is refused unless the caller's role is marked `human`** (1008, `"content class"`). A raise only narrows what is shown, so it is open to any role that may write the record.

3. **A built thing records its class, and the class is part of what a cache entry answers.** Section 53 (`ContentClasses`) of a container and section 6 of a `.tex` hold it, **written only when some level is above `none`**. The class is mixed into `cluster_cache_key` and `texture_cache_key` **only when not all `none`**. So unclassified content keeps its bytes and its keys (no golden, pin or cache version moves), and a source that gains a class gets a new key: a build made before the class existed is never found for it. A node built from several inputs carries their per-category maximum.

4. **A view policy lives in one file the operator names on the command line of a host the operator starts, and in nothing else.**
   - **The file and the flags**: `engine.content.ViewPoliciesFile`, named policies; `--view-policies <file>` and `--view-policy <name>` on engine-host, engine-editor and engine-view, read once at start.
   - **The compiled-in default**: `agent-default` (ADR-0033 decision 7) is what any host runs that was not told otherwise.
   - **Kept out of a client's reach**: the start refuses a file inside the document's directory or inside any root a client may name (T174). No protocol method writes a policy, names its file or returns its path. A role has no policy field.
   - **Credentials**: a credential names its policy when the operator issues it (A4), and a credentialed connection runs under that policy or `agent-default`, never the host's own.
   - **The forwarders**: engine-cli and engine-mcp forward no policy, so a host either starts runs `agent-default`.
   - **Narrowing**: a call may narrow for itself (`view_policy` on the render calls and `doc.blob`), never widen. `engine.view_policy` (read-only) answers the caller's effective policy and where it came from.

5. **Substitution is decided per mesh when the scene is loaded, before the mesh's geometry is read.** `load_scene` reads the class from the section table it already reads (or the sidecar the cache key needs) and calls one function, `content_class::decide`.
   - **`omit`**: the instance is not made, and its geometry and textures are never read into the loaded scene.
   - **`proxy`**: the instance is made from the engine's unit box at the mesh's bounds, with a neutral material.
   - **`cover` and `obscure at capture`**: not built until the character work (C11, C12) builds slots and section masks; a policies file naming either is refused at start.
   - **The upper bound**: the loaded scene keeps `held`, the join of what it made, and the policy it was made under. Later admissions join into `held`, and nothing lowers it while the scene is loaded, so it bounds every frame and every capture.
   - **Nothing on the GPU**: no GPU struct, shader or per-frame work changes.
   - **Not the world**: the runtime world materializes every record whatever its class, because the world is the source of truth and not an output, and its state hash is the same under every policy.
   - **The default substitutions** for `agent-default` are `proxy` for `nudity` and `sexual` and `omit` for `violence` (*recommended, not decided*, question 2).

6. **Every capture names its policy**: the capture, evaluate and benchmark results carry `view_policy`, `held` and `substituted`; every PNG the capture path writes carries them in a `tEXt` chunk; engine-view's summary and a packaged game's report carry them too.

7. **What is above the caller's policy is refused with 1011 `ContentAboveViewPolicy`**, naming the policy, the category, the level and the cap, never the content.
   - **What is refused**: a single-mesh load above the cap; a capture or evaluation of a scene whose `held` exceeds the caller's policy; a kept capture fetched by a connection whose policy does not cover it (403, no body); a comparison of an image above the cap; a blob above the cap.
   - **The item**: each refusal makes the host raise a `classified content review` item for a person (A23). It carries the subject's identity and version, assets by portable path, and numbers only. It is attributed to the caller and recorded as the host's, so a caller that may only read cannot lose it. It is deduplicated by subject, version and category.
   - **Who closes it**: a role marked `human`, after a person has looked through a host or credential whose policy covers it. The MCP tools treat 1011 as a handled outcome and never retry it.

8. **Reads of the document return a record's properties unchanged, with its effective class and `above_policy` beside them** (*recommended, not decided*, question 3). The authoring rule is that a classified asset is named neutrally.

9. **The vocabulary is one module, `foundation/content_class`, and it is not a capability.** `ENGINE_MINIMAL` and every `ENGINE_WITH_` switch leave enforcement in, because a build switch is what ADR-0033 forbids as the mechanism.

### Questions for the owner (recommended, not decided)

1. **Records may raise a class, and only a human role may lower one.** Recommended. The alternative is classes on the asset's sidecar alone: simpler, and a neutral asset can then never be classified by how it is placed.
2. **`agent-default` substitutes `proxy` for `nudity` and `sexual` and `omit` for `violence`.** Recommended, so that framing, silhouette and the place of a thing stay meaningful in an agent's picture. The alternative is `omit` for every category, so that not even a box at the bounds reaches an agent's picture.
3. **Record properties are returned unchanged above the cap, with the class beside them.** Recommended, because a review needs the subject's identity and diffs and merges need the text. The alternative withholds a record's string properties above the caller's cap: safer, at the cost of both.

## Consequences

- Unclassified content, which is all the content this repository has, keeps its container bytes, texture bytes, cache keys, golden hashes and determinism pins. A classified asset costs a section, a key of its own, and a duplicate texture entry where it shares an image with a neutral one.
- A frame costs nothing new. A load costs a section lookup per container and, for a glTF source, the sidecar open the key needs. The build row measures it on a quiet machine.
- A scene loaded by one caller is captured for another only within the other's policy. A caller with less reloads under its own, which costs a load rather than a leak.
- A benchmark of a substituted scene says it measured a different scene.
- An agent learns what it cannot see (`engine.view_policy`, 1011, `substituted`, each record's class) without seeing it.
- **Forbidden**: a class read only from the document; a cached build found for a source by its bytes alone once that source has a class; a policy in the roles file, the document, a root, or any method's params beyond a narrowing; engine-cli or engine-mcp forwarding a policy; a credentialed connection falling back to the host's policy; a GPU-side test of class; a refusal that names the content or carries a picture; a retry of a 1011.
- **Still a guard rail, not a wall**, against an agent with file-system access to the host, as ADR-0033 decision 4 says. Two limits are stated rather than hidden: a container built before this decision from content already classified then has no section; and the operator's file and command line can be edited by anything that can edit the operator's files.
- Six build rows, CPU-tested, in order ([content classes](../content-classes.md#the-build-in-rows)).

## Revisit when

- `cover`, or substitution per material or per section, is built (C11, C12): decision 5's per-mesh rule then narrows to the part.
- A host serves a client over something other than loopback (the credentials decision's own trigger, A4), which makes a credential's policy a security boundary rather than a guard rail.
- The owner answers one of the three questions.
- Load-time measurement shows the sidecar open costs more than a percent of a many-mesh load; then the class moves into a manifest the content build writes beside the derived data.
