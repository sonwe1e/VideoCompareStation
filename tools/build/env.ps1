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

function Test-VcpkgManifestResolution {
    # vcpkg's builtin registry reads ports and the baseline through git, so a manifest
    # install needs the root's own repository. ZIP-extracted trees ship without it, and
    # vcpkg then falls back to an unrelated ancestor repository instead of failing loudly.
    param([string]$Root)
    if (-not $Root) { return $false }
    try {
        return Test-Path -LiteralPath (Join-Path $Root '.git') -PathType Container
    } catch {
        return $false
    }
}

function Get-VcpkgCandidateStatus {
    # Only consulted while reporting; resolution itself stays silent so that the common
    # case does not print a warning for every unrelated stale VCPKG_ROOT-style variable.
    param([string[]]$Candidates)
    $status = foreach ($candidate in $Candidates) {
        [pscustomobject]@{
            Path = $candidate
            Toolchain = Test-VcpkgRoot $candidate
            ManifestResolution = Test-VcpkgManifestResolution $candidate
        }
    }
    return @($status)
}

function Get-VcpkgRootFromToolchain {
    # CMakeCache stores the toolchain as <root>/scripts/buildsystems/vcpkg.cmake. The path is
    # normalized first: Split-Path -LiteralPath cannot be combined with -Parent, and a cache
    # written elsewhere may use forward slashes.
    param([string]$ToolchainFile)
    if (-not $ToolchainFile) { return '' }
    try {
        $normalized = [IO.Path]::GetFullPath($ToolchainFile)
        $buildsystems = [IO.Path]::GetDirectoryName($normalized)
        $scripts = [IO.Path]::GetDirectoryName($buildsystems)
        if ([IO.Path]::GetFileName($buildsystems) -ine 'buildsystems') { return '' }
        if ([IO.Path]::GetFileName($scripts) -ine 'scripts') { return '' }
        return [IO.Path]::GetDirectoryName($scripts)
    } catch {
        return ''
    }
}

function Test-VcpkgCmakeEquivalent {
    # Visual Studio ships a bundled vcpkg beside the one the resolver selects. When the
    # toolchain contents and the versions database both match, the two roots are
    # interchangeable for configuring, so a different path is not stale-cache evidence:
    # treating it as one forces a full rebuild for no benefit.
    param([string]$Left, [string]$Right)
    if (-not $Left -or -not $Right) { return $false }
    try {
        if ([IO.Path]::GetFullPath($Left) -ieq [IO.Path]::GetFullPath($Right)) { return $false }
        $leftFile = Join-Path $Left 'scripts/buildsystems/vcpkg.cmake'
        $rightFile = Join-Path $Right 'scripts/buildsystems/vcpkg.cmake'
        if (-not (Test-Path -LiteralPath $leftFile -PathType Leaf)) { return $false }
        if (-not (Test-Path -LiteralPath $rightFile -PathType Leaf)) { return $false }
        if ((Get-FileHash -LiteralPath $leftFile -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $rightFile -Algorithm SHA256).Hash) {
            return $false
        }
        return (Test-Path -LiteralPath (Join-Path $Left 'versions/baseline.json') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $Right 'versions/baseline.json') -PathType Leaf)
    } catch {
        return $false
    }
}

$buildRepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$vcpkgCandidates = @()
if ($VcpkgRoot) {
    if (-not (Test-VcpkgRoot $VcpkgRoot)) {
        throw "Invalid -VcpkgRoot '$VcpkgRoot': scripts/buildsystems/vcpkg.cmake is missing."
    }
} else {
    $vcpkgCommand = Get-Command vcpkg -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    # The pipeline collapses a single surviving candidate into a bare string, which defeats
    # later .Count use under StrictMode; force the array shape here.
    $vcpkgCandidates = @(
        @(
            $env:DVS_VCPKG_ROOT
            $env:VCPKG_ROOT
            $env:VCPKG_INSTALLATION_ROOT
            $(if ($vcpkgCommand) { Split-Path $vcpkgCommand.Source -Parent })
            (Join-Path (Split-Path $buildRepoRoot -Parent) 'vcpkg')
        ) | Where-Object { $_ } | Select-Object -Unique
    )
    foreach ($candidate in $vcpkgCandidates) {
        if (Test-VcpkgRoot $candidate) {
            $VcpkgRoot = $candidate
            break
        }
    }
    if (-not $VcpkgRoot) {
        $availability = (Get-VcpkgCandidateStatus $vcpkgCandidates | ForEach-Object {
                "  $($_.Path) (toolchain: $(if ($_.Toolchain) { 'yes' } else { 'no' }))"
            }) -join [Environment]::NewLine
        throw ("vcpkg was not found. Pass -VcpkgRoot or set DVS_VCPKG_ROOT/VCPKG_ROOT." +
            [Environment]::NewLine + "Candidates considered:" + [Environment]::NewLine + $availability)
    }
}
$env:VCPKG_ROOT = (Resolve-Path -LiteralPath $VcpkgRoot).Path
$env:DVS_VCPKG_CANDIDATES = if (@($vcpkgCandidates).Count -gt 0) { $vcpkgCandidates -join ';' } else { '' }
$env:DVS_VCPKG_ROOT = $env:VCPKG_ROOT

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

# --- Environment preflight -------------------------------------------------------------------
# Tool discovery proves nothing about whether the selected tools actually work together. The
# checks below turn the three failure modes that used to surface only as a red quality gate or a
# confusing mid-build error into one fast, explicit verdict with the exact remedy.

function New-EnvironmentFinding {
    param([ValidateSet('ok', 'info', 'warn', 'error')][string]$Level, [string]$Message, [string]$Fix = '')
    return [pscustomobject]@{ Level = $Level; Message = $Message; Fix = $Fix }
}

function Get-VcpkgCandidateReport {
    param([string]$SelectedRoot)
    $findings = @()
    if (-not $env:DVS_VCPKG_CANDIDATES) { return $findings }
    $candidates = @($env:DVS_VCPKG_CANDIDATES -split ';' | Where-Object { $_ })
    $status = Get-VcpkgCandidateStatus $candidates
    foreach ($entry in $status) {
        # A candidate without a toolchain is normal for unset or invented environment
        # variables; only report it when an explicit variable pointed at a real directory.
        if ($entry.Toolchain) { continue }
        if (-not (Test-Path -LiteralPath $entry.Path)) {
            $findings += New-EnvironmentFinding warn `
                "vcpkg candidate '$($entry.Path)' does not exist; ignoring it."
            continue
        }
        $findings += New-EnvironmentFinding warn `
            "vcpkg candidate '$($entry.Path)' has no scripts/buildsystems/vcpkg.cmake; ignoring it."
    }
    return $findings
}

function Get-VcpkgManifestResolutionFinding {
    param([string]$SelectedRoot)
    if (Test-VcpkgManifestResolution $SelectedRoot) {
        return New-EnvironmentFinding ok "vcpkg root has its own git repository; manifest resolution available."
    }
    return New-EnvironmentFinding warn `
        ("vcpkg root '$SelectedRoot' has no .git, so vcpkg cannot resolve the manifest." +
        " Version resolution reads ports and the baseline through git and otherwise fails with" +
        " 'fatal: not a git repository'.") `
        'Run local configuration with -UseInstalledDependencies (VCPKG_MANIFEST_INSTALL=OFF).'
}

function Get-MsvcToolDirectory {
    $toolsInstallDirectory = $env:VCToolsInstallDir
    if ([string]::IsNullOrWhiteSpace($toolsInstallDirectory)) { return $null }
    $hostArch = if ($env:VSCMD_ARG_HOST_ARCH) { $env:VSCMD_ARG_HOST_ARCH } else { 'x64' }
    $targetArch = if ($env:VSCMD_ARG_TGT_ARCH) { $env:VSCMD_ARG_TGT_ARCH } else { 'x64' }
    return Join-Path (Join-Path (Join-Path (Join-Path $toolsInstallDirectory 'bin') "Host$hostArch") $targetArch) ''
}

function Get-MsvcEnglishResourceFinding {
    # VSLANG=1033 only selects a resource directory; it cannot invent one. A toolset that
    # ships only a localized resource directory makes cl.exe emit that language no matter
    # what VSLANG says, which is the precondition for the prefix defect checked below.
    $toolDirectory = Get-MsvcToolDirectory
    if (-not $toolDirectory -or -not (Test-Path -LiteralPath $toolDirectory -PathType Container)) {
        return New-EnvironmentFinding info 'MSVC tool directory is unknown before vcvars; language resources not inspected.'
    }
    if (Test-Path -LiteralPath (Join-Path $toolDirectory '1033') -PathType Container) {
        return New-EnvironmentFinding ok 'MSVC English (1033) diagnostic resources are installed.'
    }
    $installed = @(Get-ChildItem -LiteralPath $toolDirectory -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^[0-9]{4}$' } | ForEach-Object { $_.Name })
    return New-EnvironmentFinding error `
        ("MSVC has no English (1033) diagnostic resources; found only: $($installed -join ', ')." +
        ' VSLANG=1033 is therefore ineffective and cl.exe emits localized /showIncludes text.') `
        ('Add the English language pack in Visual Studio Installer (Modify > Language packs > English)' +
        ' for Visual Studio Build Tools 2022. No command-line or environment override substitutes for it.')
}

function Get-MsvcIncludePrefix {
    # Returns the /showIncludes prefix exactly as Ninja will observe it, or $null when the
    # probe cannot run. cl.exe writes its diagnostics on stdout in the console output code
    # page, so the captured bytes are decoded with that code page and compared after the
    # same UTF-8 round trip CMake performs when it writes the Ninja manifest.
    if (-not $env:VCToolsInstallDir) { return $null }
    $toolDirectory = Get-MsvcToolDirectory
    if (-not $toolDirectory) { return $null }
    $compiler = Join-Path $toolDirectory 'cl.exe'
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { return $null }
    $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("dvs-prefix-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryDirectory -Force | Out-Null
    try {
        Set-Content -LiteralPath (Join-Path $temporaryDirectory 'probe.cpp') `
            -Value "#include <cstdio>`nint main() { return 0; }`n" -Encoding utf8
        $psi = [Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $compiler
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        foreach ($argument in @(
                '/nologo', '/showIncludes', '/c'
                (Join-Path $temporaryDirectory 'probe.cpp')
                "/Fo$(Join-Path $temporaryDirectory 'probe.obj')"
            )) {
            $psi.ArgumentList.Add($argument)
        }
        $process = [Diagnostics.Process]::Start($psi)
        try {
            # Read the raw byte stream. Decoding through StreamReader would apply one fixed
            # code page and silently destroy the very bytes under test.
            $capture = [IO.MemoryStream]::new()
            $outputTask = $process.StandardOutput.BaseStream.CopyToAsync($capture)
            $errorTask = $process.StandardError.ReadToEndAsync()
            $process.WaitForExit()
            $outputTask.GetAwaiter().GetResult() | Out-Null
            $errorTask.GetAwaiter().GetResult() | Out-Null
            $bytes = $capture.ToArray()
        } finally {
            $process.Dispose()
        }
        if ($bytes.Length -eq 0) { return $null }
        $text = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage).
            GetString($bytes)
        $lines = $text -split "`r?`n"
        # The first stdout line is the source file name; the include entries follow it. Each
        # include entry is "<prefix><padding><absolute path>", and the ASCII path anchor lets
        # the regex backtrack over the localized prefix and its alignment spaces alike.
        for ($index = 1; $index -lt $lines.Length; $index++) {
            $candidate = $lines[$index]
            if ($candidate -match '^(.*?)\s+(?:[A-Za-z]:[\\/]|/|\\\\)\S') {
                # Trailing alignment spaces vary with include depth; compare the prefix text.
                return $Matches[1].TrimEnd()
            }
        }
        return $null
    } catch {
        return $null
    } finally {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Get-MsvcDependencyPrefixFinding {
    param([string]$BinaryDirectory)
    if (-not $env:VCToolsInstallDir) {
        return New-EnvironmentFinding info 'MSVC is not in scope; /showIncludes prefix not compared.'
    }
    $observed = Get-MsvcIncludePrefix
    if (-not $observed) {
        return New-EnvironmentFinding warn `
            'Could not capture cl.exe /showIncludes output; header dependency tracking was not verified.'
    }
    $rulesFile = Join-Path $BinaryDirectory 'CMakeFiles\rules.ninja'
    if (-not (Test-Path -LiteralPath $rulesFile -PathType Leaf)) {
        return New-EnvironmentFinding info `
            'No generated CMakeFiles/rules.ninja yet; /showIncludes prefix not compared. The configure that follows reports it.'
    }
    $rulesText = [IO.File]::ReadAllText($rulesFile, [Text.Encoding]::UTF8)
    $recorded = ([regex]::Match($rulesText, '(?m)^msvc_deps_prefix = (.*)$')).Groups[1].Value
    if (-not $recorded) {
        return New-EnvironmentFinding info 'CMake did not record msvc_deps_prefix; nothing to compare.'
    }
    if ($recorded.TrimEnd() -ceq $observed) {
        return New-EnvironmentFinding ok `
            "/showIncludes prefix matches the generated Ninja rules ('$observed'); header dependencies are tracked."
    }
    $remedy = 'Install the MSVC English language pack (Visual Studio Installer > Modify > ' +
    'Language packs > English), then run tools/build/build.ps1 -Preset <preset> -Fresh ' +
    '-UseInstalledDependencies and rebuild completely. Reconfiguring without the language ' +
    'pack reproduces this defect on every full rebuild.'
    $recordedDisplay = $recorded.TrimEnd()
    return New-EnvironmentFinding error `
        ("Ninja cannot recognize cl.exe /showIncludes output, so no object records header" +
        " dependencies and header edits silently leave stale objects." +
        [Environment]::NewLine +
        "    cl.exe emits      : '$observed' ($([Text.Encoding]::UTF8.GetByteCount($observed)) UTF-8 bytes)" +
        [Environment]::NewLine +
        "    CMake recorded    : '$recordedDisplay' ($([Text.Encoding]::UTF8.GetByteCount($recordedDisplay)) UTF-8 bytes)" +
        [Environment]::NewLine +
        "    generated rules   : $rulesFile") $remedy
}

function Invoke-BuildEnvironmentChecks {
    # Every check is read-only and safe to run before a lock is taken or a build is started.
    param([string]$BinaryDirectory)
    $findings = @()
    $findings += Get-VcpkgCandidateReport $env:VCPKG_ROOT
    $findings += Get-VcpkgManifestResolutionFinding $env:VCPKG_ROOT
    $findings += Get-MsvcEnglishResourceFinding
    $findings += Get-MsvcDependencyPrefixFinding $BinaryDirectory
    return $findings
}

function Show-BuildEnvironmentFindings {
    param([object[]]$Findings)
    foreach ($finding in $Findings) {
        if ($finding.Level -eq 'ok') { continue }
        $prefix = switch ($finding.Level) {
            'error' { 'environment error' }
            'warn' { 'environment warning' }
            default { 'environment note' }
        }
        Write-Host "$($prefix): $($finding.Message)"
        if ($finding.Fix) { Write-Host "  fix: $($finding.Fix)" }
    }
}
