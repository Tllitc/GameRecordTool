<#
.SYNOPSIS
    启动便携版 OBS Studio（obs64.exe）。

.DESCRIPTION
    - 自动定位 OBS\bin\64bit\obs64.exe；
    - 若 OBS 根目录存在 portable_mode.txt，则以便携模式（-portable）启动，
      此时配置与日志写入 OBS\config\obs-studio；
    - 其余命令行参数原样透传给 obs64.exe。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\run-obs.ps1
    powershell -ExecutionPolicy Bypass -File scripts\run-obs.ps1 --startreplaybuffer
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ObsArguments
)

$ErrorActionPreference = "Stop"

# 工作区根目录（脚本位于 <根目录>\scripts 下）
$RootDir = Split-Path -Parent $PSScriptRoot
$ObsRoot = Join-Path $RootDir "OBS"
$ObsExe = Join-Path $ObsRoot "bin\64bit\obs64.exe"
$PortableMarker = Join-Path $ObsRoot "portable_mode.txt"

if (-not (Test-Path -LiteralPath $ObsExe -PathType Leaf)) {
    throw "未找到 obs64.exe：$ObsExe`n请确认便携版 OBS 已解压到 $ObsRoot。"
}

# 组装启动参数：存在便携模式标记时追加 -portable，其余参数透传
$argumentList = @()
if (Test-Path -LiteralPath $PortableMarker -PathType Leaf) {
    Write-Host "检测到 portable_mode.txt，以便携模式启动 OBS。" -ForegroundColor Cyan
    $argumentList += "-portable"
} else {
    Write-Host "未检测到 portable_mode.txt，以普通模式启动 OBS（配置位于 %APPDATA%\obs-studio）。" -ForegroundColor DarkYellow
}
if ($ObsArguments) {
    $argumentList += $ObsArguments
}

Write-Host "启动：$ObsExe $($argumentList -join ' ')" -ForegroundColor Green
if ($argumentList.Count -gt 0) {
    Start-Process -FilePath $ObsExe -ArgumentList $argumentList -WorkingDirectory (Split-Path -Parent $ObsExe)
} else {
    # 无参数时不传 ArgumentList，避免空数组触发参数校验错误
    Start-Process -FilePath $ObsExe -WorkingDirectory (Split-Path -Parent $ObsExe)
}
