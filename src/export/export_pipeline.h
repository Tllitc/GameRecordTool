/**
 * @file export_pipeline.h
 * @brief 录制工件导出管线（Task 7：操作数据 JSON 与三件套导出）。
 *
 * 停止流程（SessionManager::FinishSession）调用 ExportSessionArtifacts，
 * 由本模块在会话目录内产出：
 *   - <base>.json        操作数据（game_metadata + 三类事件，严格 schema）；
 *   - <base>_keymap.json 本次会话 keymap 快照（扁平 key -> {type,action}）。
 * 两个文件均走“先写 .tmp -> 自检 -> MoveFileEx 原子替换”的落盘流程。
 *
 * 头文件只依赖标准库 + session/session_types.h + core/plugin_config.h
 * （KeymapEntry 为纯标准库结构），不引入 OBS/Qt 头。
 */

#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/plugin_config.h"
#include "session/session_types.h"

namespace grt {

/// 导出结果
struct ExportResult {
	bool ok = false;         ///< 是否导出成功（含自检通过、两文件均已原子落盘）
	std::string error;       ///< 失败原因（成功时为空）
	std::string outputPath;  ///< 操作数据 JSON（<base>.json）绝对路径（UTF-8）
	std::string keymapPath;  ///< keymap 副本（<base>_keymap.json）绝对路径（UTF-8）
	/// 非阻断性告警（如 keymap 未覆盖某操作事件、视频文件不存在等），
	/// 告警存在时 ok 仍可为 true，由上层决定是否提示用户。
	std::vector<std::string> warnings;
};

/**
 * @brief 导出一次录制会话产生的全部工件。
 *
 * @param context  会话固定上下文（目录/时间锚点/目标/录制方案）。
 * @param artifacts 会话工件（spool 路径/视频路径/三类事件/计数）。
 * @param keymap   本次会话使用的 keymap 快照（规范名 -> KeymapEntry）。
 * @return 导出结果；数据非法（NaN/Inf、区间倒挂/重叠）或落盘/自检失败时
 *         ok=false 且 error 含具体原因（含事件索引），不会留下半截文件。
 */
ExportResult ExportSessionArtifacts(
    const SessionContext& context,
    const SessionArtifacts& artifacts,
    const std::map<std::string, KeymapEntry>& keymap);

} // namespace grt
