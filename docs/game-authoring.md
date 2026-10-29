# Writing a game on the engine

This is the author's guide: for someone who has never opened the engine's tree, and may be a model
writing C++ from the headers. It says what a game module is, how the loop of building and running
one goes, what the exported SDK and the repository template give you, the rules a module has to
keep so that a run replays, how to package a build for a tester and read what comes back, and what
is not there yet. It does **not** restate the contract — the fields a module reads and the services
it may call change with the engine, and one page is kept current:
[apps.md, "The game contract"](subsystems/apps.md#the-game-contract). Read this page once, that
section every time the engine moves. Roadmap P42 made the kit; P41 made the runner it drives.

## What a game module is

**One shared library the engine loads into its walking session.** `engine-view --game <module.dll>`
loads it before any window, ticks it 240 times a second from the walking session, draws the HUD it
asks for, watches its file and swaps a rebuild in while the session runs, ends the run when the
module says so, and writes a report at exit ([apps.md, "--game"](subsystems/apps.md#--game-running-a-game-module)).
The module is three things:

- **One struct of state**, declared in a `.schema` file beside the code (`struct Run @version(1) { seconds: f64 = 0 ... }`),
  of fixed-layout values only — numbers, bools, the fixed vector types, nested structs of the same;
  never a string, an array or a pointer ([hotload.md, "What a module's state may hold"](subsystems/hotload.md#what-a-modules-state-may-hold)).
  The host allocates one object of it when the module loads, carries it across every rebuild through
  the old and the new build's descriptors (a field added starts at its default, one removed is
  dropped), and `--game-state <file>` saves and restores it. Nothing else the module keeps survives a
  rebuild, so everything that matters is in the state.
- **`on_load`**, called when the module is loaded and after every rebuild: it resolves, by name, the
  fields of the tick it will read (`resolve(context, "engine.game.GameTick", "dt", field)`) and
  declares the game actions it listens to (`game_action(context, "interact", bit)`). A name that
  does not resolve refuses the load, naming it, and the old build keeps running — which is how an
  engine-side rename reaches a game: loudly, never as a wrong read.
- **`game_tick`**, exported as a `TickFn`, called every tick with a view of that tick: it reads the
  fields it resolved (`read`), changes its own state, says what the HUD shows (`hud_text`,
  `hud_meter`), and may end the run (`end_run`) or ask the world a question (`nearest_placement`).

Everything the module knows about the world comes through that view and those services; everything
it keeps is its state. It links no engine library: its include path is the engine's core headers
and `foundation/hotload/module_abi.h` (the contract's source) and `module_state.h`, and it is
compiled with the engine's own flags. [`game/walk_poc`](../game/walk_poc/walk_poc.cpp) is the
engine's fixture — thirst, water at the ruins, a day counter — and `templates/game-repo/src/` the
smaller module the template starts from; both are the contract in use.

## The loop

Build the module, run it, edit, rebuild while it runs. From a game repository made by the template:

```powershell
./build.ps1                                # src/*.cpp and *.schema -> build/<name>.dll
./run.ps1 -Report report.json              # a window on the desert, the module loaded; Escape twice ends
./build.ps1                                # in another shell, while the window is open: the running
                                           # session swaps the new build in on its next frame
./run.ps1 -EngineArgs "--game-state save.json"   # save the state at exit, restore it next time
./run.ps1 -Offscreen -Frames 60 -Report report.json   # no window: the SDK's recorded walk, the same ticks every time
```

`run.ps1` passes `--walk --game`, and `--world` where the scene streams one; the keys are
engine-view's map (W A S D walk, Shift sprints, the mouse looks, Space jumps, E `interact`, Q `use`,
1–4 `game_1`..`game_4`, `[` `]` the day longer or shorter, `,` `.` the world slower or faster, Escape gives the mouse back and ends).
A rebuild the host refuses — another ABI, a state type with an owning field, a name that no longer
resolves — says why once on stderr and keeps the old build running. `-Offscreen` replays
`content/sessions/walk-synthetic.jsonl` from the SDK (two seconds of walking with E pressed once,
at 240 Hz) with no window and writes the same report; because a `--game` session is a function of
its ticks, the module's state at the end is a function of the module alone, which is how a module
is tested and what the engine's own test of the kit does.

## The SDK: a folder, three commands

The engine side of a game repository is **one folder the author copies**, written by
`tools/export-game-sdk.ps1 -Preset msvc-release -Out <dir> [-Zip <file>]` from a built preset of the
engine's tree (the exporter refuses an output inside the repository and a build without the hotload
capability, whose `build/<preset>/game_modules/build_config.cmake` does not exist). Its `README.md`
describes it; in short:

| Path | What |
|---|---|
| `include/` | the public headers a module compiles against — exactly the directories the engine's exported build command lists (`core/*/include`, `foundation/hotload/include`), merged; nothing from any `src/` |
| `schemas/` | the schema root a module's `.schema` may import from |
| `bin/` | `schemac` (the schema compiler), `engine-view` (the preset's, shaders embedded), `game/walk_poc.dll` (the worked example: run it before writing a line) |
| `cmake/` | `EngineGameModuleBuild.cmake`, the engine's build command, and a **relocatable** `build_config.cmake`: every path into the SDK is relative to the config itself, so the folder can be moved or unzipped anywhere ([hotload.md, "Building a module"](subsystems/hotload.md#building-a-module)) |
| `content/` | `test-scenes/desert-endless/` and `ruins-walk/` (with its ruins kit generated at export, since the tree never commits it), with every file each names, `sessions/walk-synthetic.jsonl` (recorded against the map below), `input-map.json`, the action map the SDK's engine-view flies with and its hash (`--dump-action-map`), and `materials/`, the material pack, when the exporting checkout had one ([below](#packaging-and-reading-what-comes-back)) |
| `tools/` | the three commands below, and `lib/GameContent.psm1` |
| `sdk.json` | the engine commit, whether it was dirty, the preset, the CPU baseline, the hotload ABI version, the export date, the scenes carried |
| `LICENSE`, `NOTICE` | the engine's Apache-2.0, which everything in the folder is under |

```powershell
tools/build-module.ps1 -Source <dir> -Out <file.dll>                      # the engine's compiler, flags and schemac
tools/run-game.ps1     -Module <dll> -Scene <scene.json> [-Report <f>] [-Offscreen [-Frames n]] [-EngineArgs "<args>"]
tools/package-game.ps1 -ModuleDll <dll> -Scene <scene.json> [-Out <zip>]  # the zip a tester double-clicks
```

**The compiler is the exporting machine's, and it does not have to be yours.** `build_config.cmake`
records the compiler, flags, definitions and the MSVC environment of the machine that exported the
SDK, because a module has to be compiled with the engine's own flags (the CPU baseline,
floating-point contraction off, the runtime library) to run inside it. On a machine without that
compiler, `build-module.ps1` finds Visual Studio through `vswhere`, imports its x64 developer
environment and hands the command that machine's `cl.exe` with the same flags; a missing compiler
and no substitute is a sentence, not a failure to start `cl.exe`. It needs PowerShell 7, CMake 3.28
or later (on PATH or Visual Studio's) and Visual Studio 2022 or later with the C++ x64 tools.
**Linux is not covered by an exported SDK**: the suffix and the flags are the preset's, so a Linux
SDK would be a Linux preset's export, which nothing writes yet; a module built here runs on Windows.

## The template: a repository that builds, runs and packages

`tools/new-game-repo.ps1 -Name <name> -Out <dir> -Sdk <sdk dir> [-RelativeSdk]` instantiates
[`templates/game-repo/`](../templates/game-repo/README.md) into a new directory outside the engine
repository (refused inside it; `-Name` must be a C identifier, since it names the module, its schema
namespace and its DLL):

- `README.md` — how to build, run and package, where the engine is, what is not done;
- `engine-sdk.json` — the SDK's path (absolute, or relative with `-RelativeSdk`) and the fields of
  its `sdk.json` the game pins: **the one answer to "which engine commit is this game built against"**;
  moving to a newer engine is a new export and a deliberate change to this file;
- `src/<name>.schema` and `src/<name>.cpp` — a state of three fields (seconds survived, the world's
  time when the run began, times `interact` was pressed), `on_load` resolving the three tick fields
  it reads and one action, `game_tick` drawing one HUD line and one meter and ending the run after
  three game days, every part commented with what it is for and where the contract is documented;
- `build.ps1`, `run.ps1`, `package.ps1` — the SDK's three commands over this repository;
- `.gitignore` (`build/`, `*.dll`, `report.json`, `ddc/`, `*.hotload/`, `*.zip`) and `LICENSE.md`, a
  placeholder that says the licence is the author's and records what [ADR-0047](adr/0047-reference-games-are-licensed-apart-from-the-engine.md)
  chose for Desert Survival (GPL-3.0-or-later code, non-commercial assets, a trademarked title) without
  choosing for anyone.

`git init` and the first commit are the author's; the script runs neither.

## Rules that keep a run replayable

A recorded `--game` session replays to the same module state, offscreen or in a window, as long as
the module keeps to these — they are the contract's determinism paragraph in the author's words:

- **No wall clock.** The tick's `dt`, `world_time_s` and `sky_time_s` are the only time; the ABI
  has no clock to read, and `std::chrono` or `time()` would make a replay diverge. Read `dt` from
  the view in f64, not the f32 argument, which drifts a part in ten million a tick when summed.

**The day's length and the world's rate** (2026-10-20, roadmap R88; the engine's
[clocks](subsystems/renderer.md#one-world-clock-and-the-days-length)). A game has two clocks to read
and two rates to set. **`world_time_s` is the world clock** — real seconds, scaled by the world rate
— which every animation in the engine runs on (the water, the smoke and the fire's flicker, the
trees' sway, the cloud drift) and a module animates by too. **`sky_time_s` is the sky's hour**,
absolute game seconds since the world's midnight epoch, and `day_fraction` where the sky stands in
its day: count days, schedule a night, by these. **A game sets its day's length**: the length of a
game day in real seconds, `--day-length` (or `--sun-rate`, the same said as `86,400 / length`) on its
run line and `-DayLength` (or `-SunRate`) to `package-game.ps1` — a sixty-minute day is 3,600, and
every animation stays at real speed whatever it is. **The dunes' time-lapse** is `--time-rate`
(`-TimeRate`), game seconds of sand a world second: 1 is real geology, which a desert sees move over
weeks. **The world rate is for looking, not for a game's design**: 1 by default, `--world-rate` or
`,` and `.` in the window scale it — a pause, slow motion, a fast-forward that runs the dunes and the
smoke with it — and the sky keeps its pace through all of it, so a tester can inspect what the world
does at any speed. A module that reads the rate (`GameTick.world_rate`) and the day's length
(`day_length_s`) can say so on its HUD; it never needs to divide by either.
- **Nothing allocated in `game_tick`.** The HUD's lines go through a stack buffer the host copies;
  the module's state is the one object the host allocated when it loaded the module. A module that
  allocates per tick is not wrong, only slower and harder to reason about; a module that keeps
  state outside the struct loses it at the next rebuild.
- **The state is the one object.** Everything that must survive a rebuild or a save is a field of
  the schema struct; globals are scratch that a rebuild resets (`walk_poc` keeps a cosmetic HUD
  number in one, refreshed each second, and nothing else).
- **Compile with the SDK's flags.** `build-module.ps1` does; a module compiled another way (another
  baseline, contraction on, another runtime library) may load and compute different floats from the
  engine's, or not load at all.
- **Read actions from the tick's masks** (`actions_pressed`, `actions_held`, `actions_released`,
  through `action_set(value, bit)`), never from the keyboard: a replay has no keyboard.
- **Pin the action map by its file, not by a number in a test.** Which key is `interact` is the
  action map's, and a recording names the map's hash in its header; a replay against another map
  is refused naming both hashes, because the same key would mean another action. The SDK carries
  the map its engine-view flies with as `content/input-map.json` (written at export by
  `engine-view --dump-action-map`, with its `hash`), `run-game.ps1` and the template's `run.ps1`
  pass it as `--input-map`, and the SDK's synthetic walk is recorded against it. A game's test that
  needs the hash reads that file's `hash`; a hash typed into a test is the next SDK's mismatch, and
  until 2026-10-18 the SDK's walk named the first revision's map — the one with no `interact` —
  so an offscreen run never pressed E for the module at all.

## The action map and the HUD's elements (hotload ABI 4)

ABI 4 (2026-10-18) appended services and **loads a module built against 3 unchanged**: an SDK
upgrade that only adds a service no longer forces a rebuild. A module built against 4 that wants
to run on an older host checks `ENGINE_HOTLOAD_HAS(api, hud_element)` (the helpers do, answering
`Unsupported`). What 4 adds for a game:

- **Structured HUD elements**: `hud_element(context, element)` with a `HudElement` — an id of the
  module's, an anchor (the nine points of the safe region), an offset, an alignment, text, a colour,
  a font size, an outline, an optional panel, visibility — kept by the host until `hud_remove` or a
  new run, so a prompt is said when it changes (saying it every tick is allowed). **Lengths are in
  pixels of a 1080-pixel-high picture at 100 %**; the host scales them and keeps the font legible,
  so a module never sizes anything to the window. `walk_poc`'s `[E] Pick up` is the example:
  centred, 80 px below the middle, 28 px, a 2 px outline, no panel.
- **The graphics the session drew with**: `graphics_state(context, state)` names the resolved
  raster path, shadows, present mode, display, resolution and tier, with the reasons a setting is
  not what was asked (a device with no ray queries draws cascaded shadow maps), so a game can say
  so on its options screen; `request_graphics` records a change for the next session and refuses
  one the machine cannot make. Nothing is applied to a running session.
- **A saved run resumes where it was** (not an ABI change: the state file's version 2):
  `--game-state` keeps the walker's pose, its speed scales and the clock beside the module's state
  and props, and a live session from it starts there with the sky where it stood; a recording of
  that session carries what it restored, so its replay is a replay whatever the file holds by then.
- **The report says where each element stood**: `game.hud_elements` lists every element — the
  `hud_text` lines and `hud_meter` meters under the host's reserved ids, then the module's — with
  its last state and `bounds_px`, so a test asserts its prompt was on screen and where without
  reading a picture.

## The characters: actors and the ground (hotload ABI 5)

ABI 5 (2026-10-19) appended services again and loads modules built against 3 and 4 unchanged. A
**character** the scene file holds is an actor once its instance has a `"name"` and animates (an
`animation` block): `actor_find(context, "surveyor", actor)` in `on_load` refuses the load if the
scene spells it otherwise, and the handle — the instance's index — is what the module keeps in its
state. At a tick, `actor_place` stands it (level, its scale kept), `actor_play` changes its clip
with a cross-fade, `actor_stop` fades it to its rest pose, `actor_speed` sets its rate, and
`actor_state` reads back where it is, what it plays and the hash of its pose; `ground_height(x, z)`
is the height the walker would stand on, so a tortoise ambles over the dunes rather than through
them. Everything is applied at the tick it is asked for, so a replay with the same module is the
same walk; `--game-state` keeps where every actor stood and what it played, and a resumed run puts
them back. Two limits, both the renderer's for now: a scene that also declares `props` (or streams
a world) answers `actor_place` with `Unsupported`, and `actor_spawn` — a character made at run time —
answers `Unsupported` everywhere ([apps.md, "Actors"](subsystems/apps.md#actors-the-characters-a-game-moves)
says what each needs). An actor does not collide: check reach from `actor_state`'s position.

## The HUD, and how far `nearest_placement` sees

The HUD is a proof-of-concept panel, not the game UI framework
([apps.md, "The HUD, provisional"](subsystems/apps.md#the-hud-provisional)): the host clears it at
the start of every tick, so a module says each tick what it shows — **16 lines of 95 characters and
8 meters**, more is dropped — laid out as one fixed panel at the top left, meters first (a name and a
bar with `value / max`), then the lines in order. Nothing on it takes input. On an HDR output it is
drawn at paper white (`--paper-white-nits`): white text is the display's paper white, never the
top of its curve.

**Your own art and type** (hotload ABI 6, 2026-10-19): ship the files in the game's content
directory — beside the scene file, or beside the module — and name them by a relative path.
`hud_image(context, image)` places a `HudImage` (a PNG or JPEG, at a size in 1080p pixels, tinted,
with nine-slice borders so one small panel graphic stretches to any box with its corners kept) and
`hud_font(context, "hud/title.otf", 48.0f, font)` loads a TrueType or OpenType font, a handle a
`HudElement`'s `font` names; `letter_spacing_px` and `shadow_offset_px`/`shadow_color` style the
text. Elements are drawn in the order they were first placed, so place a panel before the text on it.
A missing file is `NotFound` — a plain answer, so a module runs without its art — and the walk_poc
fixture shows both ([apps.md, "The HUD, provisional"](subsystems/apps.md#the-hud-provisional)).

`nearest_placement(kind, radius, &position, &distance)` answers the nearest placement of a scatter's
or the ruins' kind within `radius` metres of the walker's feet, measured over the ground, from the
tiles the world has streamed in ([apps.md, the services](subsystems/apps.md#the-game-contract)): a
few metres round the walker is always the generators' own answer, while a radius past the innermost
ring sees only what has streamed in so far — so a module looking far should say so on its HUD rather
than decide on it, and a scene that streams no world answers `NotFound` to everything.

## Packaging, and reading what comes back

`./package.ps1` (the SDK's `package-game.ps1 -ModuleDll build/<name>.dll -Scene <scene>`) makes the
zip a tester unzips, double-clicks `run.cmd` in, plays, and sends **one file** back from:
`report.json`, written at exit whichever way the run ended ([apps.md, "--report"](subsystems/apps.md#--report-what-a-tester-sends-back)).
The scene is the SDK's by default; a scene of the game's own (a directory in the repository holding
`scene.json` and what it names) is carried with that directory's content
([apps.md, "package-game"](subsystems/apps.md#package-game-a-zip-somebody-else-double-clicks)). The far
end needs a Vulkan driver and the Visual C++ redistributable, which the package's `README.txt` says.

**The material pack.** When the checkout the SDK was exported from had a material pack fetched
(`tools/fetch-materials.ps1`; [materials](subsystems/materials.md)), the SDK carries it as
`content/materials/` with its `LICENSES.md` (`sdk.json`'s `materials`), and `package-game.ps1`
puts it in the zip and passes `--materials` to engine-view, so a tester's palms and rocks draw the
same default sets as the author's — the bark, the fronds and the stone instead of flat colours. A
recipe of the game's own that names its own images keeps them; one that names none takes the
pack's role. Running from the SDK by hand, pass it on: `tools/run-game.ps1 ... -EngineArgs
"--materials content\materials"`. The pack's images are generated sets whose licences are in its
`LICENSES.md`; a game that ships them ships that file with them.

Reading a report: `exit` says how the run ended — `reason` is `end_run` when the module ended it (the
object it handed `end_run` is `game.result`), `game_error` when the module failed (`game.error` has
the line), `closed` or `escape` when the tester did, `error` when the engine could not run
(`exit.error` names the stage: `scene`, `device`, `game`); `machine` is the adapter, driver, Vulkan
version, device tier, OS, CPU and the engine's build; `frames` the frame-time spread; `walk` how far
they walked; `game` the module's block — `ticks`, `reloads`, `hud` (the last tick's lines and
meters), `state` (the state as its descriptor writes it), `clock`, `placements`. **An engine problem
is `exit.error` with a stage, and `machine`**: a report whose `exit.reason` is `error` with
`device: false`, or whose frames are tens of milliseconds on a named adapter, is the engine's to
look at — send it, with `machine`, to the engine's roadmap as a row. A `game_error` is the module's.

## What an author still needs the engine checkout for

Plainly: **nothing, to build, run and package a module against a given engine build** — the SDK and
the template are enough, on a Windows machine with Visual Studio. The checkout is needed to **make**
an SDK (`tools/export-game-sdk.ps1` runs from a built preset), to move a game to a newer engine
(export again), to read the documentation the SDK's `README.md` and `sdk.json` point at (the
contract, this page, the hotload page; the SDK carries the headers, not the docs), to make a scene
with content the tree generates (`engine-content ruins-kit`, the content build: the SDK ships
`engine-view`, `schemac` and the scenes, not `engine-content`), and for anything on Linux.

## Not there yet

The runner's limits are its own page's ([apps.md, "--game"](subsystems/apps.md#--game-running-a-game-module)
and ["The HUD, provisional"](subsystems/apps.md#the-hud-provisional)); of the kit's own:

- **No Linux SDK.** The export is one preset's, and every preset built here is Windows; a Linux
  preset's export is the same script with `.so` and the Linux flags, not written, and `tools.game_sdk`
  skips off Windows.
- **No `engine-content` in the SDK**, so a scene that needs generated content (a ruins kit) is made
  from a checkout; the exporter generates the kits of the scenes it carries.
- **The offscreen walk is synthetic**: its header is the committed fly session's camera with the
  exported map's hash and the walker's default numbers written by the exporter; when `walk.h`'s
  version moves, engine-view refuses the recording naming both versions, and the exporter has to
  follow.
- **The documentation is not in the SDK**: `sdk.json` names the pages by path at the pinned commit.
- **One game action set** (`interact`, `use`, `game_1`..`game_4`), and the HUD above.

## Testing

`tools.game_sdk` (`tools/export-game-sdk.Tests.ps1`, under CTest with the end-to-end tests' lock
and skip code) runs the whole loop from the build it is in: an SDK exported into the test's scratch
directory, the template instantiated from it, the module built through the repository's `build.ps1`
over the relocatable config, run offscreen for 20 frames through `run.ps1` with a report whose
`exit`, `machine`, `frames` and `game` (the name, 80 ticks, the HUD's line and meter, the walk's E
press reaching the module once through the exported map, the HUD's elements listed) are checked,
the exported `content/input-map.json` checked against the synthetic walk's header digit for digit,
packed with the SDK's `package-game.ps1` on `-ModuleDll` and the zip's `package.json`, `run.cmd` and
module checked, then a scene of the repository's own packed and found under `content/<its directory>/`;
plus the refusals. 61 checks, 127 s in `msvc-debug` on 2026-10-18 (the module's build 16 s, the
run 77 s, the package 5 s; 56 checks and 74 s before, on a quieter machine) on the RTX 5090
beside five other agents' builds; it skips with a sentence where the build
has no hotload capability. The SDK it exports from `msvc-debug` is 33 MB (a 32 MB Debug `engine-view`),
87 files.

The window host also exposes `GameTick.render_frame_ms`, `render_frame_serial` and
`render_elapsed_s` (tick schema v3). These are observation only: the last wall-clock
start-to-start render interval including present/pacing/waits, its serial, and the
sum of intervals in seconds. Zero means unavailable (startup or offscreen replay).
FPS is `1000 / render_frame_ms` when positive. They do not measure GPU duration or
physical scanouts and must not drive gameplay or deterministic state. Use the render
elapsed clock to refresh a diagnostic HUD even when world time is paused. Resolved
`graphics_state` dimensions follow the renderer after a resize/fullscreen change.
