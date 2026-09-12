# ADR-0007: Schema code generation; no hand-written serialization

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/02-architecture.md §2.5, docs/plan/03-data-model.md §3.8

## Context

Every agent-facing feature (introspection, protocol, diff, migration, MCP tool definitions) depends on knowing the shape of every type. C++ has no usable static reflection in the compilers this project builds with (as of 2026, only GCC 16 ships it behind a flag).

## Decision

All serialized or transmitted types (document objects, protocol messages, world events, components, provenance) are declared once in `schemas/` in an IDL. A generator emits C++ types, JSON (de)serializers, JSON Schema, protocol documentation, MCP tool definitions, and migration stubs. Generated code is never hand-edited. Types carry versions; additive changes generate migrators automatically, other changes require a checked-in migration function tested against the migration corpus.

## Consequences

Agents never write serialization. The IDL is a Phase 0 deliverable and everything else waits on it. Renames are aliases, never delete-and-add.

## Revisit when

C++26 static reflection ships in both MSVC and Clang, at which point the generator may shrink but the single-declaration rule stays.
