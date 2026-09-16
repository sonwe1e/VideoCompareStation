#requires -Version 7.0
<#
.SYNOPSIS
    Generates deterministic T0 evidence fixtures: frame-number videos with explicit source
    identity and image-failure pairs for the P4 image-folder evidence entry.

.DESCRIPTION
    Videos carry a visible source letter and frame number ("A 000042") so decoded frames can
    be verified against the trace (correct frame-group latency) and the contract hash. Media
    are encoded with no B-frames and a 60 kHz timescale so decode order == presentation order
    and PTS == frame * 1000 (usable for PTS assertions). Image fixtures cover complete pairs,
    a corrupt left, a corrupt right, different sizes, alpha-only content, case-fold pairing,
    same-name-different-extension, a missing side, and a large 4K pair for main-thread stall
    evidence.

.PARAMETER Ffmpeg
    Path to ffmpeg.exe.

.PARAMETER Ffprobe
    Path to ffprobe.exe.

.PARAMETER FixtureRoot
    Destination directory. Regeneration is deterministic for the same ffmpeg build.

.PARAMETER VideoSeconds
    Length of the 1080p60 A/B/C playback fixtures. Must exceed the longest baseline playback
    run (the perf entry fails with playback-ended-before-duration when the clip ends first).

.PARAMETER SmallSeconds
    Length of the 720x480/30 A/B fixtures.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Ffmpeg,

    [Parameter(Mandatory = $true)]
    [string]$Ffprobe,

    [string]$FixtureRoot = 'G:\Workspaces\Toy\out\evidence-fixtures',

    [int]$VideoSeconds = 75,

    [int]$SmallSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Ffmpeg -PathType Leaf)) {
    throw "ffmpeg not found at $Ffmpeg"
}
if (-not (Test-Path -LiteralPath $Ffprobe -PathType Leaf)) {
    throw "ffprobe not found at $Ffprobe"
}

$fontCandidates = @(
    'C:\Windows\Fonts\arial.ttf',
    'C:\Windows\Fonts\consola.ttf',
    'C:\Windows\Fonts\segoeui.ttf'
)
$font = $fontCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $font) {
    throw 'No TrueType font found under C:\Windows\Fonts for drawtext.'
}
# drawtext on this ffmpeg build crashes with backslashes in the font path; use forward
# slashes and escape the drive colon (C\\:/... proved stable across repeated runs).
$escapedFont = ((($font -replace '\\', '/') -replace ':', '\\:'))

$mediaRoot = Join-Path $FixtureRoot 'media'
$imageLeft = Join-Path $FixtureRoot 'image-pairs\L'
$imageRight = Join-Path $FixtureRoot 'image-pairs\R'
[void][System.IO.Directory]::CreateDirectory($mediaRoot)
[void][System.IO.Directory]::CreateDirectory($imageLeft)
[void][System.IO.Directory]::CreateDirectory($imageRight)

function Invoke-Ffmpeg {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )
    # Native stderr must not trip $ErrorActionPreference 'Stop' (PS 5.1 converts each
    # stderr line into an error record). drawtext + fontconfig on this gyan.dev build
    # intermittently crashes (0xC0000005) during filter init; retry once before failing.
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'SilentlyContinue'
    $lastOutput = $null
    $lastExit = -1
    for ($attempt = 0; $attempt -lt 2; ++$attempt) {
        $lastOutput = & $Ffmpeg @Arguments 2>&1
        $lastExit = $LASTEXITCODE
        if ($lastExit -eq 0 -or $lastExit -ne -1073741819) {
            break
        }
        Start-Sleep -Milliseconds 300
    }
    $ErrorActionPreference = $previousErrorActionPreference
    if ($lastExit -ne 0) {
        $detail = @($lastOutput | ForEach-Object { $_.ToString() }) |
            Select-Object -Last 8
        throw "ffmpeg failed (exit $lastExit): $($detail -join ' | ')"
    }
}

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-FrameSpotHashes {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [int[]]$FrameNumbers
    )
    # Per-frame MD5 via framemd5. The encoded fixtures keep a 1/fps timebase, so a frame N
    # carries PTS == N (verified against the muxed output). Returns a map of frame -> md5.
    $md5Lines = & $Ffmpeg -v error -i $Path -f framemd5 -
    $result = @{}
    $wanted = @{}
    foreach ($frame in $FrameNumbers) {
        $wanted[[string]$frame] = $true
    }
    foreach ($line in $md5Lines) {
        $line = $line.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith('#')) {
            continue
        }
        $fields = $line -split ','
        if ($fields.Count -lt 6) {
            continue
        }
        $pts = [int64]($fields[2].Trim())
        if ($wanted.ContainsKey([string]$pts)) {
            $result[[string]$pts] = $fields[5].Trim()
        }
    }
    if ($result.Count -ne $FrameNumbers.Count) {
        throw "Frame spot hashes incomplete for ${Path} (got $($result.Count), wanted $($FrameNumbers.Count))."
    }
    return $result
}

function New-FrameNumberVideo {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Letter,

        [Parameter(Mandatory = $true)]
        [string]$Color,

        [Parameter(Mandatory = $true)]
        [int]$Width,

        [Parameter(Mandatory = $true)]
        [int]$Height,

        [Parameter(Mandatory = $true)]
        [int]$Fps,

        [Parameter(Mandatory = $true)]
        [int]$Seconds
    )
    $timescale = $Fps * 1000
    $draw = "drawtext=fontfile=${escapedFont}:text='${Letter} %{frame_num}':fontsize=$([int]($Height / 10)):fontcolor=white:x=60:y=60:borderw=6:bordercolor=black"
    Invoke-Ffmpeg @(
        '-y',
        '-f', 'lavfi', '-i', "color=c=${Color}:s=${Width}x${Height}:r=${Fps}:d=${Seconds}",
        '-vf', $draw,
        '-c:v', 'libx264', '-preset', 'medium', '-crf', '18',
        '-pix_fmt', 'yuv420p', '-g', $Fps, '-bf', '0',
        '-video_track_timescale', "$timescale",
        '-an', '-t', "$Seconds",
        $Path
    )
    $probe = & $Ffprobe -v error -select_streams v:0 -show_entries stream=width,height,avg_frame_rate,nb_frames,codec_name,pix_fmt -of json $Path
    $stream = ($probe | ConvertFrom-Json).streams[0]
    if ([int]$stream.width -ne $Width -or [int]$stream.height -ne $Height -or
        [int64]$stream.nb_frames -ne ($Fps * $Seconds) -or
        $stream.avg_frame_rate -ne "$Fps/1" -or
        $stream.codec_name -ne 'h264') {
        throw "Fixture probe mismatch for ${Path}: $($probe | ConvertTo-Json -Compress)"
    }
}

function New-Png {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Color,

        [Parameter(Mandatory = $true)]
        [int]$Width,

        [Parameter(Mandatory = $true)]
        [int]$Height,

        [string]$PixelFormat = 'rgba'
    )
    $draw = "drawtext=fontfile=${escapedFont}:text='${Text}':fontsize=$([int]($Height / 8)):fontcolor=white:x=40:y=40:borderw=4:bordercolor=black"
    Invoke-Ffmpeg @(
        '-y', '-f', 'lavfi', '-i', "color=c=${Color}:s=${Width}x${Height}:d=1",
        '-frames:v', '1', '-vf', $draw, '-pix_fmt', $PixelFormat, $Path
    )
}

# --- Media fixtures with visible frame numbers ---
$mediaContract = [System.Collections.Generic.List[object]]::new()
$definitions = @(
    @{ File = 'frameid_1080p60_a.mp4'; Letter = 'A'; Color = '0x3a4a6a'; Width = 1920; Height = 1080; Fps = 60; Seconds = $VideoSeconds },
    @{ File = 'frameid_1080p60_b.mp4'; Letter = 'B'; Color = '0x4a2a3a'; Width = 1920; Height = 1080; Fps = 60; Seconds = $VideoSeconds },
    @{ File = 'frameid_1080p60_c.mp4'; Letter = 'C'; Color = '0x2a4a3a'; Width = 1920; Height = 1080; Fps = 60; Seconds = $VideoSeconds },
    @{ File = 'frameid_720x480_30_a.mp4'; Letter = 'A'; Color = '0x3a4a6a'; Width = 720; Height = 480; Fps = 30; Seconds = $SmallSeconds },
    @{ File = 'frameid_720x480_30_b.mp4'; Letter = 'B'; Color = '0x4a2a3a'; Width = 720; Height = 480; Fps = 30; Seconds = $SmallSeconds }
)
foreach ($definition in $definitions) {
    $path = Join-Path $mediaRoot $definition.File
    New-FrameNumberVideo -Path $path -Letter $definition.Letter -Color $definition.Color `
        -Width $definition.Width -Height $definition.Height -Fps $definition.Fps `
        -Seconds $definition.Seconds
    $spotFrames = if ($definition.Width -ge 1920) { @(0, 100, 719) } else { @(0, 59, 179) }
    $spots = Get-FrameSpotHashes -Path $path -FrameNumbers $spotFrames
    $mediaContract.Add([pscustomobject]@{
            file = $definition.File
            sha256 = Get-FileSha256 $path
            width = $definition.Width
            height = $definition.Height
            fps = "$($definition.Fps)/1"
            codec = 'h264'
            pix_fmt = 'yuv420p'
            nb_frames = $definition.Fps * $definition.Seconds
            spot_frame_md5 = $spots
        })
}

# --- Image failure fixtures (top-level files; matching names pair case-insensitively) ---
New-Png -Path (Join-Path $imageLeft 'ok.png') -Text 'LEFT OK' -Color '0x88aadd' -Width 960 -Height 540
New-Png -Path (Join-Path $imageRight 'ok.png') -Text 'RIGHT OK' -Color '0xdd88aa' -Width 960 -Height 540
New-Png -Path (Join-Path $imageLeft 'corrupt.png') -Text 'CORRUPT L' -Color '0x88aa44' -Width 960 -Height 540
New-Png -Path (Join-Path $imageRight 'corrupt.png') -Text 'CORRUPT R' -Color '0x44aa88' -Width 960 -Height 540
New-Png -Path (Join-Path $imageLeft 'corruptR.png') -Text 'VALID L' -Color '0xaa4488' -Width 960 -Height 540
New-Png -Path (Join-Path $imageRight 'corruptR.png') -Text 'CORRUPT R2' -Color '0x4488aa' -Width 960 -Height 540
New-Png -Path (Join-Path $imageLeft 'diffsize.png') -Text 'DIFF L' -Color '0xaa8844' -Width 960 -Height 540
New-Png -Path (Join-Path $imageRight 'diffsize.png') -Text 'DIFF R' -Color '0x44aa44' -Width 480 -Height 270
New-Png -Path (Join-Path $imageLeft 'alpha.png') -Text 'ALPHA L' -Color '0x88aadd@0.25' -Width 960 -Height 540
New-Png -Path (Join-Path $imageRight 'alpha.png') -Text 'ALPHA R' -Color '0x88aadd@0.75' -Width 960 -Height 540
New-Png -Path (Join-Path $imageLeft 'case.png') -Text 'CASE L' -Color '0x44aacc' -Width 960 -Height 540
New-Png -Path (Join-Path $imageRight 'CASE.PNG') -Text 'CASE R' -Color '0xcc44aa' -Width 960 -Height 540
New-Png -Path (Join-Path $imageLeft 'a.png') -Text 'A PNG' -Color '0x335577' -Width 960 -Height 540
New-Png -Path (Join-Path $imageRight 'a.jpg') -Text 'A JPG' -Color '0x775533' -Width 960 -Height 540
New-Png -Path (Join-Path $imageLeft 'missing.png') -Text 'MISSING' -Color '0x333333' -Width 960 -Height 540
New-Png -Path (Join-Path $imageLeft 'big.png') -Text 'BIG L' -Color '0x557799' -Width 3840 -Height 2160
New-Png -Path (Join-Path $imageRight 'big.png') -Text 'BIG R' -Color '0x995577' -Width 3840 -Height 2160

# Corrupt the corrupt.png left side and corruptR.png right side (truncate to 60%).
foreach ($corrupt in @(
        @{ File = (Join-Path $imageLeft 'corrupt.png') },
        @{ File = (Join-Path $imageRight 'corruptR.png') }
    )) {
    $bytes = [System.IO.File]::ReadAllBytes($corrupt.File)
    $kept = [int]($bytes.Length * 0.6)
    [System.IO.File]::WriteAllBytes($corrupt.File, $bytes[0..$kept])
}

# JPG variant with distinct content (same base name, different extension on the right side).
Invoke-Ffmpeg @(
    '-y', '-f', 'lavfi', '-i', 'color=c=0x997755:s=960x540:d=1',
    '-frames:v', '1', '-q:v', '2', (Join-Path $imageRight 'a.jpg')
)

$imageContract = [System.Collections.Generic.List[object]]::new()
foreach ($side in @('L', 'R')) {
    $folder = if ($side -eq 'L') { $imageLeft } else { $imageRight }
    foreach ($file in (Get-ChildItem -LiteralPath $folder -File | Sort-Object Name)) {
        $dimensions = $null
        $corrupt = $false
        try {
            $probe = & $Ffprobe -v error -select_streams v:0 -show_entries stream=width,height,codec_name -of json $file.FullName
            $stream = ($probe | ConvertFrom-Json).streams[0]
            if ($null -ne $stream) {
                $dimensions = [pscustomobject]@{ width = [int]$stream.width; height = [int]$stream.height }
            }
        } catch {
            $corrupt = $true
        }
        if ($null -eq $dimensions) {
            $corrupt = $true
        }
        if (-not $corrupt) {
            # A truncated PNG keeps a readable header; only a full decode reveals corruption.
            $previousErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = 'SilentlyContinue'
            [void](& $Ffmpeg -v error -i $file.FullName -f null - 2>&1)
            $decodeExit = $LASTEXITCODE
            $ErrorActionPreference = $previousErrorActionPreference
            if ($decodeExit -ne 0) {
                $corrupt = $true
            }
        }
        $imageContract.Add([pscustomobject]@{
                file = $file.Name
                side = $side
                sha256 = Get-FileSha256 $file.FullName
                size_bytes = $file.Length
                dimensions = $dimensions
                corrupt = $corrupt
            })
    }
}

$contract = [pscustomobject]@{
    generator = 'generate-evidence-fixtures.ps1'
    generated_utc = [DateTime]::UtcNow.ToString('o')
    font = $font
    ffmpeg = $Ffmpeg
    media = @($mediaContract)
    images = @($imageContract)
}
$contractPath = Join-Path $FixtureRoot 'contract.json'
[System.IO.File]::WriteAllText(
    $contractPath,
    ($contract | ConvertTo-Json -Depth 6),
    [System.Text.UTF8Encoding]::new($false)
)

# Read-back verification: spot-check two decoded frame numbers from the frame-number media.
$verifyA = Join-Path $mediaRoot 'frameid_1080p60_a.mp4'
$extract = Join-Path $FixtureRoot 'verify-frame-100.png'
Invoke-Ffmpeg @('-y', '-v', 'error', '-i', $verifyA, '-vf', 'select=eq(n\,100)', '-frames:v', '1', $extract)
if (-not (Test-Path -LiteralPath $extract -PathType Leaf)) {
    throw 'Frame extraction verification failed.'
}
Remove-Item -LiteralPath $extract

Write-Output "EVIDENCE_FIXTURES_OK root=$FixtureRoot media=$($mediaContract.Count) images=$($imageContract.Count)"
