#!/usr/bin/env bash
#
# Tests for tools/docs-gate.sh. Runs under CTest as `tools.docs_gate` wherever bash is on PATH.
#
# Each case builds a throwaway git repository in a temporary directory, commits into it, and
# runs the gate over a range of it: the gate's whole input is a list of paths and a list of
# commit messages, so a repository of empty files is a complete test of it.
#
#   tools/docs-gate.test.sh [--keep]

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
gate="$here/docs-gate.sh"
keep=0
[[ "${1:-}" == "--keep" ]] && keep=1

checks=0
failures=()
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/engine-docs-gate-XXXXXX")"

# Hermetic git: the person running this has their own identity, line-ending policy, hooks, and
# signing key, and none of them may decide whether a test passes.
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_SYSTEM=/dev/null
export GIT_AUTHOR_NAME='Engine Test' GIT_AUTHOR_EMAIL='engine@example.com'
export GIT_COMMITTER_NAME='Engine Test' GIT_COMMITTER_EMAIL='engine@example.com'

cleanup() {
  if [[ "$keep" -eq 1 ]]; then
    echo "kept $tmp_root"
  else
    rm -rf "$tmp_root"
  fi
}
trap cleanup EXIT

ok() { checks=$((checks + 1)); echo "  ok   $1"; }
fail() { checks=$((checks + 1)); failures+=("$1"); echo "  FAIL $1"; [[ -n "${2:-}" ]] && printf '       %s\n' "$2"; }

# A repository with one empty commit to branch from, so every case has a parent to diff against.
new_repo() {
  local dir
  dir="$(mktemp -d "$tmp_root/repo-XXXXXX")"
  git -C "$dir" init -q
  mkdir -p "$dir/docs"
  echo 'start' > "$dir/README.md"
  git -C "$dir" add -A
  git -C "$dir" commit -qm 'initial commit'
  echo "$dir"
}

# commit <repo> <message> <path>...   — creates each path (with its directories) and commits.
commit() {
  local dir="$1" message="$2"
  shift 2
  local path
  for path in "$@"; do
    mkdir -p "$dir/$(dirname "$path")"
    echo "// changed $(date +%s%N)" >> "$dir/$path"
  done
  git -C "$dir" add -A
  git -C "$dir" commit -qm "$message"
}

# expect <exit status> <what> -- <gate arguments...>
expect() {
  local want="$1" what="$2"
  shift 3
  local out status
  out="$("$gate" "$@" 2>&1)"
  status=$?
  if [[ "$status" -eq "$want" ]]; then
    ok "$what"
  else
    fail "$what (exit $status, wanted $want)" "$out"
  fi
}

echo 'case: an interface change with no documentation'
repo="$(new_repo)"
commit "$repo" 'gfx: add a resource view' 'domain/gfx/include/gfx/view.h'
expect 1 'a public header alone is refused' -- --repo "$repo" --range 'HEAD~1..HEAD'

out="$("$gate" --repo "$repo" --range 'HEAD~1..HEAD' 2>&1)"
if grep -q 'domain/gfx/include/gfx/view.h' <<<"$out" && grep -q 'Documentation moves with the code' <<<"$out"; then
  ok 'the report names the path and the rule'
else
  fail 'the report names the path and the rule' "$out"
fi

# Which paths the gate covers is a decision about strings, so it is tested as one: a file list
# costs no commit, and the git plumbing is exercised by the cases around it.
echo 'case: the interfaces the gate covers'
for path in \
  'domain/gfx/include/gfx/view.h' \
  'domain/gfx/shaders/cull.slang' \
  'schemas/protocol.schema' \
  'apps/engine_cli/main.cpp' \
  'cmake/EngineModule.cmake' \
  '.github/workflows/ci.yml'
do
  echo "$path" > "$tmp_root/files.txt"
  expect 1 "$path is gated" -- --files-from "$tmp_root/files.txt"
done

echo 'case: changes the gate does not cover'
for path in \
  'domain/gfx/src/view.cpp' \
  'domain/gfx/tests/view_tests.cpp' \
  'core/base/README.md' \
  'content/samples/LICENSES.md' \
  'tools/dev.ps1'
do
  echo "$path" > "$tmp_root/files.txt"
  expect 0 "$path is not gated" -- --files-from "$tmp_root/files.txt"
done

echo 'case: documentation in the same range'
repo="$(new_repo)"
commit "$repo" 'gfx: add a resource view' 'domain/gfx/include/gfx/view.h' 'docs/subsystems/gfx.md'
expect 0 'a page in the same commit' -- --repo "$repo" --range 'HEAD~1..HEAD'

repo="$(new_repo)"
commit "$repo" 'gfx: add a resource view' 'domain/gfx/include/gfx/view.h'
commit "$repo" 'doc: describe the resource view' 'docs/subsystems/gfx.md'
expect 0 'a page in a later commit of the range' -- --repo "$repo" --range 'HEAD~2..HEAD'

echo 'case: the escape hatch'
repo="$(new_repo)"
commit "$repo" 'gfx: fix a typo in a comment

Docs: unchanged (a comment typo, nothing a reader of the page would see)' 'domain/gfx/include/gfx/view.h'
expect 0 'a declared reason is accepted' -- --repo "$repo" --range 'HEAD~1..HEAD'

repo="$(new_repo)"
commit "$repo" 'gfx: fix a typo in a comment

Docs: unchanged' 'domain/gfx/include/gfx/view.h'
expect 1 'a reason with no reason in it is not' -- --repo "$repo" --range 'HEAD~1..HEAD'

repo="$(new_repo)"
commit "$repo" 'gfx: add a view' 'domain/gfx/include/gfx/view.h'
commit "$repo" 'gfx: fix the view

Docs: unchanged (follow-up to the commit before it)' 'domain/gfx/src/view.cpp'
expect 0 'the reason may be in any commit of the range' -- --repo "$repo" --range 'HEAD~2..HEAD'

echo 'case: a base that is not there'
repo="$(new_repo)"
commit "$repo" 'gfx: add a resource view' 'domain/gfx/include/gfx/view.h'
expect 1 'an all-zero base falls back to the head commit' -- \
  --repo "$repo" --base '0000000000000000000000000000000000000000' --head HEAD
expect 1 'an unreachable base falls back too' -- \
  --repo "$repo" --base 'deadbeefdeadbeefdeadbeefdeadbeefdeadbeef' --head HEAD

repo="$(new_repo)"
commit "$repo" 'doc: write the page

and the header with it' 'domain/gfx/include/gfx/view.h' 'docs/subsystems/gfx.md'
expect 0 'the fallback still sees the documentation' -- \
  --repo "$repo" --base '0000000000000000000000000000000000000000' --head HEAD

echo 'case: the first commit of a repository'
repo="$(mktemp -d "$tmp_root/repo-first-XXXXXX")"
git -C "$repo" init -q
mkdir -p "$repo/domain/gfx/include/gfx"
echo '#pragma once' > "$repo/domain/gfx/include/gfx/view.h"
git -C "$repo" add -A
git -C "$repo" commit -qm 'the first commit'
expect 1 'a root commit is diffed against nothing' -- --repo "$repo" --base '0000000000000000000000000000000000000000' --head HEAD

# Given a file list the gate runs no git at all, so these cases need no repository.
echo 'case: lists from somewhere else (a pull request)'
repo="$tmp_root"
printf 'domain/gfx/include/gfx/view.h\ndomain/gfx/src/view.cpp\n' > "$tmp_root/files.txt"
printf 'gfx: add a resource view\n\nwhy it is there\n' > "$tmp_root/messages.txt"
expect 1 'a file list with no page' -- --repo "$repo" --files-from "$tmp_root/files.txt" --messages-from "$tmp_root/messages.txt"

printf 'domain/gfx/include/gfx/view.h\ndocs/subsystems/gfx.md\n' > "$tmp_root/files.txt"
expect 0 'a file list with one' -- --repo "$repo" --files-from "$tmp_root/files.txt" --messages-from "$tmp_root/messages.txt"

printf 'domain/gfx/include/gfx/view.h\n' > "$tmp_root/files.txt"
printf 'gfx: rename a local\n\nDocs: unchanged (a local variable, invisible from the page)\n' > "$tmp_root/messages.txt"
expect 0 'a file list with the escape in its messages' -- --repo "$repo" --files-from "$tmp_root/files.txt" --messages-from "$tmp_root/messages.txt"

echo 'case: nothing changed'
repo="$(new_repo)"
expect 0 'an empty range passes' -- --repo "$repo" --range 'HEAD..HEAD'

echo 'case: usage'
expect 2 'an unknown argument is a usage error' -- --repo "$repo" --nonsense
expect 2 'a missing repository is a usage error' -- --repo "$tmp_root/no-such-directory"

echo
if [[ "${#failures[@]}" -gt 0 ]]; then
  echo "${#failures[@]} of $checks checks failed:"
  for f in "${failures[@]}"; do echo "  - $f"; done
  exit 1
fi
echo "docs-gate: $checks checks passed"
exit 0
