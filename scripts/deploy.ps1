<#
.SYNOPSIS
    将构建产物 game_record_tool.dll 部署到便携版 OBS 的插件目录。

.DESCRIPTION
    1. 校验源 DLL 与目标插件目录是否存在，目录缺失时给出清晰错误；
    2. 若 obs64.exe 正在运行则报错退出（避免 DLL 被占用导致复制失败）；
    3. 比较源/目标文件时间戳，目标已是最新则跳过复制；
    4. 仅复制 game_record_tool.dll 本身，不复制任何 Qt/运行库 DLL
       （所需运行库均由便携版 OBS 自带）；
    5. 若仓库自带 ffmpeg\bin\ffprobe.exe，则一并复制到插件同目录，
       供录制后校验使用；缺失时仅警告（内置 MP4 解析器可兜底）。

.PARAMETER Configuration
    构建配置名称，默认 Release。对应产物位于 build\<Configuration>\ 下。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1
    powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Configuration Debug
#>
param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

# 工作区根目录（脚本位于 <根目录>\scripts 下）
$RootDir = Split-Path -Parent $PSScriptRoot

# 构建产物
$SourceDll = Join-Path $RootDir "build\$Configuration\game_record_tool.dll"

# 便携版 OBS 路径
$ObsRoot = Join-Path $RootDir "OBS"
$PluginDir = Join-Path $ObsRoot "obs-plugins\64bit"
$ObsExe = Join-Path $ObsRoot "bin\64bit\obs64.exe"
$TargetDll = Join-Path $PluginDir "game_record_tool.dll"

# 1. 校验源 DLL
if (-not (Test-Path -LiteralPath $SourceDll -PathType Leaf)) {
    throw "未找到构建产物：$SourceDll`n请先执行 cmake --build build --config $Configuration。"
}

# 2. 校验插件目录（不自动创建，目录缺失通常意味着 OBS 路径不对）
if (-not (Test-Path -LiteralPath $PluginDir -PathType Container)) {
    throw "未找到 OBS 插件目录：$PluginDir`n请确认便携版 OBS 已解压到 $ObsRoot。"
}

# 3. obs64.exe 正在运行则拒绝部署
$running = Get-Process -Name "obs64" -ErrorAction SilentlyContinue
if ($running) {
    throw "检测到 obs64.exe 正在运行（PID: $($running.Id -join ', ')），请先关闭 OBS 再部署。"
}

# 4. 时间戳比较，避免用旧文件覆盖新文件
$sourceFile = Get-Item -LiteralPath $SourceDll
if (Test-Path -LiteralPath $TargetDll -PathType Leaf) {
    $targetFile = Get-Item -LiteralPath $TargetDll
    if ($targetFile.LastWriteTime -ge $sourceFile.LastWriteTime) {
        Write-Host "目标 DLL 时间戳不早于源文件，无需部署：$TargetDll" -ForegroundColor Green
        return
    }
    Write-Host "更新插件 DLL（源文件较新）..."
} else {
    Write-Host "首次部署插件 DLL..."
}

Copy-Item -LiteralPath $SourceDll -Destination $TargetDll -Force
Write-Host "已部署：$SourceDll" -ForegroundColor Cyan
Write-Host "     -> $TargetDll" -ForegroundColor Cyan
Write-Host "注意：未复制任何 Qt/运行库 DLL（便携版 OBS 已自带依赖）。" -ForegroundColor DarkGray

# 5. 复制仓库自带的 ffprobe.exe 到插件同目录（校验器优先在此查找）。
#    源文件缺失不阻断部署：校验器会回退到内置 MP4 解析器。
$SourceFfprobe = Join-Path $RootDir "ffmpeg\bin\ffprobe.exe"
$TargetFfprobe = Join-Path $PluginDir "ffprobe.exe"
if (Test-Path -LiteralPath $SourceFfprobe -PathType Leaf) {
    Copy-Item -LiteralPath $SourceFfprobe -Destination $TargetFfprobe -Force
    Write-Host "已部署 ffprobe：$SourceFfprobe" -ForegroundColor Cyan
    Write-Host "             -> $TargetFfprobe" -ForegroundColor Cyan
} else {
    Write-Host "未找到 $SourceFfprobe，跳过 ffprobe 部署（将使用内置 MP4 解析）。" -ForegroundColor Yellow
}
