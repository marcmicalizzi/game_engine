# Structure raises

One file per branch that moved a hub of `tools/structure-ceilings.json`: `<row>.json`, written by
`pwsh tools/structure-check.ps1 -Raise <row> [-Reason "<why>"]`, never by hand. A hub's effective
ceiling is its ceiling plus the sum of every file's delta for it, so two branches that each grow
a hub merge with no conflict. The coordinator folds the files into the ceilings after a merge
(`-Rebaseline`), which deletes them. The rule, and why: [structure and
citations](../../docs/ci/structure-and-citations.md#ceilings-for-structure).
