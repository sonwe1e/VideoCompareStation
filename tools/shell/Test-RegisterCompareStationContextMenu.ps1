#requires -Version 7.0
<#
.SYNOPSIS
Independent verification of the double-click Explorer registration entry point.

.DESCRIPTION
Exercises RegisterCompareStationContextMenu.cmd and .ps1 against a throwaway copy of a release
directory, running the wrapper exactly the way a user or a documented command line would, and
asserts only what can be observed from outside: registry keys, exit codes and console text.

Covered:
  1. the .cmd file holds no non-ASCII bytes and uses CRLF, so cmd.exe cannot mangle it;
  2. both scripts parse;
  3. DVS_PWSH_OVERRIDE fails closed when it points at no file;
  4. a package without the registration script explains itself and exits non-zero;
  5. a package without CompareStation.exe is refused with a readable message;
  6. a healthy package registers the CLSID and all 18 extension verbs, each pointing at that
     package - read back from HKCU, never from the script's own output - and registering twice
     changes nothing;
  7. "RegisterCompareStationContextMenu.cmd -Uninstall", the command INSTALL.txt documents,
     removes the CLSID and every verb key again, running it a second time still exits 0, and the
     decision to stay off is recorded so the application does not add the entry back;
  8. a second copy of this command left under the per-extension <ext>\shell key - the form a user's
     menu shows as a second, identical entry - is removed by registering and by uninstalling, while
     another tool's verb on the same extension and the entry this registration owns are not.

It only writes the repository's own per-user keys (HKCU\Software\Classes\CLSID\{3B790D74-...} and
SystemFileAssociations\<ext>\shell\CompareStation.Compare), the real per-user marker at
HKCU\Software\CompareStation\ExplorerContextMenu, and removes them again in the finally block.
Scratch files live under out\verification\zip-registration-<pid>\.

.EXAMPLE
pwsh -NoProfile -File tools/shell/Test-RegisterCompareStationContextMenu.ps1 -PackageRoot out\build\release\bin
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $PackageRoot,

    # Lets one-off mutation drivers exercise this same suite without replacing repository sources.
    [string] $RegistrationScriptPath = (Join-Path $PSScriptRoot 'RegisterExplorerCommand.ps1'),

    # Process-unique on purpose: two runs of this test used to share one scratch directory, and the
    # second run deleted the first run's sandbox mid-test, turning a healthy package into a bogus
    # "sandbox package was deleted" failure.
    [string] $WorkRoot = (Join-Path $PSScriptRoot ('..\..\out\verification\zip-registration-' + $PID))
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$clsid = '{3B790D74-E76E-4F28-A51D-2AB8C6BD107D}'
$verbKeyName = 'CompareStation.Compare'
$mediaExtensions = @(
    '.mp4', '.mkv', '.mov', '.avi', '.m4v',
    '.png', '.jpg', '.jpeg', '.bmp', '.gif', '.webp', '.tif', '.tiff',
    '.pnm', '.ppm', '.pgm', '.pbm', '.pam'
)
$expectedVerbValues = [ordered]@{
    MUIVerb                = 'Compare with CompareStation'
    ExplorerCommandHandler = $clsid
    MultiSelectModel       = 'Player'
}

$checkCount = 0
$failureCount = 0
$workPath = [System.IO.Path]::GetFullPath($WorkRoot)
$toolsRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$wrapper = Join-Path $toolsRoot 'RegisterCompareStationContextMenu.cmd'
$wrapperScript = Join-Path $toolsRoot 'RegisterCompareStationContextMenu.ps1'
$registrationScript = (Resolve-Path -LiteralPath $RegistrationScriptPath).Path
$disabledScriptName = 'RegisterCompareStationContextMenu.ps1.disabled'

# The application keeps the entry registered by itself, so the registration script records the
# user's decision in HKCU\Software\CompareStation\ExplorerContextMenu. This suite writes that real
# value, so it has to put back whatever it found instead of leaving the machine opted out.
$settingsKey = 'HKCU:\Software\CompareStation'
$enabledValueName = 'ExplorerContextMenu'

function Assert-That {
    param([string] $Name, [bool] $Condition, [string] $Detail = '')

    $script:checkCount++
    if ($Condition) {
        Write-Host "PASS  $Name"
        return
    }
    $script:failureCount++
    $suffix = if ([string]::IsNullOrEmpty($Detail)) { '' } else { " - $Detail" }
    Write-Host "FAIL  $Name$suffix" -ForegroundColor Red
}

# Runs a .cmd file the way Explorer or a documented command line would: cmd.exe executes it with
# stdin redirected, and call makes cmd.exe report the script's exit code to us. "call" is required
# because a bare .ps1 line is not executable from cmd.exe - .PS1 is not in PATHEXT.
function Invoke-CommandFile {
    param([string] $CommandFile, [hashtable] $ExtraEnvironment = @{}, [string[]] $Arguments = @())

    $lines = @('@echo off', 'setlocal')
    foreach ($name in $ExtraEnvironment.Keys) {
        $lines += "set `"$name=$($ExtraEnvironment[$name])`""
    }
    $argumentText = if ($Arguments.Count -eq 0) { '' } else { ' ' + ($Arguments -join ' ') }
    $lines += "call `"$CommandFile`"$argumentText < nul"
    $lines += 'echo DVS_EXIT_CODE=%ERRORLEVEL%'
    $runner = Join-Path $workPath 'invoke-command-file.cmd'
    Set-Content -LiteralPath $runner -Value $lines -Encoding ascii
    $output = & cmd.exe /c $runner 2>&1
    return (($output | ForEach-Object { "$_" }) -join "`n")
}

function Get-ExitCode {
    param([string] $Output)

    if ($Output -match 'DVS_EXIT_CODE=(-?\d+)') {
        return [int] $Matches[1]
    }
    return [int]::MinValue
}

# Neither subkeys nor values, because a key that only carries values is not "empty" to the registry
# and deleting it would take another tool's data with it.
function Test-KeyUnused {
    param([string] $Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return $false
    }
    $values = @((Get-Item -LiteralPath $Path).GetValueNames())
    $subkeys = @(Get-ChildItem -LiteralPath $Path)
    return ($values.Count -eq 0 -and $subkeys.Count -eq 0)
}

function Remove-KeyIfUnused {
    param([string] $Path)

    if (Test-KeyUnused -Path $Path) {
        Remove-Item -LiteralPath $Path -Force
    }
}

function Remove-Registration {
    foreach ($extension in $mediaExtensions) {
        $verbKey = "HKCU:\Software\Classes\SystemFileAssociations\$extension\shell\$verbKeyName"
        if (Test-Path -LiteralPath $verbKey) {
            Remove-Item -LiteralPath $verbKey -Recurse -Force
        }
        # An interrupted run must not leave the shared parents behind either.
        Remove-KeyIfUnused "HKCU:\Software\Classes\SystemFileAssociations\$extension\shell"
        Remove-KeyIfUnused "HKCU:\Software\Classes\SystemFileAssociations\$extension"
        # The per-extension parent is part of the same association chain, so a copy left there is
        # just as much a second menu entry as one under SystemFileAssociations.
        $perExtensionVerb = "HKCU:\Software\Classes\$extension\shell"
        foreach ($name in @('CompareStation.Open', 'CompareStation.Legacy', 'SomeOtherPlayer',
                'CompareStation.Compare', 'OldVCStation', 'TestOtherPlayer', 'TestSimilarName')) {
            $testVerb = Join-Path $perExtensionVerb $name
            if (Test-Path -LiteralPath $testVerb) {
                Remove-Item -LiteralPath $testVerb -Recurse -Force
            }
        }
        Remove-KeyIfUnused "HKCU:\Software\Classes\$extension\shell"
        Remove-KeyIfUnused "HKCU:\Software\Classes\$extension"
    }
    foreach ($relative in @(
            'CompareStationTest.Legacy.2.0.1\shell\CompareStation.Compare',
            'SystemFileAssociations\.m4v\shell\CompareStation.Compare-2.0.1')) {
        $testVerb = Join-Path 'HKCU:\Software\Classes' $relative
        if (Test-Path -LiteralPath $testVerb) {
            Remove-Item -LiteralPath $testVerb -Recurse -Force
            Remove-KeyIfUnused (Split-Path -Path $testVerb -Parent)
            Remove-KeyIfUnused (Split-Path -Path (Split-Path -Path $testVerb -Parent) -Parent)
        }
    }
    $clsidKey = "HKCU:\Software\Classes\CLSID\$clsid"
    if (Test-Path -LiteralPath $clsidKey) {
        Remove-Item -LiteralPath $clsidKey -Recurse -Force
    }
}

# Plants a verb under the per-extension key that binds this command, which is what an earlier
# layout or a hand edit leaves behind and what the user sees as a second, identical menu entry.
# The shape is a switch and not a nullable string on purpose: a [string] parameter turns $null into
# an empty string, which planted the command shape with an empty command line - a key no sweep would
# ever match, and a check that would then pass for the wrong reason.
function Set-SecondCopyVerb {
    param(
        [string] $Extension,
        [string] $Name,
        [switch] $AsCommandLine,
        [string] $CommandText = ''
    )

    $verbKey = "HKCU:\Software\Classes\$Extension\shell\$Name"
    if ($AsCommandLine) {
        $commandKey = Join-Path $verbKey 'command'
        New-Item -Path $commandKey -Force | Out-Null
        New-ItemProperty -LiteralPath $commandKey -Name '(default)' -Value $CommandText `
            -PropertyType String -Force | Out-Null
    }
    else {
        New-Item -Path $verbKey -Force | Out-Null
        New-ItemProperty -LiteralPath $verbKey -Name 'ExplorerCommandHandler' -Value $clsid `
            -PropertyType String -Force | Out-Null
        New-ItemProperty -LiteralPath $verbKey -Name 'MUIVerb' `
            -Value $expectedVerbValues['MUIVerb'] -PropertyType String -Force | Out-Null
    }
}

function Get-ExplorerCommandMarker {
    if (-not (Test-Path -LiteralPath $settingsKey)) {
        return $null
    }
    $found = (Get-ItemProperty -LiteralPath $settingsKey).PSObject.Properties[$enabledValueName]
    if ($null -eq $found) {
        return $null
    }
    return [int] $found.Value
}

function Set-ExplorerCommandMarker {
    param([object] $State)

    if ($null -eq $State) {
        if (Test-Path -LiteralPath $settingsKey) {
            Remove-ItemProperty -LiteralPath $settingsKey -Name $enabledValueName `
                -ErrorAction SilentlyContinue
            Remove-KeyIfUnused $settingsKey
        }
        return
    }
    if (-not (Test-Path -LiteralPath $settingsKey)) {
        New-Item -Path $settingsKey -Force | Out-Null
    }
    New-ItemProperty -LiteralPath $settingsKey -Name $enabledValueName -PropertyType DWord `
        -Value $State -Force | Out-Null
}

function Get-RegisteredVerbValue {
    param([string] $Extension, [string] $Property)

    $verbKey = "HKCU:\Software\Classes\SystemFileAssociations\$Extension\shell\$verbKeyName"
    if (-not (Test-Path -LiteralPath $verbKey)) {
        return $null
    }
    if ($Property -ceq '(default)') {
        # A registry key holding only an unnamed value exposes no '(default)' property, so the
        # default value has to come from the .NET key object itself.
        return (Get-Item -LiteralPath $verbKey).GetValue('')
    }
    $found = (Get-ItemProperty -LiteralPath $verbKey).PSObject.Properties[$Property]
    if ($null -eq $found) {
        return $null
    }
    return $found.Value
}

function Copy-Package {
    param([string] $Destination)

    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Copy-Item -Path (Join-Path $script:packagePath '*') -Destination $Destination -Recurse -Force
    foreach ($name in @(
            'RegisterCompareStationContextMenu.ps1',
            'RegisterCompareStationContextMenu.cmd'
        )) {
        Copy-Item -LiteralPath (Join-Path $script:toolsRoot $name) -Destination $Destination -Force
    }
    Copy-Item -LiteralPath $script:registrationScript `
        -Destination (Join-Path $Destination 'RegisterExplorerCommand.ps1') -Force
    Copy-Item -LiteralPath (Join-Path $script:toolsRoot '..\..\packaging\INSTALL.txt') `
        -Destination $Destination -Force
}

$packagePath = (Resolve-Path -LiteralPath $PackageRoot).Path
if (Test-Path -LiteralPath $workPath) {
    Remove-Item -LiteralPath $workPath -Recurse -Force
}
New-Item -ItemType Directory -Path $workPath -Force | Out-Null

# Every invocation below starts its own cmd.exe and inherits this process environment, so a stray
# DVS_PWSH_OVERRIDE outside the harness would replace the interpreter under test. Remove it here and
# let the checks that care about the override set it explicitly.
Remove-Item Env:DVS_PWSH_OVERRIDE -ErrorAction SilentlyContinue

# The marker is the user's own decision, so record it before the first write and put it back at the
# end: a suite that left 0 behind would silently disable the entry for whoever ran it.
$markerBefore = Get-ExplorerCommandMarker

try {
    # 1. File-level contract of the double-click entry point.
    $wrapperBytes = [System.IO.File]::ReadAllBytes($wrapper)
    $nonAscii = @($wrapperBytes | Where-Object { $_ -gt 127 }).Count
    Assert-That 'wrapper .cmd holds no non-ASCII bytes' ($nonAscii -eq 0) "found $nonAscii"

    $bareLf = 0
    for ($index = 0; $index -lt $wrapperBytes.Length; $index++) {
        if ($wrapperBytes[$index] -eq 10 -and ($index -eq 0 -or $wrapperBytes[$index - 1] -ne 13)) {
            $bareLf++
        }
    }
    Assert-That 'wrapper .cmd uses CRLF line endings' ($bareLf -eq 0) "bare LF count $bareLf"

    # 2. Both scripts parse, so a syntax error cannot hide behind "it worked on my machine once".
    foreach ($scriptPath in @($wrapperScript, $registrationScript)) {
        $parseErrors = $null
        $tokens = $null
        [System.Management.Automation.Language.Parser]::ParseFile(
            $scriptPath, [ref] $tokens, [ref] $parseErrors) | Out-Null
        $errorText = (@($parseErrors) | ForEach-Object { $_.ToString() }) -join '; '
        Assert-That "$(Split-Path -Leaf $scriptPath) parses" (@($parseErrors).Count -eq 0) $errorText
    }

    # 2b. The uninstall command exercised in section 7 has to be the one INSTALL.txt tells users to
    #     run. The two drifted apart once already: the documented line went through a wrapper that
    #     dropped its arguments, so it registered the command again instead of removing it.
    $installNotes = Get-Content -LiteralPath (Join-Path $toolsRoot '..\..\packaging\INSTALL.txt') -Raw
    Assert-That 'INSTALL.txt documents the uninstall line this test runs' `
        ($installNotes -match [regex]::Escape('.\RegisterCompareStationContextMenu.cmd -Uninstall'))

    # 3. A bad interpreter override fails closed instead of silently using another pwsh.
    $overrideOutput = Invoke-CommandFile -CommandFile $wrapper -ExtraEnvironment @{
        DVS_PWSH_OVERRIDE = (Join-Path $workPath 'no-such-pwsh.exe')
    }
    Assert-That 'a bad DVS_PWSH_OVERRIDE is refused' `
        ($overrideOutput -match 'DVS_PWSH_OVERRIDE is set but points at no file') $overrideOutput
    Assert-That 'a bad DVS_PWSH_OVERRIDE exits non-zero' ((Get-ExitCode $overrideOutput) -eq 1) `
        "exit code $(Get-ExitCode $overrideOutput)"

    # 4. The double-click experience in a console with stdin left alone. Every check that goes
    #    through Invoke-CommandFile redirects stdin, which turns pause into a no-op, so a wrapper
    #    that never pauses at all still passes all of them - this probe is the only one that can see
    #    the window closing.
    #
    #    A control file wraps the delivery wrapper and writes markers: it appends RETURNED only after
    #    the wrapper returns, which can happen only once the pause has been satisfied. A redirected
    #    run proves the marker really gets written, so "still waiting" cannot be a broken probe, and
    #    the registration has to be visible in the registry at the same time, so an early failure
    #    branch cannot pass itself off as a successful pause. The window is longer than a real
    #    registration, measured at about 3.4 s, so a slow run is not mistaken for a pause.
    $probeRoot = Join-Path $workPath 'pause probe package'
    Copy-Package -Destination $probeRoot
    $probeWrapper = Join-Path $probeRoot 'RegisterCompareStationContextMenu.cmd'
    $probeExecutable = Join-Path $probeRoot 'CompareStation.exe'
    $probeServer = Join-Path $probeRoot (
        'CompareStationShell-{0}.dll' -f (
            (Get-Item -LiteralPath $probeExecutable).VersionInfo.FileVersion `
                -replace '^(\d+\.\d+).*$', '$1'))
    $probeControl = Join-Path $workPath 'pause-probe.cmd'
    $probeMarkers = Join-Path $workPath 'pause-probe-markers.txt'
    Set-Content -LiteralPath $probeControl -Encoding ascii -Value @(
        '@echo off',
        "del `"$probeMarkers`" 2>nul",
        "call `"$probeWrapper`"",
        ">>`"$probeMarkers`" echo RETURNED=%ERRORLEVEL%"
    )

    $redirectedProbe = Start-Process -FilePath 'cmd.exe' `
        -ArgumentList '/c', "`"$probeControl`" < nul" -WindowStyle Hidden -PassThru
    $redirectedProbe.WaitForExit()
    $markersWhenRedirected = if (Test-Path -LiteralPath $probeMarkers) {
        Get-Content -LiteralPath $probeMarkers -Raw
    }
    else {
        ''
    }
    Assert-That 'a redirected probe run records its marker' `
        ($markersWhenRedirected -match 'RETURNED=0') "markers '$($markersWhenRedirected.Trim())'"

    $interactiveProbe = Start-Process -FilePath 'cmd.exe' `
        -ArgumentList '/c', "`"$probeControl`"" -WindowStyle Hidden -PassThru
    $interactiveProbeExited = $false
    $interactiveProbeDeadline = (Get-Date).AddSeconds(20)
    while (-not $interactiveProbeExited -and (Get-Date) -lt $interactiveProbeDeadline) {
        Start-Sleep -Milliseconds 200
        $interactiveProbeExited = $interactiveProbe.HasExited
    }
    $markersWhenInteractive = if (Test-Path -LiteralPath $probeMarkers) {
        Get-Content -LiteralPath $probeMarkers -Raw
    }
    else {
        ''
    }
    $serverDuringProbe = `
        (Get-ItemProperty -LiteralPath "HKCU:\Software\Classes\CLSID\$clsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
    if (-not $interactiveProbeExited) {
        $interactiveProbe.Kill()
        $interactiveProbe.WaitForExit()
    }
    Assert-That 'the paused probe really registered its own package' `
        ($serverDuringProbe -ceq $probeServer) `
        "registered server '$serverDuringProbe', expected '$probeServer'"
    Assert-That 'the success path stops on the pause and does not return on its own' `
        ((-not $interactiveProbeExited) -and $markersWhenInteractive -notmatch 'RETURNED=') `
        "exited=$interactiveProbeExited markers '$($markersWhenInteractive.Trim())'"

    # 5. Missing registration script: the wrapper has to locate the helper and say so. Both the
    #    package folder and the repository fallback are hidden for the duration of this check.
    $orphanRoot = Join-Path $workPath 'orphan package'
    New-Item -ItemType Directory -Path $orphanRoot -Force | Out-Null
    Copy-Item -LiteralPath $wrapper -Destination $orphanRoot -Force
    $disabledScript = Join-Path $workPath $disabledScriptName
    Move-Item -LiteralPath $wrapperScript -Destination $disabledScript -Force
    try {
        $orphanOutput = Invoke-CommandFile -CommandFile (Join-Path $orphanRoot 'RegisterCompareStationContextMenu.cmd')
    }
    finally {
        Move-Item -LiteralPath $disabledScript -Destination $wrapperScript -Force
    }
    Assert-That 'missing helper script is reported' `
        ($orphanOutput -match 'RegisterCompareStationContextMenu\.ps1 was not found') $orphanOutput
    Assert-That 'missing helper script exits non-zero' ((Get-ExitCode $orphanOutput) -eq 1) `
        "exit code $(Get-ExitCode $orphanOutput)"
    Assert-That 'the helper script is restored' (Test-Path -LiteralPath $wrapperScript)

    # 6. A package without the executable is refused with a readable message.
    $brokenRoot = Join-Path $workPath 'broken package'
    New-Item -ItemType Directory -Path $brokenRoot -Force | Out-Null
    Copy-Item -LiteralPath $wrapper -Destination $brokenRoot -Force
    Copy-Item -LiteralPath $wrapperScript -Destination $brokenRoot -Force
    Copy-Item -LiteralPath $registrationScript `
        -Destination (Join-Path $brokenRoot 'RegisterExplorerCommand.ps1') -Force
    $brokenOutput = Invoke-CommandFile -CommandFile (Join-Path $brokenRoot 'RegisterCompareStationContextMenu.cmd')
    Assert-That 'a package without CompareStation.exe is refused' `
        ($brokenOutput -match 'CompareStation\.exe') $brokenOutput
    Assert-That 'a package without CompareStation.exe exits non-zero' `
        ((Get-ExitCode $brokenOutput) -eq 1) "exit code $(Get-ExitCode $brokenOutput)"

    # 7. Healthy package: register, then read every key back from HKCU. The sandbox folder name
    #    carries a space on purpose, because the shipped package folder does too.
    $sandboxRoot = Join-Path $workPath 'CompareStation 2.0.0-windows-x64'
    Copy-Package -Destination $sandboxRoot
    $sandboxWrapper = Join-Path $sandboxRoot 'RegisterCompareStationContextMenu.cmd'
    $sandboxExecutable = Join-Path $sandboxRoot 'CompareStation.exe'
    Assert-That 'sandbox package holds CompareStation.exe' (Test-Path -LiteralPath $sandboxExecutable)
    Assert-That 'sandbox package holds the shell library' `
        (@(Get-ChildItem -LiteralPath $sandboxRoot -Filter 'CompareStationShell-*.dll').Count -ge 1)

    $firstRun = Invoke-CommandFile -CommandFile $sandboxWrapper
    Assert-That 'registration succeeds and says so' `
        ($firstRun -match 'DVS_EXPLORER_COMMAND_REGISTERED') $firstRun
    Assert-That 'registration exits 0' ((Get-ExitCode $firstRun) -eq 0) "exit code $(Get-ExitCode $firstRun)"
    Assert-That 'registration records that the command is wanted' ((Get-ExplorerCommandMarker) -eq 1) `
        "marker is '$(Get-ExplorerCommandMarker)'"

    $productVersion = (Get-Item -LiteralPath $sandboxExecutable).VersionInfo.FileVersion
    $expectedServerName = 'CompareStationShell-{0}.dll' -f ($productVersion -replace '^(\d+\.\d+).*$', '$1')
    $expectedServer = Join-Path $sandboxRoot $expectedServerName
    $registeredServer = `
        (Get-ItemProperty -LiteralPath "HKCU:\Software\Classes\CLSID\$clsid\InprocServer32").'(default)'
    Assert-That 'registered server is the sandbox shell library of the product version' `
        ($registeredServer -ceq $expectedServer) "registered '$registeredServer', expected '$expectedServer'"
    Assert-That 'registered server exists on disk' (Test-Path -LiteralPath $registeredServer)

    $expectedIcon = "$sandboxExecutable,0"
    # A registry key only exposes a '(default)' property when it actually carries an unnamed value,
    # so the verb key has to report exactly the four named values: a stray default value there would
    # let the menu text be overridden by something nobody reviews. The assertions below therefore
    # compare the value list, not PSObject - a registry key reached through Get-Item shows up as a
    # .NET key object with a dozen unrelated members instead of as its values.
    $expectedPropertyNames = @($expectedVerbValues.Keys) + @('Icon')
    $drivePropertyNames = @('PSPath', 'PSParentPath', 'PSChildName', 'PSDrive', 'PSProvider')
    foreach ($extension in $mediaExtensions) {
        $verbKey = "HKCU:\Software\Classes\SystemFileAssociations\$extension\shell\$verbKeyName"
        $icon = Get-RegisteredVerbValue -Extension $extension -Property 'Icon'
        $actualPropertyNames = @(
            (Get-ItemProperty -LiteralPath $verbKey).PSObject.Properties.Name |
                Where-Object { $drivePropertyNames -cnotcontains $_ }
        )
        $propertiesMatch = ($actualPropertyNames.Count -eq $expectedPropertyNames.Count)
        foreach ($name in $expectedPropertyNames) {
            if ($actualPropertyNames -cnotcontains $name) {
                $propertiesMatch = $false
            }
        }
        $valuesMatch = ($icon -ceq $expectedIcon) -and $propertiesMatch
        foreach ($name in $expectedVerbValues.Keys) {
            $actual = Get-RegisteredVerbValue -Extension $extension -Property $name
            if ($actual -cne $expectedVerbValues[$name]) {
                $valuesMatch = $false
            }
        }
        Assert-That "verb '$verbKeyName' written for $extension" $valuesMatch `
            "values=[$($actualPropertyNames -join ',')]"
    }

    $secondRun = Invoke-CommandFile -CommandFile $sandboxWrapper
    Assert-That 'registering twice stays successful' ((Get-ExitCode $secondRun) -eq 0) `
        "exit code $(Get-ExitCode $secondRun)"
    $registeredServerAfterSecondRun = `
        (Get-ItemProperty -LiteralPath "HKCU:\Software\Classes\CLSID\$clsid\InprocServer32").'(default)'
    Assert-That 'registering twice keeps the same server' `
        ($registeredServerAfterSecondRun -ceq $expectedServer) $registeredServerAfterSecondRun

    # 8. Cleanup path. "RegisterCompareStationContextMenu.cmd -Uninstall" is the command INSTALL.txt
    #    documents, and before the wrapper forwarded its arguments it re-registered instead.
    $uninstallRun = Invoke-CommandFile -CommandFile $sandboxWrapper -Arguments @('-Uninstall')
    Assert-That 'uninstall says it removed the command' `
        ($uninstallRun -match 'DVS_EXPLORER_COMMAND_REMOVED') $uninstallRun
    Assert-That 'uninstall does not re-register' `
        ($uninstallRun -notmatch 'DVS_EXPLORER_COMMAND_REGISTERED') $uninstallRun
    Assert-That 'uninstall exits 0' ((Get-ExitCode $uninstallRun) -eq 0) "exit code $(Get-ExitCode $uninstallRun)"

    Assert-That 'CLSID key is gone after uninstall' `
        (-not (Test-Path -LiteralPath "HKCU:\Software\Classes\CLSID\$clsid"))
    $leftoverVerbs = @(
        foreach ($extension in $mediaExtensions) {
            if (Test-Path -LiteralPath "HKCU:\Software\Classes\SystemFileAssociations\$extension\shell\$verbKeyName") {
                $extension
            }
        }
    )
    Assert-That 'no verb key is left after uninstall' ($leftoverVerbs.Count -eq 0) ($leftoverVerbs -join ',')
    # Without this the application would put the entry back the next time it started.
    Assert-That 'uninstall records that the command stays off' ((Get-ExplorerCommandMarker) -eq 0) `
        "marker is '$(Get-ExplorerCommandMarker)'"

    $repeatUninstall = Invoke-CommandFile -CommandFile $sandboxWrapper -Arguments @('-Uninstall')
    Assert-That 'uninstalling twice exits 0' ((Get-ExitCode $repeatUninstall) -eq 0) `
        "exit code $(Get-ExitCode $repeatUninstall)"

    # 9. A second copy of this command anywhere in the per-user classes root is a second menu entry:
    #    a file's menu is built from every verb in its association chain, and the per-extension key
    #    <ext>\shell is part of that chain just like SystemFileAssociations. Registering has to take
    #    such a copy away, and has to take away nothing else.
    Set-SecondCopyVerb -Extension '.mp4' -Name 'CompareStation.Open'
    Set-SecondCopyVerb -Extension '.mkv' -Name 'CompareStation.Legacy' -AsCommandLine `
        -CommandText "`"$sandboxExecutable`" `"%1`""
    $neighbourVerb = "HKCU:\Software\Classes\.mp4\shell\SomeOtherPlayer"
    New-Item -Path $neighbourVerb -Force | Out-Null
    New-ItemProperty -LiteralPath $neighbourVerb -Name 'ExplorerCommandHandler' `
        -Value '{11111111-2222-3333-4444-555555555555}' -PropertyType String -Force | Out-Null
    # The sweep can only be judged on the shape it is meant to see, so the planted keys are read
    # back before anything is expected to remove them.
    $plantedHandler = (Get-ItemProperty -LiteralPath `
            'HKCU:\Software\Classes\.mp4\shell\CompareStation.Open').PSObject.Properties['ExplorerCommandHandler']
    Assert-That 'the planted second copy really is a command handler key' `
        ($null -ne $plantedHandler -and $plantedHandler.Value -ceq $clsid)
    $plantedCommand = (Get-Item -LiteralPath `
            'HKCU:\Software\Classes\.mkv\shell\CompareStation.Legacy\command').GetValue('')
    Assert-That 'the planted command line really launches this application' `
        ($plantedCommand -like "*$sandboxExecutable*") $plantedCommand

    $sweepRun = Invoke-CommandFile -CommandFile $sandboxWrapper
    Assert-That 'registering takes the second copy away' `
        (-not (Test-Path -LiteralPath 'HKCU:\Software\Classes\.mp4\shell\CompareStation.Open'))
    Assert-That 'registering takes a leftover command line away' `
        (-not (Test-Path -LiteralPath 'HKCU:\Software\Classes\.mkv\shell\CompareStation.Legacy'))
    Assert-That 'registering says how many copies it removed' ($sweepRun -match 'stale_verbs=2') $sweepRun
    Assert-That "another tool's verb is left alone" (Test-Path -LiteralPath $neighbourVerb)
    Assert-That 'the entry the user is meant to see survives' `
        ((Get-RegisteredVerbValue -Extension '.mp4' -Property 'ExplorerCommandHandler') -ceq $clsid)
    Assert-That 'the emptied per-extension key is reclaimed' `
        (-not (Test-Path -LiteralPath 'HKCU:\Software\Classes\.mkv\shell'))

    # The documented removal has to take the same copies with it, or the entry simply comes back the
    # next time anybody registers it.
    Set-SecondCopyVerb -Extension '.avi' -Name 'CompareStation.Open'
    $sweepUninstall = Invoke-CommandFile -CommandFile $sandboxWrapper -Arguments @('-Uninstall')
    Assert-That 'uninstall takes a second copy away' `
        (-not (Test-Path -LiteralPath 'HKCU:\Software\Classes\.avi\shell\CompareStation.Open'))
    Assert-That 'uninstall says how many copies it removed' `
        ($sweepUninstall -match 'stale_verbs=1') $sweepUninstall

    # Same-named verbs at other locations, versioned ProgIDs and the former executable name.
    $upgradeCases = @(
        @{ Extension = '.mov'; Name = $verbKeyName },
        @{ Extension = 'SystemFileAssociations\.m4v'; Name = 'CompareStation.Compare-2.0.1' },
        @{ Extension = 'CompareStationTest.Legacy.2.0.1'; Name = $verbKeyName },
        @{ Extension = '.webp'; Name = 'OldVCStation'; AsCommandLine = $true;
            CommandText = '"C:\gone\VCStation.exe" "%1"' }
    )
    foreach ($case in $upgradeCases) {
        $path = "HKCU:\Software\Classes\$($case.Extension)\shell\$($case.Name)"
        Set-SecondCopyVerb @case
        $target = if ($case.ContainsKey('AsCommandLine')) {
            (Get-Item -LiteralPath (Join-Path $path 'command')).GetValue('')
        }
        else {
            (Get-ItemProperty -LiteralPath $path).ExplorerCommandHandler
        }
        Assert-That "upgrade fixture has the expected target: $path" `
            ($target -eq $clsid -or $target -eq '"C:\gone\VCStation.exe" "%1"')
        $upgradeRun = Invoke-CommandFile -CommandFile $sandboxWrapper
        Assert-That "register removes the old association: $path" `
            ((Get-ExitCode $upgradeRun) -eq 0 -and -not (Test-Path -LiteralPath $path)) $upgradeRun
        Set-SecondCopyVerb @case
        $removeRun = Invoke-CommandFile -CommandFile $sandboxWrapper -Arguments @('-Uninstall')
        Assert-That "uninstall removes the old association: $path" `
            ((Get-ExitCode $removeRun) -eq 0 -and -not (Test-Path -LiteralPath $path)) $removeRun
        Assert-That "uninstall reclaims the emptied parent: $path" `
            (-not (Test-Path -LiteralPath (Split-Path -Path $path -Parent)))
    }

    $null = Invoke-CommandFile -CommandFile $sandboxWrapper
    $canonicalCommand = "HKCU:\Software\Classes\SystemFileAssociations\.mp4\shell\$verbKeyName\command"
    New-Item -Path $canonicalCommand -Force | Out-Null
    Set-Item -LiteralPath $canonicalCommand -Value '"C:\gone\CompareStation.exe" "%1"'
    Assert-That 'canonical legacy command fixture is read back' `
        ((Get-Item -LiteralPath $canonicalCommand).GetValue('') -eq '"C:\gone\CompareStation.exe" "%1"')
    $null = Invoke-CommandFile -CommandFile $sandboxWrapper
    Assert-That 'registration replaces a canonical legacy command with the COM handler' `
        (-not (Test-Path -LiteralPath $canonicalCommand) -and
            (Get-RegisteredVerbValue -Extension '.mp4' -Property 'ExplorerCommandHandler') -eq $clsid)

    $foreignCases = @(
        @{ Extension = '.avi'; Name = 'TestOtherPlayer'; AsCommandLine = $true;
            CommandText = '"C:\other\Player.exe" "C:\CompareStation.exe"' },
        @{ Extension = '.avi'; Name = 'TestSimilarName'; AsCommandLine = $true;
            CommandText = '"C:\other\NotCompareStation.exe" "%1"' }
    )
    foreach ($case in $foreignCases) {
        Set-SecondCopyVerb @case
        $path = "HKCU:\Software\Classes\$($case.Extension)\shell\$($case.Name)\command"
        Assert-That "foreign fixture is read back: $($case.Name)" `
            ((Get-Item -LiteralPath $path).GetValue('') -eq $case.CommandText)
    }
    $null = Invoke-CommandFile -CommandFile $sandboxWrapper
    foreach ($case in $foreignCases) {
        $path = "HKCU:\Software\Classes\$($case.Extension)\shell\$($case.Name)\command"
        Assert-That "foreign executable survives: $($case.Name)" `
            ((Test-Path -LiteralPath $path) -and
                (Get-Item -LiteralPath $path).GetValue('') -eq $case.CommandText)
    }

    Set-SecondCopyVerb -Extension '.mov' -Name $verbKeyName
    & $registrationScript -InstallRoot $sandboxRoot -WhatIf | Out-Null
    Assert-That 'WhatIf leaves the same-named legacy verb intact' `
        (Test-Path -LiteralPath "HKCU:\Software\Classes\.mov\shell\$verbKeyName")
    $upgradedRun = Invoke-CommandFile -CommandFile $probeWrapper
    Assert-That 'a second ZIP directory registers successfully' ((Get-ExitCode $upgradedRun) -eq 0)
    Assert-That 'a second ZIP replaces the registered server instead of adding another' `
        ((Get-ItemProperty -LiteralPath "HKCU:\Software\Classes\CLSID\$clsid\InprocServer32").'(default)' -eq $probeServer)
    Assert-That 'a second ZIP keeps only its canonical menu and icon' `
        (-not (Test-Path -LiteralPath "HKCU:\Software\Classes\.mov\shell\$verbKeyName") -and
            (Get-RegisteredVerbValue -Extension '.mov' -Property 'Icon') -eq "$probeExecutable,0")
}
finally {
    $disabled = Join-Path $workPath $disabledScriptName
    if (Test-Path -LiteralPath $disabled) {
        Move-Item -LiteralPath $disabled -Destination $wrapperScript -Force
    }
    Remove-Registration
    Set-ExplorerCommandMarker -State $markerBefore
}

Write-Host ''
Write-Host "checks=$checkCount failures=$failureCount"
if ($checkCount -ne 85) {
    throw "Expected 85 checks, executed $checkCount. A test anchor or scenario is missing."
}
if ($failureCount -ne 0) {
    throw "$failureCount of $checkCount checks failed."
}
Write-Host 'DVS_CONTEXT_MENU_REGISTRATION_VERIFIED'
