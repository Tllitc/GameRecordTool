/**
 * @file history_store.cpp
 * @brief 录制会话历史存储实现（obs_data JSON，独立于 PluginConfig）。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui/history_store.h"

#include <filesystem>
#include <string_view>

#include <windows.h>

#include <obs-data.h>
#include <obs-module.h>
#include <util/bmem.h>

#include "core/log.h"

namespace grt::ui {

namespace {

// ---- JSON 字段名（英文 snake_case，持久化后保持稳定，勿随意修改）----------
constexpr const char* kKeyRecords = "records";
constexpr const char* kKeyExe = "exe";
constexpr const char* kKeyTitle = "title";
constexpr const char* kKeySessionId = "session_id";
constexpr const char* kKeyDir = "dir";
constexpr const char* kKeyVideoName = "video_name";
constexpr const char* kKeyDuration = "duration";
constexpr const char* kKeyStartUtc = "start_utc";
constexpr const char* kKeyVerifyStatus = "verify_status";
constexpr const char* kKeyVerifyDetail = "verify_detail";
constexpr const char* kKeyTime = "time";

/// UTF-8 字符串转 UTF-16（用于 std::filesystem / Win32 宽字符 API）
std::wstring Utf8ToWide(std::string_view utf8)
{
	if (utf8.empty()) {
		return std::wstring();
	}
	const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
	                                       static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring wide(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
	                    wide.data(), length);
	return wide;
}

/// RAII 包装 obs_data_t，确保异常/早退路径下正确释放引用
class ScopedData {
public:
	explicit ScopedData(obs_data_t* data) : data_(data) {}
	~ScopedData()
	{
		if (data_ != nullptr) {
			obs_data_release(data_);
		}
	}

	ScopedData(const ScopedData&) = delete;
	ScopedData& operator=(const ScopedData&) = delete;

	obs_data_t* get() const { return data_; }
	operator bool() const { return data_ != nullptr; }

private:
	obs_data_t* data_;
};

/// RAII 包装 obs_data_array_t
class ScopedArray {
public:
	explicit ScopedArray(obs_data_array_t* array) : array_(array) {}
	~ScopedArray()
	{
		if (array_ != nullptr) {
			obs_data_array_release(array_);
		}
	}

	ScopedArray(const ScopedArray&) = delete;
	ScopedArray& operator=(const ScopedArray&) = delete;

	obs_data_array_t* get() const { return array_; }

private:
	obs_data_array_t* array_;
};

/// 单条记录 -> obs_data 对象（调用方获得所有权）
obs_data_t* ToData(const HistoryEntry& entry)
{
	obs_data_t* obj = obs_data_create();
	obs_data_set_string(obj, kKeyExe, entry.exe.c_str());
	obs_data_set_string(obj, kKeyTitle, entry.title.c_str());
	obs_data_set_string(obj, kKeySessionId, entry.sessionId.c_str());
	obs_data_set_string(obj, kKeyDir, entry.dir.c_str());
	obs_data_set_string(obj, kKeyVideoName, entry.videoName.c_str());
	obs_data_set_int(obj, kKeyDuration, entry.durationSeconds);
	obs_data_set_string(obj, kKeyStartUtc, entry.startUtc.c_str());
	obs_data_set_string(
	    obj, kKeyVerifyStatus,
	    entry.verifyStatus.empty() ? HistoryStore::kDefaultVerifyStatus
	                               : entry.verifyStatus.c_str());
	obs_data_set_string(obj, kKeyVerifyDetail, entry.verifyDetail.c_str());
	obs_data_set_string(obj, kKeyTime, entry.time.c_str());
	return obj;
}

/// obs_data 对象 -> 单条记录
HistoryEntry FromData(obs_data_t* obj)
{
	HistoryEntry entry;
	entry.exe = obs_data_get_string(obj, kKeyExe);
	entry.title = obs_data_get_string(obj, kKeyTitle);
	entry.sessionId = obs_data_get_string(obj, kKeySessionId);
	entry.dir = obs_data_get_string(obj, kKeyDir);
	entry.videoName = obs_data_get_string(obj, kKeyVideoName);
	entry.durationSeconds =
	    static_cast<int>(obs_data_get_int(obj, kKeyDuration));
	entry.startUtc = obs_data_get_string(obj, kKeyStartUtc);
	entry.verifyStatus = obs_data_get_string(obj, kKeyVerifyStatus);
	if (entry.verifyStatus.empty()) {
		entry.verifyStatus = HistoryStore::kDefaultVerifyStatus;
	}
	entry.verifyDetail = obs_data_get_string(obj, kKeyVerifyDetail);
	entry.time = obs_data_get_string(obj, kKeyTime);
	return entry;
}

} // 匿名命名空间

std::vector<HistoryEntry> HistoryStore::Load()
{
	std::vector<HistoryEntry> result;
	const char* path = obs_module_config_path(kHistoryFileName);
	if (path == nullptr) {
		return result;
	}
	ScopedData root(obs_data_create_from_json_file(path));
	bfree(const_cast<char*>(path));
	if (!root) {
		return result;
	}

	ScopedArray array(obs_data_get_array(root.get(), kKeyRecords));
	if (!array.get()) {
		return result;
	}
	const size_t count = obs_data_array_count(array.get());
	result.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		ScopedData item(obs_data_array_item(array.get(), i));
		if (item) {
			result.push_back(FromData(item.get()));
		}
	}
	return result;
}

bool HistoryStore::SaveAll(const std::vector<HistoryEntry>& entries)
{
	const char* path = obs_module_config_path(kHistoryFileName);
	if (path == nullptr) {
		return false;
	}

	// 首次运行时 plugin_config/game_record_tool 目录可能尚不存在，
	// 必须先创建，否则写 .tmp 会失败导致历史记录丢失
	const std::wstring widePath = Utf8ToWide(path);
	if (widePath.empty()) {
		GRT_LOG_ERROR("历史记录路径转换为宽字符失败：%s", path);
		bfree(const_cast<char*>(path));
		return false;
	}
	const std::filesystem::path targetPath(widePath);
	std::error_code ec;
	std::filesystem::create_directories(targetPath.parent_path(), ec);
	if (ec) {
		GRT_LOG_ERROR("创建历史记录目录失败（code=%d）：%s",
		              ec.value(), ec.message().c_str());
		bfree(const_cast<char*>(path));
		return false;
	}

	ScopedData root(obs_data_create());
	ScopedArray array(obs_data_array_create());
	for (const HistoryEntry& entry : entries) {
		ScopedData item(ToData(entry));
		obs_data_array_push_back(array.get(), item.get());
	}
	obs_data_set_array(root.get(), kKeyRecords, array.get());

	// 临时文件后缀 ".tmp"，备份后缀 ".bak"；内部完成“写临时文件 + 替换”
	const bool ok = obs_data_save_json_safe(root.get(), path, ".tmp",
	                                       ".bak");
	bfree(const_cast<char*>(path));
	return ok;
}

bool HistoryStore::Append(const HistoryEntry& entry)
{
	std::vector<HistoryEntry> all = Load();
	all.push_back(entry);
	return SaveAll(all);
}

bool HistoryStore::Clear()
{
	return SaveAll({});
}

bool HistoryStore::UpdateVerifyResult(const std::string& sessionId,
                                      const std::string& status,
                                      const std::string& detail)
{
	if (sessionId.empty()) {
		return false;
	}
	std::vector<HistoryEntry> all = Load();
	bool found = false;
	for (HistoryEntry& entry : all) {
		if (entry.sessionId == sessionId) {
			entry.verifyStatus = status;
			entry.verifyDetail = detail;
			found = true;
			break;
		}
	}
	return found && SaveAll(all);
}

} // namespace grt::ui
