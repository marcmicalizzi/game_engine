# E12: proposal layers and leases under simulated agents

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)):** structural three-way merge of proposal layers — what is the conflict rate with tile leases, under simulated multi-agent edits? It decides the concurrency model of [06 §6.5](../plan/06-agent-tooling.md#65-permissions-concurrency-and-leases): whether agents coordinate by leases that fail at acquisition, by merges that report conflicts at promotion, or both.
- **Date:** 2026-09-25. **Machine:** Intel Core i9-10980XE, 18 cores / 36 threads, 64 GB, Windows 11, NVMe. No GPU involved. **Build:** `msvc-release`, commit `21d9081` plus the driver.
- **Machine state:** WARNING raised. At the start the GPU lock was held by another agent's `msvc-debug` full suite, other processes used 7.5% of the CPU and the GPU was 10% busy; at the end the machine was quiet (others 1.3%, the lock free). In between, this change's own `msvc-debug` and `msvc-minimal` builds, a Linux container build and a full `msvc-debug` test suite ran beside it, which the harness's two samples do not see. **Every count below is unaffected** — the turn order is seeded and no lease expires within a run, so a rerun gives the same numbers — and **every millisecond is an upper bound**.
- **Decision:** leases on tiles are the concurrency model and the structural merge is the safety net behind them; a lease is held until the proposal carrying its edits is promoted. Written into [06 §6.5](../plan/06-agent-tooling.md#65-permissions-concurrency-and-leases)'s status note and [protocol](../subsystems/protocol.md#roles-leases-and-proposals); the four surprising design choices under it are [ADR-0039](../adr/0039-leases-and-proposals-beside-the-document.md), **Proposed**.

## Setup

The mechanism measured is the one the engine ships: roles, leases and proposal layers in `domain/protocol` ([protocol](../subsystems/protocol.md#roles-leases-and-proposals)), driven over the dispatcher — JSON-RPC requests in, JSON out, every commit saved to disk before its answer, exactly as engine-host serves them. The driver is `domain/protocol/tests/e12_tests.cpp`, a skipped doctest case in the protocol test binary; a small configuration of it runs on every build and holds the invariants this page's conclusions rest on.

**The world.** One document with a `world` layer (a feature layer, tiled 10 m a side), 8 × 8 tiles, six props (`engine.protocol.test.Prop`: a name, a position, a colour, a scale) in each at the start: 384 records.

**The agents.** N = 4, 8 or 16 agents share the document through one dispatcher, as agents sharing one host would. Each owns a block of tiles (2 × 2 blocks of 4 × 4 tiles for four agents, 4 × 2 blocks of 2 × 4 for eight, 4 × 4 blocks of 2 × 2 for sixteen) and keeps a proposal layer over `world` (`doc.propose_layer`), which it reopens after each promotion. It makes 40 edits in two cycles of 20; after each cycle a director promotes its proposal (`doc.promote`). An edit lands in the agent's own block, or — with probability `leak` of 0%, 10% or 30% — in a tile of the ring around the block, which belongs to a neighbour. The kinds are 25% create (a third of them under a parent in the same tile), 45% set a property (colour, name, scale, or a new position inside the same tile), 15% reparent (to another prop in the tile, or to the root) and 15% delete. An agent reads the world (`doc.objects`, composed — neighbours' open proposals included) when it starts a cycle, reads a neighbour's tile again just before editing in it, and does not delete a prop that has children. Rounds: each agent in a shuffled order takes one turn — at most one edit, or its promotion. Three seeds per configuration.

**The three modes.**

| Mode | What an agent does |
|---|---|
| `none` | No leases. Every edit is applied; conflicts surface at promotion. |
| `give_up` | The document requires leases (`lease.require`). An agent leases its block at the start of a cycle; a leak edit asks for a one-tile lease, and when the neighbour holds it the edit is dropped. |
| `retry` | The same, but a refused edit is kept and asked for again each turn; the wait is counted in rounds. An agent that has made all its new edits hands its block back and works off what it owes one edit at a time, each in a proposal promoted at once. |

In both lease modes **every lease is held until the proposal carrying its edits is promoted**, then released — see [what surprised me](#what-surprised-me) for why that is not optional.

**Settling a refused promotion.** A promotion refused for conflicts or for new validation problems counts as needing a human. The driver then settles it the way a director most often would: the target's side of a conflict wins (`prefer: "Target"`), and a record a new validation problem names is taken out of the proposal (or, when the problem is the target's record — a child whose parent the proposal deleted — the proposal's opinion about the parent is), then promotion is tried again. Every edit that loses counts as lost: a conflict's losing side, and every record taken out.

**Reproduce:**

```powershell
pwsh tools/dev.ps1 build -Preset msvc-release
$env:ENGINE_E12_OUT = "e12.jsonl"    # one JSON line per configuration and seed, then the machine state
$env:ENGINE_E12_SEEDS = "3"
build/msvc-release/domain/protocol/engine_protocol_tests.exe -ns -tc="E12: the matrix"
```

It prints a Markdown table as it goes; the tables below are aggregated from the JSON lines.

## Results

Sums over three seeds; 1h24m for all 81 runs. "Refused at apply" is a `doc.apply` that committed nothing because a strict precondition failed: `stale`, an object or parent a neighbour's open proposal had deleted, or `cycle`, a reparent that would close a cycle (the agent's own). "Needing a human" is a promotion refused on its first try; "lost" counts every edit that lost when it was settled. "Rounds" is how long the whole run took, one turn per agent a round; 129 is the floor (43 a seed).

**Without leases** — conflicts surface at promotion:

| Agents | Leak | Edits | Applied | Refused at apply (stale / cycle) | Conflicts at merge (property / deleted-modified / created twice / cycle) | Validation refusals (problems) | Promotions needing a human | Edits lost |
|---|---|---|---|---|---|---|---|---|
| 4 | 0% | 480 | 480 | 0 | 0 | 0 | 0 of 24 | 0 |
| 4 | 10% | 480 | 479 | 1 (1 / 0) | 1 (0 / 1 / 0 / 0) | 1 (1) | 2 of 24 | 2 |
| 4 | 30% | 480 | 476 | 4 (4 / 0) | 5 (0 / 2 / 3 / 0) | 2 (3) | 6 of 24 | 8 |
| 8 | 0% | 960 | 958 | 2 (0 / 2) | 0 | 0 | 0 of 48 | 0 |
| 8 | 10% | 960 | 957 | 3 (1 / 2) | 7 (1 / 5 / 1 / 0) | 5 (5) | 10 of 48 | 12 |
| 8 | 30% | 960 | 952 | 8 (6 / 2) | 16 (2 / 11 / 3 / 0) | 7 (7) | 18 of 48 | 23 |
| 16 | 0% | 1,920 | 1,915 | 5 (0 / 5) | 0 | 0 | 0 of 96 | 0 |
| 16 | 10% | 1,920 | 1,911 | 9 (6 / 3) | 31 (12 / 12 / 7 / 0) | 11 (14) | 37 of 96 | 45 |
| 16 | 30% | 1,920 | 1,897 | 23 (18 / 5) | 51 (16 / 22 / 12 / 1) | 19 (21) | 47 of 96 | 72 |

**With leases** — conflicts surface at acquisition. Both modes had **zero conflicts at merge, zero validation refusals and zero promotions needing a human in all 54 runs**, so those columns are left out:

| Agents | Leak | Mode | Edits | Applied | Refused at apply (all cycles) | Lease refusals | Dropped | Kept, then made | Wait (rounds, mean / max) | Block waits | Promotions | Rounds |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 | 10% | give_up | 480 | 433 | 0 | 47 | 47 | — | — | 0 | 24 | 129 |
| 4 | 10% | retry | 480 | 480 | 0 | 264 | 0 | 34 | 20.1 / 45 | 30 | 46 | 169 |
| 4 | 30% | give_up | 480 | 365 | 0 | 115 | 115 | — | — | 21 | 24 | 150 |
| 4 | 30% | retry | 480 | 478 | 2 | 506 | 0 | 78 | 27.7 / 64 | 179 | 45 | 256 |
| 8 | 10% | give_up | 960 | 861 | 1 | 98 | 98 | — | — | 21 | 48 | 150 |
| 8 | 10% | retry | 960 | 959 | 1 | 629 | 0 | 79 | 23.7 / 61 | 222 | 78 | 239 |
| 8 | 30% | give_up | 960 | 734 | 1 | 225 | 225 | — | — | 105 | 48 | 213 |
| 8 | 30% | retry | 960 | 958 | 2 | 1,252 | 0 | 180 | 36.4 / 79 | 278 | 168 | 281 |
| 16 | 10% | give_up | 1,920 | 1,739 | 7 | 174 | 174 | — | — | 63 | 96 | 171 |
| 16 | 10% | retry | 1,920 | 1,916 | 4 | 1,324 | 0 | 141 | 23.5 / 63 | 426 | 167 | 250 |
| 16 | 30% | give_up | 1,920 | 1,460 | 5 | 455 | 455 | — | — | 248 | 96 | 212 |
| 16 | 30% | retry | 1,920 | 1,912 | 8 | 2,592 | 0 | 400 | 36.9 / 84 | 754 | 336 | 323 |

At 0% leak the three modes are the same run: every edit lands in the agent's own block, nothing conflicts and nothing is refused but the agents' own reparent cycles (0, 2 and 5 at 4, 8 and 16 agents).

**The rates side by side.** Conflicts per 100 applied edits without leases, and edits dropped per 100 with `give_up`:

| Agents | 10% leak: conflicts at merge | 10%: promotions needing a human | 10%: dropped at acquisition | 30%: conflicts at merge | 30%: promotions needing a human | 30%: dropped at acquisition |
|---|---|---|---|---|---|---|
| 4 | 0.2 | 8% | 9.8 | 1.1 | 25% | 24.0 |
| 8 | 0.7 | 21% | 10.2 | 1.7 | 38% | 23.4 |
| 16 | 1.6 | 39% | 9.1 | 2.7 | 49% | 23.7 |

**What a promotion and an edit cost** (`msvc-release`; the median over seeds of each run's median; milliseconds; upper bounds, see the machine state):

| Agents | `doc.apply` (one edit) | `doc.promote` | of which the three-way merge | the two validations | the two commits and their saves |
|---|---|---|---|---|---|
| 4 | 86.8 | 400.6 | 1.15 | 1.20 | 397.6 |
| 8 | 111.7 | 374.4 | 1.21 | 1.29 | 371.3 |
| 16 | 158.1 | 401.7 | 1.39 | 1.45 | 398.2 |

The target held 384 records at the start and up to about 600 at the end; the accepted world validated twice per promotion is the same size.

**The same matrix after the store was fixed** (2026-09-25, [doc](../subsystems/doc.md#saving), [ADR-0041](../adr/0041-a-save-is-a-log-then-files-written-in-place.md), proposed: a save writes only what the commit changed, as one log and then each file in place, and the journal and the side files skip `Vfs`'s question about every parent directory). `msvc-release`, the same machine, three seeds, the median over all 27 runs of each agent count of each run's median; machine state: other processes used 27% of the CPU at the start and 13% at the end, the GPU lock free, so upper bounds:

| Agents | `doc.apply` (one edit) | `doc.promote` | of which the three-way merge | the two validations | the two commits and their saves |
|---|---|---|---|---|---|
| 4 | 4.27 | 24.1 | 1.18 | 1.18 | 21.0 |
| 8 | 4.35 | 21.7 | 1.18 | 1.27 | 18.9 |
| 16 | 4.32 | 19.2 | 1.33 | 1.40 | 15.9 |

An edit no longer grows with the number of agents, because a save no longer visits their layers; a promotion is two commits of a few files each. **Every count in the tables above is the same** — the turn order is seeded — and the 81 runs took 4.3 minutes, where they took 1 h 24 min.

## What surprised me

**A lease released after the edit protects nothing.** The first run of the matrix had the lease-mode agents take a one-tile lease for an edit next door, make the edit, and release it — and found a `DeletedAndModified` conflict at merge *with leases required*. The edit sat unmerged in the agent's proposal; the neighbour took its block back for its next cycle, deleted the record, and promoted first. A lease has to cover the proposal that carries its edits until that proposal is promoted, and then it can go. The protocol does not tie the two together — [protocol](../subsystems/protocol.md#leases) says why and states the rule, and the MCP bridge's tools and hints teach it — so this is the one thing an agent can get wrong that turns leases back into merges. Every lease-mode number below is from agents that hold their leases until the promotion.

**Waiting while holding is a deadlock.** The same first run had a retrying agent that had made all its new edits keep its own block while it waited for a neighbour's tile. Two such agents owing each other waited until the driver's cap. The fix is the obvious one — hand the block back (promote what you have) before waiting, and work off what you owe one edit at a time — but a lease system that lets an agent hold one claim while asking for another will deadlock the first time two agents do it, and nothing in the protocol prevents it. It is the agent's discipline, like lock ordering.

**Validation catches what the structural merge cannot see.** With no leakage at all, the first run still needed a person for one promotion: an agent had deleted a prop that one of its own earlier edits had given a child, and the promoted world would have held an orphan. That is not a merge conflict — nothing disagrees — and the merge does not see it; `doc.promote`'s validation of the merged world does, and refused. The agents now read before they delete (as a model with `objects` would), and the zero-leak rows need nobody. Without leases, the same check catches the cross-agent version: one agent deletes a prop another agent is placing a child under.

**Without leases, most conflicts are structural, not two agents setting one property.** Of the 111 conflicts in the no-lease runs, 31 were two agents setting one property differently; 53 were a deletion against a modification, 26 were an agent editing a prop a neighbour had *created* and not yet promoted — its override meets the neighbour's full definition in the target as "created twice, differently" — and 1 was a pair of reparents that together closed a cycle. Beside those, 45 promotions found problems in the merged world that no conflict names, and validation refused them: a child left without its parent, or an override of a prop whose creator's proposal was still open. The plan's picture of a merge conflict (03 §3.3: "the same (object, property) differs on both sides") is the minority case; the common ones are about whether an object exists, which is exactly what an agent looking at its own proposal cannot see.

**Composed proposals surface some collisions at the edit.** Proposals compose into the document everyone reads, so a neighbour's in-progress deletion hides the prop from every other agent's strict `doc.apply` at once: the edit is refused (`refused at apply`, `stale` in the JSON) instead of merging into a conflict later. That is the fail-fast the plan wants, arrived at without leases — but only for deletions, and only when the deletion comes first.

**The merge is cheap and the commit is not.** The three-way merge of a proposal into a 384–600-record target took 1.2–1.4 ms, and validating the accepted world twice about as much; a promotion took about 400 ms, 99% of it the two commits' saves. A single edit took 87 ms with 4 agents and 158 ms with 16, and the difference is the number of layers: `DocumentStore::save` visits every layer's files on every commit, and every agent adds a partitioned proposal layer. None of it is the merge, and most of it is this Windows file system — the protocol test binary, E12's small run included, takes 0.84 s in the Linux container and 22 s in `msvc-debug` here. A document shared by many agents wants a save that visits only the layers a commit touched; that is a store change, not a merge change. **Made the same day** ([doc](../subsystems/doc.md#saving), [ADR-0041](../adr/0041-a-save-is-a-log-then-files-written-in-place.md)): a save now writes only what the commit changed, and each file once, in place, behind one log that makes the save all or nothing. The small run went from 14.5 s to 2.1 s in `msvc-debug` (13.3 s to 1.6 s in `msvc-release`), the protocol test binary from 20.5 s to 3.6 s, and the edit and promotion to the table under [Results](#results). What it cost on this file system was the kind of operation, not the bytes: `create_directories` on a directory that exists (4.3 ms, MSVC creates every prefix), a rename over a file (2.1 ms), a file's length changed in place (4.8 ms from 16 KiB), where writing over a file at its own length is 0.5 ms.

## What it decides

**Tile leases are the concurrency model, and the structural merge is the safety net behind them.** With leases held until promotion, not one of the 54 lease runs produced a conflict at merge, a validation refusal or a promotion that needed a person, from 4 agents to 16 and at up to 30% of edits next door. Without leases, the conflict rate is low per edit (0.2 to 2.7 per 100) but it lands on promotions, where it costs a person: 8% to 49% of promotions needed one, and the rate grows with the number of agents at a fixed leak (smaller blocks put more of every block on someone's border). A document edited by more than one agent at a time should require leases (`lease.require`); a single agent's document should not, and does not by default.

**The refusal rate at acquisition is the leak rate, and what it costs is the mode's choice.** `give_up` dropped 9–10% of all edits at 10% leak and 23–24% at 30% — nearly every edit aimed next door at 10%, and four in five at 30%, the rest landing while the neighbour was between cycles — and never needed anyone. `retry` lost nothing, and paid in time: the kept edits waited 20 to 37 rounds on average (one to two of the neighbour's 20-edit cycles, which is how long the neighbour holds its block), the longest 84, and whole runs took 31% to 150% longer; it also made two to three and a half times the promotions, because an agent works off what it owes in proposals of their own. The protocol supports both; an agent that owes an edit to a neighbour's tile should keep it and ask again (`retry`), because an edit an agent decided to make and then dropped is work nobody will do.

**Two rules an agent has to keep**, which the protocol states and the MCP bridge teaches but does not enforce: hold a lease until the proposal carrying its edits is promoted; and do not wait for one claim while holding another.

**It does not decide** how often real agents edit across a border (the leak is a knob), whether object-type leases are worth their overlap rule (unit-tested, not simulated), what a cross-tile move costs, or how many agents one host serves — agents share one dispatcher here, which engine-mcp's one-host-per-agent shape cannot yet do (plan 02's persistent host). Nor does it measure promotion at a scale where validating the whole accepted world twice would matter: at a few hundred records it is noise beside the saves.

## Caveats

- **The agents are simulated, and uniform.** Real agents do not pick props at random, do not edit a neighbour's tile with a fixed probability, and read the world more often than twice a cycle. The leak fraction is the knob that stands in for all of that; a real session would show where on it agents actually sit. What carries over is the mechanism: every rate here is a function of how often two agents touch one record, and leases take that to zero by construction.
- **One world, one grid, no moves across tiles.** A move to another tile needs a lease on both, and an agent that owns neither end is a case this run never made. Object-type leases (and their overlap with tile leases, refused at the edit) are unit-tested in `policy_tests.cpp` but not in the matrix.
- **One process.** The agents share one dispatcher, as agents sharing one host would. engine-mcp today starts a host per agent, and two hosts writing one document at once is not supported ([ADR-0039](../adr/0039-leases-and-proposals-beside-the-document.md)) — so a real multi-agent session needs the persistent host of plan 02 before it can run this way outside a test.
- **The timings are this Windows box's file system, on a busy machine.** Every commit wrote and renamed several files, and the same protocol tests ran about forty times faster in the Linux container. After the store's fix the edit is 4.3 ms and the promotion about 20 ms here; the file system still sets them. The counts do not depend on timing: the turn order is seeded and no lease expires within a run.
- **The merge base is per first touch** ([ADR-0039](../adr/0039-leases-and-proposals-beside-the-document.md)). A whole-target base taken at `propose_layer` would have reported more conflicts without leases — every record another promotion changed after this proposal opened — and the same zero with them.
