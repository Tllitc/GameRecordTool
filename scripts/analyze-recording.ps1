<#
.SYNOPSIS
    Objective stutter analysis for a recording: PTS gaps, duplicate frames, freezes.

.DESCRIPTION
    Uses ffprobe/ffmpeg to measure a recording independently of any player:
      - stream info (codec, size, frame rate, duration, frame count)
      - PTS interval histogram, p99, max gap, gap list
      - duplicate-frame ratio (frames kept by mpdecimate)
      - freeze events (freezedetect)
    Diagnostic only; the source file is never modified.

.PARAMETER Path
    Recording file to analyze (.mp4/.mkv/.mov).

.PARAMETER Ffprobe
    Path to ffprobe.exe. Default: ..\ffmpeg\bin next to this script, then PATH.

.PARAMETER Ffmpeg
    Path to ffmpeg.exe. Default: ..\ffmpeg\bin next to this script, then PATH.

.PARAMETER Json
    Emit the full result as JSON.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\analyze-recording.ps1 `
      -Path "C:\Users\me\Videos\rec.mp4"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Path,
    [string]$Ffprobe = "",
    [string]$Ffmpeg = "",
    [switch]$Json
)

$ErrorActionPreference = 'Continue'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

function Resolve-Tool {
    param([string]$Explicit, [string]$Name)
    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
        throw "$Name not found: $Explicit"
    }
    $candidate = Join-Path $PSScriptRoot ("..\ffmpeg\bin\$Name.exe")
    if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "$Name.exe not found; pass -$($Name.Substring(0,1).ToUpper() + $Name.Substring(1)) explicitly."
}

function Invoke-Capture {
    param([string]$Exe, [string[]]$Arguments)
    # ffmpeg writes stats and filter logs to stderr. Windows PowerShell 5.1 wraps
    # native stderr as NativeCommandError, so keep the policy permissive and
    # collect everything as text.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        return (& $Exe @Arguments 2>&1 | Out-String)
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

if (-not (Test-Path -LiteralPath $Path)) {
    throw "Input file not found: $Path"
}
$Path = (Resolve-Path -LiteralPath $Path).Path
$ffprobePath = Resolve-Tool -Explicit $Ffprobe -Name 'ffprobe'
$ffmpegPath = Resolve-Tool -Explicit $Ffmpeg -Name 'ffmpeg'

# ---------- 1. stream info ----------
$streamRaw = Invoke-Capture $ffprobePath @(
    '-v', 'error', '-select_streams', 'v:0',
    '-show_entries', 'stream=codec_name,width,height,r_frame_rate,avg_frame_rate,duration',
    '-of', 'json', $Path
)
$streamObj = $streamRaw | ConvertFrom-Json
if (-not $streamObj.streams -or $streamObj.streams.Count -eq 0) {
    throw "No video stream in: $Path"
}
$stream = $streamObj.streams[0]

$formatRaw = Invoke-Capture $ffprobePath @(
    '-v', 'error', '-show_entries', 'format=duration,size,bit_rate', '-of', 'json', $Path
)
$format = ($formatRaw | ConvertFrom-Json).format
$durationSec = 0.0
if ($format.duration) { $durationSec = [double]::Parse($format.duration, $inv) }

# r_frame_rate looks like "60/1"
$fpsNum = 0.0
$fpsDen = 1.0
$rateParts = "$($stream.r_frame_rate)" -split '/'
if ($rateParts.Count -eq 2) {
    $fpsNum = [double]::Parse($rateParts[0], $inv)
    $fpsDen = [double]::Parse($rateParts[1], $inv)
}
if ($fpsNum -le 0) { $fpsNum = 30.0; $fpsDen = 1.0 }
$expectedMs = 1000.0 * $fpsDen / $fpsNum

# ---------- 2. PTS intervals ----------
$ptsRaw = Invoke-Capture $ffprobePath @(
    '-v', 'error', '-select_streams', 'v:0',
    '-show_entries', 'frame=pts_time', '-of', 'csv=p=0', $Path
)
$pts = New-Object System.Collections.Generic.List[double]
foreach ($line in ($ptsRaw -split "`r?`n")) {
    $text = $line.Trim()
    if ($text -match '^[0-9]+(\.[0-9]+)?$') {
        $pts.Add([double]::Parse($text, $inv))
    }
}
$totalFrames = $pts.Count
if ($totalFrames -lt 2) {
    throw "Not enough decodable video frames ($totalFrames)."
}
$pts.Sort()

$deltas = New-Object System.Collections.Generic.List[double]
for ($i = 1; $i -lt $pts.Count; $i++) {
    $deltas.Add(($pts[$i] - $pts[$i - 1]) * 1000.0)
}
$sorted = @($deltas | Sort-Object)
$p50 = $sorted[[int][math]::Floor(0.50 * ($sorted.Count - 1))]
$p95 = $sorted[[int][math]::Floor(0.95 * ($sorted.Count - 1))]
$p99 = $sorted[[int][math]::Floor(0.99 * ($sorted.Count - 1))]
$maxGap = $sorted[$sorted.Count - 1]

$gapThreshold = 1.5 * $expectedMs
$hardGapThreshold = 2.5 * $expectedMs
$gaps = New-Object System.Collections.Generic.List[object]
$gapCount = 0
$hardGapCount = 0
for ($i = 1; $i -lt $pts.Count; $i++) {
    $delta = $deltas[$i - 1]
    if ($delta -gt $gapThreshold) {
        $gapCount++
        if ($delta -gt $hardGapThreshold) { $hardGapCount++ }
        if ($gaps.Count -lt 50) {
            $gaps.Add([pscustomobject]@{
                fromSec = [math]::Round($pts[$i - 1], 3)
                toSec   = [math]::Round($pts[$i], 3)
                ms      = [math]::Round($delta, 1)
            })
        }
    }
}
$gapRatio = if ($totalFrames -gt 1) { $gapCount / ($totalFrames - 1) } else { 0.0 }

# ---------- 3. duplicate frames ----------
$dupRaw = Invoke-Capture $ffmpegPath @(
    '-hide_banner', '-nostdin', '-i', $Path,
    '-vf', 'mpdecimate', '-an', '-f', 'null', '-'
)
$frameMatches = [regex]::Matches($dupRaw, 'frame=\s*(\d+)')
$keptFrames = $totalFrames
if ($frameMatches.Count -gt 0) {
    $keptFrames = [int]$frameMatches[$frameMatches.Count - 1].Groups[1].Value
}
if ($keptFrames -gt $totalFrames) { $keptFrames = $totalFrames }
$dupRatio = if ($totalFrames -gt 0) { 1.0 - ($keptFrames / [double]$totalFrames) } else { 0.0 }

# ---------- 4. freeze detection ----------
$freezeRaw = Invoke-Capture $ffmpegPath @(
    '-hide_banner', '-nostdin', '-i', $Path,
    '-vf', 'freezedetect=n=0.003:d=0.5', '-an', '-f', 'null', '-'
)
$freezeStarts = [regex]::Matches($freezeRaw, 'freeze_start:\s*([0-9.]+)')
$freezeEnds = [regex]::Matches($freezeRaw, 'freeze_end:\s*([0-9.]+)')
$freezeDurations = [regex]::Matches($freezeRaw, 'freeze_duration:\s*([0-9.]+)')
$maxFreeze = 0.0
foreach ($m in $freezeDurations) {
    $value = [double]::Parse($m.Groups[1].Value, $inv)
    if ($value -gt $maxFreeze) { $maxFreeze = $value }
}
# A freeze still active at EOF has no freeze_duration; estimate from the tail.
if ($freezeStarts.Count -gt $freezeEnds.Count -and $freezeStarts.Count -gt 0) {
    $lastStart = [double]::Parse($freezeStarts[$freezeStarts.Count - 1].Groups[1].Value, $inv)
    $tailFreeze = [math]::Max(0.0, $durationSec - $lastStart)
    if ($tailFreeze -gt $maxFreeze) { $maxFreeze = $tailFreeze }
}

# ---------- 5. findings ----------
# Gaps in the first/last second are encoder warm-up/flush, not gameplay stutter.
$effectiveGaps = @($gaps | Where-Object { $_.fromSec -gt 1.0 -and $_.toSec -lt ($durationSec - 1.0) })
$findings = New-Object System.Collections.Generic.List[string]
if ($effectiveGaps.Count -gt 0) {
    $findings.Add("PTS gaps: $($effectiveGaps.Count) outside the first/last second ($gapCount over 1.5x total, max $([math]::Round($maxGap,1)) ms). Suspect encoder lag, capture stall, or disk stall.")
}
elseif ($gapCount -gt 0) {
    $findings.Add("PTS gaps only at start/stop ($gapCount over 1.5x, max $([math]::Round($maxGap,1)) ms): normal encoder warm-up/flush, not gameplay stutter.")
}
if ($dupRatio -gt 0.5) {
    $findings.Add("Duplicate frames $([math]::Round($dupRatio*100,1))%: if output is 60 fps and the source is ~30 fps this is expected; otherwise inspect the capture path (WGC/BitBlt/game_capture).")
}
elseif ($dupRatio -gt 0.1) {
    $findings.Add("Duplicate frames $([math]::Round($dupRatio*100,1))%: elevated; check capture path and source frame-rate stability (static menus inflate this).")
}
if ($maxFreeze -gt 0.5) {
    $findings.Add("Longest freeze $([math]::Round($maxFreeze,1)) s: normal in menus/loading; in gameplay inspect capture hook, alt-tab, or disk.")
}
if ($findings.Count -eq 0) {
    $findings.Add("No obvious timeline or pacing anomaly; if it still looks choppy, check player, display refresh, and frame pacing.")
}

$result = [pscustomobject]@{
    file         = $Path
    codec        = $stream.codec_name
    width        = [int]$stream.width
    height       = [int]$stream.height
    rFrameRate   = "$($stream.r_frame_rate)"
    durationSec  = [math]::Round($durationSec, 3)
    totalFrames  = $totalFrames
    expectedMs   = [math]::Round($expectedMs, 2)
    pts          = [pscustomobject]@{
        p50Ms    = [math]::Round($p50, 2)
        p95Ms    = [math]::Round($p95, 2)
        p99Ms    = [math]::Round($p99, 2)
        maxGapMs = [math]::Round($maxGap, 2)
        gapCount = $gapCount
        gapRatio = [math]::Round($gapRatio, 5)
    }
    duplicate    = [pscustomobject]@{
        keptFrames = $keptFrames
        ratio      = [math]::Round($dupRatio, 5)
    }
    freeze       = [pscustomobject]@{
        events = $freezeStarts.Count
        maxSec = [math]::Round($maxFreeze, 2)
    }
    gaps         = $gaps
    findings     = $findings
}

if ($Json) {
    $result | ConvertTo-Json -Depth 6
    exit 0
}

Write-Host ""
Write-Host "==== Recording stutter analysis ====" -ForegroundColor Cyan
Write-Host ("File      : {0}" -f $result.file)
Write-Host ("Codec/Size: {0}  {1}x{2}  {3} fps  {4}s  {5} frames" -f `
    $result.codec, $result.width, $result.height, $result.rFrameRate, $result.durationSec, $result.totalFrames)
Write-Host ("PTS       : p50={0}ms p95={1}ms p99={2}ms max={3}ms  expected {4}ms" -f `
    $result.pts.p50Ms, $result.pts.p95Ms, $result.pts.p99Ms, $result.pts.maxGapMs, $result.expectedMs)
Write-Host ("Gaps      : {0} over 1.5x, ratio {1:P2}" -f $result.pts.gapCount, $result.pts.gapRatio)
Write-Host ("Duplicates: kept {0}/{1}, ratio {2:P1}" -f `
    $result.duplicate.keptFrames, $result.totalFrames, $result.duplicate.ratio)
Write-Host ("Freezes   : {0} event(s), longest {1}s" -f $result.freeze.events, $result.freeze.maxSec)

if ($result.gaps.Count -gt 0) {
    Write-Host ""
    Write-Host "---- Gap list (first 50) ----"
    $result.gaps | Format-Table -AutoSize | Out-String | Write-Host
}

Write-Host "---- Findings ----"
foreach ($finding in $result.findings) {
    Write-Host ("- {0}" -f $finding)
}
Write-Host ""
Write-Host "Note: static menus/idle scenes naturally trip duplicate/freeze metrics; judge gameplay segments." -ForegroundColor DarkGray