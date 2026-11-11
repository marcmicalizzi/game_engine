# ADR-0062: The machine locks have a line

- **Status:** Proposed
- **Date:** 2026-11-11
- **Plan references:** docs/plan/11-performance-principles.md (a measurement needs the GPU to itself); extends [ADR-0049](0049-tests-take-the-gpu-lock-per-device.md), whose "Revisit when" names this change ("GPU-LOCK.md changes — a queue, a shorter poll") and whose decision stands
- **Docs touched:** `docs/subsystems/gpu_lock.md` ("The line"), `docs/ci/local-linux.md` ("One container build at a time, whoever started it"), `docs/ci/what-to-run.md` ("Waiting costs more than it looks"), `docs/roadmap.md` (T96)

## Context

Two files decide who works on the development machine: the GPU lock and the Linux container build lock, one protocol (`D:\workspace\GPU-LOCK.md`) with two implementations, `foundation/gpu_lock` and `tools/lib/MachineLock.psm1`. The protocol says who *has* a lock — a file created with create-new, carrying an owner, a purpose and an expiry — and nothing about who is *next*: every waiter retried the create-new at its own interval, 15 to 20 s, and whoever's retry came first after a release had the lock. A holder that released and asked again in the same breath always came first.

On the night of 2026-11-10, with twelve agents and a merge gate on the machine, that was the whole schedule. A picture run waited from 22:39 to 01:36 for the GPU while later arrivals took it again and again; one holder re-took it at once after each lease for hours; gate tests waited their full half hour and exited 75; three rows ended the night with no engine picture. Container builds waited 110, 136, 147 and 240 minutes while later arrivals won, and four rows handed back without GCC having compiled them.

The constraints on a fix:

- The lock file is shared with tools that are not the engine's (the Blender agent takes the GPU lock from Python) and with every binary already built in another worktree. A process built before the change and one built after must not corrupt each other, and the worst an old one may do is behave as it did.
- A waiter or a holder that dies must never block the others, with the bound stated.
- Callers do not change: the engine's processes, the tests, `tools/gpu-lock.ps1 run`, `tools/linux-build.ps1`, `-NoWait`. The owner's interactive sessions take no lock and still take none.
- There is no arbiter and no daemon: the locks are files because nothing has to be running for them to work.

The alternatives:

- **An operating-system lock** (`LockFileEx`, `flock`, a named mutex). None documents an order among its waiters, Windows' own locks are deliberately unfair, and a lock the OS holds carries no owner, purpose or expiry for another tool or a person to read ([gpu_lock](../subsystems/gpu_lock.md#published-practice-and-how-far-each-was-read)).
- **A broker process** that hands the lock out. A queue for free, and a new thing that must be running, restarted and version-matched across a dozen worktrees.
- **A convention** (the briefs' "five minutes between your leases"). It was the rule that night.
- **Tickets beside the lock file** — Lamport's bakery, on a file system.

## Decision

1. **An ask that cannot have the lock at once takes a ticket, and only the oldest live ticket tries the lock.** A ticket is a file in `<lock path>.line`, named `t-<key ms>-<arrived ms>-<pid>-<counter>.json` in fixed-width digits, so the line is a sorted directory listing. The ask deletes its ticket when it has the lock or stops asking. An ask that finds no live ticket tries the lock once without one: nobody is there to be passed.
2. **The lock file and every rule about it are unchanged.** Exclusion is still its create-new. The line is advice among the processes that take part in it and decides only which of them makes the attempt. A tool that takes no ticket is neither helped nor harmed by the line's existence: it retries as it always did and has the lock when its retry lands on a free one.
3. **A ticket's place is its arrival plus the lease it asked for** (`key`), capped at half an hour; ties by arrival, then pid, then the process's own counter. Equal asks are first come, first served, and a holder that asks again is a new arrival at the back. A short ask passes a long one only while the long one has waited less than the difference between them; nobody is passed for longer than the lease it asked for. No priorities, no classes, no pre-emption, and the declared length is not enforced.
4. **A ticket has a 30 s lease of its own**, refreshed every 10 s from the loop that tries the lock. An expired ticket is a dead waiter: skipped and removed by whoever looks. The line moves under 32 s after a waiter dies. A dead holder is detected as before, one lock lease after its last refresh, and only the head of the line breaks it.
5. **Every waiter looks every 250 ms**, because the head of the line is the only one that takes a released lock and a waiter learns it is the head only by looking.
6. **The line is visible**: `tools/gpu-lock.ps1 status` prints the holder and the line in order; every waiting line and every give-up says where the process stood.
7. **A process that finds the lock held on its behalf takes no ticket** (a holder's child, a command inside `gpu-lock.ps1 run`), and a child that takes its parent's lock over while its own device is open does so without one.

## Consequences

- The starvation of 2026-11-10 cannot recur among processes that take tickets: a re-asker stands behind whoever was waiting, and a wait is bounded by the holds before it, each once.
- A test or a two-minute run no longer waits behind every ten-minute lease that was a second earlier; a ten-minute lease is passed for at most its first ten minutes in the line.
- **Holders now take turns per hold.** A test executable's device holds were effectively chained before (it usually re-took the lock across its own gaps); beside N waiters it is now served once a round, and its half-hour queue budget is spent a turn at a time. Each hand-off can leave the GPU idle for up to 250 ms. This is the convoy a first-come hand-off is known to produce; it is accepted because the holds are device lifetimes, not microseconds.
- A binary built before the line, and the Blender agent until GPU-LOCK.md's snippet takes a ticket, is outside the order: it can be served out of turn, and against a line that never empties its 15-second retry rarely lands first. Worst case it waits out its own deadline, as it could before. GPU-LOCK.md lives outside the repository and is the owner's to amend.
- The polling interval of ticket-taking tools is 250 ms, where GPU-LOCK.md's rule 2 says 15 to 30 s.
- Declared lengths are trusted. A holder that outstays what it asked for has jumped the line by the difference; `status` shows it, and nothing stops it.
- Two implementations must agree on the ticket's name and body to the byte; each one's tests plant tickets in the format and read the other's.

## Revisit when

- Declared lengths are abused (a short `-Minutes` on a long run), which would call for a rule every tool obeys, not only these two.
- The per-hold turn-taking costs a gate its long GPU tests (deadlines reached in slices): the remedy would be a ticket that keeps its place across one process's consecutive holds for the length it declared.
- A tool that cannot take tickets needs the GPU on equal terms with the line.
- The lock moves off a local file system: the line relies on a directory listing being current and on one clock.
