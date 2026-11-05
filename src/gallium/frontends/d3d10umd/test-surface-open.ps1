# Host test of the BD-296 open rule (SurfaceOpen.h). Builds SurfaceOpenTest.cpp with clang-cl twice:
#   as it is            must print PASS and exit 0
#   negative control    -DBC250_CONTROL_OLD_RULE, the rule before BD-296: must exit non-zero
#   log control         -DBC250_CONTROL_OLD_LOG, one Fail line for every open: must exit non-zero
# -ClangCl is the clang-cl.exe to use (the compiler policy: clang-cl, named explicitly, no fallback to cl.exe).
param([Parameter(Mandatory=$true)][string]$Out, [Parameter(Mandatory=$true)][string]$ClangCl)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $ClangCl)) { throw "clang-cl not found: $ClangCl" }
New-Item -ItemType Directory -Force $Out | Out-Null
Push-Location $Out
try {
  & $ClangCl --version | Select-Object -First 1
  & $ClangCl /nologo /std:c++17 /O2 /W4 /WX /EHsc "/Fe:surface-open-test.exe" "$PSScriptRoot/SurfaceOpenTest.cpp"
  if ($LASTEXITCODE) { throw 'surface-open test build failed' }
  & .\surface-open-test.exe
  if ($LASTEXITCODE) { throw 'surface-open test failed' }
  & $ClangCl /nologo /std:c++17 /O2 /W4 /WX /EHsc /DBC250_CONTROL_OLD_RULE "/Fe:surface-open-control.exe" "$PSScriptRoot/SurfaceOpenTest.cpp"
  if ($LASTEXITCODE) { throw 'surface-open control build failed' }
  & .\surface-open-control.exe | Select-Object -Last 1
  if (-not $LASTEXITCODE) { throw 'negative control passed: the test does not see the rule' }
  'PASS surface-open negative control: the rule before BD-296 fails the test'
  & $ClangCl /nologo /std:c++17 /O2 /W4 /WX /EHsc /DBC250_CONTROL_OLD_LOG "/Fe:surface-open-log-control.exe" "$PSScriptRoot/SurfaceOpenTest.cpp"
  if ($LASTEXITCODE) { throw 'surface-open log control build failed' }
  & .\surface-open-log-control.exe | Select-Object -Last 1
  if (-not $LASTEXITCODE) { throw 'negative control passed: the test does not see the log rule' }
  'PASS surface-open log negative control: one Fail line per open fails the test'
} finally { Pop-Location }
