#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $BuildDir,

    [ValidateSet("All", "domain", "application")]
    [string[]] $Module = @("domain", "application"),

    [ValidateRange(0.0, 100.0)]
    [double] $Minimum = 80.0,

    [string[]] $ReportPath,

    # The tree whose sources the report has to cover, and the base that relative report file names
    # resolve against. It defaults to the build's own sources. The fixture run points it at a
    # fixture tree so that exercising this gate does not depend on the live source list, where every
    # newly added file turned the workflow red until the fixture was updated.
    [string] $SourceRoot = "src"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$resolvedBuildDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    [System.IO.Path]::GetFullPath($BuildDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $BuildDir))
}

if (-not (Test-Path -LiteralPath $resolvedBuildDir -PathType Container)) {
    throw "Coverage build directory does not exist: $resolvedBuildDir"
}

$resolvedSourceRoot = if ([System.IO.Path]::IsPathRooted($SourceRoot)) {
    [System.IO.Path]::GetFullPath($SourceRoot)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $SourceRoot))
}
if (-not (Test-Path -LiteralPath $resolvedSourceRoot -PathType Container)) {
    throw "Coverage source root does not exist: $resolvedSourceRoot"
}

# Report file names are relative to the tree that contains the source root: the repository root for
# a real build (the default 'src' makes that identical to the previous behaviour), or the fixture
# root when the fixture tree is measured.
$sourceParent = Split-Path -Parent $resolvedSourceRoot
$sourceParentPrefix = $sourceParent.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar
) + [System.IO.Path]::DirectorySeparatorChar

if ($Module -contains "All" -and $Module.Count -ne 1) {
    throw "Select 'All' alone, or select domain/application explicitly."
}
$selectedModules = if ($Module -contains "All") {
    @("domain", "application")
} else {
    @($Module | ForEach-Object { $_.ToLowerInvariant() } | Select-Object -Unique)
}

function Resolve-ReportPath {
    param([Parameter(Mandatory)][string] $Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Coverage report paths must not be empty."
    }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $Path))
}

function Get-SourceRelativePath {
    param([Parameter(Mandatory)][string] $Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith(
        $sourceParentPrefix,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        return $null
    }
    return $fullPath.Substring($sourceParentPrefix.Length)
}

[string[]] $reports = if ($null -ne $ReportPath -and $ReportPath.Length -gt 0) {
    @($ReportPath | ForEach-Object { Resolve-ReportPath -Path $_ })
} else {
    @(Join-Path $resolvedBuildDir "coverage\coverage.cobertura.xml")
}

$uniqueReports = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)
foreach ($report in $reports) {
    if (-not $uniqueReports.Add($report)) {
        throw "Coverage report was supplied more than once: $report"
    }
    if (-not (Test-Path -LiteralPath $report -PathType Leaf)) {
        throw (
            "Coverage report does not exist: $report. Run the pinned collector/converter " +
            "first or pass explicit -ReportPath values."
        )
    }
}

function Resolve-CoveredSource {
    param(
        [Parameter(Mandatory)][string] $FileName,
        [Parameter(Mandatory)][System.Xml.XmlDocument] $Document
    )

    $slashName = $FileName.Replace("\", "/")
    $lowerName = $slashName.ToLowerInvariant()
    if ($lowerName -match "(^|/)(tests|generated|vcpkg_installed|_deps|out)(/|$)") {
        return $null
    }

    $candidates = [System.Collections.Generic.List[string]]::new()
    if ([System.IO.Path]::IsPathRooted($FileName)) {
        $candidates.Add($FileName)
    } else {
        foreach ($sourceNode in @($Document.SelectNodes("/coverage/sources/source"))) {
            $sourceRoot = [string] $sourceNode.InnerText
            if ([string]::IsNullOrWhiteSpace($sourceRoot)) {
                continue
            }
            $resolvedRoot = if ([System.IO.Path]::IsPathRooted($sourceRoot)) {
                $sourceRoot
            } else {
                Join-Path $sourceParent $sourceRoot
            }
            $candidates.Add((Join-Path $resolvedRoot $FileName))
        }
        $candidates.Add((Join-Path $sourceParent $FileName))
    }

    foreach ($candidate in $candidates) {
        $absolutePath = [System.IO.Path]::GetFullPath($candidate)
        if (-not (Test-Path -LiteralPath $absolutePath -PathType Leaf)) {
            continue
        }
        $relativePath = Get-SourceRelativePath -Path $absolutePath
        if ($null -eq $relativePath) {
            continue
        }

        $normalizedPath = $relativePath.Replace("\", "/").ToLowerInvariant()
        $sourceModule = if ($normalizedPath.StartsWith("src/domain/")) {
            "domain"
        } elseif ($normalizedPath.StartsWith("src/application/")) {
            "application"
        } else {
            return $null
        }
        return [pscustomobject]@{
            Module = $sourceModule
            Path = $normalizedPath
        }
    }

    if ($lowerName -match "(^|/)src/(domain|application)/") {
        throw "Coverage source '$FileName' does not map to an existing file under $sourceParent."
    }
    return $null
}

$lineHits = @{}
$reportedSources = @{}
foreach ($report in $reports) {
    $settings = [System.Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [System.Xml.XmlReader]::Create($report, $settings)
    try {
        $document = [System.Xml.XmlDocument]::new()
        $document.XmlResolver = $null
        $document.Load($reader)
    } finally {
        $reader.Dispose()
    }

    foreach ($line in @($document.SelectNodes("//class[@filename]/lines/line[@number][@hits]"))) {
        $class = $line.ParentNode.ParentNode
        $source = Resolve-CoveredSource -FileName $class.GetAttribute("filename") `
            -Document $document
        if ($null -eq $source -or $source.Module -notin $selectedModules) {
            continue
        }

        $lineNumber = 0
        $hits = 0L
        if (-not [int]::TryParse($line.GetAttribute("number"), [ref] $lineNumber) -or
            -not [long]::TryParse($line.GetAttribute("hits"), [ref] $hits) -or
            $lineNumber -le 0 -or $hits -lt 0) {
            throw "Invalid line number or hit count in coverage report: $report"
        }

        $sourceKey = "$($source.Module)|$($source.Path)"
        $reportedSources[$sourceKey] = $true
        $lineKey = "$sourceKey|$lineNumber"
        if (-not $lineHits.ContainsKey($lineKey) -or $hits -gt $lineHits[$lineKey]) {
            $lineHits[$lineKey] = $hits
        }
    }
}

foreach ($selectedModule in $selectedModules) {
    $moduleSourceRoot = Join-Path $resolvedSourceRoot $selectedModule
    $expectedSources = @(Get-ChildItem -LiteralPath $moduleSourceRoot -Recurse -File -Filter "*.cpp" -ErrorAction SilentlyContinue)
    if ($expectedSources.Count -eq 0) {
        throw "Module '$selectedModule' has no C++ source files to measure under $moduleSourceRoot."
    }
    foreach ($expectedSource in $expectedSources) {
        $relativePath = (Get-SourceRelativePath -Path $expectedSource.FullName).
            Replace("\", "/").ToLowerInvariant()
        if (-not $reportedSources.ContainsKey("$selectedModule|$relativePath")) {
            throw "Coverage report omitted compiled source: $relativePath"
        }
    }
}

$failed = $false
foreach ($selectedModule in $selectedModules) {
    $prefix = "$selectedModule|"
    $moduleLines = @($lineHits.GetEnumerator() | Where-Object {
        $_.Key.StartsWith($prefix, [System.StringComparison]::Ordinal)
    })
    if ($moduleLines.Count -eq 0) {
        throw "Coverage reports contain no source lines for module '$selectedModule'."
    }

    $covered = @($moduleLines | Where-Object { $_.Value -gt 0 }).Count
    $percentage = 100.0 * $covered / $moduleLines.Count
    $display = $percentage.ToString("F2", [System.Globalization.CultureInfo]::InvariantCulture)
    Write-Host "$selectedModule line coverage: $display% ($covered/$($moduleLines.Count))"
    if ($percentage -lt $Minimum) {
        $failed = $true
    }
}

if ($failed) {
    $minimumDisplay = $Minimum.ToString(
        "F2",
        [System.Globalization.CultureInfo]::InvariantCulture
    )
    Write-Error "Coverage is below the required $minimumDisplay%."
    exit 1
}
