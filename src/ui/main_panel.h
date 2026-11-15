/**
 * @file main_panel.h
 * @brief Dock 主面板：窗口选择 / 鼠标加速准入 / 录制控制 / 实时状态 /
 *        打点 + “录制参数”“历史”“日志”三个选项卡。
 *
 * 线程模型（重要）：
 *   SessionManager 的 OnStateChanged / OnElapsedTick 回调在其【处理线程】
 *   触发。面板向 SessionManager 注册的 lambda 只做一件事：把 POD 参数
 *   emit 到本对象的 Qt 信号；信号经 Qt::QueuedConnection 自动转投到
 *   面板所在的主线程后再更新控件，回调内绝不直接碰 QWidget。
 *   析构时把 SessionManager 的两个回调置空，避免悬空调用。
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <QComboBox>
#include <QString>
#include <QWidget>

#include "capture/window_enumerator.h" // grt::WindowInfo（Q_DECLARE_METATYPE）

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTimer;

Q_DECLARE_METATYPE(grt::WindowInfo)

namespace grt {
enum class SessionState;
}

namespace grt::ui {

class AsyncVerifyRunner;
class HistoryPanel;
class KeymapPanel;
class SettingsPanel;

/**
 * @brief 窗口选择下拉框：展开时自动重新枚举窗口，保证列表最新。
 *
 * 不声明 Q_OBJECT（无自有信号/槽），重写 showPopup 时通过外部注入的
 * 刷新回调工作，避免额外 moc 目标。
 */
class RefreshOnPopupComboBox : public QComboBox {
public:
	explicit RefreshOnPopupComboBox(QWidget* parent = nullptr)
	    : QComboBox(parent)
	{
	}

	void SetRefreshCallback(std::function<void()> callback)
	{
		refresh_ = std::move(callback);
	}

protected:
	void showPopup() override
	{
		if (refresh_) {
			refresh_();
		}
		QComboBox::showPopup();
	}

private:
	std::function<void()> refresh_;
};

/// Dock 主面板（所有控件均在 Qt 主线程创建与访问）
class MainPanel : public QWidget {
	Q_OBJECT

public:
	explicit MainPanel(QWidget* parent = nullptr);
	~MainPanel() override;

signals:
	// —— 处理线程 -> 主线程 的转投信号（槽内才允许更新控件）——
	void StateChangedReceived(int stateValue);
	void ElapsedTickReceived(double seconds);
	void LogReceived(const QString& text, int level);

private slots:
	// —— 控制区 ——
	void RefreshWindowList();                // 重新枚举窗口并重建下拉项
	void OnWindowSelectionChanged(int index);
	void RefreshMouseAccelStatus();          // 查询系统鼠标加速并更新 UI
	void OnDisableMouseAccelClicked();
	void OnToggleRecordClicked();            // 开始/停止按钮
	void OnMarkerClicked();                  // 手动打点

	// —— 转投后的 SessionManager 事件（主线程）——
	void OnStateChangedUi(int stateValue);
	void OnElapsedTickUi(double seconds);
	void OnLogUi(const QString& text, int level);

	// —— 录制后自动校验（Task 12）——
	/// 启动指定会话的后台校验；begin=true 时先把历史状态置为“校验中…”
	void StartSessionVerify(const std::string& sessionId,
	                        const std::string& dir,
	                        const std::string& videoName,
	                        bool markPending);
	/// 后台校验完成回调（主线程）：写回历史、刷新表格、输出日志
	void OnVerifyFinished(const std::string& sessionId, bool ok,
	                      const std::string& status,
	                      const std::string& detail);
	/// 历史页“重新校验”：按会话 ID 从历史取定位信息并启动校验
	void OnReverifyRequested(const std::string& sessionId);
	/// 录制刚结束：取最新一条历史（信息完整时）启动后台校验
	void StartVerifyForLatestSession();

private:
	/// 搭建主控制区（窗口选择/鼠标加速/启停/状态/打点）
	void BuildControlPage();
	/// 注册 SessionManager 回调（处理线程 -> Qt 信号 -> 主线程）
	void RegisterSessionCallbacks();
	/// 依据（加速状态/会话状态/是否选中窗口）刷新开始按钮可用性与轮询
	void UpdateStartEnabled();
	/// 状态变化时刷新全部控件外观（按钮文案、参数页锁定、历史追加等）
	void ApplySessionState(int stateValue);
	/// 刷新实时状态标签（分辨率/帧率/码率/编码器/容器）
	void UpdatePlanSummary();
	/// 读取当前下拉框选中的窗口；未选中返回 std::nullopt
	std::optional<WindowInfo> SelectedWindow() const;
	/// 录制完成（回到 Idle）时依据待提交信息写一条历史
	void CommitHistoryIfNeeded();
	/// 面板显示时刷新窗口列表/加速状态并管理轮询定时器
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;
	/// mm:ss / h:mm:ss 格式化
	static QString FormatElapsed(double seconds);

	// —— 主控制区控件 ——
	RefreshOnPopupComboBox* windowCombo_ = nullptr;
	QPushButton* refreshWindowButton_ = nullptr;
	QLabel* accelStatusLabel_ = nullptr;
	QPushButton* disableAccelButton_ = nullptr;
	QPushButton* toggleRecordButton_ = nullptr;
	QPushButton* markerButton_ = nullptr;
	QLabel* elapsedLabel_ = nullptr;
	QLabel* planSummaryLabel_ = nullptr;
	QLabel* below1080Label_ = nullptr;

	// —— 选项卡 ——
	QTabWidget* tabs_ = nullptr;
	SettingsPanel* settingsPanel_ = nullptr;
	KeymapPanel* keymapPanel_ = nullptr;
	HistoryPanel* historyPanel_ = nullptr;
	QPlainTextEdit* logView_ = nullptr;

	/// 录制后三件套校验（ffprobe + JSON 校验）后台执行器
	std::unique_ptr<AsyncVerifyRunner> verifyRunner_;

	// —— 定时器 / 状态 ——
	QTimer* accelPollTimer_ = nullptr;   // 空闲时 2s 轮询鼠标加速
	bool accelOn_ = true;               // 最近一次查询到的鼠标加速状态
	bool sessionBusy_ = false;          // 当前是否处于非 Idle 状态
	std::uint64_t logHandle_ = 0;       // LogBus 订阅句柄（0=未订阅）

	/// 本次会话开始时记录的信息，回到 Idle 后用于补全历史条目
	struct PendingRecord {
		bool active = false;
		WindowInfo window;
		double elapsedSeconds = 0.0;
		QString startUtcIso;
	};
	PendingRecord pending_;
};

} // namespace grt::ui
