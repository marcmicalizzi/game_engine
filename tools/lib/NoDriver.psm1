# A test run with no Vulkan driver to find and no GPU lock to ask for (`tools/dev.ps1 test
# -NoDriver`, and the off-configurations' pass under -SkipInvariantGpu; docs/ci/what-to-run.md,
# "The no-driver form"). Tested by tools/no-driver.Tests.ps1.
#
# Four variables, always together. The loader's three point its driver search at a file that does
# not exist and add none, so every GPU case skips as on a hosted runner with no GPU. The fourth is
# the engine's own switch at 0 (docs/subsystems/gpu_lock.md): the test main sets it to 1 only when
# it is empty, and a process that has it at 1 still asks for the machine-wide lock before a Vulkan
# window appears, driver or none, so a run that hides the driver by hand and leaves the switch
# alone stands in the GPU's line for every windowed test, half an hour a test (batch 68, T177).

# A process variable set, or removed when the value is null. Not [Environment]::SetEnvironmentVariable
# with $null: PowerShell hands a .NET string parameter "" for $null, which leaves the variable
# present and empty where it was absent.
function Set-ProcessVariable([string]$Name, $Value) {
  if ($null -eq $Value) { Remove-Item -LiteralPath "Env:$Name" -ErrorAction SilentlyContinue }
  else { [Environment]::SetEnvironmentVariable($Name, [string]$Value) }
}

# The four, as name and value; a null value removes the variable.
function Get-NoDriverEnvironment([string]$BuildDir) {
  $none = Join-Path $BuildDir 'no-vulkan-driver.json'
  [ordered]@{
    VK_DRIVER_FILES           = $none
    VK_ICD_FILENAMES          = $none
    VK_ADD_DRIVER_FILES       = $null
    ENGINE_GPU_LOCK_ON_DEVICE = '0'
  }
}

# Sets the four in this process, where every process it starts from now on inherits them, and
# returns what they were, for Exit-NoDriver. With -When:$false it sets nothing and returns $null, so
# a caller passes its switch straight through and calls Exit-NoDriver in a finally either way.
function Enter-NoDriver([string]$BuildDir, [bool]$When = $true) {
  if (-not $When) { return $null }
  $hide = Get-NoDriverEnvironment $BuildDir
  $saved = [ordered]@{}
  foreach ($name in $hide.Keys) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name)
    Set-ProcessVariable $name $hide[$name]
  }
  return $saved
}

# Puts back what Enter-NoDriver found (a variable that was absent is removed again); $null does
# nothing.
function Exit-NoDriver($Saved) {
  if ($null -eq $Saved) { return }
  foreach ($name in $Saved.Keys) { Set-ProcessVariable $name $Saved[$name] }
}

Export-ModuleMember -Function Get-NoDriverEnvironment, Enter-NoDriver, Exit-NoDriver
