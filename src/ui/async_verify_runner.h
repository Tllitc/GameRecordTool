/**
 * @file async_verify_runner.h
 * @brief 后台线程执行录制三件套校验（Task 12.3）。
 *
 * 校验会启动 ffprobe 子进程并解析较大的操作数据 JSON，可能耗时数百
 * 毫秒，不能阻塞 Qt 主线程。每个校验任务在独立 std::thread 中运行
 * grt::VerifySessionArtifacts，完成后通过 QMetaObject::invokeMethod
 * 以 QueuedConnection 回到主线程回调；runner 自身为 QObject 且生命周期
 * 由 MainPanel 持有，OBS 卸载插件时随 dock 销毁，未完成的线程采用
 * detach（只读操作，不触碰任何 Qt/OBS 对象），结果回调通过 QPointer
 * 判空，避免悬挂。
 */

#pragma once

#include <QObject>

#include <functional>
#include <string>

namespace grt::ui {

/// 一次后台校验的输入（全部为 UTF-8 快照，线程内不再访问任何 UI 对象）
struct VerifyTask {
	std::string sessionId;  ///< 历史记录会话 ID（回写校验结果用）
	std::string dir;        ///< 会话目录
	std::string videoName;  ///< 视频文件名
};

/**
 * @brief 完成回调参数。
 * @param sessionId 任务会话 ID
 * @param ok        校验是否通过（无 error）
 * @param status    状态短文案（"通过"/"失败"）
 * @param detail    完整报告文本（ToDetailText）
 */
using VerifyFinishedCallback =
    std::function<void(const std::string& sessionId, bool ok,
                       const std::string& status,
                       const std::string& detail)>;

class AsyncVerifyRunner : public QObject {
	Q_OBJECT

public:
	explicit AsyncVerifyRunner(QObject* parent = nullptr);
	~AsyncVerifyRunner() override;

	/**
	 * @brief 启动一次后台校验（主线程调用，立即返回）。
	 * @param task     会话定位信息快照
	 * @param finished 主线程回调（QueuedConnection 语义）
	 */
	void Start(VerifyTask task, VerifyFinishedCallback finished);

private:
	struct State;
};

} // namespace grt::ui
