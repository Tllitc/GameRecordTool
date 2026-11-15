/**
 * @file overlay_controller.cpp
 * @brief 半透明悬浮状态窗实现（Task 9 SubTask 9.1–9.4）。
 *
 * 不抢焦点 / 点击穿透 / 不被录制的三重保证：
 *   1) Qt 层：FramelessWindowHint | WindowStaysOnTopHint | Tool
 *      | WindowDoesNotAcceptFocus | WindowTransparentForInput，配合
 *      WA_ShowWithoutActivating；显示一律走 SW_SHOWNOACTIVATE。
 *   2) Win32 层：HWND 就绪后叠加 WS_EX_LAYERED | WS_EX_TRANSPARENT
 *      | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW，并用 HWND_TOPMOST +
 *      SWP_NOACTIVATE 周期兜底置顶（游戏全屏切换后仍置顶、不闪屏）。
 *   3) 采集排除：SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
 *      使悬浮窗在录制画面中完全不可见且可点击穿透；系统不支持时
 *      （build < 19041）回退 WDA_MONITOR，并在录制中强制隐藏
 *      （WDA_MONITOR 会把窗口录成黑块）。
 *
 * 半透明方案：不使用 WA_TranslucentBackground（全窗口 translucent 会让
 * stylesheet 的深色底也变成逐像素合成、不同 DWM/全屏场景下文字对比度不
 * 稳定），改为【不透明深色 stylesheet 底 + setWindowOpacity 整体不透明度】：
 * 控件自身按深色背景绘制，DWM 统一按 opacity 合成整窗，深浅画面上文字都
 * 有不透明深色底衬底，可读性最好。
 */

#include "ui/overlay_controller.h"

#include <algorithm>
#include <string>

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScreen>
#include <QScrollBar>
#include <QShowEvent>
#include <QString>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include "core/log.h"
#include "core/log_bus.h"
#include "core/plugin_config.h"
#include "input/mouse_accel_service.h"
#include "session/session_manager.h"

// WDA_EXCLUDEFROMCAPTURE 仅在较新的 Windows SDK 头中声明，旧头自行补定义
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef WDA_MONITOR
#define WDA_MONITOR 0x00000001
#endif

namespace grt::ui {

namespace {

// 窗口固定尺寸（宽 360，高 220：约可显示 8~12 行日志）
constexpr int kOverlayWidth = 360;
constexpr int kOverlayHeight = 220;
/// 默认位置距主屏右上角的边距（像素）
constexpr int kDefaultEdgeMargin = 40;
/// 置顶兜底/方案刷新周期（毫秒）
constexpr int kOneSecondIntervalMs = 1000;
/// 鼠标加速轮询周期（毫秒，任务要求 2s）
constexpr int kAccelPollIntervalMs = 2000;

/// 统一日志前缀（裁掉后悬浮窗内显示更简洁）
const QString kLogPrefix = QStringLiteral("[GameRecordTool] ");

/// 全局唯一窗口指针（Init/Shutdown 维护；所有方法在主线程访问）
OverlayWindow* g_overlay = nullptr;

/**
 * @brief 通过 ntdll!RtlGetVersion 获取真实系统版本。
 *
 * 不能用 VerifyVersionInfo / GetVersionEx：前者受应用 manifest 的兼容段
 * 影响会被“版本谎报”，后者在 Win8.1+ 同样谎报。RtlGetVersion 不受 manifest
 * 影响，直接读取内核报告的真实版本。
 * @param major [out] 主版本号
 * @param build [out] 内部版本号（如 19041/22000）
 * @return 查询成功返回 true；失败时调用方按保守策略处理。
 */
bool RealWindowsVersion(int& major, int& build)
{
	using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == nullptr) {
		return false;
	}
	auto pRtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
	    reinterpret_cast<void*>(
	        GetProcAddress(ntdll, "RtlGetVersion")));
	if (pRtlGetVersion == nullptr) {
		return false;
	}
	OSVERSIONINFOEXW info{};
	info.dwOSVersionInfoSize = sizeof(info);
	if (pRtlGetVersion(&info) != 0 /*STATUS_SUCCESS*/) {
		return false;
	}
	major = static_cast<int>(info.dwMajorVersion);
	build = static_cast<int>(info.dwBuildNumber);
	return true;
}

/// mm:ss / h:mm:ss 格式化（与 MainPanel 保持一致）
QString FormatElapsed(double seconds)
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

} // namespace

// ===========================================================================
// OverlayWindow
// ===========================================================================

OverlayWindow::OverlayWindow(QWidget* parent) : QWidget(parent)
{
	// —— Qt 层窗口属性 ——
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
	               Qt::Tool | Qt::WindowDoesNotAcceptFocus |
	               Qt::WindowTransparentForInput);
	// 创建/显示均不激活；不使用 WA_TranslucentBackground（见文件头注释）
	setAttribute(Qt::WA_ShowWithoutActivating, true);
	setAttribute(Qt::WA_DeleteOnClose, false);
	setObjectName(QStringLiteral("GrtOverlayWindow"));
	setFocusPolicy(Qt::NoFocus);
	setFixedSize(kOverlayWidth, kOverlayHeight);

	// —— 内容布局 ——
	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(10, 8, 10, 8);
	root->setSpacing(4);

	// 第 1 行：状态圆点 + 状态文案 + 时长
	auto* statusRow = new QHBoxLayout();
	statusRow->setSpacing(6);
	statusDotLabel_ = new QLabel(QStringLiteral("●"), this);
	statusDotLabel_->setStyleSheet(QStringLiteral("color: #9e9e9e;"));
	{
		QFont dotFont = statusDotLabel_->font();
		dotFont.setPointSize(dotFont.pointSize() + 2);
		statusDotLabel_->setFont(dotFont);
	}
	statusTextLabel_ = new QLabel(QStringLiteral("待机"), this);
	statusTextLabel_->setStyleSheet(
	    QStringLiteral("color: #e0e0e0; font-weight: bold;"));
	elapsedLabel_ = new QLabel(QStringLiteral("00:00"), this);
	elapsedLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	elapsedLabel_->setStyleSheet(QStringLiteral("color: #e0e0e0;"));
	statusRow->addWidget(statusDotLabel_);
	statusRow->addWidget(statusTextLabel_);
	statusRow->addStretch();
	statusRow->addWidget(elapsedLabel_);
	root->addLayout(statusRow);

	// 第 2 行：录制方案（分辨率/帧率/码率/编码），未录制显示“待机”
	planLabel_ = new QLabel(QStringLiteral("待机"), this);
	planLabel_->setStyleSheet(QStringLiteral("color: #bdbdbd;"));
	planLabel_->setWordWrap(false);
	root->addWidget(planLabel_);

	// 鼠标加速准入告警（黄色，仅 Idle 且加速开启时显示）
	accelWarnLabel_ = new QLabel(
	    QStringLiteral("⚠ 鼠标加速已开启，无法开始录制"), this);
	accelWarnLabel_->setStyleSheet(
	    QStringLiteral("color: #ffd54f; font-weight: bold;"));
	accelWarnLabel_->setVisible(false);
	root->addWidget(accelWarnLabel_);

	// 采集排除回退告警（橙色，仅 WDA_MONITOR 回退时显示）
	captureWarnLabel_ = new QLabel(
	    QStringLiteral("⚠ 当前系统不支持采集排除，悬浮窗将在录制中"
	                   "自动隐藏以防被录进画面"),
	    this);
	captureWarnLabel_->setWordWrap(true);
	captureWarnLabel_->setStyleSheet(
	    QStringLiteral("color: #ffb74d; font-weight: bold;"));
	captureWarnLabel_->setVisible(false);
	root->addWidget(captureWarnLabel_);

	// 日志区：等宽字体 + 深色半透明底 + 浅色文字
	logView_ = new QPlainTextEdit(this);
	logView_->setReadOnly(true);
	logView_->setMaximumBlockCount(200);
	logView_->setUndoRedoEnabled(false);
	logView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	logView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	logView_->setFrameStyle(QFrame::NoFrame);
	{
		QFont monoFont(QStringLiteral("Consolas"));
		monoFont.setStyleHint(QFont::Monospace);
		monoFont.setPointSize(9);
		logView_->setFont(monoFont);
	}
	logView_->setStyleSheet(QStringLiteral(
	    "QPlainTextEdit { background-color: rgba(0, 0, 0, 150);"
	    " color: #d0d0d0; border: none; }"));
	root->addWidget(logView_, 1);

	// 整窗深色底（不透明深色 + setWindowOpacity 整体半透明，见文件头）
	setStyleSheet(QStringLiteral(
	    "#GrtOverlayWindow { background-color: #1e1e1e;"
	    " border: 1px solid #4d4d4d; border-radius: 6px; }"));

	// —— 定时器 ——
	oneSecondTimer_ = new QTimer(this);
	oneSecondTimer_->setInterval(kOneSecondIntervalMs);
	connect(oneSecondTimer_, &QTimer::timeout, this,
	        &OverlayWindow::OnOneSecondTick);
	oneSecondTimer_->start();

	accelPollTimer_ = new QTimer(this);
	accelPollTimer_->setInterval(kAccelPollIntervalMs);
	connect(accelPollTimer_, &QTimer::timeout, this,
	        &OverlayWindow::OnPollMouseAccel);

	// 强制创建 HWND（此时窗口仍隐藏），随后立即叠加 Win32 扩展样式，
	// 避免首次 show 时出现短暂“可激活/可点击”窗口。
	hwnd_ = reinterpret_cast<HWND>(winId());
	ConfigureNative();

	ApplySettings();

	// 订阅前的近期日志先回填，悬浮窗一出现即有上下文
	for (const LogEntry& entry : LogBus::Instance().SnapshotRecent()) {
		AppendLog(QString::fromStdString(entry.text), entry.level);
	}

	RefreshStatusRow();
	RefreshPlanRow();
	RefreshAccelWarning();
}

OverlayWindow::~OverlayWindow()
{
	// 子 QTimer 随 QObject 父子关系自动停止/释放，这里仅确保不再触发
	if (oneSecondTimer_ != nullptr) {
		oneSecondTimer_->stop();
	}
	if (accelPollTimer_ != nullptr) {
		accelPollTimer_->stop();
	}
}

void OverlayWindow::ApplySettings()
{
	const OverlaySettings settings =
	    PluginConfig::Instance().GetOverlaySettings();

	// 不透明度限制到 0.2~1.0（配置理论范围 0.0~1.0，过低不可读）
	const double opacity = std::clamp(settings.opacity, 0.2, 1.0);
	setWindowOpacity(opacity);

	if (settings.posX >= 0 && settings.posY >= 0) {
		move(settings.posX, settings.posY);
	} else {
		// 默认：主屏右上角留 40px 边距
		QScreen* screen = QGuiApplication::primaryScreen();
		if (screen == nullptr && !QGuiApplication::screens().isEmpty()) {
			screen = QGuiApplication::screens().first();
		}
		if (screen != nullptr) {
			const QRect avail = screen->availableGeometry();
			move(avail.right() - kOverlayWidth - kDefaultEdgeMargin,
			     avail.top() + kDefaultEdgeMargin);
		}
	}

	userWantsVisible_ = settings.enabled;
	RecomputeEffectiveVisibility();
}

void OverlayWindow::SetDesiredVisible(bool visible)
{
	userWantsVisible_ = visible;
	RecomputeEffectiveVisibility();
}

void OverlayWindow::SetSessionState(int stateValue)
{
	sessionState_ = stateValue;
	RefreshStatusRow();
	RefreshPlanRow();
	RefreshAccelWarning();
	RecomputeEffectiveVisibility();
}

void OverlayWindow::SetElapsed(double seconds)
{
	elapsedLabel_->setText(FormatElapsed(seconds));
}

void OverlayWindow::AppendLog(const QString& text, int level)
{
	QString line = text;
	if (line.startsWith(kLogPrefix)) {
		line = line.mid(kLogPrefix.size());
	}

	// OBS blog 级别：LOG_WARNING=400，LOG_ERROR=500；其余按 INFO 着色
	QColor color;
	if (level >= 500) {
		color = QColor(0xff, 0x6b, 0x68); // 红
	} else if (level >= 400) {
		color = QColor(0xff, 0xd5, 0x4f); // 黄
	} else {
		color = QColor(0xd0, 0xd0, 0xd0); // 浅灰
	}

	QTextCharFormat fmt;
	fmt.setForeground(color);
	QTextCursor cursor(logView_->document());
	cursor.movePosition(QTextCursor::End);
	cursor.insertText(line + QLatin1Char('\n'), fmt);

	// 始终滚动到最新一行（只读区，maximumBlockCount 自动丢弃最旧）
	QScrollBar* bar = logView_->verticalScrollBar();
	bar->setValue(bar->maximum());
}

void OverlayWindow::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);

	// 确保任何显示路径下 Win32 样式都已就位
	ConfigureNative();
	KeepTopMost();

	// 以 SW_SHOWNOACTIVATE 再兜底一次，杜绝激活/抢焦点
	if (hwnd_ != nullptr) {
		ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
	}

	// 仅可见时才轮询鼠标加速（2s，低频）
	if (accelPollTimer_ != nullptr && !accelPollTimer_->isActive()) {
		accelPollTimer_->start();
	}
	RefreshAccelWarning();
}

void OverlayWindow::hideEvent(QHideEvent* event)
{
	QWidget::hideEvent(event);
	if (accelPollTimer_ != nullptr) {
		accelPollTimer_->stop();
	}
}

bool OverlayWindow::nativeEvent(const QByteArray& eventType, void* message,
                                qintptr* result)
{
	Q_UNUSED(eventType);
	const MSG* msg = static_cast<MSG*>(message);
	if (msg != nullptr && hwnd_ == nullptr) {
		hwnd_ = msg->hwnd;
	}
	if (msg != nullptr && msg->message == WM_WINDOWPOSCHANGING) {
		// 双保险：任何内部 show/move 路径都不允许激活窗口
		auto* pos = reinterpret_cast<WINDOWPOS*>(msg->lParam);
		if (pos != nullptr) {
			pos->flags |= SWP_NOACTIVATE;
		}
	}
	return QWidget::nativeEvent(eventType, message, result);
}

void OverlayWindow::OnOneSecondTick()
{
	// 周期兜底置顶：游戏全屏独占切换后 TOPMOST 可能被压制，重新置顶时
	// 必须带 SWP_NOACTIVATE，避免闪屏/抢焦。
	if (isVisible()) {
		KeepTopMost();
		RefreshPlanRow();
	}
}

void OverlayWindow::OnPollMouseAccel()
{
	RefreshAccelWarning();
}

void OverlayWindow::ConfigureNative()
{
	if (nativeConfigured_) {
		return;
	}
	if (hwnd_ == nullptr) {
		hwnd_ = reinterpret_cast<HWND>(winId());
	}
	if (hwnd_ == nullptr) {
		return;
	}

	// —— 叠加扩展样式：分层 + 点击穿透 + 不激活 + 工具窗 ——
	LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
	exStyle |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
	           WS_EX_TOOLWINDOW;
	SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);

	// 初始置顶（不激活）
	SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	// —— 采集排除（SubTask 9.2）——
	// WDA_EXCLUDEFROMCAPTURE 需要 Windows 10 2004（build 19041）及以上。
	int major = 0;
	int build = 0;
	const bool versionKnown = RealWindowsVersion(major, build);
	const bool canExclude =
	    versionKnown && (major > 10 || (major == 10 && build >= 19041));

	if (!versionKnown) {
		GRT_LOG_WARN("悬浮窗：RtlGetVersion 查询系统版本失败，"
		             "按不支持采集排除处理");
	}

	if (canExclude) {
		if (SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
			excludeFromCaptureSupported_ = true;
			GRT_LOG_INFO("悬浮窗：已设置 WDA_EXCLUDEFROMCAPTURE"
			             "（录制画面中完全不可见，Win %d.%d）",
			             major, build);
		} else {
			const DWORD error = GetLastError();
			GRT_LOG_ERROR("悬浮窗：WDA_EXCLUDEFROMCAPTURE 设置失败（%s），"
			              "回退 WDA_MONITOR",
			              Win32ErrorMessage(error).c_str());
			excludeFromCaptureSupported_ = false;
		}
	} else {
		// 版本已知但低于 19041：EXCLUDEFROMCAPTURE 不存在，只能 MONITOR
		excludeFromCaptureSupported_ = false;
		if (versionKnown) {
			GRT_LOG_WARN("悬浮窗：系统版本 Win %d.%d 低于 10.19041，"
			             "不支持 WDA_EXCLUDEFROMCAPTURE，回退 WDA_MONITOR",
			             major, build);
		}
	}

	if (!excludeFromCaptureSupported_) {
		// 回退：WDA_MONITOR 使窗口在采集画面中呈黑块。若连它都失败，
		// 同样按“不支持排除”处理——录制中强制隐藏，绝不入画。
		if (SetWindowDisplayAffinity(hwnd_, WDA_MONITOR)) {
			GRT_LOG_WARN("悬浮窗：已回退 WDA_MONITOR（录制中窗口会被录成"
			             "黑块），录制期间将自动隐藏悬浮窗");
		} else {
			const DWORD error = GetLastError();
			GRT_LOG_ERROR("悬浮窗：WDA_MONITOR 也设置失败（%s）；"
			              "录制期间仍将自动隐藏以防入画",
			              Win32ErrorMessage(error).c_str());
		}
	}

	RefreshCaptureWarning();
	nativeConfigured_ = true;
}

void OverlayWindow::KeepTopMost()
{
	if (hwnd_ == nullptr) {
		return;
	}
	SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void OverlayWindow::RefreshPlanRow()
{
	const auto info = SessionManager::Instance().GetRecordingInfo();
	if (!info.has_value()) {
		planLabel_->setText(QStringLiteral("待机"));
		return;
	}

	// 内部 codec 取值为 "h264" / "hevc"；编码器展示真实 id 后缀更友好
	QString codecText =
	    info->codec == "hevc" ? QStringLiteral("H.265")
	    : info->codec == "h264" ? QStringLiteral("H.264")
	                           : QString::fromStdString(info->codec);
	QString encoderText = QString::fromStdString(info->encoder);
	if (encoderText.isEmpty()) {
		encoderText = QStringLiteral("未知");
	}

	planLabel_->setText(QStringLiteral("%1×%2 @%3fps ｜ %4 kbps ｜ %5 ｜ %6")
	                        .arg(info->width)
	                        .arg(info->height)
	                        .arg(info->fps)
	                        .arg(info->bitrateKbps)
	                        .arg(codecText, encoderText));
}

void OverlayWindow::RefreshStatusRow()
{
	struct Style {
		const char* dotColor;
		const char* text;
	};
	// 0=Idle 1=Preparing 2=Recording 3=Finalizing
	const Style styles[4] = {
	    {"#9e9e9e", "待机"},        // 灰
	    {"#ffd54f", "准备中…"},     // 黄
	    {"#ff5252", "录制中"},      // 红（圆点由 statusDotLabel_ 独立显示）
	    {"#ffd54f", "收尾中…"},     // 黄
	};
	const int idx = std::clamp(sessionState_, 0, 3);
	statusDotLabel_->setStyleSheet(
	    QStringLiteral("color: %1;").arg(QLatin1String(styles[idx].dotColor)));
	statusTextLabel_->setText(QString::fromUtf8(styles[idx].text));

	// 录制中时长文字用红色强调，其余浅灰
	if (sessionState_ == 2) {
		elapsedLabel_->setStyleSheet(
		    QStringLiteral("color: #ff8a80; font-weight: bold;"));
	} else {
		elapsedLabel_->setStyleSheet(QStringLiteral("color: #e0e0e0;"));
		if (sessionState_ == 0) {
			elapsedLabel_->setText(QStringLiteral("00:00"));
		}
	}
}

void OverlayWindow::RefreshAccelWarning()
{
	// 仅 Idle 且鼠标加速开启时提示（录制中不再提示，准入已在会话层拦截）
	const bool show =
	    isVisible() && sessionState_ == 0 &&
	    MouseAccelService::IsEnhancePointerPrecisionOn();
	accelWarnLabel_->setVisible(show);
}

void OverlayWindow::RefreshCaptureWarning()
{
	// 仅当采集排除不可用（WDA_MONITOR 回退或失败）时显示橙色固定告警
	captureWarnLabel_->setVisible(!excludeFromCaptureSupported_);
}

void OverlayWindow::RecomputeEffectiveVisibility()
{
	// WDA_MONITOR 回退：Recording/Finalizing 期间强制隐藏，避免黑块入画；
	// Preparing 阶段录制尚未开始，窗口仍可显示；回到 Idle 按意图恢复。
	const bool forceHiddenForCapture =
	    !excludeFromCaptureSupported_ &&
	    (sessionState_ == 2 || sessionState_ == 3);

	const bool shouldShow = userWantsVisible_ && !forceHiddenForCapture;
	if (shouldShow) {
		if (!isVisible()) {
			ConfigureNative();
			// show() 配合 WA_ShowWithoutActivating 不会激活；
			// showEvent 中再以 SW_SHOWNOACTIVATE 兜底。
			show();
			if (hwnd_ != nullptr) {
				ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
			}
			KeepTopMost();
		}
	} else {
		if (isVisible()) {
			hide();
		}
	}
}

// ===========================================================================
// 对外自由函数
// ===========================================================================

void InitOverlay()
{
	if (g_overlay != nullptr) {
		// 幂等：已存在时仅按最新配置刷新一次
		ApplyOverlaySettings();
		return;
	}
	g_overlay = new OverlayWindow(nullptr);
	// 构造函数内已调用 ApplySettings() 并按 enabled 决定显隐
	GRT_LOG_INFO("悬浮状态窗已初始化");
}

void ShutdownOverlay()
{
	if (g_overlay == nullptr) {
		return;
	}
	// 先停定时器（父对象析构也会停，这里提前停以保证 delete 期间
	// 绝无回调访问 MainPanel 等正在销毁的对象）
	delete g_overlay;
	g_overlay = nullptr;
}

void SetOverlayVisible(bool visible)
{
	if (g_overlay == nullptr) {
		return;
	}
	g_overlay->SetDesiredVisible(visible);
}

void ToggleOverlay()
{
	if (g_overlay == nullptr) {
		return;
	}
	SetOverlayVisible(!IsOverlayVisible());
}

void ToggleOverlayFromAnyThread()
{
	QCoreApplication* app = QCoreApplication::instance();
	if (app == nullptr) {
		return;
	}
	if (QThread::currentThread() == app->thread()) {
		ToggleOverlay();
		return;
	}
	// 排队到 Qt 主线程执行；热键线程立即返回，不做任何窗口操作
	QMetaObject::invokeMethod(app, []() { ToggleOverlay(); },
	                          Qt::QueuedConnection);
}

bool IsOverlayVisible()
{
	return g_overlay != nullptr && g_overlay->isVisible();
}

void ApplyOverlaySettings()
{
	if (g_overlay == nullptr) {
		return;
	}
	g_overlay->ApplySettings();
}

void OnSessionStateChanged(int stateValue)
{
	if (g_overlay != nullptr) {
		g_overlay->SetSessionState(stateValue);
	}
}

void OnElapsedTick(double seconds)
{
	if (g_overlay != nullptr) {
		g_overlay->SetElapsed(seconds);
	}
}

void OnLog(const QString& text, int level)
{
	if (g_overlay != nullptr) {
		g_overlay->AppendLog(text, level);
	}
}

} // namespace grt::ui
