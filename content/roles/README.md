# Roles

`roles.json` is plan 06 §6.5's five named roles — **director**, **designer**, **environment**,
**QA** and **performance** — and §6.10's **reviewer**, a person, as configuration rather than code. A host reads it with
`engine-host --roles content/roles/roles.json` (engine-mcp passes it on with `--roles`), and every
writing call is then checked against the role it runs in. The format is the `RolesFile` schema type
in `schemas/protocol.schema`; `engine-cli schema.describe '{"type":"engine.protocol.Role"}'`
explains each field, and [protocol](../../docs/subsystems/protocol.md#roles-leases-and-proposals)
says what each check does and why.

What the six say, in short:

| Role | May call (writes; reads are never restricted) | Layers | Review |
|---|---|---|---|
| director | everything, `doc.checkpoint`, `doc.rollback_to` and `doc.collect_blobs` among it | every layer | no: it promotes and rejects |
| designer | `doc.apply` and a transaction's `doc.begin`, `doc.commit` and `doc.rollback`, undo and redo, `doc.propose_layer`, `doc.reject`, `lease.*`, `doc.put_blob`, the ledger's `task.*` and `review.raise` | every layer, through proposals | yes |
| environment | the same | every layer, through proposals, and only records that stand in a tile | yes |
| qa | nothing | none | — |
| performance | `content.build`, `tunables.set` | none | — |
| reviewer (`human`) | `review.*`, `task.create`, `task.update` | none | — |

**`default_role` is `qa`.** A call that names no role — every method that takes no attribution, and
one that leaves `attribution.role` blank on a host started without `--role` — runs as QA and so
changes nothing. That is the least a host told to restrict anything should grant a caller who did
not say who it is. A host started without a roles file restricts nothing at all.

**Checkpoints, rollbacks and blob collection are the director's.** A rollback rewrites every
accepted layer it differs in, so a role that works through proposals could not make one anyway (it
is refused whole, like any direct write); a checkpoint changes no document but is the history every
other agent rolls back to, and a collection deletes bytes. The designer and the environment may put
blobs, since painting a mask or a heightfield into a proposal names one
([protocol](../../docs/subsystems/protocol.md#blobs-and-checkpoints)).

**It is an example, meant to be copied and edited per game.** The layer patterns are `*` because
this repository has no game's layer names yet; a game would narrow the environment role to its
terrain and placement layers, give the designer its quest and encounter layers, and give a role
tile ranges when two teams split a map. Roles are guardrails for cooperating agents, not security:
the protocol has no authentication, and a call's role is whatever its attribution says.

**The reviewer is the one role marked `human`, and only a call in a human role closes a review item raised for a person** ([protocol](../../docs/subsystems/protocol.md#the-task-ledger-and-review-items), ADR-0072): a check above a session's view policy, and anything else a check hands to a person, waits for that call; the director, which agents may run as, cannot close one. **The role is conferred only by a credential the operator issues for it** (`engine-host --roles roles.json --issue-credential <file> --name <who> --actor <who> --role reviewer`, ADR-0069) and the person presents on a connection to `engine-host --listen --credentials <file>`: a call that names `reviewer` without one is refused (1008, `human review`), and `engine-host`, `engine-cli` and engine-mcp's host refuse `--role reviewer` at start (exit 2). Never issue an agent such a credential. Until the editor's seat is built, that listener is the only way a person closes an item, and against another process of the same user that can read the credential's file it is a rule and an audit trail, not a wall. QA files nothing by default, since every call that names no role runs as QA; an operator who wants a reviewing agent gives a role `review.raise`, as the designer and the environment have.