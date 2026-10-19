#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$PresetPath,

    [string]$WorkflowRoot,
    [string]$BuildDirectory = ''
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
$projectContent = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../../CMakeLists.txt') -Raw
$poolRequirements = @(
    'set\(CMAKE_JOB_POOLS dvs_background=2\)',
    'set\(CMAKE_JOB_POOL_COMPILE dvs_background\)',
    'set\(CMAKE_JOB_POOL_LINK dvs_background\)',
    'set\(CMAKE_AUTOGEN_PARALLEL 2\)',
    'set\(CMAKE_CXX_SCAN_FOR_MODULES OFF\)'
)
$poolChecks = 0
foreach ($requirement in $poolRequirements) {
    $poolChecks++
    if ([regex]::Matches($projectContent, $requirement).Count -ne 1) {
        $errors.Add("Direct Ninja resource protection is missing or ambiguous: $requirement")
    }
}
if ($poolChecks -ne 5) { throw "Expected 5 direct-Ninja resource checks; ran $poolChecks." }

function Test-NinjaResourcePools {
    param([string]$Graph, [string]$Rules)
    if ($Rules -notmatch '(?m)^pool dvs_background\r?\n  depth = 2\r?$') { return $false }
    $edges = [regex]::Matches($Graph,
        '(?m)^build [^\r\n]+: CXX_(?:COMPILER|EXECUTABLE_LINKER|STATIC_LIBRARY_LINKER|SHARED_LIBRARY_LINKER)[^\r\n]*\r?\n(?:  [^\r\n]*\r?\n)*')
    if ($edges.Count -eq 0) { return $false }
    foreach ($edge in $edges) {
        if ($edge.Value -notmatch '(?m)^  pool = dvs_background\r?$') { return $false }
    }
    return $true
}
if ($BuildDirectory) {
    $graph = Get-Content -LiteralPath (Join-Path $BuildDirectory 'build.ninja') -Raw
    $rules = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeFiles/rules.ninja') -Raw
    if (-not (Test-NinjaResourcePools $graph $rules)) {
        $errors.Add('Generated Ninja compile/link edges do not all share the two-slot pool.')
    }
    # Mutate in memory only. These counterexamples must be rejected by the same check.
    $counterexamples = 0
    foreach ($mutant in @(
            @($graph.Replace('  pool = dvs_background', ''), $rules),
            @($graph, $rules.Replace('depth = 2', 'depth = 24')))) {
        if (Test-NinjaResourcePools $mutant[0] $mutant[1]) {
            throw 'Ninja resource check accepted a missing pool or widened CPU budget.'
        }
        $counterexamples++
    }
    if ($counterexamples -ne 2) { throw 'Expected two Ninja resource counterexamples.' }
    Write-Host 'Generated Ninja pools verified; two resource-policy counterexamples rejected.'
}

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

$environmentPresets = @($presets.configurePresets | Where-Object name -In @('base', 'core-dev'))
if ($environmentPresets.Count -ne 2) { $errors.Add('Expected base and core-dev resource environments.') }
foreach ($basePreset in $environmentPresets) {
    $expectedEnvironment = @{
        CMAKE_BUILD_PARALLEL_LEVEL = '2'
        CTEST_PARALLEL_LEVEL = '1'
        VCPKG_MAX_CONCURRENCY = '2'
    }
    foreach ($entry in $expectedEnvironment.GetEnumerator()) {
        $actual = $basePreset.environment.($entry.Key)
        if ($actual -cne $entry.Value) {
            $errors.Add("Expected $($basePreset.name) environment $($entry.Key)=$($entry.Value); found '$actual'.")
        }
    }
}

foreach ($preset in $presets.buildPresets) {
    if ($preset.jobs -ne 2) {
        $errors.Add("Build preset '$($preset.name)' must set jobs to 2.")
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
