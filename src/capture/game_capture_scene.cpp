/**
 * @file game_capture_scene.cpp
 * @brief 游戏采集场景与源的创建/复用，以及画布分辨率跟随实现。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/game_capture_scene.h"

#include <functional>

// 注意：不要包含 <obs-scene.h>。obs-dev 中该文件是 libobs 内部头
// （含 pthread_mutex_t 完整结构定义，依赖 util/threading.h）；插件所需的
// 场景公共 API（obs_scene_create / obs_scene_add / obs_sceneitem_* 等）
// 均以不透明类型声明在 <obs.h> 中。
#include <obs.h>
#include <obs-data.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <graphics/vec2.h>

#include "core/log.h"

namespace grt {

namespace {

/// 主线程任务超时（毫秒）
constexpr DWORD kUiTaskTimeoutMs = 5000;

/**
 * @brief 在 OBS 主线程（UI 线程）同步执行一个任务。
 *
 * 已在主线程时直接执行；否则 obs_queue_task(OBS_TASK_UI, ..., wait=false)
 * 投递，任务结束时置位 Windows 事件，调用方带超时等待。
 * 详见 game_capture_scene.h 的线程约定注释。
 *
 * @param task 要在主线程执行的工作，返回是否成功。
 * @return 任务执行结果；超时返回 false。
 */
bool RunOnUiThread(const std::function<bool()>& task)
{
	if (obs_in_task_thread(OBS_TASK_UI)) {
		return task();
	}

	struct Context {
		const std::function<bool()>* task;
		HANDLE event;
		bool result;
	};

	HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!event) {
		GRT_LOG_ERROR("创建主线程同步事件失败：%s",
		              Win32ErrorMessage().c_str());
		return false;
	}

	Context ctx{&task, event, false};

	auto trampoline = [](void* param) {
		auto* c = static_cast<Context*>(param);
		c->result = (*c->task)();
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
	return ctx.result;
}

/// 宽度向上对齐到 4 的倍数（与 OBS ResizeOutputSizeOfSource 一致）
int AlignWidth4(int width)
{
	return ((width + 3) / 4) * 4;
}

/// 高度向上对齐到 2 的倍数（与 OBS ResizeOutputSizeOfSource 一致）
int AlignHeight2(int height)
{
	return ((height + 1) / 2) * 2;
}

} // namespace

bool GameCaptureScene::Prepare(const WindowInfo& target, bool captureCursor)
{
	// 目标窗口与规格串在调用线程（任意线程）预先准备好，主线程内只做 libobs 调用。
	const std::string windowSpec = WindowEnumerator::MakeWindowMatchSpec(target);
	if (windowSpec.empty()) {
		GRT_LOG_ERROR("无法生成目标窗口规格串，Prepare 中止");
		return false;
	}

	const int clientWidth = target.clientWidth;
	const int clientHeight = target.clientHeight;

	const bool ok = RunOnUiThread([&]() -> bool {
		if (obs_frontend_recording_active()) {
			GRT_LOG_ERROR("当前正在录制，无法切换采集场景与分辨率");
			return false;
		}

		// —— 1. 记录原当前场景（仅首次 Prepare 时保存，重复 Prepare 不覆盖）——
		if (!prepared_) {
			obs_source_t* previousScene = obs_frontend_get_current_scene();
			if (previousScene) {
				const char* name = obs_source_get_name(previousScene);
				if (name) {
					previousSceneName_ = name;
				}
				obs_source_release(previousScene);
			}
		}

		// —— 2. 创建/复用场景 ——
		// obs_get_scene_by_name 与 obs_scene_create 均返回“持有一个引用”的
		// obs_scene_t；该引用通过释放其底层 source 来归还。
		obs_scene_t* scene = obs_get_scene_by_name(kSceneName);
		if (!scene) {
			scene = obs_scene_create(kSceneName);
			if (!scene) {
				GRT_LOG_ERROR("创建场景失败：%s", kSceneName);
				return false;
			}
			GRT_LOG_INFO("已创建场景：%s", kSceneName);
		}
		// obs_scene_get_source 不增加引用，仅用于前端 API 与最终释放。
		obs_source_t* sceneSource = obs_scene_get_source(scene);

		// —— 3. 创建/复用采集源（优先 game_capture，回退 window_capture）——
		// obs_get_source_by_name / obs_source_create 返回的源各持有一个引用。
		obs_source_t* source = obs_get_source_by_name(kSourceName);
		if (source) {
			const char* existingId = obs_source_get_id(source);
			if (existingId) {
				usedFallback_ = (std::string(existingId) == kWindowCaptureId);
			}
		} else {
			source = obs_source_create(kGameCaptureId, kSourceName, nullptr, nullptr);
			if (source) {
				usedFallback_ = false;
				GRT_LOG_INFO("已创建游戏采集源：%s（%s）",
				             kSourceName, kGameCaptureId);
			} else {
				GRT_LOG_WARN("game_capture 源不可用，回退到 window_capture");
				source = obs_source_create(kWindowCaptureId, kSourceName,
				                           nullptr, nullptr);
				usedFallback_ = true;
				if (!source) {
					GRT_LOG_ERROR("game_capture 与 window_capture 均不可用");
					obs_source_release(sceneSource);
					return false;
				}
				GRT_LOG_INFO("已创建窗口采集源：%s（%s）",
				             kSourceName, kWindowCaptureId);
			}
		}

		// —— 4. 更新源设置（窗口模式 + 窗口规格 + 光标）——
		obs_data_t* settings = obs_data_create();
		obs_data_set_string(settings, "capture_mode", "window");
		obs_data_set_string(settings, "window", windowSpec.c_str());
		if (usedFallback_) {
			// window_capture 的光标键为 "cursor"，并采集客户区
			obs_data_set_bool(settings, "cursor", captureCursor);
			obs_data_set_bool(settings, "client_area", true);
		} else {
			// game_capture 的光标键为 "capture_cursor"
			obs_data_set_bool(settings, "capture_cursor", captureCursor);
		}
		obs_source_update(source, settings);
		obs_data_release(settings);

		// —— 5. 源加入场景并拉伸填满画布 ——
		obs_sceneitem_t* item = obs_scene_find_source(scene, kSourceName);
		if (!item) {
			item = obs_scene_add(scene, source);
			if (!item) {
				GRT_LOG_ERROR("采集源加入场景失败：%s", kSourceName);
				obs_source_release(source);
				obs_source_release(sceneSource);
				return false;
			}
		}
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		// 锚点（alignment）取左上角，与 pos=(0,0)（画布左上角）配合，
		// 让 item 从画布左上角开始铺满整个画布；若锚点取中心，item 中心
		// 会被放到画布左上角，画面整体偏移半个尺寸而贴在左上角。
		struct vec2 pos;
		vec2_zero(&pos);
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);

		// 画布尺寸要在视频重置后才更新，因此先改分辨率并触发重置，
		// 再按新画布尺寸设置源的拉伸边界（见下方第 7 步）。
		obs_source_release(source);

		// —— 6. 分辨率跟随游戏客户区 ——
		config_t* config = obs_frontend_get_profile_config();
		if (!config) {
			GRT_LOG_ERROR("无法获取 profile config");
			obs_source_release(sceneSource);
			return false;
		}

		if (!prepared_ || !havePreviousVideo_) {
			previousBaseCX_ = config_get_uint(config, "Video", "BaseCX");
			previousBaseCY_ = config_get_uint(config, "Video", "BaseCY");
			previousOutputCX_ = config_get_uint(config, "Video", "OutputCX");
			previousOutputCY_ = config_get_uint(config, "Video", "OutputCY");
			havePreviousVideo_ = true;
		}

		const int newWidth = AlignWidth4(clientWidth > 0 ? clientWidth : 1920);
		const int newHeight = AlignHeight2(clientHeight > 0 ? clientHeight : 1080);

		config_set_uint(config, "Video", "BaseCX", static_cast<uint64_t>(newWidth));
		config_set_uint(config, "Video", "BaseCY", static_cast<uint64_t>(newHeight));
		config_set_uint(config, "Video", "OutputCX", static_cast<uint64_t>(newWidth));
		config_set_uint(config, "Video", "OutputCY", static_cast<uint64_t>(newHeight));
		config_save(config);

		// obs_frontend_reset_video 内部直接调 OBSBasic::ResetVideo()，
		// 重建视频管线后当前画布即为新尺寸（必须在主线程调用）。
		obs_frontend_reset_video();

		// —— 6b. 回读校验：确认视频管线已按新分辨率重建 ——
		obs_video_info vi{};
		const bool videoOk =
		    obs_get_video_info(&vi) &&
		    static_cast<int>(vi.base_width) == newWidth &&
		    static_cast<int>(vi.base_height) == newHeight &&
		    static_cast<int>(vi.output_width) == newWidth &&
		    static_cast<int>(vi.output_height) == newHeight;
		if (!videoOk) {
			GRT_LOG_ERROR("视频分辨率重置未生效：期望 %dx%d，实际 base=%ux%u output=%ux%u，开始录制中止",
			             newWidth, newHeight,
			             static_cast<unsigned>(vi.base_width),
			             static_cast<unsigned>(vi.base_height),
			             static_cast<unsigned>(vi.output_width),
			             static_cast<unsigned>(vi.output_height));
			// 回滚 profile 分辨率配置，尽量恢复原状
			if (havePreviousVideo_) {
				config_set_uint(config, "Video", "BaseCX",
				                static_cast<uint64_t>(previousBaseCX_));
				config_set_uint(config, "Video", "BaseCY",
				                static_cast<uint64_t>(previousBaseCY_));
				config_set_uint(config, "Video", "OutputCX",
				                static_cast<uint64_t>(previousOutputCX_));
				config_set_uint(config, "Video", "OutputCY",
				                static_cast<uint64_t>(previousOutputCY_));
				config_save(config);
				obs_frontend_reset_video();
			}
			obs_source_release(sceneSource);
			return false;
		}

		// —— 7. 视频重置后按新画布尺寸设置拉伸边界 ——
		struct vec2 bounds;
		vec2_set(&bounds, static_cast<float>(newWidth),
		         static_cast<float>(newHeight));
		obs_sceneitem_set_bounds(item, &bounds);

		// —— 8. 切换到本场景 ——
		obs_frontend_set_current_scene(sceneSource);
		obs_source_release(sceneSource);

		actualWidth_ = newWidth;
		actualHeight_ = newHeight;
		lastBelow1080p_ = (newWidth < 1920 || newHeight < k1080Height);
		if (lastBelow1080p_) {
			GRT_LOG_WARN("游戏客户区分辨率 %dx%d 低于 1920x1080，"
			             "录制画质可能偏低", newWidth, newHeight);
		}

		prepared_ = true;
		GRT_LOG_INFO("采集场景准备完成：%dx%d，回退源=%d，规格=%s",
		             newWidth, newHeight, usedFallback_ ? 1 : 0,
		             windowSpec.c_str());
		return true;
	});

	return ok;
}

bool GameCaptureScene::Restore()
{
	const bool ok = RunOnUiThread([&]() -> bool {
		if (!prepared_) {
			return true; // 没有成功 Prepare 过，空操作
		}

		// 恢复原场景
		if (!previousSceneName_.empty()) {
			obs_scene_t* previous = obs_get_scene_by_name(previousSceneName_.c_str());
			if (previous) {
				obs_source_t* previousSource = obs_scene_get_source(previous);
				obs_frontend_set_current_scene(previousSource);
				obs_source_release(previousSource);
			} else {
				GRT_LOG_WARN("原场景已不存在，无法恢复切换：%s",
				             previousSceneName_.c_str());
			}
		}

		// 恢复原分辨率
		if (havePreviousVideo_) {
			config_t* config = obs_frontend_get_profile_config();
			if (config) {
				config_set_uint(config, "Video", "BaseCX",
				                 static_cast<uint64_t>(previousBaseCX_));
				config_set_uint(config, "Video", "BaseCY",
				                 static_cast<uint64_t>(previousBaseCY_));
				config_set_uint(config, "Video", "OutputCX",
				                 static_cast<uint64_t>(previousOutputCX_));
				config_set_uint(config, "Video", "OutputCY",
				                 static_cast<uint64_t>(previousOutputCY_));
				config_save(config);
				obs_frontend_reset_video();
			} else {
				GRT_LOG_ERROR("恢复分辨率时无法获取 profile config");
				return false;
			}
			havePreviousVideo_ = false;
		}

		prepared_ = false;
		GRT_LOG_INFO("采集场景与分辨率已恢复");
		return true;
	});

	return ok;
}

} // namespace grt
