param(
  [Parameter(Mandatory=$true)][string]$Client,
  [Parameter(Mandatory=$true)][ValidateSet('shader-object','graphics','compute','rt','d3d11','d3d11-cpu')][string]$Case,
  [Parameter(Mandatory=$true)][string]$Artifact,
  [Parameter(Mandatory=$true)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$Sha256,
  [Parameter(Mandatory=$true)][string]$Output,
  [ValidateRange(1,170)][int]$TimeoutSeconds=120,
  [switch]$ExpectStackOverflow
)
# One bounded process, no registry changes or deployment. The integrator owns D3D router selection.
$ErrorActionPreference='Stop'
$Client=(Resolve-Path -LiteralPath $Client).Path
$Artifact=(Resolve-Path -LiteralPath $Artifact).Path
$Output=[IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) { throw 'Output exists: preserve the earlier result' }
if ((Get-FileHash -LiteralPath $Artifact -Algorithm SHA256).Hash -ne $Sha256) { throw 'artifact hash mismatch' }
New-Item -ItemType Directory -Path $Output | Out-Null
$argsList=@()
if ($Case -like 'd3d11*') {
  $argsList=@($Case)
} else {
  $argsList=@('vulkan',$Artifact,$Case,(Split-Path -Parent $Client))
}
$info=New-Object Diagnostics.ProcessStartInfo
$info.FileName=$Client
$info.UseShellExecute=$false
$info.CreateNoWindow=$true
$info.RedirectStandardOutput=$true
$info.RedirectStandardError=$true
# Windows paths cannot contain a quote. Trim a trailing slash to keep quoting unambiguous.
$info.Arguments=($argsList | ForEach-Object { '"'+$_.TrimEnd('\')+'"' }) -join ' '
$info.EnvironmentVariables['MESA_TEST_EXPECT_UMD']=$Artifact
$info.EnvironmentVariables['TEMP']=$Output
$info.EnvironmentVariables['TMP']=$Output
$info.EnvironmentVariables['MESA_COMPILE_STACK_STATS']='1'
$info.EnvironmentVariables['MESA_SHADER_CACHE_DISABLE']='true'
$process=New-Object Diagnostics.Process
$process.StartInfo=$info
$started=[DateTime]::UtcNow
try {
  if (!$process.Start()) { throw 'process did not start' }
  $stdout=$process.StandardOutput.ReadToEndAsync()
  $stderr=$process.StandardError.ReadToEndAsync()
  $timedOut=!$process.WaitForExit($TimeoutSeconds*1000)
  if ($timedOut) {
    $process.Kill()
    if (!$process.WaitForExit(5000)) { throw "Timed-out process $($process.Id) did not exit after termination" }
  }
  [IO.File]::WriteAllText((Join-Path $Output 'stdout.txt'),$stdout.GetAwaiter().GetResult())
  [IO.File]::WriteAllText((Join-Path $Output 'stderr.txt'),$stderr.GetAwaiter().GetResult())
  $expected=0
  if ($ExpectStackOverflow) { $expected=-1073741571 }
  $status='FAIL'
  if ($timedOut) { $status='TIMEOUT' }
  elseif ($process.ExitCode -eq 77) { $status='UNAVAILABLE' }
  elseif ($process.ExitCode -eq $expected) { $status='PASS'; if ($ExpectStackOverflow) { $status='EXPECTED_STACK_OVERFLOW' } }
  $result=[ordered]@{case=$Case;status=$status;start_utc=$started.ToString('o');elapsed_s=([DateTime]::UtcNow-$started).TotalSeconds;exit_code=$process.ExitCode;expected_exit=$expected;artifact=$Artifact;sha256=$Sha256;client_sha256=(Get-FileHash -LiteralPath $Client -Algorithm SHA256).Hash}
  $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'result.json') -Encoding UTF8
  $result | ConvertTo-Json
  if ($status -notin @('PASS','EXPECTED_STACK_OVERFLOW')) { exit 1 }
} finally { $process.Dispose() }
