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

$prose = [regex]::Replace(
    $content,
    '(?ms)^[ \t]*```[^\r\n]*\r?\n.*?^[ \t]*```[ \t]*(?:\r?\n|$)',
    ''
)
$prose = [regex]::Replace(
    $prose,
    '(?ms)^[ \t]*~~~[^\r\n]*\r?\n.*?^[ \t]*~~~[ \t]*(?:\r?\n|$)',
    ''
)
$wordPattern = "[A-Za-z0-9]+(?:['-][A-Za-z0-9]+)*"
$wordCount = [regex]::Matches($prose, $wordPattern).Count
$totalWordCount = [regex]::Matches($content, $wordPattern).Count
if ($wordCount -lt 300 -or $wordCount -gt 380) {
    $errors.Add("Expected 300-380 prose words after fenced-block removal; found $wordCount.")
}
if ($totalWordCount -lt 200 -or $totalWordCount -gt 400) {
    $errors.Add("Expected 200-400 total words; found $totalWordCount.")
}

$requiredTokens = @(
    'domain',
    'application',
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
    'cmake --preset dev',
    'cmake --build --preset dev',
    'ctest --preset dev --output-on-failure',
    '.\out\build\dev\bin\VCStationCli.exe --startup-check',
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
    "Repository guide checks passed ($wordCount prose/$totalWordCount total words, " +
    "$($requiredCommands.Count) commands)."
)
