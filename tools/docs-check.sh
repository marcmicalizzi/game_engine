#!/usr/bin/env bash
#
# tools/docs-check.ps1 from a shell. The check itself is PowerShell, like the rest of tools/
# (ADR-0021), and PowerShell 7 is on both platforms' CI images; this wrapper exists so a Linux
# contributor, a hook, or a container that has no pwsh does not have to care.
#
#   tools/docs-check.sh [--root <dir>] [any other docs-check.ps1 argument]
#
# Without pwsh the check is skipped, not failed: the same tree is checked on every other
# machine and in CI, and a missing interpreter is not a documentation problem. The banned-pattern
# lint is registered the same way, by find_program(pwsh) in cmake/EngineTesting.cmake.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v pwsh >/dev/null 2>&1; then
  echo "docs-check: pwsh not found; skipping (see tools/docs-check.ps1, AGENTS.md 'Documentation moves with the code')"
  exit 0
fi

exec pwsh -NoProfile -ExecutionPolicy Bypass -File "$here/docs-check.ps1" "$@"
