/**
 * @file session_manager.cpp
 * @brief 录制会话状态机实现（控制面 / 处理线程分离，见 session_manager.h）。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "session/session_manager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <obs.h>
#include <obs-frontend-api.h>

#include <windows.h>
#include <shellapi.h>

#include "capture/window_enumerator.h"
#include "core/clock.h"
#include "core/log.h"
#include "export/export_pipeline.h"
#include "input/mouse_accel_service.h"

namespace grt {

namespace {

/// 主线程任务超时（毫秒）：输出配置/场景准备/录制启停均派发主线程
constexpr DWORD kUiTaskTimeoutMs = 5000;

/// UTF-8 -> UTF-16（路径相关 Win32 API 使用宽串）
std::wstring Utf8ToWide(const std::string& utf8)
{
	if (utf8.empty()) {
		return std::wstring();
	}
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
	                                    static_cast<int>(utf8.size()),
	                                    nullptr, 0);
	std::wstring wide;
	if (len > 0) {
		wide.resize(static_cast<std::size_t>(len));
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
		                    static_cast<int>(utf8.size()), wide.data(), len);
	}
	return wide;
}

/// UTF-16 -> UTF-8
std::string WideToUtf8(const std::wstring& wide)
{
	if (wide.empty()) {
		return std::string();
	}
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
	                                    static_cast<int>(wide.size()),
	                                    nullptr, 0, nullptr, nullptr);
	std::string utf8;
	if (len > 0) {
		utf8.resize(static_cast<std::size_t>(len));
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
		                    static_cast<int>(wide.size()), utf8.data(), len,
		                    nullptr, nullptr);
	}
	return utf8;
}

/**
 * @brief 在 OBS 主线程同步执行任务（wait=false 投递 + 手动事件超时）。
 *
 * 与 game_capture_scene.cpp 同款方案：OBS_TASK_UI 路径忽略 wait 参数，
 * 无法附加超时且在主线程调用会死锁，因此自建事件同步。
 */
bool RunOnUiThread(const std::function<void()>& task)
{
	if (obs_in_task_thread(OBS_TASK_UI)) {
		task();
		return true;
	}

	struct Context {
		const std::function<void()>* task;
		HANDLE event;
	};

	HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!event) {
		GRT_LOG_ERROR("创建主线程同步事件失败：%s",
		              Win32ErrorMessage().c_str());
		return false;
	}

	Context ctx{&task, event};
	auto trampoline = [](void* param) {
		auto* c = static_cast<Context*>(param);
		(*c->task)();
		SetEvent(c->event);
	};

	obs_queue_task(OBS_TASK_UI, trampoline, &ctx, false);

	const DWORD waitResult = WaitForSingleObject(event, kUiTaskTimeoutMs);
	CloseHandle(event);

	if (waitResult != WAIT_OBJECT_0) {
		GRT_LOG_ERROR("等待主线程任务超时或失败（wait=%lu）",
		              static_cast<unsigned long>(waitResult));
		return false;
	}
	return true;
}

/// 把游戏名/标题中不能作为目录名的字符替换为 '_'，并去除首尾空白与点
std::string SanitizePathComponent(std::string name)
{
	static const char kInvalid[] = {'\\', '/', ':', '*', '?', '"', '<', '>', '|'};
	for (char& ch : name) {
		const unsigned char uch = static_cast<unsigned char>(ch);
		bool invalid = uch < 0x20;
		if (!invalid) {
			for (char bad : kInvalid) {
				if (ch == bad) {
					invalid = true;
					break;
				}
			}
		}
		if (invalid) {
			ch = '_';
		}
	}
	// 去除首尾空白与结尾点（Windows 目录名约束）
	while (!name.empty() &&
	       (name.back() == ' ' || name.back() == '.' ||
	        static_cast<unsigned char>(name.back()) < 0x20)) {
		name.pop_back();
	}
	std::size_t prefix = 0;
	while (prefix < name.size() &&
	       (name[prefix] == ' ' ||
	        static_cast<unsigned char>(name[prefix]) < 0x20)) {
		++prefix;
	}
	if (prefix > 0) {
		name.erase(0, prefix);
	}
	return name;
}

/// "YYYY-MM-DDTHH:mm:SSZ" -> "yyyyMMdd-HHmmss"
std::string CompactTimestamp(const std::string& isoUtc)
{
	if (isoUtc.size() < 19) {
		return std::string("unknown");
	}
	std::string out;
	out.reserve(15);
	for (std::size_t i = 0; i < 19; ++i) {
		const char ch = isoUtc[i];
		if (ch >= '0' && ch <= '9') {
			out.push_back(ch);
		}
	}
	// 收集到 14 位数字 yyyyMMddHHmmss，插入日期与时间之间的连字符
	if (out.size() >= 14) {
		return out.substr(0, 8) + "-" + out.substr(8, 6);
	}
	return std::string("unknown");
}

/// 生成 3 位零填充随机后缀（000-999）
std::string Random3Suffix()
{
	static std::mt19937 rng(static_cast<std::uint32_t>(
	    std::chrono::steady_clock::now().time_since_epoch().count()));
	char buf[4];
	std::snprintf(buf, sizeof(buf), "%03u",
	              static_cast<unsigned>(rng() % 1000));
	return std::string(buf);
}

} // namespace

SessionManager& SessionManager::Instance()
{
	static SessionManager instance;
	return instance;
}

SessionManager::SessionManager()
{
	running_.store(true);
	thread_ = std::thread(&SessionManager::ThreadMain, this);
}

SessionManager::~SessionManager()
{
	{
		std::lock_guard<std::mutex> lock(actionMutex_);
		running_.store(false);
		actionCv_.notify_all();
	}
	if (thread_.joinable()) {
		thread_.join();
	}
}

// ---- 共享服务 ----------------------------------------------------------------

// raw_input_service() / calibrator() 为头文件内联访问器

// ---- 控制面 ------------------------------------------------------------------

std::string SessionManager::Start(std::optional<WindowInfo> target)
{
	if (!MouseAccelService::CanStartRecording()) {
		return kMouseAccelError;
	}

	const RecordingSettings settings = PluginConfig::Instance().GetRecordingSettings();
	if (settings.outputDir.empty()) {
		return std::string("输出目录未设置");
	}

	const SessionState state = state_.load();
	if (state != SessionState::Idle) {
		return std::string("当前已有录制会话进行中");
	}

	{
		std::lock_guard<std::mutex> lock(actionMutex_);
		// 二次检查，避免与处理线程上正在切换的状态竞争
		if (state_.load() != SessionState::Idle) {
			return std::string("当前已有录制会话进行中");
		}
		ControlAction action;
		action.kind = ControlAction::Kind::Start;
		action.target = std::move(target);
		actions_.push_back(std::move(action));
		actionCv_.notify_all();
	}
	return std::string();
}

std::string SessionManager::Stop()
{
	const SessionState state = state_.load();
	if (state == SessionState::Idle) {
		return std::string("当前没有进行中的录制");
	}
	if (state == SessionState::Finalizing) {
		return std::string(); // 已在停止流程，视为幂等成功
	}

	{
		std::lock_guard<std::mutex> lock(actionMutex_);
		actions_.push_back(ControlAction{ControlAction::Kind::Stop,
		                                std::string(), 0.0});
		actionCv_.notify_all();
	}
	return std::string();
}

bool SessionManager::IsBusy() const
{
	const SessionState state = state_.load();
	return state != SessionState::Idle;
}

std::optional<SessionManager::RecordingInfo>
SessionManager::GetRecordingInfo() const
{
	std::lock_guard<std::mutex> lock(infoMutex_);
	return recordingInfo_;
}

std::string SessionManager::AddManualMarker(const std::string& label)
{
	const SessionState state = state_.load();
	if (state != SessionState::Recording) {
		return std::string("录制未在进行中");
	}
	{
		std::lock_guard<std::mutex> lock(actionMutex_);
		actions_.push_back(ControlAction{ControlAction::Kind::Marker,
		                                label, Clock::NowSeconds()});
		actionCv_.notify_all();
	}
	return std::string();
}

void SessionManager::SetStateChangedCallback(StateChangedCallback callback)
{
	std::lock_guard<std::mutex> lock(actionMutex_);
	stateChangedCb_ = std::move(callback);
}

void SessionManager::SetElapsedTickCallback(ElapsedTickCallback callback)
{
	std::lock_guard<std::mutex> lock(actionMutex_);
	elapsedTickCb_ = std::move(callback);
}

std::uint64_t SessionManager::SubscribeLog(LogCallback callback)
{
	return LogBus::Instance().Subscribe(std::move(callback));
}

void SessionManager::NotifyRecordingStarted()
{
	{
		std::lock_guard<std::mutex> lock(eventMutex_);
		frontendStarted_ = true;
	}
	startedCv_.notify_all();
}

void SessionManager::NotifyRecordingStopped()
{
	{
		std::lock_guard<std::mutex> lock(eventMutex_);
		frontendStopped_ = true;
	}
	stoppedCv_.notify_all();
}

bool SessionManager::EnsureStoppedForUnload(DWORD timeoutMs)
{
	if (state_.load() == SessionState::Idle) {
		return true;
	}
	// 处理线程不在主线程，Stop 只投递动作；此处直接等待状态回 Idle
	Stop();

	const auto deadline = std::chrono::steady_clock::now() +
	                      std::chrono::milliseconds(timeoutMs);
	while (state_.load() != SessionState::Idle) {
		if (std::chrono::steady_clock::now() >= deadline) {
			GRT_LOG_ERROR("卸载前等待录制停止超时（%lu ms）",
			              static_cast<unsigned long>(timeoutMs));
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return true;
}

// ---- 处理线程 ----------------------------------------------------------------

void SessionManager::ThreadMain()
{
	while (running_.load()) {
		// 1. 等待并取出控制动作（Idle 时阻塞；录制中最多等一个循环周期，
		//    以便持续 drain 事件）
		ControlAction action;
		bool hasAction = false;
		{
			std::unique_lock<std::mutex> lock(actionMutex_);
			if (actions_.empty()) {
				const bool recording =
				    state_.load() == SessionState::Recording;
				if (recording) {
					actionCv_.wait_for(
					    lock,
					    std::chrono::duration<double>(
					        kProcessIntervalSeconds));
				} else {
					actionCv_.wait(lock);
				}
			}
			if (!actions_.empty()) {
				action = std::move(actions_.front());
				actions_.pop_front();
				hasAction = true;
			}
		}

		if (hasAction) {
			if (action.kind == ControlAction::Kind::Start) {
				if (state_.load() == SessionState::Idle) {
					SetState(SessionState::Preparing);
					if (!BeginSession(action.target)) {
						RollbackAfterPrepareFailure();
						SetState(SessionState::Idle);
					} else {
						SetState(SessionState::Recording);
					}
				}
			} else if (action.kind == ControlAction::Kind::Stop) {
				if (state_.load() == SessionState::Recording) {
					SetState(SessionState::Finalizing);
					FinishSession();
					SetState(SessionState::Idle);
				}
			} else if (action.kind == ControlAction::Kind::Marker) {
				if (state_.load() == SessionState::Recording && action_) {
					action_->AddMarkerOnly(ToSessionTime(action.qpc),
					                       action.label);
				}
			}
		}

		// 2. 录制态周期性 drain（Start/Stop 当轮的 drain 已在各自流程内做过，
		//    此处再跑一轮也幂等：无事件即空转）
		if (state_.load() == SessionState::Recording) {
			ProcessIteration(Clock::NowSeconds());
		}
	}
}

void SessionManager::ProcessIteration(double nowQpc)
{
	// 控制面在本循环窗口内追加的打点优先消费，保证与事件流同序
	DrainControlActions();

	std::vector<InputEvent> events;
	raw_input_service().Drain(rawInputSubId_, events);

	const double flushQpc = nowQpc;
	for (const InputEvent& event : events) {
		const double sessionTime = ToSessionTime(event.t);
		// 所有原始事件（含 t<0）实时落 spool，双写会话时间与原始 QPC
		if (spool_.IsOpen()) {
			spool_.WriteEvent(event, sessionTime);
			++artifacts_.rawEventCount;
		}
		if (sessionTime < 0.0) {
			// 录制起点之前的事件不进入任何派生数据（仍已进入 spool）
			++artifacts_.negativeTimeDropped;
			continue;
		}
		if (movement_) {
			movement_->ProcessEvent(event, sessionTime);
		}
		if (looking_) {
			looking_->ProcessEvent(event, sessionTime);
		}
		if (action_) {
			action_->ProcessEvent(event, sessionTime);
		}
	}
	if (looking_) {
		looking_->FlushReady();
	}
	spool_.MaybeTimedFlush(flushQpc);

	EmitElapsedTick(nowQpc);
}

void SessionManager::DrainControlActions()
{
	for (;;) {
		ControlAction action;
		{
			std::lock_guard<std::mutex> lock(actionMutex_);
			if (actions_.empty()) {
				return;
			}
			action = std::move(actions_.front());
			actions_.pop_front();
		}

		if (action.kind == ControlAction::Kind::Marker && action_) {
			action_->AddMarkerOnly(ToSessionTime(action.qpc), action.label);
		} else if (action.kind == ControlAction::Kind::Stop) {
			// 录制循环中收到停止：置 Finalizing，由外层下一轮处理收尾。
			// 这里直接内联收尾，保证停止请求的响应延迟不超过一个循环。
			if (state_.load() == SessionState::Recording) {
				SetState(SessionState::Finalizing);
				FinishSession();
				SetState(SessionState::Idle);
				return;
			}
		} else if (action.kind == ControlAction::Kind::Start) {
			// 录制中收到 Start 属于准入已拒绝的竞态残留，忽略
		}
	}
}

// ---- 开始流程（处理线程）------------------------------------------------------

bool SessionManager::BeginSession(const std::optional<WindowInfo>& requestedTarget)
{
	PluginConfig& config = PluginConfig::Instance();
	const RecordingSettings settings = config.GetRecordingSettings();

	// 一开始即清空上一次会话的上下文/工件：后续任一步失败回滚时，
	// context_ 只会反映本次会话（特别是本次新建的会话目录）。
	context_ = SessionContext{};
	artifacts_ = SessionArtifacts{};

	// —— 1. 准入：鼠标加速（控制面已查一次，这里在处理线程复查）——
	if (!MouseAccelService::CanStartRecording()) {
		GRT_LOG_ERROR("开始录制被拒绝：鼠标加速仍开启");
		return false;
	}

	// —— 2. 确定录制目标 ——
	// GUI 显式选定窗口时严格采集该窗口（句柄已失效则失败，提示重选）；
	// 未指定（全局热键入口）时回退为开始瞬间的前台窗口。
	const std::vector<WindowInfo> windows = WindowEnumerator::Enumerate();
	WindowInfo resolvedTarget;
	if (requestedTarget.has_value()) {
		if (requestedTarget->hwnd == nullptr ||
		    !IsWindow(requestedTarget->hwnd)) {
			GRT_LOG_ERROR("选定的录制窗口已关闭，请重新选择窗口后再开始");
			return false;
		}
		const WindowInfo* match = nullptr;
		for (const WindowInfo& info : windows) {
			if (info.hwnd == requestedTarget->hwnd) {
				match = &info;
				break;
			}
		}
		if (match == nullptr) {
			GRT_LOG_ERROR("选定窗口当前不可采集（可能已最小化或被关闭），"
			              "请重新选择窗口后再开始");
			return false;
		}
		resolvedTarget = *match;
	} else {
		const std::string foregroundExe =
		    WindowEnumerator::CurrentForegroundExe();
		if (foregroundExe.empty()) {
			GRT_LOG_ERROR("无法确定当前前台窗口对应的进程，开始录制中止");
			return false;
		}
		const WindowInfo* match = nullptr;
		for (const WindowInfo& info : windows) {
			if (WideToUtf8(info.exeName) == foregroundExe) {
				match = &info;
				break;
			}
		}
		if (match == nullptr) {
			GRT_LOG_ERROR("前台进程 %s 未枚举到可采集窗口",
			              foregroundExe.c_str());
			return false;
		}
		resolvedTarget = *match;
	}
	const WindowInfo* target = &resolvedTarget;
	const std::string targetExe = WideToUtf8(target->exeName);

	// —— 3. 游戏档案 / keymap 浅拷贝快照 / 移动键 ——
	const std::optional<GameProfile> profile = config.GetGameProfile(targetExe);
	keymapSnapshot_.clear();
	if (profile.has_value()) {
		keymapSnapshot_ = profile->keymap;
	}

	std::array<std::string, 4> movementKeys = config.GetMovementKeys();
	if (profile.has_value() && profile->movementKeys.has_value()) {
		movementKeys = *profile->movementKeys;
	}
	// 配置数组顺序为 W,A,S,D；构造参数顺序为 forward,back,left,right
	// 即 [0],[2],[1],[3]。
	const std::string& forwardName = movementKeys[0];
	const std::string& leftName = movementKeys[1];
	const std::string& backName = movementKeys[2];
	const std::string& rightName = movementKeys[3];

	// —— 4. 会话目录 ——
	const std::string startedAtUtc = Clock::WallTimeUtcIso8601();
	std::string safeGameName;
	if (profile.has_value() && !profile->title.empty()) {
		safeGameName = SanitizePathComponent(profile->title);
	}
	if (safeGameName.empty()) {
		safeGameName = SanitizePathComponent(targetExe);
	}
	if (safeGameName.empty()) {
		safeGameName = "game";
	}
	const std::string sessionDir =
	    BuildSessionDirectory(settings.outputDir, safeGameName, startedAtUtc);
	if (sessionDir.empty()) {
		GRT_LOG_ERROR("无法创建会话目录，开始录制中止");
		return false;
	}
	const std::string sessionBase =
	    sessionDir.substr(sessionDir.find_last_of("\\/") + 1);
	// 目录一旦创建成功即记入上下文：后续步骤（含 STARTED 等待）失败时，
	// 回滚流程据此删除本次新建的目录。
	context_.sessionDir = sessionDir;
	context_.sessionBase = sessionBase;
	context_.startedAtUtc = startedAtUtc;

	// —— 5. 输出配置（主线程）：plan 先填帧率，Apply 填充其余字段 ——
	RecordPlan plan;
	plan.fps = settings.fps > 0 ? settings.fps : 60;
	plan.width = target->clientWidth;
	plan.height = target->clientHeight;

	bool applyOk = false;
	RunOnUiThread([&]() {
		applyOk = output_.Apply(plan, sessionDir, sessionBase,
		                        settings.videoCodec, settings.encoder,
		                        settings.bitrateKbps, settings.crashSafe);
	});
	if (!applyOk) {
		GRT_LOG_ERROR("应用录制输出配置失败，开始录制中止");
		return false;
	}

	// —— 6. 采集场景准备（内部自行派发主线程）——
	// 视频重置前挂起 raw-video 校准回调，否则 obs_reset_video 会因
	// obs_video_active() 为真而返回 OBS_VIDEO_CURRENTLY_ACTIVE
	calibrator_.Suspend();
	const bool sceneOk = scene_.Prepare(*target, settings.captureCursor);
	calibrator_.Resume();
	if (!sceneOk) {
		GRT_LOG_ERROR("准备游戏采集场景失败，开始录制中止");
		return false;
	}
	// 场景/画布可能做了对齐，plan 以实际生效尺寸为准
	plan.width = scene_.ActualWidth();
	plan.height = scene_.ActualHeight();
	plan.below1080p = scene_.LastWasBelow1080p();

	// —— 7. 共享服务状态（raw/calibrator 由 plugin-main 负责启动；
	//       raw 未运行时本次尝试补启动，calibrator 未 Ready 仅告警）——
	if (!raw_input_service().IsRunning()) {
		if (!raw_input_service().Start()) {
			GRT_LOG_ERROR("Raw Input 采集服务启动失败，开始录制中止");
			return false;
		}
		GRT_LOG_WARN("Raw Input 服务在开始录制时才被补启动");
	}
	if (rawInputSubId_ == RawInputService::kInvalidSubscription) {
		rawInputSubId_ = raw_input_service().Subscribe();
	}
	calibrationReadyAtStart_ = calibrator_.Ready();
	if (!calibrationReadyAtStart_) {
		GRT_LOG_WARN("视频时钟校准尚未就绪（样本不足），"
		             "本次会话事件时间直接使用 qpc-qpc0");
	} else {
		GRT_LOG_DEBUG("视频时钟校准已就绪（样本=%zu）",
		              calibrator_.SampleCount());
	}

	// —— 8. 构造三个生成器（上下文/工件已在本流程开头清空）——
	movement_.emplace(forwardName, backName, leftName, rightName);
	looking_.emplace(LookingAggregator::kDefaultBucketSeconds);
	action_.emplace();
	action_->SetKeymap(keymapSnapshot_);

	// —— 9. 打开 spool（先于录制启动，确保起点附近事件不丢）——
	const std::string spoolPath = sessionDir + "\\" + sessionBase + "_raw.jsonl";
	if (!spool_.Open(spoolPath)) {
		GRT_LOG_ERROR("打开原始事件 spool 失败：%s", spoolPath.c_str());
		return false;
	}

	// —— 10. qpc0 紧贴 recording_start() 之前标记（统一时间锚点）——
	{
		std::lock_guard<std::mutex> lock(eventMutex_);
		frontendStarted_ = false;
		frontendStopped_ = false;
	}
	const double qpc0 = Clock::NowSeconds();
	calibrator_.MarkRecordingStart(qpc0);
	spool_.WriteSessionStart(qpc0, startedAtUtc);

	bool dispatchOk = RunOnUiThread([&]() {
		obs_frontend_recording_start();
	});
	if (!dispatchOk) {
		GRT_LOG_ERROR("录制启动命令未能派发到主线程");
		return false;
	}

	// —— 11. 等待前端 STARTED（10s），active() 兜底 ——
	if (!WaitForFrontendEvent(true, kStartEventTimeoutSeconds)) {
		bool active = false;
		RunOnUiThread([&]() {
			active = obs_frontend_recording_active();
		});
		if (!active) {
			GRT_LOG_ERROR("等待录制开始事件超时且录制未处于活动状态");
			return false;
		}
		GRT_LOG_WARN("未收到录制开始事件，但前端录制已活动，继续会话");
	}

	// —— 12. 固定会话上下文（目录/基名/开始时刻已在目录创建后先行写入）——
	context_.qpc0 = qpc0;
	context_.timeCalibrationReady = calibrationReadyAtStart_;
	context_.target.title = WideToUtf8(target->title);
	context_.target.exeName = WideToUtf8(target->exeName);
	context_.target.exePath = WideToUtf8(target->exePath);
	context_.target.clientWidth = target->clientWidth;
	context_.target.clientHeight = target->clientHeight;
	context_.plan = plan;

	// 公布只读录制方案快照（悬浮窗方案行展示用；独立锁，GUI 任意线程可读）
	{
		std::lock_guard<std::mutex> lock(infoMutex_);
		RecordingInfo info;
		info.width = plan.width;
		info.height = plan.height;
		info.fps = plan.fps;
		info.bitrateKbps = plan.bitrateKbps;
		info.codec = plan.codec;
		info.encoder = plan.encoderId;
		recordingInfo_ = std::move(info);
	}

	artifacts_.rawSpoolPath = spoolPath;

	lastElapsedTickQpc_ = 0.0;
	hasElapsedTickClock_ = false;

	GRT_LOG_INFO("录制会话已开始：%s（%dx%d@%d，校准%s）",
	             sessionBase.c_str(), plan.width, plan.height, plan.fps,
	             calibrationReadyAtStart_ ? "就绪" : "未就绪");

	// 立即 drain 一次，吃掉启动窗口内已缓存的事件
	ProcessIteration(Clock::NowSeconds());
	return true;
}

void SessionManager::RollbackAfterPrepareFailure()
{
	GRT_LOG_WARN("开始流程失败，执行回滚");

	// 录制可能已被前端启动但事件等待失败：尽力停止
	bool active = false;
	RunOnUiThread([&]() {
		active = obs_frontend_recording_active();
	});
	if (active) {
		RunOnUiThread([&]() {
			obs_frontend_recording_stop();
		});
		WaitForFrontendEvent(false, kStopEventTimeoutSeconds);
	}

	if (spool_.IsOpen()) {
		spool_.Close();
	}
	movement_.reset();
	looking_.reset();
	action_.reset();
	keymapSnapshot_.clear();

	if (scene_.IsPrepared()) {
		calibrator_.Suspend();
		scene_.Restore();
		calibrator_.Resume();
	}
	output_.Restore();

	// 删除可能已创建的空会话目录（spool 已关闭）
	if (!context_.sessionDir.empty()) {
		const std::wstring wideDir = Utf8ToWide(context_.sessionDir);
		// SHFileOperation 需要双零结尾
		std::wstring doubled = wideDir;
		doubled.push_back(L'\0');
		doubled.push_back(L'\0');
		SHFILEOPSTRUCTW op{};
		op.wFunc = FO_DELETE;
		op.pFrom = doubled.c_str();
		// 不允许撤销（会话目录是本次新建的临时目录），静默删除
		op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
		const int result = SHFileOperationW(&op);
		if (result != 0) {
			GRT_LOG_WARN("回滚删除会话目录失败（code=%d）：%s",
			             result, context_.sessionDir.c_str());
		}
	}

	if (rawInputSubId_ != RawInputService::kInvalidSubscription) {
		raw_input_service().Unsubscribe(rawInputSubId_);
		rawInputSubId_ = RawInputService::kInvalidSubscription;
	}

	context_ = SessionContext{};
	artifacts_ = SessionArtifacts{};
}

// ---- 停止流程（处理线程）------------------------------------------------------

void SessionManager::FinishSession()
{
	// —— 1. 先把队列中剩余事件（含晚到打点）全部处理完 ——
	ProcessIteration(Clock::NowSeconds());

	// —— 2. qpc1 取在 recording_stop() 之前 ——
	const double qpc1 = Clock::NowSeconds();
	context_.qpc1 = qpc1;

	// —— 3. flush spool 后再发停止命令，最大限度保住停止前事件 ——
	spool_.Flush();

	bool active = false;
	RunOnUiThread([&]() {
		active = obs_frontend_recording_active();
	});
	if (active) {
		const bool dispatchOk = RunOnUiThread([&]() {
			obs_frontend_recording_stop();
		});
		if (!dispatchOk) {
			GRT_LOG_ERROR("录制停止命令未能派发到主线程，仍继续收尾流程");
		}
		// —— 4. 等待前端 STOPPED（30s）——
		if (!WaitForFrontendEvent(false, kStopEventTimeoutSeconds)) {
			GRT_LOG_ERROR("等待录制停止事件超时（%ds），按已有数据收尾",
			              kStopEventTimeoutSeconds);
		}
	} else {
		GRT_LOG_WARN("收尾时前端录制已不处于活动状态");
	}

	// —— 5. 标记录制终点并确定最终时长 ——
	calibrator_.MarkRecordingEnd(qpc1);
	double duration = qpc1 - context_.qpc0;
	if (duration < 0.0 || !std::isfinite(duration)) {
		duration = 0.0;
	}
	// 注：calibrator_.LastDurationSeconds() 实现为 endQpc_-startQpc_，与
	// qpc1-qpc0 同源（均为 QPC 差值），不能反映视频容器时长，故不再参与
	// min() 修正（原写法恒取自身，是无效防御）；这里仅输出诊断日志。
	// 最终 duration_seconds 由导出阶段以实际视频容器时长为真值回填，并把
	// 事件时间轴对齐视频首帧（见 export_pipeline.cpp 第 2.1 步）。
	const double calibratorDuration = calibrator_.LastDurationSeconds();
	if (std::isfinite(calibratorDuration)) {
		GRT_LOG_DEBUG("QPC 墙钟时长=%.3fs，校准器时长=%.3fs（同源，仅诊断）",
		              duration, calibratorDuration);
	}
	context_.durationSeconds = duration;

	// —— 6. 闭合三类派生数据（不凭空新增零增量桶）——
	if (movement_) {
		movement_->Finish(duration);
		artifacts_.movementEvents = movement_->Events();
	}
	if (looking_) {
		looking_->Finish(duration);
		artifacts_.lookingEvents = looking_->Events();
		artifacts_.negativeTimeDropped +=
		    static_cast<std::uint64_t>(looking_->NegativeDropped());
	}
	if (action_) {
		artifacts_.actionEvents = action_->Events();
	}

	// —— 7. 关闭 spool ——
	spool_.Close();

	// —— 8. 录制文件路径（前端记录；remux 场景可能为中转文件路径）——
	bool gotVideoPath = false;
	std::string videoPath;
	RunOnUiThread([&]() {
		videoPath = OutputConfigurator::LastRecordingPath();
		gotVideoPath = true;
	});
	if (gotVideoPath) {
		artifacts_.videoPath = videoPath;
	}

	GRT_LOG_INFO("录制会话已收尾：时长=%.3fs，原始事件=%llu，"
	             "movement=%zu，looking=%zu，action=%zu，负时间丢弃=%llu",
	             duration,
	             static_cast<unsigned long long>(artifacts_.rawEventCount),
	             artifacts_.movementEvents.size(),
	             artifacts_.lookingEvents.size(),
	             artifacts_.actionEvents.size(),
	             static_cast<unsigned long long>(
	                 artifacts_.negativeTimeDropped));

	// —— 9. 调用 Task 7 导出桩（当前恒返回 not_implemented，不影响录制）——
	const ExportResult exportResult =
	    ExportSessionArtifacts(context_, artifacts_, keymapSnapshot_);
	if (!exportResult.ok) {
		GRT_LOG_WARN("工件导出未完成：%s", exportResult.error.c_str());
	}

	// —— 10. 恢复采集场景与输出配置（raw/calibrator 保持运行）——
	if (scene_.IsPrepared()) {
		calibrator_.Suspend();
		scene_.Restore();
		calibrator_.Resume();
	}
	output_.Restore();

	movement_.reset();
	looking_.reset();
	action_.reset();
	keymapSnapshot_.clear();

	if (rawInputSubId_ != RawInputService::kInvalidSubscription) {
		raw_input_service().Unsubscribe(rawInputSubId_);
		rawInputSubId_ = RawInputService::kInvalidSubscription;
	}

	// 回到 Idle：清空只读录制方案快照
	{
		std::lock_guard<std::mutex> lock(infoMutex_);
		recordingInfo_.reset();
	}
}

// ---- 辅助 --------------------------------------------------------------------

std::string SessionManager::BuildSessionDirectory(const std::string& outputDir,
                                                  const std::string& safeGameName,
                                                  const std::string& startedAtUtc) const
{
	const std::string stamp = CompactTimestamp(startedAtUtc);
	bool parentEnsured = false;
	// 首次用随机后缀；撞名后改用 001..009 序号后缀
	for (int attempt = 0; attempt < 10; ++attempt) {
		char suffixBuf[4];
		if (attempt == 0) {
			const std::string random = Random3Suffix();
			suffixBuf[0] = random[0];
			suffixBuf[1] = random[1];
			suffixBuf[2] = random[2];
		} else {
			std::snprintf(suffixBuf, sizeof(suffixBuf), "%03d", attempt);
		}
		const std::string dir = outputDir + "\\" + safeGameName + "_" +
		                        stamp + "_" + std::string(suffixBuf, 3);
		const std::wstring wide = Utf8ToWide(dir);
		if (CreateDirectoryW(wide.c_str(), nullptr)) {
			// 仅在确实由本次调用创建成功时返回，绝不复用既有目录
			return dir;
		}
		const DWORD error = GetLastError();
		if (error == ERROR_PATH_NOT_FOUND && !parentEnsured) {
			// 输出目录本身尚不存在时先补建父目录，再在同一轮重试一次
			parentEnsured = true;
			const std::wstring wideParent = Utf8ToWide(outputDir);
			if (CreateDirectoryW(wideParent.c_str(), nullptr) ||
			    GetLastError() == ERROR_ALREADY_EXISTS) {
				if (CreateDirectoryW(wide.c_str(), nullptr)) {
					return dir;
				}
				if (GetLastError() != ERROR_ALREADY_EXISTS) {
					GRT_LOG_ERROR("创建会话目录失败：%s（%s）",
					              dir.c_str(),
					              Win32ErrorMessage().c_str());
					return std::string();
				}
			}
			// 父目录补建失败或目录仍冲突，落入后续序号尝试
		} else if (error != ERROR_ALREADY_EXISTS) {
			GRT_LOG_ERROR("创建会话目录失败：%s（%s）", dir.c_str(),
			              Win32ErrorMessage().c_str());
			return std::string();
		}
		// ERROR_ALREADY_EXISTS：换下一个序号后缀继续尝试
	}
	GRT_LOG_ERROR("创建会话目录连续撞名，已放弃：%s\\%s_%s",
	              outputDir.c_str(), safeGameName.c_str(), stamp.c_str());
	return std::string();
}

bool SessionManager::WaitForFrontendEvent(bool started, int timeoutSeconds)
{
	std::unique_lock<std::mutex> lock(eventMutex_);
	if (started) {
		startedCv_.wait_for(lock, std::chrono::seconds(timeoutSeconds),
		                    [this] { return frontendStarted_; });
		return frontendStarted_;
	}
	stoppedCv_.wait_for(lock, std::chrono::seconds(timeoutSeconds),
	                    [this] { return frontendStopped_; });
	return frontendStopped_;
}

void SessionManager::SetState(SessionState newState)
{
	state_.store(newState);
	StateChangedCallback cb;
	{
		std::lock_guard<std::mutex> lock(actionMutex_);
		cb = stateChangedCb_;
	}
	if (cb) {
		// 在处理线程触发；GUI 回调内必须自行 QueuedConnection 切线程
		cb(newState);
	}
}

void SessionManager::EmitElapsedTick(double nowQpc)
{
	if (!hasElapsedTickClock_) {
		hasElapsedTickClock_ = true;
		lastElapsedTickQpc_ = nowQpc;
		return;
	}
	if (nowQpc - lastElapsedTickQpc_ < kElapsedTickIntervalSeconds) {
		return;
	}
	lastElapsedTickQpc_ = nowQpc;

	const double elapsed = ToSessionTime(nowQpc);
	ElapsedTickCallback cb;
	{
		std::lock_guard<std::mutex> lock(actionMutex_);
		cb = elapsedTickCb_;
	}
	if (cb && elapsed >= 0.0) {
		cb(elapsed);
	}
}

double SessionManager::ToSessionTime(double qpc) const
{
	// 统一锚点：Ready 时走校准器（其 ToSessionTime 实现即 qpc-startQpc），
	// 未 Ready 时直接 qpc-qpc0；两分支数值同源，Finalize 不重算。
	if (calibrationReadyAtStart_) {
		const double mapped = calibrator_.ToSessionTime(qpc);
		if (std::isfinite(mapped)) {
			return mapped;
		}
	}
	return qpc - context_.qpc0;
}

} // namespace grt
