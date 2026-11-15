/**
 * @file game_capture_scene.h
 * @brief 游戏采集场景与源的创建/复用，以及画布分辨率跟随。
 *
 * 职责：
 *   1. 确保存在名为 "GRT_录制场景" 的场景，并将其切为当前场景；
 *   2. 确保场景内存在名为 "GRT_游戏采集" 的采集源，按目标窗口规格
 *      更新设置（优先 game_capture，源 id 不可用时回退 window_capture）；
 *   3. 将画布基础/输出分辨率设置为游戏客户区尺寸并触发前端视频重置；
 *   4. Restore() 恢复先前的当前场景与分辨率配置。
 *
 * 【线程约定 —— 重要】
 *   几乎所有 libobs / obs-frontend-api 调用都必须在 OBS 主线程（UI 线程）
 *   执行（场景、源、obs_frontend_* 等均不是线程安全的）。本类的公开
 *   方法内部统一通过 obs_queue_task(OBS_TASK_UI, ...) 将工作派发至
 *   主线程，并使用 Windows 事件同步等待、带超时保护。
 *
 *   源码核实（OBS 32.2.2，libobs/obs.c 的 obs_queue_task 与
 *   frontend/OBSApp.cpp 的 ui_task_handler）：
 *     - OBS_TASK_UI 类型直接转交前端注册的处理器，后者用
 *       QMetaObject::invokeMethod(App(), "Exec", ...) 在主线程执行；
 *     - 该路径忽略 obs_queue_task 的 wait 参数（wait 仅对 graphics/audio
 *       等内部任务队列生效），wait=true 时前端用的是阻塞式 Qt 连接，
 *       无法附加超时；且若调用方本身就在主线程还会死锁。
 *   因此本类采用 wait=false 投递 + 任务内 SetEvent 的自建事件方案，
 *   WaitForSingleObject 带超时（默认 5 秒）；并用
 *   obs_in_task_thread(OBS_TASK_UI) 判断调用方已在主线程时直接执行。
 *
 *   例外：obs_frontend_recording_active() 仅读取原子变量，可任意线程调用，
 *   但为简化实现，Prepare/Restore 仍整体派发到主线程。
 *
 * 生命周期：单次 Prepare() 对应一次 Restore()；重复 Prepare() 不会重复
 * 保存“原值”，只刷新目标窗口/光标/分辨率设置。场景与源本身在 Restore()
 * 时保留（不删除），仅恢复场景切换与分辨率，避免破坏用户的场景集合。
 */

#pragma once

#include <cstdint>
#include <string>

#include "capture/window_enumerator.h"

namespace grt {

class GameCaptureScene {
public:
	/// 场景固定名称（窄串，libobs 内部为 UTF-8）
	static constexpr const char* kSceneName = "GRT_录制场景";
	/// 采集源固定名称
	static constexpr const char* kSourceName = "GRT_游戏采集";
	/// 首选源类型 id（win-capture 插件注册的钩子采集）
	static constexpr const char* kGameCaptureId = "game_capture";
	/// 回退源类型 id（win-capture 插件注册的窗口采集 WGC/BitBlt）
	static constexpr const char* kWindowCaptureId = "window_capture";
	/// 1080p 短边判定阈值
	static constexpr int k1080Height = 1080;

	/**
	 * @brief 准备采集场景。
	 *
	 * @param target         目标窗口（WindowEnumerator::Enumerate() 的结果项），
	 *                       用其 hwnd/尺寸与窗口规格串配置源。
	 * @param captureCursor  是否采集鼠标光标。
	 * @return 成功返回 true；超时、源两种 id 均不可创建或视频重置失败时 false。
	 *
	 * 成功后可通过 ActualWidth/ActualHeight、LastWasBelow1080p、
	 * UsedFallbackSource 查询本次结果。
	 */
	bool Prepare(const WindowInfo& target, bool captureCursor);

	/**
	 * @brief 恢复 Prepare 之前的当前场景与画布分辨率。
	 *
	 * 不删除插件创建的场景/源（便于下次复用，也避免误删用户手动加入的内容）。
	 * 未成功 Prepare 过时调用为空操作。
	 * @return 恢复动作成功返回 true（无内容可恢复时也返回 true）。
	 */
	bool Restore();

	/// 本次 Prepare 实际生效的画布宽度（像素）
	int ActualWidth() const { return actualWidth_; }
	/// 本次 Prepare 实际生效的画布高度（像素）
	int ActualHeight() const { return actualHeight_; }
	/// 最近一次 Prepare 的分辨率是否低于 1920x1080（短边 <1080）
	bool LastWasBelow1080p() const { return lastBelow1080p_; }
	/// 最近一次 Prepare 是否回退使用了 window_capture
	bool UsedFallbackSource() const { return usedFallback_; }
	/// 是否已成功 Prepare（决定 Restore 是否有内容可恢复）
	bool IsPrepared() const { return prepared_; }

private:
	// —— 以下字段仅在主线程任务内读写；Prepare/Restore 已串行化到主线程 ——
	bool prepared_ = false;
	bool usedFallback_ = false;
	bool lastBelow1080p_ = false;
	int actualWidth_ = 0;
	int actualHeight_ = 0;

	std::string previousSceneName_; ///< Prepare 前的当前场景名（可能为空）
	uint32_t previousBaseCX_ = 0;   ///< 原 [Video] BaseCX
	uint32_t previousBaseCY_ = 0;   ///< 原 [Video] BaseCY
	uint32_t previousOutputCX_ = 0; ///< 原 [Video] OutputCX
	uint32_t previousOutputCY_ = 0; ///< 原 [Video] OutputCY
	bool havePreviousVideo_ = false; ///< 是否成功记录过原分辨率
};

} // namespace grt
