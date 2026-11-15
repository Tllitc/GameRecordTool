/**
 * @file main_panel.cpp
 * @brief Dock 主面板实现（纯代码布局，无 .ui 文件）。
 *
 * 布局自上而下：
 *   1) 窗口选择行：下拉框（展开时自动重新枚举）+ “刷新”按钮；
 *   2) 鼠标加速状态区：红/绿醒目状态 + “一键关闭鼠标加速”按钮；
 *   3) “开始录制 / 停止录制”主按钮；
 *   4) 实时状态行：已录时长（4Hz tick）+ 分辨率/帧率/码率/编码器/容器
 *      + 低于 1080p 黄色告警；
 *   5) “手动打点”按钮；
 *   6) 选项卡：录制参数 / 历史 / 日志。
 *
 * 线程模型见 main_panel.h 头注：SessionManager 回调仅 emit 信号，
 * 经 Qt::QueuedConnection 转投到本对象所在主线程后才操作控件。
 */

#include "ui/main_panel.h"

#include <cmath>
#include <string>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include "capture/output_configurator.h"
#include "core/log.h"
#include "core/log_bus.h"
#include "core/plugin_config.h"
#include "input/mouse_accel_service.h"
#include "session/session_manager.h"
#include "ui/async_verify_runner.h"
#include "ui/history_panel.h"
#include "ui/history_store.h"
#include "ui/keymap_panel.h"
#include "ui/overlay_controller.h"
#include "ui/settings_panel.h"

namespace grt::ui {

namespace {

// 鼠标加速轮询间隔（毫秒）：任务要求 2 秒
constexpr int kAccelPollIntervalMs = 2000;

/// 把 UTF-16 字符串转成 UTF-8 std::string（落历史 JSON 用）
std::string ToUtf8(const std::wstring& value)
{
	return QString::fromStdWString(value).toStdString();
}

} // namespace

MainPanel::MainPanel(QWidget* parent)
    : QWidget(parent)
{
	BuildControlPage();
	RegisterSessionCallbacks();

	// 转投信号统一显式指定 QueuedConnection：即使发送方位于处理线程，
	// 槽也保证在本对象所在的 Qt 主线程执行。
	connect(this, &MainPanel::StateChangedReceived, this,
	        &MainPanel::OnStateChangedUi, Qt::QueuedConnection);
	connect(this, &MainPanel::ElapsedTickReceived, this,
	        &MainPanel::OnElapsedTickUi, Qt::QueuedConnection);
	connect(this, &MainPanel::LogReceived, this, &MainPanel::OnLogUi,
	        Qt::QueuedConnection);

	// 初始数据填充（构造发生在主线程）
	RefreshWindowList();
	RefreshMouseAccelStatus();
	historyPanel_->Refresh();
	UpdatePlanSummary();

	// 空闲时 2 秒轮询一次鼠标加速状态（录制中停止轮询）
	accelPollTimer_ = new QTimer(this);
	accelPollTimer_->setInterval(kAccelPollIntervalMs);
	connect(accelPollTimer_, &QTimer::timeout, this,
	        &MainPanel::RefreshMouseAccelStatus);
	if (SessionManager::Instance().State() == SessionState::Idle) {
		accelPollTimer_->start();
	}

	// 面板创建时 SessionManager 可能已经在录制中（热插拔场景），
	// 按当前状态刷新一次控件外观。
	ApplySessionState(static_cast<int>(SessionManager::Instance().State()));
}

MainPanel::~MainPanel()
{
	// 注销处理线程回调，避免回调访问正在析构的对象
	SessionManager::Instance().SetStateChangedCallback(nullptr);
	SessionManager::Instance().SetElapsedTickCallback(nullptr);
	LogBus::Instance().Unsubscribe(logHandle_);
	logHandle_ = 0;
}

// ---------------------------------------------------------------------------
// 界面搭建
// ---------------------------------------------------------------------------

void MainPanel::BuildControlPage()
{
	auto* rootLayout = new QVBoxLayout(this);

	// 上部控制区（1~5 项）放入滚动视口：Dock 被压缩时出现纵向滚动条，
	// 避免“开始录制”等关键控件被裁掉；选项卡自带滚动，不纳入此区。
	auto* controls = new QWidget;
	auto* controlsLayout = new QVBoxLayout(controls);
	controlsLayout->setContentsMargins(0, 0, 0, 0);

	// —— 1) 窗口选择行 ——
	auto* windowRow = new QHBoxLayout();
	windowRow->addWidget(new QLabel(QStringLiteral("录制窗口："), this), 0);
	windowCombo_ = new RefreshOnPopupComboBox(this);
	windowCombo_->setToolTip(QStringLiteral(
	    "展开列表时会自动重新枚举。\n"
	    "录制将严格采集此处选中的窗口；开始录制时若该窗口已关闭，"
	    "会提示重新选择。"));
	windowCombo_->SetRefreshCallback([this] { RefreshWindowList(); });
	windowRow->addWidget(windowCombo_, 1);
	refreshWindowButton_ = new QPushButton(QStringLiteral("刷新"), this);
	windowRow->addWidget(refreshWindowButton_, 0);
	controlsLayout->addLayout(windowRow);

	// —— 2) 鼠标加速状态区（醒目）——
	auto* accelBox = new QGroupBox(QStringLiteral("鼠标加速（增强指针精确度）"), this);
	auto* accelLayout = new QVBoxLayout(accelBox);
	accelStatusLabel_ = new QLabel(accelBox);
	accelStatusLabel_->setAlignment(Qt::AlignCenter);
	accelStatusLabel_->setMinimumHeight(34);
	accelLayout->addWidget(accelStatusLabel_);

	disableAccelButton_ = new QPushButton(
	    QStringLiteral("一键关闭鼠标加速"), accelBox);
	disableAccelButton_->setToolTip(QStringLiteral(
	    "关闭后不会自动恢复，如需恢复请到 Windows 设置"
	    "（设置→蓝牙和设备→鼠标→其他鼠标选项→指针选项）手动开启"));
	accelLayout->addWidget(disableAccelButton_);

	accelLayout->addWidget(new QLabel(
	    QStringLiteral("注意：关闭后不会自动恢复，如需恢复请到 Windows 设置"
	                   "（设置→蓝牙和设备→鼠标→其他鼠标选项→指针选项）"
	                   "手动开启。"),
	    accelBox));
	controlsLayout->addWidget(accelBox);

	// —— 3) 开始/停止按钮 ——
	toggleRecordButton_ = new QPushButton(QStringLiteral("开始录制"), this);
	{
		QFont bigFont = toggleRecordButton_->font();
		bigFont.setPointSize(bigFont.pointSize() + 2);
		bigFont.setBold(true);
		toggleRecordButton_->setFont(bigFont);
	}
	toggleRecordButton_->setMinimumHeight(40);
	controlsLayout->addWidget(toggleRecordButton_);

	// —— 4) 实时状态行 ——
	elapsedLabel_ = new QLabel(QStringLiteral("时长 00:00"), this);
	elapsedLabel_->setAlignment(Qt::AlignCenter);
	{
		QFont timeFont = elapsedLabel_->font();
		timeFont.setPointSize(timeFont.pointSize() + 3);
		timeFont.setBold(true);
		elapsedLabel_->setFont(timeFont);
	}
	controlsLayout->addWidget(elapsedLabel_);

	planSummaryLabel_ = new QLabel(this);
	planSummaryLabel_->setWordWrap(true);
	planSummaryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	controlsLayout->addWidget(planSummaryLabel_);

	below1080Label_ = new QLabel(
	    QStringLiteral("⚠ 目标分辨率低于 1080p，码率已按 1080p 档位兜底"),
	    this);
	below1080Label_->setStyleSheet(QStringLiteral("color: #b58900; font-weight: bold;"));
	below1080Label_->setVisible(false);
	controlsLayout->addWidget(below1080Label_);

	auto* restartHint = new QLabel(
	    QStringLiteral("编码器/编码格式变更后需重启 OBS 生效"), this);
	restartHint->setStyleSheet(QStringLiteral("color: #8a6d00;"));
	restartHint->setWordWrap(true);
	controlsLayout->addWidget(restartHint);

	// —— 5) 打点按钮 ——
	markerButton_ = new QPushButton(QStringLiteral("手动打点…"), this);
	markerButton_->setEnabled(false);
	controlsLayout->addWidget(markerButton_);

	// 上部控制区套滚动视口：内容高度超出视口时自动出现纵向滚动条，
	// 视口本身不抢占选项卡空间（stretch=0，可被压缩到最小）。
	auto* controlsScroll = new QScrollArea(this);
	controlsScroll->setWidget(controls);
	controlsScroll->setWidgetResizable(true);
	controlsScroll->setFrameShape(QFrame::NoFrame);
	controlsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	controlsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	rootLayout->addWidget(controlsScroll, 0);

	// —— 6) 选项卡：录制参数 / 按键映射 / 历史 / 日志 ——
	tabs_ = new QTabWidget(this);
	settingsPanel_ = new SettingsPanel(tabs_);
	keymapPanel_ = new KeymapPanel(tabs_);
	historyPanel_ = new HistoryPanel(tabs_);
	logView_ = new QPlainTextEdit(tabs_);
	logView_->setReadOnly(true);
	logView_->setMaximumBlockCount(500);
	logView_->setPlaceholderText(QStringLiteral("暂无日志"));
	tabs_->addTab(settingsPanel_, QStringLiteral("录制参数"));
	tabs_->addTab(keymapPanel_, QStringLiteral("按键映射"));
	tabs_->addTab(historyPanel_, QStringLiteral("历史"));
	tabs_->addTab(logView_, QStringLiteral("日志"));
	rootLayout->addWidget(tabs_, 1);

	// 录制后三件套后台校验器（生命周期随面板）
	verifyRunner_ = std::make_unique<AsyncVerifyRunner>(this);
	connect(historyPanel_, &HistoryPanel::ReverifyRequested, this,
	        &MainPanel::OnReverifyRequested);

	// —— 信号连接（全部为主线程内的普通连接）——
	connect(refreshWindowButton_, &QPushButton::clicked, this,
	        &MainPanel::RefreshWindowList);
	connect(windowCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &MainPanel::OnWindowSelectionChanged);
	connect(disableAccelButton_, &QPushButton::clicked, this,
	        &MainPanel::OnDisableMouseAccelClicked);
	connect(toggleRecordButton_, &QPushButton::clicked, this,
	        &MainPanel::OnToggleRecordClicked);
	connect(markerButton_, &QPushButton::clicked, this,
	        &MainPanel::OnMarkerClicked);
}

void MainPanel::RegisterSessionCallbacks()
{
	// 回调内只做 POD -> Qt 信号的转发，绝不触碰任何控件。
	SessionManager::Instance().SetStateChangedCallback(
	    [this](SessionState state) {
		    emit StateChangedReceived(static_cast<int>(state));
	    });
	SessionManager::Instance().SetElapsedTickCallback(
	    [this](double seconds) {
		    emit ElapsedTickReceived(seconds);
	    });
	logHandle_ = SessionManager::Instance().SubscribeLog(
	    [this](const LogEntry& entry) {
		    emit LogReceived(QString::fromStdString(entry.text), entry.level);
	    });

	// 订阅前的近期日志先回填一次（SnapshotRecent 可在任意线程调用）
	for (const LogEntry& entry : LogBus::Instance().SnapshotRecent()) {
		OnLogUi(QString::fromStdString(entry.text), entry.level);
	}
}

// ---------------------------------------------------------------------------
// 窗口选择
// ---------------------------------------------------------------------------

void MainPanel::RefreshWindowList()
{
	// WindowEnumerator 仅做 Win32 枚举，无 libobs/线程限制。
	const std::vector<WindowInfo> windows = WindowEnumerator::Enumerate();

	// 尽量保持当前选择（按 HWND 匹配）
	const HWND previousHwnd =
	    windowCombo_->currentIndex() >= 0 && windowCombo_->currentData().isValid()
	        ? windowCombo_->currentData().value<WindowInfo>().hwnd
	        : nullptr;

	{
		// 重建期间屏蔽 currentIndexChanged，避免中间态触发状态刷新
		const QSignalBlocker blocker(windowCombo_);
		windowCombo_->clear();

		int selectIndex = -1;
		for (size_t i = 0; i < windows.size(); ++i) {
			const WindowInfo& info = windows[i];
			const QString exe = QString::fromStdWString(info.exeName);
			const QString title = QString::fromStdWString(info.title);
			const QString text =
			    QStringLiteral("%1 | %2 | %3×%4")
			        .arg(exe, title)
			        .arg(info.clientWidth)
			        .arg(info.clientHeight);
			windowCombo_->addItem(text, QVariant::fromValue(info));
			if (info.hwnd == previousHwnd) {
				selectIndex = static_cast<int>(i);
			}
		}
		if (selectIndex >= 0) {
			windowCombo_->setCurrentIndex(selectIndex);
		} else if (!windows.empty()) {
			windowCombo_->setCurrentIndex(0);
		}
	}

	OnWindowSelectionChanged(windowCombo_->currentIndex());
}

void MainPanel::OnWindowSelectionChanged(int index)
{
	Q_UNUSED(index);
	UpdatePlanSummary();
	UpdateStartEnabled();
}

std::optional<WindowInfo> MainPanel::SelectedWindow() const
{
	const QVariant data = windowCombo_->currentData();
	if (!data.isValid() || !data.canConvert<WindowInfo>()) {
		return std::nullopt;
	}
	return data.value<WindowInfo>();
}

// ---------------------------------------------------------------------------
// 鼠标加速状态
// ---------------------------------------------------------------------------

void MainPanel::RefreshMouseAccelStatus()
{
	accelOn_ = MouseAccelService::IsEnhancePointerPrecisionOn();
	if (accelOn_) {
		accelStatusLabel_->setText(
		    QStringLiteral("● 鼠标加速：已开启（必须关闭后才能录制）"));
		accelStatusLabel_->setStyleSheet(
		    QStringLiteral("background-color: #c62828; color: white;"
	                       " font-weight: bold; border-radius: 4px;"));
		disableAccelButton_->setEnabled(true);
	} else {
		accelStatusLabel_->setText(
		    QStringLiteral("● 鼠标加速：已关闭（可以开始录制）"));
		accelStatusLabel_->setStyleSheet(
		    QStringLiteral("background-color: #2e7d32; color: white;"
	                       " font-weight: bold; border-radius: 4px;"));
		disableAccelButton_->setEnabled(false);
	}
	UpdateStartEnabled();
}

void MainPanel::OnDisableMouseAccelClicked()
{
	const QMessageBox::StandardButton answer = QMessageBox::warning(
	    this, QStringLiteral("关闭鼠标加速"),
	    QStringLiteral("将立即关闭系统鼠标加速（增强指针精确度）。\n\n"
	                   "关闭后不会自动恢复，如需恢复请到 Windows 设置"
	                   "（设置→蓝牙和设备→鼠标→其他鼠标选项→指针选项）"
	                   "手动开启。\n\n是否继续？"),
	    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (answer != QMessageBox::Yes) {
		return;
	}

	if (MouseAccelService::DisableEnhancePointerPrecision()) {
		RefreshMouseAccelStatus();
	} else {
		QMessageBox::critical(
		    this, QStringLiteral("关闭失败"),
		    QStringLiteral("写入系统鼠标设置失败，请以管理员身份检查 OBS 权限"
	                       "或在 Windows 设置中手动关闭。"));
	}
}

// ---------------------------------------------------------------------------
// 录制控制
// ---------------------------------------------------------------------------

void MainPanel::OnToggleRecordClicked()
{
	SessionManager& manager = SessionManager::Instance();
	if (manager.State() == SessionState::Idle) {
		// 开始前再查一次加速状态，尽量贴近准入时刻
		RefreshMouseAccelStatus();
		// 以下拉框选定的窗口作为严格采集目标（未选择时由会话层回退前台窗口）
		const std::optional<WindowInfo> selected = SelectedWindow();
		const std::string error = manager.Start(selected);
		if (!error.empty()) {
			QMessageBox::warning(
			    this, QStringLiteral("无法开始录制"),
			    QString::fromStdString(error));
			RefreshMouseAccelStatus();
		}
	} else if (manager.State() == SessionState::Recording) {
		manager.Stop();
	}
	// Preparing/Finalizing 期间按钮已禁用，不会进入本槽。
}

void MainPanel::OnMarkerClicked()
{
	bool ok = false;
	const QString label = QInputDialog::getText(
	    this, QStringLiteral("手动打点"),
	    QStringLiteral("标记内容："), QLineEdit::Normal,
	    QStringLiteral("标记点"), &ok);
	if (!ok) {
		return;
	}
	const std::string error =
	    SessionManager::Instance().AddManualMarker(label.toStdString());
	if (!error.empty()) {
		QMessageBox::warning(
		    this, QStringLiteral("打点失败"),
		    QString::fromStdString(error));
	}
}

// ---------------------------------------------------------------------------
// SessionManager 事件（已转投到主线程）
// ---------------------------------------------------------------------------

void MainPanel::OnStateChangedUi(int stateValue)
{
	ApplySessionState(stateValue);
	// 转发给悬浮窗（悬浮窗不占用 SessionManager 的单一回调槽）
	OnSessionStateChanged(stateValue);
}

void MainPanel::OnElapsedTickUi(double seconds)
{
	pending_.elapsedSeconds = seconds;
	elapsedLabel_->setText(QStringLiteral("时长 %1").arg(FormatElapsed(seconds)));
	OnElapsedTick(seconds);
}

void MainPanel::OnLogUi(const QString& text, int level)
{
	// OBS 日志级别：WARNING=400，ERROR=500
	QString line = text;
	if (level >= 500) {
		line = QStringLiteral("[错误] ") + line;
	} else if (level >= 400) {
		line = QStringLiteral("[警告] ") + line;
	}
	logView_->appendPlainText(line);
	OnLog(text, level);
}

// ---------------------------------------------------------------------------
// 状态刷新
// ---------------------------------------------------------------------------

void MainPanel::UpdateStartEnabled()
{
	const bool idle =
	    SessionManager::Instance().State() == SessionState::Idle;
	const bool hasWindow =
	    windowCombo_->currentData().isValid() &&
	    windowCombo_->currentData().canConvert<WindowInfo>();
	const bool blockedByAccel = accelOn_;

	toggleRecordButton_->setEnabled(
	    idle && hasWindow && !blockedByAccel);
}

void MainPanel::ApplySessionState(int stateValue)
{
	const auto state = static_cast<SessionState>(stateValue);
	sessionBusy_ = (state != SessionState::Idle);

	switch (state) {
	case SessionState::Idle:
		// 录制结束：提交历史、刷新各页、复位控件
		if (pending_.active) {
			CommitHistoryIfNeeded();
			pending_ = PendingRecord{};
			historyPanel_->Refresh();
			StartVerifyForLatestSession();
		}
		toggleRecordButton_->setText(QStringLiteral("开始录制"));
		toggleRecordButton_->setStyleSheet(QString());
		markerButton_->setEnabled(false);
		elapsedLabel_->setText(QStringLiteral("时长 00:00"));
		settingsPanel_->SetRecordingLocked(false);
		UpdatePlanSummary();
		RefreshWindowList();
		RefreshMouseAccelStatus();
		if (isVisible()) {
			accelPollTimer_->start();
		}
		UpdateStartEnabled();
		break;

	case SessionState::Preparing:
		pending_.active = true;
		pending_.elapsedSeconds = 0.0;
		pending_.startUtcIso =
		    QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
		if (auto selected = SelectedWindow()) {
			pending_.window = *selected;
		}
		toggleRecordButton_->setText(QStringLiteral("正在准备…"));
		toggleRecordButton_->setEnabled(false);
		markerButton_->setEnabled(false);
		settingsPanel_->SetRecordingLocked(true);
		accelPollTimer_->stop();
		break;

	case SessionState::Recording:
		pending_.active = true;
		if (pending_.startUtcIso.isEmpty()) {
			pending_.startUtcIso =
			    QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
		}
		if (auto selected = SelectedWindow()) {
			pending_.window = *selected;
		}
		toggleRecordButton_->setText(QStringLiteral("停止录制"));
		toggleRecordButton_->setStyleSheet(
		    QStringLiteral("background-color: #c62828; color: white;"));
		toggleRecordButton_->setEnabled(true);
		markerButton_->setEnabled(true);
		settingsPanel_->SetRecordingLocked(true);
		accelPollTimer_->stop();
		break;

	case SessionState::Finalizing:
		toggleRecordButton_->setText(QStringLiteral("正在收尾…"));
		toggleRecordButton_->setEnabled(false);
		markerButton_->setEnabled(false);
		settingsPanel_->SetRecordingLocked(true);
		accelPollTimer_->stop();
		break;
	}
}

void MainPanel::UpdatePlanSummary()
{
	const auto selectedOpt = SelectedWindow();
	const RecordingSettings settings =
	    PluginConfig::Instance().GetRecordingSettings();

	if (!selectedOpt) {
		planSummaryLabel_->setText(
		    QStringLiteral("未选择窗口（请在下拉框中选择要录制的游戏窗口）"));
		below1080Label_->setVisible(false);
		return;
	}

	const WindowInfo& selected = *selectedOpt;
	bool below1080p = false;
	const int bitrate = OutputConfigurator::ResolveBitrate(
	    selected.clientWidth, selected.clientHeight,
	    settings.bitrateKbps, below1080p);

	const QString codecText =
	    settings.videoCodec == "h265" ? QStringLiteral("H.265")
	    : settings.videoCodec == "h264" ? QStringLiteral("H.264")
	                                    : QStringLiteral("编码格式自动");
	const QString encoderText =
	    settings.encoder == "nvenc" ? QStringLiteral("NVENC")
	    : settings.encoder == "amf"   ? QStringLiteral("AMF")
	    : settings.encoder == "qsv"   ? QStringLiteral("QSV")
	    : settings.encoder == "x264"  ? QStringLiteral("x264")
	                                  : QStringLiteral("编码器自动");
	const QString containerText =
	    settings.crashSafe ? QStringLiteral("fMP4/mkv+remux")
	                       : QStringLiteral("MP4");

	planSummaryLabel_->setText(QStringLiteral(
	    "%1×%2 @%3fps ｜ 码率 %4 kbps ｜ %5 ｜ %6 ｜ 容器 %7")
	                               .arg(selected.clientWidth)
	                               .arg(selected.clientHeight)
	                               .arg(settings.fps)
	                               .arg(bitrate)
	                               .arg(codecText, encoderText, containerText));
	below1080Label_->setVisible(below1080p);
}

void MainPanel::CommitHistoryIfNeeded()
{
	// SessionManager 未公开最后一次会话的 SessionContext，历史路径只能
	// 取前端“最后一次录制文件”，父目录即会话目录。
	const std::string lastVideo = OutputConfigurator::LastRecordingPath();

	HistoryEntry entry;
	entry.exe = ToUtf8(pending_.window.exeName);
	entry.title = ToUtf8(pending_.window.title);
	entry.durationSeconds =
	    static_cast<int>(std::lround(pending_.elapsedSeconds));
	entry.startUtc = pending_.startUtcIso.toStdString();
	entry.verifyStatus = HistoryStore::kDefaultVerifyStatus;

	if (!lastVideo.empty()) {
		const QFileInfo videoInfo(QString::fromStdString(lastVideo));
		entry.videoName = videoInfo.fileName().toStdString();
		entry.dir = videoInfo.absolutePath().toStdString();
		entry.sessionId = videoInfo.absoluteDir().dirName().toStdString();
	}

	// 展示用本地时间（与 startUtc 同源的会话开始时刻，UTC->本地）
	const QDateTime startUtc =
	    QDateTime::fromString(pending_.startUtcIso, Qt::ISODate);
	if (startUtc.isValid()) {
		entry.time = startUtc.toLocalTime().toString(Qt::ISODate).toStdString();
	} else {
		entry.time =
		    QDateTime::currentDateTime().toString(Qt::ISODate).toStdString();
	}

	HistoryStore::Append(entry);
}

// ---------------------------------------------------------------------------
// 录制后三件套校验（Task 12）
// ---------------------------------------------------------------------------

void MainPanel::StartSessionVerify(const std::string& sessionId,
                                   const std::string& dir,
                                   const std::string& videoName,
                                   bool markPending)
{
	if (sessionId.empty() || dir.empty() || videoName.empty()) {
		return;
	}

	if (markPending) {
		// 先置“校验中…”并刷新，让用户立刻看到进行态
		HistoryStore::UpdateVerifyResult(sessionId, "校验中…", "");
		historyPanel_->Refresh();
	}

	VerifyTask task;
	task.sessionId = sessionId;
	task.dir = dir;
	task.videoName = videoName;
	verifyRunner_->Start(task,
	                     [this](const std::string& id, bool ok,
	                            const std::string& status,
	                            const std::string& detail) {
		                     OnVerifyFinished(id, ok, status, detail);
	                     });
}

void MainPanel::OnVerifyFinished(const std::string& sessionId, bool ok,
                                 const std::string& status,
                                 const std::string& detail)
{
	HistoryStore::UpdateVerifyResult(sessionId, status, detail);
	historyPanel_->Refresh();
	if (ok) {
		GRT_LOG_INFO("会话 %s 录制产物校验通过", sessionId.c_str());
	} else {
		GRT_LOG_WARN("会话 %s 录制产物校验未通过：%s", sessionId.c_str(),
		             status.c_str());
	}
}

void MainPanel::OnReverifyRequested(const std::string& sessionId)
{
	const auto entries = HistoryStore::Load();
	for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
		if (it->sessionId == sessionId) {
			if (it->dir.empty() || it->videoName.empty()) {
				GRT_LOG_WARN("会话 %s 缺少视频路径，无法重新校验",
				             sessionId.c_str());
				return;
			}
			StartSessionVerify(it->sessionId, it->dir, it->videoName, true);
			return;
		}
	}
}

void MainPanel::StartVerifyForLatestSession()
{
	// 历史按追加顺序存储，最新条目在末尾；仅在三件套定位信息齐全时触发
	const auto entries = HistoryStore::Load();
	if (entries.empty()) {
		return;
	}
	const HistoryEntry& latest = entries.back();
	StartSessionVerify(latest.sessionId, latest.dir, latest.videoName, true);
}

// ---------------------------------------------------------------------------
// 显示/隐藏与定时器
// ---------------------------------------------------------------------------

void MainPanel::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	RefreshWindowList();
	RefreshMouseAccelStatus();
	if (SessionManager::Instance().State() == SessionState::Idle) {
		accelPollTimer_->start();
	}
}

void MainPanel::hideEvent(QHideEvent* event)
{
	QWidget::hideEvent(event);
	accelPollTimer_->stop();
}

QString MainPanel::FormatElapsed(double seconds)
{
	const int total = static_cast<int>(seconds + 0.5);
	const int hours = total / 3600;
	const int minutes = (total % 3600) / 60;
	const int secs = total % 60;
	if (hours > 0) {
		return QString::asprintf("%d:%02d:%02d", hours, minutes, secs);
	}
	return QString::asprintf("%02d:%02d", minutes, secs);
}

} // namespace grt::ui
