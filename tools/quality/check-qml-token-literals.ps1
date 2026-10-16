#requires -Version 7.0
<#
.SYNOPSIS
Keeps QML dimension literals from drifting away from the design tokens in VcsTheme.js.

.DESCRIPTION
The theme owns one value per design decision: six font sizes, seven radii, three animation
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
brand-new value that no token defines and no baseline allows fails immediately. A family
whose literal count reaches zero passes as converged, but only after the scan proves itself
still live - the pattern must match a synthetic probe line and the raw property name must
still occur in the corpus (tokenized bindings count) - so full convergence can never read
as a broken scan, and a broken scan can never read as convergence. Values
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
# only when the corresponding literals are actually gone. 2026-10-14, same day: 11 px
# became fontCaption and 4/6 px became radiusMicro/radiusSubtle, so the '11', '4' and '6'
# entries were removed - the shadow check below rejects a baseline entry whose value a
# token already defines - and duration '100' dropped to 6 when VcsMenuItem converged to
# animationFast.
# ---------------------------------------------------------------------------
$fontBaseline = [ordered]@{
    '15' = 4
    '17' = 3
    '20' = 1
    '24' = 1
    '28' = 1
}
$radiusBaseline = [ordered]@{
    '1'   = 3
    '1.5' = 1
    # 14 -> 17 (2026-10-14, flow176 UI round): the image workspace's crosshair pips and
    # crop-handle hairlines added three geometric halves (radius 2 on 4 px dots and 14x4
    # handle pills). These are size-derived geometry like the VcsMenuItem radio dot, not a
    # design-token step; converging them to radiusMicro (4) would visibly square the dots.
    '2'   = 17
    '3'   = 17
    '3.5' = 2
    '5'   = 11
    '6.5' = 1
    '7'   = 2
    '9'   = 1
    '16'  = 1
}
$durationBaseline = [ordered]@{
    '100' = 6
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
$fontTokens = @('fontMicro', 'fontSecondary', 'fontMono', 'fontBody', 'fontGroupTitle', 'fontCaption')
$radiusTokens = @('radiusMicro', 'radiusSubtle', 'radiusSmall', 'radiusMedium', 'radiusLarge', 'radiusCard', 'radiusPill')
$durationTokens = @('animationFast', 'animationBase', 'animationSlow')
$spacingTokens = @('spaceMicro', 'spaceMain')

$families = @(
    @{
        Name          = 'font.pixelSize'
        Pattern       = 'font\.pixelSize:\s*([0-9]+(?:\.[0-9]+)?)'
        # Convergence probes, used when the literal count reaches zero: ProbeLine is a
        # synthetic sample the Pattern must still match and capture (regex liveness);
        # PresencePattern is the raw property name that must still occur in the corpus -
        # tokenized bindings such as `duration: Theme.animationFast` keep it non-zero.
        ProbeLine       = 'font.pixelSize: 999'
        PresencePattern = 'font\.pixelSize:'
        TokenNames    = $fontTokens
        Baseline      = $fontBaseline
        # Pixel sizes also appear as line heights and canvas geometry elsewhere; this
        # family is the one place a literal directly overrides a typography token.
        SiteNoun      = 'text sizes'
    }
    @{
        Name            = 'radius'
        Pattern         = '(?<![\w.])radius:\s*([0-9]+(?:\.[0-9]+)?)'
        ProbeLine       = 'radius: 999'
        PresencePattern = '(?<![\w.])radius:'
        TokenNames      = $radiusTokens
        Baseline        = $radiusBaseline
        SiteNoun        = 'corner radii'
    }
    @{
        Name            = 'duration'
        Pattern         = '(?<![\w.])duration:\s*([0-9]+(?:\.[0-9]+)?)'
        ProbeLine       = 'duration: 999'
        PresencePattern = '(?<![\w.])duration:'
        TokenNames      = $durationTokens
        Baseline        = $durationBaseline
        SiteNoun        = 'animation durations'
    }
    @{
        Name            = 'spacing'
        Pattern         = '(?<![\w.])spacing:\s*([0-9]+(?:\.[0-9]+)?)'
        ProbeLine       = 'spacing: 999'
        PresencePattern = '(?<![\w.])spacing:'
        TokenNames      = $spacingTokens
        Baseline        = $spacingBaseline
        SiteNoun        = 'layout spacings'
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
# A block whose only pointer attachment is a HoverHandler exposes a tooltip or a
# hover-reveal; it is not an activation target, so hover-only blocks do not count
# (the image marquee hint chip). Any real activation surface - a MouseArea/TapHandler,
# a button type, or an onClicked/onPressed/onTapped/onDoubleClicked handler anywhere
# in the block - still counts, so no clickable control is let through by the exclusion.
$interactiveMarker = '(?<![\w.])(MouseArea|TapHandler|Button|ToolButton|VcsToolButton|ReviewActionButton|ImageCommandButton|onClicked|onPressed|onTapped|onDoubleClicked)\b'
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

    # 4. A zero literal count must mean convergence, not a dead scan. A family whose
    #    literals are all converged to tokens (duration reached that state on
    #    2026-10-14) measures zero literal sites, so zero alone cannot fail the gate.
    #    It fails only when the zero is not trustworthy: the Pattern no longer matches
    #    its synthetic ProbeLine (regex drift), or the raw property name no longer
    #    occurs in the corpus (corpus drift). Tokenized bindings such as
    #    `duration: Theme.animationFast` keep the presence probe non-zero, so a live
    #    scan over a corpus that still writes the property reads zero only as real
    #    convergence.
    $familySites = 0
    foreach ($value in $observed.Keys) {
        $familySites += $observed[$value]
    }
    if ($familySites -eq 0) {
        $probeMatch = [regex]::Match($family.ProbeLine, $family.Pattern)
        if (-not $probeMatch.Success -or $probeMatch.Groups[1].Value -ne '999') {
            Write-Error ("$($family.Name): the pattern no longer matches its synthetic probe line " +
                "'$($family.ProbeLine)'; the scan is broken.")
            exit 1
        }
        $presenceCount = 0
        foreach ($file in $qmlFiles) {
            $presenceCount += [regex]::Matches(
                [System.IO.File]::ReadAllText($file.FullName), $family.PresencePattern).Count
        }
        if ($presenceCount -eq 0) {
            Write-Error ("$($family.Name): the raw property no longer occurs in the production " +
                'corpus; the pattern or the corpus changed.')
            exit 1
        }
        Write-Host ("  {0}: fully converged - no literal values remain ({1} property sites, all tokenized)" -f
            $family.Name, $presenceCount)
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
    # Same convergence proof as the measured families: a dimension or interactive
    # pattern that stopped matching, or a corpus with no width/height binding at all,
    # is drift and fails; a live scan reading zero interactive targets below 32 px is
    # the converged end state and passes.
    if (-not [regex]::IsMatch('width: 28', $dimensionPattern) -or
        -not [regex]::IsMatch('onClicked: {}', $interactiveMarker)) {
        Write-Error 'hit area: the dimension or interactive pattern no longer matches its probe; the scan is broken.'
        exit 1
    }
    $dimensionPresence = 0
    foreach ($file in $qmlFiles) {
        $dimensionPresence += [regex]::Matches(
            [System.IO.File]::ReadAllText($file.FullName),
            '(?m)^\s*(?:implicitWidth|implicitHeight|width|height):').Count
    }
    if ($dimensionPresence -eq 0) {
        Write-Error 'hit area: no width/height binding occurs in the corpus; the pattern or the corpus changed.'
        exit 1
    }
    Write-Host ("  hit area: fully converged - no interactive target below 32 px remains " +
        "($dimensionPresence dimension sites)")
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
# Primitive token discipline. The shared primitives every screen composes from are fully
# converged (phase 4, 2026-10-14): inside these files a literal font size, animation
# duration or font family is a regression, and the only literal radii left are the recorded
# geometric halves - a radio dot that rounds at half its own fixed size. The rest of the
# corpus still carries literals under the baselines above; converging those is accepted by
# the measured counts at integration time, not by this section.
# ---------------------------------------------------------------------------
$primitiveFiles = @(
    'VcsToolButton.qml', 'ReviewActionButton.qml', 'VcsMenu.qml', 'VcsMenuItem.qml',
    'VcsMenuSeparator.qml', 'VcsMenuShortcut.qml', 'VcsRadioMenuItem.qml', 'VcsMenuBar.qml',
    'VcsToolTip.qml', 'ToolbarCombo.qml', 'ReviewOffsetSpinBox.qml', 'VcsScrollBar.qml'
)
# Literal radii a primitive may keep, per file, each with the reason it survives.
$primitiveRadiusExceptions = @{
    # radius: 3 - half of the 6 px radio dot; tst_vcs_menu pins checkIndicator.radius === 3.
    'VcsMenuItem.qml' = 1
}
$primitiveRules = @(
    @{ Name = 'font size';   Pattern = '(?:font\.)?pixelSize:\s*[0-9]' },
    @{ Name = 'duration';    Pattern = '(?:^|[^\w.])duration:\s*[0-9]' },
    @{ Name = 'font family'; Pattern = 'font\.family:\s*"' }
)

# 7. Every exception entry must name a primitive file, or it silently guards nothing.
$orphanExceptions = @($primitiveRadiusExceptions.Keys | Where-Object { $primitiveFiles -notcontains $_ })
if ($orphanExceptions.Count -gt 0) {
    foreach ($orphan in $orphanExceptions) {
        Write-Error "primitive radius exception for '$orphan' names no file in the primitive list."
    }
    exit 1
}
Write-Host 'PASS: every primitive radius exception names a primitive file'
$passed++

# 8. One check per primitive file and rule: zero literal sites allowed.
foreach ($name in $primitiveFiles) {
    $path = Join-Path $qmlRoot $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Write-Error "Primitive '$name' not found under $qmlRoot; the list or the file changed."
        exit 1
    }
    $text = [System.IO.File]::ReadAllText($path)
    foreach ($rule in $primitiveRules) {
        $hits = [regex]::Matches($text, $rule.Pattern)
        if ($hits.Count -gt 0) {
            Write-Error ("{0}: {1} literal {2} site(s); use a Theme token (first at '{3}')" -f
                $name, $hits.Count, $rule.Name, $hits[0].Value.Trim())
            exit 1
        }
        $passed++
    }
    $radiusCeiling = 0
    if ($primitiveRadiusExceptions.ContainsKey($name)) {
        $radiusCeiling = [int]$primitiveRadiusExceptions[$name]
    }
    $radiusHits = [regex]::Matches($text, 'radius:\s*[0-9]')
    if ($radiusHits.Count -gt $radiusCeiling) {
        Write-Error ("{0}: {1} literal radius site(s), ceiling {2}; use Theme.radius* or a " +
            "geometric expression (first at '{3}')" -f $name, $radiusHits.Count, $radiusCeiling,
            $radiusHits[0].Value.Trim())
        exit 1
    }
    $passed++
}
Write-Host (("PASS: all {0} primitives carry zero literal sizes, durations and families; radii " +
    'only in the recorded geometric exceptions') -f $primitiveFiles.Count)

# 9. A production QML file that imports neither QtQuick.Controls nor QtQuick.Templates and
#    instantiates no sibling component cannot use visualFocus at all: plain Items lack the
#    property, the binding silently never shows (backlog U10) and qmllint does not flag it.
#    Syntax-level rule only - whether a Control draws its ring correctly is a behaviour
#    question (tst_review_action_button, tst_toolbar_combo, tst_review_offset_spin_box).
$controlImport = '(?m)^import\s+QtQuick\.(Controls|Templates)\b'
$siblingNames = @($qmlFiles | ForEach-Object { $_.BaseName })
$visualFocusMisuses = [System.Collections.Generic.List[string]]::new()
foreach ($file in $qmlFiles) {
    $text = [System.IO.File]::ReadAllText($file.FullName)
    if ([regex]::IsMatch($text, $controlImport)) {
        continue
    }
    $instantiatesSibling = $false
    foreach ($sibling in $siblingNames) {
        if ([regex]::IsMatch($text, '\b' + [regex]::Escape($sibling) + '\s*\{')) {
            $instantiatesSibling = $true
            break
        }
    }
    if ($instantiatesSibling) {
        continue
    }
    $lineNumber = 0
    foreach ($line in [System.IO.File]::ReadAllLines($file.FullName)) {
        $lineNumber++
        if ($line -match '\.\s*visualFocus') {
            $visualFocusMisuses.Add("$($file.Name):$lineNumber")
        }
    }
}
if ($visualFocusMisuses.Count -gt 0) {
    foreach ($misuse in $visualFocusMisuses) {
        Write-Error "$misuse binds visualFocus in a file with no Controls: an Item has no such property and the ring would never show (U10)."
    }
    exit 1
}
Write-Host 'PASS: no plain-QtQuick file references visualFocus (U10 syntax rule)'
$passed++

# ---------------------------------------------------------------------------
# Self-asserting check count. AGENTS.md requires a script to assert its own check count so
# a missing anchor cannot turn its checks into silent passes. The total is derived from the
# tables above so it cannot drift from them: 3 structural checks, one unknown-value check per
# family (the four measured families plus the hit-area family), one baseline check per
# allow-listed value, one exception-table check, four checks per primitive file (three zero
# rules plus the radius ceiling) and one visualFocus syntax check. Adding a family, a token,
# a baseline entry, a primitive or an exception without adding its check breaks this
# assertion on purpose.
# ---------------------------------------------------------------------------
$baselineEntries = $fontBaseline.Count + $radiusBaseline.Count + $durationBaseline.Count +
$spacingBaseline.Count + $hitAreaBaseline.Count
$expectedCases = 3 + ($families.Count + 1) + $baselineEntries + 1 +
    ($primitiveFiles.Count * ($primitiveRules.Count + 1)) + 1
if ($passed -ne $expectedCases) {
    throw "Expected $expectedCases QML token-literal cases; ran $passed."
}

Write-Host ("QML token-literal checks passed: $passed cases, $totalSites literal sites across 5 " +
    "families ($totalTokenValued already at a token magnitude, $totalBaselined under a baseline " +
    'ceiling).')
