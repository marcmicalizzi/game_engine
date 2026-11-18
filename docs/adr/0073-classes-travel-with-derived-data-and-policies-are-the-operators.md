# ADR-0073: Classes travel with derived data, and a view policy is the operator's file, decided at load

- **Status:** Proposed
- **Date:** 2026-11-18 (revised the same day after a review of the first pass and of its first module)
- **Plan references:** docs/plan/06-agent-tooling.md §6.6 and §6.10, docs/plan/07-content-pipeline.md §7.5 and §7.11, docs/plan/12-ai-usage-policy.md §12.6; roadmap C3 and A26. Builds on [ADR-0033](0033-content-classes-and-view-policies.md), which it does not change, on [ADR-0072](0072-the-task-ledger-beside-the-document.md) (the ledger's person's review item), and on two decisions proposed the same day and not yet merged: roadmap A4's (a connection is who its credential says, and a role a host cannot enforce fails closed) and roadmap T174's (a host reads and writes a client's paths only under the roots its operator named).
- **Docs touched:** `docs/content-classes.md` (the design), `docs/subsystems/content_class.md`, `docs/subsystems/protocol.md` ("View policies on captures, reads and review items"), `docs/subsystems/assets.md` ("Content classes"), `docs/subsystems/renderer.md` ("Content classes at load"), `docs/plan/06-agent-tooling.md` §6.10, `docs/roadmap.md` (C3, A26, C61 to C66), `AGENTS.md` (the rule on view policies)

## Context

[ADR-0033](0033-content-classes-and-view-policies.md) decided the vocabulary: a content class is a (category, level) pair; a view policy caps each category and names a substitution; the substitution happens when content is instantiated for a view; a session's policy is the host operator's, and a client cannot raise it. Nothing of it is built. Today the engine knows nothing about any content's class at any of the twenty-four ways content leaves it ([content classes](../content-classes.md#every-way-content-leaves-the-engine)). The generation tools write `content_class: general` and a flat map of the engine's categories at `none` into their sidecars, and nothing reads either.

Five things were left open, and each has a wrong answer that is easy to build:

1. **Where a class is written.** On the document record alone, any agent's edit could declassify an asset. On the source alone, a neutral asset could not be classified by how it is placed.
2. **How it survives derived data.** The cache is keyed by the source's bytes. A class kept beside the source and not in the key is shed the first time a build made before the class existed is found under the same key. And a container's key leaves out the images it names by path, on purpose, so a class carried only in the container is shed by an image that changes after the build.
3. **Where a policy lives.** In the roles file, which is in the repository and edited in ordinary work, or behind a protocol method, an agent could change its own policy.
4. **What stands in for an instance above a cap, and how a frame knows what it holds.** A flag per instance on the GPU costs every frame of every scene for a feature most scenes never use.
5. **How a refusal reaches a person.** As an error alone it is retried; as a picture it is the very thing refused.

A review of the first pass found that it said more than it built in five places: a texture's class and a record's raise never reached the decision, a raise could be shed by an edit that was not a lowering, a capture's check could be passed by content admitted after it, and "no agent changes a view policy, by construction" was false for an agent that starts a host itself. This revision closes each; the design page lists them with their tests.

## Decision

1. **A class is declared on the asset, in its provenance sidecar.** The field is `content_classes` of `<stem>.provenance.json`, made a schema type (`engine.content.ContentClasses`: category to level, optionally per material, the per-material levels joined into the asset's while v0 decides per mesh). Absent means `none` in every category.
   - **One reader** in `foundation/content_class` serves the build, the load and the store, and reads both the schema's form and the flat map the generation tools write today.
   - **What cannot be read is never `none`**: a sidecar that is not JSON, holds a key twice, is of the wrong shape or type, or names an unknown category refuses the build; at load, such a class is the highest level of every category the reader knows, and the reader writes that class into its output whenever it refuses.
   - **Categories travel with the data.** The engine's three are compiled in. A game adds its own in a categories file its content build is given, and every built thing that records a class records each category's ladder by name beside it, so no host is given a categories file. A category name is `[a-z0-9_]`; data naming a known category with a different ladder is unreadable; a level a category does not have reads as the category's highest.

2. **A document record may raise its asset's class and never lower it** (question 1, **decided by the owner, 2026-11-18**). `MeshPlacement` version 2 carries `content_classes`, and the effective class is the per-category maximum of asset and record. `doc.place_asset` and `editor.import_mesh` copy the asset's class onto the record. **A transaction is refused (1008, `"content class"`) when the composed class of any placement it touches falls, unless the caller's role is marked `human`**: compared through the whole layer stack, before and after every transaction whatever produced it (an apply, an undo, a redo, a merge's result applied, a rollback, a promotion, a change to the layer stack), with a deleted placement counted as all `none`. A raise only narrows what is shown, so it is open to any role that may write the record.

3. **A built thing records the class of exactly what its key covers, and the class, by name, is part of what a cache entry answers.**
   - **Where**: section 54 (`ContentClasses`) of a container holds its source's own class; section 6 of a `.tex` holds its image's own class. Each holds the ladders by name, and **is written only when some level is above `none`**.
   - **The keys**: the class is mixed into `cluster_cache_key` and `texture_cache_key` **only when not all `none`**, as sorted (category, level) names with the ladders, never indexes. So unclassified content keeps its bytes and its keys (no golden, pin or cache version moves), and a source that gains a class gets a new key.
   - **What a key must not hold**: a container's key holds no external image's, pack set's or record's class, and a texture's no mesh's.
   - **What is joined at load instead**: whatever a key leaves out. A node built from inputs its key covers (an impostor, a tile set, a merged mesh) carries their per-category maximum.

4. **A view policy lives in one file the operator names on the command line of a host, and in nothing a client can reach.**
   - **The file and the flags**: `engine.content.ViewPoliciesFile`, named policies that cap categories by name, a game category's cap resolved when data brings its ladder and `none` when the ladder lacks the level; `--view-policies <file>` (once) and `--view-policy <name>` (an unknown name refuses the start) on engine-host, engine-editor and engine-view, read once at start.
   - **The compiled-in default**: `agent-default` (ADR-0033 decision 7) is what any host runs that was not told otherwise.
   - **Kept out of a client's reach**: the start refuses, by canonical path, a file inside the document's directory or any root a client may name (T174), one reached through a link, and one with a second hard link, and checks again at each document opened. No protocol method writes a policy, names its file or returns its path; no client-named write lands on a `*.provenance.json`. A role has no policy field.
   - **Credentials**: a credential names its policy when the operator issues it (A4), and a credentialed connection runs under that policy or `agent-default`, never the host's own.
   - **The forwarders**: engine-cli and engine-mcp forward no policy, so a host either starts runs `agent-default`.
   - **Narrowing**: a call may narrow for itself (`view_policy` on the render calls and `doc.blob`), never widen. `engine.view_policy` (read-only) answers the caller's effective policy, its source and its file's hash.
   - **Against a protocol client this is a wall; against an agent with the host's file system, which an agent that starts a host has, it is a guard rail and an audit trail, as ADR-0033 decision 4 says.** The engine does not try to tell an agent from a person. The rail is the repository's rule (an agent never passes a view-policy flag to a binary it starts and never writes a policies file outside a test's scratch) and a lint that refuses the flags and a policies-shaped file in what is committed. The trail is decision 6.

5. **Substitution is decided per instance when it is admitted, before its mesh's geometry is read.** The class decided on is the join of the mesh's own (the container's section, or a glTF's sidecar), every texture it samples (each image's sidecar read at load beside the image, its `.tex`, a pack set's entry) and the instance's raise. One function, `content_class::decide`, decides it.
   - **One door**: every instance that reaches a loaded scene, at the load or after, comes through one admission function, and a live edit to a placement's class or mesh re-decides it through the document's change feed.
   - **`omit`**: the instance is not made, and its geometry and textures are never read into the loaded scene for it.
   - **`proxy`**: the instance is made from the engine's unit box at the mesh's bounds, with a neutral material.
   - **`cover` and `obscure at capture`**: not built until the character work (C11, C12) builds slots and section masks; a policies file naming either is refused at start.
   - **What the scene remembers**: the policy it was made under, an append-only table of each distinct class it decided with the outcome, and `held`, the join of what it admitted.
   - **Nothing on the GPU**: no GPU struct, shader or per-frame work changes.
   - **Not the world**: the runtime world materializes every record whatever its class, because the world is the source of truth and not an output, and its state hash is the same under every policy.
   - **The default substitutions** for `agent-default` are `proxy` for `nudity` and `sexual` and `omit` for `violence` (question 2, **decided by the owner, 2026-11-18**). A category `agent-default` does not name, a game's own, is capped at `none` as ADR-0033 decision 7 says and substituted by `omit`, the stricter of the two: the owner decided the engine's three, and a game's category is one nobody has judged a box at the bounds to be harmless for.

6. **Every output names its policy, where the policy came from, and the policies file's hash**: the capture, evaluate and benchmark results carry `view_policy` (name, source `compiled-in`, `start line` or `credential`, caps, substitutions, file hash), `held` and `substituted`; every PNG the capture path writes carries them in a `tEXt` chunk; engine-view's summary and a packaged game's report carry them too. No path is recorded. **A host started under a policy wider than `agent-default` says so** on stderr at start, in its start line or summary, and in every result and chunk; a connection under a credential naming a wider policy is logged when it connects.

7. **What the caller's policy would not have made is refused with 1011 `ContentAboveViewPolicy`**, naming the policy, the category, the level and the cap, never the content.
   - **What is refused**: a single-mesh load above the cap; a capture or evaluation of a scene unless the caller's policy makes the same decision as the scene's for every class in its table (admits what was admitted, proxies what was proxied, omits what was omitted), checked before the first frame and again after the last is recorded; a kept capture fetched by a connection whose policy would not (404, the same answer as no such capture); a comparison of an image above the cap; a blob above the cap.
   - **The item**: each refusal makes the host raise a `classified content review` item for a person (A23). It carries the subject's identity and version, assets by portable path, and numbers only. It is attributed to the caller and recorded as the host's, so a caller that may only read cannot lose it. It is deduplicated by the subject's identity, the build key and the category, naming the latest version.
   - **Who closes it**: a role marked `human`, after a person has looked through a host or credential whose policy covers it. The MCP tools treat 1011 as a handled outcome and never retry it.

8. **Reads of the document return a record's properties unchanged, with its effective class and `above_policy` beside them** (question 3, **decided by the owner, 2026-11-18**). The authoring rule is that a classified asset is named neutrally.

9. **The vocabulary is one module, `foundation/content_class`, and it is not a capability.** `ENGINE_MINIMAL` and every `ENGINE_WITH_` switch leave enforcement in, because a build switch is what ADR-0033 forbids as the mechanism.

### What the owner was asked, and what they chose (2026-11-18)

The first pass left three of these choices to the owner. All three were answered the same day, each as proposed.

1. **May a record raise a class, with only a person's role lowering one?** The alternative was classes on the asset's sidecar alone: simpler, and a neutral asset could then never be classified by how it is placed. **Chosen: "Yes, as proposed"** (decision 2).
2. **What does `agent-default` put in place of content above a cap?** Proposed: `proxy` for `nudity` and `sexual` and `omit` for `violence`, so that framing, silhouette and the place of a thing stay meaningful in an agent's picture. The alternative was `omit` for every category, so that not even a box at the bounds reaches an agent's picture; the module was built with it until the answer came. **Chosen: "Box, as proposed"** (decision 5).
3. **Are record properties returned unchanged above the cap, with the class beside them?** Proposed, because a review needs the subject's identity and diffs and merges need the text. The alternative withheld a record's string properties above the caller's cap: safer, at the cost of both. **Chosen: "Text, as proposed"** (decision 8).

## Consequences

- Unclassified content, which is all the content this repository has, keeps its container bytes, texture bytes, cache keys, golden hashes and determinism pins. A classified asset costs a section, a key of its own, and nothing for the images it shares with neutral ones, since a texture's class is its image's alone.
- A frame costs nothing new. A load costs a section lookup per container, for a glTF source the sidecar open the key needs, and a sidecar probe per image named by path beside the read the load already makes. The build row measures them on a quiet machine.
- A scene loaded by one caller is captured for another only when the other's policy would have made the same scene. A caller with another policy reloads under its own, which costs a load rather than a leak or a picture that misstates its policy.
- A benchmark of a substituted scene says it measured a different scene.
- An agent learns what it cannot see (`engine.view_policy`, 1011, `substituted`, each record's class) without seeing it.
- An agent cannot undo its own raise, delete a classified placement or roll back past a raise; each is a person's edit.
- **Forbidden**:
  - a class read only from the document;
  - a cached build found for a source by its bytes alone once that source has a class;
  - a key holding an index where a name belongs;
  - a container's key holding an external image's class;
  - a refused read leaving `none` behind it;
  - a policy in the roles file, the document, a root, or any method's params beyond a narrowing;
  - engine-cli or engine-mcp forwarding a policy;
  - a credentialed connection falling back to the host's policy;
  - a GPU-side test of class;
  - a way into a loaded scene other than the admission function;
  - a refusal that names the content or carries a picture;
  - a retry of a 1011;
  - a claim that an agent's start line can be told from a person's.
- **Still a guard rail, not a wall**, against an agent with file-system access to the host, as ADR-0033 decision 4 says. Four limits are stated rather than hidden: a container built before this decision from content already classified then has no section; an engine binary built before the load decides skips the section; an image copied without its chunk and with no sidecar compares as `none`; and the operator's file and a host's command line can be written by anything that can write files and start processes, which the trail of decision 6 records and does not prevent.
- The build is six rows, CPU-tested, in order, two of them two legs each ([content classes](../content-classes.md#the-build-in-rows)).

## Revisit when

- `cover`, or substitution per material or per section, is built (C11, C12): decision 5's per-instance rule then narrows to the part.
- A host serves a client over something other than loopback (the credentials decision's own trigger, A4), which makes a credential's policy a security boundary rather than a guard rail.
- An agent's picture under `agent-default` is found to tell more than the owner meant by a box at the bounds (question 2's answer), or a game asks for a substitution of its own for a category of its own.
- Load-time measurement shows the sidecar probes cost more than a percent of a many-mesh load; then the classes move into a manifest the content build writes beside the derived data, keyed like it.
