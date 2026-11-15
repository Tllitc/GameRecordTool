/**
 * @file history_panel.h
 * @brief “历史”页：会话历史表格 + 打开目录 + 查看校验报告 + 重新校验 + 清空。
 *
 * 数据由独立的 HistoryStore（obs_module_config_path/history.json）持久化。
 * 录制完成后由 MainPanel 调用 Refresh() 重新加载；双击某行或选中后点
 * “打开目录”可用系统文件管理器打开对应会话目录。
 *
 * “重新校验”在后台线程执行（ffprobe/文件解析不阻塞 UI），完成后发出
 * ReverifyRequested/VerifyFinished 信号链路：面板自身不直接调
 * HistoryStore 之外的会话校验逻辑，由 MainPanel 接线 AsyncVerifyRunner。
 */

#pragma once

#include <QWidget>

#include <string>

class QPushButton;
class QTableWidget;

namespace grt::ui {

class HistoryPanel : public QWidget {
	Q_OBJECT

public:
	explicit HistoryPanel(QWidget* parent = nullptr);

	/// 从 history.json 重新加载并刷新表格（主线程调用）
	void Refresh();

signals:
	/// 用户点击“重新校验”；参数为会话 ID（由 MainPanel 启动后台校验）
	void ReverifyRequested(const std::string& sessionId);

private slots:
	void OnOpenDirClicked();
	void OnClearClicked();
	void OnCellDoubleClicked(int row, int column);
	void OnReportClicked();
	void OnReverifyClicked();
	void OnSelectionChanged();

private:
	/// 打开第 row 行记录对应的会话目录（目录无效时提示）
	void OpenDirForRow(int row);
	/// 第 row 行对应的会话 ID（行序为“最新在最上”的倒序）
	std::string SessionIdAtRow(int row) const;
	void ShowReportForRow(int row);

	QTableWidget* table_ = nullptr;
	QPushButton* reportButton_ = nullptr;
	QPushButton* reverifyButton_ = nullptr;
};

} // namespace grt::ui
