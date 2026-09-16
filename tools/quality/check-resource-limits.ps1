[CmdletBinding()]
param(
    [string]$PresetPath,

    [string]$WorkflowRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($PresetPath)) {
    $PresetPath = Join-Path $PSScriptRoot '..\..\CMakePresets.json'
}
if ([string]::IsNullOrWhiteSpace($WorkflowRoot)) {
    $WorkflowRoot = Join-Path $PSScriptRoot '..\..\.github\workflows'
}

$errors = [System.Collections.Generic.List[string]]::new()
$presets = Get-Content -LiteralPath $PresetPath -Raw | ConvertFrom-Json
$runnerPath = Join-Path $PSScriptRoot '..\ci\invoke-low-impact.ps1'
$performanceGatePath = Join-Path $PSScriptRoot '..\testing\run-performance-gate.ps1'

$runnerContent = Get-Content -LiteralPath $runnerPath -Raw
$runnerRequirements = @(
    '\$maxConcurrency\s*=\s*4',
    'ProcessorAffinity\s*=',
    'ProcessPriorityClass\]::BelowNormal',
    'ProcessPriorityClass\]::Normal',
    '\$env:DVS_EXECUTION_RESOURCE_PROFILE',
    'switch \(\$resourceProfile\)',
    '\$env:CMAKE_BUILD_PARALLEL_LEVEL\s*=',
    '\$env:CTEST_PARALLEL_LEVEL\s*=\s*''1''',
    '\$env:VCPKG_MAX_CONCURRENCY\s*='
)
foreach ($requirement in $runnerRequirements) {
    if ($runnerContent -notmatch $requirement) {
        $errors.Add("Low-impact runner is missing policy token '$requirement'.")
    }
}

$performanceGateContent = Get-Content -LiteralPath $performanceGatePath -Raw
foreach ($requirement in @(
        'ProcessorAffinity\s*=',
        'PriorityClass\s*=',
        '\$null\s+-ne\s+\$processExitCode'
    )) {
    if ($performanceGateContent -notmatch $requirement) {
        $errors.Add("Performance gate is missing child-process policy token '$requirement'.")
    }
}

$basePreset = $presets.configurePresets | Where-Object name -CEQ 'base'
if (-not $basePreset) {
    $errors.Add('Missing base configure preset.')
} else {
    $expectedEnvironment = @{
        CMAKE_BUILD_PARALLEL_LEVEL = '4'
        CTEST_PARALLEL_LEVEL = '1'
        VCPKG_MAX_CONCURRENCY = '4'
    }
    foreach ($entry in $expectedEnvironment.GetEnumerator()) {
        $actual = $basePreset.environment.($entry.Key)
        if ($actual -cne $entry.Value) {
            $errors.Add("Expected base environment $($entry.Key)=$($entry.Value); found '$actual'.")
        }
    }
}

foreach ($preset in $presets.buildPresets) {
    if ($preset.jobs -ne 4) {
        $errors.Add("Build preset '$($preset.name)' must set jobs to 4.")
    }
}

$explicitTestPresets = $presets.testPresets |
    Where-Object { -not $_.PSObject.Properties['inherits'] }
foreach ($preset in $explicitTestPresets) {
    if ($preset.execution.jobs -ne 1) {
        $errors.Add("Test preset '$($preset.name)' must set execution.jobs to 1.")
    }
}

$heavyCommandPattern = '(?m)^\s*(?:&\s+)?(?:cmake|ctest|cpack)(?:\.exe)?\s'
$wrapperPattern = '(?m)^\s*(?:pwsh|powershell(?:\.exe)?)\s+-NoProfile\s+-File\s+' +
    '\.\\tools\\ci\\invoke-low-impact\.ps1\s'
$runBlockPattern = '(?ms)^\s*run:\s*[>|]-?\s*\r?\n(?<body>(?:^\s{10,}.*(?:\r?\n|$))+)'
$inlineRunPattern = '(?m)^\s*run:\s+(?<body>.+)$'

$workflowPaths = Get-ChildItem -LiteralPath $WorkflowRoot -File |
    Where-Object Extension -In @('.yml', '.yaml')
foreach ($workflowPath in $workflowPaths) {
    $content = Get-Content -LiteralPath $workflowPath.FullName -Raw
    $interactiveCount = [regex]::Matches(
        $content,
        '(?im)^\s*DVS_EXECUTION_RESOURCE_PROFILE:\s+Interactive\s*$'
    ).Count
    $interactiveAllowed = @('hardware-performance.yml', 'release.yml')
    if ($workflowPath.Name -notin $interactiveAllowed -and $interactiveCount -ne 0) {
        $errors.Add(
            "Only hardware-performance.yml and release.yml may use the Interactive resource profile; found it in '$($workflowPath.Name)'."
        )
    }
    if ($workflowPath.Name -ceq 'hardware-performance.yml') {
        if ($interactiveCount -ne 2) {
            $errors.Add(
                'hardware-performance.yml must use the Interactive resource profile for exactly two CTest invocations.'
            )
        }
        foreach ($preset in @('hardware-d3d11', 'performance-d3d11')) {
            $expectedInvocation =
                "(?is)DVS_EXECUTION_RESOURCE_PROFILE:\s+Interactive.*?invoke-low-impact\.ps1\s+ctest\s+--preset\s+$preset\s+--output-on-failure"
            if ($content -notmatch $expectedInvocation) {
                $errors.Add(
                    "hardware-performance.yml must run '$preset' through the Interactive resource profile."
                )
            }
        }
    }
    foreach ($match in [regex]::Matches($content, $runBlockPattern)) {
        $body = $match.Groups['body'].Value
        foreach ($line in $body -split '\r?\n') {
            if ($line -match $heavyCommandPattern) {
                $errors.Add("Unthrottled heavy command in '$($workflowPath.Name)': $line")
            }
        }
        if ($body -match '(?m)\binvoke-low-impact\.ps1\b' -and $body -notmatch $wrapperPattern) {
            $errors.Add("Malformed low-impact invocation in '$($workflowPath.Name)'.")
        }
    }
    foreach ($match in [regex]::Matches($content, $inlineRunPattern)) {
        $body = $match.Groups['body'].Value
        if ($body -match '^\s*(?:cmake|ctest|cpack)(?:\.exe)?\s') {
            $errors.Add("Unthrottled inline command in '$($workflowPath.Name)': $body")
        }
    }
}

if ($errors.Count -gt 0) {
    foreach ($message in $errors) {
        Write-Error $message -ErrorAction Continue
    }
    exit 1
}

Write-Host (
    "Resource limit checks passed ($($presets.buildPresets.Count) build presets, " +
    "$($presets.testPresets.Count) test presets, $($workflowPaths.Count) workflows)."
)
