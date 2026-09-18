#!/usr/bin/env bash
#
# The documentation diff gate (AGENTS.md, "Documentation moves with the code").
#
# A change to an interface — a public header, a shader, a schema, an app's entry point, the
# build system, or CI — lands together with the documentation that describes it. This script
# decides whether a set of changed files owes a documentation change, from the files alone, so
# it costs no build and runs in a second in CI and locally.
#
#   tools/docs-gate.sh                                   # HEAD against its parent
#   tools/docs-gate.sh --range main..HEAD                # before opening a pull request
#   tools/docs-gate.sh --base <sha> --head <sha>         # what CI passes for a push
#   tools/docs-gate.sh --files-from f --messages-from m  # a list CI already has (a PR's files)
#   tools/docs-gate.sh --repo <dir> ...                  # run against another checkout
#
# The rule: if any changed path is under a */include/ or */shaders/ directory, under schemas/,
# cmake/ or .github/, or is an apps/*/main.cpp, then at least one changed path must end in .md.
#
# The escape hatch is a line in a commit message of the range:
#
#     Docs: unchanged (the header comment was the only change)
#
# which is deliberately a sentence someone has to write and a reviewer can see, not a flag.
#
# A push whose base is unknown — the first push of a branch (all zeros), or a force push that
# left the old tip unreachable — falls back to the head commit alone, because the range it names
# does not exist and refusing to run would make the gate skippable by force-pushing.

set -uo pipefail

repo="."
base=""
head_rev=""
range=""
files_from=""
messages_from=""

usage() {
  cat <<'EOF'
tools/docs-gate.sh [--repo <dir>] [--range <base>..<head> | --base <rev> --head <rev>]
                   [--files-from <file>] [--messages-from <file>]

  --repo            repository to inspect (default: the working directory).
  --range           a git range; ... and .. are both accepted.
  --base, --head    the two ends of the range, as GitHub's push event reports them.
  --files-from      read changed paths from a file, one per line, instead of git.
  --messages-from   read commit messages from a file instead of git.

Exit status: 0 when the change is allowed, 1 when documentation is owed, 2 on a usage error.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo) repo="${2:?--repo needs a value}"; shift 2 ;;
    --range) range="${2:?--range needs a value}"; shift 2 ;;
    --base) base="${2:?--base needs a value}"; shift 2 ;;
    --head) head_rev="${2:?--head needs a value}"; shift 2 ;;
    --files-from) files_from="${2:?--files-from needs a value}"; shift 2 ;;
    --messages-from) messages_from="${2:?--messages-from needs a value}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "docs-gate: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
  esac
done

cd "$repo" || { echo "docs-gate: no such directory: $repo" >&2; exit 2; }

# A revision this repository actually has. An all-zero sha is how GitHub reports "there was no
# before": a new branch, a new tag, the first push.
have_rev() {
  local rev="$1"
  [[ -n "$rev" ]] || return 1
  [[ "$rev" =~ ^0+$ ]] && return 1
  git rev-parse --verify --quiet "${rev}^{commit}" >/dev/null 2>&1
}

changed_files=""
messages=""
described=""

if [[ -n "$files_from" ]]; then
  changed_files="$(cat "$files_from")"
  described="$files_from"
  if [[ -n "$messages_from" ]]; then
    messages="$(cat "$messages_from")"
  fi
else
  if [[ -z "$range" ]]; then
    if [[ -n "$base" ]] || [[ -n "$head_rev" ]]; then
      [[ -n "$head_rev" ]] || head_rev="HEAD"
      if have_rev "$base"; then
        range="${base}..${head_rev}"
      else
        echo "docs-gate: base '${base:-<empty>}' is not in this repository; falling back to the head commit alone"
        range=""
      fi
    elif git rev-parse --verify --quiet HEAD~1 >/dev/null 2>&1; then
      range="HEAD~1..HEAD"
      head_rev="HEAD"
    else
      head_rev="HEAD"
    fi
  fi

  if [[ -n "$range" ]]; then
    range="${range/.../..}"
    changed_files="$(git diff --name-only "$range")" || { echo "docs-gate: git diff $range failed" >&2; exit 2; }
    messages="$(git log --format=%B "$range")"
    described="$range"
  else
    # One commit: its own diff against its first parent, or everything it added if it is the
    # repository's first commit.
    changed_files="$(git show --name-only --format= "${head_rev:-HEAD}")"
    messages="$(git log -1 --format=%B "${head_rev:-HEAD}")"
    described="${head_rev:-HEAD}"
  fi
fi

# Paths that are the engine's interface to its readers. A change to one of them is a change
# somebody has to be told about.
is_gated() {
  case "$1" in
    */include/*|*/shaders/*|schemas/*|cmake/*|.github/*) return 0 ;;
    apps/*/main.cpp) return 0 ;;
  esac
  return 1
}

gated=()
docs_touched=0
count=0

while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  count=$((count + 1))
  case "$path" in
    *.md) docs_touched=1 ;;
  esac
  if is_gated "$path"; then
    gated+=("$path")
  fi
done <<<"$changed_files"

if [[ "$count" -eq 0 ]]; then
  echo "docs-gate: no files changed in $described"
  exit 0
fi

if [[ "${#gated[@]}" -eq 0 ]]; then
  echo "docs-gate: $count changed file(s) in $described, none of them an interface the gate covers"
  exit 0
fi

if [[ "$docs_touched" -eq 1 ]]; then
  echo "docs-gate: ${#gated[@]} interface file(s) changed and the documentation changed with them"
  exit 0
fi

if grep -Eq '^Docs: unchanged \(.+\)$' <<<"$messages"; then
  reason="$(grep -Em1 '^Docs: unchanged \(.+\)$' <<<"$messages")"
  echo "docs-gate: ${#gated[@]} interface file(s) changed with no documentation, allowed by: $reason"
  exit 0
fi

{
  echo "docs-gate: documentation is owed for $described"
  echo
  echo "These changed paths are interfaces, and no .md file changed with them:"
  for path in "${gated[@]}"; do
    echo "  $path"
  done
  echo
  echo "The rule (AGENTS.md, \"Documentation moves with the code\"): a change to a public header,"
  echo "a shader, a schema, an app's entry point, the build system, or CI lands in the same commit"
  echo "as the documentation that describes it — the subsystem page, the plan section, or an ADR."
  echo
  echo "If the change really changes nothing a reader needs, say so in a commit message line:"
  echo
  echo "    Docs: unchanged (<why>)"
} >&2

exit 1
