# ADR-0072: The task ledger lives beside the document, is read and written under a lock on every call, and a person's review item closes only under a role marked human

- **Status:** Proposed
- **Date:** 2026-11-18
- **Plan references:** docs/plan/06-agent-tooling.md §6.8, §6.10; ADR-0033; ADR-0039
- **Docs touched:** `docs/subsystems/protocol.md` ("The task ledger and review items"), `docs/plan/06-agent-tooling.md` §6.8, `content/roles/README.md`

## Context

Plan 06 §6.8 asks for a task ledger agents resume from instead of their conversation, with review items in it, and put it in `work/tasks/*.json`, a file per task. Three questions had to be settled before v0 (roadmap A23) could be built.

**Where and in what form.** The protocol already keeps per-document state that is not the world beside the document: `leases.json`, `proposals.json`, `transactions.json`. Checkpoints leave those out, since they are work in progress and claims, not content (ADR-0067). A file per task would give each task its own diff, but nothing commits these files, and a call that claims a task has to check and change it and mint ids in one step that no other process can interleave.

**How two hosts share it.** `leases.json` and `transactions.json` are read when a session opens and written whole from the host's memory. A review of the transactions work (roadmap A11) showed that two engine-cli calls at once, each a host of its own, lose each other's writes and can mint one id twice. engine-cli's host per call is the advertised way to drive the protocol, and an agent's parallel tool calls are ordinary, so the ledger would be raced from its first day.

**Who closes a person's review item.** AGENTS.md says a check that needs a higher view policy becomes a human review item and never a configuration edit, and v0's own rule is that nothing may let an agent close one. How strong a role is depends on how the call arrived. Over a listener that accepts credentials (roadmap A4: credentials on the listener, decision 3a of its ADR), a connection that presented a credential runs only in that credential's role, whatever its calls claim. Over stdio, and through engine-cli's host per call, a call runs in the role the operator started the host in, or in the one its attribution names. And the director, the obvious closer, is also a role agents run in (§6.10's director agents, E12).

## Decision

1. **One file, `ledger.json`, beside the document** (schema `LedgerFile`), holding every task and review item and the next ids. It is not in a checkpoint. It holds names and ids, never file paths.
2. **No host caches it.** Every ledger call, reads included, opens `ledger.json.lock`, takes the operating system's exclusive lock on that handle (`LockFileEx`, `flock`), reads the file, changes it, writes it back atomically and lets go (`detail::run_exclusive`, `domain/protocol/src/exclusive_file.h`). Nothing breaks a stale lock, because the system releases it when its holder dies. A call that waits for the lock more than 10 s answers 1006. The other side files are to move onto the same helper once roadmap A11's mend of the same fault for those two files is in.
3. **A claim is an actor's, lapses by time and is not tied to a connection**: a task outlives hosts and reconnections. Every update by the claimant renews it.
4. **`Role.human`** (Role version 2) is the operator's statement that calls in a role are a person's. A review item whose audience is `Human` is closed (Accepted or Rejected) only by a call in such a role, and nobody withdraws it, not even its raiser. **Corrected the same day, after review: a role marked human is conferred only by a credential the operator issued for it, presented on a connection** (ADR-0069), and never by a call's attribution, a connection's `client.hello` or a host's start line, which are all claims any process types: engine-cli forwarded `--role` to the host it started, so `engine-cli --role reviewer review.close` closed a person's item. A call with no credential that names a human role is refused (1008, `human review`), `engine-host` and `engine-cli` refuse `--role <a human role>` at start (exit 2; `engine-host --issue-credential --role reviewer` is how the operator confers it), and a roles file whose `default_role` is human is refused. How a caller is known to be a person is one field, `Caller::human_via` (the credential's name), set only in `resolve_caller`; `Role::human` is read nowhere else (the lint's `human-flag` rule). Until the editor's seat is built, a person's item is closed only over a listener with such a credential, and against a process of the same user that can read the credential's file this is a rule and an audit trail, not a wall. A host with no human role, a host with no roles file included, has nobody who can close one. The shipped roles file marks one role, `reviewer`, and no role an agent runs in.

## Consequences

- Two hosts on one directory, any number of engine-cli processes among them, lose no ledger write and never share an id (`ledger_tests.cpp`, the engine-cli eight-process case). A ledger call costs one read and one write of the whole file, so a document that keeps every task it ever had pays for its history. Archiving is not built.
- A host with no roles file restricts nothing else but cannot close a person's item. That is deliberate: the alternative is a default under which any caller is a person.
- `human` is as strong as the credential: a connection bound to a credential whose role is not marked human cannot close a person's item, whatever role its calls name, and nothing else can. Over stdio and engine-cli nobody closes one. Nothing in the protocol stops an operator from issuing an agent a credential in a human role; the rule is that they must not (`content/roles/README.md`). engine-mcp starts its host with its own `--role`, which that host refuses, and forwards a call's attribution, which its host refuses too.
- Held together in one tree by `ledger_tests.cpp` ("roles, and a review item for a person that no agent closes": a named `reviewer` refused, a connection bound to a `reviewer` credential closing) and `apps/engine_cli/tests/policy_tests.cpp` ("no start line runs in a role marked human").
- The plan's `work/tasks/*.json` is not built. §6.8's status note says so.

## Revisit when

- ~~stdio or engine-cli calls need a person's authority~~ Closed the same day: no start line confers a person's role; a person at a command line connects with a credential issued for it. The editor's seat, a second way for a person to be known, is a later row.
- A ledger grows past what one read and write per call is worth (thousands of tasks with long histories): archive closed work into a second file, or split per task under the same lock.
- The side-file helper is shared with `leases.json` and `transactions.json`: this ADR's lock is then theirs too, and the note in decision 2 is closed.
- The test holding a credential's role to this rule over a listener exists: the consequence naming it as a follow-up is then closed.
