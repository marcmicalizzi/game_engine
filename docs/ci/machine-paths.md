# No path of this machine in the repository

The repository is public, and it outlives every checkout: a path of the development machine in a
page, a script or a test tells a reader where the owner keeps their files, and a path into an
agent's worktree names a directory that is gone after the next cleanup (the owner, 2026-11-18). So
no tracked file names one, and `tools/lint.ps1` checks it (`machine-path`, roadmap T175). This page
says what the rule reports, how a path is written instead, and why the machine-wide locks are the
exception.

## What the rule reports

Every tracked text file is read, whatever its kind: the 243 lines the rule was written from were
pages, PowerShell, content READMEs, a CMake file and two tests, so a rule over C++ alone would have
seen three of them. A file is skipped only when it is binary (a NUL in its first 8,000 bytes, git's
own test). What git ignores is not read, since it never reaches the repository (a fetched material
pack, a derived-data root, a scratch note). A tree that is no checkout skips the rule and prints one
line saying so: the Linux container's copy is synced without `.git` (it would arrive broken from a
worktree, [local Linux builds](local-linux.md)), and the Windows run and CI's checkouts read the
same files. Reported:

- **A user's profile**: `<drive>:\Users\<name>` in either slash, its Git Bash and WSL spellings
  (`/c/Users/<name>`, `/mnt/c/Users/<name>`), and a POSIX home (`/home/<name>`, macOS's
  `/Users/<name>`) where it begins a path rather than continuing one or a URL.
- **The workspace folder at a drive's root**, the folder that holds the checkouts:
  `<drive>:\workspace` in every slash, and `/<drive>/workspace` and `/mnt/<drive>/workspace`.
- **An absolute path into a harness worktree**, wherever its checkout is:
  `...\.claude\worktrees\agent-<hex>` or `slot-<n>` under a drive or a root.

## What it lets through

What is the same on every machine: a toolchain's install folder (`C:\Program Files\...`,
`C:\Windows\...`), a placeholder (`<dir>`, `<workspace>`, `...`, `%USERNAME%`, `$env:X`), a neutral
name a test or an example uses for "somebody" (`someone`, `user`, `runner`, `Public`, `Default`), a
sample drive that stands for "another disk" (`D:\games`, `E:\cc`, `D:\work\engine`), and a worktree
named relative to its checkout (`.claude/worktrees/agent-...`). `tools/lint.Tests.ps1` holds each
kind: thirteen reported and eight let through, with their samples built from pieces so that the
test file, which the rule reads too, names no path itself.

## Spelling a path

- **A folder beside the primary checkout is named by its name**: `game_engine_local\` (the local
  folder: generated images and meshes, workflow files, captures, scratch), `forest_wildlife_poc\`,
  `game_engine_assets\`, `ge-gate\`. AGENTS.md says once where they are; a page that needs one
  writes its name and the path inside it.
- **The folder that holds them** — where the machine-wide locks and the lock protocol's page
  `GPU-LOCK.md` live — is `<workspace>`.
- **A test's sample** is a neutral path (`D:\work\engine`), and an expectation that is a hash of the
  sample moves with it: `tools/linux-build.Tests.ps1` pins the checkout volume id on
  `D:\work\engine` (`89e4adbf589b`), and the same function still gives the main checkout the
  `049fb17c077a` its volumes carry.
- **A test that looks for something on the machine** derives where it is from the tree's own git
  metadata, never a literal: `tissue_tests.cpp` finds the supine packet in `game_engine_assets`
  beside the primary checkout by reading the source tree's `.git` (a directory in the primary, a
  file naming `<common>/worktrees/<name>` in a worktree), and skips where it is not.
- **Experiment pages are records**: only a path's spelling changed in them, never a number, a date
  or a sentence.

### The local folder in a command

A command someone will paste must still run, so it names the local folder as
`$env:ENGINE_LOCAL_ROOT` rather than a placeholder:

```powershell
Import-Module ./tools/lib/LocalRoot.psm1; $env:ENGINE_LOCAL_ROOT = Get-EngineLocalRoot   # once a shell
pwsh tools/blender-run.ps1 -Script $env:ENGINE_LOCAL_ROOT\blender-kits\ruined-wall\scripts\ruined_wall.py ...
```

An environment variable was chosen over a path relative to the repository because the checkouts
are not all at the same depth: `..\game_engine_local` is right from the primary checkout and wrong
from every worktree under `.claude\worktrees\`. The tools themselves need no variable:
`generate.ps1`, `blender-run.ps1`, `e10-harness.ps1` and `flythrough.ps1`'s overlay take
`-LocalRoot` or `ENGINE_LOCAL_ROOT`, and otherwise `Get-EngineLocalRoot`, which finds the primary
checkout from git's common directory and returns the folder beside it named after it with `_local`
(`game_engine` → `game_engine_local`) — the rule the compiler and shader caches already followed
(`<main checkout>_local`, `tools/lib/CompilerCache.psm1`), so every worktree resolves to the same
folder as the primary does, and on the development machine to the folder the four tools named
before. Off Windows the default was `$HOME/game_engine_local`; it is now the same folder beside the
checkout there too. Outside a checkout, with nothing named, a tool refuses and says to set the
variable. The helper's cases are in `tools/generate.Tests.ps1` (CTest `tools.generate`, its first
user), over a throwaway repository and a worktree of it.

A lock is named by what it is for: `tools/gpu-lock.ps1 status -Kind linux-build` says who holds
the container build lock, at the path every tool takes it at.

## The machine-wide locks do not move

The GPU lock (`foundation/gpu_lock`'s `default_path()`, `tools/lib/MachineLock.psm1`'s
`Get-MachineLockPath`, `tools/gpu-lock.ps1`), the Linux build lock, and the directory
`cmake/EngineTesting.cmake` reads to choose how many GPU chains a suite runs are the lock
protocol's fixed paths in `<workspace>`, and they are left exactly as they were. Deriving them from
where a checkout is would be wrong three ways ([gpu_lock](../subsystems/gpu_lock.md)): tools that
are not this repository's (the Blender agent, `GPU-LOCK.md`'s own snippet) take the same file at
the same fixed path; binaries built at other commits are running now with the path compiled in; and
a checkout that is not a worktree of the primary would derive another folder and so take another
lock, which is no lock. So each of those lines carries `engine-lint: allow-machine-path <reason>`,
the CMake file is on the exemption list (a marker there would be a `cmake/` change for a comment),
and only the comments around them were respelt. On the development machine, before and after
T175: the GPU lock `<workspace>\gpu.lock`, the build lock `<workspace>\linux-build.lock`, ccache's
`base_dir` `<workspace>` and its cache `game_engine_local\ccache`, the shader cache
`game_engine_local/shader-cache`, and the checkout volume id `049fb17c077a`, each printed from the
base's modules and the branch's and compared byte for byte.

## Exemptions

The way out is a marker on the line, `engine-lint: allow-machine-path <reason>`, in whatever comment
syntax the file has; a marker with no reason does not count. A file that cannot carry one is a line
of `tools/machine-path-exemptions.txt`, `<file>: <reason>`, as the size table's exemptions are, and
the lint refuses a line with no reason, a line naming no tracked file, and a line for a file the
rule no longer finds anything in, so the list only shrinks. When the rule landed it held three
files: the two accepted ADRs that name the lock protocol's page (0049 and 0062; an accepted ADR is
never edited) and `cmake/EngineTesting.cmake`; three lines carry the marker (the two lock defaults
in `MachineLock.psm1` and the one in `gpu_lock.cpp`).

## What the tree held when the rule landed

Measured on batch 68's stack (`6a743042`) with `git grep -I -i -E` for the workspace folder and the
account's profile: 96 files, 243 lines, sorted by what became of them:

| Lines | Files | Kind | What was done |
|---:|---:|---|---|
| 117 | 49 | experiment records | spelling only: a sibling by its name, the folder as `<workspace>` |
| 44 | 21 | page and README prose | the same spelling |
| 17 | 8 | page and README commands | `$env:ENGINE_LOCAL_ROOT`, `-Kind linux-build`, `$PWD` |
| 24 | 15 | tool and header comments | respelt |
| 10 | 5 | tool help examples | `$env:ENGINE_LOCAL_ROOT` |
| 4 | 4 | tool defaults | derived (`tools/lib/LocalRoot.psm1`) |
| 20 | 2 | test samples | a neutral root, or derived from the tree's git metadata |
| 4 | 3 | the locks' fixed paths | kept byte for byte: marked (3) or exempted (1) |
| 2 | 2 | accepted ADRs | exempted |
| 1 | 1 | a user's profile (a session's scratchpad) | `<session scratchpad>` |

Two more the grep did not count were cleaned too: the GPU server's home directory in
[remote builds](remote-linux.md), which the rule's POSIX case found, and a network drive of the
development machine named in [content generation](../content-generation.md), found by reading
(the rule does not know a network drive's letter). The rule adds about 3.5 s to the lint (23.8 s
to 27.3 s for the whole tree, on 2026-11-18 at 83 % CPU load from other builds).
