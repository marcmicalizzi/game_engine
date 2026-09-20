# Linux builds on a Windows machine

One command, on the development desktop, builds and tests the four Linux presets
`.github/workflows/ci.yml` runs — in a container that is the hosted runner's toolchain rather
than something like it:

```powershell
pwsh tools/linux-build.ps1 -Preset all -Test
```

## Why this exists

Hosted CI refused every job for about a day in September 2026 (an account billing block), and in
that window roughly a hundred commits landed that **no** Linux compiler had seen: `physics` with
Jolt, `ecs` with flecs, `store` with SQLite, `nav` with Recast, `sim`, `anim`,
`systems/renderer`, `systems/animation`, new shaders, new tools. The tree builds with warnings as
errors on every compiler, and MSVC forgives a long list of things GCC and Clang do not, so "it
builds on Windows" had stopped meaning very much.

A gate that only exists on somebody else's machine is a gate that can be taken away. This one is
local, takes minutes, and needs the network only the first time.

It does **not** replace `ci.yml`: the workflow is still what gates a push, and the self-hosted GPU
runners ([self-hosted runners](self-hosted-runners.md)) are still the only machines that execute a
shader. This is the compiler-and-CPU half of the gate, moved to where the work happens.

## The command

```powershell
pwsh tools/linux-build.ps1 [-Preset <name>|all] [-Test] [-Filter <regex>] [-Jobs <n>]
                           [-Shell] [-Rebuild] [-Docs [-Base <rev>]] [-Prune] [-Sync:$false]
```

| Flag | What |
|---|---|
| `-Preset` | one preset, or `all` for `linux-clang-debug`, `linux-gcc-release`, `linux-clang-minimal`, `linux-clang-no-ecs` — ci.yml's matrix, in ci.yml's order. Any other Linux preset works too (`linux-clang-asan`). Default `all`. |
| `-Test` | run CTest after the build. Without it the run is a compile check. |
| `-Filter` | a regex on test names, passed through to `ctest -R`. |
| `-Jobs` | parallel compile jobs. Default 8; see [Why eight jobs](#why-eight-jobs--and-why-the-worry-was-wrong). |
| `-Rebuild` | delete `build/<preset>` inside the volume first. The downloaded dependencies are in a different volume and survive, so this costs a compile and not a download. |
| `-Shell` | an interactive `bash` in the container with the volumes mounted, for when a failure needs poking at. |
| `-Docs` | run `tools/docs-gate.sh` and `tools/docs-check.sh` in the container and build nothing; `-Base` is what the gate diffs against (default `main`). See [Running the documentation gate here](#running-the-documentation-gate-here). |
| `-Prune` | delete this checkout's two volumes and exit. |
| `-Sync:$false` | build what is already in the volume without re-syncing the checkout. |

Exit status is the build's, or the tests' with `-Test`. With `all`, every preset is attempted and
the status is the first failure: nothing stops early, because a second compiler's opinion is the
entire reason for running four of them.

`-Preset all` prints a summary table at the end. One preset prints its own line.

## What it does, and the three choices worth arguing about

### The checkout is mounted read-only and rsync'd into a volume

**Never build over a bind mount of the Windows checkout.** WSL2 reaches `D:\` over a 9p share;
a configure plus a build of this tree is hundreds of thousands of small-file operations, and the
Windows build trees under `build/` must not be touched by a Linux CMake anyway. So the checkout
arrives at `/host` **read-only**, and one `rsync -a --delete` copies it into the `/src` volume.
About 610 files and 8 MB, in about two seconds.

`--delete` makes a file removed on the host disappear in the volume; the excluded paths are
protected from it by default, which is what keeps `/src/build` alive across syncs. Excluded:
`build/`, `out/`, `ddc/`, `content/samples/`, `content/golden/local/`, `.claude/`, `.git`, and the
IDE directories.

**Why rsync and not `git clone` from the mount plus an overlay.** Two reasons, and the first is
decisive. An agent worktree's `.git` is a *file* pointing into the main repository's
`.git/worktrees/<name>` directory, so a clone from the mount would need that directory mounted as
well and would still produce a checkout of `HEAD` — while the entire point of a local Linux check
is to run it on work that is **not committed yet**. Layering uncommitted changes back over a clone
is rsync with extra steps. Second, nothing in the build or the test suite reads git history:
`tools/docs-gate.test.sh` builds its own throwaway repositories, and `tools/docs-gate.sh` takes
`--files-from`/`--messages-from`, which is how CI runs it for a pull request anyway.

### Two named volumes, one per checkout

| Volume | Mounted at | Holds |
|---|---|---|
| `engine-linux-src-<id>` | `/src` | the synced sources and `build/<preset>/` for every preset |
| `engine-linux-deps-<id>` | `/deps` | `FETCHCONTENT_BASE_DIR`, one directory per preset |

`<id>` is a hash of the checkout's path, so every agent worktree gets its own pair and two of them
cannot land in one build directory. The image is shared between checkouts — it is a function of
the Dockerfile alone.

Dependencies live in the second volume because `-Rebuild` should cost a compile and not a 700 MB
download, and because a warm run then never touches the network at all
(`FETCHCONTENT_UPDATES_DISCONNECTED=ON` also stops the git-based dependencies re-running
`git fetch` on every reconfigure; the tags are pinned, so a fetch can only cost time).
**Checked rather than assumed**: a reconfigure and build of `linux-clang-minimal` in a container
started with `--network none` succeeds. Only the first run of a given preset needs the network.

**Each preset downloads its own copy of the sources**, because `FETCHCONTENT_BASE_DIR` holds the
dependencies' *build* directories too and a Debug tree cannot share one with a Release tree.
Sharing only the sources is possible — a `FETCHCONTENT_SOURCE_DIR_<NAME>` per dependency, pointing
into a directory populated once — and was **not** done: it would save about 1.9 GB and three
downloads on the very first run, at the price of a list of dependency names in this script that
silently goes stale when someone adds a dependency. The disk is the cheaper side of that trade
today; revisit it if the first-run download starts to hurt.

### Size, and getting it back

Measured after all four presets had been built and tested once:

| | `clang-debug` | `gcc-release` | `clang-minimal` | `clang-no-ecs` |
|---|---|---|---|---|
| `/deps` | 1.1 GB | 1.3 GB | 686 MB | 884 MB |
| `/src/build` | 526 MB | 1.1 GB | 363 MB | 450 MB |

**3.9 GB** in the dependency volume and **2.4 GB** in the source volume — **6.3 GB** for all four
presets — plus a **1.65 GB** image that every checkout on the machine shares. The minimal preset
is the small one for the obvious reason: `ENGINE_MINIMAL=ON` fetches nothing for a capability it
does not contain, so Jolt, flecs, SQLite and Recast never arrive at all.

```powershell
pwsh tools/linux-build.ps1 -Prune        # this checkout's two volumes, and it names the images
docker image rm engine-linux-ci-desktop:<tag> engine-linux-ci-headless:<tag>
```

The two images share every layer up to the split, so the headless one costs a few megabytes of
metadata rather than a second 1.65 GB.

Docker Desktop's WSL2 disk does not shrink on its own; `docker system prune` and, if it matters,
Docker Desktop's own disk-reclaim are what return the space to Windows.

### Why eight jobs — and why the worry was wrong

`-Jobs 8` on a WSL2 VM with 12 CPUs and 15.6 GB. The cap was put there for memory: Jolt and flecs
each compile a few very large translation units and the fear was that eight or twelve of those at
once would exhaust the VM. **Measured, that fear is unfounded on this tree.** Sampling the VM's
`MemTotal - MemAvailable` every two seconds:

| Run | Wall | Peak used |
|---|---|---|
| cold `linux-clang-debug` + `linux-gcc-release`, `-Jobs 8` (builds Jolt, flecs, SDL3, SQLite, Recast, Tracy from source) | 4 m 28 s + 5 m 51 s | **3.41 GB** of 15.62 |
| `-Rebuild linux-clang-debug -Jobs 8` (engine code only) | 1 m 36 s | **1.79 GB** |
| `-Rebuild linux-clang-debug -Jobs 12` (engine code only) | 1 m 20 s | **2.29 GB** |

Twelve jobs is 1.2× faster and still uses a seventh of the machine. Eight stays the default
because this box runs several agents and a Windows desktop at the same time and the build is not
the only thing that wants the cores — **not** because memory is tight. Raise it with `-Jobs` if
the machine is yours alone. The two-second sampling interval can miss a spike inside one link
step, so read these as the sustained figure rather than a guaranteed ceiling.

Note what `-Rebuild` costs and does not: it removes `build/<preset>`, but the third-party object
files live under `/deps/<preset>/<name>-build` and survive, so a rebuild recompiles the **engine**
and not Jolt.

CTest runs **serially**, as it does on the hosted runner, so a failure here is a failure there.

### `-k 0`, like ci.yml

`cmake --build --preset <p> -j <n> -- -k 0`. Ninja keeps going after a failure, so one run reports
every error in the tree instead of the first file it happened to reach. On the run that found the
`linux-clang-debug` breakage that is the difference between four errors in four modules and one
error, four times.

## The image

`tools/ci/linux.Dockerfile`. **Ubuntu 24.04** — what `ubuntu-latest` resolves to today, and what
`ci.yml` names explicitly — pinned by digest, with the same apt list the workflow installs.

**Two targets, and the preset picks one.** `desktop` is that package set and is what the four
`ci.yml` presets are built in. `headless` is the same toolchain with **none** of the X11, Wayland,
Mesa, libdrm or libudev development packages, and the `linux-server` presets
(`ENGINE_WINDOW_BACKENDS=none`) are built there — so "this tree builds where no display
development file exists" is a build that breaks if it stops being true rather than a claim in a
document. `tools/linux-build.ps1` reads the target off the preset and never asks; the image tag
carries the target as well as the Dockerfile's hash, so the two can never be confused. The long
form is in [remote Linux builds](remote-linux.md).

The **X11, Wayland, Mesa and libdrm entries are development headers only**, and in the `desktop`
target only. SDL3 is built from source by FetchContent and its configure refuses without them
unless it is told to do without ([remote-linux](remote-linux.md#the-headless-windowing-switch)
has what "refuses" actually looks like, which is not what this file used to say). **No X server
and no Wayland compositor exist in the container, and none is wanted**: `window::init()` fails,
the window and swapchain tests skip with `no display: SDL_Init(video): No available video
device`, and `engine-view` exits 3, which its end-to-end test reads as a skip. That is the same
behaviour as the headless Linux GPU runner in [self-hosted runners](self-hosted-runners.md), and
it is the intended one.

There is also no GPU: a Vulkan **loader** is present (Mesa's development packages pull it in) but
no ICD behind it, so every GPU test prints
`device unavailable: no Vulkan 1.3 driver (ICD) is installed behind the loader` and passes —
again exactly what the hosted runner does. Verified test by test on the first green run: 43 `gfx`
cases, the renderer's reference and renderer cases, the swapchain test and the `window` tests all
**skip with a reason rather than fail or hang**.

Versions, all pinned by the distribution or by an explicit version in the Dockerfile:

| Tool | Version | Why this one |
|---|---|---|
| CMake | 3.28.3 (Ubuntu) | `CMakePresets.json` declares `cmakeMinimumRequired` 3.28.0 and preset schema version 8, which is exactly CMake 3.28. This is the **floor** the presets promise — a stricter test than the hosted runner, whose image ships a newer CMake and would hide a feature we had accidentally started depending on. It also keeps the third-party `CMakeLists` files that declare a minimum below 3.5 configuring. |
| Ninja | 1.11.1 | |
| Clang | 18.1.3 | Ubuntu 24.04's default, which is what the hosted `linux-clang-*` jobs get. |
| GCC | 13.3.0 | Same, for `linux-gcc-release`. `-Wdangling-reference` exists here. |
| clang-format | 18.1.3 | Not in `ci.yml`'s list. Without it `tools/new-capability.Tests.ps1` reports its formatting cases as **skipped**, and a check that only ever skips is not a check. See the note in [08 §8.5](../plan/08-toolchain.md#85-build-system-and-ci) about which versions have been measured against the scaffold; 18 now agrees with them. |
| PowerShell | 7.4.12 | Five CTest tests are pwsh scripts (`lint.banned_patterns`, `tools.lint`, `tools.new_capability`, `docs_check`, `tools.docs_check`). Without `pwsh` `cmake/EngineTesting.cmake` does not *fail* them, it does not **register** them — so a container without it would make a green run mean less than the hosted one. |
| git, rsync | 2.43.0, 3.2.7 | `tools.docs_gate` builds throwaway repositories; rsync is the source sync. |

The base image is pinned by digest. To refresh it: `docker pull ubuntu:24.04`, take the digest from
`docker image inspect ubuntu:24.04 --format '{{index .RepoDigests 0}}'`, and edit the `FROM` line.
That changes the Dockerfile's hash, which is the image tag `linux-build.ps1` computes, so every
checkout rebuilds instead of quietly disagreeing about what "the image" is.

A non-root `build` user owns everything. uid 1001, because Ubuntu 24.04 already has an `ubuntu`
account at 1000.

### Packages and licenses

None of these is an **engine** dependency — nothing here is linked into an engine binary, and
[08 §8.8](../plan/08-toolchain.md#88-engine-license-and-dependency-policy)'s policy governs what
the engine links, not what compiles it (GCC is GPL-3, and a compiler is not a dependency). Listed
because the policy says to know what is in the build:

| Package | License |
|---|---|
| `ubuntu:24.04` base | mixed; Ubuntu's own terms per package |
| `build-essential`, `g++` (GCC 13) | GPL-3 with the GCC Runtime Library Exception; glibc LGPL-2.1 |
| `clang`, `clang-format` (LLVM 18) | Apache-2.0 with LLVM exception |
| `cmake` | BSD-3-Clause |
| `ninja-build` | Apache-2.0 |
| `powershell` (7.4 LTS, Microsoft apt repository) | MIT |
| `git`, `rsync`, `pkg-config`, `gnupg`, `xz-utils` | GPL-2 / GPL-3 |
| `curl` | curl license (MIT-style) |
| `ca-certificates` | MPL-2.0 (the Mozilla CA bundle) |
| `file` | BSD-2-Clause |
| `unzip` | Info-ZIP |
| `libx11-dev`, `libxext-dev`, `libxrandr-dev`, `libxcursor-dev`, `libxi-dev`, `libxfixes-dev`, `libxss-dev`, `libxtst-dev` | MIT/X11 |
| `libwayland-dev`, `libxkbcommon-dev`, `libdecor-0-dev` | MIT |
| `libegl1-mesa-dev`, `libgl1-mesa-dev`, `libgbm-dev`, `libdrm-dev` | MIT |
| `libudev-dev` (systemd) | LGPL-2.1 |

## Measured times

Development desktop, Docker Desktop 29.6.1, WSL2 VM with 12 CPUs and 15.6 GB, `-Jobs 8`, each run
with `-Test` so the number is build **and** suite.

| | Cold: download, configure, build, test | Warm: reconfigure, no-op build, test |
|---|---|---|
| `linux-clang-debug` (53 tests) | 4 m 28 s | 2 m 06 s |
| `linux-gcc-release` (53 tests) | 5 m 51 s | 1 m 45 s |
| `linux-clang-minimal` (43 tests) | 2 m 39 s | 0 m 48 s |
| `linux-clang-no-ecs` (49 tests) | 4 m 05 s | 2 m 10 s |
| **all four, one command** | **17 m** | **6 m 55 s** |

Building the image is a one-off **~1 min** on top of the first cold run, and nothing at all once
any checkout on the machine has built it. The source sync is **about 2 s** every run (610 files).

A warm run is almost entirely the test suite: a warm **build** of `linux-clang-debug` with no
source change is **6 s end to end including the sync**, of which the container's build step is
2 s. So a four-preset *compile* check — which is what catches five of the six entries in the table
below — costs well under a minute, and `-Test` is what turns it into the seven-minute gate.
`-Filter` narrows the suite while iterating.

The hosted matrix runs its four Linux presets in parallel on four runners, so on a good day it
finishes sooner than this does. That was never the problem: the problem was the day it finished
never.

## What the first four runs found

Every error below was in code written after hosted CI stopped answering, and every one of them was
invisible to MSVC.

| Preset | Where | What | Fix |
|---|---|---|---|
| `linux-clang-debug` | `systems/renderer` (`scene.cpp` ×2, `reference_tests.cpp`) | `-Wmissing-field-initializers`: `SceneInstance{mesh, transform}` stops short of `SceneAnimation animation`, the one member with no default initializer of its own | `SceneAnimation animation{}` in `scene.h` |
| `linux-clang-debug` | `systems/animation/src/animation.cpp` | `-Wdouble-promotion` ×12: four `tunables::Float` declarations passing `f32` literals to an `f64` constructor | double literals |
| `linux-clang-debug` | `apps/engine_view/main.cpp` | `-Wunused-but-set-variable`: `page_source_name` written in one branch and never read; the JSON summary reports the source from `StreamStats::from_file` | the dead local and its assignment removed |
| `linux-gcc-release` | `domain/ecs/src/identity.cpp` | GCC calls `flecs::entity(world.c_ptr(), flecs::entity_t{0})` **ambiguous**: it still honours C++03's rule that any integral constant expression of value zero is a null pointer constant, so the `(world_t*, const char* name, ...)` constructor is a candidate against `(const world_t*, entity_t)`. Clang and MSVC pick the id overload | hand the world over as `const flecs::world_t*`, which leaves one viable candidate |
| `linux-gcc-release` | `domain/sim` | **a real bug, not a warning.** `tiers: eight workers produce the same bytes as one, and as none` failed: `TierChange` is `u32 + u8 + u8` in 8 bytes, the test `memcmp`s the change lists, and the **two bytes of tail padding** carried whatever the allocator last left there. The Clang debug build had been handing out fresh zeroed pages and hiding it | a named, zeroed `u16 pad` member; the size table's 8/4 entry is unchanged |
| all four | `tools/new-capability.Tests.ps1` | `Join-Path ${env:ProgramFiles(x86)} ...` throws on a machine where that variable does not exist, so looking for an *optional* clang-format crashed the test instead of skipping it — the one place the suite assumed Windows | each Windows candidate guarded by its own variable |

Three of the six are the same mistake in different clothes: **MSVC's warning set has no
equivalent** and the Windows build is silent. A fourth is two compilers disagreeing about which
overload a call names. The fifth is the one worth remembering — a determinism test that compares
padding bytes is a test that passes until the allocator changes its mind, and the allocator
changed its mind when the optimizer did. The sixth is a test that assumed the machine it was
written on.

None of the six was fixed with a pragma, a `NOLINT`, a lowered warning level, or a disabled test.

## Running the documentation gate here

CI runs both documentation checks on Linux, so they are worth running where CI runs them:

```powershell
pwsh tools/linux-build.ps1 -Docs [-Base main]
```

That runs `tools/docs-gate.sh` and then `tools/docs-check.sh` inside the container, and builds
nothing. The container has no git history — see the rsync note above — so the **host's** git
produces the changed-file list and the commit messages and they are mounted in read-only at
`/gate`, which is exactly the shape `ci.yml` uses for a pull request (`--files-from`,
`--messages-from`). `-Base` defaults to `main`.

`tools/docs-check.sh` also runs as the `docs_check` CTest test on every preset, so `-Test` covers
that half; the *gate* is the diff-shaped one and has no CTest equivalent by design.

## When it goes wrong

**`docker not found` / `the Docker daemon is not reachable`.** Docker Desktop is not running, or
is on the Windows-container engine. `docker version --format '{{.Server.Os}}'` must say `linux`.

**The build is killed with no message, or a link step dies.** The WSL2 VM ran out of memory. Lower
`-Jobs`. Docker Desktop's memory allowance is in its settings, or `.wslconfig`.

**A test fails here and passes on Windows.** That is the machine doing its job; it is not a
container artefact until proven otherwise, and the table above is five examples of it being real.
`-Shell` puts you in the container with the build tree mounted and `ctest -R <name> -V` available.

**`sync: N files` where N looks wrong.** The excludes are in `$SyncExcludes` in
`tools/linux-build.ps1`. A newly added top-level directory that should not cross over goes there.

**Everything rebuilds although nothing changed.** rsync preserves timestamps (`-a`), so this
should not happen; if it does, the usual cause is a `-DFETCHCONTENT_*` or preset change that moved
a dependency's source directory, which invalidates the objects that included its headers.
