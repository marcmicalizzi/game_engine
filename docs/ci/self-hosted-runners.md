# Self-hosted GPU runners

> **Linux builds on a Windows machine** are a different thing and live in
> [local Linux builds](local-linux.md): a container on the development desktop that runs the four
> Linux presets `ci.yml` builds, in minutes, without a runner and without GitHub. No GPU, no
> display — it is the compiler-and-CPU half of the gate. The two machines below are the other
> half, the only ones here that execute a shader.

Hosted GitHub runners have a Vulkan loader and nothing behind it. `enumerate_adapters` says so
politely, every GPU test prints `device unavailable` and passes, and `.github/workflows/ci.yml` stays
a compiler and CPU gate: four presets, every push and pull request, no picture ever drawn. That is
the right trade for the gate, but it means nothing in CI executes a shader.

Two machines the project owner keeps close that gap. Both are the baseline tier of
[04 §4.1](../plan/04-renderer.md#41-goals-targets-non-goals): no mesh shaders, no ray tracing, the hardware the engine promises
30 fps at reduced settings on. They are the cheapest way to find out that a shader compiles
everywhere and runs only on the development GPU, and they are the answer to risk 2 in
[10 §10.4](../plan/10-roadmap-risks.md#104-major-technical-risks), where one RTX 5090 carries every
renderer change.

| Runner | Machine | Preset | Labels |
|---|---|---|---|
| `windows-maxwell` | Windows 11 desktop, Intel i7-980 (Westmere, 2010: SSE4.2, no AVX) **until the owner's board swap**, NVIDIA GeForce GTX Titan X (Maxwell, 12 GB), 2560×1440 display | `msvc-release-v2` | `self-hosted`, `windows`, `gpu`, `maxwell` |
| `linux-pascal` | Linux server, headless (no X11, no Wayland), Intel Xeon E5-2670 (Sandy Bridge-EP, 2012: AVX, **no** AVX2, FMA or BMI2), NVIDIA GeForce GTX Titan Xp (Pascal, 12 GB) | `linux-server-debug` | `self-hosted`, `linux`, `gpu`, `pascal`, `headless` |

The two presets are deliberately different in everything but their baseline: a release build on the
slow Westmere box, because a release build is what a 30 fps target is measured in and a debug build
of this engine on six 2010 cores is not a weekly proposition; a Clang debug build on the server,
because asserts and iterator checking are worth more than speed there and it is the preset a Linux
contributor runs.

**`linux-server-debug` is `linux-clang-debug-v2` plus `ENGINE_WINDOW_BACKENDS=none`**, and the
server needs the second half as much as the first. That machine has a *partial* X11 — `libX11` and
four of its extensions, no `Xcursor`, no `Xrandr` — which is enough for SDL's configure to select
the X11 backend and then stop with `Couldn't find dependency package for XCURSOR`. Detection
cannot be trusted there; it has to be told. `linux-server` is the same switch over
`linux-gcc-release-v2`, for a release build on that machine.
[Remote Linux builds](remote-linux.md) has the whole argument, the transcripts, and
`tools/remote-build.ps1`, which is how this tree is built there today without a runner registered
at all.

## Both GPU runners are x86-64-v2, and that is what the `-v2` presets are for

[ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md) made **x86-64-v3** the promised minimum CPU on
2026-09-19: AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE. Neither of these machines has it.

| Machine | CPU | Has | Missing from v3 |
|---|---|---|---|
| `windows-maxwell` | i7-980, Westmere, 2010 | SSE4.2, POPCNT | AVX, AVX2, FMA, BMI1, BMI2, F16C, LZCNT, MOVBE |
| `linux-pascal` | Xeon E5-2670, Sandy Bridge-EP, 2012 | SSE4.2, POPCNT, **AVX** | AVX2, FMA, BMI1, BMI2, F16C, LZCNT, MOVBE |

So `-DENGINE_CPU_BASELINE=v2` is not a temporary exception for one garage box: it is the **test
baseline of the project's only two GPU machines that are not the development desktop**, and it
stays as long as either of them does. The Sandy Bridge server in particular is the only Linux GPU
runner there is, so retiring v2 would mean having no Linux GPU coverage at all.

A v3 binary on either machine would trap on its first AVX2 instruction. It does not:
`platform::require_cpu_baseline()` refuses first, with one line
([platform](../subsystems/platform.md#the-startup-check-and-why-this-module-gives-up-the-arch-flag)).
On the Xeon that line reads

```
engine: this build needs x86-64-v3 and this CPU has no AVX2, FMA, BMI1, BMI2, F16C, LZCNT, MOVBE
(Intel(R) Xeon(R) CPU E5-2670 0 @ 2.60GHz); rebuild with -DENGINE_CPU_BASELINE=v2 ...
```

and the word to notice is the one that is **absent**: AVX is not in the list, because Sandy Bridge
has it. A unit test pins that exact string (`core/platform/tests/cpu_baseline_tests.cpp`), so the
message the runner's owner would read is checked on every build rather than the first time somebody
uses the wrong preset. A run that builds `msvc-release` or `linux-clang-debug` on one of these
machines is therefore not a mysterious failure; the log's first line names the flag to change.

**There is no intermediate level for the Xeon, deliberately.** It has AVX and the Westmere does
not, so an "AVX but not AVX2" tier between them is technically available — and it is not worth
having. x86-64 defines v2, v3 and v4 and nothing between, so the tier would have to be invented and
named here; it would add a third column to every build matrix, a third `ENGINE_CPU_BASELINE` value
to test, and a third set of Jolt options, all for one machine. And what it would buy is small: AVX
without FMA, without AVX2's integer operations and without BMI is 256-bit float arithmetic and
little else, which is the half of v3 that matters least to this engine's hot paths. Two levels, one
of which is only ever a *test* configuration, keeps the matrix small — which is the whole reason
the microarchitecture levels are named rather than assembled from `-m` flags.

**What retires the `-v2` presets** is both machines going, not either: the owner's Westmere board
swap to a 4th-generation-or-newer part, *and* a replacement for the Sandy Bridge server. When that
happens, change these two rows back to `msvc-release` and `linux-clang-debug`, delete the three
`*-v2` presets and the `v2` branch of `cmake/EngineCpuBaseline.cmake`, and say so in a new ADR.
The startup check stays either way: it is about users' machines, not about ours.

Both of those machines have a toolchain on them. The next section is for the ones that never will.

## Trying the engine on a machine with no toolchain

A runner is a commitment: Visual Studio, Git, PowerShell 7, outbound HTTPS for the dependency
downloads, and a build directory that has to survive between runs. That is the right price for a
machine that will report every week. It is far too high for the question *"does this even start on
a Surface Pro?"* — a machine with 8 GB of RAM, a few gigabytes free, and no reason to ever carry a
compiler.

So there is a second, much cheaper path: **one zip, one double-click, one file back**.

```powershell
pwsh tools/package-tests.ps1 -Preset msvc-release            # on a machine that can build
pwsh tools/package-tests.ps1 -Preset msvc-release -Out D:\share\engine-tests.zip -WithSamples
```

`tools/package-tests.ps1` writes `build/<preset>/engine-tests-<preset>-<commit>.zip`: **about 36 MB
zipped and 109 MB unpacked** without the sample models, which is the size a USB stick or a home
network does not think about. In it:

- `bin/` — every test, bench and app executable the preset built, flattened into one directory.
  **No PDBs** unless `-WithSymbols`: for `msvc-release` the 56 executables are 109 MB and their
  PDBs are 751 MB, which is most of the free disk on the machine this exists for, so symbols are
  what you send *after* a stack trace asks for them. Flattened because `bundle.json` names each
  test by its executable and the layout under `build/` is a CMake detail nobody at the far end
  should have to learn.
- `content/input-logs/` — the device-log corpus `foundation/input` replays. **Bundled rather than
  skipped**: it is 176 KB and it is the only description of those three devices this repository
  has, so a machine that cannot replay it is telling us something. Its `sessions/` directory
  comes with it: engine-view's synthetic session and the trajectory it flies, which is how a
  machine nobody here can log in to shows that its compiler flies a recording to the same place.
  `content/input-maps/` (the interactive camera's default bindings, checked against the compiled
  ones), `content/roles/` (the five named role configurations, which the protocol test loads) and
  `content/test-scenes/` ride along at a few kilobytes; `content/migration-corpus/` — one save
  game per version of the save format, about 110 KB each, which engine-cli's corpus test loads
  and holds to state hashes that must be the same on every machine — rides along too, for the
  same reason the device logs do; `content/samples/` only with `-WithSamples`.
- `bundle.json` — the commit, the branch, the preset, the CPU baseline (read out of the build's
  `CMakeCache.txt`, or `unknown` where that switch does not exist yet), and the **test list taken
  from `ctest --show-only=json-v1`** rather than from a glob: each test's executable, arguments,
  labels, `RESOURCE_LOCK` and timeout, because ctest is the only thing that knows them and the
  runner has to serialize the GPU end-to-end tests exactly the way a ctest run does.
- `run-tests.ps1`, `run-tests.cmd`, `README.txt`.

**On the far end**: unzip the folder, double-click `run-tests.cmd`, send back `results.json`,
`results.txt` and `adapters.json`. Nothing is installed and nothing outside the folder is changed.
The window stays open at the end so a double-click does not flash and vanish.

`run-tests.ps1` is **Windows PowerShell 5.1** and uses nothing from PowerShell 7, no CMake, no
ctest and no Visual Studio, because the machines it is for have none of them. It does four things
in order:

1. **Checks the Visual C++ runtime by DLL name** — `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`,
   `MSVCP140.dll` — and, when one is missing, names the installer (*Microsoft Visual C++ 2015-2022
   Redistributable (x64)*) and its URL and stops with exit code 2. This is the first step for a
   reason: without it Windows fails every load with `0xC0000135` and prints nothing a person can
   act on, so all fifty tests "fail" for the same invisible cause and the report is worthless.
2. **Records the machine**: CPU name, cores and threads, RAM, OS build and architecture, free disk,
   and every GPU with its driver version and date.
3. **Runs `bin\engine-cli gpu.adapters --report adapters.json`** and prints each adapter's verdict.
   That file is the single most useful thing that comes back: the whole requirements table checked
   against every device on the machine, with a plain-words verdict per device
   ([gfx](../subsystems/gfx.md#what-a-device-has-to-have)).
4. **Runs every test in `bundle.json`**, each with its own timeout (the manifest's, or
   `-DefaultTimeoutSeconds`, 900), each with its own stdout and stderr kept under `logs/`, and with
   two tests that share a resource lock never overlapping. `-Jobs 1` is the default, because these
   machines are small; `-Jobs <n>` runs the lock-free tests `n` at a time and still keeps the
   `e2e_apps` ones to themselves. `-Filter <regex>` runs a subset and `-ListOnly` prints the list
   without running anything.

`results.json` carries the machine, the bundle's identity, the adapter step, and every test's exit
code, wall time, doctest counts and log path; `results.txt` is the same thing readable, with the
**last 50 lines of anything that failed** inline so the first reply does not have to be "send me the
log". **A test that found no GPU is not a failure**: it exits 0 with `device unavailable` in its
output, and the runner counts it separately (`gpu_unavailable`) rather than as a pass that means
more than it does. The script's exit code is 0 when nothing failed, 1 when something did, 2 when the
bundle could not be run at all.

**Six CTest tests are deliberately left out**, and `bundle.json` lists them with the reason so the
far end can see it was on purpose: `lint.banned_patterns`, `tools.lint`, `tools.new_capability`,
`docs_check` and `tools.docs_check` run `pwsh` over the source tree, and `tools.docs_gate` runs
`bash` over a throwaway git repository. They are checks on the *repository*, not on the machine, and
CI runs them on every push. Everything else — 47 tests, including every GPU suite and all five
end-to-end app suites — is in the bundle.

**The one thing the bundle does not carry is the shader manifest**, `build/<preset>/shaders/manifest.json`.
It names absolute paths into the build tree and the pinned Slang compiler under `_deps`, which is a
toolchain and not a test fixture — and a bundle exists precisely for machines without one. The two
`shader_library_tests.cpp` cases that need it say so and stop; the SPIR-V reflection of every shipped
shader, which is what those cases are really about, runs from the embedded bytes and needs no file.

**Two things Windows PowerShell 5.1 does that cost an afternoon**, written down because the next
person to touch `run-tests.ps1` will hit both and neither announces itself.

- **`Start-Process -PassThru` hands back a `Process` whose `ExitCode` is empty** once the child has
  gone, because nothing kept its native handle open. `HasExited` says `True`, `ExitCode` says
  nothing, and the runner therefore reported **every test as failed** with a blank exit code while
  their logs all ended in `[doctest] Status: SUCCESS!`. Reading `$proc.Handle` once, while the
  process is still alive, is what makes the exit code answer afterwards; `Hold-ProcessHandle` does
  exactly that and is called at every spawn.
- **Every string `Get-Content` returns carries note properties** — `PSPath`, `PSDrive`,
  `PSProvider` — and `PSDrive` leads to a provider whose `Drives` collection leads back to the
  drive. Put one of those lines into an object and hand it to `ConvertTo-Json -Depth 12` and it
  walks that cycle: measured here, writing `results.json` for two failed tests reached a **6 GB
  working set and never finished**. The failure mode is a script that hangs *after* every test has
  passed, which reads like a deadlock in the tests and is not one. `Read-Tail` casts to `[string[]]`,
  which drops the wrapper and keeps the text.

**Why the bundle can be trusted after being verified on the machine that built it.** The tests know
their executables and their data as absolute paths CMake baked in (`ENGINE_APP_PATH`,
`ENGINE_HOST_PATH`, `ENGINE_SCHEMAC_PATH`, `ENGINE_SOURCE_DIR`, `ENGINE_SHADER_MANIFEST`). On the
target those paths name directories that do not exist — and on the *build* machine they name
directories that do, so a bundle that merely *fell back* to its own copies would pass here by
accident and prove nothing. `tests/support/test_paths.h` therefore makes `ENGINE_BUNDLE_ROOT`, which
`run-tests.ps1` sets, **take over** rather than serve as a fallback: when it is set, every path is
resolved inside the bundle and nothing consults the compiled-in one. That is what makes the
out-of-tree run a real test of the bundle.

**A self-hosted runner could use the same bundle later**, and the shape is worth writing down now
even though no workflow does it: a `package` job on a machine with a toolchain runs
`tools/package-tests.ps1` and uploads the zip as an artifact, and a `download-and-test` job on a
runner with `runs-on: [self-hosted, windows, gpu]` downloads it, unzips it and runs
`run-tests.ps1 -Jobs 1`, uploading `results.json` and `adapters.json`. The runner would then need no
compiler, no CMake and no checkout — only the Actions agent — which is the difference between "this
machine can host a runner" and "this machine has a developer's toolchain on it". The workflows are
not changed now, because `gpu.yml`'s two machines build anyway and the download path would add a
second way for a run to be wrong without covering anything the first one does not.

## What runs and what skips

`tools/ci/gpu-smoke.ps1` and `tools/ci/gpu-smoke.sh` build the preset through `tools/dev.ps1`, print
`engine-cli gpu.adapters` — the machine's capability report, in the same schema-typed form the
protocol serves — and then run the whole CTest suite (the Windows one under the machine-wide GPU
lock; see [More than one suite on one machine](#more-than-one-suite-on-one-machine)). The GPU tests decide for themselves what they
can do, so the suite passes on every machine and the log is what says how much of it meant anything.
A fourth step then renders the extreme resolutions of [04 §4.6](../plan/04-renderer.md#46-extreme-displays)
and writes `build/<preset>/gpu-smoke.json`; see [The extreme resolutions](#the-extreme-resolutions)
below for what it does and what it treats as a skip.

Neither GPU has `VK_EXT_mesh_shader` or `VK_KHR_ray_query`, and no suite treats that as a reason
to skip what it can still check: the baseline tier
([ADR-0024](../adr/0024-hardware-rasterization-first.md)) draws the same clusters into the same
visibility buffer through the vertex shader, so **every GPU case about the picture runs on both
machines**, through that path.

- `domain/gfx`, since 2026-09-24: the material resolve's numerical tests against the
  double-precision reference (`shading_tests.cpp`, `attributes_tests.cpp`), the GPU cull test
  against its CPU reference (`cull_tests.cpp`), two-pass occlusion culling (`occlusion_tests.cpp`,
  and the scene test's occlusion half), the visibility test, which is also the software
  rasterizer's only coverage (`visibility_tests.cpp`), the cone and vertex-path tests, the scene
  test's pair cull and instances, and the identity deformer. They draw through
  `gfx_test::ClusterRaster` (`domain/gfx/tests/raster_path.h`), the mesh path where the device has
  one and the vertex path where it does not ([gfx](../subsystems/gfx.md), "On a device without
  mesh shaders or ray queries").
- `systems/renderer` and the protocol's `render.*` methods, since 2026-09-23: `RenderSettings{}`
  and `"raster":"hw"` resolve to the vertex path there, every case about the picture draws through
  it, and only the mesh-path and ray halves skip, each naming the missing extension in the words of
  the adapter's verdict ([the first run on the Titan Xp](#the-first-run-on-the-titan-xp) is what
  that took).

What skips is what is about a path the card does not have, each case naming the row it lacks: in
`gfx` the mesh-shader test (`mesh_shader_tests.cpp`, `VK_EXT_mesh_shader`, because it is about
the mesh stage itself), five ray cases (`VK_KHR_ray_query`: ray-query visibility, the ray-traced
shadows, the wave deformer, and with `VK_NV_cluster_acceleration_structure` the cluster
structures and the cluster templates), and the mesh and ray halves of the cases that compare
paths. A skipped case prints `skipped: <adapter> has no <rows>` and a skipped half `part skipped:
<what>: ...`, and doctest counts a skipped case as passed — so `grep -c "MESSAGE: skipped:"` over
`LastTest.log` is how many cases checked nothing, and the green summary line is not.
`ENGINE_GFX_TEST_DEVICE=titanxp` runs the `gfx` suite on any GPU with this card's absences, so what
it will skip here can be seen before a build ever reaches it.

Under those cases the `gfx` suite covers everything else the baseline tier stands on: device
creation with the required feature chain, compute, frames in flight, the render graph, bindless
descriptors, the raster pass, capture, the shader library and its SPIR-V reflection of every
shipped shader including the resolve, the CPU LOD reference in `geometry`, and every CPU test in
the tree. On the Westmere box that CPU side is the point as much as
the GPU: it is the machine that proves the **v2** build carries no instruction above x86-64-v2,
which since [ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md) is a claim about one preset rather
than about the tree. The claim about the tree is smaller and is checked elsewhere: a v3 binary
that reaches a CPU without AVX2 says so and exits 78 rather than trapping.

The headless server has no display, so `window::init()` fails there, the `window` tests and the
swapchain test skip, and `engine-view` exits 3, which its test treats as a skip. The Windows runner
does the same when it runs as a service (see Troubleshooting).

Two things the first run on each machine settles, because no one here has run this engine on Maxwell
or Pascal yet — and **`gpu.adapters` now answers both of them by itself**, before a single test runs.
Its result carries the whole requirements table checked against each device and a verdict per device
([gfx](../subsystems/gfx.md#what-a-device-has-to-have)): `verdict.usable` says whether
`Device::create()` would succeed, `verdict.blocking` says in plain words why not, and
`verdict.degraded` says what will not run although it does — including the one that decides these two
machines, that the visibility buffer is a 64-bit atomic max per pixel and every rasterizer writes it,
so a card without `shaderBufferInt64Atomics` and without mesh shaders cannot draw a frame at all.
Every picture case of `gfx` skips without that feature (`skipped: ... has no
shaderBufferInt64Atomics`); the adapters report says in advance whether it will. If a Required row
fails, every GPU test reports the device unavailable with the reason (`gfx`: `skipped: device
unavailable: ...`) and still passes, which is why the log matters more than the exit status on
these two.

## Machine prerequisites

### Windows 11 desktop

- **Visual Studio 2026 Build Tools with the "Desktop development with C++" workload**, or the full
  Visual Studio. `tools/dev.ps1` finds it through `vswhere` and uses the CMake and Ninja bundled
  with it, so nothing else has to be on `PATH`. Nothing on the machine needs a developer prompt.
- **Git for Windows.**
- **PowerShell 7** (`pwsh`). Windows PowerShell 5.1 is not enough; `tools/dev.ps1` is a PowerShell 7
  script.
- **The NVIDIA display driver.** It ships `vulkan-1.dll` and registers the ICD under
  `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`, which is the whole Vulkan runtime the engine needs — volk
  loads the loader at run time and the Vulkan headers come through FetchContent. **The Vulkan SDK is
  not required**; install it only if you want validation layers on that machine. Maxwell is on
  NVIDIA's legacy branch now, so the newest driver is not the one to install here: take the most
  recent branch that still lists the GTX Titan X as supported, and check that it advertises Vulkan
  1.3, which `gpu.adapters` prints as `api_version`.
- Outbound HTTPS. The first configure fetches Vulkan-Headers, volk, VMA, meshoptimizer, SDL3, and
  the pinned Slang release.

### Linux server

- **A compiler, Ninja and nothing for a display.** The `linux-server*` presets set
  `ENGINE_WINDOW_BACKENDS=none`, so SDL3 is built with no X11, Wayland, KMS/DRM, dummy or
  offscreen video driver and **none of the development packages this section used to list are
  needed**:

  ```sh
  sudo apt-get update
  sudo apt-get install -y ninja-build clang g++    # and that is the whole list
  ```

  Installing the X11 and Wayland headers instead is the *worse* option on a server, not merely a
  redundant one: a partial set — which is what a server that had `libX11` pulled in years ago
  actually has — makes SDL's configure select X11 and then fail on the first extension it cannot
  find. The window and swapchain tests skip at run time either way, with the same reason string.
  See [remote Linux builds](remote-linux.md#the-headless-windowing-switch).
- **`cmake` 3.28 or newer** (`CMakePresets.json` requires it) and **`git`**. The hosted runners get
  CMake for free; a bare server does not. CMake 4.3 configures this tree too — measured on the
  Gentoo server — so a rolling distribution is not a problem.
- **`pwsh`**, PowerShell 7, because `tools/dev.ps1` is the build entry point on both platforms.
  Microsoft's `.deb` or the `powershell` snap both work. Without it five CTest tests are not
  *failed*, they are not **registered** (`cmake/EngineTesting.cmake`); `remote-build.ps1` runs
  against a machine with no pwsh on purpose and
  [names the five](remote-linux.md#what-the-suite-is-missing-there-and-why).
- **`curl` and `tar`**, for `tools/ci/install-runner.sh`.
- **The NVIDIA driver, installed headless, with its Vulkan ICD.** No X server and no Wayland
  compositor are needed: NVIDIA's Vulkan implementation works on a machine that never starts a
  display server. What matters is that three pieces are present —
  - the kernel module (`nvidia-smi` lists the Titan Xp),
  - the Vulkan **loader**, `libvulkan.so.1`, from the `libvulkan1` package. This is *not* part of the
    driver on Linux, unlike Windows, and forgetting it is the most common way to end up with a
    working GPU and no Vulkan.
  - the driver's **ICD manifest**, `/usr/share/vulkan/icd.d/nvidia_icd.json`, which the distribution
    driver packages install and some `.run` installations do not.

  A driver installed without the ICD manifest leaves `vulkaninfo` reporting zero devices while
  `nvidia-smi` looks perfectly healthy. Pascal is on NVIDIA's legacy branch too, so the same rule
  applies as on the desktop: the most recent branch that still lists the GTX Titan Xp, advertising
  Vulkan 1.3.
- Outbound HTTPS, for the same FetchContent downloads.

### Verifying Vulkan

On Linux, `apt install vulkan-tools` and:

```sh
vulkaninfo --summary          # lists the devices the loader can reach
```

On Windows `vulkaninfo.exe` comes with the Vulkan SDK rather than the driver, so use the engine's own
report instead. It works on both, and needs nothing installed beyond a built tree:

```powershell
build/msvc-release/bin/engine-cli gpu.adapters                      # Windows
build/msvc-release/bin/engine-cli gpu.adapters --report gpu.json    # the same, as a file to send back
build/linux-server-debug/bin/engine-cli gpu.adapters                # Linux
```

`"available": true` with the GPU's name, `"tier": "raster"`, `VK_EXT_mesh_shader: false` and
`"verdict": {"usable": true, "tier": "raster"}` is what a correctly set up baseline machine looks
like. `"available": false` carries the reason in `"error"`, and a device the renderer would refuse
carries its reasons in `verdict.blocking` — one sentence per unmet requirement, each naming what in
the engine needs it.
The smoke scripts print this on every run and warn when it comes back false; `-RequireAdapters` /
`--require-adapters` turns that warning into a failure, which is worth using once the machines are
known good, so a driver that stops working does not look like a quiet pass.

### The Titan Xp, the moment `nvidia-smi` works again

From 2026-09-20 that machine's driver was being rebuilt: `nvidia-smi` failed with *"couldn't
communicate with the NVIDIA driver"*, `gpu.adapters` answered `"available": false`, and **every GPU
test on it skipped** — which the suite reports as passing, so a green remote run then said nothing
whatever about the GPU. It came back on 2026-09-23; [the first run](#the-first-run-on-the-titan-xp)
below is what it found. This is the sequence to run whenever the driver changes, in this order,
before anything is read into a green result. It needs no runner registered and no `sudo`; it is
[remote Linux builds](remote-linux.md) from the Windows desktop.

```powershell
# 1. The adapter report first, on its own. If this is not right, nothing below means anything.
pwsh tools/remote-build.ps1 -Host titanxp -Preset linux-server-debug -Fetch .\out\titanxp
#    Read out\titanxp\adapters.json:  "available": true, the Titan Xp by name,
#    "tier": "raster", VK_EXT_mesh_shader false, "verdict": {"usable": true, "tier": "raster"}.
#    A false here is a driver problem on that machine and not a tree problem — stop and say so.

# 2. Then the GPU suites, which until now have only ever skipped there.
pwsh tools/remote-build.ps1 -Host titanxp -Preset linux-server-debug -Test `
     -Filter 'gfx|renderer|swapchain|skin' -Fetch .\out\titanxp
#    LastTest.log is every test binary's own output; a case that still says
#    "device unavailable: no Vulkan 1.3 driver (ICD) is installed behind the loader"
#    is the loader or the ICD manifest, not the kernel module (see above).

# 3. Then the whole suite, so the GPU cases are seen beside everything else.
pwsh tools/remote-build.ps1 -Host titanxp -Preset linux-server-debug -Test -Fetch .\out\titanxp

# 4. And the release preset, which is what a 30 fps claim would ever be measured in.
pwsh tools/remote-build.ps1 -Host titanxp -Preset linux-server -Test -Fetch .\out\titanxp
```

Three things to expect the first time, because none of them has ever run on a Pascal card here:
the **window and swapchain** tests keep skipping — there is still no display server and
`ENGINE_WINDOW_BACKENDS=none` means there is no video driver to make a surface with, so GPU work
reaches the device through headless paths only; the **mesh-shader and ray-tracing** cases skip on
capability, as they do on the Maxwell box; and this is the **first Vulkan driver this engine has
met that is not NVIDIA-on-Windows**, so a validation-layer complaint or a format the desktop's
5090 never refused is the interesting kind of failure and is worth an entry in
[Troubleshooting](#troubleshooting) either way. Registering the runner proper
(`tools/ci/install-runner.sh`) comes after all four of those pass by hand.

### The first run on the Titan Xp

**2026-09-23**, the first time this engine executed a shader on anything but the development RTX
5090, and **the first real baseline-tier data point**. Gentoo, kernel 7.2.3, NVIDIA driver
580.178.04, Vulkan 1.4.312, TITAN Xp with 12 GB; `linux-server` (GCC 14.3, x86-64-v2, release).
**Machine state:** nobody else uses that GPU (`nvidia-smi`: 0 MiB used, 0 % utilization before
the runs); the before run started 10 minutes after a boot at a load average of 1.07 / 3.15 / 2.16,
the final after run at 0.59 / 1.72 / 3.71 — and nothing below is a performance number, the times
are the suite's wall clock.

**What the card reports**, `engine-cli gpu.adapters --report`, the extensions of interest:

| Present | Absent |
|---|---|
| `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_tracing_maintenance1`, `VK_KHR_swapchain`, `VK_EXT_descriptor_buffer`, `VK_EXT_shader_object`, `VK_NV_memory_decompression` | `VK_EXT_mesh_shader`, **`VK_KHR_ray_query`**, `VK_NV_cluster_acceleration_structure`, `VK_KHR_ray_tracing_position_fetch`, `VK_NV_partitioned_acceleration_structure`, `VK_EXT_memory_decompression`, `VK_KHR_fragment_shading_rate`, `VK_EXT_fragment_density_map`, `VK_NV_cooperative_matrix` |

Every Required row passes (`maxPushConstantsSize` 256, 1,536 compute invocations, 48 KiB of shared
memory, update-after-bind budgets of 1,048,576), and so do `shaderBufferInt64Atomics` and
`fragmentStoresAndAtomics` — **the visibility buffer works on Pascal**, which was the open question
of the section above. The acceleration structures and the ray-tracing pipeline are the driver's
compute fallback for Pascal. The report's rows are a fixture in `domain/gfx/tests/requirements_tests.cpp`.

**The verdict, before and after.** Before: `"tier": "rt"`, with a degraded line promising a KHR
ray-traced picture — a card that cannot trace one ray the renderer asks for, because every one of
them is a ray query, reported as the ray tracing tier. The runbook above had predicted `raster`; the
rule was wrong, not the runbook. After (the rule keys on `VK_KHR_ray_query`,
[gfx](../subsystems/gfx.md#what-a-device-has-to-have)): `"tier": "raster"`, usable, and two degraded
lines — mesh shaders fall back to the vertex path, and "no VK_KHR_ray_query and no
VK_NV_cluster_acceleration_structure: every ray the renderer traces is a ray query …, so --raster
rt, ray-traced shadows (--shadows rt) and the reference path tracer are unavailable and the tier is
"raster", although VK_KHR_acceleration_structure and VK_KHR_ray_tracing_pipeline are present …".
`--raster rt`, `--shadows rt`, `render.load` with either, and the reference integrator are refused
in exactly that sentence. **The resolved frame:** `raster` `hw` → `vertex`, shadows `auto` → off,
two-pass occlusion culling on.

**The suite.**

| | before | after |
|---|---|---|
| CTest, `linux-server` | 51 of 53 | **53 of 53** |
| `renderer` cases | 20 of 33 pass, 13 fail — and 9 of the 20 "passed" by skipping on a renderer that never built | **34 of 34** (one new), every picture case drawing through the vertex path |
| `engine_cli` cases | 7 of 10 | **10 of 10** |
| `gfx` cases | 52 of 52 | 52 of 52 |
| `gfx` suite time | 40.2 s | 38.9 s |
| `renderer` suite time | 21.6 s (almost nothing drew) | 77.9 s |
| `engine_cli` suite time | 24.3 s | 31.0 s |
| whole suite, wall clock | 290 s | 348 s |

**What was wrong, and none of it was the driver.**

1. **The renderer could not be created on a device without mesh shaders.** `resolve_settings` chose
   the vertex path correctly — `render.load` even answered `"raster":"vertex"` — and then
   `SceneRenderer::create_pipelines` built both mesh-shader pipelines anyway, `create_mesh_pipeline`
   refused, and every frame of every host failed. It builds only the pipelines the resolved path draws
   with now ([renderer](../subsystems/renderer.md#the-offscreen-contract)). Nothing had noticed
   because every GPU the suite had met had mesh shaders; the renderer's tests now reproduce this card
   on any GPU through `DeviceOptions::overrides` and compare its picture with the mesh path's: 7,931
   covered pixels, 0 bytes different on the RTX 5090.
2. **Two test harnesses reported a failed build as a skip**, so the shredded-atlas case and all eight
   streaming cases were green on the card while drawing nothing. They skip only on "no device" or
   `check_availability` now.
3. **The tier rule**, above.
4. **Once they drew, one tolerance was one GPU's calibration.** "A starved budget is coarser and
   never has a hole" counted edge pixels against an allowance of 66 and the 5090 left 65; the TITAN Xp
   left 70, all on silhouettes. It asks for zero missing pixels inside the eroded coverage now, which
   both meet (see [renderer](../subsystems/renderer.md), the fly-in).
5. **Tests that were about the picture and not about a path**: "the mesh, vertex and ray paths
   agree" compares against the vertex path and skips the other two by name; the turned surround
   and the cone case run their raster half through the vertex path; `render_tests.cpp` asks
   `gpu.adapters` which raster path `hw` resolves to instead of assuming `hw`.

**What still skips on this card, by name:** the direct path (mesh shaders); the mesh-path halves of
two renderer cases; every ray half, the four reference cases and the 8-bit-index streaming case
(ray queries and cluster structures); the reference half of `render_tests.cpp`; and in `gfx`, the
mesh-shader test and the ray query, cluster-structure, ray-traced shading and ray-traced deform
cases — plus, until the next day, six `gfx` picture cases that skipped for want of mesh shaders
without being about them (below). The window and swapchain tests skip for want of a display, as
expected. `linux-server-debug` (clang 22.1.8, asserts and iterator checking) was run after the
fixes as well; its line is in [remote Linux builds](remote-linux.md#measured-times).

**2026-09-24: the `gfx` suite on the baseline tier.** The table above counts `gfx` as 52 of 52 on
both days, and that was the least informative line in it: doctest has no skip at run time and
counts a case that returns early as passed, and 13 of the 52 returned early on this card — seven
for want of mesh shaders, five for want of ray queries, one for want of a display. Six of the seven
were not about the mesh stage at all: the lighting model against its double-precision reference
(the shading case and both attributes cases), the cull pass against its CPU reference, two-pass
occlusion culling, and the visibility test that is the software rasterizer's only coverage — each
had simply been written against `create_mesh_pipeline`. They draw through the device's own raster
path now, the vertex path here, and so does the scene test's occlusion half, which had sat inside
its mesh-path branch and was skipped without a word ([gfx](../subsystems/gfx.md), "On a device
without mesh shaders or ray queries"). `linux-server`, GCC 14.3, driver 580.178.04; the before run
started at a load average of 0.67 and the after run straight behind this checkout's own cold build,
at 2.74 / 14.76 / 15.93 — the times are wall clock and no performance claim.

| `gfx` on the TITAN Xp | before (9465a07) | after |
|---|---|---|
| doctest's count | 52 of 52 | 52 of 52 |
| cases that checked something | 39 | **45** |
| skipped: no `VK_EXT_mesh_shader` | 7 | **1**, the mesh-stage test |
| skipped: no `VK_KHR_ray_query` | 5 | 5 |
| skipped: no display | 1, the swapchain | 1 |
| halves skipped | 5, four of them without a message | 4, each named: the mesh halves of the vertex-path, scene and identity-deformer cases, and the scene's ray half |
| suite time | 39.3 s | 43.0 s |

**What the baseline tier measured**, the same as the RTX 5090 wherever the number is the GPU's:
the lighting model agrees with `brdf_reference.h` to **0 of 255** on every compared pixel (the
eight-material sweep, the off-centre pixel, the white dielectric at 149, the tilted normal and four
texels, the four metallic-roughness quadrants and the normal map) against a tolerance of 2, so no
tolerance had to move and the one that exists has its whole margin left on Pascal; the hardware and
software visibility buffers differ on 1 pixel of coverage and 29 of 21,195 ids; two-pass occlusion
culling draws 56 of 108 clusters and changes 0 pixels, and the scene's drops the hidden instance
(76 pairs to 73) with 0 changed; the identity deformer draws what the rigid instance draws with not
even a depth bit apart. The counts that come from the CPU side differ, because this build and the
desktop's make different LOD DAGs from what reads as the same terrain (181 clusters here, 184 there,
so a cull cut of 23 against 24), and each is checked against the CPU reference built beside it. It
is not the same terrain: the test makes it with `std::sin` and `std::cos`, and glibc and MSVC's C
library differ in the last bit of some of those floats; from the same bytes this build makes the
desktop's 184 ([content-build determinism](../experiments/content-build-determinism.md)). What skips now is
exactly what [What runs and what skips](#what-runs-and-what-skips) lists, and
`ENGINE_GFX_TEST_DEVICE=titanxp` reproduces that list on the RTX 5090.

## Registering a runner

### The registration token

On GitHub: **Settings > Actions > Runners > New self-hosted runner**, pick the platform, and copy the
token out of the `config` command it shows. The token is short-lived — it expires about an hour after
it is issued — and it registers a machine against this repository, so treat it as a credential:

- **never commit it**, never paste it into a file in this repository, never put it in a workflow;
- pass it on the command line or, better, in the `RUNNER_TOKEN` environment variable of the shell you
  run the installer from, and clear that variable afterwards;
- removing a runner needs a *removal* token from the same page, which is a different token.

The install scripts never print the token. They do hand it to `config.cmd`/`config.sh` on a command
line, where it is briefly visible to anything that can read the process table; on a machine only the
owner uses that is the same exposure GitHub's own instructions have.

### Windows: a service

From an **elevated** PowerShell 7 prompt on the desktop:

```powershell
$env:RUNNER_TOKEN = '<token from the runners page>'
git clone https://github.com/<owner>/<repo> C:\src\game_engine   # or any checkout of this repo
C:\src\game_engine\tools\ci\install-runner.ps1 `
  -RepoUrl https://github.com/<owner>/<repo> `
  -Labels gpu,maxwell `
  -Name maxwell-desktop
$env:RUNNER_TOKEN = ''
```

The script reads the latest `actions/runner` release (or the one `-Version` names), verifies the
SHA-256 GitHub publishes in the release notes, unpacks it into `C:\actions-runner` (`-InstallDir`),
and registers it with `config.cmd --unattended --runasservice`. There is no `./svc` on Windows and
`svc.cmd` only manages a service that already exists, so `--runasservice` at configuration time is
how the service gets created; it installs *and* starts it. Re-running the script with the same
version and an already-registered runner only checks that the service is up.

`self-hosted`, `Windows`, and `X64` are added by GitHub, and `runs-on` matches labels
case-insensitively, so `-Labels gpu,maxwell` is enough to satisfy `runs-on: [self-hosted, windows, gpu]`.

**The `rtx` label, and the job that is waiting for it.** `gpu.yml`'s nightly `reference` job asks
for `runs-on: [self-hosted, windows, gpu, rtx]` and **no registered machine carries `rtx`**, so it
queues and is never picked up. That is deliberate rather than an oversight: the reference path
tracer traces the frame's *cluster* acceleration structures
([renderer](../subsystems/renderer.md#reference-renderer)), which needs
`VK_NV_cluster_acceleration_structure` — an RTX 20-series card or newer — and the two machines
registered today are a Maxwell and a Pascal, on which every scene of the corpus would answer
"unavailable" and the report would be five skips. A runner on a machine with an RTX card and
`-Labels gpu,rtx` is all the job needs; until then
`pwsh tools/ci/reference-compare.ps1 -Preset msvc-release` is run by hand on the development box,
and it produces exactly what the workflow would upload.

### Linux: a systemd service

As the unprivileged user the runner should work as — not root, which `config.sh` refuses — on a
machine where that user can `sudo`:

```sh
export RUNNER_TOKEN='<token from the runners page>'
git clone https://github.com/<owner>/<repo> ~/src/game_engine
~/src/game_engine/tools/ci/install-runner.sh \
  --url https://github.com/<owner>/<repo> \
  --labels gpu,pascal,headless \
  --name pascal-server
unset RUNNER_TOKEN
```

Same shape: latest release or `--version`, published SHA-256 verified, unpacked into
`~/actions-runner` (`--dir`), configured unattended, then `sudo ./svc.sh install <user>` and
`sudo ./svc.sh start`. `--install-deps` runs the runner's own `bin/installdependencies.sh` first on
a distribution that needs it. `sudo ./svc.sh status` in the install directory reports on it later,
and `journalctl -u actions.runner.<owner>-<repo>.<name>` has the log.

### Removing a runner

Get a removal token from the runner's entry on the runners page, then:

```powershell
C:\actions-runner\config.cmd remove --token <removal token>            # Windows
```

```sh
cd ~/actions-runner && sudo ./svc.sh uninstall && ./config.sh remove --token <removal token>
```

## The GPU workflow

`.github/workflows/gpu.yml` runs on **`workflow_dispatch`** (Actions > GPU > Run workflow, with an
optional `clean` input that deletes `build/<preset>` first) and on a weekly **`schedule`**, Mondays at
06:17 UTC. Nothing else triggers it. Each job takes `timeout-minutes: 90`, checks the repository out
with `clean: false` so the build directory survives between runs and a weekly build stays
incremental, runs its platform's smoke script, and uploads
`build/<preset>/Testing/Temporary/LastTest.log` as an artifact with `if: always()`, so a failing run
is the one whose log you can actually read. The two jobs are independent: if one machine is offline
its job queues until that runner comes back or the run is cancelled, and the other still reports.

`ci.yml` is untouched by all of this and remains the gate on every push and pull request.

## More than one suite on one machine

A self-hosted runner is somebody's desktop as often as it is a dedicated box, and that used to be a
trap: the end-to-end app tests each opened a *fixed* directory under the system temp directory, so a
run on the runner and a run by whoever was sitting at the machine — or by an agent in another
worktree, or a release build's suite beside a debug one — deleted each other's fixtures halfway
through. The symptom was a test that failed in a full run and passed when re-run alone, which reads
like load and is not: measured on a 36-thread desktop, the suite passed `-j 16 --repeat until-fail:3`
with a release build compiling and three 1600×1000 `engine-view` windows open, and failed 19 runs out
of 30 when two copies of the end-to-end binaries started at the same instant. CTest's `RESOURCE_LOCK`
does not cover this: it orders tests *within* one `ctest` invocation and cannot see a second one.

Scratch space is now per test and unguessable (`engine::test::TempDir`, see AGENTS.md's
"Test hygiene"), so a second suite on the machine is no longer a reason for a run to fail, and two
`ctest` invocations from two build trees at once pass.

**Measured again on 2026-09-18**, once every remaining test and bench had been converted (nineteen
files; `tools/lint.ps1`'s `test-temp-path` rule now refuses the old spelling). Four copies of each
of the thirteen converted binaries started at the same instant — two from `msvc-debug` and two from
`msvc-release`, 52 processes a round, five rounds — **failed 0 of 260 runs**; `ctest` from both
build trees at once, twice, passed 51 of 51 tests each time. The control is what makes those
numbers mean something: the *pre-change* `engine_io_tests`, whose fixture opened
`<system temp>/engine_io_tests` and began with `remove_all`, **failed 20 of 20 runs** under the same
four-copies-at-once probe, with `write_file_atomic` returning `IoError` and a directory that existed
a line earlier reported missing — the shape of one process deleting another's tree mid-test.

What is still genuinely exclusive is the
hardware: one window, one GPU, one set of input devices. Keep a runner to **one job at a time** — do
not raise its concurrency — because the GPU tests measure the device and a second suite sharing it
makes the timings meaningless even when the results stay green.

The runner's concurrency setting only covers the runner's own jobs, so on a machine whose GPU is
shared with anything else **`tools/ci/gpu-smoke.ps1` takes the machine-wide GPU lock** around its
three GPU steps — the adapter report, the suite and the captures — after the build and before the
report is written ([bench](../subsystems/bench.md#measuring-on-a-shared-machine), "The GPU lock";
the file and its rules are `D:\workspace\GPU-LOCK.md`'s). It waits while somebody else holds the
card, prints whom it is waiting for, refreshes its ten-minute lease while the suite runs, and
releases it however the run ends; the tests and `engine-view` it starts inherit
`ENGINE_GPU_LOCK_HOLDER`, so their machine-state readings count the lock as theirs. A dedicated
runner has no lock directory (`D:\workspace`, or wherever `ENGINE_GPU_LOCK` points) and the step
says so and runs unlocked — create the directory on a runner that should take part.
`tools/ci/gpu-smoke.sh`, the Linux runner's step, does not take it: the protocol has a PowerShell
and a C++ implementation and no bash one, and the lock is this desktop's arrangement with the
agents that share its card. Note also that two `ctest` runs drift
out of phase within seconds and are therefore a *weak* probe for this class of bug; starting the test
binaries simultaneously is the sensitive one, and is what to reach for when a collision is suspected.

## Nothing listens, and how many GPU clients there are

Two numbers to take on a Windows runner before believing anything else it says, because both of
them have already taken a machine out for a night.

### `tools/ci/check-no-listeners.ps1`

On 2026-09-18 every process on the development machine that created a Vulkan device crashed inside
the NVIDIA driver, for hours, with binaries that had passed minutes earlier. The cause was pending
**Windows Firewall prompts**: Tracy's client opens a listening TCP socket at start-up and, by
default, binds every interface; Windows asks about that once per executable *path*; this tree makes
paths cheaply — every preset and every agent worktree has its own copy of every test, bench and app;
and each pending prompt is a `PickerHost.exe FirewallNotificationDialogServer` process **holding a
GPU context**. Thirty-five of them took the box from 49 GPU client processes to 77, and somewhere
between those two numbers the driver stopped surviving `vkCreateDevice`. Dismissing the prompts
fixed it on the spot. [profiling](../subsystems/profiling.md) has the full account and the two
commands that diagnose it.

Tracy is fixed (`TRACY_ONLY_LOCALHOST`, `TRACY_NO_BROADCAST`) and flecs' REST and HTTP addons are
compiled out ([ecs](../subsystems/ecs.md)). This script is what says a third one has not appeared:

```powershell
pwsh tools/ci/check-no-listeners.ps1 -Preset msvc-release [-Build]
```

It copies every executable the build produced — not the prebuilt third-party tools under `_deps`;
see below — into a directory with a random name, so **every path is new to the firewall and Windows
has to decide about it again**, runs each once with harmless arguments (`--help`, a bench's
`--smoke --quiet --no-pin`, `engine-view --frames 1 --width 64 --height 64`), and counts the prompt
processes before and after. The count must not grow. It also samples each live process's own
endpoints (`Get-NetTCPConnection -State Listen`, `Get-NetUDPEndpoint`) and fails on any that is not
loopback — that half needs no fresh path at all and is what keeps the check meaningful on a machine
the firewall has already made up its mind about.

A run on the development machine (RTX 5090, `msvc-release`, 2026-09-18): 54 executables from 54
brand-new paths, **0 prompts before and 0 after**, 49 GPU clients before and after, and the only
listeners observed were Tracy's, on `127.0.0.1:8086` and `127.0.0.1:8087` — which is the loopback
setting doing its job and is also the evidence that the check can see a listener when there is one.

**Never answer a prompt with "Allow", and never add, change or remove a firewall rule to make one go
away.** A prompt our own run raised is dismissed by stopping that `PickerHost.exe` process, which is
the same as pressing Cancel; the script does that by default and `-KeepPrompts` leaves them up for
diagnosis, which also leaves the machine in the state that caused the outage. When the count does
grow, the script names the binary that was running: the fix is in that binary or its dependency —
bind `127.0.0.1`, or do not listen — and `AGENTS.md`'s convention says so.

`_deps` is excluded because those executables are downloaded, not produced here. `slangc.exe` is run
by the shader hot reload and is a compiler with no socket; `slangd.exe` is a language server this
tree never starts, and launching it under this check would be inventing a listener rather than
finding one.

### GPU client count

```powershell
nvidia-smi --query-compute-apps=pid --format=csv,noheader   # one line per client; count the lines
```

This is the number the driver failed at. Forty-nine was healthy on the development machine and
seventy-seven was not, so somewhere in between is where an NVIDIA driver stops surviving
`vkCreateDevice`; the exact threshold is unknown and is certainly not a constant across driver
versions or GPUs. Worth printing beside a GPU run, and worth looking at first when device creation
starts failing machine-wide rather than in one process — together with
`Get-CimInstance Win32_Process -Filter "Name='PickerHost.exe'"` for the prompts and the Application
event log (Id 1000, faulting module `nv*`) for when it began. `check-no-listeners.ps1` prints the
count before and after its run for exactly this reason.

## The extreme resolutions

[04 §4.6](../plan/04-renderer.md#46-extreme-displays) ends with a commitment: *no 16-bit screen
coordinates anywhere, all screen-space structures sized from the render config at startup, and CI
renders at 11520×2160, 7680×4320, 1080×3840 portrait, and 5120×1440 every night.* The first two
halves of that are claims about the code and the third is what checks them. An overflow in a tile
count, a Hi-Z level count, or a workgroup dispatch does not show up at 1280×720; it shows up as a
wrong picture or a device loss at 48:9 or at 8K, and nothing smaller finds it.

So both smoke scripts have a fourth step, after ctest: `engine-view --frames 60 --capture` at each
of those four resolutions, and at plain 3840×2160 as the number the others are read against. The
pictures go to `build/<preset>/captures/<width>x<height>.png` and each run's stdout and stderr
beside them; the workflow uploads the lot, with `if: always()`, as `gpu-smoke-<runner>`.

**Each resolution is its own experiment.** It gets its own time budget
(`-CaptureTimeout` / `--capture-timeout`, 300 s, after which the process is killed) and its own
recorded result, and the loop never stops early. The reason is that a resolution is exactly the kind
of thing that hangs or crashes on one machine and not another, and a run that stopped at the first
one would tell you nothing about the other four — which are the ones you wanted to know about. The
same goes for the report: it is written whatever happened, so a failed run still says which
resolutions worked. A resolution where `engine-view` exits **3** — no display, no Vulkan device, no
mesh shaders, no presentation — is a **skip**, the same treatment its end-to-end test gives it,
because that is the headless Linux runner every time and the Windows runner whenever it runs as a
service. Any other nonzero exit, and a run that outlives its budget, is a **failure**, reported
after all five have been tried; the script's status is then ctest's, or 1 when ctest passed and a
capture did not. `-SkipCaptures` / `--skip-captures` leaves the step out entirely.

**`build/<preset>/gpu-smoke.json`** is the run's machine-readable record, written beside the ctest
log: the machine and preset, the timestamp, ctest's status, the `gpu.adapters` report embedded whole
(or `null` when it did not come back as JSON — it is read with stderr merged in), and one entry per
resolution with its status, exit code, wall time, capture path, and `engine-view`'s own JSON summary
— cluster counts, visible pairs, and `gpu_ms` per pass. That last part is the point of running 60
frames rather than 2: the summary averages the GPU timestamps over the run, so the file carries a
per-pass cost at each aspect ratio and not only a pass/fail.

**`engine-view` is a windowed application**, which is the wart in this step until `engine-host`
learns an offscreen mode ([apps](../subsystems/apps.md)). A window manager may hand back a surface
smaller than 11520×2160 was asked for, so each entry records `requested_width`/`requested_height`
alongside `rendered_width`/`rendered_height` from the summary and a `clamped` flag when they differ:
the file never claims a resolution that no rasterizer saw. On the development machine (RTX 5090, an
11520×2160 surround desktop) all five come back unclamped, including 7680×4320, which is larger than
the desktop; whether Maxwell and a 2560×1440 display do the same is one of the things the first
nightly run will say. At 11520×2160 on that machine the frame is 0.86 ms of GPU time — cull 0.030,
mesh-shader raster 0.033, Hi-Z 0.55, resolve 0.25 — which is the shape to expect: at 24.9 million
pixels the passes that are per-pixel dominate and the ones that are per-cluster do not move.

## Security

- **Self-hosted runners belong on private repositories only.** GitHub's own warning is worth
  repeating: a public repository lets anyone open a pull request, and a workflow that runs on
  `pull_request` would then execute a stranger's code on your hardware, inside your network, with
  whatever the runner user can reach. This workflow triggers on `workflow_dispatch` and `schedule`
  and on nothing else, which means a fork's pull request can never start it — but that is a property
  of this file, not of the machines. Keep it when editing the workflow, and do not enable
  "Run workflows from fork pull requests" for these runners.
- **The runner user should have no rights it does not need.** On Linux, a dedicated unprivileged
  account whose `sudo` rights (if any) end once the service is installed. On Windows the service
  runs as `NT AUTHORITY\NETWORK SERVICE` by default, which is right; use `-WindowsLogonAccount` with
  a dedicated local account only if the default cannot reach the GPU, and let `config.cmd` prompt for
  that account's password rather than putting it anywhere.
- The runner's work directory is not a secret store. Nothing in this repository's workflows needs a
  secret beyond `GITHUB_TOKEN`, and `gpu.yml` asks for `permissions: contents: read`.
- The registration token, again: short-lived, never committed.
- These runners execute whatever is on the branch that dispatched the run. Dispatching `gpu.yml` on
  an untrusted branch runs that branch's `tools/ci/*` on the machine.

## Troubleshooting

**`"error": "no Vulkan loader library (vulkan-1.dll or libvulkan.so.1); install a GPU driver"`.**
The loader itself is missing. On Windows, reinstall the display driver. On Linux,
`apt install libvulkan1` — the loader is a separate package there.

**A driver with no ICD.** `nvidia-smi` is healthy, `vulkaninfo --summary` lists no devices, and
`gpu.adapters` says `"available": false` with
`"error": "no Vulkan 1.3 driver (ICD) is installed behind the loader"`, or returns
`"available": true` with an empty `adapters` list. The driver was installed without its Vulkan ICD
manifest: check for `/usr/share/vulkan/icd.d/nvidia_icd.json` on Linux, or the
`HKLM\SOFTWARE\Khronos\Vulkan\Drivers` values on Windows, and reinstall from the distribution's
driver packages rather than a stripped `.run` install. The smoke scripts treat an empty adapter list
the same way they treat `available: false`.

**Windows: the service runs in session 0 and cannot create a window. This is expected.** A Windows
service has no desktop, so SDL cannot open one: `window::init()` or window creation fails, the
`window` tests and the gfx swapchain test skip with "no display", and the `engine_view` test sees
exit code 3 and records a skip. Everything offscreen — device creation, compute, the render graph,
the visibility buffer, capture — works normally, because Vulkan on Windows does not need a desktop
session. There is nothing to fix. If you do want the presentation path covered on that machine, run
the runner interactively instead of as a service (`run.cmd` from the install directory, in a logged-in
session) and accept that it stops when the session ends.

**Windows: the service sees no adapter although the desktop does.** Rare, but if `NETWORK SERVICE`
cannot reach the GPU, re-register with `-WindowsLogonAccount <dedicated local account>` — the script
drops `--unattended` in that case so `config.cmd` asks for the password itself — or run the runner
interactively.

**The build cannot find a compiler on Windows.** `tools/dev.ps1` throws "No Visual Studio installation
with the C++ toolset was found": the Build Tools are missing the "Desktop development with C++"
workload, or the runner user cannot read the Visual Studio installation.

**`ctest not found` on Linux.** CMake is not installed, or is older than 3.28 and the presets were
never accepted. `cmake --version` from the runner user's shell.

**The weekly run times out.** The i7-980 is a 2010 six-core; a from-scratch build of this tree plus
the Slang download is the slow case, and that is why the checkout does not clean. If a run does time
out, dispatch it manually once with the `clean` input off; if the build directory is the problem,
dispatch it with `clean` on and expect the long run.

**The runner is offline in GitHub's list.** `Get-Service actions.runner.*` on Windows,
`sudo ./svc.sh status` on Linux, and the runner's own `_diag` directory in the install directory.
