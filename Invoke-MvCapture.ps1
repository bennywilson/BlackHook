#Requires -Version 5.1
<#
.SYNOPSIS
    Launch a UE5 game with the motion-vector capture environment set correctly,
    wait for its real render process, and inject mv_hook.dll into it.

.DESCRIPTION
    Every step this automates is a documented way to lose a session:

      * MV_DUMP_DIR / MV_ENGINE_VERSION reach the render process only by
        INHERITANCE from the shell that launches the game. Setting them against
        an already-running game does nothing, and the failure is silent - the
        capture just lands in the wrong place, or meta.txt records no engine
        version and the offline decode falls back to a default for channels 2/3.
        This script sets them before Start-Process, so they are inherited.

      * The launcher is not the renderer. Valfreyja.exe spawns
        Skyrunner-Win64-Shipping.exe; Oxi.exe spawns Oxi-Win64-Shipping.exe.
        Injecting into the launcher hooks a process with no D3D12 device.
        This script waits for the *-Win64-Shipping child and injects there.

      * Some titles exit immediately unless their working directory is their
        own folder. Start-Process is given -WorkingDirectory accordingly.

      * Injecting into a process that is still starting up has crashed a
        target before. This script waits -SettleSeconds after the render
        process appears.

      * A reinjection restarts the capture index at 0 and overwrites
        vel_00000.bin. The default dump directory here is timestamped and
        fresh, and a non-empty one is refused without -Force.

      * mv_hook.dll already loaded in the target changes what injection means
        (same path = a reference-count bump, no second DllMain; different path
        = a second independent copy double-patching the same vtables). On a
        shared machine this happens without anyone asking. This script
        enumerates the target's modules and refuses to make it worse.

    See DEBUGGING.md ("Something double-injects mv_hook.dll into Skyrunner,
    unprompted") and NEXT.md ("Things that will bite") for the incidents
    behind each of these.

.PARAMETER Game
    Path to the game executable to launch - the launcher (Valfreyja.exe,
    Oxi.exe) is correct here; the child renderer is found automatically. With
    -AttachOnly this may instead be a bare process name
    (Skyrunner-Win64-Shipping.exe) of something already running.

.PARAMETER EngineVersion
    major.minor, e.g. 5.2. Recorded into meta.txt and required by the offline
    decode for channels 2/3, which 5.2 and 5.7 pack differently. Inferred from
    the executable name for titles this repo knows; otherwise you must pass it,
    because a wrong value here silently corrupts V.z and bHasPixelAnimation.
    Also picks the surface profile: 5.2 -> ue5.2-generic (profiles/ue5.json).

.PARAMETER SurfaceProfile
    Surface profile name from profiles/ue5.json, overriding the one derived
    from EngineVersion. A name that does not exist disables identification
    (the hook log says so) rather than falling back.

.PARAMETER Surfaces
    Optional surfaces to capture as well, comma-separated - today only
    "normals" (UE5's GBufferA; docs/REFACTOR_PLAN.md Phase 10). Experimental:
    read the "normals:" lines in the hook log before trusting the result.

.PARAMETER DumpDir
    Where captures go. Defaults to a fresh timestamped directory under the
    repo's gitignored temp\dumps\.

.PARAMETER Hook
    Path to mv_hook.dll. Defaults to the repo's Release build in temp\build\.

.PARAMETER RenderProcess
    Process-name pattern for the real renderer. Defaults to *-Win64-Shipping,
    the UE5 shipping-build convention.

.PARAMETER SettleSeconds
    How long to wait after the render process appears before injecting.
    Default 8. Racing startup has crashed a target.

.PARAMETER AttachOnly
    Do not launch anything; inject into an already-running process. The MV_*
    environment variables CANNOT be applied this way - the script says so
    rather than pretending they took effect.

.EXAMPLE
    .\Invoke-MvCapture.ps1 .\testgames\skyrunner\Valfreyja.exe

.EXAMPLE
    .\Invoke-MvCapture.ps1 .\testgames\skyrunner\Valfreyja.exe -DumpDir D:\mv_captures\run7 -AutoCapture
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Game,

    [string] $EngineVersion,
    [string] $SurfaceProfile,
    [string] $Surfaces,
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

    # Do everything except the injection itself: launch, find the render
    # process, settle, and run the already-loaded check. Use it to confirm the
    # script picks the process you expect before committing to a real session.
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

# The injector writes this exact path into the target with WriteProcessMemory
# for the remote LoadLibraryW call - it must be absolute, or LoadLibraryW
# resolves it against the GAME's working directory, not ours.
$Hook       = (Resolve-Path -LiteralPath $Hook).Path
$injectorEx = (Resolve-Path -LiteralPath $injectorEx).Path

# --------------------------------------------------------- engine version ---

# Only titles this repo has actually established a version for. Anything else
# must be supplied: guessing here silently mis-decodes channels 2 and 3, which
# is a failure mode this project has already paid for once.
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

    # Can legitimately fail (elevated target, or the process is mid-teardown).
    # A failure here is "unknown", not "none" - the caller must not read an
    # empty result as proof the target is clean.
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

    # $stdout must be CAPTURED, not left on the output stream. A PowerShell
    # function returns everything written to that stream, so an uncaptured
    # native command's stdout gets concatenated with the exit code below and
    # the caller receives an array - which then compares "-ne 0" as true and
    # turns a successful injection into a reported failure. That is not
    # hypothetical: it is exactly what this function did on its first live run.
    #
    # stderr is deliberately NOT redirected here. It flows straight to the
    # console, which is what we want, and 2>&1 under PS 5.1 wraps each stderr
    # line in an ErrorRecord and flips $? even when the process exits 0.
    $stdout = & $injectorEx @Arguments
    $exit   = $LASTEXITCODE

    foreach ($line in $stdout) { Write-Host "    $line" }
    return $exit
}

# ------------------------------------------------------------- pre-flight ---

Write-Step 'Pre-flight'

# RenderDoc's global hook wraps ID3D12GraphicsCommandList and doubles every
# barrier observation, producing a tidier profile than the truth - it has
# already contaminated one set of measurements in this project.
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

# Engine version is required for a launch, since that is the only moment it can
# still be applied.
$resolvedVersion = Resolve-EngineVersion -ExeName $gameLeaf -Supplied $EngineVersion
if (-not $AttachOnly) {
    if (-not $resolvedVersion) {
        Fail ("cannot infer MV_ENGINE_VERSION from '$gameLeaf', and it was not supplied.`n" +
              "       Pass -EngineVersion <major.minor>. This is not a formality: 5.2 and 5.7 pack V.z and`n" +
              "       bHasPixelAnimation into channels 2/3 differently, and the offline decode trusts meta.txt.")
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

    # Set these in THIS process so Start-Process passes them down. Start-Process
    # inherits the caller's environment block unless -UseNewEnvironment is given,
    # which is exactly why that switch must not be used here.
    $env:MV_DUMP_DIR       = $DumpDir
    $env:MV_ENGINE_VERSION = $resolvedVersion
    if ($SurfaceProfile) { $env:MV_PROFILE       = $SurfaceProfile } else { Remove-Item Env:\MV_PROFILE       -ErrorAction SilentlyContinue }
    if ($Surfaces)       { $env:MV_SURFACES      = $Surfaces }       else { Remove-Item Env:\MV_SURFACES      -ErrorAction SilentlyContinue }
    if ($AutoCapture)   { $env:MV_AUTOCAPTURE   = '1' } else { Remove-Item Env:\MV_AUTOCAPTURE   -ErrorAction SilentlyContinue }
    if ($NoCaptureDepth) { $env:MV_CAPTURE_DEPTH = '0' } else { Remove-Item Env:\MV_CAPTURE_DEPTH -ErrorAction SilentlyContinue }

    $workingDir = Split-Path -Parent $Game
    Write-Note "working directory: $workingDir  (some titles exit immediately without this)"

    # Watermark slightly before launch: process start times have coarse
    # resolution, and a child that starts in the same tick must not be missed.
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
    # Attaching: accept a process that was already running, and match on the
    # name actually passed rather than the launch-time default - otherwise the
    # message names one pattern while the search uses another.
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

# mv_injector matches the target by NAME and takes whichever the snapshot
# returns first. With two matches that is a coin flip, so refuse rather than
# inject into an arbitrary one.
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
    Write-Warn2 'Something on this machine injects the main-checkout mv_hook.dll into fresh Skyrunner processes unprompted - see NEXT.md, "Things that will bite".'

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

$code = Invoke-Injector @($targetName, $Hook)
if ($code -ne 0) {
    Fail "mv_injector exited $code - see its output above."
}
Write-Ok 'Injection reported success.'

# -------------------------------------------------- confirm from the log ----

$logPath = Join-Path $tempRoot 'logs\mv_hook.log'
Write-Step 'Confirming from the hook log'
Write-Note $logPath

# The injector can only report that LoadLibraryW returned non-NULL. That the
# hook actually came up is a separate claim, and it is in the log.
$deadline = (Get-Date).AddSeconds(15)
$sawInit  = $false
while ((Get-Date) -lt $deadline) {
    if (Test-Path -LiteralPath $logPath) {
        $tail = @(Get-Content -LiteralPath $logPath -Tail 40 -ErrorAction SilentlyContinue)
        if ($tail -match 'InitThread running') { $sawInit = $true; break }
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
Write-Host '    F7  cycle the in-game overlay      F8  capture a 60-frame burst'
Write-Host ''
Write-Host '    Wait for "capture: write queue drained" in the log before quitting the game -'
Write-Host '    "burst recorded" does NOT mean flushed, and quitting early truncates the dump.'
Write-Host ''
Write-Host '    A burst can report a clean frame count and still be short on disk (see NEXT.md).'
Write-Host '    Count what actually landed before trusting the log:'
if (-not $AttachOnly) {
    Write-Host "        (Get-ChildItem '$DumpDir' -Filter vel_*.bin).Count"
    Write-Host ''
    Write-Host '    Then validate:'
    Write-Host "        python `"$repoRoot\tools\run_validation.py`" `"$DumpDir`" <tag>"
}
Write-Host ''
