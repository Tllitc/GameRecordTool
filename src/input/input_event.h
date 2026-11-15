/**
 * @file input_event.h
 * @brief 统一的原始输入事件结构（键盘 / 鼠标）。
 *
 * 由 RawInputService 在内部采集线程上从 WM_INPUT 消息解析得到，
 * 通过线程安全队列交给录制线程消费。同一条 RAWINPUT 消息中的多个
 * 鼠标属性（移动 / 按键 / 滚轮）会被拆分成多个 InputEvent，
 * 它们共享同一个采集时间戳 t。
 *
 * 纯数据头文件，仅依赖 <cstdint>，不引入 <windows.h>。
 */

#pragma once

#include <cstdint>

namespace grt {

/// 输入事件大类
enum class InputType {
	Key,         ///< 键盘按下 / 抬起
	MouseButton, ///< 鼠标按键按下 / 抬起（含侧键）
	MouseMove,   ///< 鼠标相对移动
	MouseWheel,  ///< 鼠标滚轮（垂直 / 水平）
};

/// 鼠标按键规范常量（取值与 WinUser.h 中 RI_MOUSE_* 位标志完全一致，
/// 但 WinUser 侧键只提供 RI_MOUSE_BUTTON_4/5_* 命名，这里统一给出语义名）
namespace mouse_buttons {
inline constexpr uint16_t kLeft    = 0x0001; ///< 左键（对应 RI_MOUSE_LEFT_BUTTON_DOWN/UP）
inline constexpr uint16_t kRight   = 0x0004; ///< 右键（对应 RI_MOUSE_RIGHT_BUTTON_DOWN/UP）
inline constexpr uint16_t kMiddle  = 0x0010; ///< 中键（对应 RI_MOUSE_MIDDLE_BUTTON_DOWN/UP）
inline constexpr uint16_t kXButton1 = 0x0040; ///< 侧键 1（XBUTTON1，对应 RI_MOUSE_BUTTON_4_*）
inline constexpr uint16_t kXButton2 = 0x0100; ///< 侧键 2（XBUTTON2，对应 RI_MOUSE_BUTTON_5_*）
} // namespace mouse_buttons

/**
 * @brief 一条原始输入事件。
 *
 * 字段按事件类型分组复用：
 *   - Key：使用 vkey / scan_code / e0 / e1 / key_down；
 *   - MouseButton：使用 button / key_down；
 *   - MouseMove：使用 dx / dy（允许为 0，如属性消息）；
 *   - MouseWheel：使用 wheel_delta / horizontal_wheel。
 */
struct InputEvent {
	InputType type = InputType::MouseMove;
	double t = 0.0; ///< 采集时刻时间戳（grt::Clock::NowSeconds()）

	// ---- 键盘 ----
	uint16_t vkey = 0;        ///< WM 输入的虚拟键码（VKEY_0xFF 为 overrun 伪消息，已丢弃）
	uint16_t scan_code = 0;   ///< 原始扫描码（RAWKEYBOARD.MakeCode）
	bool e0 = false;          ///< RAWKEYBOARD.Flags 含 RI_KEY_E0
	bool e1 = false;          ///< RAWKEYBOARD.Flags 含 RI_KEY_E1
	bool key_down = false;    ///< 键盘 / 鼠标键：true=按下，false=抬起

	// ---- 鼠标 ----
	uint16_t button = 0;      ///< 鼠标按键，取 mouse_buttons 中的规范常量（down 位值）
	int16_t wheel_delta = 0;  ///< 滚轮原始带符号 delta（WHEEL 与 HWHEEL 共用本字段）
	bool horizontal_wheel = false; ///< true=水平滚轮(HWHEEL)，false=垂直滚轮(WHEEL)
	int32_t dx = 0;           ///< 鼠标 X 方向相对移动量（RAWMOUSE.lLastX）
	int32_t dy = 0;           ///< 鼠标 Y 方向相对移动量（RAWMOUSE.lLastY）

	/**
	 * @brief 是否为注入事件。
	 *
	 * Raw Input 的 usFlags 只区分 RIM_INPUT / RIM_INPUTSINK（消息来源窗口
	 * 是否在前台），并不携带"注入"信息；hDevice 也只能标识设备句柄，
	 * 无法可靠区分 SendInput 合成事件。因此当前无法可靠判定，保持 false，
	 * 保留字段以便后续接入其他来源（如低级钩子 LLKHF_INJECTED）时补充。
	 */
	bool injected = false;
};

} // namespace grt
