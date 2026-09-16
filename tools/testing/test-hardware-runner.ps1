param(
    [ValidateRange(1, 240)]
    [int]$MinimumRefreshRate = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not [Environment]::UserInteractive) {
    throw 'The hardware runner must execute in an interactive Windows session.'
}

$sessionId = (Get-Process -Id $PID).SessionId
if ($sessionId -eq 0) {
    throw 'The hardware runner is in Session 0. Start run.cmd after an interactive user login.'
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class DvsHardwareDisplayProbe
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DisplayDevice
    {
        public int cb;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string DeviceName;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceString;

        public int StateFlags;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceId;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceKey;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool EnumDisplayDevices(
        string device,
        int deviceNumber,
        ref DisplayDevice displayDevice,
        int flags);

    public static DisplayDevice[] All()
    {
        var result = new List<DisplayDevice>();
        for (var index = 0; ; ++index)
        {
            var device = new DisplayDevice();
            device.cb = Marshal.SizeOf<DisplayDevice>();
            if (!EnumDisplayDevices(null, index, ref device, 0))
            {
                break;
            }
            result.Add(device);
        }
        return result.ToArray();
    }
}
'@

$attachedFlag = 0x1
$attached = @(
    [DvsHardwareDisplayProbe]::All() |
        Where-Object { ($_.StateFlags -band $attachedFlag) -ne 0 }
)
if ($attached.Count -eq 0) {
    throw 'No display adapter is attached to the interactive desktop.'
}

$attached |
    Select-Object DeviceName, DeviceString, DeviceId, StateFlags |
    Format-Table -AutoSize

$virtualPattern = 'virtual|indirect|\bidd\b|remote|oray|gameviewer|basic display'
$physical = @(
    $attached |
        Where-Object {
            $_.DeviceId -match '^PCI\\' -and
            $_.DeviceString -notmatch $virtualPattern
        }
)
if ($physical.Count -eq 0) {
    $attachedNames = ($attached | ForEach-Object { "$($_.DeviceName) $($_.DeviceString)" }) -join '; '
    throw (
        'No physical PCI display adapter is attached to the interactive desktop. ' +
        "Attached adapters: $attachedNames"
    )
}

$controllers = @(Get-CimInstance Win32_VideoController)
$refreshRates = foreach ($adapter in $physical) {
    $controllers |
        Where-Object {
            $_.Status -eq 'OK' -and
            $_.Name -eq $adapter.DeviceString -and
            $_.CurrentRefreshRate
        } |
        ForEach-Object { [int]$_.CurrentRefreshRate }
}
$maximumRefreshRate = ($refreshRates | Measure-Object -Maximum).Maximum
if (-not $maximumRefreshRate -or $maximumRefreshRate -lt $MinimumRefreshRate) {
    throw (
        "The attached physical display must run at least $MinimumRefreshRate Hz; " +
        "detected maximum: $maximumRefreshRate Hz."
    )
}

$physical |
    Select-Object DeviceName, DeviceString, DeviceId, StateFlags |
    Format-Table -AutoSize
Write-Output (
    "DVS_HARDWARE_RUNNER_READY session=$sessionId " +
    "physical_adapters=$($physical.Count) refresh_hz=$maximumRefreshRate"
)
