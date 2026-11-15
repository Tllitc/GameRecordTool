# GameRecordTool

GameRecordTool 是一个基于 **OBS Studio 32.2.2** 的 Windows 前端插件（C++20），用于在录制游戏画面的同时，高精度采集键鼠原始输入，并自动导出符合规范的“三件套”数据：

- 游戏视频（MP4 / H.264 / H.265）
- 操作数据 JSON（`movement_events` / `looking_events` / `action_events`）
- 按键说明书 keymap JSON

插件以 OBS Dock 面板的形式集成在主界面，支持全局热键、悬浮窗、游戏配置档案、录制历史与录制后自动校验。

## 功能特性

- **游戏画面采集**：基于 OBS Game Capture 采集指定游戏窗口，分辨率跟随窗口客户区（支持 1080p / 1440p / 4K）。
- **录制参数**：帧率 30 / 60；编码 H.264 / H.265；硬件编码优先（NVENC / AMF / QSV），H.264 可回退 x264；按分辨率自动匹配码率档位（1080p 12 Mbps、1440p 30 Mbps、4K 45 Mbps），也支持手动覆盖。
- **崩溃安全**：优先使用 fragmented MP4，必要时回退 MKV 并在停止后转封装为 MP4。
- **原始输入采集**：通过 Windows Raw Input 获取键盘、鼠标按键与**相对位移**（raw mouse delta），适配指针锁定（pointer lock）的 3D / FPS 游戏；支持检测并一键关闭系统鼠标加速。
- **三类事件自动生成**：
  - `movement_events`：W/A/S/D（及组合方向 WL/WR/BL/BR）的区间型移动事件；
  - `looking_events`：高频（≥30 Hz）连续的 `dx/dy` 视角位移，附带可选粗方向标签；
  - `action_events`：按键 / 鼠标 / 滚轮触发的离散动作，以及手动打点。
- **时间同步**：以 QPC 为统一时间基准，并结合视频帧时间校准，事件与视频帧误差 ≤ 0.1 s。
- **操作体验**：全局热键（开始 / 停止 / 打点 / 悬浮窗开关）、悬浮窗状态显示、按游戏 exe 保存的配置档案与 keymap 编辑器。
- **历史与校验**：录制历史列表；录制结束后后台自动校验视频参数、JSON 结构、keymap 覆盖情况与时长一致性，也可手动重新校验。

## 界面与使用流程

1. 启动便携版 OBS，在主界面打开“游戏录制工具”Dock。
2. 在窗口下拉框中选择目标游戏窗口（展开时自动刷新窗口列表）。
3. 确认鼠标加速已关闭（未关闭时开始按钮不可用，可点击“一键关闭”）。
4. 在“录制参数”页设置输出目录、帧率、编码器、码率等。
5. 在“按键映射”页为当前游戏维护 keymap（按键 → 动作语义）。
6. 点击“开始录制”或使用全局热键；录制中可打点、查看实时分辨率 / 帧率 / 码率。
7. 停止后自动导出数据并执行三件套校验，结果可在“历史”页查看。

## 输出结构

每次录制生成一个会话目录：

```text
<output_dir>/
└─ <游戏名>_<yyyyMMdd-HHmmss>_<3位序号>/
   ├─ <base>.mp4            # 录制视频
   ├─ <base>.json           # 操作数据（三件套核心）
   ├─ <base>_keymap.json    # 本次会话的 keymap 快照
   └─ <base>_raw.jsonl      # 原始事件流（崩溃取证 / 回溯用）
```

操作数据 JSON 示例：

```json
{
  "game_metadata": {
    "game_title": "Space Adventure v1.0",
    "session_id": "SA-2023-08-15-001",
    "video_name": "xxx.mp4",
    "start_time": "2023-08-15T14:30:00",
    "duration_seconds": 600
  },
  "movement_events": [
    { "direction": "W", "start_time": 0.0, "end_time": 15.2 },
    { "direction": "WR", "start_time": 15.2, "end_time": 22.5 }
  ],
  "looking_events": [
    { "start_time": 0.00, "end_time": 0.03, "dx": 0.0, "dy": 0.0, "direction": "Neutral" },
    { "start_time": 0.03, "end_time": 0.06, "dx": 12.0, "dy": -3.0, "direction": "R" }
  ],
  "action_events": [
    { "timestamp": 12.3, "mouse_event": "rightClick", "keyboard_event": "H", "action": "上马" }
  ]
}
```

所有时间均为相对视频起点的秒数（float）。`dx/dy` 为鼠标相对位移，累计值可能远大于屏幕分辨率，属于正常现象。

keymap 格式：

```json
{
  "H": { "type": "keyboard", "action": "上马" },
  "K": { "type": "keyboard", "action": "开枪" },
  "rightClick": { "type": "mouse", "action": "捡装备" }
}
```

## 代码结构

```text
src/
├─ plugin/     插件入口与 OBS 前端事件接线
├─ core/       日志、时钟、全局配置（PluginConfig）
├─ input/      Raw Input、鼠标加速、按键名、全局热键
├─ capture/    窗口枚举、Game Capture 场景、编码器与输出配置
├─ video/      视频时间校准、偏移估计
├─ session/    录制会话状态机、spool、三类事件生成器
├─ export/     操作数据 JSON / keymap 原子落盘与自检
├─ verify/     录制后三件套校验（ffprobe 或内置 MP4 解析）
└─ ui/         Qt Dock 主面板、设置、keymap、历史、悬浮窗
scripts/
├─ deploy.ps1          # 部署 DLL 到便携版 OBS
├─ run-obs.ps1         # 启动便携版 OBS
└─ analyze-recording.ps1
```

核心模块职责：

| 模块 | 说明 |
| --- | --- |
| `SessionManager` | Idle → Preparing → Recording → Finalizing 状态机；125 Hz 处理线程；统一 QPC 时间基准 |
| `RawInputService` | 共享 Raw Input 采集服务，录制与 keymap 编辑器复用 |
| `MovementTracker` / `LookingAggregator` / `ActionMapper` | 分别生成移动、视角、动作事件 |
| `OutputConfigurator` | 探测编码器、计算码率、写入 OBS 输出配置 |
| `VideoTimeCalibrator` | 用 OBS 视频帧时间校准 QPC 与会话时间 |
| `ExportPipeline` | `<base>.json` 与 `<base>_keymap.json` 的原子导出 |
| `SessionVerifier` | 视频质量、JSON schema、keymap 覆盖与时长一致性校验 |

## 构建

### 环境要求

- Windows 10 / 11 x64
- Visual Studio 2022（MSVC 14.37+，支持 C++20）
- CMake 3.21+
- OBS Studio 32.2.2 便携版（用于运行与调试）
- Qt 6.11.1 `win64_msvc2022_64`（至少包含 `qtbase` / `Widgets`）
- OBS 开发文件：`libobs` / `obs-frontend-api` 头文件及对应的 `.lib` 导入库

依赖目录约定（也可通过 CMake 参数覆盖）：

```text
third_party/
├─ obs-dev/
│  ├─ include/libobs/
│  ├─ include/obs-frontend-api/
│  └─ lib/obs.lib
│     lib/obs-frontend-api.lib
└─ Qt/6.11.1/msvc2022_64/
```

> `third_party/`、`OBS/`、`ffmpeg/` 等体积较大的本地依赖不纳入版本控制，需要自行准备。

### 编译

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物：`build/Release/game_record_tool.dll`。

自定义依赖路径：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_DEV_DIR=<OBS 开发文件目录> `
  -DQT_DIR=<Qt msvc2022_64 目录>
```

如需输出 DEBUG 级别详细日志，追加 `-DGRT_VERBOSE_LOG=ON`。

### 部署与运行

```powershell
# 将 DLL 复制到 OBS\obs-plugins\64bit（需先关闭 OBS）
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1

# 启动便携版 OBS
powershell -ExecutionPolicy Bypass -File scripts\run-obs.ps1
```

若 `ffmpeg/bin/ffprobe.exe` 存在于工程目录，部署脚本会一并复制，用于更准确的视频参数校验；缺失时校验会回退到插件内置的 MP4 元数据解析器。

## 校验规则

录制结束后自动校验以下内容：

- 视频为 MP4 / fMP4，可解析；
- 分辨率 ≥ 1920×1080，帧率 ≥ 24 fps；
- 编码为 H.264 / H.265；
- 平均码率不低于所在分辨率档位的下限；
- 操作数据 JSON 可解析，包含 `game_metadata` 与三个事件数组；
- 事件时间合法、有序，且不超过视频时长；
- `video_name` 与实际视频文件一致；
- keymap 覆盖 `action_events` 中出现的全部键鼠事件标识；
- `duration_seconds` 与视频实际时长偏差 ≤ 0.1 s。

## 说明

- 本项目面向 Windows 平台，依赖 OBS Studio 前端 API 与 Qt 6。
- 仓库不包含 OBS Studio、Qt、FFmpeg 的二进制文件，请按各自许可协议自行获取。
- OBS Studio 采用 GPL-2.0 许可；分发本插件时请同时遵守其依赖组件的许可要求。
