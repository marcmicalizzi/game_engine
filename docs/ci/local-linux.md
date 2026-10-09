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
local, needs the network only for a dependency no checkout on the machine has fetched yet, and —
warm — takes minutes. **The first run of a fresh checkout is not minutes**, and every agent
works in a fresh worktree: see [Measured times](#measured-times) for what that costs and
[Scheduling](#scheduling-warm-early-one-at-a-time) for how to keep it off the critical path.

It does **not** replace `ci.yml`: the workflow is still what gates a push, and the self-hosted GPU
runners ([self-hosted runners](self-hosted-runners.md)) are still the only machines that execute a
shader. This is the compiler-and-CPU half of the gate, moved to where the work happens.

## The command

```powershell
pwsh tools/linux-build.ps1 [-Preset <name>|all] [-Test] [-Filter <regex>] [-Jobs <n>]
                           [-Shell] [-Rebuild] [-Docs [-Base <rev>]] [-Sync:$false]
                           [-NoWait] [-Offline]
pwsh tools/linux-build.ps1 -Prune [-Stale [-KeepUnknown] [-Base <rev>]] [-WhatIf]
```

| Flag | What |
|---|---|
| `-Preset` | one preset, or `all` for `linux-clang-debug`, `linux-gcc-release`, `linux-clang-minimal`, `linux-clang-no-ecs` — ci.yml's compile-and-test matrix, in ci.yml's order. Any other Linux preset works too: **`linux-clang-asan` is ci.yml's sanitizer job** (ASan and UBSan, undefined behaviour fatal, leaks reported; [what to run](what-to-run.md#sanitizers)) and is not in `all`, because it is a slower build the merge does not wait for. Default `all`. |
| `-Test` | run CTest after the build. Without it the run is a compile check. |
| `-Filter` | a regex on test names, passed through to `ctest -R`. |
| `-Jobs` | parallel compile jobs. Default 8; see [Why eight jobs](#why-eight-jobs--and-why-the-worry-was-wrong). |
| `-Rebuild` | delete `build/<preset>` inside the volume first. The dependencies' sources are in another volume and survive, so this costs a compile and not a download. It is **the whole compile, third-party code included**: the objects under `/deps/<preset>` survive, but ninja's record of which headers each one read is in `build/<preset>` and goes with it, and ninja rebuilds an object it has no such record for. Measured on 2026-09-24: 1116 edges, all 635 third-party ones among them, the same as a cold build. |
| `-Shell` | an interactive `bash` in the container with the volumes mounted, for when a failure needs poking at. |
| `-Docs` | run `tools/docs-gate.sh` and `tools/docs-check.sh` in the container and build nothing; `-Base` is what the gate diffs against (default `main`). See [Running the documentation gate here](#running-the-documentation-gate-here). |
| `-Prune` | delete this checkout's two volumes and exit, one line per volume with its size. The machine-wide dependency cache and the images are left alone. **Run it from your worktree before you hand it back**, once you are done with containers; see [Volumes that outlive their checkout](#volumes-that-outlive-their-checkout). |
| `-Stale` | with `-Prune`: sweep every checkout's volumes on the daemon instead of this one's, removing the [stale](#the-stale-rule) ones. |
| `-KeepUnknown` | with `-Stale`: keep the volumes whose checkout [nothing can name](#unknown-checkouts-are-removed-by-default). |
| `-WhatIf` | with `-Prune`: print the same lines and remove nothing. Any other run refuses it. |
| `-Sync:$false` | build what is already in the volume without re-syncing the checkout. |
| `-NoWait` | if another build holds the [build lock](#one-container-build-at-a-time-whoever-started-it), say whose and exit 2 instead of waiting for it. |
| `-Offline` | run the build containers with `--network none`. A configure that still needs to download something then fails instead of downloading it, which makes "this build fetched nothing" a result rather than a reading of the log. |

Exit status is the build's, or the tests' with `-Test`. **A build that fails ends the run with its status, and the tests do not run.** Until 2026-09-23 they did. The container script had `pipefail` but not `-e`, so a failed `cmake --build` fell through to `ctest`. `ctest` then ran the previous build's test binaries, which passed, and `-Test` printed `ok` with exit 0 over a Clang `-Werror` failure in the one file that had changed. The script is now `set -eo pipefail`. A run whose log says `ninja: build stopped` but whose verdict says `ok` came from before that fix. With `all`, every preset is attempted and
the status is the first failure: nothing stops early, because a second compiler's opinion is the
entire reason for running four of them. 2 means another build held the lock and `-NoWait` was
given; 3 means a wait for it gave up after four hours. `-Prune` exits 1 if docker refused a
removal. Docker or git unreachable, or a switch given without the one it belongs to (`-Stale`
without `-Prune`, `-WhatIf` on a build), is one line on stderr and exit 1, not a PowerShell error
record.

`-Preset all` prints a summary table at the end. One preset prints its own line.

## What it does, and the choices worth arguing about

### One container build at a time, whoever started it

On 2026-09-20 three agents, each in a fresh worktree, started a cold four-preset build within
minutes of each other. Each would have taken about a quarter of an hour alone; together they took
**over two and a half hours** and ran the 16 GB WSL2 VM out of memory — twenty-four compilers on
twelve CPUs, where the memory measurement in [Why eight jobs](#why-eight-jobs--and-why-the-worry-was-wrong)
is of one build's eight. Three builds side by side are not three times slower, they are worse; one
after another they are three times one.

So a build takes the **machine-wide build lock** first and holds it until its last container has
exited. It is the GPU lock's protocol and the GPU lock's implementation, pointed at a different
file ([bench](../subsystems/bench.md#measuring-on-a-shared-machine), "The GPU lock";
`tools/lib/MachineLock.psm1`): one JSON file, `D:\workspace\linux-build.lock` or
`$env:ENGINE_LINUX_BUILD_LOCK`, created atomically with an owner, a purpose that names the presets
and the checkout, and an expiry. An expired lock may be broken, an unexpired one never.

- **A second invocation waits** and says whom for — `linux-build: waiting; held by 'claude-engine':
  linux-build linux-clang-debug,... -Test in D:\...\agent-xyz (until 21:04:00Z)` — and starts the
  moment the first releases. `-NoWait` makes it fail at once with exit 2 instead; after four hours
  it gives up with exit 3.
- **The lease is ten minutes, refreshed every two** from a background thread while the build runs,
  so a build whose script is killed outright holds the machine for ten minutes and not for as long
  as the build would have taken. Ctrl+C is not killed outright: the script removes the container it
  started (they are named per invocation) before it releases, because a lock released while a
  container is still compiling is the concurrency it exists to prevent.
- **It also waits for containers the lock cannot see**: any running container from this toolchain's
  images that is not a shell or a docs check — another checkout's build started by a copy of this
  script from before the lock existed, say — holds the build until it has gone, and is named in the
  log with the `docker rm -f` that ends it. One kind is not waited for but removed: an **orphan**,
  a container this script started (they are named `engine-linux-<checkout>-<pid>-<preset>`) whose
  script is no longer running. A tool timeout kills a process tree outright, no `finally` runs,
  and the daemon keeps the container compiling for nobody; the first time a run was stopped that
  way here, the lock would have expired on schedule and the orphan would have gone on holding
  eight cores through its build and its whole suite.
- **The checkout is synced when the lock is taken**, not when the command started, so what gets
  built is the tree as it is when the wait ends. That is also what keeps two invocations from the
  *same* checkout from syncing over each other's build.
- `-Shell` and `-Docs` take no lock: neither is a build, and a shell is interactive and may sit idle
  for an hour. Their containers are labelled so a build does not wait for them either.
- On a machine without the lock's directory (no `D:\workspace`) there is nobody to coordinate with;
  the build says so and runs unlocked.

`tools/gpu-lock.ps1 status -LockFile D:\workspace\linux-build.lock` says who is building.

**Checked, on 2026-09-22**: two invocations from two checkouts, fifteen seconds apart, with a
planted orphan (named as this script names its containers, its owner pid dead) and a planted
unlabelled container running beside them. The first removed the orphan, waited 40 s for the
unlabelled one, then built; the second printed whose build it was waiting for, waited 1,001 s,
started the moment the first released, and configured all four presets with `--network none` —
the cache the first had filled was all it needed (the fresh-checkout column of
[Measured times](#measured-times) is the same checkout, pruned and built again the same way).

### The checkout is mounted read-only and rsync'd into a volume

**Never build over a bind mount of the Windows checkout.** WSL2 reaches `D:\` over a 9p share;
a configure plus a build of this tree is hundreds of thousands of small-file operations, and the
Windows build trees under `build/` must not be touched by a Linux CMake anyway. So the checkout
arrives at `/host` **read-only**, and two rsyncs copy it into the `/src` volume, which the next
section is about. 682 files and 9.2 MB, in about two and a half seconds.

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

### The sync: a file gets a new time only when its bytes changed

**The rule.** A file is copied into `/src` only when its bytes differ from what is there, and a
copied file carries the time of the copy; an unchanged file keeps its old time. Ninja rebuilds an
object when an input is newer than it, so this rule is what makes "the next run compiles what I
edited" true whenever the edit happened, while "nothing changed" still builds nothing.

**What it replaced.** Until 2026-09-24 the sync was `rsync -a`, which copies the host's
modification time along with the bytes. A source edited on Windows while a container build is
compiling its previous text is then *older* than the object built from that text. The next run
copies the new text across and compiles nothing, and no later run notices either. Reproduced on
2026-09-24 in a fresh worktree (times are UTC; the container's clock and Windows' agreed to within
the second it takes to start a container, so this is not clock skew):

| Time | What happened |
|---|---|
| 04:47:25 | run 1, a cold `-Preset linux-clang-debug`, starts; syncs, configures, and starts ninja at 04:47:59.7 |
| 04:48:17.49 | a comment in `apps/engine_view/tests/mesh_view_tests.cpp` is changed on Windows. Ninja is at edge 551 of 1116 |
| 04:49:08.1–04:49:11.1 | run 1 compiles that file (edge 1113) from the text synced before the edit |
| 04:49:42 | run 2 syncs. rsync copies the new text, **dated 04:48:17.49, 54 s older than the object**. Ninja runs the build stamp and nothing else, and `ninja -d explain` on that test target never names the file |

The same thing happened on the remote machine a day earlier. There the symptom was a link failure:
an "undefined reference" to a function that plainly exists, because the test had been recompiled
and the library it called had not. `tools/remote-build.ps1` got the same rule then; see [remote
Linux builds](remote-linux.md#the-sync-and-what-it-costs). Either symptom means a stale object.
Before their fixes, both scripts produced one whenever a file was edited after a run's sync and
before that run compiled it.

The old sync had a second, rarer hole. rsync's quick check compares modification times **to the
whole second** by default, so if a file was edited again in the same second the previous sync read
it, and the edit kept the same size, it never arrived at all. Checked in the image: a five-byte
file rewritten 0.6 s later with five other bytes keeps its old contents through `rsync -a` and
arrives with `--modify-window=-1`.

**The same trap needs no container.** Restoring a source from a backup copy with PowerShell's
`Copy-Item` (or `cp -p`, or `robocopy`) gives it the backup's modification time, which is older
than the object compiled from whatever the file said in between, so `tools/dev.ps1 build`
compiles nothing and the binary keeps the text the restore meant to undo. It produced one
misleading run on 2026-09-24: a streamer built with a rule switched off for a before-and-after
test, the file restored with `Copy-Item`, and the "after" binary stalling exactly like the
"before". Set the time after restoring (`(Get-Item <file>).LastWriteTime = Get-Date`, or
`touch`), or restore with `git checkout`, which writes a fresh one.

**How it works now** (`Invoke-Sync` in `tools/linux-build.ps1`): two rsyncs in the sync container.

1. host → `/deps/.host-mirror` with `rsync -a --delete --modify-window=-1`. The mirror keeps the
   host's times, so rsync's quick check (size and mtime, to the nanosecond) finds what changed from
   one stat per file over the 9p share, which is what the sync has always cost.
2. mirror → `/src` with `rsync -rlp --checksum --delete` and **no `-t`**. A file is copied only if
   its bytes differ from the tree the build reads, and it gets the time of the copy. Both sides are
   on the VM's own disk, so reading all of them takes tens of milliseconds.

The mirror is in the checkout's `/deps` volume, which nothing else uses except FetchContent's
per-preset directories. `-Prune` removes it with the rest. A sync that finds it missing fills it
again, which takes about 4½ s once, and copies nothing into `/src` that has not changed. The sync's
line says what it copied, for example `sync: 682 files, 1 copied, 0 removed`, followed by the file
names when there are twelve or fewer: that is the list of sources the build is about to recompile.

**Measured** on 2026-09-24, five syncs of each kind, with the build lock held and nothing else
running (host CPU 5–9 %, GPU 0 %, no other container). "Wall" is the `docker run` including the
container's start, "rsync" the time inside it. 682 files, 9.2 MB:

| | No change | One file changed (same size) | Into an empty volume |
|---|---|---|---|
| `rsync -a` (until 2026-09-24) | 2.52 s wall (rsync 1.45 s) | 2.52 s (1.44 s), **and the file keeps the host's old time** | 4.30 s |
| `rsync -rlp --checksum`, host → `/src` | 3.27 s (2.26 s) | 3.26 s (2.18 s) | 5.11 s |
| **mirror, then `--checksum` into `/src` (now)** | **2.53 s (1.49 s)** | **2.61 s (1.63 s)** | 4.45 s |

Both new rules gave the changed file a time later than the start of the sync and left the other
681 files alone. `rsync -a` gave it a time earlier than the start of the sync, which is the whole
flaw in one column. `--checksum` straight from the host is simpler and just as correct, but it reads
every byte through the 9p share on every run: three quarters of a second today, growing with the
size of the tree. The mirror's cost grows with the number of files, as the old sync's did.

**Why not ninja's own answer.** Ninja does not have one. Ninja 1.11, the version in the image,
decides what is dirty from modification times alone: an output older than its newest input, or
older than the time its log recorded. It does no content hashing. `restat` concerns outputs that a
command chose not to rewrite and cannot make an input newer; `-d explain` only reports the
decision. The time has to be right before ninja looks at it.

**Checked after the fix**, the same way round. Run A (`-Rebuild`) was compiling (edge 582 of 1116)
when the same file was restored to its committed text on Windows at 04:59:11.14. Run A compiled
the text it had synced before the restore, at 05:00:17. Run B's sync printed `1 copied` and named
that file. Ninja then compiled
exactly that object, relinked `engine_engine_view_tests`, and ran the build stamp, which runs every
time.

**A tree the old rule already left stale stays stale.** Its `/src` already holds the new bytes, so
the sync finds nothing to copy. Run `-Rebuild` once, or `touch` the file inside `-Shell`. Touching
it on Windows no longer helps, because its bytes have not changed.

### Three volumes: two per checkout, one per machine

| Volume | Mounted at | Holds | Shared by |
|---|---|---|---|
| `engine-linux-src-<id>` | `/src` | the synced sources and `build/<preset>/` for every preset | this checkout |
| `engine-linux-deps-<id>` | `/deps` | `FETCHCONTENT_BASE_DIR`: the dependencies' *build* directories, one directory per preset; and `.host-mirror`, the [sync](#the-sync-a-file-gets-a-new-time-only-when-its-bytes-changed)'s copy of the host tree with the host's times | this checkout |
| `engine-linux-fetch-cache` | `/fetch` | the dependencies' downloaded *sources*, one directory per pin | every checkout on the machine |

`<id>` is a hash of the checkout's path, so every agent worktree gets its own pair of build volumes
and two of them cannot land in one build directory. The image is shared between checkouts — it is
a function of the Dockerfile alone — and so, now, are the downloads. Since 2026-10-07 every volume
is also created with **labels that say whose it is** — `engine.root`, `engine.role`,
`engine.scope`, `engine.created` — so that a sweep can tell which checkout a volume belongs to
without hashing anything; see [Volumes that outlive their checkout](#volumes-that-outlive-their-checkout).

**The dependency cache.** Build trees differ between checkouts; the sources of Jolt v5.6.0 do not.
Keeping the downloads per checkout meant that every agent's first run in a fresh worktree fetched
every dependency again, once per preset — four copies of the same SDL3, Slang and Vulkan-Headers
per worktree — so the configure half of a cold build was mostly `git clone`. Now
`tools/ci/fetch-cache.cmake` is installed as a CMake **dependency provider**
(`-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES`, CMake 3.24's mechanism for exactly this) on the configure
line `linux-build.ps1` writes, and nowhere else. It sees every `FetchContent_MakeAvailable()` with
the details its `FetchContent_Declare()` gave, keys an entry by a hash of **all** of them —
repository, tag, URL, hash, and the rest — except the three that only say where FetchContent would
have put things (`SOURCE_DIR`, `BINARY_DIR`, `SUBBUILD_DIR`, which it fills in from the per-preset
`FETCHCONTENT_BASE_DIR`; the first version keyed on them too and kept one copy of every download
per preset), fetches the entry into `/fetch/<name>-<key>` if no checkout has yet, and hands it to
FetchContent as that one dependency's source directory. So a
pin bump is a new entry and never a stale hit; a new dependency needs no list updated anywhere
(which is why this was not done before: the only design then on the table was a hand-kept list of
`FETCHCONTENT_SOURCE_DIR_<NAME>` values in this script, which silently goes stale); and both image
targets, all presets and every checkout read the same bytes. The details that matter:

- An entry is filled in `/fetch/.partial-*` and renamed into place, so an interrupted download
  never leaves a directory that looks complete.
- A filled entry is **read-only**. Every checkout compiles from the same files, so a dependency
  whose build wrote into its own source tree would be one checkout editing another's sources. None
  of the current ones does; one that starts to fails its build with a permission error here.
- The override is taken back out of `CMakeCache.txt` after each call. `FetchContent_Populate()`
  caches it as a side effect, and a cached `FETCHCONTENT_SOURCE_DIR_<NAME>` is consulted *before*
  the provider, so without that a pin bump in one checkout would quietly keep building the old
  sources.
- `FETCHCONTENT_BASE_DIR` still points at the per-checkout `/deps/<preset>`, because a Debug tree
  cannot share a dependency's *build* directory with a Release tree. (This page used to add that it
  keeps `-Rebuild` a compile of the engine and not of Jolt. It does not; see `-Rebuild` in the
  table above.)
- `cmake --log-level=VERBOSE` prints what each entry is keyed on, for the day a miss is a surprise.

A configure prints `fetch-cache: <name> from /fetch/<name>-<key>` for a hit and
`fetch-cache: <name> is not cached yet; fetching it into ...` for a miss, so a log says which
dependencies a run downloaded. **Checked rather than assumed**: a fresh checkout's first
four-preset build with `-Offline` (`--network none`) succeeds once any checkout has filled the
cache — see [Measured times](#measured-times).

### Size, and getting it back

Measured on 2026-09-22 after all four presets had been built once, in two fresh checkouts of the
same tree — one with the script as it was, one with the shared cache:

| | `clang-debug` | `gcc-release` | `clang-minimal` | `clang-no-ecs` | all four |
|---|---|---|---|---|---|
| `/deps` per checkout, before (sources **and** build directories) | 1.1 GB | 1.3 GB | 685 MB | 884 MB | **3.9 GB** |
| `/deps` per checkout, now (build directories only) | 230 MB | 437 MB | 94 MB | 206 MB | **966 MB** |
| `/src/build` per checkout | 561 MB | 886 MB | 394 MB | 483 MB | **2.3 GB** |

and **779 MB** in `engine-linux-fetch-cache`, once for the machine: fourteen entries, of which
Slang (250 MB, a prebuilt release), flecs (134 MB), Vulkan-Headers (114 MB) and SDL3 (109 MB) are
most of it. Before, every checkout carried that four times over — Slang alone was a gigabyte per
worktree. A checkout now costs about **3.3 GB** instead of 6.3 GB, plus the shared cache and a
**1.65 GB** image that every checkout on the machine shares. The minimal preset is the small one
for the obvious reason: `ENGINE_MINIMAL=ON` fetches nothing for a capability it does not contain,
so Jolt, flecs, SQLite and Recast never arrive at all.

```powershell
pwsh tools/linux-build.ps1 -Prune        # this checkout's two volumes, and it names the images
pwsh tools/linux-build.ps1 -Prune -Stale -WhatIf   # every checkout's leftovers; drop -WhatIf to remove them
docker image rm engine-linux-ci-desktop:<tag> engine-linux-ci-headless:<tag>
docker volume rm engine-linux-fetch-cache  # every checkout's next configure refetches what it needs
```

The cache is never pruned on its own: a pin bump leaves the old entry behind, a few megabytes to a
few hundred. Removing the volume while no build is running costs the next configure its
downloads and nothing else.

The two images share every layer up to the split, so the headless one costs a few megabytes of
metadata rather than a second 1.65 GB.

Docker Desktop's WSL2 disk does not shrink on its own; `docker system prune` and, if it matters,
Docker Desktop's own disk-reclaim are what return the space to Windows.

### Volumes that outlive their checkout

Nothing removed a checkout's volumes when the checkout was finished. Measured on 2026-10-07, before
the owner swept them by hand: **250** engine volumes holding **568 GB**, and `docker system df`
calling 99% of the volume space reclaimable. **194** belonged to checkouts that no longer existed
on disk and **52** more to agent worktrees whose branches were already merged; only three
checkouts had any use for theirs. Every agent works in a fresh worktree, and every worktree that
ran one build left a pair behind (3.3 GB each for all four presets, [above](#size-and-getting-it-back)).

So, two rules and a sweep:

- **An agent prunes its own**: `tools/linux-build.ps1 -Prune` from its worktree before it hands
  back, once it is done with containers. A worktree that comes back for a follow-up pays one fresh
  first run for it; one that does not come back, which is most of them, costs nothing.
- **A merge prunes the gate worktree's** the same way, once the gate has run.
- **`-Prune -Stale` sweeps what slipped through**: an agent stopped before its last step, a
  worktree removed by hand, every volume from before the rule.

```powershell
pwsh tools/linux-build.ps1 -Prune -Stale -WhatIf   # what it would remove and why; removes nothing
pwsh tools/linux-build.ps1 -Prune -Stale           # the same, for real
```

Run the `-WhatIf` line first. Every checkout volume on the daemon gets one line — what happens to
it, its name, its size from `docker system df -v`, its checkout or `unknown checkout`, and why —
the stale ones first, and then the total. On this machine on 2026-10-07, after the hand sweep:

```text
== engine-linux checkout volumes on this daemon (-WhatIf: nothing is removed)
keep   engine-linux-deps-049fb17c077a    6.37GB  D:\workspace\game_engine  -- the main checkout
keep   engine-linux-src-049fb17c077a     4.41GB  D:\workspace\game_engine  -- the main checkout
keep   engine-linux-deps-e0439115742d    1.61GB  D:\workspace\game_engine\.claude\worktrees\agent-aa2a94a027936b44e  -- the worktree is locked (claude agent agent-aa2a94a027936b44e (pid 63416))
keep   engine-linux-src-e0439115742d      2.3GB  D:\workspace\game_engine\.claude\worktrees\agent-aa2a94a027936b44e  -- the worktree is locked (claude agent agent-aa2a94a027936b44e (pid 63416))
-WhatIf: 0 volume(s), 0B, would be removed; nothing was.
```

Without `-WhatIf` a stale line says `remove`, a removal docker refuses says `FAILED` with docker's
own words under it — a container that started after the sweep looked — and the exit status is 1.
The sweep never touches anything but `engine-linux-src-<id>` and `engine-linux-deps-<id>`: not the
dependency cache, not buildx's state, not another project's volumes (this daemon holds more than a
dozen of those).

#### The labels

Every volume the script creates since 2026-10-07 says whose it is:

| Label | Value |
|---|---|
| `engine.root` | the checkout, spelt as the script's `$Root`: `D:\workspace\game_engine\.claude\worktrees\agent-…` |
| `engine.role` | `src`, `deps`, or `fetch-cache` |
| `engine.scope` | `checkout`, or `shared` for `engine-linux-fetch-cache` |
| `engine.created` | when the script created it, in UTC: `2026-10-07T12:30:05Z` |

Docker cannot label a volume that already exists, so one made before then stays unlabelled until it
is pruned and made again. For such a volume the sweep does what the script does when it names one:
it hashes the path of every checkout `git worktree list --porcelain` reports, the main checkout
first, in the same spelling (`ConvertTo-CheckoutRoot` turns git's `D:/…` into `$Root`'s `D:\…`),
and matches the hash against the volume's `<id>`. `tools/linux-build.Tests.ps1` pins that hash for
`D:\workspace\game_engine` to the `049fb17c077a` this machine's volumes carry: if it ever changed,
every unlabelled volume would become an unknown checkout.

#### The stale rule

In this order, a checkout volume is

| | when | because |
|---|---|---|
| kept | a container holds it, running or stopped | docker would refuse; the line names the container |
| **stale** | it has no label and its id is no listed checkout's | an [unknown checkout](#unknown-checkouts-are-removed-by-default) |
| **stale** | its checkout is gone: no `.git` at the root (a worktree git still lists as `prunable`, or a label's root) | nothing can build there again |
| kept | it is the main checkout's, or the checkout's running the sweep | the first is always in use; the second has `-Prune` |
| kept | its checkout exists but this repository does not list it | another clone's: its merge state is not this repository's to judge |
| kept | its worktree is locked (`git worktree lock`) | below |
| kept | its worktree's HEAD has commits `-Base` (`main`) does not | work not yet merged |
| kept | its worktree is merged but has uncommitted changes | work in progress |
| **stale** | its worktree's HEAD is an ancestor of `-Base`, or every commit it has that `-Base` does not is in `-Base` by patch | the work is in `main` |

**A lock keeps a merged worktree.** "Merged" is `git merge-base --is-ancestor`, the test
`git branch --merged main` makes, and it is true of a branch with no commits of its own — which is
every agent worktree for the first minutes of its life, quite possibly while its first container
build runs. The harness locks a live agent's worktree (`locked claude agent <name> (pid …)` in the
porcelain), so a lock is what tells a fresh worktree from a finished one; a person's worktree is not
locked, but a person in the middle of a change has uncommitted edits, which keep it too.

**By patch, because the merge rebases.** A branch is gated "rebased onto `main`" ([what to run](what-to-run.md)),
so its commits usually land in `main` as new commits and the worktree's own are never ancestors
of it. `git cherry main <HEAD>` compares them by patch, and a worktree all of whose lines are `-`
is merged; one conflict resolved on the way changes a patch, and that worktree is kept. Measured on
2026-10-07 over the 32 worktrees git listed besides the main checkout: 13 unlocked ones had their
work in `main`, **2 by ancestry and 11 only by patch** — the ancestry test alone would have kept
eleven of thirteen finished worktrees' volumes for as long as the worktrees stayed on disk.

#### Unknown checkouts are removed by default

An unlabelled volume whose id is no listed checkout's is removed unless `-KeepUnknown` is given.
That is the safer default here, because of what such a volume is and what each mistake costs:

- **No build can reach it.** A build mounts the volumes named by its own checkout's hash; every
  checkout of this repository is in `git worktree list`, and none hashes to this one. Every volume
  the script creates from now on is labelled, so the population is the volumes from before
  2026-10-07, and on that day 194 of 250 were exactly this.
- **Removing one wrongly costs a first run; keeping them costs the disk.** A volume holds a synced
  copy of a checkout and its build trees, nothing that is not rebuilt by the next build. The one
  live checkout this can hit is an unlabelled one of *another clone* on the same machine, whose
  worktrees this repository cannot list; it pays one fresh first run. The 194 cost 99% of 568 GB.

`-KeepUnknown` is for a machine with a second clone whose checkouts predate the labels.

The decision is `Get-VolumeVerdict` in `tools/lib/LinuxVolumes.psm1`, a pure function of the
volumes (names, labels, sizes), the checkouts (present, main, listed, locked, merged, dirty) and the
containers holding volumes; the script only asks docker and git and prints.
`tools/linux-build.Tests.ps1` (CTest `tools.linux_build`, or `Invoke-Pester`) writes a machine down
with a volume for every row of the table and checks every verdict, without a daemon.

### Why eight jobs — and why the worry was wrong

`-Jobs 8` on a WSL2 VM with 12 CPUs and 15.6 GB. The cap was put there for memory: Jolt and flecs
each compile a few very large translation units and the fear was that eight or twelve of those at
once would exhaust the VM. **Measured, that fear is unfounded on this tree.** Sampling the VM's
`MemTotal - MemAvailable` every two seconds:

| Run | Wall | Peak used |
|---|---|---|
| cold `linux-clang-debug` + `linux-gcc-release`, `-Jobs 8` (builds Jolt, flecs, SDL3, SQLite, Recast, Tracy from source) | 4 m 28 s + 5 m 51 s | **3.41 GB** of 15.62 |
| `-Rebuild linux-clang-debug -Jobs 8` | 1 m 36 s | **1.79 GB** |
| `-Rebuild linux-clang-debug -Jobs 12` | 1 m 20 s | **2.29 GB** |

Twelve jobs is 1.2× faster and still uses a seventh of the machine. Eight stays the default
because this box runs several agents and a Windows desktop at the same time and the build is not
the only thing that wants the cores — **not** because memory is tight. Raise it with `-Jobs` if
the machine is yours alone. The two-second sampling interval can miss a spike inside one link
step, so read these as the sustained figure rather than a guaranteed ceiling.

The two `-Rebuild` rows were first labelled "engine code only", and that was wrong: a `-Rebuild`
recompiles the third-party code too (see `-Rebuild` in [the flag table](#the-command)). The object
files under `/deps/<preset>/<name>-build` survive, but ninja's record of which headers each one
read does not, and without it ninja rebuilds the object. The memory figures are still what a
whole-preset compile at eight and twelve jobs uses.

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
| libclang-rt-18-dev | 18.1.3 | Clang's sanitizer runtimes, for `linux-clang-asan` (ci.yml's sanitizer job). `clang` only recommends it and the image installs without recommends, so until 2026-10-07 the preset compiled every object and failed at the first link. Apache-2.0 with LLVM exception, like the rest of LLVM. |
| llvm | 18.1.3 | For one binary, `llvm-symbolizer`, which the sanitizer runtimes look for on `PATH` to print a report's stack as function, file and line; without it they print bare addresses and do not fall back to `addr2line` unless told to. The first `linux-clang-asan` run's use after free (roadmap F14) came back unreadable for want of it (2026-10-07); with it the next run (2026-10-09) named the test's line. Ubuntu's `llvm` is the unversioned links to LLVM 18's tools, the same LLVM `clang` is, and ci.yml installs it too. |
| clang-format | 18.1.3 | Not in `ci.yml`'s list. Without it `tools/new-capability.Tests.ps1` reports its formatting cases as **skipped**, and a check that only ever skips is not a check. See the note in [08 §8.5](../plan/08-toolchain.md#85-build-system-and-ci) about which versions have been measured against the scaffold; 18 now agrees with them. |
| PowerShell | 7.4.12 | Six CTest tests are pwsh scripts (`lint.banned_patterns`, `tools.lint`, `tools.new_capability`, `docs_check`, `tools.docs_check`, `tools.machine_lock`). Without `pwsh` `cmake/EngineTesting.cmake` does not *fail* them, it does not **register** them — so a container without it would make a green run mean less than the hosted one. |
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
| `clang`, `clang-format`, `libclang-rt-18-dev`, `llvm` (LLVM 18) | Apache-2.0 with LLVM exception |
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

Development desktop (i9-10980XE, 36 logical CPUs), Docker Desktop 29.6.1, WSL2 VM with 12 CPUs
and 15.6 GB, `-Jobs 8`.

### The first run of a fresh checkout — which is every agent's first run

Every agent works in a fresh worktree, and a fresh worktree has empty build volumes, so its first
run is a **cold** build whatever the machine has built before: every third-party dependency —
Jolt, flecs, SDL3, SQLite, Recast, Tracy, meshoptimizer — is compiled again, for every preset.
The shared dependency cache takes the *downloads* out of that; the *compiles* it does not touch.
Configure and build of all four presets, no tests, measured on 2026-09-22 from fresh checkouts of
one tree (so the three columns built the same sources):

| | Before: per-checkout downloads | Now, cache empty (the first run on the machine) | **Now, cache warm (every run after that)** |
|---|---|---|---|
| `linux-clang-debug` | 2 m 33 s (configure 79.5 s) | 2 m 46 s (86.3 s) | **1 m 34 s** (18.4 s) |
| `linux-gcc-release` | 4 m 21 s (94.8 s) | 3 m 03 s (27.9 s) | **3 m 11 s** (20.7 s) |
| `linux-clang-minimal` | 1 m 54 s (66.9 s) | 1 m 09 s (21.7 s) | **1 m 10 s** (19.6 s) |
| `linux-clang-no-ecs` | 2 m 14 s (70.2 s) | 1 m 23 s (22.4 s) | **1 m 26 s** (21.0 s) |
| **all four** | **11 m 02 s** (5 m 11 s configuring) | **8 m 21 s** (2 m 38 s) | **7 m 21 s** (1 m 20 s) |

Read it as: a fresh worktree's first four-preset compile check went from **eleven minutes to a
little over seven** (−33%), and the configure half of it from five minutes to under a minute and a
half; the first run on the whole machine is 8½ minutes because each dependency is now downloaded
once rather than once per preset. What remains — about six minutes — is compiling, most of it
third-party code that is identical in every checkout; that is the next thing to share (see
[Follow-ups](#follow-ups)). The warm column ran with `-Offline`, `--network none`, and succeeded:
nothing was downloaded, which the log's `fetch-cache: <name> from ...` lines say too.

Machine state for all three: no GPU lock held, no other container; the host's own CPU total read
15–57% across the runs (the container at eight jobs is about a fifth of this 36-thread host, and an
otherwise idle desktop with other agents' shells sat at 15–25%), so every number is an upper bound
in the usual sense and the three are comparable with each other.

**With `-Test`** add the suite, which is the same cold or warm: about 6½ minutes for all four
(the warm column below). A fresh worktree's full gate is therefore about **14 minutes** with the
cache warm, where it was about 17½.

### Warm runs

Measured when this page was first written, each run with `-Test` so the number is build **and**
suite; a warm run reads the dependency cache but never writes it, so the cache changes nothing here.

| | Cold (first run, before the cache) | Warm: reconfigure, no-op build, test |
|---|---|---|
| `linux-clang-debug` (53 tests) | 4 m 28 s | 2 m 06 s |
| `linux-gcc-release` (53 tests) | 5 m 51 s | 1 m 45 s |
| `linux-clang-minimal` (43 tests) | 2 m 39 s | 0 m 48 s |
| `linux-clang-no-ecs` (49 tests) | 4 m 05 s | 2 m 10 s |
| **all four, one command** | **17 m** | **6 m 55 s** |

Building the image is a one-off **~1 min** on top of the first cold run, and nothing at all once
any checkout on the machine has built it. The source sync is **about 2½ s** every run (682 files;
see [the sync](#the-sync-a-file-gets-a-new-time-only-when-its-bytes-changed)).

A warm run is almost entirely the test suite: a warm **build** of `linux-clang-debug` with no
source change is **6 s end to end including the sync**, of which the container's build step is
2 s. So a four-preset *compile* check — which is what catches five of the six entries in the table
below — costs well under a minute, and `-Test` is what turns it into the seven-minute gate.
`-Filter` narrows the suite while iterating.

The hosted matrix runs its four Linux presets in parallel on four runners, so on a good day it
finishes sooner than this does. That was never the problem: the problem was the day it finished
never.

### Scheduling: warm early, one at a time

- **Start a fresh worktree's first run early, in the background**, before you need its answer —
  as soon as the change compiles on Windows, not when you are ready to commit. It is seven to
  eight minutes of compiling for all four presets with nothing to show for it until the end.
  A warm run afterwards is the minute-long compile check the rest of this page describes.
- **One at a time is the rule, and the lock enforces it**; do not work around it with `-Jobs` or
  a second invocation. A second run waits and says whose build it is waiting for. Three agents
  starting at once is a queue: the third waits for two fresh-worktree builds — a quarter of an
  hour without `-Test`, nearly half an hour with it — before its own starts, which is still far
  better than the two and a half hours three concurrent builds took. `-NoWait` tells you instead
  of queueing.
- **Narrow the first run when the answer can be narrower**: `-Preset linux-clang-debug` is a minute
  and a half fresh, and it is the compiler that finds most of what MSVC misses; run `all` once
  before pushing.
- **`-Filter` while iterating on a test**, `-Test` without it before pushing.
- **Never `wsl --shutdown` or restart Docker Desktop to make a build go faster or to clear a
  stuck one**; it kills every other checkout's build. An orphaned container is removed by the next
  run; a stuck lock expires on its own ten minutes after its holder stopped refreshing it.

### Follow-ups

- **Share the compiles too.** Every checkout compiles byte-identical third-party sources with the
  same flags at the same paths (`/fetch/...`, `/deps/<preset>/...` and `/src` are the same in every
  container), so a machine-wide compiler cache — `ccache` in the image, one volume beside
  `engine-linux-fetch-cache`, `CMAKE_<LANG>_COMPILER_LAUNCHER` on the same configure line — would
  turn most of the remaining six minutes of a fresh worktree's first run into cache hits, and the
  unchanged engine files with them. Not done here: it changes the image and adds a tool to it, and
  it wants its own measurement.
- **`-Rebuild` could leave the third-party objects alone**, which is what this page used to claim
  it did. Deleting `build/<preset>` takes ninja's deps log with it, so every object under
  `/deps/<preset>` is rebuilt as well: 635 of the 1116 edges of `linux-clang-debug`. Removing only
  the engine's own object directories and keeping `.ninja_log` and `.ninja_deps` would fix it.
  That is a change to what `-Rebuild` means, so it wants its own measurement, and a cold compile of
  this preset is a minute and a half.
- **`tools/remote-build.ps1` keeps one dependency directory per checkout on the server** (see
  [remote Linux builds](remote-linux.md)); the same provider would share it there. The server is
  one machine per run and its first configure is two to three minutes, so it matters less.

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

The second row came back on 2026-09-23 in a spelling that **reads as explicit**: `f64{v}` for a
float `v`, twelve lines of `domain/geometry/src/uv_repair.cpp`, merged without this gate and
failing `linux-clang-debug`. To clang a braced functional cast from `float` to `double` is still an
implicit promotion, and `-Wdouble-promotion` counts it; MSVC says nothing. `static_cast<f64>(v)` is
the spelling every compiler here accepts.

## What the first v3 runs found

Until 2026-09-22 no x86-64-v3 Linux preset had ever run a test here: `/proc/cpuinfo`'s flags line is
tab-separated, the configure-time check read it wrong, and every v3 configure was refused
([remote Linux builds](remote-linux.md) has that story). The first runs of the four v3 presets found
two failures. Both passed on MSVC at either baseline and on both v2 Linux builds, and both were
**floating-point contraction**:

| Preset | Where | What | Fix |
|---|---|---|---|
| all four | `domain/geometry` (`morph_tests.cpp`, the LOD DAG's validation) | "triangle normal outside its cluster's cone" on a UV sphere's pole triangle, whose two corners coincide. Its edges are the same vector, so each cross-product component is `x*y - x*y`: zero when both products are rounded, **the product's rounding error** (8.6e-12 on a unit sphere) when one is fused. The validator's degenerate test was absolute (`|n|² <= 1e-24`), the residue passed it, `normalize` returned the zero vector, and the check failed | **Not a looser check.** The residue was a symptom: meshoptimizer's cones had no margin for *any* other arithmetic, including the 16-bit grid the rasterizers actually draw. The builder now refits every cone to the triangles as floats and on the grid with an explicit margin, leaves out triangles thinner than a grid step, and the validator checks that the margin is there ([geometry](../subsystems/geometry.md#normal-cones-fit-to-what-is-drawn)) |
| `linux-gcc-release` | `domain/anim` (`anim_tests.cpp`, the vectorized kernels against `Mat4 operator*`) | `==` between two differently spelled computations of one product. GCC fuses multiply-adds by default and fused different ones on the two sides — including across the SSE intrinsics, whose `_mm_add_ps` is a plain vector `+` in GCC's headers — so 63 of 192 `local_to_model` entries differed by up to 2 ulp and 76 of 144 skinning entries by up to 3 | `==` where the build evaluates as written (MSVC, and any build without FMA), 8 ulp where it may contract, with the reason in the test ([anim](../subsystems/anim.md#determinism-and-what-a-v2-and-a-v3-build-disagree-about)) |

**The lesson, for whoever writes the next float comparison.** A v3 GCC or clang build is not the
same arithmetic as the MSVC build or any v2 build, and that is not a bug in either: GCC contracts
`a*b + c` into one rounding by default (`-ffp-contract=fast`, kept for C++ even in ISO mode), clang
does it within a single expression, MSVC's `/fp:precise` never does, and the FMA instruction exists
only at v3. So:

- a test that compares two **differently spelled** computations with `==` is asserting the
  compiler's choice of what to fuse, not the code — clang passed the anim test only because
  neither side happens to be one expression it can fuse, which is spelling, not a guarantee;
- code that relies on a difference of equal products being **exactly zero** (`x*y - x*y`, a
  degenerate triangle's cross product) is relying on there being no FMA;
- where the answer matters to correctness — a normal cone has to hold whatever arithmetic computes
  the normal, including the GPU's — the fix is a **margin in the producer**; where it matters only
  to the test, a bound with its reason written beside it.

**Since 2026-09-24 the tree pins `-ffp-contract=off` for every target** ([ADR-0035](../adr/0035-no-floating-point-contraction.md),
`cmake/EngineFpContraction.cmake`), so GCC and clang at v3 now compute what MSVC and the v2 builds
compute. Until then it deliberately did not ([anim](../subsystems/anim.md)'s "Determinism" had the
reasoning, and `domain/physics` was the one place that did, for Jolt), and what changed the answer
was a result that leaves the machine: at v3 the content build made a different LOD DAG from the
same glTF bytes on GCC and on clang than on MSVC, under the same cache key, and turning the flag off
for the content build's modules alone leaked through the linker's choice among header-only
functions' weak copies ([geometry](../subsystems/geometry.md#the-same-bytes-from-every-toolchain)).
The lesson above still stands for arithmetic nobody here compiles — the GPU's, and a dependency's
explicit FMA — and a v3 preset is still where a float comparison only MSVC has seen should run.

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
This machine also has an old, broken `Ubuntu` WSL registration that makes WSL look unwell; it is
not Docker's, and neither `wsl --shutdown` nor restarting Docker Desktop is ever the fix here —
both kill every other checkout's build.

**`unable to resolve docker endpoint: context "desktop-linux": open ...\.docker\contexts\meta\...\meta.json: The process cannot access the file because it is being used by another process`.**
Another Docker client on the machine had the context file open at the moment this one read it — a
Windows sharing violation, not a daemon that is down. Run the command again (it passed on the
second try on 2026-09-23); this is not a reason to touch Docker Desktop or WSL either.

**`linux-build: waiting; held by ...`.** Another build has the machine; this one starts when it
finishes. The line names the presets and the checkout. If the holder is gone for good, its lease
runs out ten minutes after its last refresh and the next waiter breaks it; nobody needs to delete
the file by hand, and nobody should delete an unexpired one.

**`another build container is running outside the lock`.** A container from these images is
running that no lock accounts for — most likely a build from a checkout whose `linux-build.ps1`
predates the lock. The build waits for it; the `docker rm -f <name>` the line prints ends it if
you know it is abandoned. (An orphan of *this* script, whose owning process is gone, is removed
without asking: `removing <name>, an orphan whose linux-build.ps1 (pid N) is gone`.)

**`fetch-cache: <name> is not cached yet` on a run you expected to be warm.** Somebody changed that
dependency's declaration — a pin, a URL, any detail at all is in the key — so this is the first
configure on the machine to need the new one. `cmake --log-level=VERBOSE` prints what each entry is
keyed on. It is fetched once and every checkout after that reads it.

**`Permission denied` under `/fetch/...` during a build.** A dependency's build tried to write into
its own source tree, which is shared by every checkout and read-only for that reason. Point its
output at the build tree (usually an option of its CMakeLists) rather than making the entry
writable.

**The build is killed with no message, or a link step dies.** The WSL2 VM ran out of memory. Lower
`-Jobs`. Docker Desktop's memory allowance is in its settings, or `.wslconfig`.

**A test fails here and passes on Windows.** That is the machine doing its job; it is not a
container artefact until proven otherwise, and the table above is five examples of it being real.
`-Shell` puts you in the container with the build tree mounted and `ctest -R <name> -V` available.

**`sync: N files` where N looks wrong.** The excludes are in `$SyncExcludes` in
`tools/linux-build.ps1`. A newly added top-level directory that should not cross over goes there.
The same line says how many files were copied and names them when there are twelve or fewer:
those are exactly the files whose bytes changed, and the only sources this run recompiles.

**A test that behaves like the code before your edit, or an undefined reference to a function
that plainly exists.** That is a stale object: a source whose new text reached `/src` without a
time newer than the object built from its old text. The sync has not been able to do that since
2026-09-24 (see [the sync](#the-sync-a-file-gets-a-new-time-only-when-its-bytes-changed)), but a
volume the old sync left stale stays stale, because its bytes are already current. Run `-Rebuild`
once, or `touch` the file inside `-Shell`. Touching it on Windows does nothing now.

**Everything rebuilds although nothing changed.** The sync copies only files whose bytes changed,
so this should not happen. If it does, the usual cause is a `-DFETCHCONTENT_*` or preset change that moved
a dependency's source directory, which invalidates the objects that included its headers. The
first run of a checkout that already had build volumes before the dependency cache existed is
exactly that, once: every source directory moved from `/deps/<preset>/<name>-src` to
`/fetch/<name>-<key>`. `-Prune` first to also drop the old `-src` copies it no longer reads.
