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
        if (-not $cache.ContainsKey($key)) { continue }
        if ([string]::IsNullOrWhiteSpace($cache[$key])) {
            # A configure that failed midway can leave an empty value behind; treat that
            # cache as stale instead of crashing on GetFullPath('').
            [pscustomobject]@{ Kind = 'stale'; Message = "$key is empty; the cache is from a failed configure." }
            continue
        }
        if ([System.IO.Path]::GetFullPath($cache[$key]) -ine
            [System.IO.Path]::GetFullPath($expected[$key])) {
            if ($key -eq 'CMAKE_TOOLCHAIN_FILE') {
                $cachedRoot = Get-VcpkgRootFromToolchain $cache[$key]
                if (Test-VcpkgCmakeEquivalent $cachedRoot $env:VCPKG_ROOT) { continue }
                if ($cachedRoot -and
                    (Test-Path -LiteralPath (Join-Path $cachedRoot 'scripts/buildsystems/vcpkg.cmake') -PathType Leaf) -and
                    (-not (Test-Path -LiteralPath (Join-Path $cachedRoot 'versions/baseline.json') -PathType Leaf))) {
                    # A different toolchain path is not stale-cache evidence when the cached
                    # vcpkg cannot resolve versions at all: the cache stays usable as long as
                    # dependency installation is off, which is not what -Fresh changes.
                    [pscustomobject]@{
                        Kind = 'incomplete-vcpkg'
                        Message = ("CMAKE_TOOLCHAIN_FILE uses '$cachedRoot', which ships no" +
                            ' versions/baseline.json and therefore cannot resolve the manifest.')
                    }
                    continue
                }
            }
            [pscustomobject]@{
                Kind = 'stale'
                Message = "$key is '$($cache[$key])'; selected '$($expected[$key])'."
            }
        }
    }
    foreach ($key in @('CMAKE_CXX_COMPILER', 'CMAKE_AR', 'CLANG_FORMAT_EXECUTABLE',
            'CLANG_TIDY_EXECUTABLE', 'QMLFORMAT_EXECUTABLE', 'QMLLINT_EXECUTABLE')) {
        if ($cache.ContainsKey($key) -and
            -not (Test-Path -LiteralPath $cache[$key] -PathType Leaf)) {
            [pscustomobject]@{
                Kind = 'stale'
                Message = "$key points to an unavailable tool: '$($cache[$key])'."
            }
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

function Assert-BuildEnvironment {
    # Turns environment defects that used to surface as a red quality gate, a deep vcpkg error,
    # or an avoidable full rebuild into one explicit verdict with the exact remedy. Checks are
    # read-only, so this runs before the lock is meaningful and before anything is written.
    param([bool]$FatalErrors)
    $findings = @(Invoke-BuildEnvironmentChecks $binaryDir)
    Show-BuildEnvironmentFindings $findings
    $errors = @($findings | Where-Object { $_.Level -eq 'error' })
    if ($errors.Count -eq 0) { return }
    $separator = [Environment]::NewLine
    $detail = (($errors | ForEach-Object { $_.Message }) -join $separator) + $separator +
        (($errors | Where-Object { $_.Fix } | ForEach-Object { $_.Fix } |
            Select-Object -Unique | ForEach-Object { "Fix: $_" }) -join $separator)
    if ($FatalErrors) {
        throw "The build environment cannot produce correct incremental builds.$separator$detail"
    }
    Write-Warning "The build environment has problems that affect incremental builds.$separator$detail"
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
        $separator = [Environment]::NewLine
        $staleProblems = @($cacheProblems | Where-Object { $_.Kind -eq 'stale' })
        # A cached vcpkg root that cannot resolve versions is a capability problem, not a
        # stale cache: dependency installation has to stay off, which -Fresh does not do.
        $incompleteProblems = @($cacheProblems | Where-Object { $_.Kind -eq 'incomplete-vcpkg' })
        $messages = @($cacheProblems | ForEach-Object { $_.Message })
        if ($staleProblems.Count -gt 0) {
            $messages += "Run build.ps1 -Preset $Preset -Fresh to reconfigure and rebuild generated outputs."
        }
        if ($incompleteProblems.Count -gt 0) {
            $messages += ("Reconfigure with -Preset $Preset -UseInstalledDependencies so the existing" +
                ' out/vcpkg install tree is reused instead of resolving the manifest.')
            $messages += ("The resolved vcpkg root '$env:VCPKG_ROOT' has no .git either, so manifest" +
                ' resolution is unavailable on this machine and dependency installation stays off.')
        }
        $diagnostic = $messages -join $separator
        $blocked = ($staleProblems.Count -gt 0) -or
            ($incompleteProblems.Count -gt 0 -and -not $UseInstalledDependencies)
        if ($blocked -and (-not $Fresh -or $Doctor -or
                ($incompleteProblems.Count -gt 0 -and $staleProblems.Count -eq 0))) {
            throw $diagnostic
        }
        Write-Warning $diagnostic
    }
    if ($Doctor) {
        Invoke-BuildTool $env:CMAKE_EXECUTABLE @('--version')
        Invoke-BuildTool $env:CTEST_EXECUTABLE @('--version')
        Invoke-BuildTool $env:NINJA_BIN @('--version')
        # A broken environment is what -Doctor exists to find, so it fails here.
        Assert-BuildEnvironment -FatalErrors $true
        Test-HeaderDependencies
        Write-Host "Build environment OK: preset=$Preset; repository=$repoRoot"
        return
    }
    if ($TestOnly -and -not (Test-Path -LiteralPath $cacheFile)) {
        throw 'No configured build exists. Run with -Test to build before testing.'
    }
    # Outside -Doctor the findings are advisory: the build and the post-build header
    # dependency gate remain the authority, and failing early would misreport a recoverable
    # machine state as a source problem.
    Assert-BuildEnvironment -FatalErrors $false
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
