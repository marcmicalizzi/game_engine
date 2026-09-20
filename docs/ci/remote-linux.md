# Building on the Linux GPU server, over SSH

`tools/remote-build.ps1` builds and tests this tree on a real Linux machine from the Windows
desktop. It is the third Linux path here and the only one that is a *machine*:

| | Where | What it answers |
|---|---|---|
| `.github/workflows/ci.yml` | GitHub's hosted runners | the reference answer, when GitHub answers |
| [`tools/linux-build.ps1`](local-linux.md) | a container on this desktop | does it compile under Clang 18 and GCC 13, in minutes |
| **`tools/remote-build.ps1`** | the headless GPU server, over SSH | does it *run* — on a named CPU, a real kernel, that distribution's own compilers, and eventually a GPU |

The container cannot answer the third column. It is Ubuntu 24.04 on the desktop's own Comet Lake
cores, so "it built in the container" says nothing about GCC 14, about clang 22, or about a CPU
with no AVX2 — and every one of those found something the first time it was asked.

## The machine, and the rules for it

Host alias `titanxp` in this desktop's SSH config. Key-only, user `claude`, unprivileged, no sudo,
home `/home/volume/claude`, in the `video` group. Gentoo/OpenRC, kernel 6.16, **Intel Xeon
E5-2670** (Sandy Bridge-EP, 2012: AVX, and none of AVX2, FMA, BMI1/2, F16C, LZCNT or MOVBE), 16
threads, 31 GB, GCC 14.3.1, clang 22.1.8, CMake 4.3.4, Ninja 1.13.2, git 2.55. **No PowerShell, no
X11 or Wayland development files, and none wanted.** Outbound HTTPS works. It is the
`linux-pascal` runner of [self-hosted runners](self-hosted-runners.md).

**Rules, and they are not negotiable.** Work only under `~/game_engine-remote/`. Never attempt
privilege escalation. Install nothing outside that directory. Never touch another user's files.
Leave no background daemon behind. Keep disk use modest and say what it is — this tree's checkout,
build and downloaded dependencies came to **1.4 GB** per preset directory, on a volume with 8.6 TB
free.

**It reboots.** The owner is rebuilding the kernel and the NVIDIA 580 driver, so `nvidia-smi`
fails, every GPU test skips, and the machine disappears without warning. That is a fact about the
machine and never a verdict on the tree: `remote-build.ps1` retries the connection three times
with a backoff and says so in its failure message. Never try to fix anything on that machine.

## The headless windowing switch

`ENGINE_WINDOW_BACKENDS=auto|none` (`cmake/EngineGraphics.cmake`). `auto` is the default and is
SDL's own detection. `none` builds SDL with no display backend at all, and the `linux-server`
presets set it.

It exists because **both halves of what this tree used to believe about headless SDL were wrong**,
and each cost a build to find out.

### SDL's Unix configure does not fall back; it refuses — and a *partial* X11 is worse

`cmake/EngineGraphics.cmake` used to say that "without X11/Wayland headers on a Linux build
machine SDL simply has no video driver and `window::init()` reports that". SDL 3.4.16 does no such
thing. With neither X11 nor Wayland it ends the configure:

```
CMake Error at .../sdl3-src/cmake/macros.cmake:415 (message):
  SDL could not find X11 or Wayland development libraries on your system. ...
```

and `SDL_UNIX_CONSOLE_BUILD` is the documented way past it (SDL's own
`docs/README-cmake.md`). But a server is not a machine with *no* X11; it is a machine with
*some*, because something pulled `libX11` in years ago. This one has `libX11`, `libXext`, `libXi`,
`libXfixes`, `libXrender` and `libXtst`, and no `Xcursor` and no `Xrandr`. `SDL_X11` therefore
defaults ON, `CheckX11` finds the base library, and the configure dies harder and earlier:

```
CMake Error at .../sdl3-src/cmake/macros.cmake:433 (message):
  Couldn't find dependency package for XCURSOR.  Please install the needed
  packages or configure with -DSDL_X11_XCURSOR=OFF
```

**Detection cannot be trusted to mean "this machine wants windows."** It has to be told, which is
the whole argument for the switch being ours rather than SDL's.

### Turning X11 and Wayland off is not enough to get "no display"

This is the half that would have shipped a lie. SDL's video bootstrap list is tried in order until
one `create()` succeeds (`SDL_video.c`, the loop at `bootstrap[i]->create()`), and **`dummy` and
`offscreen` are in that list and always succeed**. A build with X11 and Wayland off but those two
left on reports a working video subsystem on a machine with no display: `SDL_Init(SDL_INIT_VIDEO)`
returns true, `window::init()` succeeds, and the window and swapchain tests — which skip on "no
display" — would instead *run*, against a driver that cannot make a Vulkan surface. The container
never showed this because it has real X11 headers.

So `none` switches off five things and sets one:

| | Why |
|---|---|
| `SDL_X11=OFF`, `SDL_WAYLAND=OFF` | the two real display backends |
| `SDL_KMSDRM=OFF` | also a display backend, and it wants `libgbm`, which this server does not have |
| `SDL_DUMMYVIDEO=OFF`, `SDL_OFFSCREEN=OFF` | **the pair that would otherwise stand in for a display** |
| `SDL_UNIX_CONSOLE_BUILD=ON` | SDL's own escape from the "could not find X11 or Wayland" check |

`SDL_Init(SDL_INIT_VIDEO)` then fails with `No available video device`, `window::init()` returns
false with that in `error`, and the tests skip printing `no display: SDL_Init(video): No available
video device` — **byte for byte what a full build prints on a machine whose `DISPLAY` is unset**,
which is the property the switch had to have. Events, joysticks and haptics are untouched and
still work: SDL reported `Joystick drivers: hidapi linux virtual` on this server.

### Proving "no X headers", rather than asserting it

`tools/ci/linux.Dockerfile` has two targets. `desktop` is the hosted runner's package set and is
what the four `ci.yml` presets are built in. **`headless` is the same toolchain with none of the
X11, Wayland, Mesa, libdrm or libudev development packages**, and `linux-server` and
`linux-server-debug` are built there — so the claim "this builds where no display development file
exists" is a build that fails if it stops being true, not a sentence. `tools/linux-build.ps1`
picks the target from the preset and never asks:

```powershell
tools/linux-build.ps1 -Preset linux-server -Test        # the headless image, automatically
tools/linux-build.ps1 -Preset linux-server -Shell       # and `ls /usr/include/X11` says no
```

`libudev-dev` is left out of `headless` on purpose. SDL's `CheckLibUDev` is a soft check — no
header means `HAVE_LIBUDEV_H` stays off and SDL polls `/dev/input` instead of subscribing to udev
— and "soft" was worth a build rather than a reading of somebody's CMake. The server *has*
libudev, so the container is the only place the absent case is exercised.

## The presets

| Preset | Compiler | Build type | Baseline | Backends |
|---|---|---|---|---|
| `linux-server` | GCC | RelWithDebInfo | x86-64-v2 | none |
| `linux-server-debug` | Clang | Debug | x86-64-v2 | none |

They are `linux-gcc-release-v2` and `linux-clang-debug-v2` with `ENGINE_WINDOW_BACKENDS=none`, so
they inherit [ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md)'s test baseline rather than restating
it. **v2 is not a preference, it is the CPU**: see the next section for what a v3 build does here.

## The script

```powershell
tools/remote-build.ps1 -Host titanxp                                   # build linux-server
tools/remote-build.ps1 -Host titanxp -Test                             # and run the suite
tools/remote-build.ps1 -Host titanxp -Test -Filter anim                # one module
tools/remote-build.ps1 -Host titanxp -Preset linux-server-debug -Test
tools/remote-build.ps1 -Host titanxp -Test -Fetch .\out\titanxp        # bring the artefacts back
tools/remote-build.ps1 -Host titanxp -Clean                            # drop the remote build tree
```

It syncs the working tree, configures, builds with `-k 0`, runs CTest, streams everything to the
caller's console and exits with the remote status. `-Fetch` brings back `LastTest.log` (which is
every test binary's own output), `LastTestsFailed.log`, and a `gpu.adapters --report` taken on the
remote machine.

One remote directory per local checkout, keyed by a hash of the checkout's path — the same rule
`linux-build.ps1` applies to its volumes, and for the same reason: every agent worktree is a
different tree and two of them must not share a build directory.

### The sync, and what it costs

Git for Windows ships `ssh`, `scp` and `tar`. It does **not** ship `rsync`, and rsync needs a
binary at both ends. So:

1. `git ls-files -z --cached --others --exclude-standard` — tracked files plus untracked ones git
   is not ignoring, which is what makes *uncommitted* work testable and is the reason this script
   exists rather than "push a branch and build it there".
2. `tar` packs exactly that list; `scp` copies the tarball and a newline-separated manifest of the
   same list; the remote `tar` unpacks it.
3. The remote then deletes every file under the tree that is not in the manifest, `build/`
   excepted. Tar cannot do this half, and without it a file renamed on Windows leaves its old copy
   behind, still compiling, for as long as the remote directory lives.

**Measured**, LAN, 632 files, 7.4 MB packed: **22.4 s**, repeatably, and **21 s of that is two SSH
connections**. See "Connections, not bytes" below — the transfer itself is about a second, and
sending the whole tree every time rather than a delta is not what this costs. `content/samples/`
and `ddc/` are git-ignored and never travel.

### Connections, not bytes

`ssh titanxp true` takes **10.6 s**, every time, from Windows' own OpenSSH client and from Git for
Windows' msys one alike — so it is the far side, and on a machine we may not touch it stays.
Profiled against the pieces, nothing else is close:

| | |
|---|---|
| `git ls-files -z` | 0.16 s |
| `git ls-files -s` (the modes) | 0.13 s |
| `tar` create, 7.4 MB | 0.24 s |
| **one SSH connection, no work** | **10.56 s** |
| `scp` of the tarball | 11.10 s (10.6 of it the connection) |
| remote `tar -x`, 632 files | 10.59 s (ditto) |

`ControlMaster` is the usual answer and is **not available here**: Windows OpenSSH does not
implement multiplexing, and Git for Windows' msys ssh answers a `ControlPath` with
`mux_client_request_session: read from master failed`. So the script spends connections instead:
reachability reads `nproc` on the way past, the tarball and both manifests go in one `scp`,
unpacking and configuring and building and testing and the adapter report are one `ssh`, and
`-Fetch` brings back a single tarball the run packed rather than four files. **Four connections a
run, five with `-Fetch`, against a naive eight** — a minute of wall clock. A warm no-op run is
**61.7 s**, of which 42 s is the tax.

Two more the sync had to learn:

- **The executable bit does not survive.** NTFS has no mode bits, so a `tar` built from the
  Windows working tree records 0644 for everything, `tools/docs-gate.sh` arrives
  non-executable, and `tools.docs_gate` — the one bash test a machine with no pwsh still
  registers — fails eighteen times with `Permission denied` and exit 126. The container never
  showed it, because Docker's bind mount of a Windows path reports everything as 0777. The modes
  come from `git ls-files -s`, whose first column is 100755; five files in this tree. An
  *untracked* file has no index entry and arrives 0644.
- **`find` must `-prune`, not filter.** `-not -path './build/*'` keeps build/ out of the output
  but still walks a gigabyte of object files; the first version of the stale-file sweep spent 11 s
  a sync statting things it then discarded.

Three Windows details that each cost a run to find, recorded so the next person does not:

- **`tar` is ambiguous on Windows.** Windows 11 ships bsdtar as `C:\Windows\System32\tar.exe` and
  it comes first on PATH in PowerShell; Git for Windows ships GNU tar, which is what Git Bash
  finds. GNU tar reads `C:\Users\...` as *the host `C`* and needs `--force-local`; bsdtar does not
  accept that flag at all. The script asks `tar --version` which one it has.
- **`scp` does not expand `$HOME`.** Since OpenSSH moved scp onto the SFTP protocol,
  `host:$HOME/x` is a literal path with a dollar sign in it. SFTP starts in the home directory, so
  scp gets a relative path and only the `ssh` command lines carry `$HOME`.
- **PowerShell has no input redirection.** `<` is a reserved operator, so a tarball goes over as a
  file rather than down a pipe; and a PowerShell double-quoted here-string expands `$f` and
  `$(...)` before `sh` ever sees them, so the remote script is a single-quoted here-string with
  its three values substituted afterwards.

### What the suite is missing there, and why

`cmake/EngineTesting.cmake` does not *fail* a pwsh test on a machine without pwsh, it does not
**register** it. This machine has no PowerShell and is not getting one, so five tests do not exist
in a remote run:

| Not registered | What it checks |
|---|---|
| `lint.banned_patterns` | the container rule, the flecs confinement, the temp-path rule |
| `tools.lint` | that lint's own rules still match |
| `tools.new_capability` | ADR-0027's scaffold |
| `docs_check` | every module has a page, every ADR is indexed, every link resolves |
| `tools.docs_check` | that check's own rules |

All five read files and are machine-independent, so the container and the hosted runner cover them
completely; nothing is *only* checked here. `tools.docs_gate` **is** registered — it is bash, and
the machine has bash and git. Everything else in the suite runs: the remote CTest total matches
the container's `linux-gcc-release-v2` count minus exactly those five.

### FetchContent, cold, from this machine

Every pinned URL is reachable from this machine: doctest, meshoptimizer, Vulkan-Headers, volk,
VMA, SDL3, Tracy, flecs, Jolt, SQLite and Recast all clone over HTTPS with no proxy
configuration, first try. The first seven alone are **299 MB** and took 1 m 46 s to fetch; a
configure that still had four to clone finished in 1 m 37 s, so **a genuinely cold configure is
two to three minutes** and effectively all of it is `git clone`. A reconfigure with everything
present is **6 s**, because `FETCHCONTENT_UPDATES_DISCONNECTED=ON` stops the git-based
dependencies re-fetching a tag that is pinned anyway. The sources live in
`~/game_engine-remote/<id>/deps/<preset>/`, outside the build tree, so `-Clean` costs a compile
and not a download.

## What a v3 build does on this CPU, and the hole it found

`platform::require_cpu_baseline()` (ADR-0031 decision 5) had only ever been unit-tested. On real
Sandy Bridge it does exactly what it promises. `engine-cli` from a `linux-gcc-release` build:

```
engine: this build needs x86-64-v3 and this CPU has no AVX2, FMA, BMI1, BMI2, F16C, LZCNT,
MOVBE (Intel(R) Xeon(R) CPU E5-2670 0 @ 2.60GHz); rebuild with -DENGINE_CPU_BASELINE=v2
(preset msvc-release-v2 or linux-gcc-release-v2), see docs/adr/0031-minimum-cpu-x86-64-v3.md
```

exit **78**, on stdout and stderr both, one line, naming the build's baseline, the missing
features, the CPU and the way out. That is the decision working.

**The hole is next to it.** A v3 build never gets as far as running `engine-cli`, because the
*build* runs a binary of its own:

```
[126/976] schemac: schemas
FAILED: [code=260] schemas/generated/schemas/include/schemas/provenance.h ...
```

260 is ninja's 256 + SIGILL. `tools/schemac` is a standalone, standard-library-only tool that
links no engine module — by design, so that it builds before `core/` exists — and therefore has no
`platform::require_cpu_baseline()` to call. Run by hand it is `Illegal instruction (core dumped)`,
exit 132, with nothing printed. Six minutes of compiling for a build that could never have worked.

`cmake/EngineCpuBaseline.cmake` now refuses at **configure** time instead: a native build whose
`/proc/cpuinfo` does not list `avx2` while `ENGINE_CPU_BASELINE` is `v3` stops with a message
naming the v2 presets. It is Linux-only (that is where `/proc/cpuinfo` costs nothing and where the
only sub-v3 machine is), skipped when cross-compiling, and `-DENGINE_ALLOW_UNRUNNABLE_BASELINE=ON`
is the way past it for someone building here to run elsewhere.

**Left for the coordinator, because it is an ADR-level call and not a build-script one:** whether
build-time *host tools* should carry the target's instruction-set baseline at all. `schemac` is
generated code's generator; it never ships, and compiling it for the product's ISA is what turned
"this machine is below the baseline" into a SIGILL rather than a sentence. ADR-0031 forbids a
second caller of `engine_strip_cpu_baseline()` in terms that were aimed at exempting a *module*,
and a host tool is arguably a different category — but that is a decision to record, not to make
in `cmake/`.

## Measured times

Xeon E5-2670, 16 threads, `-Jobs 16`. The machine's load is noted because it is shared with the
owner's work; these are wall-clock upper bounds, not costs
([bench](../subsystems/bench.md#measuring-on-a-shared-machine)).

| | `linux-server` (GCC 14.3) | Load average at the start |
|---|---|---|
| Sync, 632 files / 7.4 MB packed | 22.4 s (21 s of it two SSH connections) | — |
| Configure with **four of eleven dependencies still to clone** | 1 m 37 s | 0.9 |
| Configure with every dependency already in `deps/` | 1 m 02 s | 1.5 |
| Cold build, 972 targets, `-j 16` | **9 m 03 s** (114 m of CPU) | 1.7 |
| Warm reconfigure | 6.1 s | — |
| Warm no-op build | 0.15 s | — |
| Full CTest suite, 49 tests | **3 m 30 s** | 9.9 |
| A warm end-to-end run, nothing to do | 61.7 s | — |

`linux-server-debug` (clang 22.1.8, Debug) tests in **6 m 03 s** — asserts and iterator checking,
on 2012 cores.

**Disk: 2.1 GB for one preset, 3.6 GB for both** — checkout and build trees 1.5 GB, downloaded
dependencies 2.2 GB — under `~/game_engine-remote/<checkout-id>/`, on a volume with 8.6 TB free.
`-Clean` drops the build tree and keeps the dependencies. Nothing is written anywhere else on that
machine and no daemon is left running.

## What GCC 14 and clang 22 found

The tree had never been compiled by either. Between them they found **one** thing, and it was in
neither the engine nor its warnings.

**GCC 14.3.1: nothing.** 972 targets with `-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast
-Wcast-align -Wdouble-promotion -Wformat=2` and `-Werror`, and the only two warnings in the whole
log are in Tracy's own sources (`TracyProfiler.cpp` ignoring `pipe()`, `TracySysPower.cpp`
ignoring `fscanf()`, both `-Wunused-result`), which is third-party code compiled as such. Nothing
in `core/`, `foundation/`, `domain/`, `systems/`, `apps/` or `tools/` produced a diagnostic. That
is a better result than GCC 13 in the container had any right to predict and is worth recording as
a negative: **a GCC major version was not, this time, a source of new warnings.**

**clang 22.1.8: one, 1,797 times, and it is doctest's.** `-Wc2y-extensions` is new in clang 22 and
fires on `__COUNTER__`, which C standardized only in C2y; `DOCTEST_ANONYMOUS(x)` is
`DOCTEST_CAT(x, __COUNTER__)`, so every `TEST_CASE` in the tree tripped it and `-Werror` ended the
build. Every occurrence was in a `tests/` file and **none in engine code**. Marking doctest
`SYSTEM` does not help: clang suppresses diagnostics *inside* a system header and reports this one
at the expansion site, which is our `.cpp`. The fix is in `cmake/EngineTesting.cmake` and is as
narrow as it can be — one diagnostic, on Clang 22 and newer only, on `engine_test_main`'s
INTERFACE, so it reaches test executables and nothing else in the tree. An engine translation unit
that ever uses `__COUNTER__` will still be warned about.

**CMake 4.3.4 configures this tree**, which the container (pinned at 3.28, the floor the presets
promise) cannot tell you. Worth knowing before a rolling distribution is treated as a risk.

### And one thing that is not a compiler's fault

On this CPU the engine found its own edge, described above: a v3 build cannot run here, the
startup check says so correctly for every app, and `tools/schemac` — which the build itself runs
— had no check to say it with. That is now a configure-time refusal.

## When it goes wrong

- **`is not reachable over SSH after three attempts`** — the machine is rebooting. It is not a
  build failure. Try `ssh titanxp uptime` by hand.
- **`Couldn't find dependency package for XCURSOR`** — an `auto` preset on a machine with a
  partial X11. Use `linux-server`.
- **`[code=260]` from `schemac`** — a v3 preset on this CPU; see above. The configure should have
  stopped you first, so if you see this, say so.
- **A test that fails only here** — check `LastTest.log` from `-Fetch` first. The five pwsh tests
  above are *absent*, not failing; a run that reports fewer tests than the container is expected.
