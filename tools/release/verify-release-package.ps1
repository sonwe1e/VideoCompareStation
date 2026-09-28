#requires -Version 7.0
<#
.SYNOPSIS
Verifies that a built release ZIP can register the Explorer command on its own.

.DESCRIPTION
The published package is the only thing users receive, and the failure this guards against is
invisible from the source tree: a ZIP that is missing the version-derived shell library, or built
before the startup registration existed, looks perfectly fine until the first right-click shows no
menu entry. So the check runs on the unpacked package, not on the build tree:

  - CompareStation.exe is present and reports the product version;
  - CompareStationShell-<major>.<minor>.dll for exactly that version sits next to it, which is the
    name the startup path derives, and it is a real non-empty file;
  - the executable carries the registration code and the menu text it registers - narrow and wide
    literals live in different encodings, so both are searched for;
  - the packaged executable is byte-identical to the one this build produced;
  - the double-click entry point, its helper and RegisterExplorerCommand.ps1 ship with it, and the
    .cmd keeps the ASCII + CRLF shape cmd.exe needs;
  - INSTALL.txt documents the self-registration, the uninstall line and the escape hatch.

Nothing here starts the application or writes the registry.

.EXAMPLE
pwsh -NoProfile -File tools/release/verify-release-package.ps1
pwsh -NoProfile -File tools/release/verify-release-package.ps1 -ZipPath out\package\zip\CompareStation-2.0.1-windows-x64.zip
#>
[CmdletBinding()]
param(
    [string] $ZipPath,
    [string] $BuiltExecutable,
    [string] $WorkRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

if ([string]::IsNullOrWhiteSpace($ZipPath)) {
    $packageDirectory = Join-Path $repositoryRoot 'out\package\zip'
    $newest = Get-ChildItem -LiteralPath $packageDirectory -Filter 'CompareStation-*.zip' -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $newest) {
        throw "no package under $packageDirectory; run 'cpack --preset release-zip' first"
    }
    $ZipPath = $newest.FullName
}
$ZipPath = (Resolve-Path -LiteralPath $ZipPath).Path
if ([string]::IsNullOrWhiteSpace($BuiltExecutable)) {
    $BuiltExecutable = Join-Path $repositoryRoot 'out\build\release\bin\CompareStation.exe'
}
if ([string]::IsNullOrWhiteSpace($WorkRoot)) {
    $WorkRoot = Join-Path $repositoryRoot ('out\package\verify-' + $PID)
}

$checkCount = 0
$failureCount = 0
# The total is asserted at the end so that a missing anchor cannot turn its assertions into silent
# passes; the one conditional assertion is what makes two totals valid, and the end of the script
# says why.

function Assert-That {
    param([string] $Name, [bool] $Condition, [string] $Detail = '')

    $script:checkCount++
    if ($Condition) {
        Write-Host "PASS  $Name"
        return
    }
    $script:failureCount++
    $suffix = if ([string]::IsNullOrEmpty($Detail)) { '' } else { " - $Detail" }
    Write-Host "FAIL  $Name$suffix" -ForegroundColor Red
}

if (Test-Path -LiteralPath $WorkRoot) {
    Remove-Item -LiteralPath $WorkRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null

try {
    $hash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash
    Write-Host "package   : $ZipPath"
    Write-Host "sha256    : $hash"
    Write-Host "size      : $([math]::Round((Get-Item -LiteralPath $ZipPath).Length / 1MB, 1)) MiB"
    Write-Host ''

    Expand-Archive -LiteralPath $ZipPath -DestinationPath $WorkRoot -Force

    # The binaries sit either at the root of the archive or under a single top-level directory.
    $binRoot = $WorkRoot
    if (-not (Test-Path -LiteralPath (Join-Path $binRoot 'CompareStation.exe'))) {
        $inner = @(Get-ChildItem -LiteralPath $WorkRoot -Directory)
        if ($inner.Count -eq 1) {
            $binRoot = $inner[0].FullName
        }
    }

    $executable = Join-Path $binRoot 'CompareStation.exe'
    Assert-That 'the package holds CompareStation.exe' (Test-Path -LiteralPath $executable)

    $version = (Get-Item -LiteralPath $executable).VersionInfo.FileVersion
    Assert-That 'the executable reports a product version' `
        (-not [string]::IsNullOrWhiteSpace($version)) "FileVersion '$version'"

    $expectedLibrary = 'CompareStationShell-{0}.dll' -f ($version -replace '^(\d+\.\d+).*$', '$1')
    $shellLibrary = Join-Path $binRoot $expectedLibrary
    Assert-That "the version-derived shell library $expectedLibrary is next to the executable" `
        (Test-Path -LiteralPath $shellLibrary) `
        "found: $(@(Get-ChildItem -LiteralPath $binRoot -Filter 'CompareStationShell-*.dll').Name -join ', ')"
    # A zero-byte file satisfies Test-Path and std::filesystem::exists, so the length is checked too:
    # such a package would register a library Explorer cannot load and then never repair it.
    $shellLibraryLength = if (Test-Path -LiteralPath $shellLibrary) {
        (Get-Item -LiteralPath $shellLibrary).Length
    }
    else {
        0
    }
    Assert-That 'the shell library is a real, non-empty file' ($shellLibraryLength -gt 0) `
        "length $shellLibraryLength"

    # The registration code has to be inside the shipped executable. The narrow error code and the
    # wide environment-variable name live in different encodings, so both are searched for.
    $executableBytes = [System.IO.File]::ReadAllBytes($executable)
    $executableAscii = [System.Text.Encoding]::ASCII.GetString($executableBytes)
    $executableWide = [System.Text.Encoding]::Unicode.GetString($executableBytes)
    Assert-That 'the executable carries the startup registration code' `
        ($executableAscii.Contains('DVS_SHELL_REGISTRATION_FAILED') -and
         $executableWide.Contains('DVS_DISABLE_SHELL_REGISTRATION'))
    Assert-That 'the executable carries the menu text it registers' `
        ($executableWide.Contains('Compare with CompareStation'))

    if (Test-Path -LiteralPath $BuiltExecutable) {
        # Comparing bytes is only conclusive when the package was written after the last link. A
        # rebuilt release preset relinks CompareStation.exe and produces a different binary (embedded
        # timestamps and debug identifiers), which says nothing about the package being stale, so a
        # build that is newer than the package is reported instead of failed. That is why the check
        # count below accepts both totals rather than exactly one.
        $builtTime = (Get-Item -LiteralPath $BuiltExecutable).LastWriteTimeUtc
        $packageTime = (Get-Item -LiteralPath $ZipPath).LastWriteTimeUtc
        if ($builtTime -le $packageTime) {
            $builtHash = (Get-FileHash -LiteralPath $BuiltExecutable -Algorithm SHA256).Hash
            $packagedHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
            Assert-That 'the packaged executable is the one this build produced' `
                ($builtHash -ceq $packagedHash) `
                "packaged $($packagedHash.Substring(0, 16)) vs built $($builtHash.Substring(0, 16))"
        }
        else {
            Write-Host (
                'NOTE  the build tree was linked after this package was written, so their bytes are ' +
                'not compared. Rebuild the package (cpack --preset release-zip) to compare them.')
        }
    }

    $wrapper = Join-Path $binRoot 'RegisterCompareStationContextMenu.cmd'
    foreach ($path in @(
            $wrapper,
            (Join-Path $binRoot 'RegisterCompareStationContextMenu.ps1'),
            (Join-Path $binRoot 'RegisterExplorerCommand.ps1')
        )) {
        Assert-That "the package holds $(Split-Path -Leaf $path)" (Test-Path -LiteralPath $path)
    }
    if (Test-Path -LiteralPath $wrapper) {
        $wrapperBytes = [System.IO.File]::ReadAllBytes($wrapper)
        Assert-That 'the packaged .cmd holds no non-ASCII bytes' `
            (@($wrapperBytes | Where-Object { $_ -gt 127 }).Count -eq 0)
        $bareLf = 0
        for ($index = 0; $index -lt $wrapperBytes.Length; $index++) {
            if ($wrapperBytes[$index] -eq 10 -and ($index -eq 0 -or $wrapperBytes[$index - 1] -ne 13)) {
                $bareLf++
            }
        }
        Assert-That 'the packaged .cmd uses CRLF line endings' ($bareLf -eq 0) "bare LF count $bareLf"
    }

    $installNotes = Join-Path $binRoot 'INSTALL.txt'
    Assert-That 'the package holds INSTALL.txt' (Test-Path -LiteralPath $installNotes)
    if (Test-Path -LiteralPath $installNotes) {
        $notes = Get-Content -LiteralPath $installNotes -Raw
        Assert-That 'INSTALL.txt documents the self-registration on first start' `
            ($notes -match '启动' -and $notes -match '自动注册')
        Assert-That 'INSTALL.txt documents the uninstall line' `
            ($notes -match [regex]::Escape('.\RegisterCompareStationContextMenu.cmd -Uninstall'))
        Assert-That 'INSTALL.txt documents the escape hatch' `
            ($notes -match [regex]::Escape('DVS_DISABLE_SHELL_REGISTRATION'))
    }

    Write-Host ''
    Write-Host "checks=$checkCount failures=$failureCount"
    # One assertion (the byte comparison against the build tree) only applies when the package is at
    # least as new as that build, so the total is allowed to be either 15 or 16 - and nothing lower,
    # which is what catches an anchor that silently skipped its checks.
    if ($checkCount -lt 15 -or $checkCount -gt 16) {
        throw "expected 15 or 16 checks but ran ${checkCount}: an assertion was skipped."
    }
    if ($failureCount -ne 0) {
        throw "$failureCount of $checkCount checks failed."
    }
    Write-Host 'DVS_RELEASE_PACKAGE_CAN_SELF_REGISTER'
}
finally {
    if (Test-Path -LiteralPath $WorkRoot) {
        Remove-Item -LiteralPath $WorkRoot -Recurse -Force
    }
}
