/**
 * @file video_time_calibrator.cpp
 * @brief VideoTimeCalibrator 的 OBS 接线层。OffsetEstimator 的纯数学实现
 *        位于 offset_estimator.cpp（无 OBS 依赖，可独立单元测试）。
 */

#include "video/video_time_calibrator.h"

#include <obs.h>

#include <cstddef>

#include "core/clock.h"
#include "core/log.h"

namespace grt {

namespace {

/// 首次输出校准诊断日志的样本数（之后每累积同样多的样本再报告一次）
constexpr std::size_t kDiagInterval = 30;

/// 速率比偏离 1.0 超过该阈值时以 WARN 级别报告（仅诊断，不做校正）
constexpr double kRateWarnEpsilon = 0.01;

} // namespace

// ============================================================================
// VideoTimeCalibrator —— OBS raw-video 回调接线
// ============================================================================

VideoTimeCalibrator::VideoTimeCalibrator() = default;

VideoTimeCalibrator::~VideoTimeCalibrator()
{
	// 防御性停止，避免插件卸载后回调仍指向本对象
	Stop();
}

bool VideoTimeCalibrator::Start()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (running_) {
		return true;
	}

	obs_video_info ovi{};
	if (!obs_get_video_info(&ovi)) {
		// 视频子系统尚未初始化（无输出），无法构造转换参数
		GRT_LOG_ERROR("Start 校准失败：obs_get_video_info 返回 false（视频输出未就绪）");
		return false;
	}

	// 宽高给 0 表示沿用视频输出尺寸；libobs 对宽高为 0 的转换按输出原生尺寸处理。
	// 仅取 pts 不消费像素内容，格式选 NV12，色彩/范围给 DEFAULT。
	video_scale_info conversion{};
	conversion.format = VIDEO_FORMAT_NV12;
	conversion.width = 0;
	conversion.height = 0;
	conversion.range = VIDEO_RANGE_DEFAULT;
	conversion.colorspace = VIDEO_CS_DEFAULT;

	estimator_.Reset();
	lastDiagCount_.store(0);
	startMarked_ = false;
	endMarked_ = false;
	startQpc_ = 0.0;
	endQpc_ = 0.0;

	try {
		obs_add_raw_video_callback(&conversion, &VideoTimeCalibrator::RawVideoCallback, this);
	} catch (...) {
		GRT_LOG_ERROR("Start 校准异常：obs_add_raw_video_callback 抛出异常");
		return false;
	}

	callbackAttached_ = true;
	running_ = true;
	GRT_LOG_INFO("视频时间戳校准已启动（raw-video 回调，输出 %ux%u）", ovi.output_width, ovi.output_height);
	return true;
}

void VideoTimeCalibrator::Stop()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (!running_ && !callbackAttached_) {
		return; // 未运行且回调未注册，无需处理
	}

	if (callbackAttached_) {
		try {
			obs_remove_raw_video_callback(&VideoTimeCalibrator::RawVideoCallback, this);
		} catch (...) {
			GRT_LOG_WARN("Stop 校准时 obs_remove_raw_video_callback 抛出异常（已忽略）");
		}
		callbackAttached_ = false;
	}

	running_ = false;
	GRT_LOG_INFO("视频时间戳校准已停止（共采集 %zu 个样本）", estimator_.SampleCount());
}

void VideoTimeCalibrator::Suspend()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (!callbackAttached_) {
		return;
	}
	try {
		obs_remove_raw_video_callback(&VideoTimeCalibrator::RawVideoCallback, this);
	} catch (...) {
		GRT_LOG_WARN("Suspend 校准时 obs_remove_raw_video_callback 抛出异常（已忽略）");
	}
	callbackAttached_ = false;
	GRT_LOG_DEBUG("视频时间戳校准已临时挂起（为视频重置让路）");
}

bool VideoTimeCalibrator::Resume()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (!running_ || callbackAttached_) {
		return callbackAttached_;
	}
	video_scale_info conversion{};
	conversion.format = VIDEO_FORMAT_NV12;
	conversion.width = 0;
	conversion.height = 0;
	conversion.range = VIDEO_RANGE_DEFAULT;
	conversion.colorspace = VIDEO_CS_DEFAULT;
	try {
		obs_add_raw_video_callback(&conversion, &VideoTimeCalibrator::RawVideoCallback, this);
	} catch (...) {
		GRT_LOG_ERROR("Resume 校准异常：obs_add_raw_video_callback 抛出异常");
		return false;
	}
	callbackAttached_ = true;
	GRT_LOG_DEBUG("视频时间戳校准已恢复");
	return true;
}

bool VideoTimeCalibrator::IsRunning() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return running_;
}

bool VideoTimeCalibrator::Ready() const
{
	return estimator_.Ready();
}

std::size_t VideoTimeCalibrator::SampleCount() const
{
	return estimator_.SampleCount();
}

double VideoTimeCalibrator::VideoTimeToQpc(double videoTimeSec) const
{
	return estimator_.ToQpc(videoTimeSec);
}

double VideoTimeCalibrator::QpcToVideoTime(double qpcSec) const
{
	return estimator_.ToVideo(qpcSec);
}

void VideoTimeCalibrator::MarkRecordingStart(double qpcSec)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	startQpc_ = qpcSec;
	startMarked_ = true;
	// 新一段录制开始，清掉上一次的结束标记
	endMarked_ = false;
	endQpc_ = 0.0;
}

void VideoTimeCalibrator::MarkRecordingEnd(double qpcSec)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	endQpc_ = qpcSec;
	endMarked_ = true;
}

double VideoTimeCalibrator::RecordingVideoZero() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (!startMarked_) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	// ToVideo 内部在未就绪（样本<2）时返回 NaN，调用方据此处理
	return estimator_.ToVideo(startQpc_);
}

double VideoTimeCalibrator::ToSessionTime(double qpcSec) const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (!startMarked_) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	return qpcSec - startQpc_;
}

double VideoTimeCalibrator::LastDurationSeconds() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (!startMarked_ || !endMarked_) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	return endQpc_ - startQpc_;
}

void VideoTimeCalibrator::RawVideoCallback(void *param, video_data *frame)
{
	if (param == nullptr || frame == nullptr) {
		return;
	}
	auto *self = static_cast<VideoTimeCalibrator *>(param);
	try {
		self->OnRawVideo(*frame);
	} catch (...) {
		// 回调在 libobs 视频线程，绝不让异常穿越 C 边界
		GRT_LOG_WARN("raw-video 回调发生异常（已吞掉以保护视频线程）");
	}
}

void VideoTimeCalibrator::OnRawVideo(const video_data &frame)
{
	if (frame.timestamp == 0) {
		return; // 防御：无效时间戳不参与拟合
	}

	const double videoSec = static_cast<double>(frame.timestamp) / kVideoTimestampNsPerSec;
	const double qpcSec = Clock::NowSeconds();
	estimator_.AddSample(videoSec, qpcSec);

	const std::size_t n = estimator_.SampleCount();
	if (n == 2) {
		GRT_LOG_INFO("视频时间戳校准就绪（样本数达到 2，offset=%.6f 秒）", estimator_.Offset());
	} else if (n >= kDiagInterval && n % kDiagInterval == 0 &&
	           n > lastDiagCount_.load()) {
		lastDiagCount_.store(n);
		const double offset = estimator_.Offset();
		const double rate = estimator_.RateRatio();
		if (std::isnan(rate) || std::fabs(rate - 1.0) < kRateWarnEpsilon) {
			GRT_LOG_INFO("校准诊断：样本=%zu offset=%.6f 秒 速率比=%.7f", n, offset, rate);
		} else {
			// 仅诊断：QPC 与视频时钟速率理应一致，显著偏离值得警惕但不做校正
			GRT_LOG_WARN("校准诊断：视频/QPC 速率偏差 %.7f（样本=%zu offset=%.6f 秒），不做斜率校正",
				     rate - 1.0, n, offset);
		}
	}
}

} // namespace grt
