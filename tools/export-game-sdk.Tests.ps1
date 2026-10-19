#requires -Version 7
<#
.SYNOPSIS
  The game author's kit, end to end (roadmap P42; docs/game-authoring.md). Runs under CTest as
  `tools.game_sdk`.

.DESCRIPTION
  From the build it runs in: export an SDK into the test's own scratch directory
  (tools/export-game-sdk.ps1), instantiate the repository template from it
  (tools/new-game-repo.ps1), build the template's module through the repository's build.ps1 (the
  SDK's build-module.ps1 over the relocatable config), run it offscreen for 20 frames through
  run.ps1 (the SDK's run-game.ps1 replaying the synthetic walk, with --report), check the report
  has `exit`, `machine`, `frames` and a `game` block whose `ticks` is positive, then pack it with
  the SDK's package-game.ps1 (-ModuleDll on that module, the SDK's desert scene) and check the zip
  holds package.json, run.cmd and the module. The refusals are checked too: an export inside the
  repository, a template name that is not an identifier.

  It owns nothing outside its scratch directory — a fresh GUID under the system temp directory, so
  two copies (another worktree, CI beside a developer) never share one — and removes it, a few
  tens of megabytes, unless it fails or -KeepTemp says to keep it. It skips, with a sentence and
  exit 0, when the build has no game_modules/build_config.cmake (no hotload capability: the
  minimal preset builds no module and exports no command) and off Windows (an exported SDK does
  not cover Linux yet). engine-view takes the machine-wide GPU lock itself; when it gave up
  waiting (exit 75) this script exits 75 too, which CTest reads as skipped, and when the machine
  has no usable device (exit 3) it says so and passes, as the C++ end-to-end cases do.

      pwsh tools/export-game-sdk.Tests.ps1 -BuildDir build/msvc-debug [-KeepTemp]
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$BuildDir,
  [switch]$KeepTemp
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')

if (-not $IsWin) {
  Write-Host 'tools.game_sdk: skipped; an exported game SDK covers Windows only (docs/game-authoring.md)'
  exit 0
}
$config = Join-Path $BuildDir 'game_modules/build_config.cmake'
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
  Write-Host "tools.game_sdk: skipped; $BuildDir was configured without the hotload capability, so it builds no game module and exports no SDK"
  exit 0
}
$preset = Split-Path $BuildDir -Leaf

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
function Check([bool]$ok, [string]$what) {
  $script:checks++
  if (-not $ok) { $failures.Add($what); Write-Host "  FAIL  $what" -ForegroundColor Red } else { Write-Host "  ok    $what" }
}
function Run([string]$script, [string[]]$arguments) {
  $output = & pwsh -NoProfile -ExecutionPolicy Bypass -File $script @arguments 2>&1 | Out-String -Width 400
  return [pscustomobject]@{ Code = $LASTEXITCODE; Output = $output }
}

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("engine-game-sdk-test-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$started = Get-Date
$sdk = Join-Path $scratch 'sdk'
$repo = Join-Path $scratch 'game'
$name = 'sdk_test'

try {
  # ---- refusals that need no build ------------------------------------------------------------
  $inside = Run (Join-Path $Root 'tools/export-game-sdk.ps1') @('-BuildDir', $BuildDir, '-Out', (Join-Path $Root 'build/never-written-sdk'))
  Check ($inside.Code -eq 2 -and $inside.Output -match 'inside the repository') 'an export inside the repository is refused with exit 2'
  Check (-not (Test-Path -LiteralPath (Join-Path $Root 'build/never-written-sdk'))) 'and nothing was written there'

  # ---- export -----------------------------------------------------------------------------------
  $t = Get-Date
  $export = Run (Join-Path $Root 'tools/export-game-sdk.ps1') @('-Preset', $preset, '-BuildDir', $BuildDir, '-Out', $sdk)
  Check ($export.Code -eq 0) "export-game-sdk exits 0 ($([int]((Get-Date) - $t).TotalSeconds) s): $($export.Output.Substring(0, [Math]::Min(300, $export.Output.Length)))"
  foreach ($rel in @('sdk.json', 'README.md', 'LICENSE', 'bin/engine-view.exe', 'bin/schemac.exe', 'bin/game/walk_poc.dll',
                     'cmake/build_config.cmake', 'cmake/EngineGameModuleBuild.cmake',
                     'include/foundation/hotload/module_abi.h', 'include/core/base/types.h', 'schemas/README.md',
                     'content/test-scenes/desert-endless/scene.json', 'content/test-scenes/ruins-walk/kit/kit.json',
                     'content/sessions/walk-synthetic.jsonl', 'tools/build-module.ps1', 'tools/run-game.ps1',
                     'tools/package-game.ps1', 'tools/lib/GameContent.psm1')) {
    Check (Test-Path -LiteralPath (Join-Path $sdk $rel) -PathType Leaf) "the SDK has $rel"
  }
  Check (-not (Test-Path -LiteralPath (Join-Path $sdk 'include/foundation/hotload/src'))) 'and nothing from a src/ directory'
  # The action map, as a file, and the synthetic walk recorded against it (roadmap P47): the hash is
  # compared as the digits both files hold, never through a JSON number.
  $mapPath = Join-Path $sdk 'content/input-map.json'
  Check (Test-Path -LiteralPath $mapPath -PathType Leaf) 'the SDK has content/input-map.json'
  if (Test-Path -LiteralPath $mapPath) {
    $mapText = Get-Content -LiteralPath $mapPath -Raw
    $mapHash = if ($mapText -match '"hash":\s*(\d+)') { $Matches[1] } else { '' }
    $walkHeader = Get-Content -LiteralPath (Join-Path $sdk 'content/sessions/walk-synthetic.jsonl') -TotalCount 1
    $walkHash = if ($walkHeader -match '"map":(\d+)') { $Matches[1] } else { '' }
    Check ($mapHash -ne '' -and $mapHash -eq $walkHash) "the synthetic walk names the exported map's hash ($walkHash, the map's $mapHash)"
    Check ($mapText -match '"name":\s*"interact"') 'and the exported map has the interact action'
  }
  $sdkJson = Get-Content -LiteralPath (Join-Path $sdk 'sdk.json') -Raw | ConvertFrom-Json -Depth 16
  Check ($sdkJson.format -eq 'engine.game.sdk' -and $sdkJson.commit.Length -ge 7 -and $sdkJson.abi_version -ge 2 -and $sdkJson.preset -eq $preset) 'sdk.json names the commit, the ABI and the preset'
  $relocatable = Get-Content -LiteralPath (Join-Path $sdk 'cmake/build_config.cmake') -Raw
  Check ($relocatable -match 'GM_SDK_ROOT' -and $relocatable -notmatch [regex]::Escape($Root.Replace('\', '/'))) 'the relocatable config names no path into the tree'

  # ---- the template -----------------------------------------------------------------------------
  $bad = Run (Join-Path $Root 'tools/new-game-repo.ps1') @('-Name', '9lives', '-Out', (Join-Path $scratch 'bad'), '-Sdk', $sdk)
  Check ($bad.Code -eq 2 -and $bad.Output -match 'not a C identifier') 'a name that is not a C identifier is refused'
  $new = Run (Join-Path $Root 'tools/new-game-repo.ps1') @('-Name', $name, '-Out', $repo, '-Sdk', $sdk, '-RelativeSdk')
  Check ($new.Code -eq 0) "new-game-repo exits 0: $($new.Output.Substring(0, [Math]::Min(200, $new.Output.Length)))"
  foreach ($rel in @('README.md', 'LICENSE.md', '.gitignore', 'build.ps1', 'run.ps1', 'package.ps1', 'engine-sdk.json', "src/$name.cpp", "src/$name.schema")) {
    Check (Test-Path -LiteralPath (Join-Path $repo $rel) -PathType Leaf) "the repository has $rel"
  }
  $pin = Get-Content -LiteralPath (Join-Path $repo 'engine-sdk.json') -Raw | ConvertFrom-Json -Depth 8
  Check ($pin.commit -eq $sdkJson.commit -and $pin.sdk -eq '../sdk') 'engine-sdk.json pins the SDK and its commit'
  Check ((Select-String -LiteralPath (Join-Path $repo "src/$name.cpp") -Pattern '@@NAME@@' -Quiet) -ne $true) 'no placeholder is left in the module'

  # ---- build, run offscreen, read the report ----------------------------------------------------
  $t = Get-Date
  $build = Run (Join-Path $repo 'build.ps1') @()
  Check ($build.Code -eq 0 -and (Test-Path -LiteralPath (Join-Path $repo "build/$name.dll"))) "build.ps1 builds the module ($([int]((Get-Date) - $t).TotalSeconds) s): $(($build.Output -split "`n" | Where-Object { $_ -match 'error|built' }) -join ' | ')"
  if ($build.Code -eq 0) {
    $report = Join-Path $scratch 'report.json'
    $t = Get-Date
    $run = Run (Join-Path $repo 'run.ps1') @('-Offscreen', '-Frames', '20', '-Report', $report, '-EngineArgs', '--width 160 --height 96')
    if ($run.Code -eq 75) {
      Write-Host 'tools.game_sdk: engine-view gave up waiting for the GPU lock; not run'
      exit 75
    }
    if ($run.Code -eq 3) {
      Write-Host "tools.game_sdk: engine-view unavailable here (no device): $(($run.Output -split "`n" | Where-Object { $_ -match '^engine-view' }) -join ' | ')"
    }
    else {
      Check ($run.Code -eq 0) "run.ps1 -Offscreen exits 0 ($([int]((Get-Date) - $t).TotalSeconds) s): $(($run.Output -split "`n" | Where-Object { $_ -match '^engine-view' }) -join ' | ')"
      Check (Test-Path -LiteralPath $report -PathType Leaf) 'the report was written'
      if (Test-Path -LiteralPath $report) {
        $r = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json -Depth 32
        Check ($null -ne $r.exit -and $r.exit.code -eq 0) "the report's exit is 0 ($($r.exit.reason))"
        Check ($null -ne $r.machine -and $r.machine.adapter.Length -gt 0) "the report names the machine ($($r.machine.adapter))"
        Check ($null -ne $r.frames -and $r.frames.count -eq 20) "the report counts 20 frames ($($r.frames.count))"
        Check ($null -ne $r.game -and $r.game.ticks -gt 0 -and $r.game.name -eq $name) "the game block names $name, ticked $($r.game.ticks) times"
        Check ($null -ne $r.game.hud -and $r.game.hud.lines.Count -ge 1 -and $r.game.hud.meters.Count -ge 1) "the HUD said a line and a meter: $($r.game.hud.lines -join ' / ')"
        # E at tick 40 reaches the module through the exported map's `interact`; with the first
        # revision's map, which the walk was recorded against until 2026-10-18, it never did.
        Check ($null -ne $r.game.state -and $r.game.state.interactions -eq 1) "the walk's E press reached the module ($($r.game.state.interactions) interactions)"
        Check ($null -ne $r.game.hud_elements -and @($r.game.hud_elements).Count -ge 2) "the report lists the HUD's elements ($(@($r.game.hud_elements).Count))"
      }
      Check (Test-Path -LiteralPath (Join-Path $repo 'ddc') -PathType Container) "the run's cache went to the repository's ddc/"

      # ---- package ------------------------------------------------------------------------------
      $zip = Join-Path $scratch 'package.zip'
      $t = Get-Date
      $pack = Run (Join-Path $sdk 'tools/package-game.ps1') @('-ModuleDll', (Join-Path $repo "build/$name.dll"), '-Scene', (Join-Path $sdk 'content/test-scenes/desert-endless/scene.json'), '-Out', $zip)
      Check ($pack.Code -eq 0 -and (Test-Path -LiteralPath $zip)) "package-game.ps1 from the SDK exits 0 ($([int]((Get-Date) - $t).TotalSeconds) s)"
      if (Test-Path -LiteralPath $zip) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
        try { $entries = @($archive.Entries | ForEach-Object { $_.FullName }) } finally { $archive.Dispose() }
        $top = ($entries[0] -split '/')[0]
        Check ($top -match "^$name-$preset-") "the zip's folder is named $name-$preset-<commit> ($top)"
        foreach ($rel in @('package.json', 'run.cmd', 'README.txt', 'engine-view.exe', "game/$name.dll", 'content/test-scenes/desert-endless/scene.json')) {
          Check ($entries -contains "$top/$rel") "the zip holds $rel"
        }
      }
      # A scene outside the SDK is a game repository's own: carried under content/<its directory>.
      $own = Join-Path $repo 'scenes/start'
      New-Item -ItemType Directory -Force -Path $own | Out-Null
      Copy-Item -LiteralPath (Join-Path $sdk 'content/test-scenes/desert-endless/scene.json') -Destination $own
      Copy-Item -LiteralPath (Join-Path $sdk 'content/test-scenes/desert-endless/camera-path.json') -Destination $own
      $zip2 = Join-Path $scratch 'package-own.zip'
      $pack2 = Run (Join-Path $sdk 'tools/package-game.ps1') @('-ModuleDll', (Join-Path $repo "build/$name.dll"), '-Scene', (Join-Path $own 'scene.json'), '-Out', $zip2)
      Check ($pack2.Code -eq 0 -and (Test-Path -LiteralPath $zip2)) "a scene of the repository's own is packaged from the SDK"
      if (Test-Path -LiteralPath $zip2) {
        $archive = [System.IO.Compression.ZipFile]::OpenRead($zip2)
        try { $entries2 = @($archive.Entries | ForEach-Object { $_.FullName }) } finally { $archive.Dispose() }
        $top2 = ($entries2[0] -split '/')[0]
        Check ($entries2 -contains "$top2/content/start/scene.json" -and $entries2 -contains "$top2/content/start/camera-path.json") 'carried under content/<its directory>/ with the file it names'
      }
    }
  }
}
catch {
  $failures.Add("exception: $($_.Exception.Message) at $($_.ScriptStackTrace)")
}

$seconds = [int]((Get-Date) - $started).TotalSeconds
$sdkBytes = 0
if (Test-Path -LiteralPath $sdk) { $sdkBytes = (Get-ChildItem -LiteralPath $sdk -Recurse -File | Measure-Object Length -Sum).Sum }
if ($failures.Count -gt 0) {
  Write-Host ("tools.game_sdk: {0} of {1} checks failed in {2} s; the scratch directory is kept at {3}" -f $failures.Count, $checks, $seconds, $scratch) -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
  exit 1
}
if ($KeepTemp) { Write-Host "kept: $scratch" }
else { Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host ("tools.game_sdk: {0} checks passed in {1} s (the SDK was {2:N1} MB)" -f $checks, $seconds, ($sdkBytes / 1MB))
exit 0
