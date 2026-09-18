#!/usr/bin/env bash
#
# GPU smoke run for a self-hosted Linux runner: build, report the machine's
# Vulkan adapters, run the tests, render the extreme resolutions.
#
#   tools/ci/gpu-smoke.sh [--preset <name>] [--filter <regex>] [--clean]
#                         [--require-adapters] [--skip-captures]
#                         [--capture-timeout <seconds>] [--capture-frames <n>]
#
# Four steps, in order:
#
#   1. tools/dev.ps1 build -Preset <preset> (PowerShell 7 is the engine's build
#      entry point on both platforms; it configures first if needed).
#   2. build/<preset>/bin/engine-cli gpu.adapters, printed in full. This is the
#      machine's capability report and it is what explains the tests that skip:
#      a GPU without mesh shaders skips the mesh-shader, software-raster,
#      occlusion, and shading tests and still runs the vertex path, the cull and
#      LOD tests, the resolve reflection, and everything on the CPU. On a
#      headless server the window and swapchain tests skip as well.
#   3. ctest --preset <preset>. The log it leaves in
#      build/<preset>/Testing/Temporary/LastTest.log is what CI keeps.
#   4. engine-view at 3840x2160 and at the four resolutions plan 04 section 4.6
#      asks CI to render nightly - 11520x2160 surround, 7680x4320, 1080x3840
#      portrait, 5120x1440 ultrawide - each with --frames <n> --capture. The
#      JSON summary of each run, the adapter report, and the ctest status go to
#      build/<preset>/gpu-smoke.json and the pictures to
#      build/<preset>/captures/. Every resolution is attempted whatever the ones
#      before it did: each has its own time budget and its own result, so one
#      that crashes or hangs cannot hide the rest. On this headless server they
#      all skip, because engine-view needs a display and exits 3 without one.
#
# Adapter enumeration that finds no device is a warning, not a failure, so a
# driver problem does not look like a test failure; --require-adapters turns it
# into one, which is what a machine that is supposed to have a GPU wants. A
# resolution where engine-view exits 3 is a skip for the same reason; any other
# nonzero exit, or a run that outlives its budget, fails the script after all
# five have been tried. The exit status is ctest's, or 1 when ctest passed and a
# capture failed.

set -euo pipefail

preset="linux-clang-debug"
filter=""
clean=0
require_adapters=0
skip_captures=0
capture_timeout=300
capture_frames=60

usage() {
  cat <<'EOF'
tools/ci/gpu-smoke.sh [--preset <name>] [--filter <regex>] [--clean] [--require-adapters]
                      [--skip-captures] [--capture-timeout <seconds>] [--capture-frames <n>]

  --preset             CMake preset to build and test (default: linux-clang-debug).
  --filter             ctest -R regex, to run a subset.
  --clean              remove build/<preset> first, for a from-scratch run.
  --require-adapters   fail when Vulkan finds no device instead of warning.
  --skip-captures      stop after ctest; do not render the extreme resolutions.
  --capture-timeout    seconds one resolution may take before it is killed (default: 300).
  --capture-frames     frames to render per resolution (default: 60).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) preset="${2:?--preset needs a value}"; shift 2 ;;
    --filter) filter="${2:?--filter needs a value}"; shift 2 ;;
    --clean) clean=1; shift ;;
    --require-adapters) require_adapters=1; shift ;;
    --skip-captures) skip_captures=1; shift ;;
    --capture-timeout) capture_timeout="${2:?--capture-timeout needs a value}"; shift 2 ;;
    --capture-frames) capture_frames="${2:?--capture-frames needs a value}"; shift 2 ;;
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

# ---- extreme-resolution captures (plan 04 section 4.6) ----------------------
#
# The four resolutions the plan names, plus plain 4K as the number the others
# are read against. They are here because "no 16-bit screen coordinates
# anywhere, all screen-space structures sized from the render config" is a claim
# that only a run at 48:9 and at 8K can check: an overflow in a tile count, a
# Hi-Z level count, or a workgroup dispatch shows up as a wrong picture or a
# device loss, and nothing smaller finds it.
resolutions=(
  "3840 2160 4K, the baseline the others are read against"
  "11520 2160 triple 4K surround, 48:9"
  "7680 4320 8K"
  "1080 3840 portrait"
  "5120 1440 ultrawide"
)

json_escape() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }

captures_dir="$build_dir/captures"
view="$build_dir/bin/engine-view"
captures_json=""
capture_failures=0

if [[ "$skip_captures" -eq 1 ]]; then
  section "extreme-resolution captures (skipped by --skip-captures)"
elif [[ ! -x "$view" ]]; then
  section "extreme-resolution captures"
  echo "gpu-smoke: warning: engine-view not found at $view; skipping the captures" >&2
else
  section "extreme-resolution captures"
  mkdir -p "$captures_dir"
  for entry in "${resolutions[@]}"; do
    read -r w h why <<<"$entry"
    name="${w}x${h}"
    capture="$captures_dir/$name.png"
    out="$captures_dir/$name.stdout.txt"
    err="$captures_dir/$name.stderr.txt"
    rm -f "$capture" "$out" "$err"
    echo "-- $name ($why)"
    started=$(date +%s)
    # Its own time budget, its own result: one resolution that hangs or dies
    # must not stop the others from being tried. --kill-after turns a process
    # that ignores the TERM into a KILL ten seconds later.
    code=0
    timeout --kill-after=10s "${capture_timeout}s" "$view" \
      --width "$w" --height "$h" --frames "$capture_frames" --capture "$capture" \
      >"$out" 2>"$err" || code=$?
    seconds=$(( $(date +%s) - started ))
    summary="null"
    if [[ -f "$out" ]]; then
      line="$(grep -E '^[[:space:]]*\{' "$out" | tail -n 1 || true)"
      [[ -n "$line" ]] && summary="$line"
    fi
    exit_json="$code"
    case "$code" in
      0) state="ok" ;;
      # engine-view exits 3 when it cannot draw at all (no display, no Vulkan
      # device, no mesh shaders, no presentation), which is this machine every
      # time: a skip, not a failure.
      3) state="skipped" ;;
      124|137) state="timeout"; exit_json="null" ;;
      *) state="failed" ;;
    esac
    [[ "$state" == "ok" || "$state" == "skipped" ]] || capture_failures=$((capture_failures + 1))
    capture_path="null"
    [[ "$state" == "ok" && -f "$capture" ]] && capture_path="\"captures/$name.png\""
    [[ -n "$captures_json" ]] && captures_json+=","
    captures_json+="$(printf '\n    {"name": "%s", "why": "%s", "requested_width": %s, "requested_height": %s, "status": "%s", "exit_code": %s, "seconds": %s, "timeout_seconds": %s, "frames": %s, "capture": %s, "summary": %s}' \
      "$name" "$(json_escape "$why")" "$w" "$h" "$state" "$exit_json" "$seconds" \
      "$capture_timeout" "$capture_frames" "$capture_path" "$summary")"
    case "$state" in
      ok) echo "   ok in ${seconds}s" ;;
      skipped) echo "   skipped (engine-view exited 3: nothing to draw with)" ;;
      timeout) echo "gpu-smoke: warning: $name produced no result within ${capture_timeout}s; killed" >&2 ;;
      *) echo "gpu-smoke: warning: $name: engine-view exited $code after ${seconds}s; see $err" >&2 ;;
    esac
  done
fi

# One machine-readable file for the whole run, beside the ctest log: what the
# GPU is, what the tests did, and what came out of each resolution. The adapter
# report is embedded only when it really was JSON, since gpu.adapters is read
# with stderr merged in.
adapters_json="null"
case "$adapters_out" in
  '{'*) [[ "$adapters_status" -eq 0 ]] && adapters_json="$adapters_out" ;;
esac
report="$build_dir/gpu-smoke.json"
{
  printf '{\n'
  printf '  "machine": "%s",\n' "$(json_escape "$(hostname)")"
  printf '  "os": "%s",\n' "$(json_escape "$(uname -sr)")"
  printf '  "preset": "%s",\n' "$(json_escape "$preset")"
  printf '  "generated_utc": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '  "ctest_status": %s,\n' "$status"
  printf '  "adapters": %s,\n' "$adapters_json"
  printf '  "captures": [%s\n  ]\n' "$captures_json"
  printf '}\n'
} >"$report"

section "result"
if [[ -f "$log" ]]; then
  echo "ctest log: $log"
else
  echo "gpu-smoke: warning: no ctest log at $log" >&2
fi
echo "report:   $report"
echo "ctest exit status: $status"
if [[ "$status" -eq 0 && "$capture_failures" -gt 0 ]]; then
  echo "gpu-smoke: $capture_failures resolution(s) did not render; failing the run" >&2
  status=1
fi
exit "$status"
