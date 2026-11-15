/**
 * @file global_hotkey_manager.h
 * @brief 基于 Raw Input 的全局热键管理器（开始/停止/打点/悬浮窗开关）。
 *
 * 背景：OBS Studio 32.2.2 的 obs-frontend-api 未向插件导出任何热键
 * 注册接口（导出表中无 hotkey 相关符号）。本模块复用工程自身的
 * RawInputService（RIDEV_INPUTSINK 只读采集，不拦截、不注入、不抢
 * 焦点），在专用轮询线程上维护按键按下状态并做组合键边沿匹配，
 * 因此游戏全屏前台时同样可用，且按键消息照常送达游戏。
 *
 * 本模块只负责"检测"，命中后通过 TriggeredCallback 把动作交出去，
 * 不直接依赖 SessionManager / UI，避免输入层反向依赖上层。
 *
 * 绑定来自 PluginConfig（HotkeySettings，顺序固定），可通过
 * ReloadBindings() 在用户改键后立即生效。
 */

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "core/plugin_config.h"
#include "input/raw_input_service.h"

namespace grt {

class GlobalHotkeyManager {
public:
	/// 热键动作（整数值与 HotkeySettings 数组下标一一对应，勿随意调整）
	enum class Action {
		Start = 0,       ///< 开始录制（仍需通过鼠标加速准入）
		Stop = 1,        ///< 停止录制
		Marker = 2,      ///< 动作打点
		OverlayToggle = 3, ///< 悬浮窗显示/隐藏
	};

	using TriggeredCallback = std::function<void(Action action)>;

	/// 进程内单例
	static GlobalHotkeyManager& Instance();

	GlobalHotkeyManager(const GlobalHotkeyManager&) = delete;
	GlobalHotkeyManager& operator=(const GlobalHotkeyManager&) = delete;

	/**
	 * @brief 启动热键检测（订阅进程内共享的 RawInputService + 轮询线程）。
	 *
	 * 本模块订阅进程内共享的 RawInputService（多实例会因
	 * RIDEV_INPUTSINK "后者覆盖" 导致录制会话实例收不到 WM_INPUT），
	 * 只订阅事件流、绝不独占或停止共享服务。幂等，任意线程可调用。
	 * @return true 已运行或本次启动成功；false 为底层采集服务启动失败。
	 */
	bool Start();

	/// 停止轮询线程并退订共享采集服务（幂等；不停止共享服务本身）
	void Stop();

	/// 是否正在运行
	bool IsRunning() const { return running_.load(); }

	/**
	 * @brief 注册命中回调（在内部轮询线程触发，回调内不得阻塞；
	 *        涉及 GUI 的操作必须由接收方自行切到 Qt 主线程）。
	 */
	void SetCallback(TriggeredCallback callback);

	/**
	 * @brief 从 PluginConfig 重新读取四个热键绑定，立即生效。
	 *        未绑定（vkey==0）的动作不会触发。
	 */
	void ReloadBindings();

private:
	GlobalHotkeyManager();
	~GlobalHotkeyManager();

	/// 轮询线程入口：周期 drain 原始事件，更新按键状态并匹配组合键
	void ThreadMain();
	/// 用当前按下集合与最新一批事件做一次匹配，命中则触发回调
	void ProcessEvents(std::vector<InputEvent>& events);

	/// 共享 RawInputService 的订阅句柄（0=未订阅）
	RawInputService::SubscriptionId rawInputSubId_ =
	    RawInputService::kInvalidSubscription;
	std::thread thread_;
	std::atomic<bool> running_{false};

	std::mutex callbackMutex_;
	TriggeredCallback callback_;

	std::mutex bindingsMutex_;
	HotkeySettings bindings_;

	/// 当前按下的虚拟键码集合（含修饰键 VK_L*/VK_R* 原始码）
	bool pressed_[256] = {};
	/// 每个动作的组合键是否已就本次按下触发过（边沿去重，松开后复位）
	bool fired_[4] = {};
};

} // namespace grt
