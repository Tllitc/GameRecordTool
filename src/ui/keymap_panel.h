/**
 * @file keymap_panel.h
 * @brief “按键映射（keymap）”页：按游戏档案维护 key -> {type, action} 映射。
 *
 * 顶部选择游戏（当前打开的窗口 + PluginConfig 中已保存档案合并去重），
 * 中部三列表格（按键标识 / 类型 / 中文动作语义）支持增删改，
 * “捕获下一次输入…”会订阅进程内共享的 RawInputService 监听下一次
 * 键盘/鼠标按键/滚轮并自动填充规范名。所有持久化均经由
 * PluginConfig::SaveGameProfile（obs_data JSON，立即落盘）。
 *
 * 注意：保存为整档案覆盖，加载时读到的 movementKeys 会原样回写，
 * 避免编辑器清掉设置页配置的移动键覆盖。
 */

#pragma once

#include <QComboBox>
#include <QWidget>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QLabel;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTimer;

namespace grt {

class RawInputService;

namespace ui {

/**
 * @brief 游戏选择下拉框：展开时自动重新枚举窗口与档案。
 *
 * 与 main_panel 的 RefreshOnPopupComboBox 同思路，但在本文件内自写，
 * 不声明 Q_OBJECT（无自有信号/槽），刷新逻辑由面板注入。
 */
class KeymapGameComboBox : public QComboBox {
public:
	explicit KeymapGameComboBox(QWidget* parent = nullptr)
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

/// keymap 可视化编辑面板（所有控件均在 Qt 主线程创建与访问）
class KeymapPanel : public QWidget {
	Q_OBJECT

public:
	explicit KeymapPanel(QWidget* parent = nullptr);
	~KeymapPanel() override;

	/**
	 * @brief 从 PluginConfig 重新拉取游戏列表并重载当前游戏档案。
	 *
	 * 丢弃当前未保存的表格内容（不弹放弃确认，供外部在明确的
	 * “重新加载”场景调用）；若正在捕获输入会先中止。
	 */
	void ReloadFromConfig();

protected:
	/// 首次显示时刷新一次游戏下拉
	void showEvent(QShowEvent* event) override;

private slots:
	void OnGameChanged(int index);
	void OnRefreshClicked();
	void OnAddRowClicked();
	void OnDeleteRowClicked();
	void OnCaptureClicked();
	void OnSaveClicked();
	void OnCellChanged(int row, int column);
	void OnPollCapture();
	void OnCaptureTimeout();

private:
	/// 搭建控件与布局
	void BuildUi();
	/// 绑定信号（仅在初始化时调用一次）
	void ConnectSignals();

	/// 重新枚举窗口 + 已保存档案，合并去重后重建下拉项；
	/// keepExe 非空时尽量保持该 exe 选中
	void RefreshGameList(const std::string& keepExe = std::string());
	/// 加载当前选中游戏的档案到表格（无档案则空表）
	void LoadCurrentGame();
	/// 表格有未保存改动时弹 Yes/No；返回 true 表示可以丢弃
	bool ConfirmDiscardIfDirty();
	/// 新增一个空映射行并进入按键标识编辑态
	void AddEmptyRow();
	/// 按标识名自动判定 keyboard/mouse 并设置类型下拉
	void AutoSetRowType(int row, const std::string& keyName);
	/// 开始“捕获下一次输入”
	void StartCapture();
	/**
	 * @brief 中止捕获并复位 UI（取消 / 超时 / 成功 / 切换游戏 / 析构共用）。
	 * @param statusText 非空时在状态栏显示该提示。
	 */
	void StopCapture(const QString& statusText = QString());
	/// 捕获到规范名后的处理：已存在则选中该行，否则新增一行
	void AcceptCapturedKey(const std::string& name);
	/// 查找按键标识所在行；不存在返回 -1
	int FindRowByKey(const std::string& keyName) const;
	/// 根据是否选中了有效游戏启用/停用操作按钮
	void UpdateActionsEnabled();
	/// 状态栏提示
	void SetStatus(const QString& text);

	// —— 顶部游戏选择行 ——
	KeymapGameComboBox* gameCombo_ = nullptr;
	QPushButton* refreshButton_ = nullptr;

	// —— 映射表格 ——
	QTableWidget* table_ = nullptr;
	QPushButton* addButton_ = nullptr;
	QPushButton* deleteButton_ = nullptr;
	QPushButton* captureButton_ = nullptr;
	QPushButton* saveButton_ = nullptr;
	QLabel* statusLabel_ = nullptr;

	// —— 当前编辑中的游戏档案状态 ——
	std::string currentExe_;   ///< 当前选中游戏 exe 名（UTF-8），空=未选择
	std::string currentTitle_; ///< 当前游戏标题（保存时写入档案）
	/// 加载档案时读到的移动键覆盖：SaveGameProfile 为整对象覆盖，
	/// 必须原样回写，否则 JSON 中该游戏的 movement_keys 会丢失
	std::optional<std::array<std::string, 4>> loadedMovementKeys_;
	bool dirty_ = false;       ///< 表格是否存在未保存改动
	bool loading_ = false;     ///< 程序化回填表格期间屏蔽脏标记信号

	// —— “捕获下一次输入”相关（全部在主线程使用）——
	std::uint64_t captureSubId_ = 0; ///< 共享采集服务订阅句柄（0=未订阅）
	QTimer* pollTimer_ = nullptr;  ///< 30ms 轮询 Poll()
	QTimer* timeoutTimer_ = nullptr; ///< 5s 超时（单次）
	bool capturing_ = false;       ///< 是否处于等待输入状态
	bool shownOnce_ = false;       ///< 是否已完成首次显示刷新
};

} // namespace grt::ui
} // namespace grt
