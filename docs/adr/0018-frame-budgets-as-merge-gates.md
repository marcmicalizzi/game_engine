# ADR-0018: Frame budgets are merge gates from the first renderer milestone

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/11-performance-principles.md §11.1, docs/plan/09-testing-profiling.md

## Context

Modern high-end engines routinely ship features at unplayable frame rates and downgrade later. The project exists partly in reaction to that. Performance treated as a polish phase never recovers.

## Decision

Every target configuration has a frame-budget file (per-pass GPU milliseconds, per-system CPU milliseconds, memory by tag, streaming bandwidth). From Phase 1 onward, a change that pushes a pass over budget on the reference scenes does not merge; it lands behind a quality tier or is optimized first. Every feature carries a cost estimate before implementation and a measured cost after. Performance regressions have the same priority as crashes. Nightly regression runs use medians over repeated runs on dedicated hardware with significance tests. The frame report attributes time to engine and game systems separately.

## Consequences

"Add a quality option" is not a substitute for optimizing the default path. Some features arrive later than they otherwise would, already within budget.

## Revisit when

Never.
