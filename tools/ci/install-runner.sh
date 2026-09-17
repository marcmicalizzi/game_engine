#!/usr/bin/env bash
#
# Install a GitHub Actions self-hosted runner on Linux and register it as a
# systemd service.
#
#   tools/ci/install-runner.sh --url <repo url> [--token <t>] [--labels <a,b>]
#                              [--name <n>] [--dir <path>] [--version <x.y.z>]
#                              [--sha256 <hex>] [--runner-group <g>]
#                              [--svc-user <user>] [--install-deps] [--force]
#
# The script downloads the runner release for this architecture (the latest
# unless --version says otherwise), verifies the SHA-256 that GitHub publishes in
# the release notes, unpacks it into --dir, configures it unattended with the
# labels, and installs and starts it as a systemd service through
# `./svc.sh install` and `./svc.sh start`.
#
# The registration token comes from the repository's
# Settings > Actions > Runners > New self-hosted runner page. It expires about an
# hour after it is issued and must never be committed or pasted into a file in
# this repository. Pass it with --token or in the RUNNER_TOKEN environment
# variable; this script never prints it, though it does hand it to config.sh on a
# command line, where it is briefly visible to anyone who can read /proc on this
# machine.
#
# Run it as the unprivileged user the runner should work as, not as root:
# config.sh refuses to run as root, and svc.sh is called through sudo.
# Re-running it with the same version and an already configured runner does
# nothing but make sure the service is running.
#
# See docs/ci/self-hosted-runners.md for what the two machines run and why.

set -euo pipefail

repo_url=""
token="${RUNNER_TOKEN:-}"
labels="gpu,pascal,headless"
name="$(hostname -s 2>/dev/null || hostname)"
install_dir="$HOME/actions-runner"
version=""
sha256=""
runner_group=""
svc_user="$(id -un)"
work="_work"
install_deps=0
force=0

version_marker=".engine-runner-version"

usage() {
  cat <<'EOF'
tools/ci/install-runner.sh --url <repo url> [options]

  --url            repository URL, e.g. https://github.com/<owner>/<repo> (required)
  --token          registration token; defaults to $RUNNER_TOKEN (required, never committed)
  --labels         extra labels (default: gpu,pascal,headless; self-hosted, Linux
                   and X64 are added by GitHub)
  --name           runner name (default: this host's short name)
  --dir            install directory (default: $HOME/actions-runner)
  --version        runner version to install (default: the latest release)
  --sha256         expected archive checksum, when the release notes do not carry one
  --runner-group   runner group, for organization runners
  --svc-user       user the systemd service runs as (default: the current user)
  --work           runner work directory, relative to --dir (default: _work)
  --install-deps   run the runner's bin/installdependencies.sh first
  --force          re-unpack and re-register even if already configured
EOF
}

die() { echo "install-runner: $*" >&2; exit 1; }
section() { printf '\n== %s\n' "$1"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --url) repo_url="${2:?--url needs a value}"; shift 2 ;;
    --token) token="${2:?--token needs a value}"; shift 2 ;;
    --labels) labels="${2:?--labels needs a value}"; shift 2 ;;
    --name) name="${2:?--name needs a value}"; shift 2 ;;
    --dir) install_dir="${2:?--dir needs a value}"; shift 2 ;;
    --version) version="${2:?--version needs a value}"; shift 2 ;;
    --sha256) sha256="${2:?--sha256 needs a value}"; shift 2 ;;
    --runner-group) runner_group="${2:?--runner-group needs a value}"; shift 2 ;;
    --svc-user) svc_user="${2:?--svc-user needs a value}"; shift 2 ;;
    --work) work="${2:?--work needs a value}"; shift 2 ;;
    --install-deps) install_deps=1; shift ;;
    --force) force=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "install-runner: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$repo_url" ]] || { usage >&2; die "--url is required."; }
[[ "$repo_url" =~ ^https://[^/]+/[^/]+/[^/]+/?$ ]] || \
  die "--url should be the repository URL, for example https://github.com/<owner>/<repo>; got '$repo_url'."
[[ -n "$token" ]] || die "a registration token is required. Get one from Settings > Actions > Runners > New self-hosted runner and pass --token or set RUNNER_TOKEN. It expires in about an hour; never commit it."
[[ "$(id -u)" -ne 0 ]] || die "do not run this as root: config.sh refuses to, and svc.sh is called through sudo."

for tool in curl tar sha256sum sudo; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool is required but not on PATH."
done

case "$(uname -m)" in
  x86_64|amd64) platform="linux-x64" ;;
  aarch64|arm64) platform="linux-arm64" ;;
  *) die "unsupported architecture $(uname -m); GitHub publishes linux-x64 and linux-arm64 runners." ;;
esac

section "GitHub Actions runner for $repo_url"
echo "name:      $name"
echo "labels:    $labels (self-hosted, Linux and X64 are added by GitHub)"
echo "directory: $install_dir"
echo "platform:  $platform"

section "release"
api_headers=(-H "Accept: application/vnd.github+json" -H "User-Agent: engine-install-runner")
# Anonymous API calls are rate limited per address; a token raises the limit.
api_token="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
if [[ -n "$api_token" ]]; then
  api_headers+=(-H "Authorization: Bearer $api_token")
fi
if [[ -n "$version" ]]; then
  api_url="https://api.github.com/repos/actions/runner/releases/tags/v${version#v}"
else
  api_url="https://api.github.com/repos/actions/runner/releases/latest"
fi
release_json="$(curl -fsSL "${api_headers[@]}" "$api_url")" || \
  die "could not read $api_url from the GitHub releases API."

runner_version="$(printf '%s' "$release_json" | tr ',' '\n' | grep -m 1 '"tag_name"' |
  sed -e 's/.*"tag_name"[[:space:]]*:[[:space:]]*"//' -e 's/".*$//')"
runner_version="${runner_version#v}"
[[ -n "$runner_version" ]] || die "the releases API returned no tag_name."

asset="actions-runner-${platform}-${runner_version}.tar.gz"
asset_url="https://github.com/actions/runner/releases/download/v${runner_version}/${asset}"
echo "version:   $runner_version"
echo "asset:     $asset"

expected_sha="$sha256"
if [[ -z "$expected_sha" ]]; then
  # The release notes carry one line per platform:
  #   <!-- BEGIN SHA linux-x64 --><64 hex chars><!-- END SHA linux-x64 -->
  expected_sha="$(printf '%s' "$release_json" |
    sed -n "s/.*BEGIN SHA ${platform}[^>]*>[^0-9a-f]*\([0-9a-f]\{64\}\).*/\1/p" | head -n 1)"
fi
if [[ -n "$expected_sha" ]]; then
  echo "sha256:    $expected_sha"
else
  echo "install-runner: warning: the release notes for v$runner_version publish no $platform SHA-256; the download cannot be verified. Pass --sha256 with the checksum from the release page to check it." >&2
fi

mkdir -p "$install_dir"
marker_path="$install_dir/$version_marker"
unpacked=0
if [[ -f "$install_dir/config.sh" && -f "$marker_path" && "$(cat "$marker_path")" == "$runner_version" ]]; then
  unpacked=1
fi

if [[ "$unpacked" -eq 1 && "$force" -eq 0 ]]; then
  echo "runner $runner_version is already unpacked in $install_dir"
else
  section "download"
  tmp_dir="$(mktemp -d)"
  trap 'rm -rf "$tmp_dir"' EXIT
  echo "from $asset_url"
  curl -fSL --retry 3 -o "$tmp_dir/$asset" "$asset_url" || die "downloading $asset_url failed."

  if [[ -n "$expected_sha" ]]; then
    printf '%s  %s\n' "$expected_sha" "$tmp_dir/$asset" > "$tmp_dir/checksum"
    sha256sum -c "$tmp_dir/checksum" >/dev/null || \
      die "checksum mismatch for $asset; the download was discarded."
    echo "checksum verified"
  fi

  section "unpack"
  tar -xzf "$tmp_dir/$asset" -C "$install_dir"
  printf '%s' "$runner_version" > "$marker_path"
  echo "unpacked into $install_dir"
fi

[[ -f "$install_dir/config.sh" ]] || die "config.sh is missing from $install_dir; the archive did not unpack as expected."

if [[ "$install_deps" -eq 1 ]]; then
  section "dependencies"
  sudo "$install_dir/bin/installdependencies.sh"
fi

section "register"
already_configured=0
if [[ -f "$install_dir/.runner" ]]; then
  already_configured=1
fi

if [[ "$already_configured" -eq 1 && "$force" -eq 0 ]]; then
  echo "this runner is already configured; leaving the registration alone"
  echo "to re-register: sudo ./svc.sh uninstall && ./config.sh remove --token <removal token>, then run this script again"
else
  if [[ "$already_configured" -eq 1 ]]; then
    echo "removing the existing registration (--force)"
    if [[ -f "$install_dir/.service" ]]; then
      ( cd "$install_dir" && sudo ./svc.sh uninstall ) || die "svc.sh uninstall failed."
    fi
    ( cd "$install_dir" && ./config.sh remove --token "$token" ) || \
      die "config.sh remove failed. Removal needs a removal token from the runner's page on GitHub, which is not the registration token."
  fi

  config_args=(--unattended --replace
               --url "$repo_url" --token "$token"
               --name "$name" --labels "$labels" --work "$work")
  if [[ -n "$runner_group" ]]; then
    config_args+=(--runnergroup "$runner_group")
  fi
  echo "config.sh --unattended --replace --url $repo_url --token *** --name $name --labels $labels --work $work"
  ( cd "$install_dir" && ./config.sh "${config_args[@]}" ) || \
    die "config.sh failed. A token older than an hour, a wrong repository URL, or a name already taken by another runner are the usual causes."
fi

section "service"
if [[ -f "$install_dir/.service" ]]; then
  echo "service $(cat "$install_dir/.service") is already installed"
else
  ( cd "$install_dir" && sudo ./svc.sh install "$svc_user" ) || die "svc.sh install failed."
fi
( cd "$install_dir" && sudo ./svc.sh start ) || die "svc.sh start failed."
( cd "$install_dir" && sudo ./svc.sh status ) || true

echo ""
echo "The runner is registered. It should appear as Idle under Settings > Actions > Runners."
echo "Diagnostics: $install_dir/_diag. Remove it with: cd $install_dir && sudo ./svc.sh uninstall && ./config.sh remove --token <removal token>"
