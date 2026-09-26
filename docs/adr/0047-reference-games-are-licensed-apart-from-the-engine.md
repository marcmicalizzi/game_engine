# ADR-0047: The reference games are licensed apart from the engine — Desert Survival's code under GPL-3.0, its assets non-commercial, its title a trademark, in its own repository

- **Status:** Proposed (records the owner's decision of 2026-09-26; becomes Accepted when the owner has read it)
- **Date:** 2026-09-26
- **Plan references:** docs/plan/13-reference-consumer-games.md §13.1 and §13.2 (the two reference games and what each qualifies), §13.5 (Desert Survival as the recommended first shippable), docs/plan/12-ai-usage-policy.md (generated content is content), docs/content-generation.md (provenance sidecars). Builds on [ADR-0014](0014-apache-2-license-and-dependency-policy.md) (the engine is Apache-2.0 and its dependencies permissive) and [ADR-0022](0022-consumer-game-order.md) (Desert Survival first).
- **Docs touched:** [13 §13.1, §13.2, §13.8](../plan/13-reference-consumer-games.md), the root [README](../../README.md) (license section), [content-generation](../content-generation.md) (what a sidecar records).

## Context

The engine is Apache-2.0 ([ADR-0014](0014-apache-2-license-and-dependency-policy.md)) and is not meant to be a source of income; the owner's time and resources go into it regardless. Its two reference consumers ([13](../plan/13-reference-consumer-games.md)) exist to give the engine's capabilities concrete acceptance cases — and one of them, Desert Survival, is a compelling game in its own right: a unique and niche concept that, built to the quality the engine aims at, could earn a not-meaningless amount. The owner intends to publish it on Steam and other platforms for a nominal price while keeping its source open as the reference implementation of a game on this engine: anyone may read it to learn how the engine is used, and anyone may build it and play it themselves. What must not be easy is a **low-effort clone**: taking the game, renaming it and selling it.

No open source license can forbid selling; every OSI-approved license permits commercial use by definition, and a license that forbids it is source-available, not open source. So the protection has to come from what a clone would need and cannot have, not from a clause against selling code.

Island City has no compelling gameplay concept yet. It is an engine demo — the density benchmark environment and the candidate setting for a narrative game ([13 §13.5](../plan/13-reference-consumer-games.md#135-mapping-to-the-roadmap)) — and stays under the engine's terms unless that changes.

The alternatives on the table for Desert Survival:

1. **Everything Apache-2.0 like the engine.** Rejected: it invites the clone the owner wants to prevent, and there is no attribution requirement strong enough to matter on a storefront.
2. **A non-commercial code license** (PolyForm Noncommercial 1.0.0; Creative Commons NC licenses are not written for software and their author says not to use them for it). It forbids the sale outright, allows reading and personal use, and is the right instrument if the prohibition must be in the code license. Rejected as the default because it is not open source, which deters the contributors and packagers a reference implementation wants, and because the practical protection below is stronger than a clause a cloner would ignore.
3. **Dual licensing** (copyleft plus a commercial license sold by the owner). Rejected for now: it needs a contributor license agreement to keep the right to relicense, which is friction for exactly the contributions a reference game should welcome; it can be added later if a studio asks for a closed derivative.
4. **The Functional Source License** (non-compete now, Apache after two years). Written for hosted software; reads oddly on a game and is little known. Rejected.
5. **Copyleft code, non-free assets, a trademarked title.** Chosen.

## Decision

1. **Desert Survival's code is GPL-3.0-or-later.** Anyone may read it, build it, play it and redistribute it with source; a derivative that ships must be GPL with source offered, which removes the incentive to fork and resell without removing the right to learn from it. The engine's Apache-2.0 code is included in a GPL-3.0 program without friction (one-way compatible), and anything that belongs in the engine is contributed back to the engine under Apache-2.0, never the other way.
2. **Desert Survival's assets are not free.** Art, audio, kits, scenes, generated meshes and images, recorded input and any content that is not code are licensed CC BY-NC-SA 4.0, or all rights reserved with an explicit grant of personal use and study, per asset as its provenance sidecar records. A game without its assets is not the game; this is the clause that stops the low-effort clone. The store build is the assets plus the convenience of a built, updated, achievement-bearing copy at a nominal price — the model id Software set for Doom and Quake and that open-code, paid-build games use on Steam today.
3. **The title is a trademark.** Anyone can ship a fork; nobody can ship it under the game's name or marks. Registration is the owner's, before the store page.
4. **The game lives in its own repository**, depending on the engine as a pinned dependency, so the license boundary is a repository boundary and the engine tree stays uniformly Apache-2.0. The engine repository's `game/` directory holds only small Apache-2.0 samples. The same applies to Island City if it ever becomes a game; as a demo it may live in the engine tree or in a repository of its own under the engine's terms, whichever keeps the engine's build simple.
5. **Every shipped asset's grant is recorded.** The generated assets come from services whose paid tiers grant commercial use; the grant, the service, the tier and the date are recorded in the asset's provenance sidecar ([content-generation](../content-generation.md)), and the CC0 and CC-BY samples ship with their attributions, so the storefront build can prove every asset's right to be sold.
6. **Nothing here changes the engine.** [ADR-0014](0014-apache-2-license-and-dependency-policy.md) stands: the engine and everything under `third_party/` stay permissive; a game's copyleft never reaches into the engine repository.

This is a recording of the owner's decision, not legal advice; the trademark, the asset services' terms and the exact license texts are checked by counsel before the store page.

## Consequences

- A reference implementation that is fully readable and buildable, and a game that is pointless to clone rather than merely forbidden to.
- Two repositories to keep in step: the game pins an engine commit and moves it deliberately; an engine change that breaks the game is found by the game's CI, not by the engine's.
- Contributors to the game contribute under GPL-3.0; contributors to the engine under Apache-2.0; a change that touches both is two pull requests.
- The provenance sidecars gain fields (license, service, tier, date of the grant), and the content pipeline refuses to ship an asset without them.
- If Island City acquires a gameplay concept worth selling, it takes the same route by the same record; until then it is a demo and says so.

## Revisit when

- A studio asks for a closed commercial derivative of Desert Survival: dual licensing, with a contributor license agreement from that point on.
- The storefront or a platform's terms conflict with GPL-3.0 distribution of the same build (the usual answer is that the store sells the assets and the convenience, and the source stays public; if a platform forbids it outright, that platform's build is discussed then).
- A clone appears regardless: the trademark and the asset license are the instruments; the code license is not, and was never meant to be.
