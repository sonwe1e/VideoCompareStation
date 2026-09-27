<#
    Installs the English (1033) diagnostic resources for the MSVC toolset that Build Tools 2022
    installed, by fetching the exact payloads the Visual Studio installer itself would use.

    Why this exists
    ---------------
    cl.exe emits its /showIncludes prefix in the language whose resource directory exists under
    VC\Tools\MSVC\<ver>\bin\Host<x64|x86>\<x64|x86>\. MSVC has no "language pack" component in
    the Visual Studio catalog: each locale is a separate resource package, for example
    Microsoft.VC.14.44.17.14.Tools.HostX64.TargetX64.Res.base with language en-US or zh-CN,
    and each payload is a small VSIX holding only the *ui.dll files for that locale. A machine
    installed in Chinese therefore gets 2052/ and nothing else, VSLANG=1033 cannot help, and
    Ninja never matches the localized prefix against the generated Ninja rules — so no object
    records its header dependencies.

    What it does
    ------------
    1. Reads the Visual Studio channel catalog that the installer already cached locally.
    2. Selects every resource package whose language is en-US and whose payload path lands in
       the toolset found on this machine (all host/target architecture pairs).
    3. Downloads each payload and verifies the SHA256 recorded in the catalog. A mismatch
       aborts without touching the installation.
    4. Extracts the locale directory into the toolset's bin directory.
    5. Re-checks that 1033\ now exists for every architecture, and that cl.exe actually reports
       the English prefix.

    Requires an elevated PowerShell 7 window: the toolset lives under Program Files.
#>
#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$InstallPath = '',
    [string]$CatalogPath = '',
    [string]$CacheDirectory = '',
    [ValidateSet('en-US')]
    [string]$Language = 'en-US',
    # Overrides the destination root. Used to rehearse the download and extraction pipeline
    # without touching the Visual Studio installation; also the escape hatch for a
    # non-standard layout. Defaults to the resolved installation path.
    [string]$TargetRoot = '',
    [switch]$VerifyOnly,
    [switch]$KeepDownload
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------- locating things

function Resolve-InstallPath {
    param([string]$Explicit)
    if ($Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $found = & $vswhere -latest -products '*' -version '[17.0,18.0)' `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $found) { return ($found | Select-Object -First 1).Trim() }
    }
    $fallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools'
    if (Test-Path -LiteralPath $fallback -PathType Container) { return $fallback }
    throw 'Could not locate a Visual Studio 2022 installation with the C++ toolset. Pass -InstallPath.'
}

function Resolve-CatalogPath {
    param([string]$Explicit)
    if ($Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
    $roots = @(
        'C:\ProgramData\Microsoft\VisualStudio\Packages\_Channels'
        'C:\ProgramData\Microsoft\VisualStudio\Packages\_Instances'
    )
    $candidates = foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        Get-ChildItem -LiteralPath $root -Recurse -File -Filter 'catalog.json' -ErrorAction SilentlyContinue |
            Where-Object { $_.Length -gt 1MB }
    }
    $best = $candidates | Sort-Object Length -Descending | Select-Object -First 1
    if (-not $best) {
        throw ('No cached Visual Studio catalog found. Run the Visual Studio Installer once, or ' +
        'pass -CatalogPath pointing at a catalog.json.')
    }
    return $best.FullName
}

function Get-ToolsetLayout {
    param([string]$Root)
    $toolsRoot = Join-Path $Root 'VC\Tools\MSVC'
    if (-not (Test-Path -LiteralPath $toolsRoot -PathType Container)) { return @() }
    $layout = foreach ($toolchain in (Get-ChildItem -LiteralPath $toolsRoot -Directory | Sort-Object Name -Descending)) {
        foreach ($hostArch in @('Hostx64', 'Hostx86')) {
            foreach ($targetArch in @('x64', 'x86')) {
                $bin = Join-Path (Join-Path (Join-Path $toolchain.FullName 'bin') $hostArch) $targetArch
                if (-not (Test-Path -LiteralPath $bin -PathType Container)) { continue }
                $locales = @(Get-ChildItem -LiteralPath $bin -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^[0-9]{4}$' } | ForEach-Object { $_.Name })
                [pscustomobject]@{
                    Toolchain = $toolchain.Name
                    Bin = $bin
                    Locales = $locales
                    HasEnglish = $locales -contains '1033'
                }
            }
        }
    }
    return @($layout)
}

Write-Host '=== Current MSVC diagnostic resources ==='
$resolvedInstall = Resolve-InstallPath $InstallPath
Write-Host "installation : $resolvedInstall"
$before = Get-ToolsetLayout $resolvedInstall
if ($before.Count -eq 0) { throw "No MSVC toolset found under '$resolvedInstall'." }
$before | Format-Table Toolchain, @{ n = 'Locales'; e = { $_.Locales -join ',' } }, HasEnglish -AutoSize

$missing = @($before | Where-Object { -not $_.HasEnglish })
if ($missing.Count -eq 0) {
    Write-Host 'English (1033) resources are already present for every toolset architecture.'
    if ($VerifyOnly) { exit 0 }
}

if ($VerifyOnly) {
    Write-Host 'VerifyOnly: no changes made.'
    exit ($(if ($missing.Count -gt 0) { 1 } else { 0 }))
}

$writeRoot = if ($TargetRoot) { [IO.Path]::GetFullPath($TargetRoot) } else { $resolvedInstall }
$intoInstallation = $writeRoot -ieq $resolvedInstall
if ($intoInstallation) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw ('This script writes into the Visual Studio installation and needs an elevated ' +
            'PowerShell 7 window. Reopen PowerShell as Administrator and run it again.')
    }
}
Write-Host "write root   : $writeRoot"

# ---------------------------------------------------------------- selecting packages

$resolvedCatalog = Resolve-CatalogPath $CatalogPath
Write-Host ''
Write-Host "catalog      : $resolvedCatalog"
Write-Host 'reading catalog (about 19k packages; this takes a few seconds)...'
$catalog = Get-Content -LiteralPath $resolvedCatalog -Raw | ConvertFrom-Json

# Each entry of interest looks like
#   Microsoft.VC.14.44.17.14.Tools.HostX64.TargetX64.Res.base
# carrying a payload such as ...\Res.base.enu.vsix. The package version is a servicing number
# (for example 14.44.35228) and is NOT the toolchain directory: the payload paths inside
# decide where the files land, so selection keys on the 14.44 segment while the destination
# comes from the payload itself.
$toolsetSegments = @($before | ForEach-Object {
        $parts = $_.Toolchain -split '\.'
        if ($parts.Count -ge 2) { "$($parts[0]).$($parts[1])" }
    } | Select-Object -Unique)
$pattern = '^Microsoft\.VC\.(?<toolset>[0-9]+\.[0-9]+)\.[0-9]+\.[0-9]+\.(?:Premium\.)?Tools\.Host[xX]?[0-9]+\.Target[xX]?[0-9]+\.Res\.base$'
$selected = @(foreach ($package in $catalog.packages) {
        # Most catalog entries omit language and payloads entirely; StrictMode rejects that.
        $languageProperty = $package.PSObject.Properties['language']
        if (-not $languageProperty -or $languageProperty.Value -ne $Language) { continue }
        $match = [regex]::Match([string]$package.id, $pattern)
        if (-not $match.Success) { continue }
        if ($toolsetSegments -notcontains $match.Groups['toolset'].Value) { continue }
        $payloadsProperty = $package.PSObject.Properties['payloads']
        if (-not $payloadsProperty) { continue }
        $payload = @($payloadsProperty.Value)[0]
        if (-not $payload) { continue }
        [pscustomobject]@{
            Id = [string]$package.id
            PayloadFile = [string]$payload.fileName
            Sha256 = ([string]$payload.sha256).ToUpperInvariant()
            Size = [int]$payload.size
            Url = [string]$payload.url
        }
    })

if (-not $selected) {
    throw ("No '$Language' resource packages found in the catalog for toolset(s) " +
        "$($toolsetSegments -join ', '). Run the Visual Studio Installer once so it refreshes " +
        'the catalog, then retry.')
}

Write-Host ''
Write-Host "Selected $(@($selected).Count) payload(s) for $($Language):"
$selected | ForEach-Object { Write-Host "  $($_.Id)  -> $($_.PayloadFile)" }
Write-Host "total download: $([math]::Round((($selected | Measure-Object Size -Sum).Sum) / 1KB, 0)) KB"

# ---------------------------------------------------------------- downloading and verifying

if (-not $CacheDirectory) { $CacheDirectory = Join-Path $env:TEMP "dvs-msvc-$Language" }
New-Item -ItemType Directory -Path $CacheDirectory -Force | Out-Null
Write-Host ''
Write-Host "cache        : $CacheDirectory"

$downloaded = @()
foreach ($item in $selected) {
    $destination = Join-Path $CacheDirectory $item.PayloadFile
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $existing = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
        if ($existing -eq $item.Sha256) {
            Write-Host "cached     : $($item.PayloadFile)"
            $downloaded += $destination
            continue
        }
        Write-Host "stale      : $($item.PayloadFile) (hash mismatch, re-downloading)"
        Remove-Item -LiteralPath $destination -Force
    }
    Write-Host "downloading: $($item.PayloadFile)"
    # WebClient follows the CDN redirect and does not depend on the PowerShell edition's
    # web cmdlets, which differ between Windows PowerShell and PowerShell 7.
    $client = [Net.WebClient]::new()
    try {
        $client.DownloadFile($item.Url, $destination)
    } finally {
        $client.Dispose()
    }
    $actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if ($actual -ne $item.Sha256) {
        Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
        throw ("SHA256 mismatch for $($item.PayloadFile).`n  expected $($item.Sha256)`n  actual   $actual`n" +
            'Nothing was written to the Visual Studio installation.')
    }
    $downloaded += $destination
}
Write-Host 'All payloads verified against the catalog SHA256.'

# ---------------------------------------------------------------- installing

Add-Type -AssemblyName System.IO.Compression.FileSystem
$installed = 0
foreach ($archive in $downloaded) {
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            # Payload paths appear as Contents/VC/... or /Contents/VC/...; both forms occur.
            if ($entry.FullName -notmatch '^/?Contents/(?<rel>.+)$') { continue }
            if ($entry.Length -eq 0) { continue }
            $relative = $Matches['rel'] -replace '/', '\'
            $target = [IO.Path]::GetFullPath((Join-Path $writeRoot $relative))
            if (-not $target.StartsWith($writeRoot, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to write outside the target root: $target"
            }
            # Validate the structure against the real installation, because -TargetRoot only
            # redirects where the bytes land and that directory may not be populated yet. The
            # locale directory itself is the one level this script is expected to create, so
            # its parent - <toolchain>\bin\<host>\<arch> - is what must already exist.
            $toolchainRoot = [IO.Path]::GetDirectoryName(
                [IO.Path]::GetDirectoryName((Join-Path $resolvedInstall $relative)))
            if (-not (Test-Path -LiteralPath $toolchainRoot -PathType Container)) {
                throw ("Payload '$([IO.Path]::GetFileName($archive))' targets a toolset that does " +
                    "not exist: '$toolchainRoot'. The resource package belongs to a different " +
                    'MSVC version than the one installed; nothing was written. Install the ' +
                    'English language through the Visual Studio Installer UI instead.')
            }
            $targetDirectory = [IO.Path]::GetDirectoryName($target)
            if (-not (Test-Path -LiteralPath $targetDirectory -PathType Container)) {
                New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
            }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
            $installed++
        }
    } finally {
        $zip.Dispose()
    }
}

if ($installed -eq 0) {
    throw 'No payload entries matched the expected Contents/ layout; nothing was installed.'
}
Write-Host "Extracted $installed file(s) into the toolset."

if (-not $KeepDownload) {
    Remove-Item -LiteralPath $CacheDirectory -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- verifying

Write-Host ''
Write-Host '=== After ==='
$after = Get-ToolsetLayout $writeRoot
$after | Format-Table Toolchain, @{ n = 'Locales'; e = { $_.Locales -join ',' } }, HasEnglish -AutoSize

$stillMissing = @($after | Where-Object { -not $_.HasEnglish })
if ($stillMissing.Count -gt 0) {
    Write-Host ''
    Write-Warning ('English resources are still missing for: ' +
        (($stillMissing | ForEach-Object { "$($_.Toolchain)/$($_.Bin)" }) -join '; '))
    exit 1
}

# The decisive check: cl.exe must now report an ASCII /showIncludes prefix.
$cl = Join-Path ((Get-ToolsetLayout $writeRoot | Where-Object { $_.Bin -match 'Hostx64\\x64$' } |
        Select-Object -First 1).Bin) 'cl.exe'
if (Test-Path -LiteralPath $cl -PathType Leaf) {
    $temporaryDirectory = Join-Path $env:TEMP ("dvs-prefix-check-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryDirectory -Force | Out-Null
    try {
        Set-Content -LiteralPath (Join-Path $temporaryDirectory 'probe.cpp') `
            -Value "#include <cstdio>`nint main() { return 0; }`n" -Encoding utf8
        $psi = [Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $cl
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        foreach ($argument in @('/nologo', '/showIncludes', '/c',
                (Join-Path $temporaryDirectory 'probe.cpp'),
                "/Fo$(Join-Path $temporaryDirectory 'probe.obj')")) {
            $psi.ArgumentList.Add($argument)
        }
        $env:VSLANG = '1033'
        $process = [Diagnostics.Process]::Start($psi)
        $capture = [IO.MemoryStream]::new()
        $outputTask = $process.StandardOutput.BaseStream.CopyToAsync($capture)
        $errorTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $outputTask.GetAwaiter().GetResult() | Out-Null
        $errorTask.GetAwaiter().GetResult() | Out-Null
        $process.Dispose()
        $text = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage).
            GetString($capture.ToArray())
        # The first stdout line is the source file name; the include entries follow it. An
        # empty selection means the probe produced no include entry at all, which must fail
        # rather than slip through as "no non-ASCII characters found".
        $lines = @($text -split "`r?`n")
        $prefixLine = $null
        for ($index = 1; $index -lt $lines.Length; $index++) {
            if ($lines[$index] -match '^(.*?)\s+(?:[A-Za-z]:[\\/]|/|\\\\)\S') {
                $prefixLine = $Matches[1]
                break
            }
        }
        Write-Host ''
        if (-not $prefixLine) {
            Write-Warning ("cl.exe produced no /showIncludes entry to inspect. Files were " +
                "installed, but the prefix could not be confirmed; re-run with -VerifyOnly " +
                'and check that cl.exe runs at all in a developer prompt.')
            exit 1
        }
        Write-Host "cl.exe /showIncludes prefix is now: '$prefixLine'"
        if ($prefixLine -notmatch '^[\x20-\x7E]+$') {
            Write-Host ''
            Write-Warning ('cl.exe still emits a non-ASCII prefix, so header dependency tracking ' +
                'stays broken. The 1033 files are present but cl.exe is not selecting them; ' +
                'check that VSLANG=1033 reaches the compiler process.')
            exit 1
        }
    } finally {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host 'English diagnostic resources are installed and cl.exe now emits an ASCII prefix.'
Write-Host 'Rebuild completely so every object records its header dependencies again:'
Write-Host '  pwsh tools/build/build.ps1 -Preset dev -Fresh -UseInstalledDependencies -Test'
exit 0
