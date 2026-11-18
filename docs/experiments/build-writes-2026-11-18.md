# T185: what a build writes to the drive

The owner, 2026-11-18: the slots exist for warmer builds and fewer total writes, and drive wear is a
cost to stay aware of. This page measures the bytes an `msvc-debug` build writes, warm and cold, and
what ccache's `hard_link` would change. The rule that follows is in
[what to run](../ci/what-to-run.md#what-a-build-writes).

## Setup

`msvc-debug` at 5d0634bb in a slot worktree, `CMAKE_BUILD_PARALLEL_LEVEL=8`, ccache 4.14.1 with the
machine's cache (`base_dir` the workspace, `hash_dir=false`, 30 GB, `/Z7`), MSVC 18. Machine state:
about seventeen agents building beside every run, free commit 19 to 55 GB, so every time is an
upper bound; the bytes are the build's own. The cold builds went into binary directories of their
own inside the slot, with the dependency sources taken from the slot's tree (no download); the
`hard_link` runs used a cache directory of their own, so the machine's cache and its settings were
never changed.

Three measures, because none of them alone is what reaches the drive:

- **counted**: the build's process tree inside a job object of its own, whose I/O accounting keeps
  the counts of processes that have exited (a compiler, ccache): every write made through the file
  API, which is objects, ccache's stores and ninja's logs. It misses the linker's outputs, which it
  writes through mapped files.
- **files**: the bytes of the build tree's files written since the start. A debug link is
  incremental (`/INCREMENTAL`, the `.ilk` beside each executable) and patches its outputs in place,
  so this overstates a relink about twenty times.
- **pages**: for the linker's outputs, the 4 KiB pages whose bytes changed (a floor: a page written
  back unchanged is not seen).

## Results

| Build | Time | Written |
|---|---|---|
| a one-line code change to `systems/renderer/src/overlay.cpp` in a warm slot: one compile, the renderer's `.lib`, 19 links | 41 s | 0.12 GB counted + 0.24 GB of pages changed in the outputs (the `.lib`'s 136 MB whole, 94 MB of 2.6 GB of `.pdb`, 10 MB of `.ilk`, 0.3 MB of `.exe`), so about 0.4 GB; files 5.5 GB |
| a slot catching up two hours of `main` (1,164 steps, mostly ccache hits) | 1,044 s | 13.3 GB of files rewritten (7.1 GB counted) |
| cold, a fresh binary directory, `engine_physics_tests`, `engine_doc_tests` and `engine_geometry_tests` (251 objects, 264 MB of them), every object a hit, copied out of the cache | 104 s | tree 0.66 GB, 0.45 GB counted |
| the same with ccache's `hard_link`, every object a hit | 87 s | tree 0.66 GB, 0.19 GB counted: the 264 MB of objects are links to the cache's files |
| the same with `hard_link`, every object a miss (an empty cache of its own) | 355 s | 0.66 GB, 1.39 GB counted; the objects are stored by linking, so the cache took no second copy |

A warm `msvc-debug` tree is 17.5 GB in 21,000 files: `.pdb` 5.8 GB, `.ilk` 4.1 GB, objects 3.4 GB,
executables 1.8 GB, libraries 1.4 GB. **A fresh worktree's first build writes the whole tree, about
18.5 GB** (17.5 GB, every link's output written fresh, plus 0.9 GB of dependency sources it
fetches; the checkout itself is 0.05 GB), derived from the tree's size: a whole cold build was not
run, for the reason under Caveats.

The machine's compiler cache, read without changing it (`ccache -x` and the files' times):
142.3 GiB of objects held in 28.0 GiB, 5.1 to 1, 73,768 files; by last use, 3.1 GB in the last
hour, 5.8 GB one to three hours old, 0.6 GB three to six, 5.4 GB six to twelve, 14.7 GB twelve to
24, 0.4 GB older. Over 20 minutes of the afternoon it took 1,468 misses and about 3,000 writes, and
ran 38 cleanups.

## What surprised me

- **A static library is archived again whole when one of its objects changes**, so a one-file change
  costs at least its module's `.lib`: the renderer's 136 MB was the largest single write of the
  incremental build, more than all nineteen relinks' changed pages.
- **The relinks are cheap.** Nineteen executables relinked with 5.5 GB of outputs, and 0.1 GB of
  their pages changed: the incremental linker earns its `.ilk`.
- **`hard_link`'s warnings were partly not true here.** On Windows a linked object is not read-only
  (the 4.13 manual: "As of version 4.0, ccache makes stored and fetched object files read-only"),
  and a hit refreshed the cache's own entries' times but not the linked objects' (one of 254 in the
  other tree changed), so a hit in one checkout does not make another relink.
- **A build directory that points CMake at dependency sources elsewhere
  (`FETCHCONTENT_SOURCE_DIR_<NAME>`) misses the cache** for every object that includes one of them,
  204 of the 251: `base_dir` makes the include paths relative to the build directory, and
  `../msvc-debug/_deps/...` is not `_deps/...`. Sources at `<build>/_deps/<name>-src`, where every
  worktree has them, hit.

## What it decides

- **The pool's saving is the tree, not the cache**: a fresh worktree writes about 18.5 GB before
  its first useful edit, a warm slot about 0.4 GB for a one-line change.
- **Hard links stay off.** They would save the objects' share of a cold build, 3.4 GB of 18.5 (the
  subset's 0.26 GB of 0.45 counted, measured) and nothing of an incremental one; but ccache stores a
  linked object uncompressed ("Files stored via hard links cannot be compressed", the 4.13 manual,
  `hard_link`), and `/Z7` puts the debug information in every object, which is why they compress
  5.1 to 1: the same 30 GB would hold a fifth of the objects, 6 GB, about the last two hours of use
  instead of a day, and fresh worktrees would compile again what they now copy. A linked object is
  also the cache's own bytes: a tool that rewrote one in place (nothing here does today) would
  rewrite every checkout's copy, and a corrupt object (LNK1236, four times on 2026-11-14) would be
  served to every checkout until it was replaced. ccache's `file_clone` has none of these faults
  ("completely safe to use, but not all file systems support the feature") and is not available on
  NTFS, so it is not an option on this machine's volumes.

## Caveats

- The whole cold pair (copies, then `hard_link` on a full hit) was not run: a hit-only `hard_link`
  build needs a cache of its own filled first, a whole miss build of one to two hours, and the
  row's brief held cold builds to after 20:00, which its time and call budget did not reach. The
  subset above measured each case at a smaller scale, and the whole tree's figures are derived
  from its size.
- The counted writes are what processes asked to write; temporary files deleted before the file
  cache flushed them count here and never reach a disk. The changed pages are a floor.
- `tools/disk-writes.ps1` gives the machine-wide view (per physical disk and per volume) over a
  window; the per-build figures here are from a job object, because a compiler lives less than one
  of its samples.
