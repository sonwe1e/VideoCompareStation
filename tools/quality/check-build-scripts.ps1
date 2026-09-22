#requires -Version 7.0
[CmdletBinding()]
param(
    # Each run keeps an isolated fixture below this directory for failure diagnosis.
    [string]$TestRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) {
    throw 'The Windows build-wrapper contract checks require Windows and PowerShell 7.'
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $TestRoot) {
    $TestRoot = Join-Path $repositoryRoot 'out\quality\build-scripts'
}
$fixtureRoot = Join-Path ([IO.Path]::GetFullPath($TestRoot)) ([guid]::NewGuid().ToString('N'))
$fixtureRepository = Join-Path $fixtureRoot 'repository with spaces'
$outsideDirectory = Join-Path $fixtureRoot 'outside repository'
$toolDirectory = Join-Path $fixtureRoot 'tools with spaces'
$vcpkgRoot = Join-Path $fixtureRoot 'vcpkg with spaces'
$buildDirectory = Join-Path $fixtureRepository 'out\build\dev'
$argumentLog = Join-Path $fixtureRoot 'arguments.log'
$vcvarsPath = Join-Path $toolDirectory 'vcvarsall.bat'
$fixtureBuild = Join-Path $fixtureRepository 'tools\build\build.ps1'
$pwshPath = (Get-Process -Id $PID).Path
$directories = @(
    (Join-Path $fixtureRepository 'tools\build'), $outsideDirectory, $toolDirectory,
    (Join-Path $vcpkgRoot 'scripts\buildsystems'), $buildDirectory
)
foreach ($directory in $directories) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
foreach ($fileName in @('build.ps1', 'env.ps1')) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "tools\build\$fileName") `
        -Destination (Join-Path $fixtureRepository "tools\build\$fileName")
}
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'CMakePresets.json') `
    -Destination $fixtureRepository
Set-Content -LiteralPath (Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake') `
    -Value '# Build-wrapper fixture; never configures a real project.'
Set-Content -LiteralPath $vcvarsPath -Encoding ascii -Value @'
@echo off
set TOY_BUILD_TEST_VCVARS=imported value with spaces
set VSCMD_ARG_HOST_ARCH=x64
set VSCMD_ARG_TGT_ARCH=x64
set VCPKG_ROOT=Z:\overridden-by-vcvars\vcpkg
exit /b 0
'@

# Native fixtures preserve argv exactly. A .bat cmake would itself go through cmd.exe and
# could conceal the very argument-quoting regression these checks are intended to catch.
# The .NET Framework compiler ships with Windows; no Visual Studio or package restore is needed.
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
}
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw 'The Windows .NET Framework C# compiler is required to create native test fixtures.'
}
$sourceFile = Join-Path $fixtureRoot 'FixtureTool.cs'
Set-Content -LiteralPath $sourceFile -Encoding utf8 -Value @'
using System;
using System.IO;
using System.Text;

internal static class FixtureTool {
    private static string Encode(string value) {
        return Convert.ToBase64String(Encoding.UTF8.GetBytes(value ?? ""));
    }

    private static bool Has(string[] args, string value) {
        return Array.IndexOf(args, value) >= 0;
    }

    public static int Main(string[] args) {
        string tool = Path.GetFileNameWithoutExtension(Environment.GetCommandLineArgs()[0]);
        string log = Environment.GetEnvironmentVariable("TOY_BUILD_TEST_LOG");
        StringBuilder record = new StringBuilder(tool);
        record.Append('\t').Append(Encode(Environment.CurrentDirectory));
        record.Append('\t').Append(Encode(Environment.GetEnvironmentVariable("TOY_BUILD_TEST_VCVARS")));
        record.Append('\t').Append(Encode(Environment.GetEnvironmentVariable("VSLANG")));
        foreach (string arg in args) record.Append('\t').Append(Encode(arg));
        File.AppendAllText(log, record.ToString() + Environment.NewLine);
        if (Has(args, "--version")) {
            Console.WriteLine(tool == "ninja" ? "1.12.1" : tool + " version 3.31.8");
            return 0;
        }
        string mode = Environment.GetEnvironmentVariable("TOY_BUILD_TEST_MODE");
        if (tool == "ctest" && mode == "no-tests") {
            Console.Error.WriteLine("No tests were found!!!");
            return Has(args, "--no-tests=error") ? 8 : 0;
        }
        Console.WriteLine("FIXTURE STDOUT: spaces | & >");
        Console.Error.WriteLine("FIXTURE STDERR: spaces | & >");
        if (tool == "cmake" && Has(args, "--build") && mode == "build-fails") return 23;
        if (tool == "cmake" && !Has(args, "--build") && mode == "configure-fails") return 24;
        if (tool == "cmake" && !Has(args, "--build")) {
            string root = Environment.GetEnvironmentVariable("VCPKG_ROOT");
            string binary = Path.Combine(Environment.CurrentDirectory, "out", "build", "dev");
            Directory.CreateDirectory(binary);
            File.WriteAllText(Path.Combine(binary, "CMakeCache.txt"),
                "CMAKE_HOME_DIRECTORY:INTERNAL=" + Environment.CurrentDirectory + "\n" +
                "CMAKE_TOOLCHAIN_FILE:FILEPATH=" + Path.Combine(root, "scripts", "buildsystems", "vcpkg.cmake") + "\n" +
                "CMAKE_GENERATOR:INTERNAL=Ninja\n" +
                "CMAKE_MAKE_PROGRAM:FILEPATH=" + Path.Combine(Path.GetDirectoryName(Environment.GetCommandLineArgs()[0]), "ninja.exe") + "\n");
            File.WriteAllText(Path.Combine(binary, "build.ninja"), "# native fixture\n");
        }
        return 0;
    }
}
'@
$fixtureExecutable = Join-Path $toolDirectory 'cmake.exe'
& $compiler /nologo /target:exe "/out:$fixtureExecutable" $sourceFile
if ($LASTEXITCODE -ne 0) {
    throw "Could not compile native build fixtures (exit $LASTEXITCODE)."
}
foreach ($toolName in @('ctest', 'ninja')) {
    Copy-Item -LiteralPath $fixtureExecutable -Destination (Join-Path $toolDirectory "$toolName.exe")
}

$runner = Join-Path $fixtureRoot 'run-case.ps1'
Set-Content -LiteralPath $runner -Encoding utf8 -Value @'
#requires -Version 7.0
param([Parameter(Mandatory)][string]$SettingsPath)
$ErrorActionPreference = 'Stop'
$settings = Get-Content -LiteralPath $SettingsPath -Raw | ConvertFrom-Json -AsHashtable
$env:TOY_BUILD_TEST_LOG = $settings.Log
$env:TOY_BUILD_TEST_MODE = $settings.Mode
$env:TOY_BUILD_TEST_VCVARS = ''
# Keep machine-local overrides from contaminating the fixture's explicit paths.
$env:VCPKG_ROOT = ''
$env:VCVARSALL_PATH = ''
$env:NINJA_BIN = ''
$env:VSLANG = '2052'
Set-Location -LiteralPath $settings.WorkingDirectory
$arguments = $settings.Arguments
$global:LASTEXITCODE = 0
try {
    & $settings.Script @arguments
    exit $LASTEXITCODE
} catch {
    [Console]::Error.WriteLine($_.ToString())
    exit 1
}
'@

function Invoke-WrapperCase {
    param([hashtable]$Overrides = @{}, [string]$Mode = '')
    $parameters = @{
        Preset = 'dev'
        VcpkgRoot = $vcpkgRoot
        VcvarsAll = $vcvarsPath
        CMakeExecutable = $fixtureExecutable
        NinjaExecutable = (Join-Path $toolDirectory 'ninja.exe')
    }
    foreach ($key in $Overrides.Keys) {
        $parameters[$key] = $Overrides[$key]
    }
    Set-Content -LiteralPath $argumentLog -Value '' -NoNewline
    $settingsPath = Join-Path $fixtureRoot 'case.json'
    @{
        Script = $fixtureBuild
        WorkingDirectory = $outsideDirectory
        Arguments = $parameters
        Log = $argumentLog
        Mode = $Mode
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $settingsPath -Encoding utf8
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $pwshPath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('-NoLogo', '-NoProfile', '-File', $runner, $settingsPath)) {
        $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) {
            $process.Kill($true)
            throw 'Build-wrapper fixture exceeded its 60-second timeout.'
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $records = @(foreach ($line in (Get-Content -LiteralPath $argumentLog)) {
            if (-not $line) { continue }
            $fields = $line.Split([char]9)
            $decoded = @(foreach ($field in $fields[1..($fields.Length - 1)]) {
                [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($field))
            })
            [pscustomobject]@{
                Tool = $fields[0]
                WorkingDirectory = $decoded[0]
                VcvarsValue = $decoded[1]
                VSLang = $decoded[2]
                Arguments = @($decoded | Select-Object -Skip 3)
            }
        })
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdout
            Stderr = $stderr
            Records = $records
        }
    } finally {
        $process.Dispose()
    }
}

function Assert-Contract {
    param([bool]$Condition, [string]$Message, $Result)
    if (-not $Condition) {
        if ($null -ne $Result) {
            throw "$Message`nExit: $($Result.ExitCode)`nSTDOUT:`n$($Result.Stdout)`nSTDERR:`n$($Result.Stderr)`nFixture: $fixtureRoot"
        }
        throw "$Message`nFixture: $fixtureRoot"
    }
}

$passed = 0
$result = Invoke-WrapperCase
Assert-Contract ($result.ExitCode -eq 0) 'Build from outside the repository failed.' $result
$configureCalls = @($result.Records | Where-Object {
    $_.Tool -eq 'cmake' -and $_.Arguments -contains '--preset' -and $_.Arguments -notcontains '--build'
})
$buildCalls = @($result.Records | Where-Object { $_.Arguments -contains '--build' })
Assert-Contract ($configureCalls.Count -eq 1 -and $buildCalls.Count -eq 1) `
    'An uncached build must configure once and build once.' $result
foreach ($record in @($configureCalls) + @($buildCalls)) {
    Assert-Contract ($record.WorkingDirectory -eq $fixtureRepository) `
        'CMake ran in the caller directory instead of the repository.' $result
    Assert-Contract ($record.VcvarsValue -eq 'imported value with spaces') `
        'The imported vcvars environment did not reach CMake.' $result
    Assert-Contract ($record.VSLang -eq '1033') `
        'MSVC diagnostics are not pinned to English for Ninja dependency parsing.' $result
}
Assert-Contract ($result.Stdout.Contains("FIXTURE STDOUT: spaces | & >`r`n")) `
    'The native stdout line was swallowed or rewritten.' $result
Assert-Contract ($result.Stderr.Contains("FIXTURE STDERR: spaces | & >`r`n")) `
    'The native stderr line was swallowed or rewritten.' $result
Write-Host 'PASS: outside working directory, paths with spaces, vcvars import, native output'
$passed++

$lockPath = Join-Path $fixtureRepository 'out\build\.dev.lock'
$heldLock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate,
    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    foreach ($arguments in @(@{}, @{ TestOnly = $true; TestRegex = 'ui[.]Fixture' })) {
        $result = Invoke-WrapperCase $arguments
        Assert-Contract ($result.ExitCode -ne 0) 'A concurrent operation bypassed the preset lock.' $result
        Assert-Contract ($result.Records.Count -eq 0) `
            'A locked preset still executed a build or test tool.' $result
        Assert-Contract (($result.Stdout + $result.Stderr).Contains("preset 'dev'")) `
            'The lock failure did not identify the busy preset.' $result
    }
} finally {
    $heldLock.Dispose()
}
$result = Invoke-WrapperCase
Assert-Contract ($result.ExitCode -eq 0) 'The preset remained locked after its owner released it.' $result
Assert-Contract (@($result.Records | Where-Object { $_.Arguments -contains '--build' }).Count -eq 1) `
    'The released preset did not resume normal builds.' $result
Write-Host 'PASS: preset lock excludes concurrent builds and tests, then releases cleanly'
$passed++

$regex = '^(ui[.]A|application[.]B)$|[&><^%! "]'
$result = Invoke-WrapperCase @{ Target = @('format-check', 'target with spaces'); Test = $true; TestRegex = $regex }
Assert-Contract ($result.ExitCode -eq 0) 'Targets and CTest regex invocation failed.' $result
$buildCalls = @($result.Records | Where-Object { $_.Arguments -contains '--build' })
$testCalls = @($result.Records | Where-Object { $_.Tool -eq 'ctest' -and $_.Arguments -notcontains '--version' })
Assert-Contract ($buildCalls.Count -eq 1 -and $testCalls.Count -eq 1) `
    'Expected one build followed by one CTest invocation.' $result
Assert-Contract ($buildCalls[0].Arguments -contains 'format-check' -and
    $buildCalls[0].Arguments -contains 'target with spaces') 'Target array elements were changed.' $result
$regexIndex = [Array]::IndexOf($testCalls[0].Arguments, '-R')
Assert-Contract ($regexIndex -ge 0 -and $testCalls[0].Arguments[$regexIndex + 1] -ceq $regex) `
    'CTest regex was split or interpreted by a shell.' $result
Write-Host 'PASS: target array and CTest regex preserve literal arguments'
$passed++

$result = Invoke-WrapperCase @{ Configure = $true }
Assert-Contract ($result.ExitCode -eq 0) '-Configure failed with an existing cache.' $result
Assert-Contract (@($result.Records | Where-Object {
    $_.Tool -eq 'cmake' -and $_.Arguments -contains '--preset' -and $_.Arguments -notcontains '--build'
}).Count -eq 1) '-Configure did not configure the existing build tree.' $result
Write-Host 'PASS: Configure explicitly refreshes an existing cache'
$passed++

$result = Invoke-WrapperCase @{ UseInstalledDependencies = $true }
Assert-Contract ($result.ExitCode -eq 0) '-UseInstalledDependencies failed.' $result
Assert-Contract (@($result.Records | Where-Object {
    $_.Tool -eq 'cmake' -and $_.Arguments -contains '-DVCPKG_MANIFEST_INSTALL=OFF'
}).Count -eq 1) 'Preinstalled dependency mode was not passed explicitly to CMake.' $result
Write-Host 'PASS: preinstalled dependency mode is explicit in configure arguments'
$passed++

$result = Invoke-WrapperCase @{ Test = $true } 'build-fails'
Assert-Contract ($result.ExitCode -ne 0) 'A native build failure reported success.' $result
Assert-Contract ($result.Stderr.Contains('FIXTURE STDERR: spaces | & >')) `
    'Build failure hid native stderr.' $result
Assert-Contract (@($result.Records | Where-Object { $_.Tool -eq 'ctest' }).Count -eq 0) `
    'Tests ran after a failed build.' $result
Write-Host 'PASS: native build failure is visible and prevents tests'
$passed++

$result = Invoke-WrapperCase @{ TestOnly = $true; TestRegex = 'does-not-match-any-test' } 'no-tests'
Assert-Contract ($result.ExitCode -ne 0) 'Zero matching tests incorrectly reported success.' $result
Assert-Contract (@($result.Records | Where-Object { $_.Tool -eq 'cmake' -and $_.Arguments -contains '--build' }).Count -eq 0) `
    'TestOnly unexpectedly built the project.' $result
Write-Host 'PASS: zero matching tests fails without building'
$passed++

foreach ($parameter in @('VcpkgRoot', 'VcvarsAll', 'CMakeExecutable', 'NinjaExecutable')) {
    $missing = Join-Path $fixtureRoot "missing $parameter"
    $result = Invoke-WrapperCase @{ $parameter = $missing }
    Assert-Contract ($result.ExitCode -ne 0) "An explicitly invalid $parameter was ignored." $result
    Assert-Contract (($result.Stdout + $result.Stderr).Contains($missing)) `
        "The invalid $parameter diagnostic did not identify its path." $result
    Assert-Contract (@($result.Records | Where-Object { $_.Arguments -contains '--build' }).Count -eq 0) `
        "The wrapper built with an invalid $parameter." $result
    Write-Host "PASS: explicit invalid $parameter reports its path"
    $passed++
}

$cachePath = Join-Path $buildDirectory 'CMakeCache.txt'
$cache = Get-Content -LiteralPath $cachePath -Raw
$cache.Replace($vcpkgRoot, (Join-Path $fixtureRoot 'old vcpkg')) | Set-Content -LiteralPath $cachePath
$result = Invoke-WrapperCase
Assert-Contract ($result.ExitCode -ne 0) 'A cache from another vcpkg silently continued.' $result
Assert-Contract (($result.Stdout + $result.Stderr).Contains('-Fresh')) `
    'The stale-cache diagnostic does not explain how to recover with -Fresh.' $result
Assert-Contract (@($result.Records | Where-Object { $_.Arguments -contains '--build' }).Count -eq 0) `
    'A stale cache reached the build command.' $result
$result = Invoke-WrapperCase @{ Fresh = $true }
Assert-Contract ($result.ExitCode -eq 0) '-Fresh did not recover the stale cache.' $result
Assert-Contract (@($result.Records | Where-Object { $_.Arguments -contains '--fresh' }).Count -eq 1) `
    '-Fresh did not invoke CMake with --fresh.' $result
$cleanCalls = @($result.Records | Where-Object { $_.Tool -eq 'ninja' -and $_.Arguments -contains 'clean' })
Assert-Contract ($cleanCalls.Count -eq 1 -and $cleanCalls[0].Arguments -contains $buildDirectory) `
    '-Fresh did not clean generated Ninja outputs before building.' $result
Assert-Contract ($result.Records[0].Tool -eq 'cmake' -and
    $result.Records[0].Arguments -contains '--fresh' -and $result.Records[1].Tool -eq 'ninja') `
    '-Fresh must replace the old build graph before cleaning its generated outputs.' $result
Write-Host 'PASS: stale CMake cache requires explicit fresh configure'
$passed++

$cache = Get-Content -LiteralPath $cachePath -Raw
$cache.Replace($fixtureRepository, $outsideDirectory) | Set-Content -LiteralPath $cachePath
$result = Invoke-WrapperCase @{ Fresh = $true } 'configure-fails'
Assert-Contract ($result.ExitCode -ne 0) 'A failed fresh configure incorrectly reported success.' $result
Assert-Contract (@($result.Records | Where-Object { $_.Tool -eq 'ninja' }).Count -eq 0) `
    '-Fresh ran clean against the old foreign build graph after configure failed.' $result
Assert-Contract (@($result.Records | Where-Object { $_.Arguments -contains '--build' }).Count -eq 0) `
    '-Fresh built after configure failed.' $result
$result = Invoke-WrapperCase @{ Fresh = $true }
Assert-Contract ($result.ExitCode -eq 0) '-Fresh could not recover a cache from another repository.' $result
Assert-Contract ($result.Records[0].Arguments -contains '--fresh' -and
    $result.Records[1].Tool -eq 'ninja') `
    'A foreign build graph was cleaned before it was replaced.' $result
Write-Host 'PASS: foreign build cache is replaced before clean, and configure failure prevents clean'
$passed++

$missingCompiler = Join-Path $fixtureRoot 'removed compiler\cl.exe'
Add-Content -LiteralPath $cachePath -Value "CMAKE_CXX_COMPILER:FILEPATH=$missingCompiler"
$result = Invoke-WrapperCase @{ Doctor = $true }
Assert-Contract ($result.ExitCode -ne 0) 'Doctor accepted a cached compiler path that no longer exists.' $result
Assert-Contract (($result.Stdout + $result.Stderr).Contains($missingCompiler) -and
    ($result.Stdout + $result.Stderr).Contains('-Fresh')) `
    'The expired compiler diagnostic did not identify the path and recovery command.' $result
Assert-Contract ((Get-Content -LiteralPath $cachePath -Raw).Contains($missingCompiler)) `
    'Doctor modified the stale cache.' $result
$result = Invoke-WrapperCase @{ Fresh = $true }
Assert-Contract ($result.ExitCode -eq 0) '-Fresh did not recover the missing cached compiler.' $result
Write-Host 'PASS: Doctor identifies expired cached tools without modifying the cache'
$passed++

$result = Invoke-WrapperCase @{ Doctor = $true }
Assert-Contract ($result.ExitCode -eq 0) 'Doctor failed with valid fixture tools.' $result
Assert-Contract (@($result.Records | Where-Object {
    $_.Arguments -contains '--build' -or $_.Arguments -contains '--preset'
}).Count -eq 0) 'Doctor unexpectedly configured or built the project.' $result
Write-Host 'PASS: Doctor diagnoses without configuring or building'
$passed++

Write-Host "Build-script contracts passed: $passed cases. Fixtures: $fixtureRoot"
