#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Install a GitHub Actions self-hosted runner on Windows and register it as a service.

.DESCRIPTION
  tools/ci/install-runner.ps1 -RepoUrl <url> [-Token <t>] [-Labels <a,b>] [-Name <n>]
                              [-InstallDir <path>] [-Version <x.y.z>] [-Sha256 <hex>]
                              [-RunnerGroup <g>] [-WindowsLogonAccount <acct>] [-Force]

  The script downloads the runner release for win-x64 (the latest unless -Version
  says otherwise), verifies the SHA-256 that GitHub publishes in the release
  notes, unpacks it into -InstallDir, configures it unattended with the labels,
  and installs it as a Windows service through `config.cmd --runasservice`
  (`svc.cmd` only manages a service that already exists, and there is no `./svc`
  on Windows).

  The registration token comes from the repository's
  Settings > Actions > Runners > New self-hosted runner page. It expires about an
  hour after it is issued and must never be committed or pasted into a file in
  this repository. Pass it with -Token or in the RUNNER_TOKEN environment
  variable; this script never prints it, though it does hand it to config.cmd on
  a command line, where it is briefly visible to anyone who can read the process
  table of this machine.

  Registering a service needs administrator rights, so run this from an elevated
  PowerShell 7 prompt. Re-running it with the same version and an already
  configured runner does nothing but make sure the service is running.

  See docs/ci/self-hosted-runners.md for what the two machines run and why.

.EXAMPLE
  $env:RUNNER_TOKEN = '<token from the runners page>'
  tools/ci/install-runner.ps1 -RepoUrl https://github.com/<owner>/<repo> -Labels gpu,maxwell
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$RepoUrl,
  [string]$Token = $env:RUNNER_TOKEN,
  [string]$Labels = 'gpu,maxwell',
  [string]$Name = [Environment]::MachineName.ToLowerInvariant(),
  [string]$InstallDir = 'C:\actions-runner',
  [string]$Version,
  [string]$Sha256,
  [string]$RunnerGroup,
  [string]$WindowsLogonAccount,
  [string]$Work = '_work',
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
# Every native call below checks its own exit code and reports it in context.
$PSNativeCommandUseErrorActionPreference = $false

$Platform = 'win-x64'
$VersionMarker = '.engine-runner-version'

function Write-Section([string]$Text) {
  Write-Host ''
  Write-Host "== $Text" -ForegroundColor Cyan
}

function Test-Administrator {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  return ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-RunnerRelease([string]$Wanted) {
  $uri = if ($Wanted) {
    "https://api.github.com/repos/actions/runner/releases/tags/v$($Wanted.TrimStart('v'))"
  } else {
    'https://api.github.com/repos/actions/runner/releases/latest'
  }
  $headers = @{
    'User-Agent' = 'engine-install-runner'
    'Accept'     = 'application/vnd.github+json'
  }
  # Anonymous API calls are rate limited per address; a token raises the limit.
  $apiToken = if ($env:GH_TOKEN) { $env:GH_TOKEN } else { $env:GITHUB_TOKEN }
  if ($apiToken) { $headers['Authorization'] = "Bearer $apiToken" }
  try { return Invoke-RestMethod -Uri $uri -Headers $headers }
  catch { throw "could not read $uri from the GitHub releases API: $($_.Exception.Message)" }
}

if (-not $Token) {
  throw 'A registration token is required. Get one from Settings > Actions > Runners > New self-hosted runner and pass -Token or set RUNNER_TOKEN. It expires in about an hour; never commit it.'
}
if ($RepoUrl -notmatch '^https://[^/]+/[^/]+/[^/]+/?$') {
  throw "RepoUrl should be the repository URL, for example https://github.com/<owner>/<repo>; got '$RepoUrl'."
}
if (-not (Test-Administrator)) {
  throw 'Run this from an elevated PowerShell 7 prompt: installing the runner as a Windows service needs administrator rights.'
}

Write-Section "GitHub Actions runner for $RepoUrl"
Write-Host "name:      $Name"
Write-Host "labels:    $Labels (self-hosted, Windows, and X64 are added by GitHub)"
Write-Host "directory: $InstallDir"

Write-Section 'release'
$release = Get-RunnerRelease $Version
$runnerVersion = "$($release.tag_name)".TrimStart('v')
if (-not $runnerVersion) { throw 'the releases API returned no tag_name.' }
$asset = "actions-runner-$Platform-$runnerVersion.zip"
$assetUrl = "https://github.com/actions/runner/releases/download/v$runnerVersion/$asset"
Write-Host "version:   $runnerVersion"
Write-Host "asset:     $asset"

$expectedSha = $Sha256
if (-not $expectedSha -and $release.body -match "BEGIN SHA $Platform\s*-->\s*([0-9a-fA-F]{64})") {
  $expectedSha = $Matches[1]
}
if ($expectedSha) { Write-Host "sha256:    $($expectedSha.ToLowerInvariant())" }
else { Write-Warning "the release notes for v$runnerVersion publish no $Platform SHA-256; the download cannot be verified. Pass -Sha256 with the checksum from the release page to check it." }

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$markerPath = Join-Path $InstallDir $VersionMarker
$unpacked = (Test-Path (Join-Path $InstallDir 'config.cmd')) -and
            (Test-Path $markerPath) -and
            ((Get-Content -LiteralPath $markerPath -Raw).Trim() -eq $runnerVersion)

if ($unpacked -and -not $Force) {
  Write-Host "runner $runnerVersion is already unpacked in $InstallDir"
}
else {
  Write-Section 'download'
  $zipPath = Join-Path ([IO.Path]::GetTempPath()) $asset
  if (Test-Path $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
  Write-Host "from $assetUrl"
  $progress = $ProgressPreference
  $ProgressPreference = 'SilentlyContinue'   # a progress bar in CI output helps nobody
  try { Invoke-WebRequest -Uri $assetUrl -OutFile $zipPath -UseBasicParsing }
  catch { throw "downloading $assetUrl failed: $($_.Exception.Message)" }
  finally { $ProgressPreference = $progress }

  if ($expectedSha) {
    $actual = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    if ($actual -ne $expectedSha.ToUpperInvariant()) {
      Remove-Item -LiteralPath $zipPath -Force
      throw "checksum mismatch for ${asset}: expected $($expectedSha.ToLowerInvariant()), got $($actual.ToLowerInvariant()). The download was deleted."
    }
    Write-Host 'checksum verified'
  }

  Write-Section 'unpack'
  Expand-Archive -LiteralPath $zipPath -DestinationPath $InstallDir -Force
  Remove-Item -LiteralPath $zipPath -Force
  Set-Content -LiteralPath $markerPath -Value $runnerVersion -NoNewline
  Write-Host "unpacked into $InstallDir"
}

$configCmd = Join-Path $InstallDir 'config.cmd'
if (-not (Test-Path $configCmd)) { throw "config.cmd is missing from $InstallDir; the archive did not unpack as expected." }

Write-Section 'register'
$alreadyConfigured = Test-Path (Join-Path $InstallDir '.runner')
if ($alreadyConfigured -and -not $Force) {
  Write-Host 'this runner is already configured; leaving the registration alone'
  Write-Host 'to re-register: config.cmd remove --token <removal token>, then run this script again'
}
else {
  if ($alreadyConfigured) {
    Write-Host 'removing the existing registration (-Force)'
    Push-Location $InstallDir
    try {
      & $configCmd remove --token $Token
      $removeStatus = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($removeStatus -ne 0) {
      throw "config.cmd remove failed ($removeStatus). Removal needs a removal token from the runner's page on GitHub, which is not the registration token."
    }
  }

  $configArgs = @(
    '--url', $RepoUrl,
    '--token', $Token,
    '--name', $Name,
    '--labels', $Labels,
    '--work', $Work,
    '--replace',
    '--runasservice'
  )
  if ($RunnerGroup) { $configArgs += @('--runnergroup', $RunnerGroup) }
  if ($WindowsLogonAccount) {
    # config.cmd asks for that account's password itself; this script never
    # handles it, so the run cannot be unattended.
    $configArgs += @('--windowslogonaccount', $WindowsLogonAccount)
    Write-Host "the service will run as $WindowsLogonAccount; config.cmd will ask for its password"
  }
  else {
    $configArgs += '--unattended'
  }
  Write-Host ("config.cmd " + (($configArgs | ForEach-Object { if ($_ -eq $Token) { '***' } else { $_ } }) -join ' '))

  Push-Location $InstallDir
  try {
    & $configCmd @configArgs
    $configStatus = $LASTEXITCODE
  }
  finally { Pop-Location }
  if ($configStatus -ne 0) {
    throw "config.cmd failed ($configStatus). A token older than an hour, a wrong repository URL, or a name already taken by another runner are the usual causes."
  }
}

Write-Section 'service'
# config.cmd --runasservice writes the service name it created into .service;
# fall back to a search when an older runner layout did not.
$service = $null
$serviceFile = Join-Path $InstallDir '.service'
if (Test-Path $serviceFile) {
  $serviceName = (Get-Content -LiteralPath $serviceFile -Raw).Trim()
  $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
}
if (-not $service) {
  $candidates = @(Get-Service -Name 'actions.runner.*' -ErrorAction SilentlyContinue | Sort-Object -Property Name)
  $service = $candidates | Where-Object { $_.Name -like "*.$Name" } | Select-Object -First 1
  if (-not $service) { $service = $candidates | Select-Object -First 1 }
}
if (-not $service) {
  throw "no actions.runner.* service exists after configuration; check $InstallDir\_diag for the runner's own logs."
}
if ($service.Status -ne 'Running') { Start-Service -Name $service.Name }
$service.Refresh()
Write-Host "$($service.Name): $($service.Status)"
Write-Host ''
Write-Host "The runner is registered. It should appear as Idle under Settings > Actions > Runners."
Write-Host "Diagnostics: $InstallDir\_diag. Remove it with: $configCmd remove --token <removal token>"
