/**
 * @file plugin_config.cpp
 * @brief 插件全局配置存储实现（obs_data_t JSON 持久化）。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "plugin_config.h"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <utility>

#include <windows.h>

#include <obs-data.h>
#include <obs-module.h>
#include <util/bmem.h>

#include "log.h"

namespace grt {

namespace {

// ---- JSON 字段名（英文 snake_case，持久化后保持稳定，勿随意修改）----------
constexpr const char *kKeyOutputDir = "output_dir";
constexpr const char *kKeyFps = "fps";
constexpr const char *kKeyVideoCodec = "video_codec";
constexpr const char *kKeyEncoder = "encoder";
constexpr const char *kKeyBitrateKbps = "bitrate_kbps";
constexpr const char *kKeyCrashSafe = "crash_safe";
constexpr const char *kKeyCaptureCursor = "capture_cursor";
constexpr const char *kKeyMovementKeys = "movement_keys";
constexpr const char *kKeyHotkeys = "hotkeys";
constexpr const char *kKeyHotkeyStart = "start";
constexpr const char *kKeyHotkeyStop = "stop";
constexpr const char *kKeyHotkeyMarker = "marker";
constexpr const char *kKeyHotkeyOverlay = "overlay_toggle";
constexpr const char *kKeyHotkeyModifiers = "modifiers";
constexpr const char *kKeyHotkeyVkey = "vkey";
constexpr const char *kKeyOverlay = "overlay";
constexpr const char *kKeyOverlayEnabled = "enabled";
constexpr const char *kKeyOverlayOpacity = "opacity";
constexpr const char *kKeyOverlayPosX = "pos_x";
constexpr const char *kKeyOverlayPosY = "pos_y";
constexpr const char *kKeyGames = "games";
constexpr const char *kKeyGameTitle = "title";
constexpr const char *kKeyGameMovementKeys = "movement_keys";
constexpr const char *kKeyGameKeymap = "keymap";
constexpr const char *kKeyKeymapType = "type";
constexpr const char *kKeyKeymapAction = "action";
// obs_data 数组元素只能是对象，故字符串数组统一存为 [{ "value": "..." }, ...]
constexpr const char *kKeyValue = "value";

constexpr const char *kConfigFileName = "game_record_tool.json";

/// RAII 包装 obs_data_t，确保异常/早退路径下正确释放引用
class ScopedData {
public:
	explicit ScopedData(obs_data_t *data) : data_(data) {}
	~ScopedData()
	{
		if (data_ != nullptr) {
			obs_data_release(data_);
		}
	}

	ScopedData(const ScopedData &) = delete;
	ScopedData &operator=(const ScopedData &) = delete;

	obs_data_t *get() const { return data_; }
	obs_data_t *operator->() const { return data_; }
	operator bool() const { return data_ != nullptr; }

private:
	obs_data_t *data_;
};

/// RAII 包装 obs_data_array_t
class ScopedArray {
public:
	explicit ScopedArray(obs_data_array_t *array) : array_(array) {}
	~ScopedArray()
	{
		if (array_ != nullptr) {
			obs_data_array_release(array_);
		}
	}

	ScopedArray(const ScopedArray &) = delete;
	ScopedArray &operator=(const ScopedArray &) = delete;

	obs_data_array_t *get() const { return array_; }
	operator bool() const { return array_ != nullptr; }

private:
	obs_data_array_t *array_;
};

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

/// 展开 "%USERPROFILE%\\Videos\\GameRecordTool" 形式的默认输出目录
std::string DefaultOutputDir()
{
	const char *userProfile = std::getenv("USERPROFILE");
	if (userProfile != nullptr && userProfile[0] != '\0') {
		return std::string(userProfile) + "\\Videos\\GameRecordTool";
	}
	return std::string("C:\\Users\\Public\\Videos\\GameRecordTool");
}

bool IsOneOf(std::string_view value, std::initializer_list<std::string_view> candidates)
{
	for (const auto candidate : candidates) {
		if (value == candidate) {
			return true;
		}
	}
	return false;
}

} // 匿名命名空间

PluginConfig::PluginConfig()
{
	LoadDefaults();
}

PluginConfig& PluginConfig::Instance()
{
	static PluginConfig instance;
	return instance;
}

void PluginConfig::LoadDefaults()
{
	recording_ = RecordingSettings{};
	recording_.outputDir = DefaultOutputDir();
	recording_.videoCodec = "auto";
	recording_.encoder = "auto";

	overlay_ = OverlaySettings{};

	// 默认热键：Ctrl+Alt+R / S / M / H（开始 / 停止 / 打点 / 悬浮窗）
#ifdef _WIN32
	constexpr int kModCtrlAlt = MOD_CONTROL | MOD_ALT;
	hotkeys_ = {{
	    {kModCtrlAlt, 'R'},
	    {kModCtrlAlt, 'S'},
	    {kModCtrlAlt, 'M'},
	    {kModCtrlAlt, 'H'},
	}};
#endif

	movementKeys_ = {"W", "A", "S", "D"};

	games_.clear();
}

void PluginConfig::Initialize()
{
	// obs_module_get_config_path 返回由 bfree 释放的路径字符串
	char *path = obs_module_get_config_path(obs_current_module(), kConfigFileName);
	std::string configPath = (path != nullptr) ? std::string(path) : std::string();
	if (path != nullptr) {
		bfree(path);
	}

	std::lock_guard lock(mutex_);
	LoadDefaults();
	configPath_ = std::move(configPath);

	if (configPath_.empty()) {
		GRT_LOG_WARN("无法获取插件配置文件路径，配置将不会持久化");
		return;
	}

	LoadFromDisk(configPath_);
}

void PluginConfig::LoadFromDisk(const std::string& configPath)
{
	// 文件不存在或解析失败时返回空对象（obs_data_create_from_json_file 语义）
	ScopedData loaded(obs_data_create_from_json_file(configPath.c_str()));
	if (!loaded) {
		return; // 使用默认值
	}

	// ---- 录制设置 --------------------------------------------------------
	if (const char *value = obs_data_get_string(loaded.get(), kKeyOutputDir);
	    value != nullptr && value[0] != '\0') {
		recording_.outputDir = value;
	}

	const long long fps = obs_data_get_int(loaded.get(), kKeyFps);
	if (fps == 30 || fps == 60) {
		recording_.fps = static_cast<int>(fps);
	}

	if (const char *value = obs_data_get_string(loaded.get(), kKeyVideoCodec);
	    IsOneOf(value, {"auto", "h264", "h265"})) {
		recording_.videoCodec = value;
	}

	if (const char *value = obs_data_get_string(loaded.get(), kKeyEncoder);
	    IsOneOf(value, {"auto", "nvenc", "amf", "qsv", "x264"})) {
		recording_.encoder = value;
	}

	const long long bitrate = obs_data_get_int(loaded.get(), kKeyBitrateKbps);
	if (bitrate >= 0 && bitrate <= 1'000'000) {
		recording_.bitrateKbps = static_cast<int>(bitrate);
	}

	// 布尔值缺失时 obs_data_get_bool 返回 false，无法区分“显式 false”与缺失，
	// 但 crash_safe 默认 true，故仅当用户显式写入该键时才覆盖
	if (obs_data_has_user_value(loaded.get(), kKeyCrashSafe)) {
		recording_.crashSafe = obs_data_get_bool(loaded.get(), kKeyCrashSafe);
	}
	if (obs_data_has_user_value(loaded.get(), kKeyCaptureCursor)) {
		recording_.captureCursor = obs_data_get_bool(loaded.get(), kKeyCaptureCursor);
	}

	// ---- 全局移动键（存为 [{ "value": "W" }, ...] 对象数组）---------------
	if (ScopedArray array(obs_data_get_array(loaded.get(), kKeyMovementKeys)); array) {
		const size_t count = obs_data_array_count(array.get());
		if (count == movementKeys_.size()) {
			bool valid = true;
			std::array<std::string, 4> keys{};
			for (size_t i = 0; i < count; ++i) {
				ScopedData item(obs_data_array_item(array.get(), i));
				if (!item) {
					valid = false;
					break;
				}
				const char *key = obs_data_get_string(item.get(), kKeyValue);
				if (key == nullptr || key[0] == '\0') {
					valid = false;
					break;
				}
				keys[i] = key;
			}
			if (valid) {
				movementKeys_ = keys;
			}
		}
	}

	// ---- 叠加层 ----------------------------------------------------------
	if (ScopedData overlay(obs_data_get_obj(loaded.get(), kKeyOverlay)); overlay) {
		if (obs_data_has_user_value(overlay.get(), kKeyOverlayEnabled)) {
			overlay_.enabled = obs_data_get_bool(overlay.get(), kKeyOverlayEnabled);
		}
		if (obs_data_has_user_value(overlay.get(), kKeyOverlayOpacity)) {
			const double opacity = obs_data_get_double(overlay.get(), kKeyOverlayOpacity);
			if (opacity >= 0.0 && opacity <= 1.0) {
				overlay_.opacity = opacity;
			}
		}
		if (obs_data_has_user_value(overlay.get(), kKeyOverlayPosX)) {
			overlay_.posX = static_cast<int>(obs_data_get_int(overlay.get(), kKeyOverlayPosX));
		}
		if (obs_data_has_user_value(overlay.get(), kKeyOverlayPosY)) {
			overlay_.posY = static_cast<int>(obs_data_get_int(overlay.get(), kKeyOverlayPosY));
		}
	}

	// ---- 全局热键 --------------------------------------------------------
	if (ScopedData hotkeys(obs_data_get_obj(loaded.get(), kKeyHotkeys)); hotkeys) {
		constexpr std::array<std::string_view, 4> kNames = {
		    kKeyHotkeyStart, kKeyHotkeyStop, kKeyHotkeyMarker, kKeyHotkeyOverlay};
		for (size_t i = 0; i < hotkeys_.size(); ++i) {
			ScopedData item(obs_data_get_obj(hotkeys.get(),
			                                 std::string(kNames[i]).c_str()));
			if (!item) {
				continue;
			}
			if (obs_data_has_user_value(item.get(), kKeyHotkeyVkey)) {
				const long long vkey = obs_data_get_int(item.get(), kKeyHotkeyVkey);
				const long long mods = obs_data_get_int(item.get(),
				                                         kKeyHotkeyModifiers);
				// vkey 合法范围 1..254；修饰符只保留 Ctrl/Shift/Alt
				if (vkey >= 1 && vkey <= 0xFE) {
					hotkeys_[i].vkey = static_cast<int>(vkey);
					hotkeys_[i].modifiers =
					    static_cast<int>(mods) & (MOD_CONTROL | MOD_SHIFT | MOD_ALT);
				}
			}
		}
	}

	// ---- 游戏档案 --------------------------------------------------------
	if (ScopedData games(obs_data_get_obj(loaded.get(), kKeyGames)); games) {
		for (obs_data_item_t *item = obs_data_first(games.get()); item != nullptr;
		     obs_data_item_next(&item)) {
			if (obs_data_item_gettype(item) != OBS_DATA_OBJECT) {
				continue;
			}

			const char *exe = obs_data_item_get_name(item);
			if (exe == nullptr || exe[0] == '\0') {
				continue;
			}

			ScopedData profileData(obs_data_item_get_obj(item));
			if (!profileData) {
				continue;
			}

			GameProfile profile;
			if (const char *title = obs_data_get_string(profileData.get(), kKeyGameTitle);
			    title != nullptr) {
				profile.title = title;
			}

			if (ScopedArray keys(obs_data_get_array(profileData.get(), kKeyGameMovementKeys));
			    keys && obs_data_array_count(keys.get()) == movementKeys_.size()) {
				std::array<std::string, 4> movement{};
				bool valid = true;
				for (size_t i = 0; i < movement.size(); ++i) {
					ScopedData keyItem(obs_data_array_item(keys.get(), i));
					if (!keyItem) {
						valid = false;
						break;
					}
					const char *key = obs_data_get_string(keyItem.get(), kKeyValue);
					if (key == nullptr || key[0] == '\0') {
						valid = false;
						break;
					}
					movement[i] = key;
				}
				if (valid) {
					profile.movementKeys = movement;
				}
			}

			if (ScopedData keymap(obs_data_get_obj(profileData.get(), kKeyGameKeymap)); keymap) {
				for (obs_data_item_t *keyItem = obs_data_first(keymap.get());
				     keyItem != nullptr; obs_data_item_next(&keyItem)) {
					if (obs_data_item_gettype(keyItem) != OBS_DATA_OBJECT) {
						continue;
					}
					const char *keyName = obs_data_item_get_name(keyItem);
					if (keyName == nullptr || keyName[0] == '\0') {
						continue;
					}
					ScopedData entryData(obs_data_item_get_obj(keyItem));
					if (!entryData) {
						continue;
					}
					KeymapEntry entry;
					if (const char *type = obs_data_get_string(entryData.get(), kKeyKeymapType);
					    type != nullptr) {
						entry.type = type;
					}
					if (const char *action = obs_data_get_string(entryData.get(),
					                                             kKeyKeymapAction);
					    action != nullptr) {
						entry.action = action;
					}
					profile.keymap.emplace(keyName, std::move(entry));
				}
			}

			games_.emplace(exe, std::move(profile));
		}
	}
}

bool PluginConfig::Save()
{
	std::lock_guard lock(mutex_);

	if (configPath_.empty()) {
		GRT_LOG_WARN("配置文件路径为空，跳过保存");
		return false;
	}

	ScopedData data(obs_data_create());
	if (!data) {
		GRT_LOG_ERROR("obs_data_create 失败，无法保存配置");
		return false;
	}

	// ---- 录制设置 --------------------------------------------------------
	obs_data_set_string(data.get(), kKeyOutputDir, recording_.outputDir.c_str());
	obs_data_set_int(data.get(), kKeyFps, recording_.fps);
	obs_data_set_string(data.get(), kKeyVideoCodec, recording_.videoCodec.c_str());
	obs_data_set_string(data.get(), kKeyEncoder, recording_.encoder.c_str());
	obs_data_set_int(data.get(), kKeyBitrateKbps, recording_.bitrateKbps);
	obs_data_set_bool(data.get(), kKeyCrashSafe, recording_.crashSafe);
	obs_data_set_bool(data.get(), kKeyCaptureCursor, recording_.captureCursor);

	obs_data_array_t *movementArray = obs_data_array_create();
	for (const auto& key : movementKeys_) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, kKeyValue, key.c_str());
		obs_data_array_push_back(movementArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(data.get(), kKeyMovementKeys, movementArray);
	obs_data_array_release(movementArray);

	// ---- 叠加层 ----------------------------------------------------------
	obs_data_t *overlay = obs_data_create();
	obs_data_set_bool(overlay, kKeyOverlayEnabled, overlay_.enabled);
	obs_data_set_double(overlay, kKeyOverlayOpacity, overlay_.opacity);
	obs_data_set_int(overlay, kKeyOverlayPosX, overlay_.posX);
	obs_data_set_int(overlay, kKeyOverlayPosY, overlay_.posY);
	obs_data_set_obj(data.get(), kKeyOverlay, overlay);
	obs_data_release(overlay);

	// ---- 全局热键 --------------------------------------------------------
	obs_data_t *hotkeys = obs_data_create();
	constexpr std::array<std::string_view, 4> kHotkeyNames = {
	    kKeyHotkeyStart, kKeyHotkeyStop, kKeyHotkeyMarker, kKeyHotkeyOverlay};
	for (size_t i = 0; i < hotkeys_.size(); ++i) {
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, kKeyHotkeyModifiers, hotkeys_[i].modifiers);
		obs_data_set_int(item, kKeyHotkeyVkey, hotkeys_[i].vkey);
		obs_data_set_obj(hotkeys, std::string(kHotkeyNames[i]).c_str(), item);
		obs_data_release(item);
	}
	obs_data_set_obj(data.get(), kKeyHotkeys, hotkeys);
	obs_data_release(hotkeys);

	// ---- 游戏档案 --------------------------------------------------------
	obs_data_t *games = obs_data_create();
	for (const auto& [exe, profile] : games_) {
		obs_data_t *profileData = obs_data_create();
		obs_data_set_string(profileData, kKeyGameTitle, profile.title.c_str());

		if (profile.movementKeys.has_value()) {
			obs_data_array_t *keys = obs_data_array_create();
			for (const auto& key : *profile.movementKeys) {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, kKeyValue, key.c_str());
				obs_data_array_push_back(keys, entry);
				obs_data_release(entry);
			}
			obs_data_set_array(profileData, kKeyGameMovementKeys, keys);
			obs_data_array_release(keys);
		}

		obs_data_t *keymap = obs_data_create();
		for (const auto& [keyName, entry] : profile.keymap) {
			obs_data_t *entryData = obs_data_create();
			obs_data_set_string(entryData, kKeyKeymapType, entry.type.c_str());
			obs_data_set_string(entryData, kKeyKeymapAction, entry.action.c_str());
			obs_data_set_obj(keymap, keyName.c_str(), entryData);
			obs_data_release(entryData);
		}
		obs_data_set_obj(profileData, kKeyGameKeymap, keymap);
		obs_data_release(keymap);

		obs_data_set_obj(games, exe.c_str(), profileData);
		obs_data_release(profileData);
	}
	obs_data_set_obj(data.get(), kKeyGames, games);
	obs_data_release(games);

	// ---- 原子写：先写 .tmp 再替换正式文件 --------------------------------
	const std::filesystem::path targetPath(Utf8ToWide(configPath_));
	std::error_code ec;
	std::filesystem::create_directories(targetPath.parent_path(), ec);
	if (ec) {
		GRT_LOG_ERROR("创建配置目录失败（code=%d）：%s", ec.value(), ec.message().c_str());
		return false;
	}

	const std::filesystem::path tmpPath =
	    targetPath.parent_path() / (targetPath.filename().wstring() + L".tmp");

	if (!obs_data_save_json(data.get(), tmpPath.string().c_str())) {
		GRT_LOG_ERROR("写入临时配置文件失败：%ls", tmpPath.c_str());
		return false;
	}

	// MoveFileExW + MOVEFILE_REPLACE_EXISTING 在同卷下为原子替换
	if (MoveFileExW(tmpPath.c_str(), targetPath.c_str(),
	                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
		const DWORD error = GetLastError();
		GRT_LOG_ERROR("替换配置文件失败：%s", Win32ErrorMessage(error).c_str());
		return false;
	}

	return true;
}

// ---- 整体读写 --------------------------------------------------------------

RecordingSettings PluginConfig::GetRecordingSettings() const
{
	std::lock_guard lock(mutex_);
	return recording_;
}

void PluginConfig::SetRecordingSettings(const RecordingSettings& settings)
{
	{
		std::lock_guard lock(mutex_);
		recording_ = settings;
	}
	Save();
}

OverlaySettings PluginConfig::GetOverlaySettings() const
{
	std::lock_guard lock(mutex_);
	return overlay_;
}

void PluginConfig::SetOverlaySettings(const OverlaySettings& settings)
{
	{
		std::lock_guard lock(mutex_);
		overlay_ = settings;
	}
	Save();
}

HotkeySettings PluginConfig::GetHotkeySettings() const
{
	std::lock_guard lock(mutex_);
	return hotkeys_;
}

void PluginConfig::SetHotkeySettings(const HotkeySettings& settings)
{
	{
		std::lock_guard lock(mutex_);
		hotkeys_ = settings;
	}
	Save();
}

// ---- 标量读写 --------------------------------------------------------------

std::string PluginConfig::GetOutputDir() const
{
	std::lock_guard lock(mutex_);
	return recording_.outputDir;
}

void PluginConfig::SetOutputDir(const std::string& dir)
{
	{
		std::lock_guard lock(mutex_);
		recording_.outputDir = dir;
	}
	Save();
}

int PluginConfig::GetFps() const
{
	std::lock_guard lock(mutex_);
	return recording_.fps;
}

void PluginConfig::SetFps(int fps)
{
	{
		std::lock_guard lock(mutex_);
		recording_.fps = (fps == 30) ? 30 : 60;
	}
	Save();
}

std::string PluginConfig::GetVideoCodec() const
{
	std::lock_guard lock(mutex_);
	return recording_.videoCodec;
}

void PluginConfig::SetVideoCodec(const std::string& codec)
{
	{
		std::lock_guard lock(mutex_);
		if (IsOneOf(codec, {"auto", "h264", "h265"})) {
			recording_.videoCodec = codec;
		}
	}
	Save();
}

std::string PluginConfig::GetEncoder() const
{
	std::lock_guard lock(mutex_);
	return recording_.encoder;
}

void PluginConfig::SetEncoder(const std::string& encoder)
{
	{
		std::lock_guard lock(mutex_);
		if (IsOneOf(encoder, {"auto", "nvenc", "amf", "qsv", "x264"})) {
			recording_.encoder = encoder;
		}
	}
	Save();
}

int PluginConfig::GetBitrateKbps() const
{
	std::lock_guard lock(mutex_);
	return recording_.bitrateKbps;
}

void PluginConfig::SetBitrateKbps(int bitrateKbps)
{
	{
		std::lock_guard lock(mutex_);
		recording_.bitrateKbps = bitrateKbps;
	}
	Save();
}

bool PluginConfig::GetCrashSafe() const
{
	std::lock_guard lock(mutex_);
	return recording_.crashSafe;
}

void PluginConfig::SetCrashSafe(bool enabled)
{
	{
		std::lock_guard lock(mutex_);
		recording_.crashSafe = enabled;
	}
	Save();
}

bool PluginConfig::GetCaptureCursor() const
{
	std::lock_guard lock(mutex_);
	return recording_.captureCursor;
}

void PluginConfig::SetCaptureCursor(bool enabled)
{
	{
		std::lock_guard lock(mutex_);
		recording_.captureCursor = enabled;
	}
	Save();
}

std::array<std::string, 4> PluginConfig::GetMovementKeys() const
{
	std::lock_guard lock(mutex_);
	return movementKeys_;
}

void PluginConfig::SetMovementKeys(const std::array<std::string, 4>& keys)
{
	{
		std::lock_guard lock(mutex_);
		movementKeys_ = keys;
	}
	Save();
}

// ---- 游戏档案 --------------------------------------------------------------

std::optional<GameProfile> PluginConfig::GetGameProfile(const std::string& exe) const
{
	std::lock_guard lock(mutex_);
	const auto it = games_.find(exe);
	if (it == games_.end()) {
		return std::nullopt;
	}
	return it->second;
}

void PluginConfig::SaveGameProfile(const std::string& exe, const GameProfile& profile)
{
	{
		std::lock_guard lock(mutex_);
		games_[exe] = profile;
	}
	Save();
}

bool PluginConfig::RemoveGameProfile(const std::string& exe)
{
	bool removed = false;
	{
		std::lock_guard lock(mutex_);
		removed = games_.erase(exe) > 0;
	}
	if (removed) {
		Save();
	}
	return removed;
}

std::vector<GameProfileSummary> PluginConfig::ListGameProfiles() const
{
	std::vector<GameProfileSummary> result;
	{
		std::lock_guard lock(mutex_);
		result.reserve(games_.size());
		for (const auto& [exe, profile] : games_) {
			result.push_back(GameProfileSummary{exe, profile.title});
		}
	}
	// games_ 为 std::map，已按 exe 名排序，直接返回即可
	return result;
}

} // namespace grt
