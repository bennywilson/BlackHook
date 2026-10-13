#Requires -Version 5.1
<#
.SYNOPSIS
    Runs every mv_testhost mode against the built hook, one at a time, each with a timeout.

.DESCRIPTION
    Runs are sequential on purpose: each run's checks read "this run's part" of the shared, appended
    temp\logs\mv_hook.log. A hung run is killed (a process stuck at exit can hold mv_hook.dll locked; if
    a later link fails with LNK1104, rename the locked DLL aside).

    -Overlay adds the two GBuffer-grid runs under GPU-based validation (slow).
    -Baseline <dll> also runs the byte oracle: a 4000-frame capture with that DLL and with the built
    one, compared by tools\compare_dumps.py (must be EQUIVALENT).

    Output: temp\suite\<run>.txt, dumps in temp\dumps\suite_<run>. Exit code = number of failures.

.EXAMPLE
    .\tools\run_testhost_suite.ps1
    .\tools\run_testhost_suite.ps1 -Overlay -Baseline temp\base\temp\build\hook\Release\mv_hook.dll
#>
param(
    [switch] $Overlay,
    [string] $Baseline,
    [string[]] $Only,
    [int] $TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$hook = Join-Path $repo 'temp\build\hook\Release\mv_hook.dll'
$exe  = Join-Path $repo 'temp\build\testhost\Release\mv_testhost.exe'
$out  = Join-Path $repo 'temp\suite'
foreach ($f in $hook, $exe) { if (-not (Test-Path $f)) { throw "not built: $f" } }
New-Item -ItemType Directory -Force $out | Out-Null

$runs = [ordered]@{
    'default'           = '--no-gbv'
    'ambiguous'         = '--no-gbv --ambiguous'
    'skyrunner'         = '--no-gbv --pipeline skyrunner'
    'normals'           = '--no-gbv --normals'
    'normals-skyrunner' = '--no-gbv --normals --pipeline skyrunner'
    'normals-ambiguous' = '--no-gbv --normals --ambiguous'
    'normals-phase8'    = '--no-gbv --normals --phase8-tests'
    'phase8'            = '--no-gbv --phase8-tests'
    'velocity-realloc'  = '--no-gbv --velocity-realloc --frames 900'
    'specular'          = '--no-gbv --specular'
    'normals-specular'  = '--no-gbv --normals --specular'
    'phase9-negative'   = '--no-gbv --phase9-negative'
}
if ($Overlay) {
    $runs['overlay-oxi']       = '--normals --specular'
    $runs['overlay-skyrunner'] = '--normals --specular --pipeline skyrunner'
}

# One run: returns $true on exit code 0 with no FAIL line.
function Invoke-Testhost([string]$Name, [string]$Dll, [string]$Arguments, [hashtable]$Env = @{}) {
    foreach ($v in 'MV_PROFILE', 'MV_PROFILE_FILE', 'MV_ENGINE_VERSION', 'MV_SURFACES', 'MV_OVERLAY', 'MV_SNAPSHOT') {
        Remove-Item "Env:$v" -ErrorAction SilentlyContinue
    }
    $env:MV_AUTOCAPTURE = '1'
    $env:MV_DUMP_DIR = Join-Path $repo "temp\dumps\suite_$Name"
    foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
    if (Test-Path $env:MV_DUMP_DIR) { Remove-Item -Recurse -Force $env:MV_DUMP_DIR }
    if ($Arguments -notmatch '--frames') { $Arguments += ' --frames 240' }
    $log = Join-Path $out "$Name.txt"
    $p = Start-Process $exe "--hook `"$Dll`" $Arguments" -RedirectStandardOutput $log -NoNewWindow -PassThru
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        $p | Stop-Process -Force
        Write-Host "FAIL $Name - hung, killed after $TimeoutSeconds s"
        return $false
    }
    $text = Get-Content $log
    $fails = @($text | Select-String 'FAIL')
    $ok = $fails.Count -eq 0 -and ($text | Select-String 'exiting with code 0')
    Write-Host ("{0} {1}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $Name)
    $fails | ForEach-Object { Write-Host "     $($_.Line)" }
    return [bool]$ok
}

$failures = 0
foreach ($name in $runs.Keys) {
    if ($Only -and $name -notin $Only) { continue }
    $extra = if ($name -like 'overlay-*') { @{ MV_OVERLAY = '3' } } else { @{} }
    if (-not (Invoke-Testhost $name $hook $runs[$name] $extra)) { $failures++ }
}

if ($Baseline) {
    $Baseline = (Resolve-Path $Baseline).Path
    $null = Invoke-Testhost 'oracle-before' $Baseline '--no-gbv --frames 4000'
    $null = Invoke-Testhost 'oracle-after' $hook '--no-gbv --frames 4000'
    $result = python (Join-Path $repo 'tools\compare_dumps.py') `
        (Join-Path $repo 'temp\dumps\suite_oracle-before') (Join-Path $repo 'temp\dumps\suite_oracle-after')
    $result | Set-Content (Join-Path $out 'oracle.txt')
    $verdict = $result | Select-String '^RESULT'
    Write-Host "ORACLE $verdict"
    if ($verdict -notmatch 'EQUIVALENT') { $failures++ }
}

Write-Host "$failures failure(s)"
exit $failures
