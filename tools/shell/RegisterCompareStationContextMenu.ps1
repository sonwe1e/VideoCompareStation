#requires -Version 7.0
<#
.SYNOPSIS
Registers (or removes) the Explorer right-click command from the folder it is unpacked into.

.DESCRIPTION
The double-click entry point for an unpacked release: "Register CompareStation context menu.cmd"
calls this script, which finds the package next to itself, checks that it holds the executable and
the shell library, runs RegisterExplorerCommand.ps1 and then reads the written keys back. No
terminal knowledge is needed - double-clicking the .cmd file is the whole procedure.

The real work still lives in RegisterExplorerCommand.ps1. This script only locates things, so a
package without that script fails with a readable message instead of a silent no-op.

.PARAMETER InstallRoot
Package directory. Defaults to the directory holding this script, which is how the shipped .cmd
invokes it. Pass an explicit path only when the package had to be registered from elsewhere.

.PARAMETER Uninstall
Removes the per-user registration instead of writing it.

.EXAMPLE
pwsh -NoProfile -File .\RegisterCompareStationContextMenu.ps1

.EXAMPLE
pwsh -NoProfile -File .\RegisterCompareStationContextMenu.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string] $InstallRoot,

    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-RegistrationScript {
    param([string] $Root)

    $candidates = @(
        (Join-Path $Root 'RegisterExplorerCommand.ps1'),
        (Join-Path $PSScriptRoot 'RegisterExplorerCommand.ps1'),
        (Join-Path $PSScriptRoot '..\..\tools\shell\RegisterExplorerCommand.ps1')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw (
        "在本文件所在目录（'$PSScriptRoot'）和包目录里都没找到 RegisterExplorerCommand.ps1。" +
        '请解压完整的发布 ZIP，并从解压出来的目录里运行本文件。'
    )
}

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = $PSScriptRoot
}
if (-not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
    throw "找不到包目录：$InstallRoot"
}
$root = (Resolve-Path -LiteralPath $InstallRoot).Path
$registrationScript = Get-RegistrationScript -Root $root

if ($Uninstall) {
    & $registrationScript -Uninstall
    Write-Host ''
    Write-Host '完成：右键菜单里的 CompareStation 已移除。' -ForegroundColor Green
    return
}

$executable = Join-Path $root 'CompareStation.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw (
        "在 '$root' 里找不到 CompareStation.exe。请把本文件放回解压出来的发布目录再运行，" +
        '或者显式指定目录：-InstallRoot <目录>。'
    )
}

& $registrationScript -InstallRoot $root

Write-Host ''
Write-Host '完成：当前 Windows 账户的右键菜单里已经有 CompareStation 了。' -ForegroundColor Green
Write-Host ''
Write-Host '怎么用：'
Write-Host '  - 选中一个文件：出现这一项，打开该文件。'
Write-Host '  - 选中两个视频或两张图片：出现 “Compare with CompareStation”，直接进对比。'
Write-Host '  - 选中三个视频：也会出现这一项。'
Write-Host '  - 以下情况这一项被故意隐藏，不是出错：目录、网络路径、图片与视频混选、'
Write-Host '    三张及以上图片、超过三个文件。'
Write-Host '  - Windows 11 默认把这些命令收在“显示更多选项”里（也可以按 Shift+F10）。'
Write-Host ''
Write-Host '要取消注册：双击那个 .cmd 是没法带参数的，请在解压目录里打开一个终端执行'
Write-Host '下面这一行（或者直接删掉解压目录，右键项也会随之失效）：'
Write-Host "  pwsh -NoProfile -File `"$registrationScript`" -Uninstall"
