/**
 * @file keymap_panel.cpp
 * @brief “按键映射（keymap）”页实现：表格编辑 + 按游戏档案保存 + 捕获下一次输入。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui/keymap_panel.h"

#include <windows.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractItemView>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include "capture/window_enumerator.h"
#include "core/log.h"
#include "core/plugin_config.h"
#include "input/input_event.h"
#include "input/key_names.h"
#include "input/raw_input_service.h"

namespace grt::ui {

namespace {

/// 表格列枚举
enum KeymapColumn {
	kColKey = 0, // 按键标识（规范名）
	kColType,    // 类型（keyboard / mouse）
	kColAction,  // 动作（中文语义）
	kColumnCount,
};

constexpr const char* kTypeKeyboard = "keyboard";
constexpr const char* kTypeMouse = "mouse";

/// itemData 角色：exe 名（UTF-8）与窗口/档案标题（UTF-8）
constexpr int kRoleExe = Qt::UserRole;
constexpr int kRoleTitle = Qt::UserRole + 1;

/// 动作语义长度约束
constexpr int kMaxActionLength = 40;

/// UTF-16 -> UTF-8（本面板自用；session_manager.cpp 中的同名工具不可复用）
std::string WideToUtf8(const std::wstring& wide)
{
	if (wide.empty()) {
		return std::string();
	}
	const int length = WideCharToMultiByte(
	    CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
	    nullptr, 0, nullptr, nullptr);
	std::string utf8(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(),
	                    static_cast<int>(wide.size()), utf8.data(),
	                    length, nullptr, nullptr);
	return utf8;
}

/// 类型下拉的当前值（keyboard / mouse）
std::string RowType(QTableWidget* table, int row)
{
	auto* combo = qobject_cast<QComboBox*>(
	    table->cellWidget(row, kColType));
	if (combo == nullptr) {
		return std::string(kTypeKeyboard);
	}
	return combo->currentData().toString().toStdString();
}

} // 匿名命名空间

KeymapPanel::KeymapPanel(QWidget* parent) : QWidget(parent)
{
	BuildUi();
	ConnectSignals();

	// 轮询定时器（30ms）与超时定时器（5s 单次）均为成员对象，
	// 捕获期间通过 captureSubId_ 订阅共享 RawInputService
	pollTimer_ = new QTimer(this);
	pollTimer_->setInterval(30);
	timeoutTimer_ = new QTimer(this);
	timeoutTimer_->setSingleShot(true);
	timeoutTimer_->setInterval(5000);
	connect(pollTimer_, &QTimer::timeout, this,
	        &KeymapPanel::OnPollCapture);
	connect(timeoutTimer_, &QTimer::timeout, this,
	        &KeymapPanel::OnCaptureTimeout);

	// 启动时加载：刷新下拉并尝试选中枚举到的第一个窗口
	RefreshGameList();
	if (gameCombo_->count() > 0 && gameCombo_->currentIndex() < 0) {
		gameCombo_->setCurrentIndex(0);
	} else {
		LoadCurrentGame();
	}
}

KeymapPanel::~KeymapPanel()
{
	// 集中中止捕获：停定时器并退订共享服务，
	// 避免定时器在面板析构后被回调
	StopCapture();
}

void KeymapPanel::BuildUi()
{
	auto* root = new QVBoxLayout(this);

	// —— 顶部游戏选择行 ——
	auto* gameRow = new QHBoxLayout();
	gameRow->addWidget(new QLabel(QStringLiteral("游戏："), this));
	gameCombo_ = new KeymapGameComboBox(this);
	gameCombo_->setToolTip(QStringLiteral(
	    "列表为当前打开的窗口与已保存按键档案的合并；展开时自动重新枚举。"));
	gameRow->addWidget(gameCombo_, 1);
	refreshButton_ = new QPushButton(QStringLiteral("刷新"), this);
	gameRow->addWidget(refreshButton_);
	root->addLayout(gameRow);

	// —— 映射表格 ——
	table_ = new QTableWidget(0, kColumnCount, this);
	table_->setHorizontalHeaderLabels(
	    {QStringLiteral("按键标识"), QStringLiteral("类型"),
	     QStringLiteral("动作（中文语义）")});
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setAlternatingRowColors(true);
	table_->verticalHeader()->setVisible(false);
	table_->setColumnWidth(kColKey, 160);
	table_->setColumnWidth(kColType, 110);
	table_->horizontalHeader()->setStretchLastSection(true);
	root->addWidget(table_, 1);

	// —— 按钮行 ——
	auto* buttonRow = new QHBoxLayout();
	addButton_ = new QPushButton(QStringLiteral("新增映射"), this);
	deleteButton_ = new QPushButton(
	    QStringLiteral("删除选中行"), this);
	captureButton_ = new QPushButton(
	    QStringLiteral("捕获下一次输入…"), this);
	buttonRow->addWidget(addButton_);
	buttonRow->addWidget(deleteButton_);
	buttonRow->addWidget(captureButton_);
	buttonRow->addStretch();
	saveButton_ = new QPushButton(QStringLiteral("保存"), this);
	buttonRow->addWidget(saveButton_);
	root->addLayout(buttonRow);

	// —— 状态栏 ——
	statusLabel_ = new QLabel(this);
	root->addWidget(statusLabel_);

	UpdateActionsEnabled();
}

void KeymapPanel::ConnectSignals()
{
	gameCombo_->SetRefreshCallback([this] {
		// 展开时静默刷新列表（保留当前选择），不弹未保存确认
		RefreshGameList(currentExe_);
		// 当前游戏窗口已关闭且列表无对应项时，index 变为 -1，
		// currentIndexChanged 会触发 OnGameChanged 走未选择分支
		if (gameCombo_->currentIndex() < 0) {
			OnGameChanged(-1);
		}
	});
	connect(gameCombo_,
	        qOverload<int>(&QComboBox::currentIndexChanged), this,
	        &KeymapPanel::OnGameChanged);
	connect(refreshButton_, &QPushButton::clicked, this,
	        &KeymapPanel::OnRefreshClicked);
	connect(addButton_, &QPushButton::clicked, this,
	        &KeymapPanel::OnAddRowClicked);
	connect(deleteButton_, &QPushButton::clicked, this,
	        &KeymapPanel::OnDeleteRowClicked);
	connect(captureButton_, &QPushButton::clicked, this,
	        &KeymapPanel::OnCaptureClicked);
	connect(saveButton_, &QPushButton::clicked, this,
	        &KeymapPanel::OnSaveClicked);
	connect(table_, &QTableWidget::cellChanged, this,
	        &KeymapPanel::OnCellChanged);
}

void KeymapPanel::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	if (!shownOnce_) {
		shownOnce_ = true;
		// 首次显示再刷新一次（构造时可能 OBS/窗口尚未就绪）；
		// 此时用户尚未编辑，不弹未保存确认
		RefreshGameList(currentExe_);
		if (gameCombo_->currentIndex() < 0 &&
		    gameCombo_->count() > 0) {
			gameCombo_->setCurrentIndex(0);
		} else {
			LoadCurrentGame();
		}
	}
}

void KeymapPanel::ReloadFromConfig()
{
	StopCapture();
	RefreshGameList(currentExe_);
	if (gameCombo_->currentIndex() < 0 &&
	    gameCombo_->count() > 0) {
		gameCombo_->setCurrentIndex(0);
	} else {
		LoadCurrentGame();
	}
}

// ---------------------------------------------------------------------------
// 游戏下拉与档案加载
// ---------------------------------------------------------------------------

void KeymapPanel::RefreshGameList(const std::string& keepExe)
{
	// 合并来源：① 当前窗口 ② 已保存但未开窗的档案，按 exe 名去重
	struct Entry {
		std::string exe;
		std::string title;
	};
	std::map<std::string, Entry> merged; // map 同时按 exe 排序

	for (const WindowInfo& info : WindowEnumerator::Enumerate()) {
		std::string exe = WideToUtf8(info.exeName);
		if (exe.empty()) {
			continue;
		}
		std::string title = WideToUtf8(info.title);
		if (auto it = merged.find(exe); it != merged.end()) {
			// 同一 exe 多个窗口：保留第一个非空标题
			if (it->second.title.empty() && !title.empty()) {
				it->second.title = std::move(title);
			}
		} else {
			merged.emplace(exe, Entry{exe, std::move(title)});
		}
	}

	for (const GameProfileSummary& summary :
	     PluginConfig::Instance().ListGameProfiles()) {
		if (summary.exe.empty()) {
			continue;
		}
		merged.try_emplace(summary.exe,
		                   Entry{summary.exe, summary.title});
	}

	const QSignalBlocker blocker(gameCombo_);
	gameCombo_->clear();
	for (const auto& [exe, entry] : merged) {
		QString text = QString::fromStdString(entry.exe);
		if (!entry.title.empty()) {
			text += QStringLiteral(" | ") +
			        QString::fromStdString(entry.title);
		}
		gameCombo_->addItem(text);
		const int index = gameCombo_->count() - 1;
		gameCombo_->setItemData(
		    index, QString::fromStdString(entry.exe), kRoleExe);
		gameCombo_->setItemData(
		    index, QString::fromStdString(entry.title), kRoleTitle);
	}

	// 尽量保持（或恢复）目标 exe 的选中状态
	const std::string target =
	    keepExe.empty() ? currentExe_ : keepExe;
	int selectIndex = -1;
	if (!target.empty()) {
		for (int i = 0; i < gameCombo_->count(); ++i) {
			if (gameCombo_->itemData(i, kRoleExe)
			        .toString()
			        .toStdString() == target) {
				selectIndex = i;
				break;
			}
		}
	}
	gameCombo_->setCurrentIndex(selectIndex);
}

void KeymapPanel::OnRefreshClicked()
{
	if (!ConfirmDiscardIfDirty()) {
		return;
	}
	// 用户已明确放弃：清除脏标记，避免刷新后当前游戏消失时重复弹问
	dirty_ = false;
	StopCapture();
	RefreshGameList(currentExe_);
	if (gameCombo_->currentIndex() < 0 &&
	    gameCombo_->count() > 0) {
		gameCombo_->setCurrentIndex(0);
	} else {
		LoadCurrentGame();
	}
}

void KeymapPanel::OnGameChanged(int index)
{
	if (index < 0) {
		// 没有任何可选游戏：清空编辑状态
		if (!currentExe_.empty()) {
			if (!ConfirmDiscardIfDirty()) {
				// 用户不愿放弃：恢复原选择（信号被阻塞，手动加载）
				for (int i = 0; i < gameCombo_->count(); ++i) {
					if (gameCombo_->itemData(i, kRoleExe)
					        .toString()
					        .toStdString() == currentExe_) {
						const QSignalBlocker blocker(gameCombo_);
						gameCombo_->setCurrentIndex(i);
						break;
					}
				}
				return;
			}
		}
		currentExe_.clear();
		currentTitle_.clear();
		loadedMovementKeys_.reset();
		loading_ = true;
		table_->setRowCount(0);
		loading_ = false;
		dirty_ = false;
		UpdateActionsEnabled();
		SetStatus(QStringLiteral("未选择游戏。"));
		return;
	}

	if (currentExe_ !=
	    gameCombo_->itemData(index, kRoleExe).toString().toStdString()) {
		if (!ConfirmDiscardIfDirty()) {
			// 用户选择不放弃：恢复原选中项
			for (int i = 0; i < gameCombo_->count(); ++i) {
				if (gameCombo_->itemData(i, kRoleExe)
				        .toString()
				        .toStdString() == currentExe_) {
					const QSignalBlocker blocker(gameCombo_);
					gameCombo_->setCurrentIndex(i);
					return;
				}
			}
			return;
		}
	}

	StopCapture();
	LoadCurrentGame();
}

void KeymapPanel::LoadCurrentGame()
{
	const int index = gameCombo_->currentIndex();
	if (index < 0) {
		currentExe_.clear();
		currentTitle_.clear();
		loadedMovementKeys_.reset();
		loading_ = true;
		table_->setRowCount(0);
		loading_ = false;
		dirty_ = false;
		UpdateActionsEnabled();
		return;
	}

	currentExe_ =
	    gameCombo_->itemData(index, kRoleExe).toString().toStdString();
	currentTitle_ =
	    gameCombo_->itemData(index, kRoleTitle).toString().toStdString();

	std::map<std::string, KeymapEntry> keymap;
	std::optional<GameProfile> profile =
	    PluginConfig::Instance().GetGameProfile(currentExe_);
	if (profile.has_value()) {
		if (currentTitle_.empty()) {
			// 档案有标题但当前没有窗口标题时沿用档案标题
			currentTitle_ = profile->title;
		}
		// 关键：带回 movementKeys，保存时原样回写，防止整档案覆盖丢失
		loadedMovementKeys_ = profile->movementKeys;
		keymap = profile->keymap;
	} else {
		loadedMovementKeys_.reset();
		if (currentTitle_.empty()) {
			currentTitle_ = currentExe_; // 无档案时以 exe 名为标题
		}
	}

	loading_ = true;
	table_->setRowCount(0);
	table_->setRowCount(static_cast<int>(keymap.size()));

	int row = 0;
	for (const auto& [keyName, entry] : keymap) {
		auto* keyItem =
		    new QTableWidgetItem(QString::fromStdString(keyName));
		table_->setItem(row, kColKey, keyItem);

		auto* typeCombo = new QComboBox(this);
		typeCombo->addItem(QStringLiteral("keyboard"),
		                   QString::fromUtf8(kTypeKeyboard));
		typeCombo->addItem(QStringLiteral("mouse"),
		                   QString::fromUtf8(kTypeMouse));
		const bool isMouse = (entry.type == kTypeMouse);
		typeCombo->setCurrentIndex(isMouse ? 1 : 0);
		table_->setCellWidget(row, kColType, typeCombo);
		connect(typeCombo,
		        qOverload<int>(&QComboBox::currentIndexChanged), this,
		        [this](int) {
			        if (!loading_) {
				        dirty_ = true;
			        }
		        });

		auto* actionItem =
		    new QTableWidgetItem(QString::fromStdString(entry.action));
		table_->setItem(row, kColAction, actionItem);
		++row;
	}

	loading_ = false;
	dirty_ = false;
	UpdateActionsEnabled();

	if (profile.has_value()) {
		SetStatus(QStringLiteral("已加载档案 “%1”（%2 个映射）。")
		              .arg(QString::fromStdString(currentTitle_))
		              .arg(table_->rowCount()));
	} else {
		SetStatus(QStringLiteral("新档案 “%1”：编辑后点击保存。")
		              .arg(QString::fromStdString(currentTitle_)));
	}
}

bool KeymapPanel::ConfirmDiscardIfDirty()
{
	if (!dirty_ || loading_) {
		return true;
	}
	const auto answer = QMessageBox::question(
	    this, QStringLiteral("未保存的按键映射"),
	    QStringLiteral("当前按键映射有未保存的修改，切换/刷新将丢弃这些修改。\n"
	                   "确定放弃吗？"),
	    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	return answer == QMessageBox::Yes;
}

// ---------------------------------------------------------------------------
// 表格编辑
// ---------------------------------------------------------------------------

void KeymapPanel::OnCellChanged(int row, int column)
{
	if (loading_) {
		return;
	}
	dirty_ = true;

	// 手工编辑按键标识后，按规范名自动判定一次类型（用户仍可手动改）
	if (column == kColKey) {
		const std::string keyName =
		    table_->item(row, kColKey)
		        ->text()
		        .trimmed()
		        .toStdString();
		if (!keyName.empty()) {
			AutoSetRowType(row, keyName);
		}
	}
}

void KeymapPanel::AutoSetRowType(int row,
                                 const std::string& keyName)
{
	auto* combo = qobject_cast<QComboBox*>(
	    table_->cellWidget(row, kColType));
	if (combo == nullptr) {
		return;
	}
	const QString wanted =
	    IsMouseName(keyName) ? QString::fromUtf8(kTypeMouse)
	                         : QString::fromUtf8(kTypeKeyboard);
	for (int i = 0; i < combo->count(); ++i) {
		if (combo->itemData(i).toString() == wanted) {
			const QSignalBlocker blocker(combo);
			combo->setCurrentIndex(i);
			break;
		}
	}
}

void KeymapPanel::AddEmptyRow()
{
	loading_ = true;
	const int row = table_->rowCount();
	table_->insertRow(row);
	table_->setItem(row, kColKey, new QTableWidgetItem());
	auto* typeCombo = new QComboBox(this);
	typeCombo->addItem(QStringLiteral("keyboard"),
	                   QString::fromUtf8(kTypeKeyboard));
	typeCombo->addItem(QStringLiteral("mouse"),
	                   QString::fromUtf8(kTypeMouse));
	table_->setCellWidget(row, kColType, typeCombo);
	connect(typeCombo,
	        qOverload<int>(&QComboBox::currentIndexChanged), this,
	        [this](int) {
		        if (!loading_) {
			        dirty_ = true;
		        }
	        });
	table_->setItem(row, kColAction, new QTableWidgetItem());
	loading_ = false;
	dirty_ = true;

	table_->setCurrentCell(row, kColKey);
	table_->editItem(table_->item(row, kColKey));
}

void KeymapPanel::OnAddRowClicked()
{
	if (currentExe_.empty()) {
		return;
	}
	AddEmptyRow();
}

void KeymapPanel::OnDeleteRowClicked()
{
	const int row = table_->currentRow();
	if (row < 0 || row >= table_->rowCount()) {
		QMessageBox::information(
		    this, QStringLiteral("删除选中行"),
		    QStringLiteral("请先选中要删除的映射行。"));
		return;
	}
	table_->removeRow(row);
	if (!loading_) {
		dirty_ = true;
	}
}

// ---------------------------------------------------------------------------
// 保存
// ---------------------------------------------------------------------------

void KeymapPanel::OnSaveClicked()
{
	if (currentExe_.empty()) {
		QMessageBox::warning(
		    this, QStringLiteral("保存按键映射"),
		    QStringLiteral("请先选择一个游戏。"));
		return;
	}

	std::map<std::string, KeymapEntry> keymap;

	// 逐条校验，定位到首条错误行
	for (int row = 0; row < table_->rowCount(); ++row) {
		const QString keyQ =
		    (table_->item(row, kColKey) != nullptr)
		        ? table_->item(row, kColKey)->text().trimmed()
		        : QString();
		const QString actionQ =
		    (table_->item(row, kColAction) != nullptr)
		        ? table_->item(row, kColAction)->text().trimmed()
		        : QString();
		const std::string key = keyQ.toStdString();
		const std::string type = RowType(table_, row);
		const std::string action = actionQ.toStdString();

		if (key.empty()) {
			QMessageBox::warning(
			    this, QStringLiteral("保存按键映射"),
			    QStringLiteral("第 %1 行的按键标识为空，请填写或删除该行。")
			        .arg(row + 1));
			table_->setCurrentCell(row, kColKey);
			return;
		}

		if (IsMouseName(key)) {
			if (type != kTypeMouse) {
				QMessageBox::warning(
				    this, QStringLiteral("保存按键映射"),
				    QStringLiteral("第 %1 行：“%2” 是鼠标标识，类型必须为 mouse。")
				        .arg(row + 1)
				        .arg(keyQ));
				table_->setCurrentCell(row, kColType);
				return;
			}
		} else if (!IsKeyboardName(key)) {
			QMessageBox::warning(
			    this, QStringLiteral("保存按键映射"),
			    QStringLiteral("第 %1 行：“%2” 不是合法的按键标识。")
			        .arg(row + 1)
			        .arg(keyQ));
			table_->setCurrentCell(row, kColKey);
			return;
		} else if (type != kTypeKeyboard) {
			QMessageBox::warning(
			    this, QStringLiteral("保存按键映射"),
			    QStringLiteral("第 %1 行：“%2” 是键盘标识，类型必须为 keyboard。")
			        .arg(row + 1)
			        .arg(keyQ));
			table_->setCurrentCell(row, kColType);
			return;
		}

		if (actionQ.isEmpty()) {
			QMessageBox::warning(
			    this, QStringLiteral("保存按键映射"),
			    QStringLiteral("第 %1 行（%2）的动作语义为空，请填写中文动作描述。")
			        .arg(row + 1)
			        .arg(keyQ));
			table_->setCurrentCell(row, kColAction);
			return;
		}
		// 长度按字符数（UTF-16 码元）计，中文每个字计 1
		if (actionQ.length() > kMaxActionLength) {
			QMessageBox::warning(
			    this, QStringLiteral("保存按键映射"),
			    QStringLiteral("第 %1 行（%2）的动作语义过长：trim 后需为 1–%3 个字符，"
			                   "当前 %4 个。")
			        .arg(row + 1)
			        .arg(keyQ)
			        .arg(kMaxActionLength)
			        .arg(actionQ.length()));
			table_->setCurrentCell(row, kColAction);
			return;
		}

		if (!keymap.try_emplace(key, KeymapEntry{type, action}).second) {
			QMessageBox::warning(
			    this, QStringLiteral("保存按键映射"),
			    QStringLiteral("第 %1 行：按键标识 “%2” 重复，每个键只能映射一个动作。")
			        .arg(row + 1)
			        .arg(keyQ));
			table_->setCurrentCell(row, kColKey);
			return;
		}
	}

	// movementKeys 原样回写：SaveGameProfile 整体覆盖该 GameProfile，
	// 不回写会丢失设置页为该游戏保存过的移动键覆盖
	GameProfile profile;
	profile.title = currentTitle_;
	profile.movementKeys = loadedMovementKeys_;
	profile.keymap = std::move(keymap);
	PluginConfig::Instance().SaveGameProfile(currentExe_, profile);

	dirty_ = false;
	GRT_LOG_INFO("keymap 已保存：exe=%s title=%s 条目数=%zu",
	             currentExe_.c_str(), currentTitle_.c_str(),
	             profile.keymap.size());

	const QString savedAt =
	    QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
	SetStatus(QStringLiteral("已保存（%1），共 %2 个映射。")
	              .arg(savedAt)
	              .arg(profile.keymap.size()));

	// 保存可能引入新档案 / 标题变化，刷新下拉保持列表一致
	RefreshGameList(currentExe_);
}

// ---------------------------------------------------------------------------
// “捕获下一次输入”
// ---------------------------------------------------------------------------

void KeymapPanel::OnCaptureClicked()
{
	if (capturing_) {
		// 按钮在等待期间承担“取消”功能
		StopCapture(QStringLiteral("已取消捕获。"));
		return;
	}
	StartCapture();
}

void KeymapPanel::StartCapture()
{
	if (currentExe_.empty()) {
		QMessageBox::warning(
		    this, QStringLiteral("捕获下一次输入"),
		    QStringLiteral("请先选择一个游戏。"));
		return;
	}

	// 复用进程内共享采集服务：Windows 对同一 usage 的 RIDEV_INPUTSINK
	// 注册是"后者覆盖"，多实例会导致只有最后一个注册窗口收到 WM_INPUT。
	// 这里只订阅，绝不 Stop 共享服务（录制会话与全局热键仍在使用）。
	RawInputService& service = RawInputService::Shared();
	if (!service.Start()) {
		QMessageBox::warning(
		    this, QStringLiteral("捕获下一次输入"),
		    QStringLiteral("无法启动临时输入采集（Raw Input 注册失败），请重试。"));
		return;
	}
	captureSubId_ = service.Subscribe();

	capturing_ = true;
	captureButton_->setText(
	    QStringLiteral("等待输入中…（点击取消）"));
	gameCombo_->setEnabled(false);
	refreshButton_->setEnabled(false);
	addButton_->setEnabled(false);
	deleteButton_->setEnabled(false);
	saveButton_->setEnabled(false);
	SetStatus(QStringLiteral("请按下要映射的键 / 鼠标按键 / 滚轮（5 秒内有效）…"));

	pollTimer_->start();
	timeoutTimer_->start();
}

void KeymapPanel::StopCapture(const QString& statusText)
{
	if (pollTimer_ != nullptr) {
		pollTimer_->stop();
	}
	if (timeoutTimer_ != nullptr) {
		timeoutTimer_->stop();
	}
	if (captureSubId_ != RawInputService::kInvalidSubscription) {
		RawInputService::Shared().Unsubscribe(captureSubId_);
		captureSubId_ = RawInputService::kInvalidSubscription;
	}
	capturing_ = false;
	if (captureButton_ != nullptr) {
		captureButton_->setText(
		    QStringLiteral("捕获下一次输入…"));
	}
	UpdateActionsEnabled();
	if (!statusText.isEmpty()) {
		SetStatus(statusText);
	}
}

void KeymapPanel::OnPollCapture()
{
	if (!capturing_ || captureSubId_ == RawInputService::kInvalidSubscription) {
		return;
	}

	const std::vector<InputEvent> events =
	    RawInputService::Shared().Poll(captureSubId_);
	for (const InputEvent& event : events) {
		std::string name;

		switch (event.type) {
			case InputType::Key:
				// 只取按下；255 为 overrun 伪消息，忽略
				// （按住连发的第一个 down 即结束采集）
				if (!event.key_down || event.vkey == 255) {
					continue;
				}
				name = KeyVkeyToName(event.vkey);
				break;

			case InputType::MouseButton:
				if (!event.key_down) {
					continue; // 忽略抬起
				}
				name = MouseButtonToName(event.button);
				if (name.empty()) {
					continue;
				}
				break;

			case InputType::MouseWheel:
				if (event.wheel_delta == 0) {
					continue;
				}
				// delta 可能 >120（连续 notch），转换只取方向
				name = WheelEventName(event.horizontal_wheel,
				                      event.wheel_delta);
				break;

			case InputType::MouseMove:
				continue; // 忽略鼠标移动
		}

		if (name.empty()) {
			continue;
		}

		AcceptCapturedKey(name);
		return;
	}
}

void KeymapPanel::OnCaptureTimeout()
{
	if (capturing_) {
		StopCapture(QStringLiteral("捕获超时：5 秒内未收到有效输入。"));
	}
}

void KeymapPanel::AcceptCapturedKey(const std::string& name)
{
	const QString nameQ = QString::fromStdString(name);

	// 先停服务并复位按钮
	StopCapture();

	const int existing = FindRowByKey(name);
	if (existing >= 0) {
		table_->setCurrentCell(existing, kColAction);
		table_->scrollToItem(table_->item(existing, kColKey));
		SetStatus(QStringLiteral("按键 “%1” 已存在于第 %2 行，已为你选中。")
		              .arg(nameQ)
		              .arg(existing + 1));
		return;
	}

	// 新增一行：填好标识与自动类型，焦点落到动作单元格
	loading_ = true;
	const int row = table_->rowCount();
	table_->insertRow(row);
	table_->setItem(row, kColKey, new QTableWidgetItem(nameQ));

	auto* typeCombo = new QComboBox(this);
	typeCombo->addItem(QStringLiteral("keyboard"),
	                   QString::fromUtf8(kTypeKeyboard));
	typeCombo->addItem(QStringLiteral("mouse"),
	                   QString::fromUtf8(kTypeMouse));
	table_->setCellWidget(row, kColType, typeCombo);
	connect(typeCombo,
	        qOverload<int>(&QComboBox::currentIndexChanged), this,
	        [this](int) {
		        if (!loading_) {
			        dirty_ = true;
		        }
	        });
	AutoSetRowType(row, name);
	table_->setItem(row, kColAction, new QTableWidgetItem());
	loading_ = false;
	dirty_ = true;

	table_->setCurrentCell(row, kColAction);
	table_->scrollToItem(table_->item(row, kColKey));
	table_->editItem(table_->item(row, kColAction));
	SetStatus(QStringLiteral("已捕获 “%1”，请填写动作语义后保存。")
	              .arg(nameQ));
}

int KeymapPanel::FindRowByKey(const std::string& keyName) const
{
	const QString target = QString::fromStdString(keyName);
	for (int row = 0; row < table_->rowCount(); ++row) {
		const QTableWidgetItem* item = table_->item(row, kColKey);
		if (item != nullptr &&
		    item->text().trimmed() == target) {
			return row;
		}
	}
	return -1;
}

// ---------------------------------------------------------------------------
// 杂项
// ---------------------------------------------------------------------------

void KeymapPanel::UpdateActionsEnabled()
{
	const bool hasGame = !currentExe_.empty();
	const bool busy = capturing_;
	gameCombo_->setEnabled(hasGame && !busy);
	refreshButton_->setEnabled(!busy);
	addButton_->setEnabled(hasGame && !busy);
	deleteButton_->setEnabled(hasGame && !busy);
	captureButton_->setEnabled(hasGame);
	saveButton_->setEnabled(hasGame && !busy);
}

void KeymapPanel::SetStatus(const QString& text)
{
	statusLabel_->setText(text);
}

} // namespace grt::ui
