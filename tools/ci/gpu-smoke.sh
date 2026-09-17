#!/usr/bin/env bash
#
# GPU smoke run for a self-hosted Linux runner: build, report the machine's
# Vulkan adapters, run the tests.
#
#   tools/ci/gpu-smoke.sh [--preset <name>] [--filter <regex>] [--clean]
#                         [--require-adapters]
#
# Three steps, in order:
#
#   1. tools/dev.ps1 build -Preset <preset> (PowerShell 7 is the engine's build
#      entry point on both platforms; it configures first if needed).
#   2. build/<preset>/bin/engine-cli gpu.adapters, printed in full. This is the
#      machine's capability report and it is what explains the tests that skip:
#      a GPU without mesh shaders skips the mesh-shader, software-raster,
#      occlusion, and shading tests and still runs the vertex path, the cull and
#      LOD tests, the resolve reflection, and everything on the CPU. On a
#      headless server the window and swapchain tests skip as well.
#   3. ctest --preset <preset>. The script exits with ctest's status; the log it
#      leaves in build/<preset>/Testing/Temporary/LastTest.log is what CI keeps.
#
# Adapter enumeration that finds no device is a warning, not a failure, so a
# driver problem does not look like a test failure; --require-adapters turns it
# into one, which is what a machine that is supposed to have a GPU wants.

set -euo pipefail

preset="linux-clang-debug"
filter=""
clean=0
require_adapters=0

usage() {
  cat <<'EOF'
tools/ci/gpu-smoke.sh [--preset <name>] [--filter <regex>] [--clean] [--require-adapters]

  --preset             CMake preset to build and test (default: linux-clang-debug).
  --filter             ctest -R regex, to run a subset.
  --clean              remove build/<preset> first, for a from-scratch run.
  --require-adapters   fail when Vulkan finds no device instead of warning.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) preset="${2:?--preset needs a value}"; shift 2 ;;
    --filter) filter="${2:?--filter needs a value}"; shift 2 ;;
    --clean) clean=1; shift ;;
    --require-adapters) require_adapters=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "gpu-smoke: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
  esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dev="$root/tools/dev.ps1"
build_dir="$root/build/$preset"

section() { printf '\n== %s\n' "$1"; }

section "gpu smoke on $(hostname): preset $preset"
echo "os:        $(uname -sr)"
echo "cpus:      $(nproc 2>/dev/null || echo '?')"
echo "root:      $root"

for tool in pwsh ctest; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "gpu-smoke: $tool not found on PATH (see docs/ci/self-hosted-runners.md)" >&2
    exit 1
  fi
done

if [[ "$clean" -eq 1 ]]; then
  section "clean"
  pwsh -NoProfile -File "$dev" clean -Preset "$preset"
fi

section "build"
pwsh -NoProfile -File "$dev" build -Preset "$preset"

section "gpu.adapters"
cli="$build_dir/bin/engine-cli"
if [[ ! -x "$cli" ]]; then
  echo "gpu-smoke: engine-cli not found at $cli; did the build produce it?" >&2
  exit 1
fi

adapters_status=0
adapters_out="$("$cli" gpu.adapters 2>&1)" || adapters_status=$?
printf '%s\n' "$adapters_out"

# gpu.adapters answers the driver question in its result, not in its exit code:
# it returns available=false with an error when the loader or the ICD behind it
# is missing, which is what a hosted runner looks like, and available=true with
# an empty list when an ICD is installed but no device is reachable. Every
# adapter carries exactly one "tier" field, so counting those counts devices.
available=0
if grep -Eq '"available"[[:space:]]*:[[:space:]]*true' <<<"$adapters_out"; then
  available=1
fi
adapter_count="$(grep -c '"tier"' <<<"$adapters_out" || true)"
echo "$adapter_count adapter(s) reported"

if [[ "$adapters_status" -ne 0 || "$available" -eq 0 || "$adapter_count" -eq 0 ]]; then
  message="no usable Vulkan device on this machine, so every GPU test will skip. Check that the NVIDIA driver is installed, that libvulkan.so.1 is present, and that the ICD JSON is in /usr/share/vulkan/icd.d (see docs/ci/self-hosted-runners.md)."
  if [[ "$require_adapters" -eq 1 ]]; then
    echo "gpu-smoke: $message" >&2
    exit 1
  fi
  echo "gpu-smoke: warning: $message" >&2
fi

section "ctest"
ctest_args=(--preset "$preset")
if [[ -n "$filter" ]]; then
  ctest_args+=(-R "$filter")
fi
cd "$root"
status=0
ctest "${ctest_args[@]}" || status=$?

log="$build_dir/Testing/Temporary/LastTest.log"
section "result"
if [[ -f "$log" ]]; then
  echo "ctest log: $log"
else
  echo "gpu-smoke: warning: no ctest log at $log" >&2
fi
echo "ctest exit status: $status"
exit "$status"
