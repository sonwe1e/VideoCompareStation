[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+$')]
    [string] $Tag,

    [string] $Executable,

    [string] $ZipPath,

    [string] $MsiPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$cmakeLists = Get-Content -LiteralPath (Join-Path $repositoryRoot 'CMakeLists.txt') -Raw
$versionMatch = [regex]::Match(
    $cmakeLists,
    'project\s*\(\s*VCStation\s+VERSION\s+(?<version>\d+\.\d+\.\d+)\s+LANGUAGES\s+CXX\s*\)',
    [Text.RegularExpressions.RegexOptions]::IgnoreCase
)
if (-not $versionMatch.Success) {
    throw 'Could not resolve the VCStation project version from CMakeLists.txt.'
}

$version = $versionMatch.Groups['version'].Value
$expectedTag = "v$version"
if ($Tag -cne $expectedTag) {
    throw "Release tag '$Tag' does not match project version '$version' (expected '$expectedTag')."
}

$versionParts = $version.Split('.') | ForEach-Object { [int] $_ }
if ($Executable) {
    $resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
    $versionInfo = (Get-Item -LiteralPath $resolvedExecutable).VersionInfo
    $fixedFileVersion = @(
        $versionInfo.FileMajorPart,
        $versionInfo.FileMinorPart,
        $versionInfo.FileBuildPart,
        $versionInfo.FilePrivatePart
    )
    $fixedProductVersion = @(
        $versionInfo.ProductMajorPart,
        $versionInfo.ProductMinorPart,
        $versionInfo.ProductBuildPart,
        $versionInfo.ProductPrivatePart
    )
    $expectedFixedVersion = @($versionParts) + 0
    foreach ($fixedVersion in @($fixedFileVersion, $fixedProductVersion)) {
        if (-not (Compare-Object -ReferenceObject $expectedFixedVersion -DifferenceObject $fixedVersion)) {
            continue
        }
        throw (
            "Executable fixed file version '$($fixedVersion -join '.')' does not match " +
            "project version '$version'."
        )
    }
    foreach ($property in @('FileVersion', 'ProductVersion')) {
        $value = $versionInfo.$property
        if ($value -notmatch "^$([regex]::Escape($version))(?:\.0)?$") {
            throw "Executable $property '$value' does not match project version '$version'."
        }
    }
}

$expectedBaseName = "VCStation-$version-windows-x64"
foreach ($artifact in @(
        @{ Path = $ZipPath; Extension = '.zip' },
        @{ Path = $MsiPath; Extension = '.msi' }
    )) {
    if (-not $artifact.Path) {
        continue
    }
    $resolvedArtifact = (Resolve-Path -LiteralPath $artifact.Path).Path
    $expectedName = "$expectedBaseName$($artifact.Extension)"
    if ([IO.Path]::GetFileName($resolvedArtifact) -cne $expectedName) {
        throw "Release artifact must be named '$expectedName': $resolvedArtifact"
    }
}

if ($MsiPath) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.GetType().InvokeMember(
        'OpenDatabase',
        'InvokeMethod',
        $null,
        $installer,
        @((Resolve-Path -LiteralPath $MsiPath).Path, 0)
    )
    foreach ($property in @(
            @{ Name = 'ProductName'; Expected = 'VCStation' },
            @{ Name = 'ProductVersion'; Expected = $version }
        )) {
        $query =
            "SELECT ``Value`` FROM ``Property`` WHERE ``Property``='$($property.Name)'"
        $view = $database.GetType().InvokeMember(
            'OpenView',
            'InvokeMethod',
            $null,
            $database,
            @($query)
        )
        $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
        $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
        if (-not $record) {
            throw "MSI property '$($property.Name)' is missing."
        }
        $value = $record.GetType().InvokeMember(
            'StringData',
            'GetProperty',
            $null,
            $record,
            1
        )
        if ($value -cne $property.Expected) {
            throw (
                "MSI property '$($property.Name)' is '$value', expected " +
                "'$($property.Expected)'."
            )
        }
    }
}

Write-Output "DVS_RELEASE_VERSION_VERIFIED version=$version tag=$Tag"
