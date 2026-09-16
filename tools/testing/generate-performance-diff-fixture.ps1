[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$Ffmpeg,

    [ValidateNotNullOrEmpty()]
    [string]$Ffprobe = (Join-Path (Split-Path -Parent $Ffmpeg) 'ffprobe.exe'),

    [string]$FixtureRoot = $env:DVS_PERFORMANCE_FIXTURE_ROOT,

    [ValidateNotNullOrEmpty()]
    [string]$SourceName = 'gate-1080p60-a.mp4',

    [ValidateNotNullOrEmpty()]
    [string]$OutputName = 'gate-1080p60-diff-b.mp4',

    [ValidateRange(1, 3600)]
    [int]$NativeTimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $Ffmpeg -PathType Leaf)) {
    throw "FFmpeg executable was not found: $Ffmpeg"
}
if (-not (Test-Path -LiteralPath $Ffprobe -PathType Leaf)) {
    throw "ffprobe executable was not found: $Ffprobe"
}
if (-not $FixtureRoot) {
    throw 'Set FixtureRoot or DVS_PERFORMANCE_FIXTURE_ROOT.'
}

$resolvedFixtureRoot = (Resolve-Path -LiteralPath $FixtureRoot).Path
$sourcePath = [System.IO.Path]::GetFullPath((Join-Path $resolvedFixtureRoot $SourceName))
$outputPath = [System.IO.Path]::GetFullPath((Join-Path $resolvedFixtureRoot $OutputName))
$pathComparison = [System.StringComparison]::OrdinalIgnoreCase
if (-not [string]::Equals(
        [System.IO.Path]::GetDirectoryName($sourcePath), $resolvedFixtureRoot, $pathComparison
    ) -or
    -not [string]::Equals(
        [System.IO.Path]::GetDirectoryName($outputPath), $resolvedFixtureRoot, $pathComparison
    )) {
    throw 'SourceName and OutputName must be leaf names inside FixtureRoot.'
}
if (-not [string]::Equals([System.IO.Path]::GetExtension($outputPath), '.mp4', $pathComparison)) {
    throw 'OutputName must have an .mp4 extension.'
}
if ([string]::Equals($sourcePath, $outputPath, $pathComparison)) {
    throw 'The diff fixture output must not overwrite its source.'
}
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "The source performance fixture was not found: $sourcePath"
}

$manifestName = [System.IO.Path]::GetFileNameWithoutExtension($outputPath) + '.contract.json'
$manifestPath = [System.IO.Path]::GetFullPath((Join-Path $resolvedFixtureRoot $manifestName))
if ([string]::Equals($manifestPath, $sourcePath, $pathComparison) -or
    [string]::Equals($manifestPath, $outputPath, $pathComparison)) {
    throw 'The source, output, and generated contract paths must be distinct.'
}
$temporarySuffix = [Guid]::NewGuid().ToString('N')
$temporaryVideo = Join-Path $resolvedFixtureRoot ".diff-fixture-$temporarySuffix.mp4"
$temporaryManifest = Join-Path $resolvedFixtureRoot ".diff-fixture-$temporarySuffix.json"

function Invoke-BoundedNative {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][string]$Operation
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw "$Operation could not be started."
        }
        $standardOutput = $process.StandardOutput.ReadToEndAsync()
        $standardError = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($NativeTimeoutSeconds * 1000)) {
            try {
                $process.Kill($true)
            } catch [System.Management.Automation.MethodException] {
                $process.Kill()
            }
            $process.WaitForExit()
            throw "$Operation timed out after $NativeTimeoutSeconds seconds."
        }
        $process.WaitForExit()

        $output = $standardOutput.GetAwaiter().GetResult()
        $errorOutput = $standardError.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            $detail = $errorOutput.Trim()
            if ($detail.Length -gt 2000) {
                $detail = $detail.Substring($detail.Length - 2000)
            }
            throw "$Operation failed with exit code $($process.ExitCode): $detail"
        }
        return [pscustomobject]@{
            StandardOutput = $output
            StandardError = $errorOutput
        }
    } finally {
        $process.Dispose()
    }
}

function Invoke-Ffmpeg {
    param([Parameter(Mandatory)][string[]]$Arguments)

    $null = Invoke-BoundedNative -FilePath $Ffmpeg -Arguments $Arguments -Operation 'FFmpeg'
}

function Get-VideoMetadata {
    param([Parameter(Mandatory)][string]$Path)

    $probeResult = Invoke-BoundedNative `
        -FilePath $Ffprobe `
        -Operation "ffprobe for $Path" `
        -Arguments @(
            '-v', 'error',
            '-select_streams', 'v:0',
            '-show_entries',
            'stream=codec_name,width,height,avg_frame_rate,pix_fmt,nb_frames,duration',
            '-of', 'json',
            $Path
        )
    $probe = $probeResult.StandardOutput | ConvertFrom-Json
    $streams = @($probe.streams)
    if ($streams.Count -ne 1) {
        throw "Expected one primary video stream in $Path."
    }
    return $streams[0]
}

function Assert-1080p60 {
    param(
        [Parameter(Mandatory)]$Metadata,
        [Parameter(Mandatory)][string]$Path
    )

    $rateParts = [string]$Metadata.avg_frame_rate -split '/'
    if ($rateParts.Count -ne 2 -or [double]$rateParts[1] -eq 0.0) {
        throw "Invalid average frame rate in $Path."
    }
    $rate = [double]$rateParts[0] / [double]$rateParts[1]
    if ([int]$Metadata.width -ne 1920 -or [int]$Metadata.height -ne 1080 -or
        [Math]::Abs($rate - 60.0) -gt 0.000001) {
        throw "$Path must be 1920x1080 at 60 fps."
    }
}

function Get-FirstFrameHash {
    param([Parameter(Mandatory)][string]$Path)

    $hashResult = Invoke-BoundedNative `
        -FilePath $Ffmpeg `
        -Operation "FFmpeg first-frame hash for $Path" `
        -Arguments @(
            '-hide_banner', '-loglevel', 'error',
            '-i', $Path,
            '-map', '0:v:0',
            '-frames:v', '1',
            '-f', 'hash',
            '-hash', 'sha256',
            '-'
        )
    $hashLine = $hashResult.StandardOutput -split '\r?\n' |
        Where-Object { $_ -match '^SHA256=[0-9a-fA-F]{64}$' } |
        Select-Object -Last 1
    if (-not $hashLine) {
        throw "FFmpeg did not emit a first-frame SHA-256 for $Path."
    }
    return $hashLine.Substring('SHA256='.Length).ToLowerInvariant()
}

function Publish-FixturePair {
    param(
        [Parameter(Mandatory)][string]$Video,
        [Parameter(Mandatory)][string]$Contract
    )

    $backupSuffix = [Guid]::NewGuid().ToString('N')
    $backupVideo = Join-Path $resolvedFixtureRoot ".diff-fixture-backup-$backupSuffix.mp4"
    $backupManifest = Join-Path $resolvedFixtureRoot ".diff-fixture-backup-$backupSuffix.json"
    $videoBackedUp = $false
    $manifestBackedUp = $false
    $videoPublished = $false
    $manifestPublished = $false
    try {
        if (Test-Path -LiteralPath $outputPath -PathType Leaf) {
            Move-Item -LiteralPath $outputPath -Destination $backupVideo
            $videoBackedUp = $true
        }
        if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
            Move-Item -LiteralPath $manifestPath -Destination $backupManifest
            $manifestBackedUp = $true
        }

        Move-Item -LiteralPath $Video -Destination $outputPath
        $videoPublished = $true
        Move-Item -LiteralPath $Contract -Destination $manifestPath
        $manifestPublished = $true
    } catch {
        $publishError = $_
        $rollbackErrors = [System.Collections.Generic.List[string]]::new()
        if ($manifestPublished -and (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
            try {
                Remove-Item -LiteralPath $manifestPath -Force
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($videoPublished -and (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
            try {
                Remove-Item -LiteralPath $outputPath -Force
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($manifestBackedUp -and (Test-Path -LiteralPath $backupManifest -PathType Leaf)) {
            try {
                Move-Item -LiteralPath $backupManifest -Destination $manifestPath
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($videoBackedUp -and (Test-Path -LiteralPath $backupVideo -PathType Leaf)) {
            try {
                Move-Item -LiteralPath $backupVideo -Destination $outputPath
            } catch {
                $rollbackErrors.Add($_.Exception.Message)
            }
        }
        if ($rollbackErrors.Count -ne 0) {
            $message = "Fixture pair publication failed and rollback was incomplete. " +
                "Backups were retained in $resolvedFixtureRoot. Publish error: " +
                "$($publishError.Exception.Message) Rollback errors: $($rollbackErrors -join '; ')"
            throw $message
        }
        $message = "Fixture pair publication failed; the previous pair was restored: " +
            $publishError.Exception.Message
        throw $message
    }

    foreach ($backup in @($backupVideo, $backupManifest)) {
        if (Test-Path -LiteralPath $backup -PathType Leaf) {
            try {
                Remove-Item -LiteralPath $backup -Force
            } catch {
                Write-Warning "The new fixture pair is valid, but backup cleanup failed: $backup"
            }
        }
    }
}

try {
    $sourceMetadata = Get-VideoMetadata -Path $sourcePath
    Assert-1080p60 -Metadata $sourceMetadata -Path $sourcePath

    # A solid center patch covers 25% of the decoded frame. This creates a deterministic visible
    # difference while preserving the source duration, 1080p geometry, and 60 fps cadence.
    $differenceFilter = 'drawbox=x=iw/4:y=ih/4:w=iw/2:h=ih/2:color=white:t=fill'
    Invoke-Ffmpeg -Arguments @(
        '-hide_banner', '-loglevel', 'error', '-y',
        '-i', $sourcePath,
        '-map', '0:v:0', '-an',
        '-vf', $differenceFilter,
        '-fps_mode', 'passthrough',
        '-c:v', 'libx264', '-preset', 'fast', '-crf', '18',
        '-pix_fmt', 'yuv420p', '-g', '120', '-keyint_min', '120', '-sc_threshold', '0',
        $temporaryVideo
    )

    $outputMetadata = Get-VideoMetadata -Path $temporaryVideo
    Assert-1080p60 -Metadata $outputMetadata -Path $temporaryVideo
    if (-not [string]$sourceMetadata.nb_frames -or -not [string]$outputMetadata.nb_frames -or
        [string]$sourceMetadata.codec_name -cne [string]$outputMetadata.codec_name -or
        [string]$sourceMetadata.pix_fmt -cne [string]$outputMetadata.pix_fmt -or
        [string]$sourceMetadata.nb_frames -cne [string]$outputMetadata.nb_frames) {
        throw 'The generated diff fixture did not preserve codec, pixel format, and frame count.'
    }

    $sourceFrameHash = Get-FirstFrameHash -Path $sourcePath
    $outputFrameHash = Get-FirstFrameHash -Path $temporaryVideo
    if ($sourceFrameHash -ceq $outputFrameHash) {
        throw 'The generated diff fixture has the same decoded first frame as its source.'
    }

    $sourceFileHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $outputFileHash = (Get-FileHash -LiteralPath $temporaryVideo -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = [ordered]@{
        contractVersion = 1
        sourceFileName = [System.IO.Path]::GetFileName($sourcePath)
        sourceFileSha256 = $sourceFileHash
        fixtureFileName = [System.IO.Path]::GetFileName($outputPath)
        fixtureFileSha256 = $outputFileHash
        width = 1920
        height = 1080
        averageFrameRate = '60/1'
        codec = [string]$outputMetadata.codec_name
        pixelFormat = [string]$outputMetadata.pix_fmt
        frameCount = [string]$outputMetadata.nb_frames
        differenceFilter = $differenceFilter
        sourceFirstFrameSha256 = $sourceFrameHash
        fixtureFirstFrameSha256 = $outputFrameHash
    }
    $manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $temporaryManifest -Encoding utf8

    Publish-FixturePair -Video $temporaryVideo -Contract $temporaryManifest
} finally {
    if (Test-Path -LiteralPath $temporaryVideo -PathType Leaf) {
        Remove-Item -LiteralPath $temporaryVideo -Force
    }
    if (Test-Path -LiteralPath $temporaryManifest -PathType Leaf) {
        Remove-Item -LiteralPath $temporaryManifest -Force
    }
}

Write-Output "DVS_DIFF_FIXTURE_READY fixture=$outputPath contract=$manifestPath"
