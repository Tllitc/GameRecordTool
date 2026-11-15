/**
 * @file action_mapper.h
 * @brief 动作事件（action_events）生成器：按下/滚轮边沿命中 keymap 时产出动作。
 *
 * 纯逻辑模块：只依赖标准库、core/plugin_config.h（KeymapEntry 为纯标准库
 * 结构）、input/input_event.h 与 input/key_names.h，不包含 <windows.h>/OBS/Qt，
 * 供 Task 13 单元测试。
 *
 * 规则（与任务约定一一对应）：
 *   1. keymap 在开始录制时做浅拷贝快照（SetKeymap），录制过程中用户修改
 *      配置不影响本次会话的判定；
 *   2. 只有 down 边沿（键盘按下、鼠标按键按下）触发动作；up 边沿一律忽略；
 *      键盘自动重复产生的额外 down 边沿同样会命中（系统语义上每次重复
 *      都是一次 down）；
 *   3. 滚轮事件按 WHEEL_DELTA(=120) 为一个 notch 拆分：|delta|/120 条
 *      同时刻、同规范名的动作事件；不足一个 notch 的余数忽略
 *      （高分辩率触控板滚轮本次不产生动作）；
 *   4. 未在 keymap 中登记或登记项 action 为空的输入不产出动作
 *           （原始事件仍由 SessionManager 写入 spool，两者互不影响）；
 *   5. AddMarkerOnly 通道用于手动打点（6.6）：mouse_event/keyboard_event
 *      均为空串；空 label 由调用方默认成"标记点"。
 *
 * 非线程安全：除 keymap 快照设置外，事件消费仅由 SessionManager 的处理
 * 线程调用；keymap 快照只在录制开始（处理线程尚未消费）时设置一次。
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/plugin_config.h" // KeymapEntry
#include "input/input_event.h"
#include "session/session_types.h"

namespace grt {

class ActionMapper {
public:
	/// Win32 一个标准滚轮 notch 的 delta 量（WHEEL_DELTA，见 WinUser.h）
	static constexpr std::int32_t kWheelDeltaPerNotch = 120;

	ActionMapper() = default;

	/**
	 * @brief 设置本次会话使用的 keymap 浅拷贝快照。
	 *
	 * 键空间为 key_names.h 定义的规范名（键盘名 / 鼠标按键名 / 滚轮名）。
	 * 在录制开始、处理线程消费事件之前调用一次。
	 */
	void SetKeymap(std::map<std::string, KeymapEntry> keymap);

	/// 当前 keymap 快照（只读，便于诊断）
	const std::map<std::string, KeymapEntry>& Keymap() const { return keymap_; }

	/**
	 * @brief 处理一条原始输入事件，命中映射时向内部追加 ActionEvent。
	 * @param event       原始事件。
	 * @param sessionTime 会话时间（秒，相对 qpc0）；t<0 的事件不产出动作。
	 *
	 * MouseMove 永远忽略；Key/MouseButton 仅 down 处理；
	 * MouseWheel 按 notch 拆分多条同时刻事件。
	 */
	void ProcessEvent(const InputEvent& event, double sessionTime);

	/**
	 * @brief 追加一条手动打点动作（6.6 AddMarkerOnly 通道）。
	 * @param sessionTime 打点会话时间（秒）。
	 * @param label       打点标签；空串由本方法默认成"标记点"。
	 */
	void AddMarkerOnly(double sessionTime, const std::string& label);

	/// 已生成的动作事件（按会话时间非降序，处理线程单线程追加）
	const std::vector<ActionEvent>& Events() const { return events_; }

	/// 清空状态与 keymap，供同实例复用到下一次录制
	void Reset();

private:
	/// 若规范名命中 keymap 且 action 非空，则追加一条动作事件；返回是否追加
	bool EmitIfMapped(double sessionTime, const std::string& name,
	                  const std::string& mouseEvent,
	                  const std::string& keyboardEvent);

	std::map<std::string, KeymapEntry> keymap_;
	std::vector<ActionEvent> events_;
};

} // namespace grt
