/**
 * @file history_panel.cpp
 * @brief “历史”页实现。
 */

#include "ui/history_panel.h"

#include <string>

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include "ui/history_store.h"

namespace grt::ui {

namespace {

/// 表格列枚举
enum HistoryColumn {
	kColTime = 0,    // 开始时间（本地展示）
	kColGame,        // 游戏（exe 名 + 标题）
	kColDuration,    // 时长
	kColDir,         // 目录（截断显示，完整路径放 tooltip）
	kColVerify,      // 校验状态
	kColumnCount,
};

/// 行内隐藏的会话 ID（倒序映射回历史记录用）
constexpr int kSessionIdRole = Qt::UserRole + 1;
/// 行内隐藏的完整报告（供“查看报告”直接展示，避免再读一次盘）
constexpr int kReportRole = Qt::UserRole + 2;

/// 秒数格式化为 m:ss / h:mm:ss
QString FormatDuration(int totalSeconds)
{
	if (totalSeconds < 0) {
		totalSeconds = 0;
	}
	const int hours = totalSeconds / 3600;
	const int minutes = (totalSeconds % 3600) / 60;
	const int seconds = totalSeconds % 60;
	if (hours > 0) {
		return QString::asprintf("%d:%02d:%02d", hours, minutes,
		                         seconds);
	}
	return QString::asprintf("%d:%02d", minutes, seconds);
}

/// UTC ISO-8601 转本地可读时间；失败时退回存储的展示时间或原值
QString FormatStartTime(const HistoryEntry& entry)
{
	const QDateTime utc =
	    QDateTime::fromString(QString::fromStdString(entry.startUtc),
	                          Qt::ISODate);
	if (utc.isValid()) {
		return utc.toLocalTime().toString(
		    QStringLiteral("yyyy-MM-dd HH:mm:ss"));
	}
	if (!entry.time.empty()) {
		return QString::fromStdString(entry.time);
	}
	return QStringLiteral("-");
}

/// 校验状态 -> 前景色（通过深绿 / 失败红 / 校验中灰 / 未校验默认）
QColor StatusColor(const std::string& status)
{
	if (status == "通过") {
		return QColor(0x1B, 0x7F, 0x3B);
	}
	if (status == "失败") {
		return QColor(0xC0, 0x39, 0x2B);
	}
	if (status == "校验中…") {
		return QColor(0x80, 0x80, 0x80);
	}
	return QColor();
}

} // 匿名命名空间

HistoryPanel::HistoryPanel(QWidget* parent) : QWidget(parent)
{
	auto* root = new QVBoxLayout(this);

	table_ = new QTableWidget(0, kColumnCount, this);
	table_->setHorizontalHeaderLabels(
	    {QStringLiteral("开始时间"), QStringLiteral("游戏"),
	     QStringLiteral("时长"), QStringLiteral("目录"),
	     QStringLiteral("校验状态")});
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setAlternatingRowColors(true);
	table_->verticalHeader()->setVisible(false);
	table_->setColumnWidth(kColTime, 150);
	table_->setColumnWidth(kColGame, 180);
	table_->setColumnWidth(kColDuration, 70);
	table_->setColumnWidth(kColDir, 320);
	table_->setColumnWidth(kColVerify, 90);
	table_->horizontalHeader()->setStretchLastSection(true);
	root->addWidget(table_);

	auto* buttonRow = new QHBoxLayout();
	auto* openButton =
	    new QPushButton(QStringLiteral("打开目录"), this);
	auto* reportButton =
	    new QPushButton(QStringLiteral("查看报告"), this);
	auto* reverifyButton =
	    new QPushButton(QStringLiteral("重新校验"), this);
	auto* refreshButton =
	    new QPushButton(QStringLiteral("刷新"), this);
	auto* clearButton =
	    new QPushButton(QStringLiteral("清空历史"), this);
	reportButton_ = reportButton;
	reverifyButton_ = reverifyButton;
	buttonRow->addWidget(openButton);
	buttonRow->addWidget(reportButton);
	buttonRow->addWidget(reverifyButton);
	buttonRow->addWidget(refreshButton);
	buttonRow->addStretch();
	buttonRow->addWidget(clearButton);
	root->addLayout(buttonRow);

	connect(openButton, &QPushButton::clicked, this,
	        &HistoryPanel::OnOpenDirClicked);
	connect(reportButton, &QPushButton::clicked, this,
	        &HistoryPanel::OnReportClicked);
	connect(reverifyButton, &QPushButton::clicked, this,
	        &HistoryPanel::OnReverifyClicked);
	connect(refreshButton, &QPushButton::clicked, this,
	        &HistoryPanel::Refresh);
	connect(clearButton, &QPushButton::clicked, this,
	        &HistoryPanel::OnClearClicked);
	connect(table_, &QTableWidget::cellDoubleClicked, this,
	        &HistoryPanel::OnCellDoubleClicked);
	connect(table_->selectionModel(),
	        &QItemSelectionModel::selectionChanged, this,
	        &HistoryPanel::OnSelectionChanged);

	Refresh();
}

void HistoryPanel::Refresh()
{
	const std::vector<HistoryEntry> entries = HistoryStore::Load();
	table_->setRowCount(0);
	table_->setRowCount(static_cast<int>(entries.size()));

	// 最新一条显示在最上方
	for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
		const HistoryEntry& entry =
		    entries[entries.size() - 1 - static_cast<size_t>(i)];
		const int row = i;

		auto* timeItem =
		    new QTableWidgetItem(FormatStartTime(entry));
		timeItem->setData(kSessionIdRole,
		                  QString::fromStdString(entry.sessionId));
		timeItem->setData(kReportRole,
		                  QString::fromStdString(entry.verifyDetail));
		table_->setItem(row, kColTime, timeItem);

		QString gameText = QString::fromStdString(entry.exe);
		if (!entry.title.empty()) {
			gameText += QStringLiteral(" | ") +
			            QString::fromStdString(entry.title);
		}
		auto* gameItem = new QTableWidgetItem(gameText);
		table_->setItem(row, kColGame, gameItem);

		auto* durationItem =
		    new QTableWidgetItem(FormatDuration(entry.durationSeconds));
		durationItem->setTextAlignment(Qt::AlignCenter);
		table_->setItem(row, kColDuration, durationItem);

		auto* dirItem =
		    new QTableWidgetItem(QString::fromStdString(entry.dir));
		dirItem->setToolTip(QString::fromStdString(entry.dir));
		table_->setItem(row, kColDir, dirItem);

		auto* verifyItem = new QTableWidgetItem(
		    QString::fromStdString(entry.verifyStatus));
		verifyItem->setTextAlignment(Qt::AlignCenter);
		const QColor color = StatusColor(entry.verifyStatus);
		if (color.isValid()) {
			verifyItem->setForeground(QBrush(color));
		}
		table_->setItem(row, kColVerify, verifyItem);
	}
	OnSelectionChanged();
}

void HistoryPanel::OnCellDoubleClicked(int row, int /*column*/)
{
	OpenDirForRow(row);
}

void HistoryPanel::OnOpenDirClicked()
{
	const int row = table_->currentRow();
	if (row >= 0) {
		OpenDirForRow(row);
	}
}

void HistoryPanel::OnReportClicked()
{
	const int row = table_->currentRow();
	if (row >= 0) {
		ShowReportForRow(row);
	}
}

void HistoryPanel::OnReverifyClicked()
{
	const int row = table_->currentRow();
	if (row < 0) {
		return;
	}
	const std::string sessionId = SessionIdAtRow(row);
	if (sessionId.empty()) {
		return;
	}
	emit ReverifyRequested(sessionId);
}

void HistoryPanel::OnSelectionChanged()
{
	const bool hasSelection = table_->currentRow() >= 0;
	if (reportButton_ != nullptr) {
		reportButton_->setEnabled(hasSelection);
	}
	if (reverifyButton_ != nullptr) {
		// “校验中…”的行不允许重复触发
		bool canVerify = false;
		if (hasSelection) {
			QTableWidgetItem* statusItem =
			    table_->item(table_->currentRow(), kColVerify);
			canVerify = statusItem != nullptr &&
			            statusItem->text() !=
			                QStringLiteral("校验中…");
		}
		reverifyButton_->setEnabled(canVerify);
	}
}

std::string HistoryPanel::SessionIdAtRow(int row) const
{
	if (row < 0 || row >= table_->rowCount()) {
		return {};
	}
	QTableWidgetItem* item = table_->item(row, kColTime);
	if (item == nullptr) {
		return {};
	}
	return item->data(kSessionIdRole).toString().toStdString();
}

void HistoryPanel::ShowReportForRow(int row)
{
	if (row < 0 || row >= table_->rowCount()) {
		return;
	}
	QTableWidgetItem* timeItem = table_->item(row, kColTime);
	QTableWidgetItem* statusItem = table_->item(row, kColVerify);
	if (timeItem == nullptr || statusItem == nullptr) {
		return;
	}
	const QString report = timeItem->data(kReportRole).toString();
	if (report.isEmpty()) {
		QMessageBox::information(
		    this, QStringLiteral("校验报告"),
		    QStringLiteral("该会话尚未校验。可点击“重新校验”生成报告。"));
		return;
	}
	QMessageBox box(this);
	box.setWindowTitle(QStringLiteral("校验报告"));
	box.setIcon(statusItem->text() == QStringLiteral("通过")
	                ? QMessageBox::Information
	                : QMessageBox::Warning);
	box.setText(QStringLiteral("状态：%1").arg(statusItem->text()));
	box.setDetailedText(report);
	box.exec();
}

void HistoryPanel::OpenDirForRow(int row)
{
	if (row < 0 || row >= table_->rowCount()) {
		return;
	}
	const QString dir = table_->item(row, kColDir)->text();
	if (dir.isEmpty()) {
		QMessageBox::information(
		    this, QStringLiteral("打开目录"),
		    QStringLiteral("该记录没有保存会话目录。"));
		return;
	}
	const QFileInfo info(dir);
	if (!info.exists() || !info.isDir()) {
		QMessageBox::warning(
		    this, QStringLiteral("打开目录"),
		    QStringLiteral("目录不存在或已被移动/删除：\n%1").arg(dir));
		return;
	}
	// 用系统文件管理器打开目录（不选中具体文件）
	QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void HistoryPanel::OnClearClicked()
{
	const auto answer = QMessageBox::question(
	    this, QStringLiteral("清空历史"),
	    QStringLiteral("确定清空全部会话历史吗？\n"
	                   "（仅清空本插件历史记录，不会删除任何录制文件）"),
	    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (answer != QMessageBox::Yes) {
		return;
	}
	if (!HistoryStore::Clear()) {
		QMessageBox::warning(
		    this, QStringLiteral("清空历史"),
		    QStringLiteral("历史文件写入失败，请检查配置目录权限。"));
		return;
	}
	Refresh();
}

} // namespace grt::ui
