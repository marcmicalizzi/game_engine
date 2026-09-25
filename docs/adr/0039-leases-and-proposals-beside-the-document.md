# ADR-0039: Roles, leases and proposal layers are the protocol's, kept beside the document; a proposal merges each record against its first touch

- **Status:** Proposed
- **Date:** 2026-09-25
- **Plan references:** docs/plan/06-agent-tooling.md §6.5 (permissions, concurrency and leases), §6.10 (review and the director); docs/plan/03-data-model.md §3.2 (layer roles), §3.3 (structural three-way merge), §3.7 (the tile grid as the unit of leases); experiment E12 ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)).
- **Docs touched:** [protocol](../subsystems/protocol.md#roles-leases-and-proposals), [doc](../subsystems/doc.md), [apps](../subsystems/apps.md), [06 §6.5](../plan/06-agent-tooling.md#65-permissions-concurrency-and-leases), [10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), `content/roles/README.md`

## Context

Plan 06 §6.5 asks for three things and settles none of the questions building them raises: roles as configuration, leases on (layer, tile set) or (layer, object-type set) that fail at acquisition, and proposal layers promoted into a target by structural merge after validation. Four questions came up that a later contributor would otherwise have to rediscover the answers to.

1. **Where the checks live.** The writes to guard are `domain/protocol`'s own `doc.*` methods. engine-host is the only host today, but the editor will be another client of a host, and the tests drive a dispatcher in-process. Putting roles and leases in engine-host would leave every other dispatcher unguarded and would need a hook into the protocol's handlers to intercept its writes.
2. **Where the state lives.** A lease table and a proposal's properties could live in the host's memory. engine-cli starts a host per call, and engine-mcp replaces a host that misses a deadline, so a host's memory is the one place a lease granted a moment ago is guaranteed not to be.
3. **What a proposal merges against.** "Base = the target's state when the proposal was opened" is the obvious reading of a three-way merge. It means a copy of the whole target per open proposal, and it reports a conflict for every record another promotion changed after this proposal opened, even when this proposal's agent read that change and built on it.
4. **What happens to the proposal layer on promotion.** Removing it breaks the journal: every earlier patch names its layer, `Document::undo` fails on a layer that is gone, and the journal is one line per document, so one agent's promotion would end undo for everyone past it.

## Decision

1. **The checks are the protocol's.** `domain/protocol` has the role table (`policy.h`), the lease table (`leases.h`), the proposals' properties and merge bases (`proposals.h`), and the methods (`lease.*`, `doc.propose_layer`, `doc.promote`, `doc.reject`, `engine.roles`). The dispatcher gates methods by role; the session checks each write's records. A host loads a roles file and states its default identity (`engine-host --roles`, `--actor`, `--role`, `--task`) and does nothing else.
2. **The state is beside the document**, written before the answer the way the document's own files are: `leases.json`, `proposals.json`, and one `proposals/<layer>.base.jsonl` per open proposal. A lease's expiry is wall-clock milliseconds, so it means the same to every process on the machine. Two hosts writing one document at once is as unsupported as it was before; one host at a time, including one per call, now sees every lease and proposal an earlier one granted.
3. **A proposal merges each record against the target's record as the proposal first touched it.** The first commit that changes a record in a proposal appends the target's record (or its absence) to the base file. Promotion builds `base` as the target with those records put back, `theirs` as that base with the proposal's opinions composed on, and `ours` as the target now, and runs `doc::merge_layers`. A record the proposal holds with no recorded base is merged against the target as it stands. The cost is what the proposal touches, not what the target holds, and a change promoted into the target before the proposal first touched a record is never reported against it.
4. **Promotion is two journal patches and the proposal layer stays**: the target's patch, attributed to the promoter with the proposal's own rationale in it, then the proposal's emptying patch. The layer stays in the stack, empty and closed (`Promoted`), and its owner reopens it by name for the next piece of work, so the stack grows by one layer per agent rather than per proposal. `doc.undo` with `steps: 2` takes a promotion back and leaves the proposal holding its records; the proposal's state is not journaled, so it stays closed until reopened.

Beside those four, and written down in [protocol](../subsystems/protocol.md#roles-leases-and-proposals) rather than here because they are ordinary design rather than surprises: roles are guardrails for cooperating agents and not security (the protocol has no authentication); a method is a write unless registered `read_only`; leases are required per document and off by default; a record is placed by its composed position; a tile lease and a type lease that could both cover a record are refused at the edit, not at acquisition; and **a lease has to be held until the proposal that carries its edits is promoted**, which E12's first run found the hard way.

## Consequences

- Every dispatcher that registers the built-in methods enforces roles, ownership and leases alike: engine-host, a future editor host, and the tests. The price is that `domain/protocol` knows about tiles (through `domain/doc/partition.h`) and about proposal bookkeeping.
- engine-cli works with leases and proposals, one host per call. The price is three more files in a document directory, and a save per lease change. Nothing about those files is merged by git; they are coordination state, like the manifest's undo position.
- A promotion finds fewer, truer conflicts than a whole-target base would, and costs what the proposal touched. The price: a record's base is the target as of the first touch, so a change another promotion makes *between* a proposal's first and a later touch of the same record is a conflict even when the agent saw it; and the base file has to be kept with the proposal.
- The layer stack grows with the number of agents that ever proposed, not with proposals. Undo keeps working across promotions for every agent. The price: a closed proposal layer that an undo refilled is readable but not writable until reopened.

## Revisit when

- A host outlives its clients and serves several at once (plan 02's persistent host): the state could then live in the host, and leases could be tied to a connection rather than to a clock.
- Authentication exists: roles stop being self-declared, and the bridge's rule that a call cannot name another role moves into the host.
- E12 or real use shows the per-record first-touch base reporting conflicts agents had seen and built on — the case above — often enough to matter: record a base per touch rather than per first touch.
- Documents get large enough that a promotion's two validations of the whole accepted world dominate its cost (E12 measures it at its own document's size): validate only the records the promotion changed and their neighbours.
