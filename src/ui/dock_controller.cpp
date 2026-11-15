/**
 * @file dock_controller.cpp
 * @brief Dock 注册/注销实现。
 *
 * GameRecordDock 不声明 Q_OBJECT（没有自有信号/槽，仅承载 MainPanel），
 * 以减少一个 moc 目标。dock 构造时 parent=nullptr，随后通过
 * obs_frontend_add_custom_qdock 移交 OBS 前端管理。
 */

#include "ui/dock_controller.h"

#include <QDockWidget>
#include <QPointer>
#include <QString>

#include <obs-frontend-api.h>

#include "ui/main_panel.h"
#include "ui/overlay_controller.h"

namespace grt::ui {

namespace {

/// 承载 MainPanel 的 Dock（无 Q_OBJECT：无自有信号/槽）
class GameRecordDock : public QDockWidget {
public:
	explicit GameRecordDock(QWidget* parent = nullptr)
	    : QDockWidget(parent)
	{
		setObjectName(QStringLiteral("GameRecordTool"));
		setWindowTitle(QStringLiteral("游戏录制工具"));
		setAllowedAreas(Qt::AllDockWidgetAreas);
		auto* panel = new MainPanel(this);
		setWidget(panel);
	}
};

// QPointer 用于观察 OBS 前端在 remove_dock 时是否已同步销毁 dock；
// 若仍存活，UnregisterDock 做兜底删除。
QPointer<GameRecordDock> g_dock;

} // namespace

void RegisterDock()
{
	if (g_dock) {
		// 已注册，避免重复添加
		return;
	}

	// parent=nullptr：所有权随后移交 OBS 前端
	auto* dock = new GameRecordDock(nullptr);
	const bool added = obs_frontend_add_custom_qdock(
	    kDockId, static_cast<void*>(dock));
	if (added) {
		g_dock = dock;
		// MainPanel 已随 Dock 创建，且 RegisterDock 运行于 Qt 主线程
		// （FINISHED_LOADING），此刻初始化悬浮窗安全；事件转发依赖
		// MainPanel 的主线程槽，必须在其创建之后进行。
		InitOverlay();
	} else {
		// 注册失败（理论上不会发生）：自行销毁，避免泄漏
		delete dock;
	}
}

void UnregisterDock()
{
	if (!g_dock) {
		return;
	}

	// 必须在 Dock/MainPanel 销毁之前关闭悬浮窗：先停掉悬浮窗定时器并
	// 释放窗口，避免析构链期间仍有事件转发访问已销毁对象。
	ShutdownOverlay();

	// 前端在 remove 路径中通常会 delete dock；g_dock（QPointer）随之清空
	obs_frontend_remove_dock(kDockId);

	if (g_dock) {
		// 兜底：某些前端实现未取得所有权时由我们删除。
		// MainPanel 析构链会注销 SessionManager 回调并退订日志。
		delete g_dock;
	}
}

} // namespace grt::ui
