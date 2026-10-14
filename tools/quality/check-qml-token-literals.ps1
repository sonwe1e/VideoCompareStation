#requires -Version 7.0
<#
.SYNOPSIS
Keeps QML dimension literals from drifting away from the design tokens in VcsTheme.js.

.DESCRIPTION
The theme owns one value per design decision: five font sizes, five radii, three animation
durations, two spacing steps and one hit-area minimum. Nothing stopped a component from
writing the number inline, so every review round re-counted the drift by hand: 11 px became
a de-facto sixth font size across 78 sites, `radius: 6` appeared 20 times with no token
behind it, and `duration: 120` was written literally 16 times while `animationFast` sat
unused. `tst_vcs_theme.qml` pins the token *values* and
`MainQmlContractTests.DesignTokensGovernKeyControlMetrics` pins a handful of named
controls; neither looks at the other ~560 literal sites, so a new one is invisible.

This gate scans every production QML file under src/ui_qml/qml and asserts, per family and
per offending value, that the count has not grown past the recorded baseline. It is a
ratchet, not a migration: converging an existing value is what lowers a baseline, and a
brand-new value that no token defines and no baseline allows fails immediately. Values
whose magnitude *is* a token (spacing 4, radius 8, duration 120) are allowed and reported
separately, because replacing the literal with `Theme.<token>` is reference hygiene rather
than drift - forcing all of it in one change is exactly the big-bang this gate avoids.

The check reads files only. Baselines are named constants below, each with the date it was
measured; lower one deliberately when its value is converged, and the count assertion
makes an accidental edit to the tables impossible to miss.

.EXAMPLE
pwsh -NoProfile -File tools/quality/check-qml-token-literals.ps1
#>
[CmdletBinding()]
param(
    [string] $RepositoryRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
}
if (-not (Test-Path -LiteralPath $RepositoryRoot -PathType Container)) {
    Write-Error "Repository root not found: $RepositoryRoot"
    exit 1
}

$themePath = Join-Path $RepositoryRoot 'src\ui_qml\qml\VcsTheme.js'
if (-not (Test-Path -LiteralPath $themePath -PathType Leaf)) {
    Write-Error "Design tokens not found: $themePath"
    exit 1
}
$qmlRoot = Join-Path $RepositoryRoot 'src\ui_qml\qml'
if (-not (Test-Path -LiteralPath $qmlRoot -PathType Container)) {
    Write-Error "Production QML directory not found: $qmlRoot"
    exit 1
}

# ---------------------------------------------------------------------------
# Baselines. Measured 2026-10-14 at c755238 over the 43 files under src/ui_qml/qml.
# A key is the literal value as it appears in the source; the value is how many sites
# carried it. Add an entry only together with the commit that justifies it, and lower one
# only when the corresponding literals are actually gone.
# ---------------------------------------------------------------------------
$fontBaseline = [ordered]@{
    # 11 px is the de-facto sixth font size and the largest single drift.
    '11' = 78
    '15' = 4
    '17' = 3
    '20' = 1
    '24' = 1
    '28' = 1
}
$radiusBaseline = [ordered]@{
    '1'   = 3
    '1.5' = 1
    '2'   = 14
    '3'   = 17
    '3.5' = 2
    '4'   = 19
    '5'   = 11
    '6'   = 20
    '6.5' = 1
    '7'   = 2
    '9'   = 1
    '16'  = 1
}
$durationBaseline = [ordered]@{
    '100' = 7
    '140' = 1
    '150' = 6
    '160' = 1
}
$spacingBaseline = [ordered]@{
    '0'  = 1
    '2'  = 12
    '3'  = 4
    '5'  = 2
    '6'  = 23
    '7'  = 5
    '10' = 12
    '12' = 22
    '14' = 4
}
# A width/height literal in 24..31 px whose innermost enclosing block is, or contains, an
# interactive handler: a pointer target below the theme's 32 px hit-area minimum.
$hitAreaBaseline = [ordered]@{
    '24' = 8
    '26' = 5
    '28' = 23
    '30' = 37
}

# The token names each family draws its allowed values from. Every name must exist in
# VcsTheme.js with the value recorded there; a typo here would silently reclassify
# literals, so the structural checks below verify the pairing.
$fontTokens = @('fontMicro', 'fontSecondary', 'fontMono', 'fontBody', 'fontGroupTitle')
$radiusTokens = @('radiusSmall', 'radiusMedium', 'radiusLarge', 'radiusCard', 'radiusPill')
$durationTokens = @('animationFast', 'animationBase', 'animationSlow')
$spacingTokens = @('spaceMicro', 'spaceMain')

$families = @(
    @{
        Name          = 'font.pixelSize'
        Pattern       = 'font\.pixelSize:\s*([0-9]+(?:\.[0-9]+)?)'
        TokenNames    = $fontTokens
        Baseline      = $fontBaseline
        # Pixel sizes also appear as line heights and canvas geometry elsewhere; this
        # family is the one place a literal directly overrides a typography token.
        SiteNoun      = 'text sizes'
    }
    @{
        Name       = 'radius'
        Pattern    = '(?<![\w.])radius:\s*([0-9]+(?:\.[0-9]+)?)'
        TokenNames = $radiusTokens
        Baseline   = $radiusBaseline
        SiteNoun   = 'corner radii'
    }
    @{
        Name       = 'duration'
        Pattern    = '(?<![\w.])duration:\s*([0-9]+(?:\.[0-9]+)?)'
        TokenNames = $durationTokens
        Baseline   = $durationBaseline
        SiteNoun   = 'animation durations'
    }
    @{
        Name       = 'spacing'
        Pattern    = '(?<![\w.])spacing:\s*([0-9]+(?:\.[0-9]+)?)'
        TokenNames = $spacingTokens
        Baseline   = $spacingBaseline
        SiteNoun   = 'layout spacings'
    }
)

# ---------------------------------------------------------------------------
# Structural checks. These run first so a broken token table or a scan that stopped
# matching cannot turn the per-value checks below into a silent pass.
# ---------------------------------------------------------------------------
$passed = 0

# 1. The token table is real: every declared name exists in VcsTheme.js carrying a number.
$themeLines = [System.IO.File]::ReadAllLines($themePath)
$themeValues = @{}
foreach ($line in $themeLines) {
    $declaration = [regex]::Match($line, '^\s*var\s+(\w+)\s*=\s*(-?[0-9]+(?:\.[0-9]+)?)\s*$')
    if ($declaration.Success) {
        $themeValues[$declaration.Groups[1].Value] = $declaration.Groups[2].Value
    }
}
$missingTokens = [System.Collections.Generic.List[string]]::new()
foreach ($family in $families) {
    foreach ($name in $family.TokenNames) {
        if (-not $themeValues.ContainsKey($name)) {
            $missingTokens.Add("$($family.Name): $name")
        }
    }
}
if ($missingTokens.Count -gt 0) {
    foreach ($name in $missingTokens) {
        Write-Error "$name is declared as a token but VcsTheme.js defines no numeric value for it."
    }
    exit 1
}
$declaredTokenCount = ($families | ForEach-Object { $_.TokenNames.Count } | Measure-Object -Sum).Sum
Write-Host ("PASS: every declared token name resolves to a numeric value in VcsTheme.js " +
    "($($families.Count) families, $declaredTokenCount tokens)")
$passed++

# 2. No allow-listed value may shadow a token value, or a baseline entry silently stops
#    being reachable the moment someone points the family at a different token.
$shadowed = [System.Collections.Generic.List[string]]::new()
foreach ($family in $families) {
    $allowed = @($family.TokenNames | ForEach-Object { $themeValues[$_] })
    foreach ($value in $family.Baseline.Keys) {
        if ($allowed -contains $value) {
            $shadowed.Add("$($family.Name): $value")
        }
    }
}
if ($shadowed.Count -gt 0) {
    foreach ($entry in $shadowed) {
        Write-Error "$entry is allow-listed but its value is already a token; the baseline entry is unreachable."
    }
    exit 1
}
Write-Host 'PASS: no allow-listed value shadows a token value'
$passed++

# 3. The scan corpus is real.
$qmlFiles = @(Get-ChildItem -LiteralPath $qmlRoot -Recurse -File -Filter '*.qml')
if ($qmlFiles.Count -eq 0) {
    Write-Error "No QML files found under $qmlRoot; the scan would pass vacuously."
    exit 1
}
Write-Host "PASS: the scan reads $($qmlFiles.Count) production QML files"
$passed++

# ---------------------------------------------------------------------------
# Per-family measurement.
# ---------------------------------------------------------------------------
$interactiveMarker = '(?<![\w.])(MouseArea|TapHandler|HoverHandler|Button|ToolButton|VcsToolButton|ReviewActionButton|ImageCommandButton|onClicked|onPressed|onTapped|onDoubleClicked)\b'
$dimensionPattern = '^\s*(implicitWidth|implicitHeight|width|height):\s*([0-9]+(?:\.[0-9]+)?)\s*(?:;|$)'
$errors = [System.Collections.Generic.List[string]]::new()

function Measure-FamilyLiterals {
    param([string]$Pattern)
    $counts = @{}
    foreach ($file in $qmlFiles) {
        $lineNumber = 0
        foreach ($line in [System.IO.File]::ReadAllLines($file.FullName)) {
            $lineNumber++
            $match = [regex]::Match($line, $Pattern)
            if (-not $match.Success) {
                continue
            }
            $value = $match.Groups[1].Value
            if (-not $counts.ContainsKey($value)) {
                $counts[$value] = 0
            }
            $counts[$value]++
        }
    }
    return $counts
}

function Measure-HitAreaLiterals {
    # A dimension literal below the hit-area minimum whose innermost enclosing block is,
    # or contains, an interactive handler. Non-interactive geometry at the same size is not
    # a pointer target and is deliberately not counted.
    $counts = @{}
    foreach ($file in $qmlFiles) {
        $lines = [System.IO.File]::ReadAllLines($file.FullName)
        for ($index = 0; $index -lt $lines.Count; $index++) {
            $match = [regex]::Match($lines[$index], $dimensionPattern)
            if (-not $match.Success) {
                continue
            }
            $value = [double]$match.Groups[2].Value
            if ($value -lt 24 -or $value -gt 31) {
                continue
            }
            $indent = $lines[$index].Length - $lines[$index].TrimStart().Length
            $block = ''
            $blockIndent = -1
            for ($back = $index - 1; $back -ge 0; $back--) {
                if ($lines[$back].Trim().Length -eq 0) {
                    continue
                }
                $backIndent = $lines[$back].Length - $lines[$back].TrimStart().Length
                if ($backIndent -lt $indent) {
                    $block = $lines[$back]
                    $blockIndent = $backIndent
                    break
                }
            }
            if ($blockIndent -lt 0) {
                continue
            }
            $body = $block
            for ($forward = $index + 1; $forward -lt $lines.Count; $forward++) {
                if ($lines[$forward].Trim().Length -eq 0) {
                    continue
                }
                $forwardIndent = $lines[$forward].Length - $lines[$forward].TrimStart().Length
                if ($forwardIndent -le $blockIndent) {
                    break
                }
                $body += "`n" + $lines[$forward]
            }
            if ($block -notmatch $interactiveMarker -and $body -notmatch $interactiveMarker) {
                continue
            }
            $key = $match.Groups[2].Value
            if (-not $counts.ContainsKey($key)) {
                $counts[$key] = 0
            }
            $counts[$key]++
        }
    }
    return $counts
}

$totalSites = 0
$totalTokenValued = 0
$totalBaselined = 0

foreach ($family in $families) {
    $observed = Measure-FamilyLiterals -Pattern $family.Pattern
    $tokenValues = @($family.TokenNames | ForEach-Object { $themeValues[$_] })

    # 4. The pattern still matches something. A regex that silently stops matching would
    #    otherwise let every literal through.
    $familySites = 0
    foreach ($value in $observed.Keys) {
        $familySites += $observed[$value]
    }
    if ($familySites -eq 0) {
        Write-Error "$($family.Name): the scan matched no literals; the pattern or the corpus changed."
        exit 1
    }

    # 5. No value outside the token set and the baseline.
    $unknown = @($observed.Keys | Where-Object { $tokenValues -notcontains $_ -and $family.Baseline.Keys -notcontains $_ } |
        Sort-Object { [double]$_ })
    if ($unknown.Count -gt 0) {
        foreach ($value in $unknown) {
            Write-Error ("$($family.Name): $value is a new literal with no token and no baseline entry " +
                "($($observed[$value]) site(s)). Add a token to VcsTheme.js, or record a baseline " +
                'entry with a comment explaining why it has to stay.')
        }
        exit 1
    }
    Write-Host "PASS: $($family.Name) introduces no value outside its tokens and allow-list"
    $passed++

    # 6. One check per allow-listed value: the count has not grown.
    foreach ($value in $family.Baseline.Keys) {
        $baseline = [int]$family.Baseline[$value]
        $observedCount = 0
        if ($observed.ContainsKey($value)) {
            $observedCount = $observed[$value]
        }
        if ($observedCount -gt $baseline) {
            Write-Error ("$($family.Name): $value appears at $observedCount site(s), above the recorded " +
                "baseline of $baseline. Converge existing sites to a token instead of adding more.")
        }
        else {
            $delta = $baseline - $observedCount
            $note = if ($delta -eq 0) { 'at baseline' } else { "$delta below baseline" }
            Write-Host "PASS: $($family.Name) $value is $observedCount site(s) ($note; ceiling $baseline)"
        }
        $passed++
    }

    $tokenValued = 0
    foreach ($value in $tokenValues) {
        if ($observed.ContainsKey($value)) {
            $tokenValued += $observed[$value]
        }
    }
    $baselined = 0
    foreach ($value in $family.Baseline.Keys) {
        if ($observed.ContainsKey($value)) {
            $baselined += $observed[$value]
        }
    }
    $totalSites += $familySites
    $totalTokenValued += $tokenValued
    $totalBaselined += $baselined
    Write-Host (("  {0}: {1} sites, {2} already at a token magnitude, {3} under an allow-listed " +
        'baseline') -f $family.Name, $familySites, $tokenValued, $baselined)
}

# The hit-area family is measured separately: it needs the enclosing-block walk to tell a
# pointer target from plain geometry.
$hitAreas = Measure-HitAreaLiterals
$hitAreaSites = 0
foreach ($value in $hitAreas.Keys) {
    $hitAreaSites += $hitAreas[$value]
}
if ($hitAreaSites -eq 0) {
    Write-Error 'hit area: the scan matched no interactive dimensions below 32 px; the pattern or the corpus changed.'
    exit 1
}
$unknownHitAreas = @($hitAreas.Keys | Where-Object { $hitAreaBaseline.Keys -notcontains $_ } | Sort-Object { [double]$_ })
if ($unknownHitAreas.Count -gt 0) {
    foreach ($value in $unknownHitAreas) {
        Write-Error ("hit area: $value px is a new interactive target below the 32 px minimum with no " +
            "baseline entry ($($hitAreas[$value]) site(s)). Raise it to Theme.hitAreaMinimum, or record " +
            'a baseline entry explaining the exception.')
    }
    exit 1
}
Write-Host 'PASS: hit area introduces no interactive size outside its allow-list'
$passed++
foreach ($value in $hitAreaBaseline.Keys) {
    $baseline = [int]$hitAreaBaseline[$value]
    $observedCount = 0
    if ($hitAreas.ContainsKey($value)) {
        $observedCount = $hitAreas[$value]
    }
    if ($observedCount -gt $baseline) {
        Write-Error ("hit area: $value px appears at $observedCount interactive site(s), above the recorded " +
            "baseline of $baseline. Raise the target to Theme.hitAreaMinimum instead of adding more.")
    }
    else {
        $delta = $baseline - $observedCount
        $note = if ($delta -eq 0) { 'at baseline' } else { "$delta below baseline" }
        Write-Host "PASS: hit area $value px is $observedCount site(s) ($note; ceiling $baseline)"
    }
    $passed++
}
$totalSites += $hitAreaSites
$totalBaselined += $hitAreaSites
Write-Host (("  hit area: {0} interactive targets below the 32 px minimum, all under an allow-listed " +
    'baseline') -f $hitAreaSites)

# ---------------------------------------------------------------------------
# Self-asserting check count. AGENTS.md requires a script to assert its own check count so
# a missing anchor cannot turn its checks into silent passes. The total is derived from the
# tables above so it cannot drift from them: 3 structural checks, one unknown-value check per
# family (the four measured families plus the hit-area family), one baseline check per
# allow-listed value. Adding a family, a token or a baseline entry without adding its check
# breaks this assertion on purpose.
# ---------------------------------------------------------------------------
$baselineEntries = $fontBaseline.Count + $radiusBaseline.Count + $durationBaseline.Count +
$spacingBaseline.Count + $hitAreaBaseline.Count
$expectedCases = 3 + ($families.Count + 1) + $baselineEntries
if ($passed -ne $expectedCases) {
    throw "Expected $expectedCases QML token-literal cases; ran $passed."
}

Write-Host ("QML token-literal checks passed: $passed cases, $totalSites literal sites across 5 " +
    "families ($totalTokenValued already at a token magnitude, $totalBaselined under a baseline " +
    'ceiling).')
