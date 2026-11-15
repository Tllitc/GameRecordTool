/**
 * @file global_hotkey_manager.cpp
 * @brief 基于 Raw Input 的全局热键匹配实现。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include "global_hotkey_manager.h"

namespace grt {

namespace {

/// 轮询周期：8ms ≈ 125Hz，保证热键响应延迟远低于 0.1s
constexpr auto kPollInterval = std::chrono::milliseconds(8);

/**
 * @brief 把可能的左/右通用修饰键归一化为通用 VK。
 *
 * Raw Input 对左右 Ctrl/Shift/Alt 分别给出 VK_LCONTROL/VK_RCONTROL
 * 等原始码；绑定里只使用通用 VK_CONTROL/VK_SHIFT/VK_MENU。
 */
uint16_t NormalizeModifier(uint16_t vk)
{
	switch (vk) {
	case VK_LCONTROL:
	case VK_RCONTROL:
		return VK_CONTROL;
	case VK_LSHIFT:
	case VK_RSHIFT:
		return VK_SHIFT;
	case VK_LMENU:
	case VK_RMENU:
		return VK_MENU;
	default:
		return vk;
	}
}

/// 通用修饰键 VK -> RegisterHotKey 风格修饰符位（与配置存储一致）
int ModifierBit(uint16_t normalizedVk)
{
	switch (normalizedVk) {
	case VK_CONTROL:
		return MOD_CONTROL;
	case VK_SHIFT:
		return MOD_SHIFT;
	case VK_MENU:
		return MOD_ALT;
	default:
		return 0;
	}
}

} // 匿名命名空间

GlobalHotkeyManager& GlobalHotkeyManager::Instance()
{
	static GlobalHotkeyManager instance;
	return instance;
}

GlobalHotkeyManager::GlobalHotkeyManager()
{
	bindings_ = PluginConfig::Instance().GetHotkeySettings();
}

GlobalHotkeyManager::~GlobalHotkeyManager()
{
	Stop();
}

bool GlobalHotkeyManager::Start()
{
	if (running_.load()) {
		return true;
	}
	// 复用进程内共享采集服务（多实例会因 RIDEV_INPUTSINK "后者覆盖"
	// 导致会话实例收不到 WM_INPUT）；此处只订阅，不独占服务
	RawInputService& service = RawInputService::Shared();
	if (!service.Start()) {
		return false;
	}
	rawInputSubId_ = service.Subscribe();
	running_.store(true);
	thread_ = std::thread(&GlobalHotkeyManager::ThreadMain, this);
	return true;
}

void GlobalHotkeyManager::Stop()
{
	if (!running_.exchange(false)) {
		return;
	}
	if (thread_.joinable()) {
		thread_.join();
	}
	// 只退订，不停止共享服务（录制会话等其他消费方仍在使用）
	if (rawInputSubId_ != RawInputService::kInvalidSubscription) {
		RawInputService::Shared().Unsubscribe(rawInputSubId_);
		rawInputSubId_ = RawInputService::kInvalidSubscription;
	}
	std::fill(std::begin(pressed_), std::end(pressed_), false);
	std::fill(std::begin(fired_), std::end(fired_), false);
}

void GlobalHotkeyManager::SetCallback(TriggeredCallback callback)
{
	std::lock_guard lock(callbackMutex_);
	callback_ = std::move(callback);
}

void GlobalHotkeyManager::ReloadBindings()
{
	std::lock_guard lock(bindingsMutex_);
	bindings_ = PluginConfig::Instance().GetHotkeySettings();
	// 绑定更换后清空触发去重状态，避免上一轮组合键残留
	std::fill(std::begin(fired_), std::end(fired_), false);
}

void GlobalHotkeyManager::ThreadMain()
{
	std::vector<InputEvent> events;
	while (running_.load()) {
		events.clear();
		RawInputService::Shared().Drain(rawInputSubId_, events);
		if (!events.empty()) {
			ProcessEvents(events);
		}
		std::this_thread::sleep_for(kPollInterval);
	}
}

void GlobalHotkeyManager::ProcessEvents(std::vector<InputEvent>& events)
{
	// 1) 维护按下状态（修饰键同时记录左/右原始码与通用码）
	for (const InputEvent& e : events) {
		if (e.type != InputType::Key || e.vkey == 0 || e.vkey >= 256) {
			continue;
		}
		pressed_[e.vkey] = e.key_down;
		const uint16_t normalized = NormalizeModifier(e.vkey);
		if (normalized != e.vkey) {
			pressed_[normalized] = e.key_down;
		}
	}

	// 当前实际按下的修饰符位掩码
	int currentMods = 0;
	if (pressed_[VK_CONTROL]) {
		currentMods |= MOD_CONTROL;
	}
	if (pressed_[VK_SHIFT]) {
		currentMods |= MOD_SHIFT;
	}
	if (pressed_[VK_MENU]) {
		currentMods |= MOD_ALT;
	}

	HotkeySettings bindings;
	{
		std::lock_guard lock(bindingsMutex_);
		bindings = bindings_;
	}

	// 2) 逐个动作匹配：触发键处于按下、所需修饰符齐全（允许额外修饰符）
	for (size_t i = 0; i < bindings.size(); ++i) {
		const HotkeyBinding& b = bindings[i];
		if (b.vkey <= 0 || b.vkey >= 256) {
			continue;
		}
		// 纯修饰键不允许作为触发键（否则按下修饰键组合的过程中会误触发）
		const uint16_t triggerVk = static_cast<uint16_t>(b.vkey);
		if (ModifierBit(NormalizeModifier(triggerVk)) != 0) {
			continue;
		}

		const bool modsSatisfied =
		    (currentMods & b.modifiers) == b.modifiers;
		const bool triggerDown = pressed_[static_cast<size_t>(b.vkey)];

		// 触发键或任一所需修饰符松开即复位，允许下一轮重新触发
		if (!triggerDown || !modsSatisfied) {
			fired_[i] = false;
			continue;
		}
		if (fired_[i]) {
			continue;
		}
		fired_[i] = true;

		TriggeredCallback cb;
		{
			std::lock_guard lock(callbackMutex_);
			cb = callback_;
		}
		if (cb) {
			cb(static_cast<Action>(i));
		}
	}
}

} // namespace grt
