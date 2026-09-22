#requires -Version 7.0
# Resolve tools without machine-specific drive letters. Explicit overrides must be valid.
[CmdletBinding()]
param(
    [string]$VcpkgRoot = '',
    [string]$VcvarsAll = '',
    [string]$CMakeExecutable = '',
    [string]$NinjaExecutable = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-BuildFile {
    param([string]$Path, [string]$Description)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description not found: '$Path'. Supply its explicit build.ps1 parameter."
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Test-VcpkgRoot {
    param([string]$Root)
    if (-not $Root) { return $false }
    try {
        return Test-Path -LiteralPath (Join-Path $Root 'scripts/buildsystems/vcpkg.cmake') -PathType Leaf
    } catch {
        return $false
    }
}

$buildRepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if ($VcpkgRoot) {
    if (-not (Test-VcpkgRoot $VcpkgRoot)) {
        throw "Invalid -VcpkgRoot '$VcpkgRoot': scripts/buildsystems/vcpkg.cmake is missing."
    }
} else {
    $vcpkgCommand = Get-Command vcpkg -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $vcpkgCandidates = @(
        $env:DVS_VCPKG_ROOT
        $env:VCPKG_ROOT
        $env:VCPKG_INSTALLATION_ROOT
        $(if ($vcpkgCommand) { Split-Path $vcpkgCommand.Source -Parent })
        (Join-Path (Split-Path $buildRepoRoot -Parent) 'vcpkg')
    ) | Where-Object { $_ } | Select-Object -Unique
    foreach ($candidate in $vcpkgCandidates) {
        if (Test-VcpkgRoot $candidate) {
            $VcpkgRoot = $candidate
            break
        }
        Write-Warning "Ignoring unavailable vcpkg candidate '$candidate'."
    }
    if (-not $VcpkgRoot) {
        throw 'vcpkg was not found. Pass -VcpkgRoot or set DVS_VCPKG_ROOT/VCPKG_ROOT.'
    }
}
$env:VCPKG_ROOT = (Resolve-Path -LiteralPath $VcpkgRoot).Path

if (-not $VcvarsAll) { $VcvarsAll = $env:VCVARSALL_PATH }
if (-not $VcvarsAll -and $env:VCINSTALLDIR) {
    $VcvarsAll = Join-Path $env:VCINSTALLDIR 'Auxiliary/Build/vcvarsall.bat'
}
if (-not $VcvarsAll) {
    $vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $vsInstallation = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0) { throw "vswhere failed (exit $LASTEXITCODE)." }
        if ($vsInstallation) {
            $VcvarsAll = Join-Path ($vsInstallation | Select-Object -First 1) 'VC/Auxiliary/Build/vcvarsall.bat'
        }
    }
}
$env:VCVARSALL_PATH = Resolve-BuildFile $VcvarsAll 'Visual Studio 2022 vcvarsall.bat'
$buildVcRoot = [System.IO.Path]::GetFullPath((Join-Path (Split-Path $env:VCVARSALL_PATH) '../..'))
$buildVsRoot = Split-Path $buildVcRoot -Parent

function Resolve-BuildTool {
    param([string]$Override, [string]$Name, [string]$Fallback)
    if ($Override) { return Resolve-BuildFile $Override $Name }
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($command) { return $command.Source }
    return Resolve-BuildFile $Fallback $Name
}

if (-not $CMakeExecutable) { $CMakeExecutable = $env:CMAKE_EXECUTABLE }
if (-not $NinjaExecutable) { $NinjaExecutable = $env:NINJA_BIN }
$env:CMAKE_EXECUTABLE = Resolve-BuildTool $CMakeExecutable 'cmake.exe' (Join-Path $buildVsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
$env:CTEST_EXECUTABLE = Resolve-BuildFile (Join-Path (Split-Path $env:CMAKE_EXECUTABLE) 'ctest.exe') 'ctest.exe (beside CMake)'
$env:NINJA_BIN = Resolve-BuildTool $NinjaExecutable 'ninja.exe' (Join-Path $buildVsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe')

$buildToolDirectories = @(
    (Split-Path $env:CMAKE_EXECUTABLE)
    (Split-Path $env:NINJA_BIN)
    (Join-Path $buildVcRoot 'Tools/Llvm/x64/bin')
) | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | Select-Object -Unique
$env:PATH = ($buildToolDirectories -join [System.IO.Path]::PathSeparator) +
    [System.IO.Path]::PathSeparator + $env:PATH
# The configure probe and every subsequent compile must agree on /showIncludes language.
$env:VSLANG = '1033'

Write-Host "build env: VCPKG_ROOT=$env:VCPKG_ROOT"
Write-Host "build env: VCVARSALL=$env:VCVARSALL_PATH"
Write-Host "build env: CMAKE=$env:CMAKE_EXECUTABLE"
Write-Host "build env: CTEST=$env:CTEST_EXECUTABLE"
Write-Host "build env: NINJA=$env:NINJA_BIN"
Write-Host "build env: VSLANG=$env:VSLANG"
