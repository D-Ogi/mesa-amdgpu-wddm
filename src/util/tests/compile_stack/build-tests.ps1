param(
  [Parameter(Mandatory=$true)][string]$Out,
  [Parameter(Mandatory=$true)][string]$Glslang,
  [string]$Mesa = (Join-Path $PSScriptRoot '../../../..')
)
# Run from an x64 or x86 VS developer shell. All output and compiler temp files use Out.
$ErrorActionPreference = 'Stop'
$Out = [IO.Path]::GetFullPath($Out)
$Mesa = [IO.Path]::GetFullPath($Mesa)
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$env:TEMP=$Out
$env:TMP=$Out
Push-Location $Out
try {
  & cl /nologo /std:c11 /O2 /W4 /WX /MD /Z7 /utf-8 /Fe:compile-stack.exe "$Mesa/src/util/tests/compile_stack_win32.c" /link /STACK:1048576 /DEBUG
  if ($LASTEXITCODE) { throw 'helper regression build failed' }
  & cl /nologo /std:c++17 /O2 /W4 /WX /MD /Z7 /EHsc /utf-8 "/I$Mesa/include" /Fe:api-stack.exe "$PSScriptRoot/api-stack.cpp" /link d3d11.lib dxgi.lib d3dcompiler.lib /STACK:8388608 /DEBUG
  if ($LASTEXITCODE) { throw 'public API client build failed' }
  foreach ($stage in @('vert','frag','comp','rgen')) {
    & $Glslang -V --target-env vulkan1.3 -o "$Out/stack.$stage.spv" "$PSScriptRoot/stack.$stage"
    if ($LASTEXITCODE) { throw "SPIR-V $stage build failed" }
  }
} finally { Pop-Location }
