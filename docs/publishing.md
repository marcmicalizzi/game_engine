# Publishing: how this repository goes public, and how private work continues

This repository will be public (Apache-2.0, [ADR-0014](adr/0014-apache-2-license-and-dependency-policy.md)). Some work will not be: experiments, unreleased engine work, and content that is not this project's to redistribute. The constraint that shapes everything below is simple and often discovered too late:

> **A public repository has no private branches.** Every branch, tag and commit of a public GitHub repository is public, including a branch pushed by mistake and deleted a minute later. GitHub's Fork button does not help either: a fork of a public repository is public.

So private work does not live in a branch of the public repository. It lives in one of two places, and the first is much better than the second.

## (a) Private and experimental capabilities and content: an overlay repository

The preferred home for anything private is a **separate private repository** whose tree is an *overlay*: additional capability modules, additional content packs, additional schemas, additional docs. [ADR-0027](adr/0027-additive-capabilities.md) makes this structural rather than a convention — a capability is a module in its own directory that attaches through registration points and is proved removable by `ENGINE_MINIMAL=ON`. An overlay is exactly that, one directory further out.

- The **build discovers an overlay directory when it is present** (an `ENGINE_OVERLAY_DIR` cache variable or a sibling directory by convention) and adds its modules through the same `engine_module()` path everything else uses.
- **The public tree never references the overlay**: no path, no module name, no `if(EXISTS ...)` special case, no docs link. A public checkout with no overlay is a complete, buildable, passing tree, which is the same property `ENGINE_MINIMAL` already proves.
- Nothing in the overlay ever needs publishing, so **it is immune to history mistakes**: there is no commit to cherry-pick, no branch to rebase, and no way for a private asset to reach the public repository through an ordinary workflow.

This is where experiments belong by default. Move work to (b) only when it genuinely modifies the engine itself.

## (b) Unreleased engine work: a private mirror that behaves like a fork

Work on the engine's own code before it is ready to be seen lives in a **private mirror**: a private repository holding the same history, which is what a private fork would be if GitHub allowed one. It is created by pushing this repository's history into a new private repository (`git push --mirror`), and from then on git treats it exactly like any fork — the same commits, the same objects, `public` and `private` as two remotes of one working copy.

The mirror's history is a **superset** of the public one: everything public is in it, plus the unreleased work.

**Publishing from it.** GitHub cannot open a pull request from a private repository into a public one, so publishing is a push plus an ordinary pull request:

1. Build a branch that contains **only publishable commits** — cherry-picked or rebased onto the public tip, never a merge of a branch whose history ever contained private material. A squash is fine; a merge commit that drags in a private ancestor is not, and the difference is invisible in a diff.
2. Push that branch to the public repository (or to a public fork of it, if the contributor does not have push access).
3. Open the pull request from that branch, and review it there like any other.

**A commit is the unit of publishability.** Rewriting one later is a history rewrite for everyone; keeping private and public changes in separate commits from the start costs nothing at the time.

**Numbering.** ADR numbers and experiment numbers are contiguous and checked (`tools/docs-check.ps1`), and a number reserved in private is a gap or a dangling reference in public. So **ADRs, experiments and docs for private work live in the overlay**, with their own numbering space and their own index, and take a public number only when the work is published. The same goes for a plan section that would reference them.

## (c) Before the first public push

The first push publishes history, not just a tree. Audit it, then decide what to publish:

- **Personal paths and identity**: absolute paths from the owner's disks in source, scripts, presets, test fixtures, JSON outputs and committed logs; machine and host names; e-mail addresses in commit metadata that should not be public.
- **Asset paths and assets**: anything under `content/` that is not licensed for redistribution, sample assets that a fetch script should download instead ([AGENTS.md](../AGENTS.md)), and any generated asset whose provenance does not record a redistributable licence ([07 §7.5](plan/07-content-pipeline.md#75-provenance-and-licensing-metadata)).
- **Anything that was ever committed and then removed**, which is still in the history and is the whole reason this is an audit of history rather than of the working tree.
- **Third-party licences**: `third_party/LICENSES.md` complete and accurate, `NOTICE` correct ([ADR-0014](adr/0014-apache-2-license-and-dependency-policy.md)).
- Then **decide between publishing the history as it stands and starting the public repository from a squashed snapshot.** A squash loses the development record — which is real value on a project whose documents are its memory — but it is the only reliable answer if the audit finds anything in the history that cannot be published. Decide once; both are defensible, and rewriting later is not.

## (d) The gate runs on the public tree alone

`tools/dev.ps1 docs` and the whole test suite must pass on a **fresh clone of the public repository with no overlay present**. A link into the overlay, a test that needs a private asset, a module whose page is in the overlay, or a plan reference to a private experiment breaks the public build for everyone who is not the owner — and the person who would notice first is the first outside contributor, which is the worst possible time. CI runs against the public tree for that reason, and an overlay is exercised by a second job, never by the first.
