# Roles

`roles.json` is plan 06 §6.5's five named roles — **director**, **designer**, **environment**,
**QA** and **performance** — as configuration rather than code. A host reads it with
`engine-host --roles content/roles/roles.json` (engine-mcp passes it on with `--roles`), and every
writing call is then checked against the role it runs in. The format is the `RolesFile` schema type
in `schemas/protocol.schema`; `engine-cli schema.describe '{"type":"engine.protocol.Role"}'`
explains each field, and [protocol](../../docs/subsystems/protocol.md#roles-leases-and-proposals)
says what each check does and why.

What the five say, in short:

| Role | May call (writes; reads are never restricted) | Layers | Review |
|---|---|---|---|
| director | everything | every layer | no: it promotes and rejects |
| designer | `doc.apply`, undo and redo, `doc.propose_layer`, `doc.reject`, `lease.*` | every layer, through proposals | yes |
| environment | the same | every layer, through proposals, and only records that stand in a tile | yes |
| qa | nothing | none | — |
| performance | `content.build`, `tunables.set` | none | — |

**`default_role` is `qa`.** A call that names no role — every method that takes no attribution, and
one that leaves `attribution.role` blank on a host started without `--role` — runs as QA and so
changes nothing. That is the least a host told to restrict anything should grant a caller who did
not say who it is. A host started without a roles file restricts nothing at all.

**It is an example, meant to be copied and edited per game.** The layer patterns are `*` because
this repository has no game's layer names yet; a game would narrow the environment role to its
terrain and placement layers, give the designer its quest and encounter layers, and give a role
tile ranges when two teams split a map. Roles are guardrails for cooperating agents, not security:
the protocol has no authentication, and a call's role is whatever its attribution says.
