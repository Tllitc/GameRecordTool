/**
 * @file dock_controller.h
 * @brief Dock 的注册/注销入口（plugin-main 在 FINISHED_LOADING / unload 调用）。
 *
 * 接线约定：
 *   - plugin-main 收到 OBS_FRONTEND_EVENT_FINISHED_LOADING 后调用
 *     grt::ui::RegisterDock()；
 *   - obs_module_unload 中调用 grt::ui::UnregisterDock()（内部先从 OBS
 *     移除 dock，再删除其与主面板，析构链会注销 SessionManager 回调）。
 *
 * 注册通过 obs_frontend_add_custom_qdock(id, QDockWidget*) 完成：
 *   - QDockWidget 子类构造时 parent=nullptr，所有权移交 OBS（移除 dock
 *     时由前端 delete）；本模块同时保留裸指针仅用于卸载时兜底删除。
 *   - objectName / dock id 固定为 "GameRecordTool"，窗口标题为中文
 *     “游戏录制工具”。
 */

#pragma once

namespace grt::ui {

/// OBS Dock 的固定 id / objectName
inline constexpr const char* kDockId = "GameRecordTool";

/**
 * @brief 创建 Dock 与主面板并注册到 OBS 前端。
 *
 * 必须在 OBS 前端 FINISHED_LOADING 之后、Qt 主线程调用。重复调用为空操作。
 */
void RegisterDock();

/**
 * @brief 从 OBS 前端移除 Dock 并释放资源。
 *
 * 先 obs_frontend_remove_dock（前端通常会同步删除 dock），随后对残留
 * 指针兜底 delete；MainPanel 析构时注销 SessionManager 回调并退订日志。
 */
void UnregisterDock();

} // namespace grt::ui
