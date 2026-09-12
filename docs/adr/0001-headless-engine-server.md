# ADR-0001: Headless engine server with a versioned protocol; the editor is a client

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/02-architecture.md §2.1–2.2, docs/plan/06-agent-tooling.md §6.1–6.3

## Context

The engine must be fully drivable by agents and by humans who never use an agent, and later must serve as a dedicated multiplayer server. An editor with in-process privileged access to engine internals creates a second code path that agents cannot reach and that diverges from the API.

## Decision

The runtime (`engine-host`) is a server process that runs in `sim` (no GPU, no window), `offscreen` (GPU, no window), or `windowed` mode. All modes speak one versioned JSON-RPC protocol plus a bulk channel for large data. The GUI editor, the MCP bridge, the CLI, and tests are clients with equal access. Every editor gesture compiles to the same commands an agent sends. If a feature is reachable only from the GUI, that is a protocol bug.

## Consequences

Agents and humans share one transaction, attribution, undo, and validation path. Headless playtesting, CI rendering, and a dedicated server come for free. The editor is more work to build than an in-process one and every new engine feature must ship with its protocol surface.

## Revisit when

Never for the principle. The transport (JSON-RPC over WebSocket/stdio) may change with a new ADR if profiling shows it limiting the editor.
