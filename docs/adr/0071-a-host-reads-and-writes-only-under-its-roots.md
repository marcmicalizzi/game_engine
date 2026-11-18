# ADR-0071: A host reads and writes a client's paths only under its roots

- **Status:** Proposed
- **Date:** 2026-11-18
- **Plan references:** docs/plan/06-agent-tooling.md §6.5 (permissions), §6.9 (the tools)
- **Docs touched:** `docs/subsystems/protocol.md` ("The host's file roots"), `docs/subsystems/io.md` ("Roots"), `docs/subsystems/apps.md`, `docs/experiments/host-file-roots-survey-2026-11-18.md`

## Context

A host (`engine-host`, and `engine-editor`'s in-process listener) serves clients on loopback: engine-cli, the browser shell, and engine-mcp for agents. Several of its methods take a path the client names and read, write or create there with the host's rights. The survey of 2026-11-18 found thirty-seven such places on main and the branches merging that night. Most are registered `read_only`, so the default `qa` role reaches them. `session.save_game` and `session.load_game` make a directory anywhere and copy a document into it. `content.build` writes a container over any file, a document's own layer files included. `session.open` and `render.load` read any file on the machine and say whether it exists. Three methods (`doc.place_asset`, `doc.put_blob`, the listener's static files) each carry their own lexical check, and only one follows links. The listener's shell directory is on disk and read per request, so a writer that can reach it plants a page in the operator's origin. A host has several clients at once, and credentials bind a connection to a role. A role that may only read must not be able to make the host read or write the operator's disk at will.

The alternatives were: a check per method (what the three have, and why they differ); roots per role (machine directories in a shipped roles file); roots per credential (a second policy beside roles, with the same answer on every host there is); or a switch that turns a check off for old callers (no caller but tests and tools of this tree exists).

## Decision

1. **Every client-named path is admitted by one function, `io::Roots::admit` in `foundation/io`, before the host touches it.** The host opens the canonical path the function returns, never the client's string. That covers every path field of every method, every path named inside a file the host reads for a client (a scene's meshes, a glTF's buffers and images, a manifest's entries), every path a document's record names, and the listener's static files.
2. **A host has roots, each a name and a directory with an access (read, or read and write), set by its operator and by nothing else.** With nothing configured, a host has `doc`, its `--doc` directory (read), and `out`, that document's `out/` (read and write, made by the first write under it). The operator adds `--root <name>=<dir>[:rw]`, the same spelling on engine-host and engine-editor, forwarded by engine-cli and engine-mcp. Every `--mount` is a root of its scheme's name. No protocol method changes them. **A context with no roots admits nothing.**
3. **The owner's decision, 2026-11-18 ("Only operator folders"):** a client makes the host write only inside the document's own `out/` and inside folders the operator named when starting the host. That holds for every role alike: a role decides which writers it may call, not where they write.
4. **A path crosses the protocol, and is stored, as a root's name and a path under it** (`out://captures/a.png`), **never as an absolute path.** An absolute path exists only on the operator's command line and inside the host process. The owner gave the reasons on 2026-11-18. An absolute path of the operator's machine should not end up in a committed document, which ends up in a public repository. And it may point into a worktree that will not exist after cleanup. A stored path whose root this host does not have, or whose directory is gone, is refused with "root <name> is not present on this host" and is never resolved another way.
5. **Roots are per host, the same for every role and credential.** A role decides whether a method may be called; the roots decide where any call may reach.
6. **Never written by a client, whatever the roots say**: an open document's directory outside its `out/`, and a directory the host serves to a browser. The start refuses a configuration that would let a writable root hold the served directory.
7. **The function's rules** are those of io.md's "Roots". The lexical refusals are the same on both systems: control characters, Windows' reserved characters, a `:` that is not a drive's, namespace prefixes, UNC unless a root is UNC, device names, and components ending in a dot or a space. Then `..` is collapsed, and the longest existing prefix is made canonical by the system (`GetFinalPathNameByHandleW` after `CreateFileW` on Windows, `realpath` on Linux). A dangling link is refused. The path is compared with each root by whole components, case-insensitively on Windows. A write's last component must be a regular file, which is replaced by rename.
8. **A refusal and a missing file are one answer**: 1003, `data.reason` `"path"`, one sentence per access that repeats only the client's string. The operator's log has the reason and the canonical path.
9. **Recommended, not yet answered by the owner: the browser shell never names a path.** It picks an entry the host listed (a root and a path under it), uses `keep` for pictures and a grant for imports, and its console is any client's. The reason beside it: the page carries the operator's cookie, and a listed entry is already in the portable form, so the shell never handles an absolute path of the machine at all.
10. **The editor's own dialog and window drops are the operator's choice** and are to admit one file once, through a grant; no other method takes a grant. Not built.

## What was built (2026-11-18)

`io::Roots` with its tests; `domain/protocol/paths.h` (`admit_client_path`, `client_base`, `path_not_found`, `portable_path`, `configure_host_roots`); `Context::roots`; `--root` on the four apps; the listener's static files; `session.open` (and the protection of open documents); `session.save_game` and `session.load_game`; `content.build` (source, manifest, entries, output, ddc); `render.load` (mesh, scene, overlay, ddc); `render.capture`, `render.evaluate` (out_dir, name); `render.compare` (a, b, flip_out); `render.benchmark` (camera_path). Answers in the portable form: `SessionInfo.path`, `SaveGameResult.path`, `LoadGameResult.dir`, `RenderCompareResult.flip_image`, a loaded scene's `source`. The methods left, with their reasons, are listed in protocol.md's "What is built, and what is left".

## Consequences

- Every test and tool that drives a host with a path names its roots or works under the document's `out/`. The end-to-end tests give each host they start `--root tests=<the TempDir parent>:rw` (`tests/support/test_temp_dir.h`, `scratch_root_spec`), and a case that tests the roots names its own. engine-cli forwards `--doc` to the host it starts instead of sending `session.open` with a path; engine-mcp gives its host `--root workspace=<workspace>:rw`. Nothing keeps the old behaviour.
- A client-named `ddc` is a place the host writes, admitted as any output. Whether the field should go entirely, leaving the cache's place to the operator, is left to the row that moves the readers.
- `doc.place_asset`'s and `doc.put_blob`'s checks and `net::safe_relative_path` are to be replaced by the function. The listener's check is replaced now; the other two still stand.
- Readers in lower layers (`renderer::read_scene_file`, `doc_scene`, `assets::load_gltf`, `content_build::read_manifest`) are to take an optional `const io::Roots*`. engine-view, engine-content and the tools pass none, since every path they read is their operator's.
- Forbidden: a method that opens a client's string without the function; a protocol host built without roots; a role or credential field that widens the roots; an absolute path in an answer or a record where a root's name and a path under it can stand.

## Revisit when

- A host serves a client of less trust than its operator over something other than loopback, or a credential has to be narrower than the host: then roots per credential, passed to the same function.
- A method needs to list a directory or follow a link on purpose.
- The operating system's path rules change (a new namespace, device names retired on every supported Windows), or a third platform is supported.
- The owner answers decision 9.
