#requires -Version 7.0
<#
.SYNOPSIS
Adds or removes the per-user Explorer command that opens CompareStation for video files.

.DESCRIPTION
CompareStation ships as an unpacked ZIP. A machine-wide MSI used to be the only thing that
registered this command - under HKLM, and only with administrator rights - and it was dropped
because it cost more to maintain than it returned. This script writes the same keys under
HKCU\Software\Classes, which Explorer merges into HKCR, so an extracted release can offer
"Open in CompareStation" / "Compare with CompareStation" for the current user without installing
anything.

The command itself is the IExplorerCommand implemented in CompareStationShell (see
src/shell_windows/ExplorerCommand.cpp). Explorer loads it per selection and passes one to three
selected files to CompareStation.exe on a single command line, which Main.cpp turns into a
single-video session or a comparison. Directories, network paths and selections of more than
three files leave the entry hidden, so nothing here has to duplicate that policy.

.PARAMETER InstallRoot
Directory holding CompareStation.exe and CompareStationShell-<version>.dll, normally the
extracted release ZIP. Required unless -Uninstall is used.

.PARAMETER Uninstall
Removes the keys this script writes, so the command disappears for the current user.

.PARAMETER Extensions
Extensions that get the command. The default is the set the shipped handler accepts - five video
and thirteen still-image extensions; keep it in sync with kSupportedVideoExtensions and
kSupportedImageExtensions in src/shell_windows/ExplorerCommandSupport.cpp.

.EXAMPLE
pwsh tools/shell/RegisterExplorerCommand.ps1 -InstallRoot 'D:\apps\CompareStation-1.8.0'

.EXAMPLE
pwsh tools/shell/RegisterExplorerCommand.ps1 -Uninstall
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string] $InstallRoot,

    [switch] $Uninstall,

    [string[]] $Extensions = @(
        '.mp4', '.mkv', '.mov', '.avi', '.m4v',
        '.png', '.jpg', '.jpeg', '.bmp', '.gif', '.webp', '.tif', '.tiff',
        '.pnm', '.ppm', '.pgm', '.pbm', '.pam'
    )
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Explorer command server shipped as CompareStationShell-<major>.<minor>.dll.
$explorerCommandClsid = '{3B790D74-E76E-4F28-A51D-2AB8C6BD107D}'
$explorerCommandTitle = 'CompareStation Explorer comparison command'
$verbKeyName = 'CompareStation.Compare'
$menuText = 'Compare with CompareStation'
$selectionModel = 'Player'

$classesRoot = 'HKCU:\Software\Classes'
$clsidKey = Join-Path $classesRoot "CLSID\$explorerCommandClsid"
$inprocKey = Join-Path $clsidKey 'InprocServer32'

function Get-ShellBinary {
    param([string] $Root, [string] $Executable)

    $candidates = @(
        Get-ChildItem -LiteralPath $Root -Filter 'CompareStationShell-*.dll' -File |
            ForEach-Object {
                if ($_.Name -match '^CompareStationShell-(?<version>\d+\.\d+)\.dll$') {
                    [pscustomobject]@{ Path = $_.FullName; Version = [version] $Matches.version }
                }
            } |
            Sort-Object -Property Version -Descending
    )
    if ($candidates.Count -eq 0) {
        throw (
            "No CompareStationShell-<major>.<minor>.dll found under '$Root'. " +
            'Pass the directory of an extracted CompareStation release.'
        )
    }
    # The executable loads the shell library of its own product version, so prefer that name. Build
    # directories keep copies of older releases next to the current one, and registering a stale DLL
    # would run an old handler.
    $productVersion = (Get-Item -LiteralPath $Executable).VersionInfo.FileVersion
    if ($productVersion -match '^(?<major>\d+)\.(?<minor>\d+)') {
        $expected = "CompareStationShell-$($Matches.major).$($Matches.minor).dll"
        $exact = $candidates |
            Where-Object { (Split-Path -Path $_.Path -Leaf) -ceq $expected } |
            Select-Object -First 1
        if ($exact) {
            return $exact.Path
        }
    }
    return $candidates[0].Path
}

function Set-RegistryString {
    param([string] $Path, [string] $Name, [string] $Value)

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -Force | Out-Null
    }
    Set-ItemProperty -LiteralPath $Path -Name $Name -Value $Value
}

function Invoke-ShellAssociationChanged {
    if (-not ('DvsShell.ShellNotify' -as [type])) {
        Add-Type -Namespace 'DvsShell' -Name 'ShellNotify' -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll")]
public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);
'@
    }
    # SHCNE_ASSOCCHANGED: ask the shell to rebuild its association cache so the new verb shows up
    # without signing out.
    [DvsShell.ShellNotify]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Remove-KeyIfEmpty {
    param([string] $Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    if (@(Get-ChildItem -LiteralPath $Path).Count -eq 0 -and
        @(Get-Item -LiteralPath $Path).Property.Count -eq 0) {
        Remove-Item -LiteralPath $Path -Force
    }
}

$verbKeys = @(
    $Extensions | ForEach-Object {
        Join-Path $classesRoot "SystemFileAssociations\$_\shell\$verbKeyName"
    }
)

if ($Uninstall) {
    foreach ($extension in $Extensions) {
        $verbKey = Join-Path $classesRoot "SystemFileAssociations\$extension\shell\$verbKeyName"
        if (-not (Test-Path -LiteralPath $verbKey)) {
            continue
        }
        if ($PSCmdlet.ShouldProcess($verbKey, 'remove the CompareStation verb')) {
            Remove-Item -LiteralPath $verbKey -Recurse -Force
            Remove-KeyIfEmpty (Split-Path -Path $verbKey -Parent)
            Remove-KeyIfEmpty (Split-Path -Path (Split-Path -Path $verbKey -Parent) -Parent)
        }
    }
    if (Test-Path -LiteralPath $clsidKey) {
        if ($PSCmdlet.ShouldProcess($clsidKey, 'unregister the Explorer command server')) {
            Remove-Item -LiteralPath $clsidKey -Recurse -Force
        }
    }
    if ($PSCmdlet.ShouldProcess('shell32.dll', 'notify the shell that associations changed')) {
        Invoke-ShellAssociationChanged
    }
    Write-Output "DVS_EXPLORER_COMMAND_REMOVED extensions=$($Extensions -join ',')"
    return
}

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    throw 'InstallRoot is required unless -Uninstall is used.'
}
if (-not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
    throw "Install root not found: $InstallRoot"
}
$installRootPath = (Resolve-Path -LiteralPath $InstallRoot).Path
$executable = Join-Path $installRootPath 'CompareStation.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "CompareStation.exe not found in the install root: $installRootPath"
}
$shellBinary = Get-ShellBinary -Root $installRootPath -Executable $executable

if ($PSCmdlet.ShouldProcess($clsidKey, 'register the Explorer command server')) {
    Set-RegistryString -Path $clsidKey -Name '(default)' -Value $explorerCommandTitle
    Set-RegistryString -Path $inprocKey -Name '(default)' -Value $shellBinary
    Set-RegistryString -Path $inprocKey -Name 'ThreadingModel' -Value 'Apartment'
}

$expected = [ordered]@{
    'MUIVerb'                = $menuText
    'Icon'                   = "$executable,0"
    'ExplorerCommandHandler' = $explorerCommandClsid
    'MultiSelectModel'       = $selectionModel
}
foreach ($extension in $Extensions) {
    $verbKey = Join-Path $classesRoot "SystemFileAssociations\$extension\shell\$verbKeyName"
    if (-not $PSCmdlet.ShouldProcess($verbKey, "register the CompareStation verb for $extension")) {
        continue
    }
    foreach ($name in $expected.Keys) {
        Set-RegistryString -Path $verbKey -Name $name -Value $expected[$name]
    }
}

if ($PSCmdlet.ShouldProcess('shell32.dll', 'notify the shell that associations changed')) {
    Invoke-ShellAssociationChanged
}

# Read the keys back so a silent failure cannot pass as success. A dry run writes nothing, so the
# read-back only makes sense for a real registration.
if (-not $WhatIfPreference) {
    foreach ($name in $expected.Keys) {
        foreach ($verbKey in $verbKeys) {
            $actual = (Get-ItemProperty -LiteralPath $verbKey -Name $name).$name
            if ($actual -cne $expected[$name]) {
                throw "Registry value '$name' under '$verbKey' is '$actual', expected '$($expected[$name])'."
            }
        }
    }
    $registeredServer = (Get-ItemProperty -LiteralPath $inprocKey).'(default)'
    if ($registeredServer -cne $shellBinary) {
        throw "Registered server is '$registeredServer', expected '$shellBinary'."
    }
}

Write-Output "DVS_EXPLORER_COMMAND_REGISTERED root=$installRootPath extensions=$($Extensions -join ',')"
