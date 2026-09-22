# ADR-0034: Host tools compile for the build machine, never for the target baseline

- **Status:** Accepted
- **Date:** 2026-09-22
- **Plan references:** docs/plan/08-toolchain.md §8.9. Refines [ADR-0031](0031-minimum-cpu-x86-64-v3.md) decisions 1 and 5 — which target carries the baseline, and who says so when a CPU is below it — without superseding anything in it.
- **Docs touched:** `AGENTS.md` (the preset paragraph), [docs/plan/08-toolchain.md §8.9](../plan/08-toolchain.md#89-the-cpu-baseline-and-what-each-dependency-does-about-it), [docs/ci/remote-linux.md](../ci/remote-linux.md), [docs/subsystems/schema.md](../subsystems/schema.md), [docs/subsystems/platform.md](../subsystems/platform.md), and the ADR index's note under ADR-0031

## Context

ADR-0031 put the instruction-set flag on the top-level directory so that every target inherits it,
and decision 5 turned "a v3 binary on a v2 machine" from a trap into a sentence: every `main()`, the
test main and the bench main call `platform::require_cpu_baseline()` first, which prints one line
and exits 78. Both rest on an assumption nobody wrote down — that every executable the tree builds
is a *product*: something that runs on a user's machine, or stands in for one in a test.

One is not. **`tools/schemac`** generates the schema headers, and the build runs it. It is a
standalone, standard-library-only tool that links no engine module — by design, so it builds
before `core/` exists — so it has no `require_cpu_baseline()` to call. Real hardware found the
hole on 2026-09-20: a v3 build natively on the project's headless GPU server (Xeon E5-2670, Sandy
Bridge: AVX, and none of AVX2, FMA, BMI1/2, F16C, LZCNT or MOVBE) compiled for six minutes and then

```
[126/976] schemac: schemas
FAILED: [code=260] schemas/generated/schemas/include/schemas/provenance.h ...
```

— ninja's 256 plus SIGILL, the tool having died on the first AVX2 instruction in its own code, with
nothing printed. `engine-cli` from the same build printed ADR-0031's line correctly. The decision
worked where it was applied; this was the hole beside it.

The interim fix refused at **configure** time: a native Linux configure with `ENGINE_CPU_BASELINE=v3`
on a machine whose `/proc/cpuinfo` lacked `avx2` stopped with a message naming the v2 presets, with
an `ENGINE_ALLOW_UNRUNNABLE_BASELINE` escape hatch. (Its first pattern also never matched the
kernel's tab-separated `flags` line, so for its first two days it refused every v3 configure on
every Linux machine, AVX2 or not; the container found that the morning this was decided.) That
stopped the six wasted minutes, and it is the wrong rule: it forbids building a v3 product on a v2 machine at all, when the only part of that
build that cannot work is a tool that never ships. Three facts decide it:

- **A host tool's speed is irrelevant and its portability is not.** schemac spends milliseconds a
  build. What it must do is run on whatever machine is building, including one below the product's
  baseline — a CI runner, a packaging box, a developer's older laptop, the Sandy Bridge server.
- **Cross-compiling a v3 product from a v2 build machine has to work.** It is the ordinary case for
  a build farm and costs nothing to allow: the compiler emits AVX2 code without needing to execute
  it. The only obstacle is the build executing its *own* output, which is exactly the host tools.
- **The baseline is a promise to users** (ADR-0031). A host tool is not delivered to users, so the
  promise has nothing to say about it, and applying it anyway bought nothing and cost a build.

Alternatives considered:

| Option | Why not |
|---|---|
| Keep the configure-time refusal | Forbids a legitimate build (v3 product, v2 builder) to protect a tool that did not need the flag. It is also Linux-only and reads `/proc/cpuinfo`, so it is one heuristic about one machine rather than a rule. |
| Give schemac its own `require_cpu_baseline()` | It would then print a sentence instead of dying, and the build would still fail — the right message for the wrong outcome. It would also pull `core/platform` into a tool that exists to build before `core/`. |
| Compile host tools at an explicit `-march=x86-64-v2` | Picks a *different* promise instead of none. A host tool should run wherever the compiler that built it runs, and that is the compiler's default, which is the build platform's own floor (x86-64 on the distributions and MSVC this tree uses; on a distribution whose default is higher, the OS already requires it). |
| Build host tools in a separate host configuration (`ExternalProject`, a second toolchain) | The right answer for a *different architecture* (an ARM target built on x86), and far more machinery than an ISA-level difference needs. Recorded under "Revisit when". |
| Mark nothing, and document "build v2 on v2 machines" | Leaves the next generator tool to rediscover this on the one machine below the baseline, six minutes into a build. |

## Decision

1. **Two kinds of executable, and only shipped ones carry the baseline.** A *shipped* target runs on
   a user's machine or stands in for one: every engine module, app, test and bench, and every
   third-party library they link. It carries `ENGINE_CPU_BASELINE` exactly as ADR-0031 decides. A
   **host tool** is anything a build step runs on the build machine — today `tools/schemac` — and
   it carries **no instruction-set flag from `ENGINE_CPU_BASELINE`**: it is compiled for the
   compiler's default, whatever the baseline is.

2. **The classification is declared once, where the tool is declared.** `engine_host_tool(<target>)`
   in `cmake/EngineCpuBaseline.cmake`, called right after the tool's `add_executable()`, removes the
   baseline flags from the target and records it as a host tool. It shares the flag-removal with
   `engine_strip_cpu_baseline()`, which stays what ADR-0031 made it: the one exemption *inside* the
   product, `core/platform`, whose startup check must run on the CPU it refuses. The two are
   different categories and have different names on purpose — a module calling the host-tool
   function to escape the baseline would be exactly the unravelling ADR-0031 forbids, and would
   show up by name in the check below.

3. **The configure-time refusal is removed**, and with it `ENGINE_ALLOW_UNRUNNABLE_BASELINE`. A v3
   build on a v2 machine now builds to the end. It fails where it should: at the first **engine**
   binary that runs, with ADR-0031's one line and exit 78 — in a normal build that is the first
   CTest test; nothing in the build itself runs a shipped binary.

4. **What stays is a check, and it reads the build rather than the source.** `build.cpu_baseline`
   (`cmake/CheckCpuBaseline.cmake`, registered for every Ninja preset) reads
   `compile_commands.json` and `build.ninja` after the build and fails if:
   - a host tool's or the floor's translation unit carries **any** instruction-set flag;
   - any other translation unit lacks **any** baseline flag, as a whole token — ADR-0031's one-off
     proof, "846 of 851 translation units carry `/arch:AVX2` and the five that do not are
     `core/platform`'s", now taken on every run;
   - a build step **runs** an executable of this build that is not a declared host tool. This is
     what makes the rule mechanical: the next generator added without `engine_host_tool()` fails
     on the machine that adds it, not on the one machine below the baseline.

   `build.cpu_baseline.self_test` drives the same classification with synthetic input whose
   answer is known, so a check that stopped matching anything fails rather than passing quietly.

## What the cross-check measured

On the machine that found the hole, 2026-09-22: the Xeon E5-2670 (Sandy Bridge; its
`/proc/cpuinfo` lists no `avx2`), this tree configured as `linux-server` with
`-DENGINE_CPU_BASELINE=v3`, natively, in a build directory of its own, GCC 14.3:

| Step | What happened |
|---|---|
| configure | succeeded — `engine CPU baseline: v3 (-march=x86-64-v3)`, `host tools [engine_schemac], floor [engine_platform]`; there is no refusal left to stop it |
| build | **all 976 targets, 0 failed**, 10 min 7 s (the machine was at a load average of 15 from other work); `[117/976] schemac: schemas` — the step that was ninja's code 260 — now passes |
| `tools/schemac/schemac` with no arguments | exit 2, its usage error: it runs on the machine that built it |
| `bin/engine-cli engine.methods` | ADR-0031's one line — *this build needs x86-64-v3 and this CPU has no AVX2, FMA, BMI1, BMI2, F16C, LZCNT, MOVBE* — and exit **78** |
| `ctest` | 49 of 52 fail, every one an engine test executable exiting 78 with that line; the first is `schemac` (#4), which is `engine_schemac_tests` — a *test* of the tool, so it carries the baseline, as it should. The three that pass are `build.cpu_baseline.self_test`, `tools.docs_gate` (bash) and `build.cpu_baseline` itself, which read files and run no engine code: *841 translation units: 829 carry `-march=x86-64-v3`, 7 of 7 host-tool [engine_schemac] and 5 of 5 floor [engine_platform] carry no instruction-set flag, and the build runs [engine_schemac]* |

So **nothing in the build runs an engine binary**. The first place a v3 engine binary runs is
whatever runs after it — CTest's first test, or `tools/remote-build.ps1`'s adapter report — and
that is where a machine below the baseline is told so, in one line, with exit 78. The same check
on every other preset it ran on says the same shape — the twelve without a flag are always
schemac's seven and `core/platform`'s five: `msvc-debug` 848 of 860 translation units carry
`/arch:AVX2`, `msvc-release` 849 of 861, `msvc-minimal` 489 of 501, and the server's
`linux-server` (v2) 829 of 841 carry `-march=x86-64-v2`.

## Consequences

**Easier.** A v3 product builds on any x86-64 machine that has the compiler, which is what a build
farm, a packaging step or a CI runner below the baseline needs. The one place a v3 build on a v2
machine fails is the place ADR-0031 designed to fail — with a sentence. The classification of
every translation unit in the tree is now a test result rather than a paragraph.

**Harder, slightly.** A new tool the build runs needs one line, `engine_host_tool(<target>)`, and
the check says so if it is missing. A host tool may not use intrinsics above the compiler's default
without its own dispatch — which no generator has any reason to.

**Must now be done.** Any executable run by `add_custom_command()` or `add_custom_target()` is
declared a host tool. Keep `build.cpu_baseline` registered in every Ninja preset.

**Forbidden now.** Calling `engine_host_tool()` on anything that ships, and calling
`engine_strip_cpu_baseline()` on anything but `core/platform` (ADR-0031's rule, now visible in a
test log). Refusing a configure because the build machine is below the baseline.

## Revisit when

- **A host tool becomes slow enough to matter** — a content-build step that runs for minutes during
  the build rather than milliseconds. Then it may want the build machine's best instruction set,
  which is a runtime dispatch inside the tool, still not the product's baseline.
- **A target architecture differs from the build machine's** — an ARM port, a console. A host tool
  compiled by the target toolchain cannot run on the host at all, and the answer is a host
  toolchain for the tools (a second configuration or `ExternalProject`), which this ADR deliberately
  did not build.
- **A generator other than Ninja becomes a supported preset.** The check reads Ninja's files and is
  not registered elsewhere.
