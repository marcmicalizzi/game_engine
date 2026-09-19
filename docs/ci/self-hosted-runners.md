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
| `windows-maxwell` | Windows 11 desktop, Intel i7-980 (Westmere, SSE4.2, no AVX), NVIDIA GeForce GTX Titan X (Maxwell, 12 GB), 2560×1440 display | `msvc-release` | `self-hosted`, `windows`, `gpu`, `maxwell` |
| `linux-pascal` | Linux server, headless (no X11, no Wayland), first-generation Skylake Xeon, NVIDIA GeForce GTX Titan Xp (Pascal, 12 GB) | `linux-clang-debug` | `self-hosted`, `linux`, `gpu`, `pascal`, `headless` |

The two presets are deliberately different: `msvc-release` on the slow Westmere box, because a
release build is what a 30 fps target is measured in and a debug build of this engine on six 2010
cores is not a weekly proposition; `linux-clang-debug` on the server, because asserts and iterator
checking are worth more than speed there and it is the preset a Linux contributor runs.

## What runs and what skips

`tools/ci/gpu-smoke.ps1` and `tools/ci/gpu-smoke.sh` build the preset through `tools/dev.ps1`, print
`engine-cli gpu.adapters` — the machine's capability report, in the same schema-typed form the
protocol serves — and then run the whole CTest suite. The GPU tests decide for themselves what they
can do, so the suite passes on every machine and the log is what says how much of it meant anything.
A fourth step then renders the extreme resolutions of [04 §4.6](../plan/04-renderer.md#46-extreme-displays)
and writes `build/<preset>/gpu-smoke.json`; see [The extreme resolutions](#the-extreme-resolutions)
below for what it does and what it treats as a skip.

Neither GPU has `VK_EXT_mesh_shader`, so on both machines these skip with a reason:

- the mesh-shader test (`mesh_shader_tests.cpp`),
- the visibility test, which is also the software rasterizer's only coverage (`visibility_tests.cpp`),
- the two-pass occlusion test (`occlusion_tests.cpp`),
- the material resolve's numerical tests (`shading_tests.cpp`, `attributes_tests.cpp`),
- the GPU cull test (`cull_tests.cpp`), which draws its cut through the mesh path.

What they do cover is the baseline tier and everything under it: the vertex-shader cluster path
(`vertex_path_tests.cpp`, which runs the cull pass with `CullParams::count_index` = 1, the LOD cut,
the indirect draw, and the visibility buffer, and only its comparison against the mesh path needs
mesh shaders), device creation with the required feature chain, compute, frames in flight, the
render graph, bindless descriptors, the raster pass, capture, the shader library and its SPIR-V
reflection of every shipped shader including the resolve, the CPU LOD reference in `geometry`, and
every CPU test in the tree. On the Westmere box that CPU side is the point as much as
the GPU: it is the machine that proves the build carries no ISA above the x86-64 baseline.

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
`vertex_path_tests.cpp` skips without that feature; the adapters report says in advance whether it
will. If a Required row fails, every GPU test reports `device unavailable` with the reason and still
passes, which is why the log matters more than the exit status on these two.

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

- **The packages `.github/workflows/ci.yml` installs**, which are what SDL3 needs to build:

  ```sh
  sudo apt-get update
  sudo apt-get install -y ninja-build clang g++ \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
    libwayland-dev libxkbcommon-dev libegl1-mesa-dev libgl1-mesa-dev libdrm-dev libgbm-dev \
    libxtst-dev libdecor-0-dev libudev-dev
  ```

  SDL3 is built from source and wants the X11 and Wayland headers even on a machine that will never
  open a window; the window tests then skip at run time, which is the intended behaviour.
- **`cmake` 3.28 or newer** (`CMakePresets.json` requires it; Ubuntu 24.04 ships 3.28) and **`git`**.
  The hosted runners get CMake for free; a bare server does not.
- **`pwsh`**, PowerShell 7, because `tools/dev.ps1` is the build entry point on both platforms.
  Microsoft's `.deb` or the `powershell` snap both work.
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
build/linux-clang-debug/bin/engine-cli gpu.adapters                 # Linux
```

`"available": true` with the GPU's name, `"tier": "raster"`, `VK_EXT_mesh_shader: false` and
`"verdict": {"usable": true, "tier": "raster"}` is what a correctly set up baseline machine looks
like. `"available": false` carries the reason in `"error"`, and a device the renderer would refuse
carries its reasons in `verdict.blocking` — one sentence per unmet requirement, each naming what in
the engine needs it.
The smoke scripts print this on every run and warn when it comes back false; `-RequireAdapters` /
`--require-adapters` turns that warning into a failure, which is worth using once the machines are
known good, so a driver that stops working does not look like a quiet pass.

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
makes the timings meaningless even when the results stay green. Note also that two `ctest` runs drift
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
