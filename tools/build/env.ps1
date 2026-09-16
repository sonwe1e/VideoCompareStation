#requires -Version 7.0
# env.ps1 — single source of truth for machine-local build defaults (VCStation).
#
# Usage (PowerShell 7 / pwsh):  . ./tools/build/env.ps1
# Optional overrides:          . ./tools/build/env.ps1 -VcpkgRoot X:\vcpkg -VcvarsAll C:\...\vcvarsall.bat
#
# Dot-source this file (or run via the tools/build/build.ps1 wrapper). It:
#   - sets $env:VCPKG_ROOT (used by the CMake base preset toolchain), and
#   - exports $env:VCVARSALL_PATH and $env:NINJA_BIN so callers can wrap cmake in
#     the MSVC developer environment.
# Every repository PowerShell script carries `#requires -Version 7.0`; Windows
# PowerShell 5.1 is not a supported shell for this project.

[CmdletBinding()]
param(
    # Vcpkg root with scripts/buildsystems/vcpkg.cmake (default: $env:VCPKG_ROOT, then the
    # machine-local default below).
    [string]$VcpkgRoot = '',
    # Full path to vcvarsall.bat (default: auto-detected BuildTools/VS 2022 layout).
    [string]$VcvarsAll = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# --- Vcpkg ----------------------------------------------------------------
# Test-Path/Join-Path throw for a drive that does not exist, so validation must not
# propagate those (an injected/stale VCPKG_ROOT must degrade to the machine default).
function Test-VcpkgRoot {
    param([string]$Root)
    if (-not $Root) { return $false }
    try {
        return Test-Path (Join-Path $Root 'scripts\buildsystems\vcpkg.cmake') -ErrorAction Stop
    } catch {
        return $false
    }
}

if (-not $VcpkgRoot) {
    $VcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'G:\Workspaces\vcpkg' }
    if (-not (Test-VcpkgRoot $VcpkgRoot)) {
        Write-Warning "Ignoring invalid VCPKG_ROOT '$VcpkgRoot' (no scripts/buildsystems/vcpkg.cmake); using default."
        $VcpkgRoot = 'G:\Workspaces\vcpkg'
    }
}
if (-not (Test-VcpkgRoot $VcpkgRoot)) {
    throw "VCPKG_ROOT '$VcpkgRoot' has no scripts/buildsystems/vcpkg.cmake; pass -VcpkgRoot or set the VCPKG_ROOT environment variable."
}
$env:VCPKG_ROOT = $VcpkgRoot

# --- MSVC / vcvarsall -----------------------------------------------------
if (-not $VcvarsAll) {
    $candidates = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat'
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat'
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat'
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
    )
    $VcvarsAll = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $VcvarsAll -or -not (Test-Path $VcvarsAll)) {
    throw "vcvarsall.bat not found; pass -VcvarsAll or install VS 2022 BuildTools/Community/Professional/Enterprise."
}
$env:VCVARSALL_PATH = $VcvarsAll

# --- Ninja ------------------------------------------------------------------
# CMake's Ninja generator needs ninja on PATH at configure time. The machine-local
# copy (msys2) is prepended only when no ninja is already discoverable.
if (-not (Get-Command ninja -ErrorAction SilentlyContinue) -and
    (Test-Path 'C:\msys64\ucrt64\bin\ninja.exe')) {
    $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
    $env:NINJA_BIN = 'C:\msys64\ucrt64\bin\ninja.exe'
}

Write-Host ("build env: VCPKG_ROOT={0}" -f $env:VCPKG_ROOT)
Write-Host ("build env: VCVARSALL={0}" -f $env:VCVARSALL_PATH)
Write-Host ("build env: NINJA={0}" -f $(if ($env:NINJA_BIN) { $env:NINJA_BIN } else { 'PATH' }))
