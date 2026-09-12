# 12 — AI Usage Policy and Terms Compliance

This project uses AI in two places: development-time agents that design, build, and test (currently Claude via Claude Code), and an optional runtime model that adds life to a shipped game when the player's hardware has spare capacity. The two are kept strictly separate. The project owner's requirement is that everything here stays clearly inside the terms of every provider involved, with margin. This document is the rule set; it is conservative by design.

## 12.1 Sources of truth

Verify against the current text before any phase that touches AI:

- Anthropic Usage Policy: https://www.anthropic.com/legal/aup
- Anthropic Commercial Terms of Service: https://www.anthropic.com/legal/commercial-terms
- Anthropic Consumer Terms: https://www.anthropic.com/legal/consumer-terms
- The terms of any other inference provider a game uses, and the license of any model weights.

This document is engineering policy, not legal advice. When in doubt, ask the provider or counsel before proceeding.

## 12.2 Rules

1. **Development-time agent outputs are content and code, never training data.** Dialogue, quest logic, world data, rules tables, C++, shaders, and documentation produced with Claude are committed as ordinary project artifacts with provenance. No output of an Anthropic model is used to train, fine-tune, distill, evaluate-for-training, or assemble a dataset for any machine-learning model, including the runtime model. Anthropic's commercial terms prohibit using the service or its outputs to develop competing models; this rule is deliberately broader so the project never approaches that line.
2. **No fine-tuning of the runtime model in v1.** If a game enables a runtime model, it is a third-party open-weights model used as published. If fine-tuning is ever considered, the data must be human-authored or licensed for that purpose and must not include outputs of any provider whose terms forbid it. That decision requires an ADR and a fresh terms review.
3. **Model weights are dependencies with licenses.** Only weights whose license permits redistribution and commercial use are allowed. Community licenses with usage conditions are reviewed individually. The license and its obligations are recorded in the dependency manifest ([08 §8.8](08-toolchain.md#88-engine-license-and-dependency-policy)).
4. **Cloud inference at runtime is the game developer's account, not the engine's.** If a game calls a hosted API at runtime, the developer is the customer and must comply with that provider's usage policy, including any requirement to tell players they are interacting with an AI and any content restrictions. The engine provides the hooks (a disclosure UI element, the content filter, logging, rate limiting) and ships no credentials.
5. **All generated text passes a content filter** before it reaches a player, and is logged with `origin = LLM` so it is inspectable and replayable ([03 §3.5](03-data-model.md#35-persistent-world-state)).
6. **Provenance on everything generated**, at build time and at runtime ([07 §7.5](07-content-pipeline.md#75-provenance-and-licensing-metadata)).
7. **Terminology.** Describe the long-tail process as *authoring* and *caching*. Do not use "distillation" or "training" for anything in this project, because nothing in it trains a model.
8. **Player data.** Runtime inference input is limited to game state. No personal data leaves the machine without explicit consent, and the local tier never sends anything anywhere.

## 12.3 What is fine

Using Claude to write the engine, design the world, author dialogue, generate quests, review code, and drive playtests, then shipping the results in a game, is ordinary use of a coding and writing assistant. The line this project stays far from is turning model outputs into another model.

## 12.4 The runtime tiers under this policy

| Tier | What runs | Policy status |
|---|---|---|
| 0 | Deterministic and authored content only | Always required; the game is complete at this tier |
| 1–3 | A third-party open-weights model, local, unmodified, GPU if the VRAM budget has headroom after rendering, otherwise CPU on efficiency cores | Allowed under rules 2, 3, 5, 6, 8 |
| 4 | Hosted inference via the game developer's own provider account | Allowed under rule 4; the engine provides hooks only |

## 12.5 Review points

Re-read the provider terms and this document at the start of Phase 4 (agent API), Phase 5 (content generation), and Phase 7 (shipping), and before enabling any runtime tier above 0.
