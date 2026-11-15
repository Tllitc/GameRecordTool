/**
 * @file session_types.h
 * @brief 录制会话相关的纯数据结构定义（Task 6）。
 *
 * 本头文件只允许依赖 C++ 标准库与 capture/output_configurator.h 中的
 * RecordPlan（RecordPlan 本身也是纯标准库结构），不引入 OBS / Qt /
 * <windows.h>，以便后续 Task 7（导出）、Task 8/12（GUI/重处理）以及
 * Task 13（单元测试）安全包含。
 *
 * 时间约定（重要）：
 *   本文件中所有事件的 start/end/timestamp 字段均为【会话时间】（秒），
 *   即相对录制起点 qpc0 的 QPC 差值，t=0 为录制起点；允许 0 与正值，
 *   负值事件由各聚合器丢弃。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "capture/output_configurator.h" // grt::RecordPlan（纯标准库结构）

namespace grt {

/**
 * @brief 录制目标窗口的描述（快照，不含 HWND）。
 *
 * 全部字符串使用 UTF-8。刻意不保存 HWND：会话结束/重处理时原窗口可能
 * 已销毁，且 SessionContext 可能被序列化（Task 7），句柄无跨进程意义。
 */
struct SessionTargetInfo {
	std::string title;       ///< 窗口标题（UTF-8）
	std::string exeName;     ///< 可执行文件名（如 "game.exe"，UTF-8）
	std::string exePath;     ///< 可执行文件完整路径（UTF-8，取不到为空）
	int clientWidth = 0;     ///< 开始录制时的客户区宽度（像素）
	int clientHeight = 0;    ///< 开始录制时的客户区高度（像素）
};

/**
 * @brief 区间型移动事件，对应操作数据 JSON 中的 movement_events。
 *
 * 不变式（由 MovementTracker 保证）：
 *   - start_time < end_time（差小于 epsilon 的零长区间不输出）；
 *   - 相邻区间首尾相接（end_i == start_(i+1)）或中间存在无移动空档；
 *   - 同方向相邻区间已合并；
 *   - direction 仅取 W/B/L/R/WL/WR/BL/BR。
 */
struct MovementEvent {
	std::string direction; ///< 纵码在前的组合方向，如 "WL"、"BR"
	double start_time = 0.0;
	double end_time = 0.0;
};

/**
 * @brief 时间桶聚合的视角事件，对应 looking_events。
 *
 * 整事件计入其起点所在桶（不以落点切分）；无原始移动的空桶不输出，
 * 因此各条目在时间轴上允许不连续。桶对齐会话 t=0。
 */
struct LookingEvent {
	double start_time = 0.0;
	double end_time = 0.0;
	double dx = 0.0;       ///< 桶内相对 X 位移合计（像素原始增量）
	double dy = 0.0;       ///< 桶内相对 Y 位移合计（像素原始增量；屏幕坐标向下为正）
	std::string direction; ///< 粗标签，垂直分量在前（如右上为 "UR"）；无位移桶为 "Neutral"
};

/**
 * @brief 离散动作事件，对应 action_events。
 *
 * 仅在输入的 down 边沿（或滚轮）命中 keymap 时产生；手动打点事件的
 * mouse_event/keyboard_event 均为空字符串。滚轮一次滚动多条 notch 时
 * 会拆成多条同时刻、同名事件。
 */
struct ActionEvent {
	double timestamp = 0.0;
	std::string mouse_event;    ///< 鼠标规范名（如 "rightClick"/"wheelUp"），无则为空
	std::string keyboard_event; ///< 键盘规范名（如 "H"/"Space"），无则为空
	std::string action;         ///< keymap 中的中文动作语义
};

/**
 * @brief 一次录制会话的固定上下文（Start 成功后即不再改变）。
 */
struct SessionContext {
	std::string sessionDir;     ///< 会话目录绝对路径（UTF-8）
	std::string sessionBase;    ///< 会话文件基名（与目录同名，不含扩展名）
	std::string startedAtUtc;   ///< 开始时刻 UTC ISO-8601（"YYYY-MM-DDTHH:MM:SSZ"）
	double qpc0 = 0.0;          ///< 录制起点 QPC 秒（统一时间锚点）
	double qpc1 = 0.0;          ///< 调用录制停止前的 QPC 秒
	double durationSeconds = 0.0; ///< 最终会话时长（秒）
	bool timeCalibrationReady = false; ///< 开始时视频时钟校准是否已就绪
	SessionTargetInfo target;   ///< 录制目标窗口快照
	RecordPlan plan;            ///< 本次录制方案（分辨率/帧率/码率/编码器/容器）
};

/**
 * @brief 一次录制会话产出的全部工件（Task 6 负责收集，Task 7 负责落盘）。
 */
struct SessionArtifacts {
	std::string rawSpoolPath;          ///< <base>_raw.jsonl 完整路径
	std::string videoPath;             ///< 前端记录的录制文件完整路径（可能为空/remux 前路径）
	std::vector<MovementEvent> movementEvents;
	std::vector<LookingEvent> lookingEvents;
	std::vector<ActionEvent> actionEvents;
	std::uint64_t rawEventCount = 0;   ///< 写入 spool 的原始事件总数（不含会话元信息行）
	std::uint64_t negativeTimeDropped = 0; ///< 因会话时间 <0 被丢弃的事件数（含未入桶移动）
};

} // namespace grt
