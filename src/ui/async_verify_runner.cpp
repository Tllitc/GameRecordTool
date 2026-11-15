/**
 * @file async_verify_runner.cpp
 * @brief 后台录制校验任务实现。
 */

#include "ui/async_verify_runner.h"

#include <QPointer>
#include <QString>
#include <QThread>
#include <QtGlobal>

#include <memory>
#include <thread>
#include <utility>

#include <QApplication>

#include "verify/session_verifier.h"

namespace grt::ui {

namespace {

/// 一次校验任务的跨线程共享状态。工作线程只写 result* 字段；投递回主线程
/// 后由 qApp 事件循环中的 lambda 读取，无并发访问。
struct VerifyState {
	VerifyTask task;
	QPointer<AsyncVerifyRunner> runner;
	VerifyFinishedCallback callback;

	bool finished = false;
	bool ok = false;
	std::string status;
	std::string detail;
};

} // namespace

AsyncVerifyRunner::AsyncVerifyRunner(QObject* parent) : QObject(parent) {}

AsyncVerifyRunner::~AsyncVerifyRunner() = default;

void AsyncVerifyRunner::Start(VerifyTask task,
                              VerifyFinishedCallback finished)
{
	auto state = std::make_shared<VerifyState>();
	state->task = std::move(task);
	state->runner = QPointer<AsyncVerifyRunner>(this);
	state->callback = std::move(finished);

	// 工作线程：纯文件 IO + 子进程，不访问任何 Qt/OBS/UI 对象。
	std::thread([state]() {
		const std::string ffprobe = ResolveFfprobePath();
		const VerifyReport report = VerifySessionArtifacts(
		    state->task.dir, state->task.videoName, ffprobe);
		state->ok = report.ok;
		state->status = report.StatusText();
		state->detail = report.ToDetailText();
		state->finished = true;

		// 投递到 qApp（生命周期覆盖整个 OBS 进程），实际执行时在主线程
		// 检查 runner 是否仍存在，避免访问已析构对象。
		QMetaObject::invokeMethod(
		    qApp,
		    [state]() {
			    if (state->runner == nullptr || !state->finished) {
				    return;
			    }
			    if (state->callback) {
				    state->callback(state->task.sessionId, state->ok,
				                    state->status, state->detail);
			    }
		    },
		    Qt::QueuedConnection);
	}).detach();
}

} // namespace grt::ui
