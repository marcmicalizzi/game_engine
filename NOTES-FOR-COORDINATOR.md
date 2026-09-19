# Notes for the coordinator — local Linux build branch

Scratch file for the merge. **Delete it when this branch lands.**

Branch: the worktree `.claude/worktrees/agent-a4fae07be8d0ea8c3`.
Task: give the project a local Linux build (container on the Windows desktop) and fix everything
the four Linux presets found.

## Preset status

| Preset | Build | Tests | Note |
|---|---|---|---|
| `linux-clang-debug` | green | **53/53** | Clang 18.1.3, Debug |
| `linux-gcc-release` | green | **53/53** | GCC 13.3.0, RelWithDebInfo, Tracy compiled in |
| `linux-clang-minimal` | green | **43/43** | `ENGINE_MINIMAL=ON` |
| `linux-clang-no-ecs` | green | **49/49** | `ENGINE_WITH_ECS=OFF`; `animation` follows it off |
| `msvc-debug` (Windows) | green | **53/53** | re-verified after every fix: `tools/dev.ps1 build` 3 m 00 s, `tools/dev.ps1 test` 5 m 51 s, cold. The GPU suites ran against the real RTX 5090 (no `device unavailable` line anywhere in the log) and passed; no driver trouble, no firewall prompt. |

Nothing is red. Every GPU and window test skips with a reason (`device unavailable: no Vulkan 1.3
driver (ICD)…`, `no display: SDL_Init(video)…`) exactly as on the hosted runner — none fails, none
hangs.

## Files I touched that another agent owns

Only one is in a declared ownership list, and the edit is far from the part that agent owns:

| File | Owner | What I changed | Conflict risk |
|---|---|---|---|
| `apps/engine_view/main.cpp` | CPU-baseline agent (owns *one call at the top of main*) | removed a dead `const char* page_source_name` local (~line 1293) and inverted the `attach_page_source` branch that assigned it (~line 1424). `-Wunused-but-set-variable`. | **low** — both edits are ~1300 lines below the top of `main`. |

Nothing else of mine is on any of the three lists. In particular I did **not** touch
`cmake/EnginePhysics.cmake`, `CMakePresets.json`, `cmake/EngineOptions.cmake`,
`core/platform`'s CPU check, `domain/gfx/src/device.cpp`, `include/domain/gfx/device.h`,
`bindless.*`, the `gpu.adapters` method, `tools/package-tests.ps1`,
`systems/renderer/src/{streaming,gpu_scene,page_source}.cpp`, or
`domain/gfx/src/cluster_acceleration.cpp`.

**No Linux preset needed a `CMakePresets.json` change.** The four Linux entries configure and
build as they are.

## Everything this branch changes

New:

- `tools/ci/linux.Dockerfile` — Ubuntu 24.04, ci.yml's apt list, CMake 3.28.3, Clang 18, GCC 13,
  clang-format 18, PowerShell 7.4, non-root build user.
- `tools/linux-build.ps1` — the one command. Also `-Docs`, which runs `tools/docs-gate.sh` and
  `tools/docs-check.sh` in the container (both pass on this branch against `main`).
- `docs/ci/local-linux.md` — the page.
- `NOTES-FOR-COORDINATOR.md` — this file. Committed so it travels with the branch; delete it on
  merge.

Edited, shared with other agents (**merge-conflict candidates, all additive**):

- `AGENTS.md` — a paragraph in the build section, and one clause in the presets paragraph.
- `docs/plan/08-toolchain.md` — one §8.5 status bullet, and clang-format 18.1.3 added to the
  measured-versions sentence.
- `docs/ci/self-hosted-runners.md` — a five-line pointer block at the very top.
- `docs/subsystems/sim.md` — one invariant bullet.
- `docs/subsystems/renderer.md` — one clause in "What a caller says".

Edited, engine code (one commit each, message names the warning or the bug):

- `systems/animation/src/animation.cpp` — `-Wdouble-promotion` ×12, f32 literals into an f64
  constructor.
- `systems/renderer/include/systems/renderer/scene.h` — `SceneAnimation animation{}`,
  `-Wmissing-field-initializers` at three call sites.
- `apps/engine_view/main.cpp` — dead local, `-Wunused-but-set-variable` (see the table above).
- `domain/ecs/src/identity.cpp` — a `flecs::entity(...)` call GCC finds ambiguous and Clang and
  MSVC do not.
- `domain/sim/include/domain/sim/tiers.h` — **a real bug**: `TierChange`'s two bytes of tail
  padding were uninitialized and the determinism test `memcmp`s them. Now a named `u16 pad = 0`;
  `ENGINE_EXPECT_SIZE(8, 4, TierChange)` is unchanged, so no size-table churn.
- `tools/new-capability.Tests.ps1` — `Join-Path ${env:ProgramFiles(x86)} …` threw on Linux, so
  the optional-clang-format probe crashed the test instead of skipping.

No suppressions, no pragmas, no warning turned off, no `NOLINT`.

## main moved while this ran — re-run the gate after the merge

This branch is based on `9824902`. While it was building, `main` advanced to `634daec` with
`7d79463 renderer: a staged page's 8-bit indices are its own` and
`634daec docs: the quarter budget with the ray chain running, measured`, which touch
`systems/renderer/{include/systems/renderer/streaming.h,src/streaming.cpp,tests/streaming_tests.cpp}`.
**Those commits have not been compiled by any Linux compiler.** After the merge:

```powershell
pwsh tools/linux-build.ps1 -Preset all -Test
```

is the whole verification, and it is minutes. That is the point of the branch.

## If you are merging this beside the other three branches

- The docs edits are all *insertions* at distinct points; a three-way merge should take them.
- `apps/engine_view/main.cpp` is the only code file with a plausible overlap, and the CPU-baseline
  agent's edit is at the top of `main` while mine is in the middle of the render setup.
- If `CMakePresets.json` gains a preset from another branch, `tools/linux-build.ps1`'s `$AllPresets`
  array is the only place that lists the four Linux ones; a new Linux preset can be passed to
  `-Preset` without editing anything.
