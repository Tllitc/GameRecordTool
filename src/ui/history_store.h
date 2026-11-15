/**
 * @file history_store.h
 * @brief 录制会话历史的独立 JSON 存储（UI 层自持，与 PluginConfig 分离）。
 *
 * 历史不写入 PluginConfig（该文件归属其他任务，本任务不得修改），而是使用
 * 独立的 obs_data JSON 文件：obs_module_config_path("history.json")。
 * 写入采用 obs_data_save_json_safe 的“临时文件 + 备份替换”原子策略。
 *
 * 线程模型：仅在 Qt 主线程使用（面板显示、录制结束回调经 QueuedConnection
 * 转投后调用）。obs_data_t 的全部使用隐藏在 .cpp 中。
 */

#pragma once

#include <string>
#include <vector>

namespace grt::ui {

/// 一条会话历史记录（字段全部为 UTF-8）
struct HistoryEntry {
	std::string exe;           ///< 目标进程可执行文件名（如 "game.exe"）
	std::string title;         ///< 目标窗口标题
	std::string sessionId;     ///< 会话目录基名（会话 ID）
	std::string dir;           ///< 会话目录绝对路径
	std::string videoName;     ///< 录制文件名（取不到时为空）
	int durationSeconds = 0;   ///< 录制时长（秒，四舍五入取整）
	std::string startUtc;      ///< 开始时刻 UTC ISO-8601（可能为空）
	std::string verifyStatus;  ///< 校验状态，默认 "未校验"
	std::string verifyDetail;  ///< 校验完整报告（多行中文；未校验时为空）
	std::string time;          ///< 本地展示时间（由 UI 追加，与 startUtc 区分）
};

class HistoryStore {
public:
	/// 历史文件名（位于 OBS 模块配置目录下）
	static constexpr const char* kHistoryFileName = "history.json";
	/// 未校验条目的默认状态文案
	static constexpr const char* kDefaultVerifyStatus = "未校验";

	/**
	 * @brief 加载历史（文件不存在/损坏时返回空列表，不抛异常）。
	 * @return 按文件中的原始顺序返回（追加在尾部，最新记录在末尾）。
	 */
	static std::vector<HistoryEntry> Load();

	/**
	 * @brief 追加一条历史并原子写盘。
	 * @return 写盘成功返回 true。
	 */
	static bool Append(const HistoryEntry& entry);

	/**
	 * @brief 覆盖写全部历史（主要用于“清空历史”：传入空列表）。
	 * @return 写盘成功返回 true。
	 */
	static bool SaveAll(const std::vector<HistoryEntry>& entries);

	/// 清空历史（只把空数组写回 JSON，不删除文件）
	static bool Clear();

	/**
	 * @brief 按会话 ID 更新某条记录的校验结果并原子写盘。
	 * @param sessionId 目标会话 ID（HistoryEntry.sessionId）
	 * @param status    校验状态短文案（如“通过”/“失败”）
	 * @param detail    完整报告文本（详情对话框展示）
	 * @return 找到并写盘成功返回 true。
	 */
	static bool UpdateVerifyResult(const std::string& sessionId,
	                               const std::string& status,
	                               const std::string& detail);

private:
	HistoryStore() = delete;
};

} // namespace grt::ui
