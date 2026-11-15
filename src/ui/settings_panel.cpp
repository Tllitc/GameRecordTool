/**
 * @file settings_panel.cpp
 * @brief “录制参数”页实现：控件 ↔ PluginConfig 双向同步。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui/settings_panel.h"

#include <windows.h>

#include <array>
#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "core/plugin_config.h"
#include "input/global_hotkey_manager.h"
#include "input/input_event.h"
#include "input/key_names.h"
#include "input/raw_input_service.h"
#include "ui/overlay_controller.h"

namespace grt::ui {

namespace {

/// 下拉项：显示文案 + 写入配置的内部值
struct ComboOption {
	const char* text;
	const char* value;
};

/// 在组合框中按内部值选中对应项（找不到时回退到第 0 项）
void SelectByValue(QComboBox* combo, const std::string& value)
{
	for (int i = 0; i < combo->count(); ++i) {
		if (combo->itemData(i).toString().toStdString() == value) {
			combo->setCurrentIndex(i);
			return;
		}
	}
	combo->setCurrentIndex(0);
}

/// 判断是否为单个 ASCII 字母（A-Z / a-z）
bool IsSingleAsciiLetter(const QString& text)
{
	if (text.size() != 1) {
		return false;
	}
	const char16_t code = text[0].unicode();
	return (code >= u'A' && code <= u'Z') ||
	       (code >= u'a' && code <= u'z');
}

/// 左/右修饰键归一化为通用 VK（与 GlobalHotkeyManager 内部口径一致）
uint16_t NormalizeModifierVk(uint16_t vk)
{
	switch (vk) {
	case VK_LCONTROL:
	case VK_RCONTROL:
		return VK_CONTROL;
	case VK_LSHIFT:
	case VK_RSHIFT:
		return VK_SHIFT;
	case VK_LMENU:
	case VK_RMENU:
		return VK_MENU;
	default:
		return vk;
	}
}

/// 四个热键动作的中文名称（顺序与 HotkeySettings 下标一致）
constexpr const char* kHotkeyActionNames[4] = {
    "开始录制", "停止录制", "动作打点", "悬浮窗显示/隐藏"};

} // 匿名命名空间

SettingsPanel::SettingsPanel(QWidget* parent) : QWidget(parent)
{
	BuildUi();
	ConnectSignals();

	// 热键捕获轮询 / 超时定时器（仅捕获期间运行，均为 this 子对象）
	hotkeyPollTimer_ = new QTimer(this);
	hotkeyPollTimer_->setInterval(16);
	hotkeyTimeoutTimer_ = new QTimer(this);
	hotkeyTimeoutTimer_->setSingleShot(true);
	hotkeyTimeoutTimer_->setInterval(5000);
	connect(hotkeyPollTimer_, &QTimer::timeout, this,
	        &SettingsPanel::OnHotkeyPoll);
	connect(hotkeyTimeoutTimer_, &QTimer::timeout, this,
	        &SettingsPanel::OnHotkeyCaptureTimeout);

	ReloadFromConfig();
}

SettingsPanel::~SettingsPanel()
{
	StopHotkeyCapture();
}

void SettingsPanel::BuildUi()
{
	auto* root = new QVBoxLayout(this);

	// 表单内容置于滚动视口内：Dock 高度不足时出现纵向滚动条，避免
	// 底部的悬浮窗/全局热键设置被裁掉。
	auto* content = new QWidget;
	auto* contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(0, 0, 0, 0);

	// —— 录制参数分组 ——
	auto* recordGroup = new QGroupBox(QStringLiteral("录制参数"), this);
	auto* recordForm = new QFormLayout(recordGroup);

	// 帧率：30 / 60
	fpsCombo_ = new QComboBox(recordGroup);
	fpsCombo_->addItem(QStringLiteral("30 fps"), 30);
	fpsCombo_->addItem(QStringLiteral("60 fps"), 60);
	recordForm->addRow(QStringLiteral("帧率："), fpsCombo_);

	// 编码器：自动/NVENC/AMF/QSV/x264
	encoderCombo_ = new QComboBox(recordGroup);
	static const ComboOption kEncoderOptions[] = {
	    {QT_TR_NOOP("自动"), "auto"},
	    {"NVENC", "nvenc"},
	    {"AMF", "amf"},
	    {"QSV", "qsv"},
	    {"x264", "x264"},
	};
	for (const ComboOption& opt : kEncoderOptions) {
		encoderCombo_->addItem(QString::fromUtf8(opt.text),
		                       QString::fromUtf8(opt.value));
	}
	recordForm->addRow(QStringLiteral("编码器："), encoderCombo_);

	// 编码格式：自动/H.264/H.265
	codecCombo_ = new QComboBox(recordGroup);
	static const ComboOption kCodecOptions[] = {
	    {QT_TR_NOOP("自动"), "auto"},
	    {"H.264", "h264"},
	    {"H.265", "h265"},
	};
	for (const ComboOption& opt : kCodecOptions) {
		codecCombo_->addItem(QString::fromUtf8(opt.text),
		                     QString::fromUtf8(opt.value));
	}
	recordForm->addRow(QStringLiteral("编码格式："), codecCombo_);

	// 码率：0=自动（按分辨率档位）
	auto* bitrateRow = new QHBoxLayout();
	bitrateSpin_ = new QSpinBox(recordGroup);
	bitrateSpin_->setRange(0, 200000);
	bitrateSpin_->setSingleStep(500);
	// 值为 0（minimum）时显示自动档位说明
	bitrateSpin_->setSpecialValueText(
	    QStringLiteral("自动（按分辨率档位）"));
	bitrateSpin_->setSuffix(QStringLiteral(" kbps"));
	bitrateRow->addWidget(bitrateSpin_);
	bitrateRow->addWidget(new QLabel(
	    QStringLiteral("自动档位：1080p=12M，1440p=30M，4K=45M"),
	    recordGroup));
	bitrateRow->addStretch();
	recordForm->addRow(QStringLiteral("码率："), bitrateRow);

	// 输出目录 + 浏览
	auto* dirRow = new QHBoxLayout();
	outputDirEdit_ = new QLineEdit(recordGroup);
	outputDirEdit_->setPlaceholderText(
	    QStringLiteral("请选择录制输出目录"));
	auto* browseButton =
	    new QPushButton(QStringLiteral("浏览…"), recordGroup);
	dirRow->addWidget(outputDirEdit_);
	dirRow->addWidget(browseButton);
	recordForm->addRow(QStringLiteral("输出目录："), dirRow);
	connect(browseButton, &QPushButton::clicked, this,
	        &SettingsPanel::OnBrowseDir);

	// 崩溃安全
	crashSafeCheck_ = new QCheckBox(recordGroup);
	crashSafeCheck_->setText(QStringLiteral(
	    "崩溃安全输出（优先 fragmented MP4，否则 mkv 自动 remux）"));
	recordForm->addRow(crashSafeCheck_);

	// 采集鼠标光标
	captureCursorCheck_ =
	    new QCheckBox(QStringLiteral("采集鼠标光标"), recordGroup);
	recordForm->addRow(captureCursorCheck_);

	// 移动键配置：配置数组顺序为 W,A,S,D；界面按 前进/左移/后退/右移
	// （与 SessionManager 取 [0]/[1]/[2]/[3] 的顺序一致）
	auto* movementRow = new QHBoxLayout();
	static const char* kMovementLabels[4] = {"前进(W)", "左移(A)",
	                                         "后退(S)", "右移(D)"};
	for (int i = 0; i < 4; ++i) {
		movementRow->addWidget(new QLabel(
		    QString::fromUtf8(kMovementLabels[i]), recordGroup));
		movementKeyEdits_[i] = new QLineEdit(recordGroup);
		movementKeyEdits_[i]->setMaxLength(1);
		movementKeyEdits_[i]->setFixedWidth(36);
		movementRow->addWidget(movementKeyEdits_[i]);
	}
	movementRow->addStretch();
	recordForm->addRow(QStringLiteral("移动键："), movementRow);

	contentLayout->addWidget(recordGroup);

	// —— 悬浮窗设置分组（Task 9 读取同一份 OverlaySettings）——
	auto* overlayGroup = new QGroupBox(QStringLiteral("悬浮窗设置"), this);
	auto* overlayForm = new QFormLayout(overlayGroup);
	overlayEnabledCheck_ =
	    new QCheckBox(QStringLiteral("启用悬浮窗"), overlayGroup);
	overlayForm->addRow(overlayEnabledCheck_);
	overlayOpacitySpin_ = new QDoubleSpinBox(overlayGroup);
	overlayOpacitySpin_->setRange(0.3, 1.0);
	overlayOpacitySpin_->setSingleStep(0.05);
	overlayOpacitySpin_->setDecimals(2);
	overlayForm->addRow(QStringLiteral("不透明度："),
	                    overlayOpacitySpin_);

	// 悬浮窗位置：-1=默认（主屏右上角留 40px），其余为屏幕绝对坐标
	overlayPosXSpin_ = new QSpinBox(overlayGroup);
	overlayPosXSpin_->setRange(-1, 10000);
	overlayPosXSpin_->setSpecialValueText(
	    QStringLiteral("默认（右上角）"));
	overlayForm->addRow(QStringLiteral("横坐标："), overlayPosXSpin_);
	overlayPosYSpin_ = new QSpinBox(overlayGroup);
	overlayPosYSpin_->setRange(-1, 10000);
	overlayPosYSpin_->setSpecialValueText(
	    QStringLiteral("默认（右上角）"));
	overlayForm->addRow(QStringLiteral("纵坐标："), overlayPosYSpin_);

	contentLayout->addWidget(overlayGroup);

	// —— 全局热键分组（Task 11）——
	auto* hotkeyGroup = new QGroupBox(QStringLiteral("全局热键"), this);
	auto* hotkeyLayout = new QGridLayout(hotkeyGroup);
	hotkeyLayout->setColumnStretch(2, 1);
	for (int i = 0; i < 4; ++i) {
		hotkeyLayout->addWidget(
		    new QLabel(QString::fromUtf8(kHotkeyActionNames[i]),
		               hotkeyGroup),
		    i, 0);
		hotkeyButtons_[i] = new QPushButton(hotkeyGroup);
		hotkeyButtons_[i]->setMinimumWidth(150);
		hotkeyLayout->addWidget(hotkeyButtons_[i], i, 1);
		hotkeyClearButtons_[i] =
		    new QPushButton(QStringLiteral("清除"), hotkeyGroup);
		hotkeyLayout->addWidget(hotkeyClearButtons_[i], i, 2);
		connect(hotkeyButtons_[i], &QPushButton::clicked, this,
		        [this, i]() { OnHotkeyButtonClicked(i); });
		connect(hotkeyClearButtons_[i], &QPushButton::clicked, this,
		        [this, i]() {
			        if (loading_ || !hotkeyButtons_[i]->isEnabled()) {
				        return;
			        }
			        StopHotkeyCapture();
			        hotkeys_[static_cast<size_t>(i)] = HotkeyBinding{};
			        PluginConfig::Instance().SetHotkeySettings(hotkeys_);
			        GlobalHotkeyManager::Instance().ReloadBindings();
			        RefreshHotkeyButtons();
			        hotkeyStatusLabel_->setText(QStringLiteral(
			            "已清除“%1”热键。")
			            .arg(QString::fromUtf8(kHotkeyActionNames[i])));
		        });
	}
	hotkeyStatusLabel_ = new QLabel(hotkeyGroup);
	hotkeyStatusLabel_->setWordWrap(true);
	hotkeyLayout->addWidget(hotkeyStatusLabel_, 4, 0, 1, 3);
	auto* hotkeyHint = new QLabel(QStringLiteral(
	    "点击按键框后按下组合键即可改键（Esc 取消）；热键为只读监听，"
	    "不会吞掉游戏输入，全屏下同样可用。"), hotkeyGroup);
	hotkeyHint->setWordWrap(true);
	hotkeyHint->setStyleSheet(QStringLiteral("color: #666666;"));
	hotkeyLayout->addWidget(hotkeyHint, 5, 0, 1, 3);
	contentLayout->addWidget(hotkeyGroup);

	// 编码器/编码格式需重启 OBS 生效的静态提示（OutputConfigurator 已知行为）
	auto* restartHint = new QLabel(QStringLiteral(
	    "提示：编码器/编码格式变更后需重启 OBS 生效。"), this);
	restartHint->setWordWrap(true);
	restartHint->setStyleSheet(QStringLiteral("color: #8a6d00;"));
	contentLayout->addWidget(restartHint);

	contentLayout->addStretch();

	auto* scroll = new QScrollArea(this);
	scroll->setWidget(content);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	root->addWidget(scroll);
}

void SettingsPanel::ConnectSignals()
{
	connect(fpsCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
	        &SettingsPanel::OnFpsChanged);
	connect(encoderCombo_,
	        qOverload<int>(&QComboBox::currentIndexChanged), this,
	        &SettingsPanel::OnEncoderChanged);
	connect(codecCombo_,
	        qOverload<int>(&QComboBox::currentIndexChanged), this,
	        &SettingsPanel::OnCodecChanged);
	connect(bitrateSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
	        &SettingsPanel::OnBitrateChanged);
	connect(outputDirEdit_, &QLineEdit::editingFinished, this,
	        &SettingsPanel::OnOutputDirEdited);
	connect(crashSafeCheck_, &QCheckBox::toggled, this,
	        &SettingsPanel::OnCrashSafeToggled);
	connect(captureCursorCheck_, &QCheckBox::toggled, this,
	        &SettingsPanel::OnCaptureCursorToggled);
	connect(overlayEnabledCheck_, &QCheckBox::toggled, this,
	        &SettingsPanel::OnOverlayEnabledToggled);
	connect(overlayOpacitySpin_, &QDoubleSpinBox::valueChanged, this,
	        &SettingsPanel::OnOverlayOpacityChanged);
	connect(overlayPosXSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
	        [this](int) { OnOverlayPosChanged(); });
	connect(overlayPosYSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
	        [this](int) { OnOverlayPosChanged(); });
	for (int i = 0; i < 4; ++i) {
		connect(movementKeyEdits_[i], &QLineEdit::editingFinished, this,
		        [this, i]() { OnMovementKeyEdited(i); });
	}
}

void SettingsPanel::ReloadFromConfig()
{
	StopHotkeyCapture();
	loading_ = true;

	PluginConfig& config = PluginConfig::Instance();
	const RecordingSettings rec = config.GetRecordingSettings();
	const OverlaySettings overlay = config.GetOverlaySettings();
	hotkeys_ = config.GetHotkeySettings();

	fpsCombo_->setCurrentIndex(rec.fps == 30 ? 0 : 1);
	SelectByValue(encoderCombo_, rec.encoder);
	SelectByValue(codecCombo_, rec.videoCodec);
	bitrateSpin_->setValue(rec.bitrateKbps);
	outputDirEdit_->setText(QDir::fromNativeSeparators(
	    QString::fromStdString(rec.outputDir)));
	crashSafeCheck_->setChecked(rec.crashSafe);
	captureCursorCheck_->setChecked(rec.captureCursor);

	const std::array<std::string, 4> keys = config.GetMovementKeys();
	for (int i = 0; i < 4; ++i) {
		movementKeyEdits_[i]->setText(
		    QString::fromStdString(keys[static_cast<size_t>(i)]));
	}

	overlayEnabledCheck_->setChecked(overlay.enabled);
	overlayOpacitySpin_->setValue(overlay.opacity);
	overlayPosXSpin_->setValue(overlay.posX);
	overlayPosYSpin_->setValue(overlay.posY);

	RefreshHotkeyButtons();

	loading_ = false;
}

void SettingsPanel::SetRecordingLocked(bool locked)
{
	fpsCombo_->setEnabled(!locked);
	encoderCombo_->setEnabled(!locked);
	codecCombo_->setEnabled(!locked);
	bitrateSpin_->setEnabled(!locked);
	outputDirEdit_->setEnabled(!locked);
	// 浏览按钮与目录输入框同生命周期
	const auto buttons = findChildren<QPushButton*>();
	for (QPushButton* button : buttons) {
		if (button->text() == QStringLiteral("浏览…")) {
			button->setEnabled(!locked);
		}
	}
	crashSafeCheck_->setEnabled(!locked);
	captureCursorCheck_->setEnabled(!locked);
	for (QLineEdit* edit : movementKeyEdits_) {
		edit->setEnabled(!locked);
	}
	overlayEnabledCheck_->setEnabled(!locked);
	overlayOpacitySpin_->setEnabled(!locked);
	overlayPosXSpin_->setEnabled(!locked);
	overlayPosYSpin_->setEnabled(!locked);
	SetHotkeyWidgetsEnabled(!locked);
}

void SettingsPanel::OnFpsChanged(int index)
{
	if (loading_) {
		return;
	}
	PluginConfig::Instance().SetFps(fpsCombo_->itemData(index).toInt());
}

void SettingsPanel::OnEncoderChanged(int index)
{
	if (loading_) {
		return;
	}
	PluginConfig::Instance().SetEncoder(
	    encoderCombo_->itemData(index).toString().toStdString());
}

void SettingsPanel::OnCodecChanged(int index)
{
	if (loading_) {
		return;
	}
	PluginConfig::Instance().SetVideoCodec(
	    codecCombo_->itemData(index).toString().toStdString());
}

void SettingsPanel::OnBitrateChanged(int valueKbps)
{
	if (loading_) {
		return;
	}
	PluginConfig::Instance().SetBitrateKbps(valueKbps);
}

void SettingsPanel::OnOutputDirEdited()
{
	if (loading_) {
		return;
	}
	PluginConfig::Instance().SetOutputDir(
	    QDir::toNativeSeparators(outputDirEdit_->text()).toStdString());
}

void SettingsPanel::OnBrowseDir()
{
	const QString current = outputDirEdit_->text();
	const QString dir = QFileDialog::getExistingDirectory(
	    this, QStringLiteral("选择录制输出目录"),
	    current.isEmpty() ? QString() : current);
	if (!dir.isEmpty()) {
		outputDirEdit_->setText(QDir::toNativeSeparators(dir));
		OnOutputDirEdited();
	}
}

void SettingsPanel::OnCrashSafeToggled(bool checked)
{
	if (loading_) {
		return;
	}
	PluginConfig::Instance().SetCrashSafe(checked);
}

void SettingsPanel::OnCaptureCursorToggled(bool checked)
{
	if (loading_) {
		return;
	}
	PluginConfig::Instance().SetCaptureCursor(checked);
}

void SettingsPanel::OnOverlayEnabledToggled(bool checked)
{
	if (loading_) {
		return;
	}
	PluginConfig& config = PluginConfig::Instance();
	OverlaySettings overlay = config.GetOverlaySettings();
	overlay.enabled = checked;
	config.SetOverlaySettings(overlay);
	// 立即生效（悬浮窗尚未初始化时该调用为空操作，幂等）
	ApplyOverlaySettings();
}

void SettingsPanel::OnOverlayOpacityChanged(double opacity)
{
	if (loading_) {
		return;
	}
	PluginConfig& config = PluginConfig::Instance();
	OverlaySettings overlay = config.GetOverlaySettings();
	overlay.opacity = opacity;
	config.SetOverlaySettings(overlay);
	ApplyOverlaySettings();
}

void SettingsPanel::OnOverlayPosChanged()
{
	if (loading_) {
		return;
	}
	PluginConfig& config = PluginConfig::Instance();
	OverlaySettings overlay = config.GetOverlaySettings();
	overlay.posX = overlayPosXSpin_->value();
	overlay.posY = overlayPosYSpin_->value();
	config.SetOverlaySettings(overlay);
	ApplyOverlaySettings();
}

void SettingsPanel::OnMovementKeyEdited(int slotIndex)
{
	if (loading_) {
		return;
	}
	QLineEdit* edit = movementKeyEdits_[slotIndex];
	QString text = edit->text().trimmed().toUpper();
	if (!IsSingleAsciiLetter(text)) {
		// 非法输入：恢复为配置中的当前值
		const std::array<std::string, 4> keys =
		    PluginConfig::Instance().GetMovementKeys();
		edit->setText(QString::fromStdString(
		    keys[static_cast<size_t>(slotIndex)]));
		return;
	}
	edit->setText(text);

	std::array<std::string, 4> keys =
	    PluginConfig::Instance().GetMovementKeys();
	keys[static_cast<size_t>(slotIndex)] = text.toStdString();
	PluginConfig::Instance().SetMovementKeys(keys);
}

// ---------------------------------------------------------------------------
// 全局热键改键（Task 11）
// ---------------------------------------------------------------------------

QString SettingsPanel::HotkeyToText(const HotkeyBinding& binding)
{
	if (binding.vkey <= 0 || binding.vkey >= 256) {
		return QStringLiteral("未绑定");
	}
	QString text;
	if ((binding.modifiers & MOD_CONTROL) != 0) {
		text += QStringLiteral("Ctrl+");
	}
	if ((binding.modifiers & MOD_ALT) != 0) {
		text += QStringLiteral("Alt+");
	}
	if ((binding.modifiers & MOD_SHIFT) != 0) {
		text += QStringLiteral("Shift+");
	}
	text += QString::fromStdString(
	    KeyVkeyToName(static_cast<uint16_t>(binding.vkey)));
	return text;
}

void SettingsPanel::RefreshHotkeyButtons()
{
	for (int i = 0; i < 4; ++i) {
		if (hotkeyButtons_[i] == nullptr) {
			continue;
		}
		if (i == hotkeyCapturingSlot_) {
			hotkeyButtons_[i]->setText(
			    QStringLiteral("请按下组合键…"));
		} else {
			hotkeyButtons_[i]->setText(
			    HotkeyToText(hotkeys_[static_cast<size_t>(i)]));
		}
	}
}

void SettingsPanel::SetHotkeyWidgetsEnabled(bool enabled)
{
	if (!enabled) {
		// 锁定（录制开始）时若正在捕获，先中止，避免捕获服务悬挂
		StopHotkeyCapture();
	}
	for (int i = 0; i < 4; ++i) {
		if (hotkeyButtons_[i] != nullptr) {
			hotkeyButtons_[i]->setEnabled(enabled);
		}
		if (hotkeyClearButtons_[i] != nullptr) {
			// 未绑定项的“清除”按钮始终禁用
			const bool bound =
			    hotkeys_[static_cast<size_t>(i)].vkey > 0;
			hotkeyClearButtons_[i]->setEnabled(enabled && bound);
		}
	}
}

void SettingsPanel::OnHotkeyButtonClicked(int slotIndex)
{
	if (loading_) {
		return;
	}
	if (hotkeyCapturingSlot_ == slotIndex) {
		// 同一按钮再次点击 = 取消
		StopHotkeyCapture(QStringLiteral("已取消改键。"));
		return;
	}
	StartHotkeyCapture(slotIndex);
}

void SettingsPanel::StartHotkeyCapture(int slotIndex)
{
	StopHotkeyCapture();
	if (slotIndex < 0 || slotIndex >= 4) {
		return;
	}

	// 复用进程内共享采集服务：只订阅事件流，绝不 Stop 共享服务
	// （录制会话与全局热键仍在使用）。
	RawInputService& service = RawInputService::Shared();
	if (!service.Start()) {
		hotkeyStatusLabel_->setText(QStringLiteral(
		    "无法启动临时输入采集（Raw Input 注册失败），请重试。"));
		return;
	}
	hotkeyCaptureSubId_ = service.Subscribe();

	hotkeyCapturingSlot_ = slotIndex;
	RefreshHotkeyButtons();
	// 捕获期间只保留当前按钮可点（用于取消）
	for (int i = 0; i < 4; ++i) {
		hotkeyButtons_[i]->setEnabled(i == slotIndex);
		hotkeyClearButtons_[i]->setEnabled(false);
	}
	hotkeyStatusLabel_->setText(
	    QStringLiteral("请按下“%1”的组合键（按住 Ctrl/Alt/Shift 等修饰键"
	                   "再按触发键；Esc 取消，5 秒超时）。")
	        .arg(QString::fromUtf8(kHotkeyActionNames[slotIndex])));
	hotkeyPollTimer_->start();
	hotkeyTimeoutTimer_->start();
}

void SettingsPanel::StopHotkeyCapture(const QString& statusText)
{
	if (hotkeyPollTimer_ != nullptr) {
		hotkeyPollTimer_->stop();
	}
	if (hotkeyTimeoutTimer_ != nullptr) {
		hotkeyTimeoutTimer_->stop();
	}
	if (hotkeyCaptureSubId_ != RawInputService::kInvalidSubscription) {
		RawInputService::Shared().Unsubscribe(hotkeyCaptureSubId_);
		hotkeyCaptureSubId_ = RawInputService::kInvalidSubscription;
	}
	const bool wasCapturing = hotkeyCapturingSlot_ >= 0;
	hotkeyCapturingSlot_ = -1;
	if (wasCapturing) {
		RefreshHotkeyButtons();
		// 恢复启用态：录制中（录制参数控件被锁）时热键区保持禁用。
		// 不能在此调用 SetHotkeyWidgetsEnabled（其 locked 分支会
		// 反过来 StopHotkeyCapture，形成递归）。
		const bool editable =
		    fpsCombo_ != nullptr && fpsCombo_->isEnabled();
		for (int i = 0; i < 4; ++i) {
			hotkeyButtons_[i]->setEnabled(editable);
			hotkeyClearButtons_[i]->setEnabled(
			    editable && hotkeys_[static_cast<size_t>(i)].vkey > 0);
		}
	}
	if (!statusText.isEmpty() && hotkeyStatusLabel_ != nullptr) {
		hotkeyStatusLabel_->setText(statusText);
	}
}

void SettingsPanel::OnHotkeyCaptureTimeout()
{
	if (hotkeyCapturingSlot_ >= 0) {
		StopHotkeyCapture(QStringLiteral("改键超时：5 秒内未收到有效组合键。"));
	}
}

void SettingsPanel::OnHotkeyPoll()
{
	if (hotkeyCapturingSlot_ < 0 ||
	    hotkeyCaptureSubId_ == RawInputService::kInvalidSubscription) {
		return;
	}

	int modifiers = 0;
	int triggerVkey = 0;

	const std::vector<InputEvent> events =
	    RawInputService::Shared().Poll(hotkeyCaptureSubId_);
	for (const InputEvent& e : events) {
		if (e.type != InputType::Key || e.vkey == 0 || e.vkey >= 256 ||
		    e.vkey == 255) {
			continue;
		}
		if (!e.key_down) {
			continue;
		}
		const uint16_t normalized =
		    NormalizeModifierVk(static_cast<uint16_t>(e.vkey));
		if (normalized == VK_ESCAPE) {
			StopHotkeyCapture(QStringLiteral("已取消改键。"));
			return;
		}
		switch (normalized) {
		case VK_CONTROL:
			modifiers |= MOD_CONTROL;
			break;
		case VK_MENU:
			modifiers |= MOD_ALT;
			break;
		case VK_SHIFT:
			modifiers |= MOD_SHIFT;
			break;
		default:
			// 第一个非修饰键即作为触发键
			if (triggerVkey == 0) {
				triggerVkey = normalized;
			}
			break;
		}
	}

	if (triggerVkey != 0) {
		AcceptHotkeyCapture(modifiers, triggerVkey);
	}
}

void SettingsPanel::AcceptHotkeyCapture(int modifiers, int vkey)
{
	const int slot = hotkeyCapturingSlot_;

	// 不允许无修饰符的单键（避免与游戏内常用按键冲突、误触）
	if (modifiers == 0) {
		hotkeyStatusLabel_->setText(QStringLiteral(
		    "请至少按住一个修饰键（Ctrl / Alt / Shift）再按触发键。"));
		// 不结束捕获，给用户一次继续按键的机会；超时/再点按钮可取消
		return;
	}

	// 重复绑定检查（与其他三个动作比较）
	for (int i = 0; i < 4; ++i) {
		if (i == slot) {
			continue;
		}
		const HotkeyBinding& other = hotkeys_[static_cast<size_t>(i)];
		if (other.vkey == vkey && other.modifiers == modifiers) {
			StopHotkeyCapture();
			hotkeyStatusLabel_->setText(
			    QStringLiteral("该组合键已绑定给“%1”，请换一个。")
			        .arg(QString::fromUtf8(kHotkeyActionNames[i])));
			return;
		}
	}

	HotkeyBinding binding;
	binding.modifiers = modifiers;
	binding.vkey = vkey;
	hotkeys_[static_cast<size_t>(slot)] = binding;
	PluginConfig::Instance().SetHotkeySettings(hotkeys_);
	// 立即热生效
	GlobalHotkeyManager::Instance().ReloadBindings();

	// StopHotkeyCapture 会按录制锁定状态恢复按钮启用态
	StopHotkeyCapture();
	hotkeyStatusLabel_->setText(
	    QStringLiteral("“%1”已绑定为 %2，立即生效。")
	        .arg(QString::fromUtf8(kHotkeyActionNames[slot]))
	        .arg(HotkeyToText(binding)));
}

} // namespace grt::ui
