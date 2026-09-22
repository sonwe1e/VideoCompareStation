#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$GuidePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($GuidePath)) {
    $GuidePath = Join-Path $PSScriptRoot '..\..\AGENTS.md'
}

if (-not (Test-Path -LiteralPath $GuidePath -PathType Leaf)) {
    Write-Error "Repository guide not found: $GuidePath"
    exit 1
}

$content = Get-Content -LiteralPath $GuidePath -Raw
$errors = [System.Collections.Generic.List[string]]::new()

$firstLine = ($content -split '\r?\n', 2)[0]
if ($firstLine -cne '# Repository Guidelines') {
    $errors.Add("Expected exact first line '# Repository Guidelines'.")
}

# Check the engineering contract, not a word count: useful routing and tool setup guidance
# must be allowed to grow without weakening the architecture or performance requirements.
$requiredTokens = @(
    'docs/agent-guide.md',
    'docs/product/visual-review.md',
    'docs/engineering/visual-review-backlog.md',
    'domain',
    'application',
    'presentation_contract',
    'platform_windows',
    'media_ffmpeg',
    'persistence_json',
    'ui_qml',
    '80%',
    '0.5%',
    '500 ms',
    '100 ms',
    '256 MiB',
    'PascalCase',
    'lowerCamelCase',
    'kPascalCase',
    'dvs::<module>',
    'qmlformat',
    'qmllint',
    'clang-format',
    '19.1.5',
    'clang-tidy',
    'PowerShell 7',
    'pwsh',
    'tools/build/env.ps1',
    'tools/build/build.ps1',
    'outer types',
    'never enter core',
    'Never block GUI/render threads',
    'session/generation/request',
    'transactionally',
    'Preserve approved tests and performance gates',
    'Conventional Commits',
    'FFmpeg',
    'D3D11'
)

foreach ($token in $requiredTokens) {
    if ($content.IndexOf($token, [System.StringComparison]::Ordinal) -lt 0) {
        $errors.Add("Missing required token: $token")
    }
}

$requiredCommands = @(
    'pwsh tools/build/build.ps1 -Preset dev',
    'pwsh tools/build/build.ps1 -Preset release -Test',
    'pwsh tools/build/build.ps1 -Preset dev -Target format-check',
    'pwsh tools/build/build.ps1 -Preset dev -Target lint',
    'cmake --preset dev',
    'cmake --build --preset dev',
    'ctest --preset dev --output-on-failure',
    '.\out\build\dev\bin\CompareStationCli.exe --startup-check',
    'cmake --build --preset dev --target format-check',
    'cmake --build --preset dev --target lint',
    'cmake --preset release',
    'cmake --build --preset release',
    'cpack --preset release-zip',
    'cpack --preset release-msi'
)

foreach ($command in $requiredCommands) {
    $linePattern = '(?m)^[ \t]*' + [regex]::Escape($command) + '[ \t]*\r?$'
    if (-not [regex]::IsMatch($content, $linePattern)) {
        $errors.Add("Missing required command line: $command")
    }
}

$forbiddenRepositoryName = 'DualVideo' + 'Tool'
if ($content.IndexOf($forbiddenRepositoryName, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
    $errors.Add('The guide references the legacy repository.')
}

if ($errors.Count -gt 0) {
    foreach ($message in $errors) {
        Write-Error $message -ErrorAction Continue
    }
    exit 1
}

Write-Host (
    "Repository guide checks passed ($($requiredTokens.Count) required contract terms, " +
    "$($requiredCommands.Count) commands)."
)
