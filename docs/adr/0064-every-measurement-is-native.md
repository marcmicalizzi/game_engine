# ADR-0064: Every measurement is native, and sampling over time is never the reason a cost is accepted

**Status:** Accepted

## Context

Dense alpha-tested foliage drawn at one sample a pixel differs from its own supersampled picture
by tens of display codes and flickers under any motion, and the frame rates the plan promises
below the top machine and on a surround assume an upscaler
([temporal sampling](../experiments/temporal-sampling.md), roadmap R153;
[04 §4.1](../plan/04-renderer.md)). So the renderer is going to sample over time: accumulation at
native resolution first, internal resolutions below the output's after it, and vendors' methods as
optional capabilities.

The owner's rule until this ADR was "performance first, no upscalers". Its meaning, in the owner's
words on 2026-11-14:

> no upscalers is more about not relying on them for performance as a crutch [...] upscalers are
> necessary, but the big rule on that is to never rule that not optimizing performance further is
> acceptable because we can just push the dlss more to performance mode. So the big caveat is that
> our performance measurements and performance observations from a walk are taken without
> upscalers, and that we're never thinking it's 'good enough' just because an upscaler takes it to
> an acceptable framerate.

On the design's nine decisions: "I agree with those decision[s], I don't have an amd machine so
FSR falls to last by that alone"; "DLAA is also an interesting one for me because so few games
actually implement it"; and that in other engines' games TAA has sometimes performed better than
DLSS, so the engine's own accumulation is measured against a vendor's wherever there is one.

A rule like this one erodes unless a tool holds it: a frame time read off a summary does not say
what drew the frame, and the first figure taken with reconstruction on would be compared with a
budget taken without it. [ADR-0014](0014-apache-2-license-and-dependency-policy.md) already keeps
proprietary SDKs out of the repository and the engine binary.

## Decision

1. **Every measurement is native.** Every frame budget, cost pair, benchmark and observation from
   a walk is taken with reconstruction off, at the output's own resolution. **A row's merge fact is
   never a figure with reconstruction on**, and **no cost is accepted because a reconstruction mode
   makes the frame rate acceptable**: a frame that misses its budget natively has missed it.
2. **Every record says what it was.** Every frame's statistics (`renderer::FrameStats`), every
   frame record and every summary carries a `reconstruction` block: the provider (`off`, `engine`,
   a vendor's name), the mode, whether the inputs were on, and the internal and the output sizes
   (`engine.scene.Reconstruction`). A session's summary line carries it too, so a walk's
   observation states it.
3. **A record that is not a native measurement has no cost in it.** With a provider other than
   `off`, or an internal size other than the output's, the times are written under
   `reconstructed.gpu_ms` and **the top-level `gpu_ms` is null**; a summary one of whose frames was
   not native is the same, and its markers' times are zero. A tool that reads a cost finds none.
   The rule is made where the record is made (`renderer::frame_record`,
   `renderer::summarize_frames`, `renderer::measurement_keys`), so both hosts' lines carry it.
4. **The tools refuse.** `tools/frame-budget.ps1` refuses such a summary before it reads a number
   of it, with an exit code of its own (5), and never re-baselines from one;
   `tools/flythrough.ps1` puts "refused" in the run's row in place of its numbers and fails. One
   function decides (`Get-ReconstructionRefusal`, `tools/lib/FrameBudget.psm1`), and it refuses on
   any one of the signs — the block, the `reconstructed` key, a null `gpu_ms` — so a writer that
   forgot one is still refused. Neither tool passes a flag that turns sampling over time on.
5. **A figure with reconstruction on is labelled and never alone**: it reads "reconstructed
   (provider, internal size -> output size)" and stands beside the native figure of the same flight
   on the same machine.
6. **The one exception is reconstruction's own cost**: a row that changes a provider or its inputs
   reports the pair on and off, natively sized, and its merge fact is that difference. The inputs
   alone (the sub-pixel offset and the motion pass, with no provider) are a native frame that costs
   the motion pass more: its record keeps `gpu_ms`, says `inputs: true`, and names the pass's time
   as `gpu_ms.motion`.
7. **The order** the owner agreed: the content faults the measurement found (a masked material's
   coverage, the far crowns' sub-pixel geometry) beside the inputs; then the engine's own
   accumulation at native resolution, which is the reference implementation, deterministic, on
   every device, and **off by default** until the owner turns it on after walking it; then a
   registration point for a reconstruction provider; then internal resolutions below the output's;
   then a vendor's provider as an optional capability whose SDK its consumer supplies (ADR-0014,
   ADR-0027), **DLSS super resolution and DLAA first**. **FSR is last** (the project has no AMD
   machine, and FSR's learned parts need hardware it has none of). **Frame generation is its own
   later decision**, and if it is ever built the rendered frame's time stays the frame's time and
   the presented rate is a second, labelled figure.
8. **Under a ratio the cut is the output's**: the LOD thresholds are held in output pixels, so
   reconstruction never coarsens geometry, and what it saves is shading.
9. **One history per signal**: a term that already remembers frames (the bounce's cache) is not
   given a second history, and no ray count is lowered on the grounds that accumulation hides its
   noise without a native pair that shows the picture holds.

## Consequences

- A summary's `gpu_ms` is now nullable (`engine.scene.FrameRecord` and `FlythroughSummary`,
  version 20). Every reader of one written before this ADR still reads it: no block and a `gpu_ms`
  is a native measurement. A reader that takes `gpu_ms` without asking gets a null from a
  reconstructed run, never a wrong number.
- The frame-budget gate has a fifth exit code. A gate that sees it has a flag in a budget run that
  does not belong there; nothing of that gate is a verdict.
- `render.capture`'s and `render.load`'s statistics (`engine.protocol.RenderStats`) do not carry
  the block yet. Nothing can turn a provider on through the protocol today; the row that adds one
  (roadmap R159) adds the block there in the same change.
- Tests keep comparing bytes: off is the default, and a frame with everything off is the frame it
  was.
- A vendor's quality on thin foliage is available on the hardware that vendor supports and nowhere
  else; the engine's own is the floor everywhere, and the only one a test can hold to bytes.

## Revisit when

A vendor's licence changes so that its SDK could be vendored; the engine gains a stochastic
real-time light path (ray reconstruction then has an input); a walk shows the engine's own
accumulation is not good enough on a machine no vendor's method runs on; or the owner decides the
accumulated picture is the default one, at which point what "native" means for a walk's
observation is written again here.
