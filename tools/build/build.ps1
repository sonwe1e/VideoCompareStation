#requires -Version 7.0
# Unified entry point; can be invoked from any directory. Output streams in real time.
#   pwsh tools/build/build.ps1 -Preset dev -Doctor
#   pwsh tools/build/build.ps1 -Preset dev -Fresh -Test
#   pwsh tools/build/build.ps1 -Preset dev -Target format-check
#   pwsh tools/build/build.ps1 -Preset dev -Target lint
#   pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'ui.ImageReviewControllerTests'
[CmdletBinding()]
param(
    [string]$Preset = 'dev',
    [switch]$Configure,
    [switch]$Fresh,
    [switch]$Doctor,
    [switch]$UseInstalledDependencies,
    [string[]]$Target = @(),
    [switch]$Test,
    [string]$TestRegex = '',
    [switch]$TestOnly,
    [string]$VcpkgRoot = '',
    [string]$VcvarsAll = '',
    [string]$CMakeExecutable = '',
    [string]$NinjaExecutable = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$presets = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakePresets.json') -Raw |
    ConvertFrom-Json
$configurePreset = $presets.configurePresets | Where-Object name -CEQ $Preset
if (-not $configurePreset -or
    ($configurePreset.PSObject.Properties['hidden'] -and $configurePreset.hidden) -or
    -not ($presets.buildPresets | Where-Object name -CEQ $Preset)) {
    throw "Unknown build preset '$Preset'. Use a non-hidden preset from CMakePresets.json."
}
if ($TestOnly -and (-not $TestRegex -or $Fresh -or $Configure -or $UseInstalledDependencies -or $Target.Count -gt 0)) {
    throw '-TestOnly requires -TestRegex and cannot combine with configure/build options.'
}

. (Join-Path $PSScriptRoot 'env.ps1') -VcpkgRoot $VcpkgRoot -VcvarsAll $VcvarsAll -CMakeExecutable $CMakeExecutable -NinjaExecutable $NinjaExecutable
$binaryDir = Join-Path $repoRoot "out/build/$Preset"
$cacheFile = Join-Path $binaryDir 'CMakeCache.txt'

function Invoke-BuildTool {
    param([string]$Executable, [string[]]$ToolArguments)
    & $Executable @ToolArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed (exit $LASTEXITCODE): $Executable $($ToolArguments -join ' ')"
    }
}

function Import-VcEnvironment {
    # Only vcvarsall runs in cmd. Build arguments and test regex go directly to native argv.
    $selectedVcpkgRoot = $env:VCPKG_ROOT
    if ($env:VCVARSALL_PATH -match '[%"\r\n]') {
        throw 'vcvarsall.bat path cannot contain %, quotes, or newlines.'
    }
    $vcStart = [System.Diagnostics.ProcessStartInfo]::new()
    $vcStart.FileName = $env:ComSpec
    $vcStart.UseShellExecute = $false
    $vcStart.CreateNoWindow = $true
    $vcStart.RedirectStandardOutput = $true
    $vcStart.RedirectStandardError = $true
    # cmd has its own quoting rules; ArgumentList would add C-runtime backslash escapes.
    $vcStart.Arguments = '/d /s /c "call "{0}" x64 >nul && set"' -f $env:VCVARSALL_PATH
    $vcProcess = [System.Diagnostics.Process]::Start($vcStart)
    try {
        $vcErrorTask = $vcProcess.StandardError.ReadToEndAsync()
        $vcOutput = $vcProcess.StandardOutput.ReadToEnd()
        $vcProcess.WaitForExit()
        $vcErrors = $vcErrorTask.GetAwaiter().GetResult()
        if ($vcProcess.ExitCode -ne 0) {
            throw "vcvarsall.bat failed (exit $($vcProcess.ExitCode)): $vcErrors"
        }
        foreach ($line in ($vcOutput -split '\r?\n')) {
            if ($line -match '^([^=]+)=(.*)$') {
                [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            }
        }
    } finally {
        $vcProcess.Dispose()
    }
    $env:VSLANG = '1033'
    # VS may inject its own bundled vcpkg; preserve the resolver/user selection.
    $env:VCPKG_ROOT = $selectedVcpkgRoot
}

function Get-CacheProblems {
    if (-not (Test-Path -LiteralPath $cacheFile)) { return }
    $cache = @{}
    foreach ($line in (Get-Content -LiteralPath $cacheFile)) {
        if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    $expected = @{
        CMAKE_HOME_DIRECTORY = $repoRoot
        CMAKE_TOOLCHAIN_FILE = Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'
        CMAKE_MAKE_PROGRAM = $env:NINJA_BIN
    }
    foreach ($key in $expected.Keys) {
        if ($cache.ContainsKey($key) -and
            [System.IO.Path]::GetFullPath($cache[$key]) -ine
            [System.IO.Path]::GetFullPath($expected[$key])) {
            "$key is '$($cache[$key])'; selected '$($expected[$key])'."
        }
    }
    foreach ($key in @('CMAKE_CXX_COMPILER', 'CMAKE_AR', 'CLANG_FORMAT_EXECUTABLE',
            'CLANG_TIDY_EXECUTABLE', 'QMLFORMAT_EXECUTABLE', 'QMLLINT_EXECUTABLE')) {
        if ($cache.ContainsKey($key) -and
            -not (Test-Path -LiteralPath $cache[$key] -PathType Leaf)) {
            "$key points to an unavailable tool: '$($cache[$key])'."
        }
    }
}

function Test-HeaderDependencies {
    $dependencyObject = Join-Path $binaryDir 'src/domain/CMakeFiles/dvs_domain.dir/src/ComparisonSelection.cpp.obj'
    if (Test-Path -LiteralPath $dependencyObject -PathType Leaf) {
        Invoke-BuildTool $env:CMAKE_EXECUTABLE @(
            "-DDVS_BINARY_DIR=$binaryDir", "-DDVS_SOURCE_DIR=$repoRoot",
            "-DDVS_NINJA_EXECUTABLE=$env:NINJA_BIN", '-P',
            (Join-Path $repoRoot 'cmake/CheckMsvcDependencies.cmake'))
    }
}

$buildLock = $null
if (-not $Doctor) {
    $lockDirectory = Join-Path $repoRoot 'out/build'
    New-Item -ItemType Directory -Path $lockDirectory -Force | Out-Null
    try {
        $buildLock = [IO.File]::Open((Join-Path $lockDirectory ".$Preset.lock"),
            [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    } catch [IO.IOException] {
        throw "Another build or test is using preset '$Preset'. Wait for it to finish before retrying."
    }
}
Push-Location -LiteralPath $repoRoot
try {
    Import-VcEnvironment
    $cacheProblems = @(Get-CacheProblems)
    if ($cacheProblems.Count -gt 0) {
        $diagnostic = ($cacheProblems -join [Environment]::NewLine) +
            [Environment]::NewLine +
            "Run build.ps1 -Preset $Preset -Fresh to reconfigure and rebuild generated outputs."
        if (-not $Fresh -or $Doctor) { throw $diagnostic }
        Write-Warning $diagnostic
    }
    if ($Doctor) {
        Invoke-BuildTool $env:CMAKE_EXECUTABLE @('--version')
        Invoke-BuildTool $env:CTEST_EXECUTABLE @('--version')
        Invoke-BuildTool $env:NINJA_BIN @('--version')
        Test-HeaderDependencies
        Write-Host "Build environment OK: preset=$Preset; repository=$repoRoot"
        return
    }
    if ($TestOnly -and -not (Test-Path -LiteralPath $cacheFile)) {
        throw 'No configured build exists. Run with -Test to build before testing.'
    }
    if (-not $Fresh) { Test-HeaderDependencies }
    if (-not $TestOnly) {
        if ($Fresh -or $Configure -or $UseInstalledDependencies -or -not (Test-Path -LiteralPath $cacheFile)) {
            $configureArgs = @('--preset', $Preset, "-DCMAKE_MAKE_PROGRAM=$env:NINJA_BIN")
            if ($Fresh) { $configureArgs += '--fresh' }
            if ($UseInstalledDependencies) {
                $configureArgs += '-DVCPKG_MANIFEST_INSTALL=OFF'
                Write-Host 'Using preinstalled dependencies; CMake will validate required packages.'
            }
            Invoke-BuildTool $env:CMAKE_EXECUTABLE $configureArgs
        }
        if ($Fresh -and (Test-Path -LiteralPath (Join-Path $binaryDir 'build.ninja'))) {
            # Regenerate paths first, then clean objects whose header dependencies were lost.
            # Never ask a relocated cache to clean outputs in its previous workspace.
            Invoke-BuildTool $env:NINJA_BIN @('-C', $binaryDir, '-t', 'clean')
        }
        $buildArgs = @('--build', '--preset', $Preset)
        if ($Target.Count -gt 0) { $buildArgs += @('--target') + $Target }
        Invoke-BuildTool $env:CMAKE_EXECUTABLE $buildArgs
        Test-HeaderDependencies
    }
    if ($Test -or $TestOnly) {
        $testArgs = @('--preset', $Preset, '--output-on-failure', '--no-tests=error')
        if ($TestRegex) { $testArgs += @('-R', $TestRegex) }
        Invoke-BuildTool $env:CTEST_EXECUTABLE $testArgs
    }
    Write-Host "Done: preset=$Preset"
} finally {
    Pop-Location
    if ($null -ne $buildLock) { $buildLock.Dispose() }
}
