# ADR-0069: A connection is who its credential says, and a credential is never in a URL

- **Status:** Proposed
- **Date:** 2026-11-18
- **Plan references:** docs/plan/06-agent-tooling.md §6.5; roadmap A4 (on A3)
- **Docs touched:** `docs/subsystems/protocol.md` ([Who a connection is](../subsystems/protocol.md#who-a-connection-is)), `docs/subsystems/apps.md`, `docs/subsystems/engine_shell.md`

## Context

Until now the protocol had no authentication. `engine-host --listen` printed one 128-bit token, and whoever held it was whoever each call's attribution said: the roles file restricted what a *role* may change, but a role was what a call claimed ([protocol](../subsystems/protocol.md#identity-and-what-a-role-restricts)). With several clients on one host (A3) — the owner's shell, an agent's bridge, a command line — that is no longer enough: an agent given the token to edit as `designer` could name `director` in its next call and the host would believe it. The token also travelled in URLs: `?token=` on the WebSocket upgrade, on `GET /capture/<id>`, and in the shell's address the host printed, which is where browser history, proxies and error messages keep things.

Three questions: what is a credential and who issues it; how a connection presents it, browsers included; and what it binds.

## Decision

1. **The operator issues credentials; the host keeps only their digests.** `engine-host --issue-credential <file> --name <n> --actor <a> [--role <r>] [--task <t>]` makes a token of 128 bits from the operating system's secure random source (`platform::secure_random`: `BCryptGenRandom` with the system-preferred generator on Windows, `getrandom(2)` on Linux; never a seeded engine generator), written as 22 characters of unpadded base64url, appends `{name, actor, role, task, token_sha256, issued_unix_ms}` to the credentials file (schema `CredentialsFile`) and prints the token once, then exits. A host started with `--credentials <file>` (engine-host, and engine-editor's in-process listener) accepts those tokens and the one it prints at start. The file holds no secret: it keeps each token's **SHA-256**, a lookup key over a uniformly random 128-bit input. A fast hash is the right kind for that — a slow, salted password hash exists to stretch a guessable human secret, and 2^128 equally likely tokens leave nothing to stretch — and SHA-256 is the strongest member of the kind the tree has: no practical collision or preimage attack is known against it, where SHA-1 has had practical collisions since 2017 and its own header says it decides nothing about trust. No dependency is added: `core/hash/sha256.h` is the one the document's content-addressed blobs brought (roadmap D13), carried here byte for byte from that branch so the two merge cleanly; `domain/tissue` still keeps a copy of its own, which can now forward to it. A presented token's digest is compared with every entry's in constant time, so how long a refusal takes says neither how much of a guess was right nor which entry matched. An agent never issues one: issuing is a flag of the host's command line, not a protocol method, for the same reason view policies are the operator's (ADR-0033).

2. **A connection authenticates once, at the upgrade, before any method runs.** A program sends the token in the `X-Engine-Token` header. A browser cannot set a header on a WebSocket, so the host's printed `shell` address carries a **one-time ticket** instead of the token: the first `GET /?ticket=` spends it, sets an `HttpOnly; SameSite=Strict` cookie holding a fresh per-page secret and redirects to `/` with the rest of the query and no ticket in it, and the page's upgrade and its `<img>` captures carry the cookie without the page ever seeing it. A ticket is the same 128 random bits as a token, good for one spend within ten minutes of its making (`Listener::k_ticket_lifetime_ms`), checked only on a request whose `Host` names the listener's own loopback address, and worth nothing after its redirect: one that was spent, expired or never issued is refused (403). A second tab opened from the same printed address, in a browser that already holds the cookie, is sent on without a new one; a reload of the page is a GET of `/?session=...`, which needs nothing, and its socket carries the cookie, which lives for the browser session and the host's life (no `Max-Age`; the host forgets every cookie when it exits). `?token=` is no longer read anywhere. A connection that presents nothing the host knows is closed with 1008 and a reason before its first message is read, so no method — not even a read — runs for it.

3. **A credential binds the connection's identity.** A connection that presented a credential is its actor, role and task: blank attribution fields are filled from it, and a call or a `client.hello` that names another actor or role is refused with 1008 (`data.reason` "identity") by the dispatcher, before the handler. The host's own printed token (and the shell's cookie made from it) is the operator: it is not bound, keeps A3's `client.hello`, and its calls may name any role the roles file has, as on stdin.

4. **Never logged, never in a URL.** No log line names a token, a ticket's secret or a cookie's value; a refusal says which kind of credential was missing or wrong, never what was sent. The announcement on stdout, the operator's own channel, prints the token; it is not a log.

## Consequences

- An agent's bridge given a credential cannot act outside its role by naming another; the roles file becomes a boundary for that connection rather than a guardrail.
- Every client changes how it presents the token: engine-mcp `--connect` already sent the header; the shell reads nothing from its address and relies on the cookie; tests and tools that put `?token=` in a URL send the header.
- The ticket makes the printed shell address single-use and short-lived: a browser that has not spent it within ten minutes, or a second browser, needs a new address, and a running engine-host has no way yet to print one (a row). engine-editor makes one for each page it loads.
- stdio is unchanged: a process that can write to the host's stdin started it, and is its operator.
- What this is not: TLS, a listener another machine can reach, revocation while a host runs (a credential removed from the file takes effect at the next start), or per-user accounts.

## When to revisit

When a listener is reachable from another machine (TLS, and a credential with an expiry), or when credentials need revoking without a restart. If SHA-256 is ever weakened for this use, the digest field is replaced the way any field is (a new name, a version) by SHA-3 or BLAKE3, whichever the tree carries by then; the token's length is set by `make_token` alone.
