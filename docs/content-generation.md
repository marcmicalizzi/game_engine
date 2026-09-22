# Content generation: `tools/generate.ps1` and the E10 harness

The generator-service interface of [07 §7.7](plan/07-content-pipeline.md#77-ml-asset-generation-novelty-content) — `generate(kind, spec, constraints) → candidates[] with provenance`, pluggable backends, cached by spec hash — exists today as a **tool**, not yet as the C++ interface the content build will call. `tools/generate.ps1` drives the backends and writes every output beside a provenance sidecar; `tools/e10-harness.ps1` takes what came out through the content build and the renderer and says which of it the engine can use. Experiment E10 ([10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), [first pass](experiments/e10-generated-props.md)) is the first thing both were used for.

Read [12-ai-usage-policy](plan/12-ai-usage-policy.md) before extending either: the image and mesh models here are third-party services and local open-weights models used as published, and nothing they make — or anything an agent writes about it — is used to train anything.

## The rule that comes first: outputs are never committed

Generated images and meshes, the owner's ComfyUI workflow files, captures and containers built from generated meshes all live under the **local root** — `$env:ENGINE_LOCAL_ROOT`, else `D:\workspace\game_engine_local` — which is outside the repository on purpose. Both tools **refuse** a root or an output directory inside the repository (either this checkout or, from an agent worktree, the main checkout that contains it), because an output under the tree is one `git add .` from being committed. What *is* committed is text an engineer wrote: the tools, their tests, and subject lists such as [content/generation/e10-desert-props.json](../content/generation/e10-desert-props.json).

```
<local root>\
  workflows\                          the owner's ComfyUI UI exports (never committed)
  generated\<service>\<yyyy-MM-dd>\   <name>.<ext> beside <name>.provenance.json
  generated\meshy\ledger.jsonl        every Meshy submission, success, failure and batch balance
  generated\tripo\manifest-<date>.json, README.md, inbox\
  e10\<service>-<date>\               the harness's report.json, report.md, captures, heat maps
```

Services are `comfyui` (images), `comfyui-<workflow>` (meshes a ComfyUI image-to-3D workflow made — named after the workflow so two local 3D models' outputs of the same subject never collide), `meshy` and `tripo`. The file name is the subject's name in every service, so the same prop from three services is three files with one name in three folders, which is what a comparison joins on.

## Commands

| Command | What it does |
|---|---|
| `image -Workflow <ui.json> -Subjects <list.json>` (or `-Subject <text> -Name <name>`) | text → image through ComfyUI, holding the GPU lock for the batch |
| `3d -Backend meshy -Images <png\|dir>...` | image → mesh through Meshy's API, under the credit rules below |
| `3d -Backend comfyui -Workflow <image-to-3d ui.json> -Images ...` | image → mesh through any ComfyUI workflow (TRELLIS, Hunyuan3D, when the owner adds them) |
| `pipeline -Pipeline 'text->image->3d' -Workflow <image ui.json> -Backend meshy\|comfyui\|tripo-folder [-Workflow3d <ui.json>]` | both stages in one command; `tripo-folder` ends by writing the manifest |
| `manifest -Backend tripo-folder -Images ...` | the list the owner works from by hand in Tripo Studio |
| `ingest -Backend tripo-folder -Manifest <file>` | the GLBs the owner dropped in the inbox, each copied beside a sidecar |
| `balance` | Meshy's balance, and what this machine's ledger says it spent |
| `free` | ComfyUI: unload the models and free the memory |
| `prompt -Subject <text>` / `convert -Workflow <ui.json>` / `verify [-Path <dir>]` | the templated prompt; the API form a workflow converts to; re-hash every output against its sidecar |

Common flags: `-Date`, `-LocalRoot`, `-Only <name,...>`, `-DryRun` (plan and print, spend nothing, touch nothing), `-Force` (replace an output made from a different spec), `-Yes` (the Meshy spend confirmation). **stdout is one JSON line per result and nothing else**, so a script or an agent reads it; everything for a person goes to stderr. Exit 0 when every result is good, 1 when any failed (the lines say which) or when the command was refused before it started.

**Caching by spec.** Every output records a `spec_hash`: for an image, the workflow's SHA-256 and the whole API graph as sent (prompt, seed, size, every override) minus the date-bearing file prefix; for a mesh, the input image's SHA-256 and the request. Running the same command again is a cache hit and does nothing; asking for a *different* spec under a name that already exists is refused rather than silently replacing an asset, and `-Force` is how to mean it.

## ComfyUI workflows are data

A workflow is any ComfyUI **UI export** (the UI's "Save"; an "Export (API)" file is accepted too). The tool converts it to the API form `/prompt` takes, against the running server's `/object_info`, which is the authority on the node versions actually installed. It then finds the nodes it overrides **by title or by class, never by id**, so a new workflow needs no code — title its nodes and pass it:

| What | Where it goes |
|---|---|
| the prompt | the node titled **`Positive`** (`text`, or `prompt`) |
| the negative | the node titled **`Negative`**, when there is one; when there is none the sidecar records `negative: null` rather than a negative nothing used |
| the seed | every literal `seed`, `noise_seed` and `random_seed` input |
| the size | a `KreaDualResolutionSelector` (aspect, final and base megapixels), else every literal `Empty*LatentImage` width and height |
| the input image (3D) | uploaded through `/upload/image`, then set on the node titled **`Input Image`**, else the first `LoadImage` |
| the server-side file prefix | every `filename_prefix`: `Agentic/<date>/<name>` |
| anything else | `-Set '<title or class>.<input>=<value>'`, repeatable, JSON-typed |

Every override is recorded in the sidecar with the workflow's own value beside the new one, and the whole graph as sent is stored too, so an image is reproducible from its sidecar and the workflow file alone.

The conversion keeps what the first driver learned, as comments in the code and here: a widget input converted to a link keeps a **stale placeholder** in `widgets_values`, which is skipped so the widgets after it stay aligned; a seed widget is followed by a UI-only **control value** (`randomize`, `fixed`, ...) that is not an input; the UI's **`%date:...%` expands only in the browser** — through the API a literal `%` reaches the server's file system — so the tool expands the date itself; a `Note` is dropped, a `Reroute` is followed to its source, a `PrimitiveNode` becomes its literal, a **bypassed** node passes its input of the matching type through and a **muted** one is removed. One trap that is PowerShell's rather than ComfyUI's: a node with a single widget value must be read as a one-element array, or it is indexed one character at a time (`vae_name: "q"`); the test pins it. Subgraphs are refused with a sentence rather than half-converted.

Outputs are fetched through `/view`, not read off the server's disk, so the tool works wherever ComfyUI keeps its output directory (on this machine a network drive, `Z:\marc\ComfyUI\output\Agentic`, where they also stay).

## Image discipline

What an image-to-3D model reconstructs well is decided before it runs, by the picture it is given. The template (`image-to-3d/1`) wraps each subject so the image is: **one object, whole, centred and filling about 85% of a square frame**, on a **plain light-gray background**, in **soft even light with no cast shadow**, **with no text** and **no people**, from a three-quarter view slightly above. Each clause is there for a reason: a reconstruction guesses the unseen sides from what it can see, so a cropped or small object comes back cropped or blurred; a busy background has to be segmented away and sometimes is not; baked highlights and shadows get painted into the texture; text becomes texture garbage. `-Raw` passes a subject through untouched.

- **Sizes.** 1024×1024 for a workflow with a literal latent size (the turbo workflow, about 14 s an image on the RTX 5090), 1:1 at 2 MP for a `KreaDualResolutionSelector` (the two-stage workflow, 1456×1456, about two minutes). Decided by what the workflow contains, not by its name.
- **Seeds** are recorded always. Unless `-Seed` or the subject list gives one, a seed is derived from the subject's name (48 bits of its hash), so the same list regenerates the same images.
- **The GPU lock** ([bench](subsystems/bench.md#measuring-on-a-shared-machine), `D:\workspace\GPU-LOCK.md`) is taken once for the whole batch — not per image, which would let another agent's bake in between every fourteen seconds — with a 30-minute lease refreshed every ten minutes, and released in a `finally`. Waiting on it is normal; the lock tool says who holds it and why.
- **`/free`** (`{"unload_models":true,"free_memory":true}`) runs after every ComfyUI batch, inside the lock, so whoever takes the GPU next gets its memory back.

## Meshy, and the credit rules

`3d -Backend meshy` sends each image as a `data:` URI to `POST /openapi/v1/image-to-3d` with the body the owner's first task was verified with — `{"ai_model":"latest","should_texture":true,"enable_pbr":true,"topology":"triangle"}`, **30 credits** a task — and polls `GET .../image-to-3d/<id>`. Anything else in the body goes through `-Set <field>=<value>` and is recorded. The rules, all enforced by the tool rather than left to whoever runs it:

1. **The balance is read before every batch**, and the batch prints its task count and estimated cost before anything is submitted.
2. A batch that would take the balance below **`-MinBalance` (165)** or spend more than **`-MaxCredits` (700)** is refused before it starts; the same two limits are checked again before every submission, retries included.
3. A spend is confirmed: interactively with a question, non-interactively only with `-Yes`.
4. **Every task's `consumed_credits`** — which the task object reports itself — goes into its sidecar and into `generated\meshy\ledger.jsonl`, and the batch ends by reading the balance again and printing both numbers.
5. **A task id is on disk before its first poll** (`<name>.meshy-task.json`), so an interrupted run resumes the task rather than paying for it twice. A failed task is retried once (`-Retries 1`) within the limits and recorded in `<name>.meshy-failed.json`; nothing retries beyond that. This was tested by accident on E10's batch, which stopped after its fifth submission on a PowerShell 7.6 fault — **array-wrapping a generic `List[object]` (`@($list)`) throws "Argument types do not match"**, so the poll loop snapshots its list with `.ToArray()` — and the rerun resumed those five and paid only for the other fifteen.
6. **The key** is `$env:MESHY_API_KEY`, a user environment variable. The tool never takes it on the command line and never prints, logs or writes it.
7. **The download URLs are signed** and their signatures carry `~` and `&`: they are taken from the parsed JSON and handed over untouched (text extraction corrupted them twice on first contact), and the download is checked for the `glTF` magic before it replaces anything.

## Tripo, by hand

Tripo Studio's credits cannot be spent through its API, so the owner generates by hand. `manifest` writes `generated\tripo\manifest-<date>.json` — every input image with its SHA-256, the file name to save the result as, the settings to use, and an empty `recorded` block per entry for the model version, settings, credits and notes the owner can fill in — and a `README.md` beside it telling them which image to upload for which name and where to drop the GLBs (`generated\tripo\inbox\`). `ingest` copies each GLB it finds beside a sidecar carrying what the owner recorded, warns when an input image changed since the manifest was written, and reports the ones that are not there yet. It is how the comparison uses **byte-identical inputs** across services.

## The provenance sidecar: `engine.generation.provenance/1`

One JSON file per output, `<name>.provenance.json`. Its `asset_provenance` block is **exactly the schema type `engine.content.AssetProvenance`** ([schemas/provenance.schema](../schemas/provenance.schema), 07 §7.5) — the same fields, the same encodings, nothing else — so the content build will read it without translation; the test validates a sidecar against the JSON Schema the build generates from that file, and checks that a block with an extra field fails. Everything the record has no field for yet sits beside it:

| Field | What it holds |
|---|---|
| `schema`, `asset_id`, `name`, `kind`, `service` | `asset_id` is the primary output's content id (the first 128 bits of its SHA-256); `kind` is `image` or `mesh` |
| `spec_hash` | what makes two runs the same run (above) |
| `asset_provenance` | `generator` (`comfyui/<workflow>`, `meshy/image-to-3d`, `tripo-studio/image-to-3d`), `model_id` and `model_version`, `prompt_hash` (the prompt for an image, the spec for a mesh), `seed`, `inputs` and `derived_from` as content ids, `operator_id`, `created_at_unix_ms`, the licence fields |
| `prompt` | template, subject, full text and negative; a mesh copies its input image's, marked `from_input`, so one file tells the whole chain |
| `seed`, `parameters` | the seed, and the literal sampling settings (ComfyUI) or the request (Meshy) or the settings (Tripo) |
| `backend` | ComfyUI: server version and device, workflow file name, path and **SHA-256**, the **checkpoint, UNet, CLIP, VAE and LoRA file names** the graph loads, every override, the prompt id and the full graph sent. Meshy: task id, attempt, model asked for and reported, the service's timestamps, consumed credits. Tripo: the manifest, its settings, and what the owner recorded |
| `inputs`, `outputs` | each with path and SHA-256; an input image also names its own sidecar |
| `credits`, `operator`, `started_utc`, `finished_utc`, `seconds` | |
| `license` | `owner-generated-local` (ComfyUI on the owner's machine), `meshy-pro`, `tripo-studio-max` |
| `content_class` | always **`general`**, with `content_classes` `{nudity: none, sexual: none, violence: none}` ([ADR-0033](adr/0033-content-classes-and-view-policies.md)). This tool is not a path for classified content, and its subject lists are reviewed text: no people, no likenesses, nothing trademarked |
| `tool` | the script, the commit, and whether `tools/` was dirty |

**Open, and recorded rather than guessed:** every licence row carries the schema's conservative defaults, `commercial_ok: false` and `attribution_required: true`, with `license_terms_url` null, until the owner has reviewed each service's terms and the local models' licences; the content build's allowlist will read exactly these fields. A local model is identified by file name, not by the hash of a multi-gigabyte file, so `model_version` is null for ComfyUI and the workflow's SHA-256 pins which names were asked for.

## The E10 harness: `tools/e10-harness.ps1`

`tools/e10-harness.ps1 -Folder <dir of .glb>` takes every GLB in a folder — any service; a sidecar beside it adds the service, the task and the credits — through `engine-content build` and `stats`, renders it twice with `engine-view` from a fixed framing orbit (640×640, `--orbit 16 --frames 4 --shadows off`) at the **default LOD threshold (1 px)** and at **`--lod 0.05`** (effectively the leaves), compares the two with `engine-image compare --json`, and writes `report.json` and `report.md` under `<local root>\e10\<service>-<date>\`: per asset the triangles, clusters, levels, warnings by rule, atlas islands, seam fraction, smallest and median island in texels of a 4096 atlas, container bytes, build milliseconds, coarse-against-finest PSNR, SSIM and FLIP (with a heat map), the object's share of the frame and the FLIP mean over the object alone; a pass or a fail; for a failure a one-line diagnosis naming what a repair step would have to do; the totals — pass rate, and the mean, worst and best of every column; and **what the picture error follows**, as correlations with log triangle count, log island count, seam fraction, median island size and coverage, which is how a set's failures are read rather than only counted.

**The diagnoses tell apart the defects E10 found under one threshold.** An island below a texel in an atlas whose median island is large and whose seams are few is a *UV-degenerate* triangle or two (fold them into the neighbouring island); in an atlas with a median island below 1,024 texels or more than 30% of vertices on a seam it is *fragmentation* (repack and rebake). A FLIP failure on a mesh above `-DenseTriangles` (250,000) is *detail carried by geometry* (decimate to a budget and bake it into the normal map); on a fragmented atlas it is the atlas as well; on neither, the heat map has to be read.

**Measuring and judging are separate.** `-FromReport <report.json>` re-reads a report's measurements and judges them again under whatever thresholds are passed, rewriting `report.json` and `report.md` in place without building or rendering anything: a threshold is a decision, and revisiting one should not cost forty captures.

**The thresholds, and why those.**

- **0 warnings.** An asset that needs a person to read a warning before it is used is not one the pipeline can take unattended, which is the question E10 asks.
- **Smallest island ≥ 1 texel of a 4096 atlas.** An island below a texel cannot be sampled as itself at any mip, and no seam-respecting simplification can make it coarser ([geometry](subsystems/geometry.md#what-the-simplifier-is-given-and-why)); the repair is in texture space. Both Meshy characters that motivated the atlas statistics had an island that rounds to zero.
- **Coarse-against-finest FLIP ≤ 0.02.** From the seam fix's own numbers ([geometry](subsystems/geometry.md#what-the-simplifier-is-given-and-why)): the defective builds of those characters measured 0.0241 and 0.0223, the fixed builds 0.0115 and 0.0083, the FlightHelmet 0.0173 — so 0.02 passes every known-good build and fails every known-bad one. It is a whole-frame mean at a fixed framing, as those were; the object-only mean is reported beside it, and E10 showed why it matters: over twenty props the whole-frame mean followed the object's share of the frame at r = 0.70 and the object-only mean at 0.02, so a prop that fills the frame is judged more harshly than a thin one. Moving the threshold to the object-only mean needs the reference builds' coverage, which is one harness run over them.

**Which binaries.** Never a build tree's in place: the harness copies `engine-content`, `engine-view` and `engine-image` from `-Bin`, or from `build/<-Preset>/bin` of the checkout it runs from (`msvc-release` by default), into the output directory and runs the copies, so a rebuild during a run neither fails nor changes it; the report records each copy's SHA-256 and the commit.

**The GPU lock is not needed for the captures.** Two short captures per asset of one small scene are neither heavy nor a measurement whose numbers depend on load — the picture metrics are the same on a busy card. The build milliseconds do depend on load, so the report carries the `machine_state` engine-view sampled around every capture and who held the lock at the start and the end, and a write-up quotes them ([experiments README](experiments/README.md#the-machine-state-line)).

## Testing

`tools/generate.Tests.ps1`, under CTest as **`tools.generate`** (`tools/dev.ps1 test -Filter tools`), runs offline — no ComfyUI, no Meshy, no GPU, no credits. It converts a synthetic workflow against a synthetic `/object_info` with one of each case above (seed plus control value, stale placeholder behind a `PrimitiveNode`, `Reroute`, bypassed, muted, `Note`, a lone widget value) and checks every input; plans an image batch with `-DryRun` and checks each override, the templated prompt, the derived seeds, the 1024² default, the date-expanded prefix, `-Only`/`-Seed`/`-Width`/`-Set`, a spec hash that ignores the date, and the refusals (an unknown `-Set` target, a non-portable name, a local root inside the repository); and drives the Tripo folder end to end — manifest and README, ingest of the one GLB present and a report of the one missing, the sidecar's fields, its `asset_provenance` against the generated JSON Schema, `verify`, a cache hit, a different GLB under the same name refused, and a tampered output failing `verify`. The live paths were exercised against the real services by E10 — twenty images in one locked batch (one of them regenerated byte for byte from its sidecar), twenty Meshy tasks including an interrupted run that resumed without paying twice, a Tripo manifest, `verify` over all forty sidecars — and by hand: the ComfyUI 3D stage through a three-node stand-in workflow (`LoadImage` titled `Input Image` → `ImageScaleBy` → `SaveImage`), which proves the upload, the input override and the output collection but not a mesh; a Meshy dry run; and a Meshy spend refused without `-Yes` and over `-MaxCredits`, with the balance unchanged after both.

## Not yet

- **The C++ interface.** `generate(kind, spec, constraints)` as a service the content build and the protocol call, with the cache in the derived-data store; the tool is its prototype and its sidecar the record that interface will write.
- **The content build does not read sidecars yet.** 07 §7.1's "the build refuses assets without provenance" and §7.4's provenance rule need the build to take a sidecar beside a source mesh; the shape is fixed so that step is a read, not a design.
- **Local image-to-3D backends** (TRELLIS, Hunyuan3D) need only their workflows, titled as above; the ComfyUI 3D stage has run end to end only on the mechanics (upload, override, output collection), not yet on a workflow that makes a mesh, and a mesh-saving node that reports its file under a key other than a list of `{filename, subfolder, type}` objects would need a line in `Get-ComfyOutputs`.
- **Licence review** of every service and local model, which decides `commercial_ok` and `attribution_required`.
- **Model file hashes** for local models, cached by path, size and modification time, so `model_version` can name the exact weights.
