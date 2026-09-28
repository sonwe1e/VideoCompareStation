#requires -Version 7.0
<#
.SYNOPSIS
Checks that the repository's scripts are still scripts.

.DESCRIPTION
Two accidents this catches, and both have happened:

  - a formatter applied to a file whose extension it does not own. clang-format treats an unknown
    extension as C++ and rewrote a 470 line .ps1 into a file that no longer parsed, after which the
    only copy of a test suite had to be reconstructed from a log;
  - a .cmd/.bat that lost its ASCII or CRLF shape. cmd.exe decodes the file in the OEM code page, so
    non-ASCII bytes change the command, and a bare LF breaks label scanning, which once turned a
    documented "-Uninstall" into a second registration.

Every .ps1/.psm1 must parse with the PowerShell parser, and every .cmd/.bat must be pure ASCII with
CRLF line endings. The check reads files only.

.EXAMPLE
pwsh -NoProfile -File tools/quality/check-script-integrity.ps1
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

# Build outputs hold generated scripts of their own, and the vcpkg tree has hundreds of thousands of
# files, so the excluded directories are pruned while walking rather than filtered afterwards.
$skipDirectoryNames = @('out', '.git', 'node_modules')

function Get-RepositoryFile {
    param([string] $Root)

    $files = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    $pending = [System.Collections.Generic.Stack[System.IO.DirectoryInfo]]::new()
    $pending.Push((Get-Item -LiteralPath $Root))
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($child in Get-ChildItem -LiteralPath $current.FullName -Directory -ErrorAction SilentlyContinue) {
            if ($skipDirectoryNames -cnotcontains $child.Name) {
                $pending.Push($child)
            }
        }
        foreach ($file in Get-ChildItem -LiteralPath $current.FullName -File -ErrorAction SilentlyContinue) {
            $files.Add($file)
        }
    }
    return $files
}

function Get-RelativePath {
    param([string] $Path)

    return $Path.Substring($RepositoryRoot.Length + 1)
}

$errors = [System.Collections.Generic.List[string]]::new()
$parsedScripts = 0
$checkedCommandFiles = 0
$repositoryFiles = Get-RepositoryFile -Root $RepositoryRoot

foreach ($file in $repositoryFiles) {
    if (@('.ps1', '.psm1') -cnotcontains $file.Extension) {
        continue
    }
    $tokens = $null
    $parseErrors = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
        $file.FullName, [ref] $tokens, [ref] $parseErrors) | Out-Null
    $parsedScripts++
    if (@($parseErrors).Count -gt 0) {
        $first = @($parseErrors)[0]
        $errors.Add(
            "$(Get-RelativePath $file.FullName) is not valid PowerShell: " +
            "$($first.Message) (line $($first.Extent.StartLineNumber))")
    }
}

foreach ($file in $repositoryFiles) {
    if (@('.cmd', '.bat') -cnotcontains $file.Extension) {
        continue
    }
    $checkedCommandFiles++
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $nonAscii = @($bytes | Where-Object { $_ -gt 127 }).Count
    if ($nonAscii -gt 0) {
        $errors.Add(
            "$(Get-RelativePath $file.FullName) holds $nonAscii non-ASCII byte(s); cmd.exe reads it " +
            'in the OEM code page, so the command can change meaning.')
    }
    $bareLf = 0
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        if ($bytes[$index] -eq 10 -and ($index -eq 0 -or $bytes[$index - 1] -ne 13)) {
            $bareLf++
        }
    }
    if ($bareLf -gt 0) {
        $errors.Add(
            "$(Get-RelativePath $file.FullName) holds $bareLf bare LF line ending(s); cmd.exe label " +
            'scanning requires CRLF.')
    }
}

if ($errors.Count -gt 0) {
    foreach ($message in $errors) {
        Write-Error $message -ErrorAction Continue
    }
    exit 1
}

Write-Host (
    "Script integrity checks passed ($parsedScripts PowerShell files parsed, " +
    "$checkedCommandFiles command files byte-checked).")
