/**
 * @file settings_panel.h
 * @brief “录制参数”页：帧率/编码器/编码格式/码率/输出目录/崩溃安全/
 *        采集鼠标/移动键/悬浮窗设置。
 *
 * 控件初始化时从 PluginConfig 读取当前值；用户改动立即写回（PluginConfig
 * 的 Set 方法内部即落盘）。录制会话进行中通过 SetRecordingLocked(true)
 * 整体禁用编辑（只允许查看），避免录制中改配置。
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <QWidget>

#include "core/plugin_config.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace grt {
class RawInputService;
}

namespace grt::ui {

class SettingsPanel : public QWidget {
	Q_OBJECT

public:
	explicit SettingsPanel(QWidget* parent = nullptr);
	~SettingsPanel() override;

	/**
	 * @brief 录制期间锁定/解锁参数编辑。
	 * @param locked true=禁用全部可编辑控件（仅查看）；false=恢复。
	 */
	void SetRecordingLocked(bool locked);

	/// 从 PluginConfig 重新读取全部配置并刷新控件（丢弃未保存的非法输入）
	void ReloadFromConfig();

private slots:
	// 各控件改动即写回 PluginConfig
	void OnFpsChanged(int index);
	void OnEncoderChanged(int index);
	void OnCodecChanged(int index);
	void OnBitrateChanged(int valueKbps);
	void OnOutputDirEdited();
	void OnBrowseDir();
	void OnCrashSafeToggled(bool checked);
	void OnCaptureCursorToggled(bool checked);
	void OnOverlayEnabledToggled(bool checked);
	void OnOverlayOpacityChanged(double opacity);
	void OnOverlayPosChanged();

	// —— 全局热键改键（Task 11）——
	void OnHotkeyButtonClicked(int slotIndex);
	void OnHotkeyPoll();
	void OnHotkeyCaptureTimeout();

private:
	/// 搭建控件与布局
	void BuildUi();
	/// 绑定信号（仅在初始化时调用一次）
	void ConnectSignals();
	/// 四个移动键输入框的统一校验 + 写回（长度 1 且为字母）
	void OnMovementKeyEdited(int slotIndex);

	/// 把绑定刷新到四个热键按钮的显示文案
	void RefreshHotkeyButtons();
	/// 绑定 -> 可显示文本（如 “Ctrl+Alt+R”；未绑定为“未绑定”）
	static QString HotkeyToText(const HotkeyBinding& binding);
	/**
	 * @brief 进入“按下组合键”捕获状态（再次点击同一按钮则取消）。
	 * @param slotIndex 0..3；传 -1 表示仅中止捕获
	 */
	void StartHotkeyCapture(int slotIndex);
	/// 中止捕获并复位按钮/状态（取消 / 超时 / 成功 / 析构共用）
	void StopHotkeyCapture(const QString& statusText = QString());
	/// 校验并接受一次捕获结果；重复绑定时提示且不写入
	void AcceptHotkeyCapture(int modifiers, int vkey);
	/// 录制锁定时统一启停热键区控件（捕获按钮 + 清除按钮）
	void SetHotkeyWidgetsEnabled(bool enabled);

	// —— 录制设置 ——
	QComboBox* fpsCombo_ = nullptr;
	QComboBox* encoderCombo_ = nullptr;
	QComboBox* codecCombo_ = nullptr;
	QSpinBox* bitrateSpin_ = nullptr;
	QLineEdit* outputDirEdit_ = nullptr;
	QCheckBox* crashSafeCheck_ = nullptr;
	QCheckBox* captureCursorCheck_ = nullptr;
	std::array<QLineEdit*, 4> movementKeyEdits_{};

	// —— 悬浮窗设置（Task 9 读取同一份 OverlaySettings）——
	QCheckBox* overlayEnabledCheck_ = nullptr;
	QDoubleSpinBox* overlayOpacitySpin_ = nullptr;
	QSpinBox* overlayPosXSpin_ = nullptr;
	QSpinBox* overlayPosYSpin_ = nullptr;

	// —— 全局热键（Task 11，顺序：开始/停止/打点/悬浮窗）——
	std::array<QPushButton*, 4> hotkeyButtons_{};
	std::array<QPushButton*, 4> hotkeyClearButtons_{};
	QLabel* hotkeyStatusLabel_ = nullptr;
	HotkeySettings hotkeys_{}; ///< 界面工作副本，改键即整体写回 PluginConfig

	// 捕获组合键期间订阅进程内共享的 Raw Input 服务（全部在 Qt 主线程使用）
	std::uint64_t hotkeyCaptureSubId_ = 0; ///< 共享采集服务订阅句柄（0=未订阅）
	QTimer* hotkeyPollTimer_ = nullptr;
	QTimer* hotkeyTimeoutTimer_ = nullptr;
	int hotkeyCapturingSlot_ = -1; ///< -1=未在捕获；否则为正在改的热键下标

	/// 初始化/Reload 期间屏蔽信号，避免“读配置回填”被当成用户改动再写回
	bool loading_ = false;
};

} // namespace grt::ui
