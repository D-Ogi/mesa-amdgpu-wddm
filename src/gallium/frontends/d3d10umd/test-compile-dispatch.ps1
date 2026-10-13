param([Parameter(Mandatory=$true)][string]$Out)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
Push-Location $Out
try {
  & cl /nologo /std:c++17 /O2 /W4 /WX /MD /Z7 /EHsc /utf-8 /Fe:ddi-stack-test.exe "$PSScriptRoot/CompileDispatchTest.cpp" /link /STACK:1048576 /DEBUG /MANIFEST:EMBED
  if ($LASTEXITCODE) { throw 'DDI stack regression build failed' }
  & .\ddi-stack-test.exe
  if ($LASTEXITCODE) { throw 'DDI stack regression failed' }
} finally { Pop-Location }
