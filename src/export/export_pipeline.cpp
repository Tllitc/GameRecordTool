/**
 * @file export_pipeline.cpp
 * @brief 录制工件导出管线（Task 7）：操作数据 JSON + keymap 副本严格 schema 导出。
 *
 * 导出流程：
 *   1. 由 SessionArtifacts/SessionContext 派生输出路径（<base>.json /
 *      <base>_keymap.json，与 raw.jsonl、视频同目录同基名）；
 *   2. 三类事件做防御性稳定排序，校验区间 start<end、相邻不重叠，
 *      拒绝 NaN/Inf（违例直接返回错误，绝不写半截文件）；
 *   2.1 以视频容器时长为真值回填 duration_seconds，并把事件时间轴整体
 *      平移 lead（= QPC 墙钟跨度 − 视频时长，即编码器前导预热）秒，
 *      使 t=0 对齐视频首帧；平移后完全落在起点前的事件丢弃；
 *   3. 手写极简 JSON（转义复用 SpoolWriter::EscapeJson；数字固定 3/6 位
 *      小数并强制小数点，规避中文 locale 输出逗号）；
 *   4. 先写 <target>.tmp，用 obs_data JSON 解析器整体读回自检
 *      （严格字段名/类型/数量），全部通过后 MoveFileExW 原子替换；
 *   5. keymap 未覆盖操作事件、视频文件缺失等不阻断导出，仅进 warnings。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <obs-data.h>

#include "core/log.h"
#include "export/export_pipeline.h"
#include "input/key_names.h"
#include "session/spool_writer.h"
#include "verify/session_verifier.h"

namespace grt {

namespace {

// ---- 路径 / 字符串辅助 --------------------------------------------------------

/// UTF-8 -> UTF-16（用于 Win32 宽字符文件 API，支持含中文的会话路径）
std::wstring Utf8ToWide(std::string_view utf8)
{
	if (utf8.empty()) {
		return {};
	}
	const int len = MultiByteToWideChar(
	    CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (len <= 0) {
		return {};
	}
	std::wstring wide(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
	                    static_cast<int>(utf8.size()), wide.data(), len);
	return wide;
}

/// 取路径最后一段（同时兼容 '\\' 与 '/'）
std::string BaseName(const std::string& path)
{
	const size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// 去掉文件名扩展名（按最后一个 '.' 切分，'.' 须在最后一个分隔符之后）
std::string StripExtension(const std::string& fileName)
{
	const size_t slash = fileName.find_last_of("\\/");
	const size_t dot = fileName.find_last_of('.');
	if (dot != std::string::npos &&
	    (slash == std::string::npos || dot > slash)) {
		return fileName.substr(0, dot);
	}
	return fileName;
}

// ---- JSON 构造辅助 -----------------------------------------------------------

/// 输出 JSON 字符串字面量（含外层引号）
std::string JsonStr(const std::string& raw)
{
	return "\"" + SpoolWriter::EscapeJson(raw) + "\"";
}

/**
 * @brief 固定小数位数字输出（C locale 风格小数点）。
 *
 * snprintf 受 C locale 影响，理论上插件不会切换 locale，这里仍把可能
 * 出现的逗号统一替换为点号，杜绝中文系统输出 "0,000" 造成 JSON 非法。
 */
std::string FmtFixed(double value, int decimals)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
	std::string out(buf);
	for (char& ch : out) {
		if (ch == ',') {
			ch = '.';
		}
	}
	return out;
}

// ---- 文件原子写 --------------------------------------------------------------

/// 以二进制写临时文件 <target>.tmp（UTF-8 原样字节，不做换行转换）
bool WriteTmpFile(const std::wstring& tmpPath, const std::string& content,
                  std::string* error)
{
	std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
	if (!file) {
		*error = "无法创建临时文件";
		return false;
	}
	file.write(content.data(), static_cast<std::streamsize>(content.size()));
	file.flush();
	if (!file.good()) {
		*error = "写入临时文件失败（磁盘空间不足或路径不可写）";
		file.close();
		DeleteFileW(tmpPath.c_str());
		return false;
	}
	file.close();
	return true;
}

/// 用临时文件原子替换目标文件（同卷 MoveFileEx + REPLACE_EXISTING）
bool RenameOver(const std::wstring& tmpPath, const std::wstring& targetPath,
                std::string* error)
{
	if (MoveFileExW(tmpPath.c_str(), targetPath.c_str(),
	                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
		const DWORD winError = GetLastError();
		*error = "原子替换文件失败：" + Win32ErrorMessage(winError);
		DeleteFileW(tmpPath.c_str());
		return false;
	}
	return true;
}

// ---- obs_data 自检辅助 -------------------------------------------------------

/// 枚举 obs_data 对象的全部成员名（first/next 带引用，结束统一 release）
std::vector<std::string> EnumItemNames(obs_data_t* obj)
{
	std::vector<std::string> names;
	obs_data_item_t* item = obs_data_first(obj);
	while (item != nullptr) {
		const char* name = obs_data_item_get_name(item);
		names.emplace_back(name != nullptr ? name : "");
		if (!obs_data_item_next(&item)) {
			break;
		}
	}
	if (item != nullptr) {
		obs_data_item_release(&item);
	}
	return names;
}

/// 比较两个字符串集合是否一致（顺序无关）
bool SameNameSet(std::vector<std::string> actual,
                 std::vector<std::string> expected)
{
	std::sort(actual.begin(), actual.end());
	std::sort(expected.begin(), expected.end());
	return actual == expected;
}

/// 校验数组中每个对象元素的字段名集合（全量检查，数量已在外部比对）
bool CheckArrayElementKeys(obs_data_t* root, const char* arrayName,
                           const std::vector<std::string>& expectedKeys,
                           size_t expectedCount, std::string* error)
{
	obs_data_array_t* array = obs_data_get_array(root, arrayName);
	if (array == nullptr) {
		*error = std::string("自检失败：缺少数组字段 ") + arrayName;
		return false;
	}
	bool ok = false;
	do {
		if (obs_data_array_count(array) != expectedCount) {
			*error = std::string("自检失败：") + arrayName +
			         " 元素数量与输入不一致";
			break;
		}
		for (size_t i = 0; i < expectedCount; ++i) {
			obs_data_t* element = obs_data_array_item(array, i);
			if (element == nullptr) {
				*error = std::string("自检失败：") + arrayName + "[" +
				         std::to_string(i) + "] 不是对象";
				break;
			}
			const std::vector<std::string> names =
			    EnumItemNames(element);
			const bool keysOk = SameNameSet(names, expectedKeys);
			obs_data_release(element);
			if (!keysOk) {
				*error = std::string("自检失败：") + arrayName + "[" +
				         std::to_string(i) + "] 字段名不匹配";
				break;
			}
		}
		ok = error->empty();
	} while (false);
	// obs_data_get_array 返回带引用，结束统一释放
	obs_data_array_release(array);
	return ok;
}

/**
 * @brief 操作数据 JSON 读回自检（obs_data 解析器）。
 *
 * 校验：可解析；顶层恰好 game_metadata（对象）+ 三个数组；
 * metadata 恰含 5 个约定字段且 video_name/start_time/duration 回读一致；
 * 三个数组元素数量与输入一致，且每个元素字段名严格匹配 schema。
 */
bool VerifyDataJson(const std::string& tmpPathUtf8,
                    const std::string& expectedVideoName,
                    const std::string& expectedStartTime,
                    double expectedDuration,
                    size_t movementCount, size_t lookingCount,
                    size_t actionCount, std::string* error)
{
	obs_data_t* root = obs_data_create_from_json_file(tmpPathUtf8.c_str());
	if (root == nullptr) {
		*error = "自检失败：操作数据 JSON 无法解析";
		return false;
	}

	bool ok = false;
	do {
		// —— 顶层 4 个字段：1 个对象 + 3 个数组，字段名严格匹配 ——
		const std::vector<std::string> rootNames = EnumItemNames(root);
		if (!SameNameSet(rootNames,
		                 {"game_metadata", "movement_events",
		                  "looking_events", "action_events"})) {
			*error = "自检失败：顶层字段名集合不匹配 schema";
			break;
		}
		obs_data_item_t* metaItem =
		    obs_data_item_byname(root, "game_metadata");
		obs_data_item_t* moveItem =
		    obs_data_item_byname(root, "movement_events");
		obs_data_item_t* lookItem =
		    obs_data_item_byname(root, "looking_events");
		obs_data_item_t* actItem =
		    obs_data_item_byname(root, "action_events");
		const bool typesOk =
		    metaItem != nullptr && moveItem != nullptr &&
		    lookItem != nullptr && actItem != nullptr &&
		    obs_data_item_gettype(metaItem) == OBS_DATA_OBJECT &&
		    obs_data_item_gettype(moveItem) == OBS_DATA_ARRAY &&
		    obs_data_item_gettype(lookItem) == OBS_DATA_ARRAY &&
		    obs_data_item_gettype(actItem) == OBS_DATA_ARRAY;
		// obs_data_item_byname 返回带引用，取完类型立即释放，
		// 后续具体取值仍通过 obs_data_get_obj/get_array 重新获取
		if (metaItem != nullptr) {
			obs_data_item_release(&metaItem);
		}
		if (moveItem != nullptr) {
			obs_data_item_release(&moveItem);
		}
		if (lookItem != nullptr) {
			obs_data_item_release(&lookItem);
		}
		if (actItem != nullptr) {
			obs_data_item_release(&actItem);
		}
		if (!typesOk) {
			*error = "自检失败：顶层字段类型不匹配 schema";
			break;
		}

		// —— game_metadata ——
		obs_data_t* meta = obs_data_get_obj(root, "game_metadata");
		if (meta == nullptr) {
			*error = "自检失败：game_metadata 缺失";
			break;
		}
		if (!SameNameSet(EnumItemNames(meta),
		                 {"game_title", "session_id", "video_name",
		                  "start_time", "duration_seconds"})) {
			obs_data_release(meta);
			*error = "自检失败：game_metadata 字段名集合不匹配 schema";
			break;
		}
		const std::string videoName =
		    obs_data_get_string(meta, "video_name");
		const std::string startTime =
		    obs_data_get_string(meta, "start_time");
		const double duration =
		    obs_data_get_double(meta, "duration_seconds");
		obs_data_release(meta);
		if (videoName != expectedVideoName) {
			*error = "自检失败：video_name 回读不一致";
			break;
		}
		if (startTime != expectedStartTime) {
			*error = "自检失败：start_time 回读不一致";
			break;
		}
		// duration 按 3 位小数字面量写出，允许半个最低位误差
		if (std::fabs(duration - expectedDuration) > 0.0005 + 1e-9) {
			*error = "自检失败：duration_seconds 回读不一致";
			break;
		}

		// —— 三个数组：数量 + 每元素字段名 ——
		if (!CheckArrayElementKeys(
		        root, "movement_events",
		        {"direction", "start_time", "end_time"},
		        movementCount, error)) {
			break;
		}
		if (!CheckArrayElementKeys(
		        root, "looking_events",
		        {"start_time", "end_time", "dx", "dy", "direction"},
		        lookingCount, error)) {
			break;
		}
		if (!CheckArrayElementKeys(
		        root, "action_events",
		        {"timestamp", "mouse_event", "keyboard_event",
		         "action"},
		        actionCount, error)) {
			break;
		}
		ok = true;
	} while (false);

	obs_data_release(root);
	if (!ok && error->empty()) {
		*error = "自检失败：未知原因";
	}
	return ok;
}

/// keymap 副本读回自检：顶层为扁平对象，条目数一致，每项恰含 type/action
bool VerifyKeymapJson(const std::string& tmpPathUtf8,
                      size_t expectedCount, std::string* error)
{
	obs_data_t* root = obs_data_create_from_json_file(tmpPathUtf8.c_str());
	if (root == nullptr) {
		*error = "自检失败：keymap JSON 无法解析";
		return false;
	}

	bool ok = false;
	do {
		const std::vector<std::string> keys = EnumItemNames(root);
		if (keys.size() != expectedCount) {
			*error = "自检失败：keymap 条目数量与输入不一致";
			break;
		}
		for (const std::string& key : keys) {
			obs_data_t* entry = obs_data_get_obj(root, key.c_str());
			if (entry == nullptr) {
				*error = "自检失败：keymap 条目 " + key +
				         " 不是对象";
				break;
			}
			const bool keysOk = SameNameSet(
			    EnumItemNames(entry), {"type", "action"});
			obs_data_release(entry);
			if (!keysOk) {
				*error = "自检失败：keymap 条目 " + key +
				                 " 字段名不匹配 schema";
				break;
			}
		}
		ok = error->empty();
	} while (false);

	obs_data_release(root);
	return ok;
}

// ---- 事件校验 ----------------------------------------------------------------

/// 全部时间/位移字段必须为有限值（NaN/Inf 无法序列化为合法 JSON 数字）。
/// 入参为稳定排序后的副本，使错误索引与实际输出顺序保持一致。
bool CheckFinite(const std::vector<MovementEvent>& movements,
                 const std::vector<LookingEvent>& looking,
                 const std::vector<ActionEvent>& actions,
                 std::string* error)
{
	for (size_t i = 0; i < movements.size(); ++i) {
		const MovementEvent& e = movements[i];
		if (!std::isfinite(e.start_time) || !std::isfinite(e.end_time)) {
			*error = "movement_events[" + std::to_string(i) +
			         "] 时间含 NaN/Inf，拒绝导出";
			return false;
		}
	}
	for (size_t i = 0; i < looking.size(); ++i) {
		const LookingEvent& e = looking[i];
		if (!std::isfinite(e.start_time) || !std::isfinite(e.end_time) ||
		    !std::isfinite(e.dx) || !std::isfinite(e.dy)) {
			*error = "looking_events[" + std::to_string(i) +
			         "] 字段含 NaN/Inf，拒绝导出";
			return false;
		}
	}
	for (size_t i = 0; i < actions.size(); ++i) {
		if (!std::isfinite(actions[i].timestamp)) {
			*error = "action_events[" + std::to_string(i) +
			         "] timestamp 含 NaN/Inf，拒绝导出";
			return false;
		}
	}
	return true;
}

/**
 * @brief 校验移动区间：按起点稳定排序后 start<end 且互不重叠。
 *
 * 文档允许相邻区间首尾相接；end_i 与 start_(i+1) 相差小于 1e-6 秒
 * （约 1 微秒）的浮点抖动视为相接，不报重叠。
 */
bool ValidateMovement(const std::vector<MovementEvent>& events,
                      std::string* error)
{
	constexpr double kTouchEpsilon = 1e-6;
	for (size_t i = 0; i < events.size(); ++i) {
		const MovementEvent& e = events[i];
		if (!(e.end_time > e.start_time)) {
			*error = "movement_events[" + std::to_string(i) +
			         "] 非法区间：start_time 必须 < end_time（" +
			         FmtFixed(e.start_time, 6) + " -> " +
			         FmtFixed(e.end_time, 6) + "）";
			return false;
		}
		if (i > 0) {
			const double prevEnd = events[i - 1].end_time;
			if (e.start_time + kTouchEpsilon < prevEnd) {
				*error = "movement_events[" +
				         std::to_string(i - 1) + "]/[" +
				         std::to_string(i) +
				         "] 区间重叠（前 end=" +
				         FmtFixed(prevEnd, 6) + " > 后 start=" +
				         FmtFixed(e.start_time, 6) + "）";
				return false;
			}
		}
	}
	return true;
}

// ---- 时长/时间轴对齐（视频为真值）--------------------------------------------

/// 允许平移的最大前导量（秒）：超过视为异常（如探测到错误文件），不平移
constexpr double kMaxAlignLeadSeconds = 5.0;
/// 事件时间轴与视频时长偏差的告警阈值（秒），与二期 4.4 口径一致
constexpr double kAlignWarnSeconds = 0.1;

/**
 * @brief 三类事件时间统一平移 delta 秒，使 t=0 对齐视频首帧。
 *
 * delta 取负值（事件时间轴比视频早，需整体后移）。平移后：
 *   - 区间型事件（movement/looking）start_time 裁剪到 ≥ 0，end_time 不小于
 *     start_time，保持 start<end 不变式；完全落在起点前（end_time+delta ≤ 0）
 *     的事件丢弃；
 *   - 离散型事件（action）timestamp 裁剪到 ≥ 0，完全落在起点前的丢弃。
 */
void ShiftEventTimes(double delta, std::vector<MovementEvent>* movements,
                     std::vector<LookingEvent>* looking,
                     std::vector<ActionEvent>* actions)
{
	std::erase_if(*movements, [delta](const MovementEvent& e) {
		return e.end_time + delta <= 0.0;
	});
	for (MovementEvent& e : *movements) {
		e.start_time = std::max(0.0, e.start_time + delta);
		e.end_time += delta;
	}

	std::erase_if(*looking, [delta](const LookingEvent& e) {
		return e.end_time + delta <= 0.0;
	});
	for (LookingEvent& e : *looking) {
		e.start_time = std::max(0.0, e.start_time + delta);
		e.end_time += delta;
	}

	std::erase_if(*actions, [delta](const ActionEvent& e) {
		return e.timestamp + delta <= 0.0;
	});
	for (ActionEvent& e : *actions) {
		e.timestamp = std::max(0.0, e.timestamp + delta);
	}
}

// ---- keymap 类型规范化 --------------------------------------------------------

/// 快照中的 type 仅接受 "keyboard"/"mouse"，其余按规范名空间判定补全
std::string NormalizeKeymapType(const std::string& keyName,
                                const std::string& storedType)
{
	if (storedType == "keyboard" || storedType == "mouse") {
		return storedType;
	}
	return IsMouseName(keyName) ? "mouse" : "keyboard";
}

} // namespace

// ---- 主入口 ------------------------------------------------------------------

ExportResult ExportSessionArtifacts(
    const SessionContext& context,
    const SessionArtifacts& artifacts,
    const std::map<std::string, KeymapEntry>& keymap)
{
	ExportResult result;

	GRT_LOG_INFO("开始导出会话工件：sessionDir=%s，原始事件=%llu，"
	             "movement=%zu，looking=%zu，action=%zu，keymap 条目=%zu",
	             context.sessionDir.c_str(),
	             static_cast<unsigned long long>(artifacts.rawEventCount),
	             artifacts.movementEvents.size(),
	             artifacts.lookingEvents.size(),
	             artifacts.actionEvents.size(),
	             keymap.size());

	// —— 1. 派生输出路径：优先从 <base>_raw.jsonl 路径剥离后缀，
	//       保证与 raw.jsonl/视频同目录同基名 ——
	constexpr std::string_view kRawSuffix = "_raw.jsonl";
	std::string dataPath;
	if (artifacts.rawSpoolPath.size() >= kRawSuffix.size() &&
	    artifacts.rawSpoolPath.compare(
	        artifacts.rawSpoolPath.size() - kRawSuffix.size(),
	        kRawSuffix.size(), kRawSuffix) == 0) {
		dataPath = artifacts.rawSpoolPath.substr(
		               0, artifacts.rawSpoolPath.size() - kRawSuffix.size()) +
		           ".json";
	} else {
		if (context.sessionDir.empty() || context.sessionBase.empty()) {
			result.error = "无法确定导出目录：sessionDir/sessionBase 为空"
			               "且 raw spool 路径缺失";
			GRT_LOG_ERROR("%s", result.error.c_str());
			return result;
		}
		dataPath = context.sessionDir + "\\" + context.sessionBase + ".json";
	}
	constexpr std::string_view kJsonExt = ".json";
	result.outputPath = dataPath;
	result.keymapPath =
	    dataPath.substr(0, dataPath.size() - kJsonExt.size()) +
	    "_keymap.json";

	// —— 2. 元数据字段 ——
	std::string gameTitle = context.target.title;
	if (gameTitle.empty()) {
		// 标题缺失：退化为可执行文件名去扩展名
		gameTitle = StripExtension(context.target.exeName);
		if (gameTitle.empty()) {
			// 极端兜底：使用会话基名，仍保证 game_title 非空
			gameTitle = BaseName(StripExtension(dataPath));
		}
	}
	const std::string sessionId = context.sessionBase;

	std::string videoName;
	std::string videoFullPath;
	if (!artifacts.videoPath.empty()) {
		videoFullPath = artifacts.videoPath;
		videoName = BaseName(artifacts.videoPath);
	} else {
		videoName = context.sessionBase + ".mp4";
		result.warnings.push_back(
		    "前端未返回录制文件路径，video_name 推断为 \"" + videoName +
		    "\"");
	}
	// 视频文件存在性仅告警（Task 12 再做硬校验）
	if (!videoFullPath.empty()) {
		const DWORD attrs =
		    GetFileAttributesW(Utf8ToWide(videoFullPath).c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			result.warnings.push_back("录制文件不存在：" + videoFullPath);
		}
	}

	// 最终写入 JSON 的时长：先取 QPC 估计值，2.1 步以视频容器时长覆盖
	double jsonDuration = context.durationSeconds;
	if (!std::isfinite(jsonDuration)) {
		result.error = "duration_seconds 为 NaN/Inf，拒绝导出";
		GRT_LOG_ERROR("%s", result.error.c_str());
		return result;
	}

	// —— 3. 防御性排序 + 数值/区间校验（在写任何文件之前完成）——
	std::vector<MovementEvent> movements = artifacts.movementEvents;
	std::vector<LookingEvent> looking = artifacts.lookingEvents;
	std::vector<ActionEvent> actions = artifacts.actionEvents;

	std::stable_sort(movements.begin(), movements.end(),
	                 [](const MovementEvent& a, const MovementEvent& b) {
		                 return a.start_time < b.start_time;
	                 });
	std::stable_sort(looking.begin(), looking.end(),
	                 [](const LookingEvent& a, const LookingEvent& b) {
		                 return a.start_time < b.start_time;
	                 });
	std::stable_sort(actions.begin(), actions.end(),
	                 [](const ActionEvent& a, const ActionEvent& b) {
		                 return a.timestamp < b.timestamp;
	                 });

	// —— 2.1 时长与事件时间轴对齐视频（视频为真值）——
	// 会话收尾时 duration_seconds = qpc1 - qpc0（QPC 墙钟跨度），而 qpc0
	// 采样在 obs_frontend_recording_start() 之前，视频内容首帧要晚约一个
	// 编码器前导预热（实测 0.204s ≈ 12 帧 @60fps）。此处视频已定稿，故以
	// 容器时长回填 duration_seconds，并把事件时间轴整体平移 lead 秒使其
	// 与视频首帧对齐，保证 |JSON−视频| 与事件对齐均落在 0.1s 内。
	if (!videoFullPath.empty()) {
		double videoDuration = 0.0;
		if (ProbeVideoDuration(videoFullPath, ResolveFfprobePath(),
		                       &videoDuration)) {
			const double lead = jsonDuration - videoDuration;
			if (std::isfinite(lead) && lead > 0.0 &&
			    lead <= kMaxAlignLeadSeconds) {
				ShiftEventTimes(-lead, &movements, &looking, &actions);
			} else if (std::isfinite(lead) &&
			           std::fabs(lead) > kAlignWarnSeconds) {
				char msg[192];
				std::snprintf(msg, sizeof(msg),
				              "事件时间轴与视频时长偏差 %.3fs 超过 "
				              "%.1fs，未做对齐，请检查视频文件",
				              lead, kAlignWarnSeconds);
				result.warnings.push_back(msg);
			}
			jsonDuration = videoDuration;
			GRT_LOG_INFO("以视频时长对齐：QPC 时长=%.3fs，视频时长=%.3fs，"
			             "前导=%.3fs",
			             context.durationSeconds, videoDuration, lead);
		} else {
			result.warnings.push_back(
			    "无法探测视频时长，duration_seconds 保留 QPC 估计值");
			GRT_LOG_WARN("视频时长探测失败，保留 duration=%.3fs",
			             jsonDuration);
		}
	}

	if (!CheckFinite(movements, looking, actions, &result.error) ||
	    !ValidateMovement(movements, &result.error)) {
		GRT_LOG_ERROR("导出前置校验失败：%s", result.error.c_str());
		return result;
	}

	// —— 4. keymap 覆盖性校验（不阻断导出，仅警告；不自动补全语义）——
	std::set<std::string> usedNames;
	for (const ActionEvent& e : actions) {
		if (!e.mouse_event.empty()) {
			usedNames.insert(e.mouse_event);
		}
		if (!e.keyboard_event.empty()) {
			usedNames.insert(e.keyboard_event);
		}
	}
	for (const std::string& name : usedNames) {
		if (keymap.find(name) == keymap.end()) {
			result.warnings.push_back("操作事件 \"" + name +
			                          "\" 未在 keymap 中定义，"
			                          "操作说明书未覆盖全部操作");
		}
	}

	// —— 5. 构造操作数据 JSON ——
	std::string sb;
	sb.reserve(2048 + actions.size() * 96 + movements.size() * 64 +
	           looking.size() * 96);
	sb += "{\n";
	sb += "  \"game_metadata\": {\n";
	sb += "    \"game_title\": " + JsonStr(gameTitle) + ",\n";
	sb += "    \"session_id\": " + JsonStr(sessionId) + ",\n";
	sb += "    \"video_name\": " + JsonStr(videoName) + ",\n";
	// startedAtUtc 形如 "YYYY-MM-DDTHH:MM:SSZ"，按任务约定直接输出 ISO 串
	sb += "    \"start_time\": " + JsonStr(context.startedAtUtc) + ",\n";
	sb += "    \"duration_seconds\": " +
	      FmtFixed(jsonDuration, 3) + "\n";
	sb += "  },\n";

	sb += "  \"movement_events\": [";
	if (movements.empty()) {
		sb += "],\n";
	} else {
		sb += "\n";
		for (size_t i = 0; i < movements.size(); ++i) {
			const MovementEvent& e = movements[i];
			sb += "    { \"direction\": " + JsonStr(e.direction) +
			      ", \"start_time\": " + FmtFixed(e.start_time, 6) +
			      ", \"end_time\": " + FmtFixed(e.end_time, 6) + " }";
			sb += (i + 1 == movements.size()) ? "\n  ],\n" : ",\n";
		}
	}

	sb += "  \"looking_events\": [";
	if (looking.empty()) {
		sb += "],\n";
	} else {
		sb += "\n";
		for (size_t i = 0; i < looking.size(); ++i) {
			const LookingEvent& e = looking[i];
			sb += "    { \"start_time\": " + FmtFixed(e.start_time, 6) +
			      ", \"end_time\": " + FmtFixed(e.end_time, 6) +
			      ", \"dx\": " + FmtFixed(e.dx, 6) +
			      ", \"dy\": " + FmtFixed(e.dy, 6) +
			      ", \"direction\": " + JsonStr(e.direction) + " }";
			sb += (i + 1 == looking.size()) ? "\n  ],\n" : ",\n";
		}
	}

	sb += "  \"action_events\": [";
	if (actions.empty()) {
		sb += "]\n";
	} else {
		sb += "\n";
		for (size_t i = 0; i < actions.size(); ++i) {
			const ActionEvent& e = actions[i];
			sb += "    { \"timestamp\": " + FmtFixed(e.timestamp, 6) +
			      ", \"mouse_event\": " + JsonStr(e.mouse_event) +
			      ", \"keyboard_event\": " + JsonStr(e.keyboard_event) +
			      ", \"action\": " + JsonStr(e.action) + " }";
			sb += (i + 1 == actions.size()) ? "\n  ]\n" : ",\n";
		}
	}
	sb += "}\n";

	// —— 6. 构造 keymap 副本 JSON（扁平 key -> {type,action}）——
	std::string km;
	km.reserve(128 + keymap.size() * 96);
	km += "{\n";
	size_t kmIndex = 0;
	for (const auto& [keyName, entry] : keymap) {
		const std::string type =
		    NormalizeKeymapType(keyName, entry.type);
		km += "  " + JsonStr(keyName) + ": { \"type\": " +
		      JsonStr(type) + ", \"action\": " + JsonStr(entry.action) +
		      " }";
		km += (++kmIndex == keymap.size()) ? "\n" : ",\n";
	}
	km += "}\n";

	// —— 7. 先写两个 .tmp ——
	const std::wstring wideDataPath = Utf8ToWide(result.outputPath);
	const std::wstring wideKeymapPath = Utf8ToWide(result.keymapPath);
	const std::wstring wideDataTmp = wideDataPath + L".tmp";
	const std::wstring wideKeymapTmp = wideKeymapPath + L".tmp";

	if (!WriteTmpFile(wideDataTmp, sb, &result.error)) {
		GRT_LOG_ERROR("操作数据写入失败：%s", result.error.c_str());
		return result;
	}
	if (!WriteTmpFile(wideKeymapTmp, km, &result.error)) {
		GRT_LOG_ERROR("keymap 副本写入失败：%s", result.error.c_str());
		DeleteFileW(wideDataTmp.c_str());
		return result;
	}

	// —— 8. 读回自检（全部通过后才允许 rename）——
	if (!VerifyDataJson(result.outputPath + ".tmp", videoName,
	                    context.startedAtUtc, jsonDuration,
	                    movements.size(), looking.size(), actions.size(),
	                    &result.error)) {
		GRT_LOG_ERROR("%s：%s", result.error.c_str(),
		              result.outputPath.c_str());
		DeleteFileW(wideDataTmp.c_str());
		DeleteFileW(wideKeymapTmp.c_str());
		return result;
	}
	if (!VerifyKeymapJson(result.keymapPath + ".tmp", keymap.size(),
	                      &result.error)) {
		GRT_LOG_ERROR("%s：%s", result.error.c_str(),
		              result.keymapPath.c_str());
		DeleteFileW(wideDataTmp.c_str());
		DeleteFileW(wideKeymapTmp.c_str());
		return result;
	}

	// —— 9. 原子替换 ——
	if (!RenameOver(wideDataTmp, wideDataPath, &result.error)) {
		GRT_LOG_ERROR("%s：%s", result.error.c_str(),
		              result.outputPath.c_str());
		DeleteFileW(wideKeymapTmp.c_str());
		return result;
	}
	if (!RenameOver(wideKeymapTmp, wideKeymapPath, &result.error)) {
		GRT_LOG_ERROR("%s：%s", result.error.c_str(),
		              result.keymapPath.c_str());
		return result;
	}

	result.ok = true;
	GRT_LOG_INFO("会话工件导出成功：%s、%s（告警 %zu 条）",
	             result.outputPath.c_str(), result.keymapPath.c_str(),
	             result.warnings.size());
	return result;
}

} // namespace grt
