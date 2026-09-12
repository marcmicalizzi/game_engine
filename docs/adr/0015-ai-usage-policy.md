# ADR-0015: AI usage policy: agent outputs are content and code, never training data

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/12-ai-usage-policy.md, docs/plan/07-content-pipeline.md §7.8

## Context

The project uses development-time agents (Claude) to design, build, and test, and may optionally ship a small runtime model to add life to a game. The owner requires that every use of AI stays clearly inside provider terms, with margin, and that the vocabulary never suggests otherwise.

## Decision

Development-time agent outputs (code, dialogue, world data, rules, documentation) are committed as ordinary project artifacts with provenance and are never used to train, fine-tune, distill, or build datasets for any model. Any runtime model is a third-party open-weights model used as published, under a license permitting redistribution and commercial use, running on GPU only when the VRAM budget has headroom and otherwise on efficiency cores; the game is complete at Tier 0 without it. Runtime cloud inference, if a game uses it, runs on the game developer's own provider account with the engine supplying only hooks (disclosure UI, content filter, logging). All generated text passes a content filter and is logged as an event. The recurring-case process is described as authoring and caching, never as distillation or training. Provider terms are re-read at the start of Phases 4, 5, and 7 and before enabling any runtime tier above 0.

## Consequences

Long-tail authoring produces deterministic content, not model updates. No fine-tuning exists in v1; introducing it requires a new ADR and a terms review.

## Revisit when

Provider terms change, or a fine-tuning proposal is made.
