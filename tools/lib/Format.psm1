# Which clang-format `tools/dev.ps1 format` runs, and the one it refuses (roadmap T184;
# docs/ci/structure-and-citations.md, "The formatter is pinned").
#
# Two majors of clang-format lay the same file out differently, so a format run with another
# version than the tree's rewraps files nobody touched, and a commit carries them. The pin is the
# major only: a minor or a patch release has not changed this tree's output in the cases measured.

# The major the tree is formatted with: Visual Studio 2026's own (VC\Tools\Llvm\x64\bin, 22.1.3 on
# 2026-11-18), which is what an agent's rows format their own files with.
$script:ClangFormatMajor = 22

function Get-ClangFormatMajor { return $script:ClangFormatMajor }

# The verdict on a `clang-format --version` line: Ok, the Major read, and when not Ok one line that
# says what was found and which to install.
function Get-ClangFormatVerdict {
  [CmdletBinding()]
  param([AllowEmptyString()][string]$VersionText, [string]$Path = 'clang-format')
  $want = $script:ClangFormatMajor
  $install = "use LLVM $want (Visual Studio 2026's VC\Tools\Llvm\x64\bin\clang-format.exe is $want; or https://github.com/llvm/llvm-project/releases), first on PATH"
  if ($VersionText -notmatch 'clang-format version (\d+)\.(\d+)\.(\d+)') {
    return [pscustomobject]@{ Ok = $false; Major = 0; Message = "format: $Path did not say which clang-format it is ('$VersionText'); $install" }
  }
  $major = [int]$Matches[1]
  if ($major -ne $want) {
    return [pscustomobject]@{ Ok = $false; Major = $major
      Message = "format: $Path is clang-format $($Matches[1]).$($Matches[2]).$($Matches[3]) and the tree is formatted with $want, which lays files out differently; $install" }
  }
  return [pscustomobject]@{ Ok = $true; Major = $major; Message = '' }
}

Export-ModuleMember -Function Get-ClangFormatMajor, Get-ClangFormatVerdict
