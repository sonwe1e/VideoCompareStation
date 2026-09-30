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

# The application keeps the entry registered by itself, so removing it has to record that decision
# somewhere the next launch reads. Without this value an explicit -Uninstall would be undone the
# first time the user opened the program again. The name and location match
# src/shell_windows/ExplorerCommandRegistration.h.
$settingsKey = 'HKCU:\Software\CompareStation'
$enabledValueName = 'ExplorerContextMenu'

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

function Set-ExplorerCommandEnabled {
    param([bool] $Enabled)

    if (-not (Test-Path -LiteralPath $settingsKey)) {
        New-Item -Path $settingsKey -Force | Out-Null
    }
    $value = if ($Enabled) { 1 } else { 0 }
    New-ItemProperty -LiteralPath $settingsKey -Name $enabledValueName -PropertyType DWord `
        -Value $value -Force | Out-Null
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
    # Get-ItemProperty rather than (Get-Item).Property: a registry key that holds nothing but its
    # unnamed default value has no Property member, and under Set-StrictMode -Version Latest the
    # member access throws instead of returning nothing - which aborted an -Uninstall run after the
    # first extension and left every other verb key behind.
    if (@(Get-ChildItem -LiteralPath $Path).Count -eq 0 -and
        @(Get-ItemProperty -LiteralPath $Path).Count -eq 0) {
        Remove-Item -LiteralPath $Path -Force
    }
}

$verbKeys = @(
    $Extensions | ForEach-Object {
        Join-Path $classesRoot "SystemFileAssociations\$_\shell\$verbKeyName"
    }
)

function Get-RegistryString {
    param([string] $Path, [string] $Name)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return $null
    }
    $properties = Get-ItemProperty -LiteralPath $Path -ErrorAction SilentlyContinue
    if ($null -eq $properties) {
        return $null
    }
    # Reading a value by member access throws under Set-StrictMode -Version Latest when the value is
    # not there, which would abort the whole sweep on the first key that has no such value. The
    # property collection is the lookup that answers "absent" instead.
    $property = $properties.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [string]) {
        return $null
    }
    return $property.Value
}

# Every verb key that binds this command, wherever it sits in the per-user classes root. A file's
# right-click menu is built from every verb in its association chain, and the per-extension key
# `<ext>\shell` is part of that chain just like SystemFileAssociations, so a copy left there is a
# second, identical entry in the menu. Two shapes have pointed at this application: the COM handler
# below and a plain command line from an earlier layout. Both are recognised by what they point at,
# never by the key's name, so a neighbour registered by another tool is never touched.
function Get-CompareStationVerbKey {
    param([string] $ShellKeyPath, [bool] $KeepCurrent)

    if (-not (Test-Path -LiteralPath $ShellKeyPath -PathType Container)) {
        return
    }
    foreach ($child in Get-ChildItem -LiteralPath $ShellKeyPath) {
        # The same verb name under a different association is a duplicate, not our current entry.
        if ($KeepCurrent -and $verbKeys -contains $child.Name.Replace('HKEY_CURRENT_USER', 'HKCU:')) {
            continue
        }
        $handler = Get-RegistryString -Path $child.PSPath -Name 'ExplorerCommandHandler'
        if ($null -ne $handler -and $handler.Trim() -eq $explorerCommandClsid) {
            $child.PSPath
            continue
        }
        $line = Get-RegistryString -Path (Join-Path $child.PSPath 'command') -Name '(default)'
        if ($null -ne $line -and $line -match '^\s*(?:"(?<exe>[^"]+)"|(?<exe>[^\s]+))') {
            # Match the executable, not an argument mentioning our name or a similarly named tool.
            $leaf = [System.IO.Path]::GetFileName($Matches.exe).TrimEnd(' ', '.')
            if ($leaf -in @('CompareStation.exe', 'VCStation.exe')) {
                $child.PSPath
            }
        }
    }
}

function Remove-StaleCompareStationVerbs {
    param([bool] $KeepCurrent)

    $removed = 0
    # Include extension keys, versioned ProgIDs and generic file associations. Do not recursively
    # sweep the registry: only verb locations in the file-association chain, matched by target.
    foreach ($parent in @($classesRoot, (Join-Path $classesRoot 'SystemFileAssociations'))) {
        if (-not (Test-Path -LiteralPath $parent)) {
            continue
        }
        foreach ($association in @(Get-ChildItem -LiteralPath $parent)) {
            $shellKeyPath = Join-Path $association.PSPath 'shell'
            foreach ($stale in @(Get-CompareStationVerbKey -ShellKeyPath $shellKeyPath -KeepCurrent $KeepCurrent)) {
                if ($PSCmdlet.ShouldProcess($stale, 'remove a second copy of the CompareStation verb')) {
                    Remove-Item -LiteralPath $stale -Recurse -Force
                    $removed++
                    Remove-KeyIfEmpty $shellKeyPath
                    Remove-KeyIfEmpty $association.PSPath
                }
            }
        }
    }
    return $removed
}

if ($Uninstall) {
    # Record the decision before removing anything: if this run is interrupted halfway, the next
    # launch has to stay out of the user's way instead of adding the keys back.
    if ($PSCmdlet.ShouldProcess($settingsKey, 'record that the Explorer command stays off')) {
        Set-ExplorerCommandEnabled -Enabled $false
    }
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
    # A copy of the command this run does not own is still this command: leaving it behind would
    # only bring the entry back the next time anybody registered it.
    $staleRemoved = Remove-StaleCompareStationVerbs -KeepCurrent $false
    if (Test-Path -LiteralPath $clsidKey) {
        if ($PSCmdlet.ShouldProcess($clsidKey, 'unregister the Explorer command server')) {
            Remove-Item -LiteralPath $clsidKey -Recurse -Force
        }
    }
    if ($PSCmdlet.ShouldProcess('shell32.dll', 'notify the shell that associations changed')) {
        Invoke-ShellAssociationChanged
    }
    Write-Output "DVS_EXPLORER_COMMAND_REMOVED extensions=$($Extensions -join ',') stale_verbs=$staleRemoved"
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
if ($PSCmdlet.ShouldProcess($settingsKey, 'record that the Explorer command is wanted')) {
    Set-ExplorerCommandEnabled -Enabled $true
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
    $legacyCommand = Join-Path $verbKey 'command'
    if (Test-Path -LiteralPath $legacyCommand) {
        Remove-Item -LiteralPath $legacyCommand -Recurse -Force
    }
}

# The entry the user is meant to see is the one this script owns. A second copy elsewhere in the
# per-user classes root would show up as a second identical menu entry, so registering also removes
# it - exactly what the application does on its own next launch.
$staleRemoved = Remove-StaleCompareStationVerbs -KeepCurrent $true

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
    $enabled = (Get-ItemProperty -LiteralPath $settingsKey -Name $enabledValueName).$enabledValueName
    if ($enabled -ne 1) {
        throw "Registry value '$enabledValueName' under '$settingsKey' is '$enabled', expected 1."
    }
}

# Invalidate Explorer's cache only after both registration and legacy cleanup have completed.
if ($PSCmdlet.ShouldProcess('shell32.dll', 'notify the shell that associations changed')) {
    Invoke-ShellAssociationChanged
}

Write-Output "DVS_EXPLORER_COMMAND_REGISTERED root=$installRootPath extensions=$($Extensions -join ',') stale_verbs=$staleRemoved"
