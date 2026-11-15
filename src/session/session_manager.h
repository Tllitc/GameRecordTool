/**
 * @file session_manager.h
 * @brief 录制会话状态机：Start/Stop 控制面 + 125Hz 处理线程 + 三类事件生成。
 *
 * 状态机：
 *
 *      Start()                停止流程完成
 *   Idle -------> Preparing -> Recording -> Finalizing -> Idle
 *    ^                            |  ^
 *    |________ Stop() ___________|  |
 *              （Recording 中可请求停止）
 *
 *   - Idle：无会话；Start 仅在 Idle 准入；
 *   - Preparing：在处理线程上完成目录/配置/场景准备与录制启动确认；
 *   - Recording：处理线程以 8ms（约 125Hz）周期 drain 原始事件，
 *     写 spool、喂 movement/looking/action 三个生成器；
 *   - Finalizing：flush spool、停止录制、等待 STOPPED、闭合生成器、
 *     调用 Task 7 导出桩、恢复采集/输出配置，随后回到 Idle。
 *
 * 线程模型（重要）：
 *   - 控制面（Start/Stop/AddManualMarker/状态查询）可从任意线程调用，
 *     经 mutex/原子与线程安全动作队列与处理线程交互；
 *   - 处理线程独占 SpoolWriter 与三个生成器，所有 libobs 前端调用经
 *     obs_queue_task(OBS_TASK_UI, wait=false) + 手动事件超时派发到主线程；
 *   - 前端 STARTED/STOPPED 事件由 plugin-main 转发到 NotifyRecording*，
 *     处理线程用条件变量等待（STARTED 10s / STOPPED 30s）。
 *
 * GUI 回调（OnStateChanged / OnElapsedTick / OnLog）均在【处理线程】上
 * 同步触发，GUI 侧必须自行通过 Qt QueuedConnection 等方式切回 UI 线程，
 * 不得在回调内阻塞。
 *
 * 时间基准（统一锚点）：
 *   qpc0 在调用 obs_frontend_recording_start() 之前于处理线程采样，
 *   同时作为 VideoTimeCalibrator 的 MarkRecordingStart。所有事件的会话时间
 *   统一为 qpc - qpc0（Ready 时走 calibrator.ToSessionTime，其实现同为
 *   qpc-startQpc；未 Ready 时直接相减，两分支数值同源），Finalize 不重算。
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "capture/game_capture_scene.h"
#include "capture/output_configurator.h"
#include "core/log_bus.h"
#include "core/plugin_config.h"
#include "input/raw_input_service.h"
#include "session/action_mapper.h"
#include "session/looking_aggregator.h"
#include "session/movement_tracker.h"
#include "session/session_types.h"
#include "session/spool_writer.h"
#include "video/video_time_calibrator.h"

namespace grt {

/// 录制会话状态
enum class SessionState {
	Idle,       ///< 无会话
	Preparing,  ///< 开始流程进行中
	Recording,  ///< 录制中
	Finalizing, ///< 停止/收尾进行中
};

class SessionManager {
public:
	/// 处理线程循环周期（秒）：8ms ≈ 125Hz
	static constexpr double kProcessIntervalSeconds = 0.008;
	/// 已录时长通知周期（秒）：4Hz
	static constexpr double kElapsedTickIntervalSeconds = 0.25;
	/// 等待前端"录制已开始"事件的超时（秒）
	static constexpr int kStartEventTimeoutSeconds = 10;
	/// 等待前端"录制已停止"事件的超时（秒）
	static constexpr int kStopEventTimeoutSeconds = 30;
	/// 鼠标加速准入失败时返回的错误信息
	static constexpr const char* kMouseAccelError =
	    "鼠标加速（增强指针精度）仍开启，请先一键关闭";

	using StateChangedCallback = std::function<void(SessionState)>;
	using ElapsedTickCallback = std::function<void(double elapsedSeconds)>;
	using LogCallback = LogBus::LogCallback;

	/// 当前/最近一次录制方案的只读快照（供悬浮窗等 GUI 展示，无会话时为空）
	struct RecordingInfo {
		int width = 0;          ///< 视频宽（像素）
		int height = 0;         ///< 视频高（像素）
		int fps = 0;            ///< 帧率
		int bitrateKbps = 0;    ///< 码率（kbps）
		std::string codec;      ///< 编码格式："h264" / "hevc"
		std::string encoder;    ///< 编码器真实 id（如 "ffmpeg_nvenc"）
	};

	/// 进程内单例（plugin-main 负责服务启动/停止与事件转发）
	static SessionManager& Instance();

	SessionManager(const SessionManager&) = delete;
	SessionManager& operator=(const SessionManager&) = delete;

	// ---- 共享服务（Task 10 keymap 编辑器等复用同一 RawInputService）----

	/// 共享 Raw Input 采集服务引用（进程内单例，plugin-main 负责启停）
	RawInputService& raw_input_service() { return RawInputService::Shared(); }
	/// 共享视频时间校准器引用
	VideoTimeCalibrator& calibrator() { return calibrator_; }

	// ---- 控制面（任意线程调用）-------------------------------------------

	/**
	 * @brief 请求开始一次录制会话（异步：实际工作在处理线程完成）。
	 * @param target GUI 选定的采集目标窗口；为空时回退为开始瞬间的前台
	 *               窗口（供全局热键等无显式选择的入口使用）。非空时
	 *               严格采集该窗口，若其已关闭则开始流程失败并提示重选。
	 * @return 空串表示请求已受理（最终状态以回调为准）；非空为同步准入
	 *         失败原因（非 Idle、鼠标加速开启、输出目录为空等）。
	 */
	std::string Start(std::optional<WindowInfo> target = std::nullopt);

	/**
	 * @brief 请求停止当前录制（异步幂等：录制中请求进入 Finalizing）。
	 * @return 空串表示请求已受理/无需停止；非空为当前状态不允许停止。
	 */
	std::string Stop();

	/// 当前状态（原子读取）
	SessionState State() const { return state_.load(); }

	/// 是否处于录制相关活动状态（Preparing/Recording/Finalizing）
	bool IsBusy() const;

	/**
	 * @brief 查询当前录制方案快照（BeginSession 方案确定后写入，
	 *        FinishSession 回到 Idle 时清空）。
	 * @return 有会话时返回方案信息；Idle 时为 std::nullopt。加锁拷贝，
	 *         可在任意线程调用；使用独立互斥锁，不与回调锁混用。
	 */
	std::optional<RecordingInfo> GetRecordingInfo() const;

	/**
	 * @brief 手动打点（6.6 AddMarkerOnly 通道）。
	 *
	 * 线程安全：把打点（标签 + 入队时刻 QPC）投递到处理线程，
	 * 由处理线程按事件流顺序生成 ActionEvent。空 label 默认成"标记点"。
	 * @return 空串表示已入队；非 Idle/Recording 之外返回状态错误。
	 */
	std::string AddManualMarker(const std::string& label);

	/// 注册状态变化回调（在处理线程触发；GUI 自行 QueuedConnection）
	void SetStateChangedCallback(StateChangedCallback callback);
	/// 注册已录时长回调（4Hz，处理线程触发）
	void SetElapsedTickCallback(ElapsedTickCallback callback);

	/**
	 * @brief 订阅日志总线（转发到 LogBus），返回订阅句柄。
	 *
	 * GUI 用此句柄在 OnLog 中接收日志；不使用时由 LogBus::Unsubscribe 退订。
	 */
	std::uint64_t SubscribeLog(LogCallback callback);

	// ---- 前端事件转发（plugin-main 在事件回调中调用，任意线程）----------

	/// OBS_FRONTEND_EVENT_RECORDING_STARTED 转发
	void NotifyRecordingStarted();
	/// OBS_FRONTEND_EVENT_RECORDING_STOPPED 转发
	void NotifyRecordingStopped();

	// ---- 卸载支持（plugin-main 的 obs_module_unload 调用）----------------

	/**
	 * @brief 若录制中则同步执行一次停止并等待回到 Idle。
	 * @param timeoutMs 最长等待毫秒（建议 35000：覆盖 30s STOPPED 等待）。
	 * @return true 已回到 Idle；false 超时（调用方仍应继续卸载流程）。
	 */
	bool EnsureStoppedForUnload(DWORD timeoutMs);

private:
	SessionManager();
	~SessionManager();

	/// 处理线程入口
	void ThreadMain();
	/// 录制态主循环的单次迭代（drain/生成器/spool flush/tick）
	void ProcessIteration(double nowQpc);

	// ---- 开始/停止流程（均在处理线程执行）----
	bool BeginSession(const std::optional<WindowInfo>& requestedTarget);
	void FinishSession();
	/// 开始流程任一步失败后的回滚
	void RollbackAfterPrepareFailure();
	/// 生成会话目录 <output_dir>/<安全游戏名>_<yyyyMMdd-HHmmss>_<3位序号>/
	std::string BuildSessionDirectory(const std::string& outputDir,
	                                  const std::string& safeGameName,
	                                  const std::string& startedAtUtc) const;
	/// 等待前端录制开始/停止事件（处理线程调用）
	bool WaitForFrontendEvent(bool started, int timeoutSeconds);
	/// 处理线程上派发并执行待执行控制动作（Start/Stop/打点）
	void DrainControlActions();

	/// 状态切换（处理线程调用，负责回调通知）
	void SetState(SessionState newState);
	/// 触发一次已录时长通知
	void EmitElapsedTick(double nowQpc);

	/// 计算事件会话时间：Ready 走校准器（数值同为 qpc-qpc0），否则直接相减
	double ToSessionTime(double qpc) const;

	// —— 共享服务（生命周期由 plugin-main 在 FINISHED_LOADING/unload 管理）——
	/// 当前会话在共享 RawInputService 上的订阅句柄（0=未订阅）
	RawInputService::SubscriptionId rawInputSubId_ =
	    RawInputService::kInvalidSubscription;
	VideoTimeCalibrator calibrator_;

	// —— 单次会话对象（处理线程独占）——
	OutputConfigurator output_;
	GameCaptureScene scene_;
	SpoolWriter spool_;
	// 三个生成器在 BeginSession 内按下发配置就地构造，FinishSession 后复位
	std::optional<MovementTracker> movement_;
	std::optional<LookingAggregator> looking_;
	std::optional<ActionMapper> action_;
	SessionContext context_;
	SessionArtifacts artifacts_;
	std::map<std::string, KeymapEntry> keymapSnapshot_;
	bool calibrationReadyAtStart_ = false;

	// —— 处理线程 ——
	std::thread thread_;
	std::atomic<bool> running_{false};

	// —— 状态 ——
	std::atomic<SessionState> state_{SessionState::Idle};

	// —— 当前录制方案只读快照（独立小锁：写入在处理线程，读取可在任意线程）——
	mutable std::mutex infoMutex_;
	std::optional<RecordingInfo> recordingInfo_;

	// —— 控制动作队列（Start/Stop/打点，处理线程按入队顺序执行）——
	struct ControlAction {
		enum class Kind { Start, Stop, Marker };
		Kind kind = Kind::Start;
		std::string label;
		double qpc = 0.0;
		// 仅 Start 使用：GUI 选定的目标窗口，空表示回退前台窗口
		std::optional<WindowInfo> target;
	};
	std::mutex actionMutex_;
	std::condition_variable actionCv_;
	std::deque<ControlAction> actions_;

	// —— 前端事件同步 ——
	std::mutex eventMutex_;
	std::condition_variable startedCv_;
	std::condition_variable stoppedCv_;
	bool frontendStarted_ = false;
	bool frontendStopped_ = false;

	// —— 已录时长 tick ——
	double lastElapsedTickQpc_ = 0.0;
	bool hasElapsedTickClock_ = false;

	// —— 回调（只在注册/处理线程触发时访问；注册发生在录制前）——
	StateChangedCallback stateChangedCb_;
	ElapsedTickCallback elapsedTickCb_;
};

} // namespace grt
