#requires -Version 7.0
# build.ps1 — unified build/verify entry point for VCStation, running under PowerShell 7.
#
# Examples (pwsh):
#   pwsh tools/build/build.ps1 -Preset dev                 # build the dev preset
#   pwsh tools/build/build.ps1 -Preset release -Test       # build release, then run its ctest
#   pwsh tools/build/build.ps1 -Preset dev -FormatCheck -Lint
#   pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'ui.ImageReviewControllerTests'
#   pwsh tools/build/build.ps1 -Preset release -Configure  # force a fresh configure first
#
# The wrapper dot-sources tools/build/env.ps1 (vcpkg/MSVC/ninja defaults), auto-configures
# when the build directory has no CMake cache, and runs cmake/ctest inside the MSVC
# vcvarsall environment (required by the Ninja + external-toolset preset).

[CmdletBinding()]
param(
    # Configure preset to use (dev, release, dev-coverage, asan, ...).
    [string]$Preset = 'dev',
    # Force a configure step before building.
    [switch]$Configure,
    # Additional cmake --build targets (format-check, lint, format, ...).
    [string[]]$Target = @(),
    # Run the preset's ctest suite after a successful build.
    [switch]$Test,
    # ctest -R filter for -Test.
    [string]$TestRegex = '',
    # Run only the ctest filter, skipping the build.
    [switch]$TestOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$presetsFile = Join-Path $repoRoot 'CMakePresets.json'
if (-not (Test-Path $presetsFile)) {
    throw "CMakePresets.json not found under '$repoRoot'."
}
$presets = Get-Content $presetsFile -Raw | ConvertFrom-Json
$configurePreset = $presets.configurePresets | Where-Object { $_.name -eq $Preset }
if (-not $configurePreset) {
    throw "Unknown configure preset '$Preset'. Available: $($presets.configurePresets.name -join ', ')."
}
$buildPreset = $presets.buildPresets | Where-Object { $_.name -eq $Preset }

# Environment defaults (vcpkg / MSVC / ninja).
. (Join-Path $PSScriptRoot 'env.ps1')

$binaryDir = Join-Path $repoRoot ("out\build\{0}" -f $Preset)
$cacheFile = Join-Path $binaryDir 'CMakeCache.txt'
$needsConfigure = $Configure -or -not (Test-Path $cacheFile)

# Everything below runs inside the MSVC developer environment. The cmd wrapper is the
# established pattern for importing vcvarsall state into a fresh cmd.exe process.
function Invoke-InVcEnv {
    param([Parameter(Mandatory)][string]$CommandLine)
    $vcvars = $env:VCVARSALL_PATH
    $wrapped = 'call "{0}" x64 >nul 2>&1 && {1}' -f $vcvars, $CommandLine
    $exit = & cmd.exe /d /s /c $wrapped
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed (exit $LASTEXITCODE): $CommandLine"
    }
}

if ($TestOnly) {
    if (-not $TestRegex) {
        throw '-TestOnly requires -TestRegex.'
    }
    Invoke-InVcEnv ("ctest --preset {0} --output-on-failure -R ""{1}""" -f $Preset, $TestRegex)
    Write-Host "ctest OK: preset=$Preset filter=$TestRegex"
    exit 0
}

if ($needsConfigure) {
    Write-Host "Configuring preset '$Preset' ..."
    Invoke-InVcEnv ("cmake --preset {0}" -f $Preset)
}

if ($Target.Count -gt 0) {
    $targetArgs = ($Target | ForEach-Object { "--target {0}" -f $_ }) -join ' '
    Write-Host "Building preset '$Preset' targets: $($Target -join ', ')"
    Invoke-InVcEnv ("cmake --build --preset {0} {1}" -f $Preset, $targetArgs)
} else {
    Write-Host "Building preset '$Preset' ..."
    Invoke-InVcEnv ("cmake --build --preset {0}" -f $Preset)
}

if ($Test) {
    if ($TestRegex) {
        Invoke-InVcEnv ("ctest --preset {0} --output-on-failure -R ""{1}""" -f $Preset, $TestRegex)
        Write-Host "ctest OK: preset=$Preset filter=$TestRegex"
    } else {
        Invoke-InVcEnv ("ctest --preset {0} --output-on-failure" -f $Preset)
        Write-Host "ctest OK: preset=$Preset"
    }
}

Write-Host "Done: preset=$Preset"
