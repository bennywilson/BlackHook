<#
.SYNOPSIS
    Launch a UE5 game with the capture environment set, wait for its render
    process, and inject mv_hook.dll into it.

.DESCRIPTION
    - The MV_* variables reach the game only by inheritance from the process
      that launches it, so the script sets them before Start-Process. Set
      against a running game they do nothing.
    - The launcher is not the renderer: Valfreyja.exe starts
      Skyrunner-Win64-Shipping.exe. The script waits for the *-Win64-Shipping
      process and injects there.
    - Some titles exit at once unless started in their own folder, so the
      working directory is set.
    - Injecting while the process is still starting has crashed a game, so the
      script waits -SettleSeconds first.
    - Each injection restarts the capture index at 0, so the default dump
      directory is new and timestamped, and a directory with captures in it is
      refused without -Force.
    - A target that already has an mv_hook.dll loaded is refused: the same path
      only bumps a reference count, and a different path patches the vtables
      twice. Something on this machine has injected the hook unasked before.

.PARAMETER Game
    The game's launcher (Valfreyja.exe); the renderer is found from it. With
    -AttachOnly, the name of a running process (Skyrunner-Win64-Shipping.exe).

.PARAMETER EngineVersion
    major.minor, e.g. 5.2. Recorded in manifest.json and needed to decode
    velocity channels 2 and 3, which 5.2 and 5.7 pack differently; a wrong
    value gives a wrong V.z and bHasPixelAnimation. Inferred for titles listed
    below, otherwise required. Also picks the profile (5.2 -> ue5.2-generic).

.PARAMETER SurfaceProfile
    A profile from profiles/ue5.json, instead of the one EngineVersion picks. An
    unknown name disables identification and the hook log says so.

.PARAMETER Surfaces
    Optional surfaces to capture as well: "normals" (GBufferA), "specular"
    (GBufferB), or both. Check their lines in the hook log on a new title.

.PARAMETER DumpDir
    Where captures go. Default: a new timestamped directory in temp\dumps\.

.PARAMETER Hook
    Path to mv_hook.dll. Default: the Release build in temp\build\.

.PARAMETER RenderProcess
    Process-name pattern of the renderer. Default *-Win64-Shipping.

.PARAMETER SettleSeconds
    Wait after the render process appears before injecting. Default 8.

.PARAMETER AttachOnly
    Inject into a running process without launching. The MV_* variables
    cannot reach it this way, and the script says so.

.EXAMPLE
    .\Invoke-MvCapture.ps1 .\testgames\skyrunner\Valfreyja.exe

.EXAMPLE
    .\Invoke-MvCapture.ps1 .\testgames\skyrunner\Valfreyja.exe -DumpDir D:\mv_captures\run7 -AutoCapture
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Game,

    [string] $EngineVersion,
    [string] $SurfaceProfile,
    [string[]] $Surfaces, # -Surfaces normals,specular arrives as an array
    [string] $DumpDir,
    [string] $Hook,
    [string] $RenderProcess = '*-Win64-Shipping',

    [int] $SettleSeconds = 8,
    [int] $LaunchTimeoutSeconds = 120,

    [string[]] $GameArgs = @(),

    [switch] $AttachOnly,
    [switch] $AutoCapture,
    [switch] $NoCaptureDepth,
    [switch] $Force,

    # Everything up to the injection: launch, find the render process, settle,
    # and check for a loaded hook. Confirms the target before a real session.
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- output ----

function Write-Step  { param([string]$m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok    { param([string]$m) Write-Host "    $m" -ForegroundColor Green }
function Write-Note  { param([string]$m) Write-Host "    $m" -ForegroundColor DarkGray }
function Write-Warn2 { param([string]$m) Write-Host "!!  $m" -ForegroundColor Yellow }
function Fail        { param([string]$m) Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }

# ------------------------------------------------------------- locate bits --

$repoRoot   = $PSScriptRoot
# Everything generated lives under the gitignored temp/ (see tools/paths.py).
$tempRoot   = Join-Path $repoRoot 'temp'
$buildDir   = Join-Path $tempRoot 'build'

# Visual Studio generator puts binaries under Release\; a single-config
# generator (Ninja, as VS Code's CMake extension uses) does not.
function Find-Built { param([string]$SubDir, [string]$Leaf)
    foreach ($candidate in @("$SubDir\Release\$Leaf", "$SubDir\$Leaf")) {
        $p = Join-Path $buildDir $candidate
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return Join-Path $buildDir "$SubDir\Release\$Leaf"
}

$buildHint  = "       Build it first:  cmake -B temp/build -S . ; cmake --build temp/build --config Release --target mv_hook mv_injector"
$injectorEx = Find-Built 'injector' 'mv_injector.exe'

if (-not $Hook) {
    $Hook = Find-Built 'hook' 'mv_hook.dll'
}

if (-not (Test-Path -LiteralPath $injectorEx)) {
    Fail ("mv_injector.exe not found at $injectorEx`n" + $buildHint)
}
if (-not (Test-Path -LiteralPath $Hook)) {
    Fail ("mv_hook.dll not found at $Hook`n" + $buildHint)
}

# The injector passes this path to LoadLibraryW in the target, so it must be
# absolute; a relative path resolves against the game's working directory.
$Hook       = (Resolve-Path -LiteralPath $Hook).Path
$injectorEx = (Resolve-Path -LiteralPath $injectorEx).Path

# --------------------------------------------------------- engine version ---

# Titles with a known engine version. Any other must be given: a wrong guess
# mis-decodes velocity channels 2 and 3.
$knownEngineVersions = @{
    'valfreyja'                  = '5.2'
    'skyrunner-win64-shipping'   = '5.2'
    'oxi'                        = '5.7'
    'oxi-win64-shipping'         = '5.7'
}

function Resolve-EngineVersion {
    param([string]$ExeName, [string]$Supplied)

    if ($Supplied) {
        if ($Supplied -notmatch '^\d+\.\d+') {
            Fail "EngineVersion must be major.minor (e.g. 5.2); got '$Supplied'. The hook ignores anything else and records no version at all."
        }
        return $Supplied
    }

    $key = [System.IO.Path]::GetFileNameWithoutExtension($ExeName).ToLowerInvariant()
    if ($knownEngineVersions.ContainsKey($key)) {
        return $knownEngineVersions[$key]
    }
    return $null
}

# ------------------------------------------------------- process helpers ----

function Get-MatchingProcesses {
    param([string]$Pattern, [Nullable[datetime]]$StartedAfter)

    $all = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -like $Pattern })
    if (-not $StartedAfter) { return $all }

    $out = @()
    foreach ($p in $all) {
        $started = $null
        try { $started = $p.StartTime } catch { continue }
        if ($started -and $started -ge $StartedAfter) { $out += $p }
    }
    return $out
}

function Get-HookModules {
    param([System.Diagnostics.Process]$Process)

    # Can fail (an elevated target, or one shutting down). Failure means
    # "unknown", not "none": the caller must not read it as a clean target.
    try {
        $mods = $Process.Modules | Where-Object { $_.ModuleName -like '*mv_hook*' }
        return @{ Ok = $true; Modules = @($mods) }
    } catch {
        return @{ Ok = $false; Modules = @() }
    }
}

function Invoke-Injector {
    param([string[]]$Arguments)

    Write-Note "$injectorEx $($Arguments -join ' ')"

    # Capture $stdout: a function returns everything written to the output
    # stream, so uncaptured native output would be returned along with the exit
    # code, and the caller's "-ne 0" check would report failure on success.
    #
    # stderr goes straight to the console. Redirecting it (2>&1) under PS 5.1
    # wraps each line in an ErrorRecord and sets $? false even on exit code 0.
    $stdout = & $injectorEx @Arguments
    $exit   = $LASTEXITCODE

    foreach ($line in $stdout) { Write-Host "    $line" }
    return $exit
}

# ------------------------------------------------------------- pre-flight ---

Write-Step 'Pre-flight'

# RenderDoc's global hook wraps ID3D12GraphicsCommandList, so every barrier is
# seen twice and the barrier profiles are wrong.
$renderDoc = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'qrenderdoc*' -or $_.Name -like 'renderdoccmd*' })
if ($renderDoc.Count -gt 0) {
    Write-Warn2 "RenderDoc is running (pid $($renderDoc[0].Id)). If its GLOBAL hook is enabled, every barrier is observed twice and the identification profile is wrong. Turn the global hook off, not just this window."
}

if ($AttachOnly) {
    Write-Note 'AttachOnly: not launching, and MV_* variables will NOT reach the target.'
    Write-Note 'Environment is inherited at process start only - a running game cannot pick them up.'
} else {
    if (-not (Test-Path -LiteralPath $Game)) {
        Fail "game executable not found: $Game"
    }
    $Game = (Resolve-Path -LiteralPath $Game).Path
}

$gameLeaf = Split-Path -Leaf $Game

# The engine version is needed at launch, the only time it can be set.
$resolvedVersion = Resolve-EngineVersion -ExeName $gameLeaf -Supplied $EngineVersion
if (-not $AttachOnly) {
    if (-not $resolvedVersion) {
        Fail ("cannot infer MV_ENGINE_VERSION from '$gameLeaf', and it was not supplied.`n" +
              "       Pass -EngineVersion <major.minor>. This is not a formality: 5.2 and 5.7 pack V.z and`n" +
              "       bHasPixelAnimation into channels 2/3 differently, and the offline decode trusts the manifest.")
    }
    if (-not $EngineVersion) {
        Write-Note "MV_ENGINE_VERSION=$resolvedVersion (inferred from '$gameLeaf')"
    } else {
        Write-Note "MV_ENGINE_VERSION=$resolvedVersion"
    }
}

# --------------------------------------------------------------- dump dir ---

if (-not $AttachOnly) {
    if (-not $DumpDir) {
        $stamp   = Get-Date -Format 'yyyyMMdd_HHmmss'
        $slug    = [System.IO.Path]::GetFileNameWithoutExtension($gameLeaf).ToLowerInvariant()
        $DumpDir = Join-Path $tempRoot "dumps\${slug}_$stamp"
    }

    if (Test-Path -LiteralPath $DumpDir) {
        $existing = @(Get-ChildItem -LiteralPath $DumpDir -Filter 'vel_*.bin' -ErrorAction SilentlyContinue)
        if ($existing.Count -gt 0 -and -not $Force) {
            Fail ("$DumpDir already contains $($existing.Count) vel_*.bin file(s).`n" +
                  "       The capture index restarts at 0 on every injection and nothing checks before`n" +
                  "       overwriting, so this would destroy them. Use a fresh -DumpDir, or -Force if you`n" +
                  "       genuinely mean to overwrite.")
        }
    } else {
        New-Item -ItemType Directory -Path $DumpDir -Force | Out-Null
    }
    $DumpDir = (Resolve-Path -LiteralPath $DumpDir).Path
    Write-Note "MV_DUMP_DIR=$DumpDir"
}

# ----------------------------------------------------------------- launch ---

$launchWatermark = $null

if (-not $AttachOnly) {
    Write-Step "Launching $gameLeaf"

    # Set in this process, so Start-Process passes them on (it inherits the
    # environment unless -UseNewEnvironment is given).
    $env:MV_DUMP_DIR       = $DumpDir
    $env:MV_ENGINE_VERSION = $resolvedVersion
    if ($SurfaceProfile) { $env:MV_PROFILE       = $SurfaceProfile } else { Remove-Item Env:\MV_PROFILE       -ErrorAction SilentlyContinue }
    if ($Surfaces)       { $env:MV_SURFACES      = $Surfaces -join ',' } else { Remove-Item Env:\MV_SURFACES      -ErrorAction SilentlyContinue }
    if ($AutoCapture)   { $env:MV_AUTOCAPTURE   = '1' } else { Remove-Item Env:\MV_AUTOCAPTURE   -ErrorAction SilentlyContinue }
    if ($NoCaptureDepth) { $env:MV_CAPTURE_DEPTH = '0' } else { Remove-Item Env:\MV_CAPTURE_DEPTH -ErrorAction SilentlyContinue }

    $workingDir = Split-Path -Parent $Game
    Write-Note "working directory: $workingDir  (some titles exit immediately without this)"

    # Slightly before launch: process start times are coarse, and a child
    # started in the same tick must still count.
    $launchWatermark = (Get-Date).AddSeconds(-2)

    $startArgs = @{
        FilePath         = $Game
        WorkingDirectory = $workingDir
    }
    if ($GameArgs.Count -gt 0) {
        $startArgs['ArgumentList'] = $GameArgs
        Write-Note "arguments: $($GameArgs -join ' ')"
    }
    Start-Process @startArgs | Out-Null
    Write-Ok 'Launched.'
}

# ------------------------------------------------- find the render process --

if ($AttachOnly) {
    # Attaching: accept a process already running, and match the name passed
    # in rather than the launch pattern.
    $pattern = [System.IO.Path]::GetFileNameWithoutExtension((Split-Path -Leaf $Game))
    if ($PSBoundParameters.ContainsKey('RenderProcess')) { $pattern = $RenderProcess }
    Write-Step "Looking for the render process (pattern: $pattern)"
    $candidates = @(Get-MatchingProcesses -Pattern $pattern -StartedAfter $null)
    if ($candidates.Count -eq 0) {
        Fail "no running process matches '$pattern'."
    }
} else {
    Write-Step "Waiting for the render process (pattern: $RenderProcess)"
    $deadline   = (Get-Date).AddSeconds($LaunchTimeoutSeconds)
    $candidates = @()
    while ((Get-Date) -lt $deadline) {
        $candidates = @(Get-MatchingProcesses -Pattern $RenderProcess -StartedAfter $launchWatermark)
        if ($candidates.Count -gt 0) { break }
        Start-Sleep -Milliseconds 500
    }

    if ($candidates.Count -eq 0) {
        Fail ("no process matching '$RenderProcess' started within $LaunchTimeoutSeconds s.`n" +
              "       If this title's renderer is named differently, pass -RenderProcess <pattern>.`n" +
              "       If the game exited immediately, check the working directory requirement.")
    }
}

# mv_injector picks its target by name and takes the first match, so with two
# matches the script refuses.
if ($candidates.Count -gt 1) {
    Write-Warn2 "more than one process matches '$RenderProcess':"
    foreach ($c in $candidates) { Write-Warn2 "      pid $($c.Id)  $($c.Name)" }
    Fail ("mv_injector selects its target by name and cannot disambiguate these.`n" +
          "       Close the extras, or narrow -RenderProcess to match exactly one.")
}

$target     = $candidates[0]
$targetName = "$($target.Name).exe"
Write-Ok "Found $targetName, pid=$($target.Id)"

# ----------------------------------------------------------------- settle ---

if ($SettleSeconds -gt 0) {
    Write-Step "Letting it settle for $SettleSeconds s before injecting"
    Write-Note 'Injecting into a process still starting up has crashed a target before.'
    Start-Sleep -Seconds $SettleSeconds

    if ($target.HasExited) {
        Fail ("$targetName exited during the settle wait.`n" +
              "       A title that exits this fast is usually missing its working directory or a launch argument.")
    }
}

# ------------------------------------------- is the hook already in there? --

Write-Step 'Checking the target for an existing mv_hook.dll'

$target.Refresh()
$hookState = Get-HookModules -Process $target

if (-not $hookState.Ok) {
    Write-Warn2 "could not enumerate $targetName's modules (elevated target?). Proceeding, but this check was NOT performed - do not read its silence as 'clean'."
} elseif ($hookState.Modules.Count -eq 0) {
    Write-Ok 'Clean: no mv_hook.dll loaded.'
} else {
    Write-Warn2 "$($hookState.Modules.Count) mv_hook.dll module(s) already loaded in this process:"
    foreach ($m in $hookState.Modules) { Write-Warn2 "      $($m.FileName)" }

    $sameCopy = @($hookState.Modules | Where-Object { $_.FileName -ieq $Hook })
    if ($sameCopy.Count -gt 0) {
        Write-Warn2 'One of them is the copy about to be injected. LoadLibraryW would only bump its reference count - no second DllMain, nothing reset.'
    } else {
        Write-Warn2 'These are DIFFERENT files from the one about to be injected. Injecting would map a second independent copy, double-patching the same vtables.'
    }
    Write-Warn2 'Something on this machine injects the main-checkout mv_hook.dll into fresh Skyrunner processes unprompted.'

    if (-not $Force) {
        Fail ("refusing to inject on top of an existing copy.`n" +
              "       Restart the game and re-run this script, which is the documented fix. Use -Force to`n" +
              "       inject anyway, knowing what the paragraph above says it will do.")
    }
    Write-Warn2 'Proceeding anyway because -Force was given.'
}

# ----------------------------------------------------------------- inject ---

if ($DryRun) {
    Write-Step 'DryRun: stopping before injection'
    Write-Note "would run: $injectorEx $targetName $Hook"
    Write-Ok "Everything up to the injection checked out. Target would be $targetName (pid $($target.Id))."
    exit 0
}

Write-Step "Injecting $Hook"

$logPath = Join-Path $tempRoot 'logs\mv_hook.log'
# The log is appended across sessions: only lines written after injection count.
$logLinesBefore = if (Test-Path -LiteralPath $logPath) { @(Get-Content -LiteralPath $logPath).Count } else { 0 }
$code = Invoke-Injector @($targetName, $Hook)
if ($code -ne 0) {
    Fail "mv_injector exited $code - see its output above."
}
Write-Ok 'Injection reported success.'

# -------------------------------------------------- confirm from the log ----

Write-Step 'Confirming from the hook log'
Write-Note $logPath

# The injector only knows LoadLibraryW returned non-NULL; the log says whether
# the hook started.
$deadline = (Get-Date).AddSeconds(15)
$sawInit  = $false
while ((Get-Date) -lt $deadline) {
    if (Test-Path -LiteralPath $logPath) {
        $new = @(Get-Content -LiteralPath $logPath -ErrorAction SilentlyContinue | Select-Object -Skip $logLinesBefore)
        if ($new -match 'InitThread running') { $sawInit = $true; break }
    }
    Start-Sleep -Milliseconds 500
}

if ($sawInit) {
    Write-Ok 'Hook initialised (saw "InitThread running").'
} else {
    Write-Warn2 'Did not see "InitThread running" in the log within 15 s. The DLL loaded, but the hook may not have come up - check the log before trusting a capture.'
}

# ------------------------------------------------------------- what next ----

Write-Host ''
Write-Step 'Ready'
Write-Host "    target      : $targetName (pid $($target.Id))"
if (-not $AttachOnly) {
    Write-Host "    dump dir    : $DumpDir"
    Write-Host "    engine ver  : $resolvedVersion"
}
Write-Host "    hook log    : $logPath"
Write-Host ''
Write-Host '    F7  cycle the in-game overlay      F8  capture a 2400-frame burst'
Write-Host ''
Write-Host '    Wait for "capture: write queue drained" in the log before quitting the game -'
Write-Host '    "burst recorded" does NOT mean flushed, and quitting early truncates the dump.'
Write-Host ''
Write-Host '    Backpressure can drop frames; manifest.json counts them.'
Write-Host '    Count what actually landed before trusting the log:'
if (-not $AttachOnly) {
    Write-Host "        (Get-ChildItem '$DumpDir' -Filter vel_*.bin).Count"
    Write-Host ''
    Write-Host '    Then validate:'
    Write-Host "        python `"$repoRoot\tools\run_validation.py`" `"$DumpDir`" <tag>"
}
Write-Host ''
