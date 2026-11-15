/**
 * @file overlay_controller.h
 * @brief 半透明悬浮状态窗（Task 9）：置顶 / 点击穿透 / 不抢焦点 / 不被录制。
 *
 * 对外只提供 grt::ui 命名空间下的一组自由函数（InitOverlay /
 * ShutdownOverlay / SetOverlayVisible / ToggleOverlay / IsOverlayVisible /
 * ApplyOverlaySettings 及三个事件转发入口），供 dock_controller 接线、
 * settings_panel 实时生效、Task 11 热键切换显隐使用。
 *
 * 生命周期：
 *   - RegisterDock 在 MainPanel 创建后于 Qt 主线程调用 InitOverlay()；
 *   - UnregisterDock 在销毁 Dock/MainPanel 之前调用 ShutdownOverlay()，
 *     保证先停定时器、断开一切外部来源，再释放窗口。
 *
 * 线程模型：
 *   SessionManager 的回调槽位（单一 std::function）已被 MainPanel 占用，
 *   悬浮窗【绝不】再调 SetStateChangedCallback 抢占；改为由 MainPanel 在
 *   其主线程槽（OnStateChangedUi / OnElapsedTickUi / OnLogUi）内调用本
 *   文件的 OnSessionStateChanged / OnElapsedTick / OnLog 转发。因此本模块
 *   所有方法都假定在 Qt 主线程调用。
 *
 * 不被录制（SubTask 9.2）：
 *   HWND 就绪后调用 SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)；
 *   通过 ntdll!RtlGetVersion（不受 manifest 版本谎报影响）判定
 *   Windows 10 build 19041 及以上才使用 EXCLUDEFROMCAPTURE，否则回退
 *   WDA_MONITOR。仅支持 WDA_MONITOR 时，录制中窗口会被录成黑块，因此
 *   进入 Recording/Finalizing 时自动隐藏、回到 Idle 后按可见意图恢复，
 *   并在窗口中固定显示橙色告警说明该行为。
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QShowEvent;
class QTimer;

namespace grt::ui {

// ---- 对外自由函数（Task 11 热键直接对接以下签名）--------------------------

/// 创建悬浮窗并按 OverlaySettings 应用透明度/位置/显隐。幂等，须在主线程。
void InitOverlay();

/// 停止全部定时器、隐藏并销毁悬浮窗。幂等，须在主线程。
void ShutdownOverlay();

/**
 * @brief 运行时显示/隐藏悬浮窗（热键/Dock 复选框入口）。
 *
 * 只改变运行时可见意图，不写回 OverlaySettings.enabled；显隐过程绝不
 * 激活窗口、不抢占前台焦点。WDA_MONITOR 回退期间即使意图为可见，录制中
 * 仍会被强制隐藏，回到 Idle 后自动恢复。
 */
void SetOverlayVisible(bool visible);

/// 在当前【实际可见】状态基础上切换（SetOverlayVisible(!IsOverlayVisible())）。
void ToggleOverlay();

/**
 * @brief 可从【任意线程】调用的悬浮窗切换（全局热键轮询线程入口）。
 *
 * 内部通过 Qt 排队调用把 ToggleOverlay 投递到主线程执行，不触碰 QWidget，
 * 因此不会导致游戏失焦。悬浮窗尚未初始化时调用安全空转。
 */
void ToggleOverlayFromAnyThread();

/// 窗口当前是否实际可见（被录制回退强制隐藏时返回 false）。
bool IsOverlayVisible();

/// 从 PluginConfig 重读 enabled/opacity/posX/posY 并立即应用。
void ApplyOverlaySettings();

/// MainPanel 主线程状态槽转发（stateValue 为 grt::SessionState 的 int 值）。
void OnSessionStateChanged(int stateValue);

/// MainPanel 主线程时长槽转发（秒）。
void OnElapsedTick(double seconds);

/// MainPanel 主线程日志槽转发（OBS blog 级别：INFO=300/WARN=400/ERROR=500）。
void OnLog(const QString& text, int level);

// ---- 悬浮窗本体（Q_OBJECT 置于头文件，由 CMake AUTOMOC 扫描）---------------

/**
 * @brief 无边框、半透明、置顶、点击穿透、不接受焦点的状态窗口。
 *
 * Win32 扩展样式在 HWND 创建后叠加：
 *   WS_EX_LAYERED | WS_EX_TRANSPARENT（点击穿透）
 *   | WS_EX_NOACTIVATE（不抢焦点/前台）| WS_EX_TOOLWINDOW（不入任务栏）。
 */
class OverlayWindow : public QWidget {
	Q_OBJECT

public:
	explicit OverlayWindow(QWidget* parent = nullptr);
	~OverlayWindow() override;

	/// 从 PluginConfig 重读并应用 enabled 意图/不透明度/位置
	void ApplySettings();

	/**
	 * @brief 运行时设置“用户希望可见”意图（不写配置），随后立即按录制
	 *        回退状态重算实际显隐；显示过程不激活、不抢焦点。
	 */
	void SetDesiredVisible(bool visible);

	/// 设置会话状态（0=Idle 1=Preparing 2=Recording 3=Finalizing）
	void SetSessionState(int stateValue);
	/// 刷新已录时长显示（mm:ss）
	void SetElapsed(double seconds);
	/// 追加一条按级别着色的日志（自动裁掉 "[GameRecordTool] " 前缀）
	void AppendLog(const QString& text, int level);

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;
	bool nativeEvent(const QByteArray& eventType, void* message,
	                 qintptr* result) override;

private slots:
	/// 1s 周期：置顶兜底（全屏切换后仍保持）+ 刷新录制方案行
	void OnOneSecondTick();
	/// 2s 周期（仅窗口可见时）：轮询鼠标加速准入告警
	void OnPollMouseAccel();

private:
	/// 叠加 Win32 扩展样式并设置采集排除（幂等，HWND 就绪后调用）
	void ConfigureNative();
	/// SetWindowPos(HWND_TOPMOST, SWP_NOACTIVATE) 兜底置顶
	void KeepTopMost();
	/// 刷新录制方案行（SessionManager::GetRecordingInfo 快照）
	void RefreshPlanRow();
	/// 刷新状态行（圆点颜色/文案/时长样式）
	void RefreshStatusRow();
	/// 刷新鼠标加速黄色告警行的显隐
	void RefreshAccelWarning();
	/// 刷新橙色采集排除回退告警行的显隐
	void RefreshCaptureWarning();
	/**
	 * @brief 按“用户可见意图 && 非录制回退强制隐藏”重算实际显隐。
	 * @param show  true=SW_SHOWNOACTIVATE；隐藏走 hide()。
	 */
	void RecomputeEffectiveVisibility();

	HWND hwnd_ = nullptr;
	bool nativeConfigured_ = false;  ///< Win32 扩展样式是否已叠加
	/// 采集排除能力：true=WDA_EXCLUDEFROMCAPTURE 可用；
	/// false=系统不支持或设置失败，仅 WDA_MONITOR，录制中必须自动隐藏
	bool excludeFromCaptureSupported_ = true;

	// —— 内容控件 ——
	QLabel* statusDotLabel_ = nullptr;
	QLabel* statusTextLabel_ = nullptr;
	QLabel* elapsedLabel_ = nullptr;
	QLabel* planLabel_ = nullptr;
	QLabel* accelWarnLabel_ = nullptr;
	QLabel* captureWarnLabel_ = nullptr;
	QPlainTextEdit* logView_ = nullptr;

	// —— 定时器 ——
	QTimer* oneSecondTimer_ = nullptr;  ///< 置顶兜底 + 方案行刷新
	QTimer* accelPollTimer_ = nullptr;  ///< 2s 鼠标加速轮询（仅可见时运行）

	// —— 状态 ——
	int sessionState_ = 0;             ///< grt::SessionState 的 int 值
	bool userWantsVisible_ = true;     ///< 配置 enabled / SetOverlayVisible 的意图
};

} // namespace grt::ui
