/**
 * @file action_mapper.cpp
 * @brief 动作事件映射器实现（纯逻辑，见 action_mapper.h 的规则说明）。
 */

#include "session/action_mapper.h"

#include <cstdlib>
#include <utility>

#include "input/key_names.h"

namespace grt {

void ActionMapper::SetKeymap(std::map<std::string, KeymapEntry> keymap)
{
	keymap_ = std::move(keymap);
}

void ActionMapper::ProcessEvent(const InputEvent& event, double sessionTime)
{
	// 录制起点之前的事件不产生动作（原始 spool 仍由 SessionManager 记录）
	if (sessionTime < 0.0) {
		return;
	}

	switch (event.type) {
	case InputType::Key: {
		// 仅 down 边沿；up 忽略
		if (event.key_down) {
			const std::string name = KeyVkeyToName(event.vkey);
			EmitIfMapped(sessionTime, name, std::string(), name);
		}
		break;
	}

	case InputType::MouseButton:
		// 仅按下；x1/x2 等规范名由 MouseButtonToName 统一给出
		if (event.key_down) {
			const std::string name = MouseButtonToName(event.button);
			if (!name.empty()) {
				EmitIfMapped(sessionTime, name, name, std::string());
			}
		}
		break;

	case InputType::MouseWheel: {
		// 滚轮按 WHEEL_DELTA(120) 一个 notch 拆多条同时刻同名事件；
		// 余数不足一个 notch 时忽略（不四舍五入，避免虚触动作）。
		const std::string wheelName =
		    WheelEventName(event.horizontal_wheel, event.wheel_delta);
		if (wheelName.empty()) {
			break;
		}
		const std::int32_t notches =
		    static_cast<std::int32_t>(
		        std::abs(static_cast<int>(event.wheel_delta))) /
		    kWheelDeltaPerNotch;
		for (std::int32_t i = 0; i < notches; ++i) {
			EmitIfMapped(sessionTime, wheelName, wheelName, std::string());
		}
		break;
	}

	case InputType::MouseMove:
		// 鼠标移动不映射动作
		break;
	}
}

void ActionMapper::AddMarkerOnly(double sessionTime, const std::string& label)
{
	if (sessionTime < 0.0) {
		return;
	}
	ActionEvent event;
	event.timestamp = sessionTime;
	event.action = label.empty() ? std::string("标记点") : label;
	events_.push_back(std::move(event));
}

void ActionMapper::Reset()
{
	keymap_.clear();
	events_.clear();
}

bool ActionMapper::EmitIfMapped(double sessionTime, const std::string& name,
                                const std::string& mouseEvent,
                                const std::string& keyboardEvent)
{
	auto it = keymap_.find(name);
	if (it == keymap_.end() || it->second.action.empty()) {
		// 未登记或动作语义为空：不产出（原始事件仍已进入 spool）
		return false;
	}

	ActionEvent event;
	event.timestamp = sessionTime;
	event.mouse_event = mouseEvent;
	event.keyboard_event = keyboardEvent;
	event.action = it->second.action;
	events_.push_back(std::move(event));
	return true;
}

} // namespace grt
