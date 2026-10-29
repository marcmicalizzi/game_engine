# deadwood_schemas (core, capability-owned generated types)

Schema descriptions and JSON serialization for `engine.deadwood.DeadwoodFile`, `RemainKind`
and `Support`. The [deadwood solver](deadwood.md) owns their semantics and validation.

Located under the domain capability and compiled at the core schema layer. Source: `domain/deadwood/schemas/deadwood.schema`, importing the existing flora schema. A local
schemac command supplies flora's schema directory as the import root and emits only the deadwood
types under the build directory. Generated code is never edited. The schema module links
`flora_schemas`; it does not duplicate those types or their registrations.

The `deadwood` capability switch removes this module together with its owner. Its public API is
the generated `schemas/deadwood.h`; it has no runtime tick or hot data. Deadwood's recipe
round-trip, rejection, canonical identity and complete-container determinism tests exercise it.
