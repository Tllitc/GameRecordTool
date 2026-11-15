/**
 * @file plugin-main.cpp
 * @brief GameRecordTool 插件入口（OBS Studio 32.2.2 前端插件）。
 *
 * 当前职责：
 *   - obs_module_load：最先初始化全局配置，随后输出加载日志并注册前端事件回调；
 *   - 前端事件回调：
 *       FINISHED_LOADING：启动共享 RawInputService 与 VideoTimeCalibrator；
 *       RECORDING_STARTED / RECORDING_STOPPED：转发给 SessionManager，
 *         由其处理线程上的条件变量同步开始/停止流程；
 *   - obs_module_unload：先移除前端事件回调，再确保录制会话停止
 *       （35s 超时保护，覆盖 30s STOPPED 等待），随后停止校准器与
 *       RawInputService，最后保存配置。
 * 不使用 OBS 默认 locale 宏（OBS_MODULE_USE_DEFAULT_LOCALE），无外部数据依赖。
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include "core/log.h"
#include "core/plugin_config.h"
#include "input/global_hotkey_manager.h"
#include "session/session_manager.h"
#include "ui/dock_controller.h"
#include "ui/overlay_controller.h"

// 声明本模块为 OBS 插件模块（展开 obs_module_set_pointer / obs_current_module /
// obs_module_ver 等模块基础符号）
OBS_DECLARE_MODULE()

namespace {

/**
 * @brief 标记前端事件回调是否已成功注册。
 *        卸载时据此安全移除回调，避免重复移除或对未注册回调操作。
 */
bool g_frontendCallbackRegistered = false;

/**
 * @brief OBS 前端事件回调。
 * @param event        前端事件类型。
 * @param private_data 注册时透传的用户数据，本插件不使用。
 */
void frontend_event_callback(enum obs_frontend_event event, void * /*private_data*/)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		GRT_LOG_INFO("OBS 前端加载完成");
		// 启动共享采集 / 校准服务：录制会话直接复用，失败仅告警，
		// SessionManager 在开始录制时会再补启动 RawInputService。
		grt::SessionManager& session = grt::SessionManager::Instance();
		if (!session.raw_input_service().Start()) {
			GRT_LOG_WARN("Raw Input 服务在前端加载完成时启动失败，"
			             "将在开始录制时重试");
		}
		if (!session.calibrator().Start()) {
			GRT_LOG_WARN("视频时间校准器启动失败，"
			             "本次会话事件时间将直接使用 qpc-qpc0");
		}
		// 创建并注册“游戏录制工具”Dock 主界面（必须在前端加载完成后，
		// 其内部会 InitOverlay，须先于全局热键启动，确保悬浮窗热键可用）
		grt::ui::RegisterDock();

		// 全局热键（开始/停止/打点/悬浮窗开关）：OBS 32.2.2 前端 API
		// 未导出插件热键接口，故基于只读 Raw Input 自建匹配（不抢焦点）
		grt::GlobalHotkeyManager& hotkeys = grt::GlobalHotkeyManager::Instance();
		hotkeys.SetCallback([](grt::GlobalHotkeyManager::Action action) {
			grt::SessionManager& sm = grt::SessionManager::Instance();
			switch (action) {
			case grt::GlobalHotkeyManager::Action::Start: {
				// 与 GUI 按钮同一入口：无显式窗口时由会话层回退前台窗口；
				// 鼠标加速开启等准入失败原因经日志总线同步到悬浮窗
				const std::string error = sm.Start();
				if (!error.empty()) {
					GRT_LOG_WARN("热键开始录制被拒绝：%s", error.c_str());
				}
				break;
			}
			case grt::GlobalHotkeyManager::Action::Stop: {
				const std::string error = sm.Stop();
				if (!error.empty()) {
					GRT_LOG_WARN("热键停止录制未生效：%s", error.c_str());
				}
				break;
			}
			case grt::GlobalHotkeyManager::Action::Marker: {
				const std::string error = sm.AddManualMarker("标记点");
				if (!error.empty()) {
					GRT_LOG_WARN("热键打点未生效：%s", error.c_str());
				}
				break;
			}
			case grt::GlobalHotkeyManager::Action::OverlayToggle:
				// 内部排队到 Qt 主线程，热键线程不触碰窗口，游戏不失焦
				grt::ui::ToggleOverlayFromAnyThread();
				break;
			}
		});
		if (!hotkeys.Start()) {
			GRT_LOG_WARN("全局热键服务启动失败，热键功能不可用");
		}
		break;
	}
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		// 任意线程转发（此回调在 OBS 主线程）；SessionManager 内部加锁
		grt::SessionManager::Instance().NotifyRecordingStarted();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		grt::SessionManager::Instance().NotifyRecordingStopped();
		break;
	default:
		break;
	}
}

} // 匿名命名空间

/**
 * @brief 模块加载入口，由 OBS 主程序在加载插件 DLL 时调用。
 * @return 始终返回 true，表示模块加载成功。
 */
MODULE_EXPORT bool obs_module_load(void)
{
	// 最先初始化全局配置（确定 obs_module_config_path 并从磁盘加载）
	grt::PluginConfig::Instance().Initialize();

	GRT_LOG_INFO("插件加载成功，当前为工程骨架版本");

	obs_frontend_add_event_callback(frontend_event_callback, nullptr);
	g_frontendCallbackRegistered = true;

	return true;
}

/**
 * @brief 模块卸载入口，由 OBS 主程序在卸载插件 DLL 前调用。
 *
 * 顺序要求：
 *   1. 先移除前端事件回调，避免卸载过程中再收到启停事件；
 *   2. 若录制仍在进行，同步执行一次停止并等待回到 Idle（35s 超时保护，
 *      覆盖 STOPPED 的 30s 等待）；
 *   3. 停止校准器与 RawInputService（二者在录制之间保持运行）；
 *   4. 最后保存配置。
 */
MODULE_EXPORT void obs_module_unload(void)
{
	if (g_frontendCallbackRegistered) {
		obs_frontend_remove_event_callback(frontend_event_callback, nullptr);
		g_frontendCallbackRegistered = false;
	}

	// 先移除 Dock 主界面（其析构会注销 SessionManager 回调与日志订阅）
	grt::ui::UnregisterDock();

	// 停止全局热键轮询线程与自有 Raw Input，避免卸载后线程/回调悬挂
	grt::GlobalHotkeyManager::Instance().Stop();

	grt::SessionManager& session = grt::SessionManager::Instance();

	// 录制中先 Stop 并等待收尾完成（35s：30s STOPPED 等待 + 余量）
	if (!session.EnsureStoppedForUnload(35000)) {
		GRT_LOG_WARN("卸载前录制会话未能在超时内完全收尾，仍继续卸载");
	}

	// 采集 / 校准服务停止（幂等，任意线程）
	session.calibrator().Stop();
	session.raw_input_service().Stop();

	// 卸载前落盘一次配置
	grt::PluginConfig::Instance().Save();

	GRT_LOG_INFO("插件已卸载");
}
