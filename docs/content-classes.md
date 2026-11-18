# Content classes and view policies: the design (C3, A26)

**Designed 2026-11-18, not built.** [ADR-0033](adr/0033-content-classes-and-view-policies.md) decided what an output of the engine may contain: a **content class** on the content, a (category, level) pair per category; a **view policy** on the output, a cap and a substitution per category; and substitution at the earliest point where the two meet, so that classified data never reaches a restricted view's render. It left open where a class is written, how it survives derived data, where a policy lives and how a host picks one, what stands in for an instance above a cap, and how a refusal reaches a person. [ADR-0074](adr/0074-classes-travel-with-derived-data-and-policies-are-the-operators.md) (proposed) decides those. This page is the design the build rows read: the ways content leaves the engine today, roadmap C3 (classes on assets), roadmap A26 (policies on captures, reads and reviews), what it costs, and the rows that build it.

Three rules hold throughout, and every row below is checked against them.

- **Nothing classified is needed to build or test it.** Every example and every test uses neutral fixtures: a cube that carries a made-up class, under a made-up game category (`fixture_glow`, levels `none`, `dim`, `bright`) or under an engine category it does not depict. No row opens a classified asset, an authoring folder or an image of a body.
- **No agent changes a view policy, by construction.** A policy lives in one file the operator names on a host's command line. It is outside the document, outside every root a client may name, unread and unwritten by any protocol method, and reached by no MCP tool ("Where a policy lives", below, says how each is kept away from it).
- **The engine stays general purpose.** The engine knows three categories and their ladders and nothing about what a game depicts. A game adds categories as data. Nothing keys on a generator's output, a file name or a mesh's shape: a class is what an asset declares, and an asset that declares nothing is `none` in every category.

## Every way content leaves the engine

Twenty-four exits, as of `main` on 2026-11-18 with the batch of that day's branches (credentials, roadmap A4; client-named paths, T174; the ledger, A23). "Known today" is what the engine knows about the content's class at that point. It is nothing everywhere, because no class is recorded anywhere yet: `tools/generate.ps1` and `tools/blender-run.ps1` write `content_class: general` into their sidecars and refuse anything else, and nothing reads it. "ADR-0033 asks" is the decision that applies, and "Here" is the section below that answers it.

| # | Exit | What leaves | Known today | ADR-0033 asks | Here |
|---|---|---|---|---|---|
| 1 | `render.capture`, `color` | a PNG under a writable root, or kept in memory | nothing | built restricted for the view; the policy in the capture's provenance (decisions 2, 4) | instantiation, provenance |
| 2 | `render.capture`, `ids`, `depth`, `normals` | three more files, and the ids header | nothing | diagnostics obey the colour's policy (decision 5) | instantiation |
| 3 | `render.capture` with `integrator: reference` | the path tracer's picture of the same scene | nothing | as 1 | instantiation |
| 4 | `GET /capture/<id>[/ids]` on the listener | a kept capture, to any connection that authenticated and knows the id | nothing; served whoever made it | a capture is seen under the viewer's policy (decision 4) | refusals |
| 5 | the browser shell's viewport | a stream of kept captures (4) | nothing | as 4 | refusals |
| 6 | `render.evaluate` | the real-time, reference and difference pictures | nothing | as 1 | instantiation |
| 7 | `render.compare` | a FLIP heat map of two images a client names | nothing about either image | a picture derived from pixels is those pixels (decision 5) | refusals |
| 8 | `render.benchmark`, `render.progress` | numbers, measured on whatever the load instantiated | nothing | numbers are allowed (decision 5); a number of a substituted scene must say so | provenance |
| 9 | `engine-view`'s window | the operator's picture | nothing | the operator's policy (decision 3, uses 1 and 2) | where a policy lives |
| 10 | `engine-view --capture`, `--offscreen`, its JSON summary | a PNG and a line of JSON | nothing | as 1 | provenance |
| 11 | `engine-view --benchmark`, `tools/flythrough.ps1`, `tools/frame-budget.ps1` | per-frame numbers, and the pictures a run takes | nothing | as 8 and 10 | provenance |
| 12 | `engine-editor`'s native viewport | the operator's picture of the document | nothing | the operator's policy | where a policy lives |
| 13 | the packaged game (`tools/package-game.ps1`) | `engine-view` on a tester's machine, and `report.json` back | nothing | a distribution's policy (decision 3, use 1) | where a policy lives |
| 14 | engine-mcp's `capture`, `evaluate`, `compare` tools | files in the workspace, resource links in the tool result | nothing | as 1; a refusal is a normal outcome and is not retried (decision 7) | refusals |
| 15 | `tools/e10-harness.ps1`, `tools/hdr-measure.ps1`, the GPU tests' outputs | pictures of fixtures and generated meshes | nothing | as 10 | where a policy lives |
| 16 | visual mips (plan 06 §6.7, not built) | capture cameras per tile, into derived data | not built | as 1, and the cached picture keyed by its policy | instantiation |
| 17 | `doc.blob` with `data` | a blob's bytes, base64, to every role (a read) | nothing | an asset above the cap is refused with a structured error (decision 4) | refusals |
| 18 | `doc.get`, `doc.objects`, `doc.diff`, `doc.journal`, `doc.describe`, `doc.query` | record properties: names, paths, numbers | nothing | numbers and identities are allowed (decisions 5, 6); the class should be visible | reads of the document |
| 19 | `render.load`, `render.scenes` | a scene's meshes by name, its counts | nothing | as 18 | provenance |
| 20 | `content.build`, `engine.run_tests`'s content checks, `engine-content info` and `stats` | numbers about built assets; `content.build` also writes a container where a client names | nothing | validators' numbers are the way classified content is checked (decision 6) | carried through derived data |
| 21 | `session.events`, `session.materialize`, `session.run_headless`, `session.state_hash` | ids and component values of the runtime world | nothing | the source of truth is never redacted (decision 6) | the world |
| 22 | `log.tail`, every host's stderr, `profile.report` and its trace | log fields, zone names, times | nothing | numbers and ids only | logs and reports |
| 23 | `review.list`, `task.list` | summaries, subjects, reports | the ledger forbids a picture | the item carries numbers and identity, never a picture (decision 6) | refusals |
| 24 | `session.save_game` | a copy of the document directory | nothing | the class travels with the copy | carried through derived data |

**The three most exposed.** `render.capture` (1 to 3, and through it 14): the agent's own picture, every channel, written where the agent reads it, with nothing known about what is in the scene. `doc.blob` with `data` (17): a read, so open to every role, that hands back any blob's bytes, a mesh or an image as readily as a layer file. `GET /capture/<id>` (4): with credentials (A4) a host serves connections of different standing at once, and a capture one of them kept is served to any other that has the id.

## C3: classes on assets

### Where a class is declared, and by whom

**On the asset, in its sidecar.** A source asset's class is written in the provenance sidecar beside it, `<stem>.provenance.json`, the `engine.generation.provenance/1` file `tools/generate.ps1` already writes, or a minimal one a person writes by hand (`schema`, `name`, `content_classes`). Its `content_classes` field, already present, becomes a schema type, `engine.content.ContentClasses`: a map from category to level, plus an optional `materials` map from a material's name to its own classes, for an asset whose classified part is one material. The author declares it: a person, or a game's tooling under that person's review. It is not the document's, because the document is what agents edit all day.

- **Absent is `none`** in every category, as ADR-0033 decision 1 says. A Khronos sample, a generated prop and a procedural mesh declare nothing and are `none`. The engine cannot detect classified content, and does not pretend to: a game that ships it declares it, the way it declares a licence.
- **A category is the engine's or the game's.** The engine's are `nudity` (`none`, `partial`, `full`), `sexual` (`none`, `suggestive`, `explicit`) and `violence` (`none`, `mild`, `graphic`), compiled in. A game adds its own in a categories file of its content (`engine.content.ContentCategoriesFile`: each category's levels, lowest first), which the content build is given. **A category neither knows is refused by the build**, because a level that cannot be ordered cannot be capped. **A level the category does not have is read as the category's highest**: an unknown level fails closed.
- **One sidecar for every source format.** A glTF could carry the class in `extras` and a PNG cannot, so the sidecar is the one place for meshes, images, audio and material sets alike. A material pack's set declares it in its `pack.json` entry (none of the shipped sets do).

**On the record, raising only.** A `MeshPlacement` record (`MeshPlacement` version 2, `content_classes: ContentClasses?`) may **raise** the class of the asset it places: a neutral asset placed in a way that makes it classified, such as a material override or a pose. Its effective class is the per-category maximum of the asset's and the record's, so a record can never lower an asset's class. `doc.place_asset` and `editor.import_mesh` copy the asset's declared class onto the record they make, so the record says what it places without opening the sidecar. **A write that lowers any category of a record's `content_classes`, or removes the property, is refused unless the caller's role is marked `human`** (1008, `data.reason` `"content class"`). Otherwise an agent's edit could take back a class a person set. A raise is open to every role that may write the record, because a raise only narrows what is shown. *Recommended, not decided* (question 1 of ADR-0074): the alternative is classes on the sidecar alone, which is simpler and cannot classify a neutral asset by context.

### How it travels through derived data

A built thing records its class, and **a cached build cannot shed one**:

- **A container** (`.clusters`) gets section **54, `ContentClasses`** (53 is the door leaves'): 16-byte records `{category, level, scope, reserved}`, where the category and level are offsets into the container's strings and the scope is `0xFFFFFFFF` for the whole asset or a material index. **It is written only when some level is above `none`.** So every container built today, and every unclassified one built later, is byte-identical to what it is now. No golden hash, no determinism pin and no `k_cluster_cache_version` moves.
- **The cache key holds the class**, the way `source_mesh_hash` already mixes a tag into the key of a file that states `KHR_materials_diffuse_transmission` ([assets](subsystems/assets.md)): `cluster_cache_key` mixes in a hash of the canonical class vector **only when it is not all `none`**. An unclassified source keeps its key. A source whose sidecar gains a class gets a new key, so the cached build made before the class existed is never the answer to it. This is what "cannot shed a class" means: the class is part of what the entry is the answer to, not a label beside an answer found by the source's bytes alone.
- **A texture** (`.tex`) gets section **6, `ContentClasses`**, the same records with no material scope. Its class is the maximum of its image's own sidecar and the class of every material in the build that samples it, and it is mixed into `texture_cache_key` only when not `none`. The same image bytes used by a neutral asset and a classified one are then two entries. That wastes space only for classified content and keeps a neutral entry neutral.
- **A node built from several inputs carries the per-category maximum of them all** (the join): a container over its source and its images, a tile set over its forms, a far-forest impostor over the meshes it was baked from, a merged scene mesh, a generator's output over the material sets it drew from. An engine generator declares nothing of its own, so its output is the join of its inputs.
- **The identity** an incremental build trusts (`read_cluster_file_identity`) holds the key, so `engine-content build-all` rebuilds an entry whose sidecar changed. A copy of a document (`session.save_game`, exit 24) copies the sidecars that sit in it, and the records carry their own raise.
- `engine-content info` prints the classes, and `content.build`'s result reports them, as data. Numbers about classified content are allowed (exit 20).

**A container named directly** (`--mesh x.clusters`, a scene naming a `.clusters`) is what its section says, joined with a sidecar beside it if there is one. A container built before C3 from a source that was classified even then has no section. That is the one way to hold classified content with no class, and it is a guard rail's limit, stated as such: such a file was built by a person's tool before the rule existed.

### What "decided at instantiation" means

**In the renderer, per mesh, at load, before its geometry is read.** `SceneDesc` gains `view_policy` (`content_class::ViewPolicy`, a few bytes), and so does `PlacementMeshOptions`, so a mesh built after the load, such as a world tile's, is decided the same way. For each mesh, `load_scene` first reads the class:

- from the container's section table, which it reads anyway (one more kind looked up, nothing read when it is absent);
- or, for a glTF source, from the sidecar the content build reads to compute the key.

Then it decides, through one function (`content_class::decide(classes, policy)`), for every instance of that mesh:

- **within the cap**: loaded and drawn as today;
- **`omit`**: the instance is not made. Its mesh's geometry and textures are never read into `SceneData`, never uploaded, and never in the cull pass, the acceleration structures, the shadow maps or any channel;
- **`proxy`**: the instance is made from the engine's own unit box, scaled to the mesh's bounds (from the container's header and LOD section, numbers only) and placed by the instance's transform. It gets one neutral material, so silhouettes, framing and the place of the thing stay meaningful. Collision is not changed: the world is not an output (below).

`cover` (a mandatory garment or decal slot) and `obscure at capture` are **not built**. Both need the character work (roadmap C11, C12). A policies file naming either is refused when the host starts, rather than silently treated as `omit`. A substitution happens per mesh in v0, because a whole-asset class is what C3 records first. Per-material and per-section substitution, which drops a tagged layer or hides a section with the mask garments use, comes with C11 and C12. The per-material records of section 54 are kept for them.

**The scene knows the highest class it holds.** `SceneData` gains `held` (the join of the classes of every instance it made, after substitution, so within the policy by construction) and `instantiated_under` (the policy it was loaded with). `SceneLoadReport` gains `substituted`, a count per category and substitution. A mesh that arrives later (a tile, a placement's form) joins into `held` when it is admitted. Nothing lowers `held` while the scene is loaded, so it is an upper bound of every frame the scene draws, never a per-frame exact answer. A frame's highest class is the scene's `held`, and a capture's is the frame's.

**What a frame that holds nothing classified costs: nothing measurable, by construction.** No GPU struct changes (`gfx::InstanceDesc`, `ClusterDesc` and `MeshDesc` and their size tables stay), no shader changes, and no per-frame or per-instance work. The decision is per mesh at load. Its cost is one section-table lookup per container, and, for a glTF source, the one sidecar open the key needs. That second cost is the only new I/O, and the build row measures it on the load of a many-mesh scene with and without sidecars, on a quiet machine, against the load's own wall time. A capture adds one comparison of two class vectors of a few entries.

**In the world: nothing is decided, and that is the decision.** The runtime world materializes every record whatever its class. A policy affects outputs, never the source of truth (ADR-0033 decision 6), and a game's state must not depend on who is watching. A streamer-mode player and anyone else run the same simulation, and `session.state_hash` is the same under every policy (a test). What the world gives out is ids and component values (exit 21), never an asset's bytes.

## A26: the policy on captures, reads and reviews

### Where a policy lives, and how no agent reaches it

**One file, the operator's: `engine.content.ViewPoliciesFile`**, a list of named policies, each a cap and a substitution per category. A category a policy does not name is capped at `none` with `omit`. The engine compiles in one more, **`agent-default`**: ADR-0033 decision 7's `nudity: none`, `sexual: none`, `violence: mild`, a game's categories `none`. A host with no file has only that one, so **the default is what every host runs unless its operator says otherwise**. The default substitutions are a question for the owner (question 2 of ADR-0074, *recommended, not decided*): `proxy` for `nudity` and `sexual`, `omit` for `violence`.

How the operator names it, and why each way an agent works is kept away from it:

- **The command line of the hosts the operator starts**: `engine-host`, `engine-editor` and `engine-view` take `--view-policies <file>` and `--view-policy <name>` (the host's own policy, `agent-default` when absent). The file is read once, at start, and never again: no hot reload, so a change takes effect when the operator restarts the host.
- **Never under a root, never in a document.** The start refuses a policies file inside the open document's directory or inside any root a client may name (roadmap T174, readable or writable), the way it refuses a writable root that holds the shell's served directory. So no client-named path can read or write it.
- **No protocol method writes a policy, names its file or returns its path.** `engine.view_policy` (read-only) answers the caller's effective policy by name, caps, substitutions and source (`default`, `host`, `credential`), never the file. A per-call `view_policy` parameter on `render.load`, `render.capture`, `render.evaluate`, `render.compare` and `doc.blob` may **narrow** for that call (a per-category minimum with the effective one), never widen.
- **Not in the roles file.** `content/roles/roles.json` is in the repository and edited in ordinary work, so a policy is never a field of a role. A role says what a caller may change; a policy says what a caller may see; and the two are named separately.
- **A credential names its policy** (on top of A4's credentials): `--issue-credential … --view-policy <name>` records the name in the credentials file. A connection that presented a credential runs under that policy or `agent-default`, **never the host's own**, which fails closed exactly as A4's decision does for a role a host cannot enforce. The operator's own token and the shell's cookie run under the host's policy.
- **engine-cli and engine-mcp forward no policy.** Agents run both routinely, and engine-mcp's command line is written into an agent's client configuration. The host either one starts therefore always runs `agent-default`. An agent that is to see more is attached by the operator to a host the operator started, with a credential the operator issued.
- **What this is not**: a wall against an agent with file-system access to the host, which could edit the operator's file or a host's command line. ADR-0033 decision 4 says so, and it stays true. Against that agent the design keeps a guard rail and an audit trail: AGENTS.md's rule, a file outside the repository, and every capture naming its policy.

**Who may look at full policy** is the operator's statement, made twice: a policy wide enough, in the operator's file, and a credential or host that names it. Closing a person's review item takes a role marked `human` (A23). Seeing the content takes a policy that covers it. The two are separate on purpose, and a reviewer's credential carries both. Under ADR-0033 decision 7 an operator may also raise an agent's caps: the engine encodes no provider's terms, and does not forbid it.

### Every capture says what it was made under

`RenderCaptureResult`, `RenderEvaluateResult` and `RenderBenchmarkResult` gain `view_policy` (`RenderViewPolicy`: name, source, caps, substitutions), `held` and `substituted`. So a benchmark of a substituted scene says it measured a different scene (exit 8). Every PNG the capture path writes, the host's and `engine-view --capture`'s, carries the same as one `tEXt` chunk (`engine-view-policy`, compact JSON), because a picture separated from its result must still say it was restricted. `engine-view`'s JSON summary and a packaged game's `report.json` carry it too. The chunk changes a capture's bytes and not its pixels. The build row checks that every golden comparison in the tree decodes pixels and does not hash files.

### Refusals, and the review item for a person

**What is refused**, with **1011 `ContentAboveViewPolicy`**: `data` names the policy, the category, the level found, the cap, and the review item raised (below), and never the content.

- `render.load` of a single mesh (`mesh`) above the cap: a look at one asset, where a proxy of the whole subject would answer nothing. A scene is substituted, not refused.
- `render.capture` and `render.evaluate` of a loaded scene whose `held` is above the caller's effective policy. A scene loaded by one caller is drawn for another only within that other's policy, and a caller with less reloads the scene under its own.
- `GET /capture/<id>`: a kept capture records `held`. A connection whose policy does not cover it gets 403 and no body, and the id says nothing more.
- `render.compare` of an image whose class (its sidecar, or a capture's own chunk) is above the cap.
- `doc.blob` with `data` of a blob above the cap. `doc.put_blob` takes an optional `content_classes`, and the store keeps it beside the blob, joined on every later put of the same bytes and never lowered.

**What it raises.** The host raises a ledger item itself, through the ledger's own write (A23, `review.raise`'s path):

- **Kind and audience**: kind `classified content review`, audience `Human`.
- **Raiser**: attributed to the caller's actor, marked as raised by the host. The caller's role need not name `review.raise`, because the refusal is the host's own record and must not be lost because the caller may only read.
- **Subject**: the records, the blobs, and, in `ReviewSubject` version 2, `assets` as portable paths (a root's name and a path under it, never an absolute path), with the document's version the host stamps.
- **Report**: numbers only. The asset's identity (source hash, build key, the container's content hash), the class found against the cap, and the validator numbers the build has: triangles, clusters, bounds, texel density, the `content.*` checks.
- **Deduplication**: by (subject identity, version, category). An open item for the same thing is named again, not raised again, so a looping caller fills nothing.

**What an agent's session sees instead**: in a picture, the proxies or nothing; in the result, `held`, `substituted` and the policy; for a direct ask, the 1011 with the item's id; in `engine.view_policy`, what it may see; and in reads of the document, each record's class. MCP tools say in their descriptions that a 1011 is a handled outcome: the check has become a person's item and the work continues. The bridge never retries one or rephrases around it (ADR-0033 decision 7).

**How the item is closed.** A person opens it at full policy, through a host whose operator gave that person's credential a policy that covers it: `review.list`, then the subject captured or read there. The person closes it Accepted or Rejected under a role marked `human`. The raiser cannot withdraw it (A23 already holds both). Closing changes no class and no policy. A Rejected item says what must change, and the work is an asset's, a task's or a record's.

### Reads of the document

`doc.get`, `doc.describe` and `doc.query` return a record's effective class (the asset's joined with the record's raise) and `above_policy` for the caller. Record properties are returned as they are: the document is authored text and numbers, and a review needs the subject's identity. The authoring rule that goes with it is ADR-0033's own: a classified asset is named neutrally, as its base texture is authored neutral. *Recommended, not decided* (question 3 of ADR-0074): the alternative withholds a record's string properties above the caller's cap. That is safer, but it breaks diffs and merges for the caller, and the identity a review needs.

### Logs and reports

A log field, a profile zone name and a ledger report hold ids, paths and numbers, never an asset's bytes or a decoded image. That is already so, and the rule is written down so that it stays so. No mechanism beyond review enforces it.

## The build, in rows

Each row is one leg, CPU tests named, in dependency order: roadmap C61 to C66. C61 to C64 close C3; C65 and C66 close A26.

| Row | What | Depends on | CPU tests (no device) | Needs a device |
|---|---|---|---|---|
| C61 | `foundation/content_class`: `ContentClasses` (a small fixed vector of (category, level), size-tabled), the three engine ladders, a game's categories file, `join`, `narrow`, `within`, `ViewPolicy`, `ViewPoliciesFile` with `agent-default` compiled in, `decide`; the schema types in `schemas/provenance.schema`. **Not `OPTIONAL`**: a build switch is what ADR-0033 forbids as the mechanism, and `ENGINE_MINIMAL` must not turn enforcement off | C2 | ordering per category; join and narrow; an unknown category refused and an unknown level read as the top; `agent-default`'s values; each malformation of a policies file and of a categories file; `cover` and `obscure at capture` refused | none |
| C62 | the content build: the sidecar's `content_classes` read by `domain/content_build`; section 54 and `.tex` section 6 written only when not `none`; the class mixed into both cache keys only when not `none`; the join over inputs (images' sidecars, material sets, generators); `engine-content info` and `content.build`'s result | C61 | an unclassified cube's container and texture byte-identical to today's (the existing determinism pins unchanged); `fixture_glow: bright` on a cube gives a new key, section 54 and a classified `.tex`; a cube built neutral, then given a sidecar, rebuilt into a new entry; a mesh over a neutral and a `fixture_glow: dim` image joins to `dim`; an undeclared category refused | none |
| C63 | the record and the store: `MeshPlacement` version 2 `content_classes`; `doc.place_asset` and `editor.import_mesh` stamp it; the session refuses a lowering unless the role is human (1008 `"content class"`); `doc.put_blob`'s `content_classes` kept beside the blob, joined, never lowered; reads report the effective class | C62 | a raise allowed to `designer`; a lowering and a removal refused to `director` and allowed to a role of the test's own roles file marked `human` that may `doc.apply` (the shipped `reviewer` may not write records at all), the transaction rolled back whole; a placed cube's record carrying its asset's class; a blob's class joined across two puts | none |
| C64 | instantiation: `SceneDesc::view_policy`, `PlacementMeshOptions::view_policy`, the decision in `load_scene` before a mesh's geometry is read, the proxy box, `SceneData::held` and `instantiated_under`, `SceneLoadReport::substituted`, `doc_scene` and the editor's viewport through the same function, world tiles' meshes decided when admitted | C62 | `load_scene` (CPU) of three cubes, one `fixture_glow: bright`: under a policy capping it, `omit` leaves its clusters out of `SceneData::lod` and its textures unread, `proxy` adds one box at its bounds, `held` stays within the policy and `substituted` counts one; a scene with nothing classified loads the same tables as before; `session.state_hash` the same under two policies; the load-time cost measured on a many-mesh scene, quiet | the capture of the same scene: colour, ids, depth and normals show the proxy or nothing (for the merge's GPU run) |
| C65 | the policy in hosts: `--view-policies`, `--view-policy` on engine-host, engine-editor and engine-view; the start's refusal of a file under a root or the document; the credential's `view_policy` (A4); per-call narrowing; `engine.view_policy`; `view_policy`, `held` and `substituted` on every capture, evaluate and benchmark result, the PNG chunk, engine-view's summary and `report.json`; engine-cli and engine-mcp forwarding none | C64, A4, T174 | the effective policy for stdio, the operator's token, a credential with and without a policy, and a narrowing; a policies file under a root and under the document refused at start; engine-mcp's started host reporting `agent-default`; the PNG chunk round-tripped by `image` | a capture's result and chunk naming its policy |
| C66 | refusals and items: 1011; `render.load` of a single mesh, `render.capture` and `render.evaluate` above `held`, `GET /capture/<id>`, `render.compare`, `doc.blob`; the host-raised `classified content review` with `ReviewSubject` version 2 `assets`, deduplicated; the MCP tools' descriptions and the bridge's no-retry; `doc.describe` and `doc.query` reporting `above_policy` | C63, C65, A23 | `doc.blob` of a classified blob refused with 1011 naming the policy and the category, an item raised with its report and stamped version, a second refusal naming the same item, the item closed by `reviewer` and refused to `director`; `GET /capture/<id>` refused to a narrower credential's connection over a loopback listener (no device: a kept capture planted by the test); the bridge's handling of 1011 | the end-to-end refusal of a capture of a scene loaded under a wider policy |

**After these, not in them**: `cover` and `obscure at capture`, and substitution per material and per section (C11, C12, where the slots and the section mask are built); audio assets' classes; the editor's review queue showing a classified item as numbers (A24); a review item that blocks a promotion (A23's "not yet"); visual mips under a policy, keyed by it (A21).
