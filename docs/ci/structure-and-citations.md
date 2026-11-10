# Ceilings for structure, and code-to-doc citations

Two checks that run under CTest in every preset, read files only, and take seconds (roadmap
[T81](../roadmap.md)). The first pins the size of the files every feature edits, the way a size
table pins a hot type's bytes and the frame budgets a pass's milliseconds; the second holds the
source comments that cite a page section to headings the page still has. Both come from the
2026-11-09 review of the engine: its "hub files" table (a dozen files that grow with every feature,
because the feature had no registration point to go through) and its finding A10 S3-1 (comments
citing headings that had gone). Neither judges content. The first makes "do not make it worse" a
failing test while the real refactors are planned; the second has to exist before any large page
is restructured, or the restructuring breaks the citations silently.

## Ceilings for structure

`tools/structure-check.ps1` reads [`tools/structure-ceilings.json`](../../tools/structure-ceilings.json),
measures each hub it names, and fails naming every value over its ceiling. CTest runs it as
`structure_check` and its tests as `tools.structure_check` (registered by
`cmake/EngineStructureCheck.cmake`, which the top-level `CMakeLists.txt` includes last), and
`tools/dev.ps1 test -Affected` selects `structure_check` for every change, beside the lint and the
documentation check, through `tools/lib/Affected.psm1`'s list of the checks that always run: a
page that grows is documentation and reaches no module, and it can still pass its ceiling.

**A ceiling only goes down.** Three rules, each a failure with a sentence that says what to do:

- A value **over** its ceiling fails. Shrink the hub — split the function, move the section, give
  the feature a registration point of its own — or, if the growth is the change's point, raise the
  ceiling with a rebaseline that says why (below) and say so in the commit message.
- A value **under** its ceiling by more than `lower_after_percent` (2%, rounded down, of the
  ceiling) fails too: the hub shrank, and the ceiling is lowered in the same change, so the slack
  a refactor leaves cannot let the growth back in. 2% is about 65 lines of `record_frame` and
  19 KB of `renderer.md`, so an edit that moves a few lines either way never has to touch the file;
  for a count under 50 (the resolve's modules, the root's fields, `add_pass`) it is zero, so taking
  one away lowers the ceiling at once.
- A page the file does not name is held to `page_default_bytes` (below).

**Raising and lowering are one command.** `pwsh tools/structure-check.ps1 -Rebaseline` sets every
ceiling to its hub's measured value. When that only lowers ceilings it needs no reason and writes
no record; git's history of the file has the old values. When it raises any ceiling, or names a
page that has grown past the default, it refuses without `-Reason "<why>"`, and with one appends
to the file's `rebaselines` the date, who (`-By`, default the git user), the commit, the reason and
every old and new value it moved. It refuses while a hub is not there, and writes nothing when it
refuses. That is the manner of `tools/frame-budget.ps1 -Rebaseline -Reason` (ADR-0018): the file
moves with the change that moved the numbers, and a raise is a sentence a reviewer reads. Never
edit a ceiling up by hand.

**The batch's one line.** A merge that stacks several branches that grow the hubs runs, once, on
the stacked tree:

```powershell
pwsh -NoProfile -File tools/structure-check.ps1 -Rebaseline -Reason "batch <n>: <the rows that grew the hubs, and what for>"
```

### How each thing is counted

The tree is clang-formatted, and every rule leans on that. Each is written so two people with the
same tree get the same number.

| Measure | What counts | Why this rule |
|---|---|---|
| `function-lines` | The lines of one function's definition: from the first line at column 0 that names it (`Class::name` or `name`, not preceded by a letter, digit, `_`, `:` or `~`) followed by `(` and does not end in `;`, through the first line after it that is exactly `}`. | A definition at namespace scope closes at column 0 and nothing inside it does, so the first `}` alone on a line is its end; a declaration ends in `;`. No parsing of C++ needed, and a signature wrapped over lines counts from its first. |
| `function-matches` | Matches of the hub's regex in those lines (`\badd_pass\s*[(<]` for the frame's passes). | A pass added inline is the thing a frame-pass seam would have kept out. |
| `file-lines` | The file's lines: its newlines, plus one for a last line without one. | What an editor's line count says. |
| `shader-modules` | The distinct `.slang` sources named in every `engine_shaders(...)` call of every `CMakeLists.txt` under the layer directories, after `#` comments are dropped, whose file name matches the hub's wildcard (`visibility_resolve*.slang`), case and all. | Each source is one SPIR-V module the build compiles, so this is what the build produces and not a number someone typed. A call behind an `if()` counts: the declaration is the growth. |
| `schema-fields` | The lines between `struct <Name>` at column 0 and its `}` at column 0 that declare a field, `name: type`; `///` comments and blank lines do not count. | One field a line is the IDL's own layout (`schemas/README.md`). |
| `conditionals` | `#if` lines whose condition names a macro that starts with the prefix (`ENGINE_VIEW_`, `ENGINE_HOST_`), in one file or in every C and C++ source under a directory. `#ifdef`, `#ifndef` and `#elif` do not count. | The tree's capability macros are always defined, 0 or 1, and tested with `#if`; a capability wired in by hand adds one. |
| `page-bytes` | The page's bytes, with each CRLF counted as one byte. | A page's size is what a model reading it pays. |

### The same number on every machine

The check also runs in the Linux containers and on the GPU server, where nothing guarantees the
checkout's line endings or the file system's case rules. So every count is taken after CRLF is made
LF, and a path in the ceilings file is resolved one component at a time against the directory's
entries with an exact, case-sensitive comparison: on Windows `Main.cpp` would open `main.cpp` and on
Linux it would not, so on both the check says the hub is not there and how the tree spells it.
The pages walked are the same set docs-check walks (`build/`, `third_party/`, `.git/`, `.claude/`
and the other caches skipped by name; `content/samples/` skipped because it is downloaded).

### Pages

A page has its own ceiling when it is named in the file; every other page is held to
`page_default_bytes`, 100,000 bytes. Every page of 100 KB or more on 2026-11-09 is named (thirteen,
from `renderer.md` at 956 KB down to `world.md` at 101 KB) and so is `AGENTS.md`, which every
session reads first; the largest unnamed page was `audio.md` at 94 KB, so every unnamed page passes
today. 100 KB is a threshold and not a measurement: a page that grows past it becomes a named page
by a rebaseline with a reason, which is the moment someone should ask whether it should split
(the review's A10-R1 proposes about 120 KB for a reference page). The default is not rebaselined.

### What a contributor sees

```text
structure-check: 1 problem(s)
  renderer.record_frame.lines: 3301 lines, over its ceiling of 3255 (SceneRenderer::record_frame, systems/renderer/src/scene_renderer.cpp)

A hub over its ceiling grew: shrink it — split the function, move the section, give the
feature a registration point of its own — or, if the growth is the change's point, raise
the ceiling with  pwsh tools/structure-check.ps1 -Rebaseline -Reason "<why>"  and say so in
the commit message. A hub that shrank lowers its ceiling with  pwsh tools/structure-check.ps1
-Rebaseline  (no reason needed). Never edit a ceiling up by hand.
```

A passing run prints each hub's value beside its ceiling, so `pwsh tools/structure-check.ps1` is
also how to read the table below on any tree.

### The hubs on 2026-11-09

Measured at T81's tip, on main 2d867dd2's code; the review's figures were taken at 6353fac0, whose
code is the same. `AGENTS.md` and `docs/roadmap.md` are this row's own bullet and row larger than
main.

| Hub | Measure | Ceiling | The review's figure |
|---|---|---|---|
| `SceneRenderer::record_frame` | `function-lines`; `add_pass` matches | 3,255; 43 | 3,255; 43 |
| `systems/renderer/src/gpu_scene.cpp`, `gpu_scene.h` | `file-lines` | 6,595; 1,819 | 6,595; 1,819 |
| `apps/engine_view/main.cpp`; `run_view`; `run_offscreen` | `file-lines`; `function-lines` | 7,466; 3,166; 1,162 | 7,466; 3,166; 1,162 |
| The material resolve | `shader-modules`, `visibility_resolve*.slang` | 16 | 16 |
| `Scene` in `schemas/scene.schema` (version 11) | `schema-fields` | 20 | not counted |
| `#if ENGINE_VIEW_*`: `apps/engine_view/main.cpp`; the whole app with its tests | `conditionals` | 84; 146 | 84 (main.cpp) |
| `#if ENGINE_HOST_*` in `apps/engine_host/ops_methods.cpp` | `conditionals` | 65 | 65 |
| `renderer.md`, `apps.md`, `gfx.md`, `docs/roadmap.md`, `AGENTS.md` | `page-bytes` | see the file | 956 KB, 652 KB, 306 KB, 296 KB, 68 KB |

The app-wide `#if ENGINE_VIEW_*` count is a second hub beside the review's: `main.cpp` holds 84 of
the 146, and a guard that moves into `walk.cpp` (16 today) is the same growth.

**What it costs.** About 2.5 s for the check over this tree and 4 s for its 26 tests (msvc-debug's
CTest, a shared machine with other agents building; 2026-11-09).

## Code-to-doc citations

`tools/docs-check.ps1`'s `citation` rule. A source comment that cites a page section by its heading
has to name a page that exists and a heading or bold lead-in on it, or the check fails with the file,
the line, the text and the nearest heading the page has:

```text
apps/engine_view/main.cpp:5718: [citation] "One clock" is not a heading or bold lead-in of docs/subsystems/renderer.md; the nearest is "One pipeline" (line 2146)
```

The nearest is the heading with the most words in common, which is a hint and not a fix: in that
example the text now lives under "One world clock, and the day's length".

**The forms recognised.**

- `(docs/subsystems/<page>.md, "<Heading>")`, and any other page path: `docs/ci/<page>.md`,
  `docs/experiments/<page>.md`, `AGENTS.md`. A path with a directory is the tree's, from its root,
  or failing that the citing file's directory's (`../../docs/...`).
- The short form, a bare name: `(<page>.md, "<Heading>")`. It is `docs/subsystems/<page>.md` when
  that exists, else the one page of that name in the tree; a name two pages share fails and asks
  for the directory.
- A list: `(<page>.md, "<One>", "<Two>")`, each text checked.
- Any of those wrapped over comment lines: each continuation line's comment leader (`//`, `///`,
  `#`, `--`, `*`, `;`) is dropped and the line joined, so `(gfx.md, "The` at the end of one line and
  `// HDR branches")` on the next is the citation "The HDR branches".

**The match.** The text and the page's headings and bold lead-ins are compared by GitHub's slug,
the one the link rule resolves anchors with: case, backticks, emphasis, link syntax and
punctuation do not count. A text resolves when its slug is a heading's or a bold lead-in's, or the
first whole words of one ("Normal cones" for "Normal cones fit to what is drawn"), and a heading's
enumerator may be left out ("Evaluation" for "C. Evaluation", "Streaming" for "4.3 Streaming"). A
bold lead-in is a line that starts, after an optional list marker or quote, with `**...**`. A
heading in a fenced block is not a heading. The first words are allowed because that is how the
tree cites: on 2026-11-09, more than 200 of the 2,700 citations named a heading by its first
words, almost always because the full heading would not fit a comment line, and a rule that
refused them would have asked for 200 edits that tell a reader nothing more.

**The files read.** Every file under the tree (the same directories skipped as for the links) whose
extension is `.c`, `.cc`, `.cpp`, `.h`, `.hpp`, `.inc`, `.slang`, `.schema`, `.ps1`, `.psm1`,
`.cmake`, `.sh`, `.js`, `.html`, `.css`, `.yml` or `.yaml`, and every `CMakeLists.txt`,
`Dockerfile` and `.gitignore`. Not a `.md`: a page links with Markdown, which the link rule checks.
Not JSON, which has no comments. A text must start with something other than a space, so two C++
string literals joined across a line (`"... (water.md, " "The banks)"`) are not read as one.

**What it does not check.** An anchor written in a comment (`renderer.md#shadows`, 11 in the tree
on 2026-11-09) and a plan section by number (`04 §4.3`): neither is this form, and the second has
its own rule in the plan pages' links.

**Listing the failures.** `pwsh tools/docs-check.ps1` prints every problem, one a line, and the
citation ones carry `[citation]`; so on a stacked tree where other rows added citations,
`pwsh tools/docs-check.ps1 | Select-String '\[citation\]'` lists them alone. A stale citation is
fixed in the comment, to the heading that now holds the text; when the text has gone from the
page, the comment cites the heading that holds the rule now, or loses the citation, and the page is
not edited to suit it.

**What it found, 2026-11-09.** About 2,700 citations (850 distinct page and text pairs) in about 820
files. 51 did not resolve, 39 distinct: headings renamed ("One clock", now "One world clock, and the day's
length"; gfx's "The surface's offers", now "The HDR branches and the surface's offers"), headings
folded into another ("Samplers" into "The material table: samplers and the UV transform"), a page
moved (`docs/subsystems/native-terrain-materials.md`, whose stochastic sampling is now a section of
`renderer.md`), and texts that had never been headings. Each was pointed at the heading that holds
the text now; T81's report lists them.

**What it costs.** The citation rule adds about 3 s to docs-check's run (about 8 s before, 11 s
after, on a shared machine), most of it reading the cited pages' headings; the scan looks for
`.md,` with a string search first, because one regex for the whole citation tried every position
of every word in every file and took four times as long.
