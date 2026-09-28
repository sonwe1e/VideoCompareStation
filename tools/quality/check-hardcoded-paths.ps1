#requires -Version 7.0
<#
.SYNOPSIS
Keeps machine-specific absolute paths out of the repository's executable files.

.DESCRIPTION
AGENTS.md already says tool discovery is centralized in tools/build/env.ps1 and that machine paths
must not be hardcoded; nothing enforced it. One workspace rename then left 16 defaults in
tools/testing pointing at a directory that no longer existed, which is invisible until the tool is
run and fails for a reason that has nothing to do with the change being tested.

Executable files (.ps1/.psm1/.cmd/.bat/.cmake/CMakeLists.txt/.json) outside out/ must derive their
paths from $PSScriptRoot, and must not name a sibling workspace root, a user profile, or the
checkout's former names. tools/build/env.ps1 is the one place allowed to probe machine locations,
and it builds its candidates from the checkout's own location rather than spelling them out, so it
needs no exemption.

The check reads files only.

.EXAMPLE
pwsh -NoProfile -File tools/quality/check-hardcoded-paths.ps1
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

# The patterns are assembled from fragments so that this file, which necessarily talks about them,
# does not trip its own check.
$separator = '[\\/]'
$machineRoots = @(
    ('Work' + 'spaces'),   # sibling workspaces, such as this checkout's parent
    ('Work' + 'Stations'), # sibling workspaces, such as the vcpkg tree beside this checkout
    ('Us' + 'ers')         # user profiles
)
$absoluteMachinePath = '[A-Za-z]:' + $separator + '(' + ($machineRoots -join '|') + ')' + $separator
$legacyRepositoryName = 'DualVideo' + 'Tool'

$scannedExtensions = @('.ps1', '.psm1', '.cmd', '.bat', '.cmake', '.json')
$skipSegments = @('\out\', '\.git\', '\node_modules\')

# Only repository sources are scanned: git-ignored files such as CMakeUserPresets.json exist
# precisely to hold this machine's paths, and flagging them would make the check noise. Untracked
# files that are not ignored are included, so a new script is checked before it is committed.
$candidates = $null
try {
    $listed = & git -C $RepositoryRoot ls-files --cached --others --exclude-standard 2>$null
    if ($LASTEXITCODE -eq 0 -and $listed) {
        $candidates = @($listed | ForEach-Object { Join-Path $RepositoryRoot $_ })
    }
}
catch {
    $candidates = $null
}
if ($null -eq $candidates) {
    Write-Host 'git was unavailable; falling back to walking the working tree.'
    $candidates = @(Get-ChildItem -LiteralPath $RepositoryRoot -Recurse -File -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName })
}

$errors = [System.Collections.Generic.List[string]]::new()
$scannedFiles = 0

foreach ($path in $candidates) {
    $file = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
    if ($null -eq $file) {
        continue
    }
    $isCommandList = $file.Name -ceq 'CMakeLists.txt'
    if (-not $isCommandList -and $scannedExtensions -cnotcontains $file.Extension) {
        continue
    }
    $skip = $false
    foreach ($segment in $skipSegments) {
        if ($file.FullName.Contains($segment)) {
            $skip = $true
        }
    }
    if ($skip) {
        continue
    }
    $scannedFiles++
    $relative = $file.FullName.Substring($RepositoryRoot.Length + 1)
    $lineNumber = 0
    foreach ($line in [System.IO.File]::ReadAllLines($file.FullName)) {
        $lineNumber++
        if ([regex]::IsMatch($line, $absoluteMachinePath)) {
            $errors.Add(
                "$relative($lineNumber) names a machine-specific absolute path; derive it from " +
                'PSScriptRoot, or use tools/build/env.ps1 for tool discovery.')
        }
        if ($line.IndexOf($legacyRepositoryName, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $errors.Add("$relative($lineNumber) references the legacy repository name.")
        }
    }
}

if ($errors.Count -gt 0) {
    foreach ($message in $errors) {
        Write-Error $message -ErrorAction Continue
    }
    exit 1
}

Write-Host "Hardcoded path checks passed ($scannedFiles executable files inspected)."
