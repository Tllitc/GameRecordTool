/**
 * @file plugin_config.h
 * @brief 插件全局配置存储（基于 OBS obs_data_t 的 JSON 持久化）。
 *
 * 对外只暴露纯 C++ 结构体与接口，obs_data_t 的使用完全隐藏在 .cpp 中。
 * 线程安全：所有读写均由内部互斥锁保护。
 */

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace grt {

/// 单个按键映射项（对应游戏按键名 -> 映射类型与动作）
struct KeymapEntry {
	std::string type;   ///< 映射类型（具体取值由后续输入/热键任务定义）
	std::string action; ///< 映射动作（具体取值由后续输入/热键任务定义）
};

/// 单个游戏的配置档案（以 exe 名（如 "game.exe"）为键）
struct GameProfile {
	std::string title;                            ///< 游戏显示名称
	std::optional<std::array<std::string, 4>> movementKeys; ///< 可选的移动键覆盖（默认 WASD 顺序）
	std::map<std::string, KeymapEntry> keymap;    ///< 按键名 -> 映射项
};

/// 游戏档案摘要（供列表展示，不含按键明细）
struct GameProfileSummary {
	std::string exe;   ///< 可执行文件名（如 "game.exe"）
	std::string title; ///< 游戏显示名称
};

/// 录制相关设置
struct RecordingSettings {
	std::string outputDir;   ///< 录制输出目录
	int fps = 60;            ///< 录制帧率，允许 30 / 60
	std::string videoCodec;  ///< 视频编码选择：auto/h264/h265
	std::string encoder;     ///< 编码器选择：auto/nvenc/amf/qsv/x264
	int bitrateKbps = 0;     ///< 码率（kbps），0 表示按分辨率档位自动
	bool crashSafe = true;   ///< 崩溃安全策略（fMP4 或 mkv+remux）
	bool captureCursor = false; ///< 是否采集鼠标光标
};

/// 单个全局热键绑定（Win32 修饰符位掩码 + 虚拟键码；vkey==0 表示未绑定）
struct HotkeyBinding {
	int modifiers = 0; ///< Win32 MOD_CONTROL(0x2)/MOD_SHIFT(0x4)/MOD_ALT(0x1) 位或
	int vkey = 0;      ///< Win32 虚拟键码（如 'R'、VK_F9）
};

/**
 * @brief 四个全局热键绑定，顺序固定：
 *        [0]开始录制 [1]停止录制 [2]动作打点 [3]悬浮窗显示/隐藏
 */
using HotkeySettings = std::array<HotkeyBinding, 4>;

/// 叠加层（Overlay）设置
struct OverlaySettings {
	bool enabled = true;        ///< 是否启用叠加层
	double opacity = 0.7;       ///< 不透明度（0.0 ~ 1.0）
	int posX = -1;              ///< 横坐标（像素），-1 表示使用默认位置
	int posY = -1;              ///< 纵坐标（像素），-1 表示使用默认位置
};

/**
 * @brief 插件配置单例。
 *
 * 典型生命周期：
 *   obs_module_load   -> PluginConfig::Initialize()
 *   运行期间          -> PluginConfig::Instance().GetXxx() / SetXxx()
 *   obs_module_unload -> PluginConfig::Instance().Save()
 */
class PluginConfig {
public:
	/**
	 * @brief 获取单例实例。
	 * @note Initialize() 之前调用会得到尚未加载磁盘数据的默认配置。
	 */
	static PluginConfig& Instance();

	/**
	 * @brief 确定配置文件路径（obs_module_config_path）并从磁盘加载。
	 *
	 * 配置文件不存在时使用全部默认值。应在 obs_module_load 最前面调用一次。
	 */
	void Initialize();

	/**
	 * @brief 将当前配置原子写入磁盘（先写 .tmp 再替换正式文件）。
	 * @return 是否写入成功。
	 */
	bool Save();

	// ---- 录制设置（整体读写）----------------------------------------------

	RecordingSettings GetRecordingSettings() const;
	void SetRecordingSettings(const RecordingSettings& settings);

	OverlaySettings GetOverlaySettings() const;
	void SetOverlaySettings(const OverlaySettings& settings);

	/// 获取四个全局热键绑定（开始/停止/打点/悬浮窗，顺序固定）
	HotkeySettings GetHotkeySettings() const;
	/// 整体写回四个全局热键绑定并立即持久化
	void SetHotkeySettings(const HotkeySettings& settings);

	// ---- 录制设置（标量便捷读写）------------------------------------------

	std::string GetOutputDir() const;
	void SetOutputDir(const std::string& dir);

	int GetFps() const;
	void SetFps(int fps);

	std::string GetVideoCodec() const;
	void SetVideoCodec(const std::string& codec);

	std::string GetEncoder() const;
	void SetEncoder(const std::string& encoder);

	int GetBitrateKbps() const;
	void SetBitrateKbps(int bitrateKbps);

	bool GetCrashSafe() const;
	void SetCrashSafe(bool enabled);

	bool GetCaptureCursor() const;
	void SetCaptureCursor(bool enabled);

	/// 获取移动键（W/A/S/D 顺序的 Windows vkey 名字符串，默认 "W","A","S","D"）
	std::array<std::string, 4> GetMovementKeys() const;
	void SetMovementKeys(const std::array<std::string, 4>& keys);

	// ---- 游戏档案 ----------------------------------------------------------

	/**
	 * @brief 按 exe 名获取游戏档案。
	 * @return 存在则返回档案；不存在返回 std::nullopt。
	 */
	std::optional<GameProfile> GetGameProfile(const std::string& exe) const;

	/**
	 * @brief 保存（新增或覆盖）指定 exe 的游戏档案并立即持久化。
	 */
	void SaveGameProfile(const std::string& exe, const GameProfile& profile);

	/// 删除指定 exe 的游戏档案；返回是否存在并删除
	bool RemoveGameProfile(const std::string& exe);

	/**
	 * @brief 列举全部已保存的游戏档案摘要（exe 名与标题）。
	 * @return 按 exe 名排序的摘要列表；只读操作，不改动任何配置。
	 */
	std::vector<GameProfileSummary> ListGameProfiles() const;

private:
	PluginConfig();

	/// 以内置默认值填充全部配置项
	void LoadDefaults();

	/**
	 * @brief 在锁保护下从 obs_data JSON 文件重新加载（不存在则保持默认值）。
	 * @param configPath 配置文件完整路径（UTF-8）。
	 */
	void LoadFromDisk(const std::string& configPath);

	mutable std::mutex mutex_; ///< 保护以下全部字段的读写
	std::string configPath_;   ///< 配置文件完整路径（Initialize 后有效）
	RecordingSettings recording_;
	OverlaySettings overlay_;
	HotkeySettings hotkeys_;
	std::array<std::string, 4> movementKeys_;
	std::map<std::string, GameProfile> games_;
};

} // namespace grt
