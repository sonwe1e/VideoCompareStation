#requires -Version 7.0
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:RUNNER_ENVIRONMENT -cne 'github-hosted' -or $env:RUNNER_OS -cne 'Windows') {
    throw 'Provisioning is restricted to disposable GitHub-hosted Windows runners.'
}
if (-not $env:RUNNER_TEMP -or -not $env:VCToolsVersion -or -not $env:WindowsSDKVersion) {
    throw 'RUNNER_TEMP and the MSVC x64/Windows SDK developer environment are required.'
}
$toolRoot = Join-Path $env:RUNNER_TEMP 'dvs-ci'
$pythonRoot = Join-Path $toolRoot 'python'
& python -m venv $pythonRoot
if ($LASTEXITCODE -ne 0) { throw 'Could not create the isolated CI Python environment.' }
$python = Join-Path $pythonRoot 'Scripts/python.exe'
& $python -m pip install --disable-pip-version-check --only-binary=:all: cmake==4.4.0 ninja==1.13.0
if ($LASTEXITCODE -ne 0) { throw 'Could not install pinned CMake/Ninja.' }

$llvmRoot = Join-Path $toolRoot 'llvm'
$llvmBin = Join-Path $llvmRoot 'bin'
if (-not (Test-Path -LiteralPath (Join-Path $llvmBin 'clang-tidy.exe'))) {
    $installer = Join-Path $toolRoot 'LLVM-19.1.5-win64.exe'
    Invoke-WebRequest -Uri (
        'https://github.com/llvm/llvm-project/releases/download/' +
        'llvmorg-19.1.5/LLVM-19.1.5-win64.exe'
    ) -OutFile $installer
    # Microsoft winget-pkgs LLVM.LLVM 19.1.5 x64 installer manifest.
    $expected = '9B29856202D82047B504A9E501D66080BED0EAFF3F1127191F4F361E0E00C770'
    if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -cne $expected) {
        throw 'LLVM installer checksum mismatch.'
    }
    $install = Start-Process -FilePath $installer -ArgumentList @('/S', "/D=$llvmRoot") `
        -WindowStyle Hidden -Wait -PassThru
    if ($install.ExitCode -ne 0) { throw "LLVM install failed: $($install.ExitCode)" }
}

$toolScripts = Join-Path $pythonRoot 'Scripts'
$env:PATH = "$toolScripts;$llvmBin;$env:PATH"
$checks = 0
foreach ($tool in @('cmake', 'ctest', 'cpack')) {
    $version = & (Join-Path $toolScripts "$tool.exe") --version
    if ($LASTEXITCODE -ne 0 -or ($version -join "`n") -notmatch '\b4\.4\.0\b') {
        throw "Expected $tool 4.4.0, found $version"
    }
    $checks++
}
foreach ($tool in @('clang-format', 'clang-tidy')) {
    $version = & (Join-Path $llvmBin "$tool.exe") --version
    if ($LASTEXITCODE -ne 0 -or ($version -join "`n") -notmatch '\b19\.1\.5\b') {
        throw "Expected $tool 19.1.5, found $version"
    }
    $checks++
}
$ninjaVersion = & (Join-Path $toolScripts 'ninja.exe') --version
if ($LASTEXITCODE -ne 0 -or $ninjaVersion -notmatch '^1\.13\.0(?:[.-].*)?$') {
    throw 'Ninja version mismatch.'
}
$checks++
Get-Command cl.exe, lib.exe -ErrorAction Stop | Select-Object Name, Source
if ($checks -ne 6) { throw "Expected six tool checks, ran $checks." }
@($llvmBin, $toolScripts) | Out-File $env:GITHUB_PATH -Encoding utf8 -Append
"DVS_LLVM_BIN=$llvmBin" | Out-File $env:GITHUB_ENV -Encoding utf8 -Append
"msvc=$($env:VCToolsVersion.Trim())" | Out-File $env:GITHUB_OUTPUT -Encoding utf8 -Append
"sdk=$($env:WindowsSDKVersion.TrimEnd([char]92))" |
    Out-File $env:GITHUB_OUTPUT -Encoding utf8 -Append
Write-Host "Hosted Windows toolchain ready: $checks pinned tool checks."
