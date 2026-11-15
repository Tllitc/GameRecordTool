/**
 * @file offset_estimator.cpp
 * @brief OffsetEstimator 纯数学层实现（不依赖 OBS / Windows 头文件），
 *        可在无 OBS 环境直接编译，供单元测试复用。
 */

#include "video/video_time_calibrator.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace grt {

void OffsetEstimator::AddSample(double videoSec, double qpcSec)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ring_[head_] = TimeSample{videoSec, qpcSec};
	head_ = (head_ + 1) % kCalibratorRingSize;
	if (count_ < kCalibratorRingSize) {
		++count_;
	}
}

void OffsetEstimator::Reset()
{
	std::lock_guard<std::mutex> lock(mutex_);
	head_ = 0;
	count_ = 0;
}

bool OffsetEstimator::Ready() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return count_ >= 2;
}

std::size_t OffsetEstimator::SampleCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return count_;
}

std::array<TimeSample, kCalibratorRingSize>
OffsetEstimator::SnapshotOrdered(std::size_t &outCount) const
{
	// 调用方已持锁（本方法仅在 Offset()/RateRatio() 持锁区间内调用）
	std::array<TimeSample, kCalibratorRingSize> out{};
	outCount = count_;
	if (count_ == 0) {
		return out;
	}

	// 环形缓冲中最旧样本的位置：未写满时从 0 开始，写满后即 head_。
	const std::size_t start = (count_ < kCalibratorRingSize) ? 0 : head_;
	for (std::size_t i = 0; i < count_; ++i) {
		out[i] = ring_[(start + i) % kCalibratorRingSize];
	}
	return out;
}

double OffsetEstimator::MedianOffset(const TimeSample *samples, std::size_t count)
{
	if (count < 2 || samples == nullptr) {
		return std::numeric_limits<double>::quiet_NaN();
	}

	std::vector<double> diffs;
	diffs.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		diffs.push_back(samples[i].qpcSec - samples[i].videoSec);
	}

	const std::size_t mid = diffs.size() / 2;
	std::nth_element(diffs.begin(), diffs.begin() + static_cast<std::ptrdiff_t>(mid), diffs.end());
	const double upper = diffs[mid];
	if ((diffs.size() % 2) != 0) {
		return upper; // 奇数个：中位数即中间元素
	}

	// 偶数个：取中间两个的平均，需先找到下半部分的最大值
	const double lower = *std::max_element(diffs.begin(), diffs.begin() + static_cast<std::ptrdiff_t>(mid));
	return (lower + upper) * 0.5;
}

double OffsetEstimator::ComputeRateRatio(const TimeSample *samples, std::size_t count)
{
	if (count < 2 || samples == nullptr) {
		return std::numeric_limits<double>::quiet_NaN();
	}

	// 最小二乘斜率：qpc = slope * video + b，slope = cov / var
	double sumV = 0.0, sumQ = 0.0;
	for (std::size_t i = 0; i < count; ++i) {
		sumV += samples[i].videoSec;
		sumQ += samples[i].qpcSec;
	}
	const double meanV = sumV / static_cast<double>(count);
	const double meanQ = sumQ / static_cast<double>(count);

	double cov = 0.0, varV = 0.0;
	for (std::size_t i = 0; i < count; ++i) {
		const double dv = samples[i].videoSec - meanV;
		cov += dv * (samples[i].qpcSec - meanQ);
		varV += dv * dv;
	}
	if (varV <= 0.0) {
		return std::numeric_limits<double>::quiet_NaN(); // 视频方向无方差
	}
	return cov / varV;
}

double OffsetEstimator::Offset() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::size_t count = 0;
	auto snap = SnapshotOrdered(count);
	return MedianOffset(snap.data(), count);
}

double OffsetEstimator::ToQpc(double videoSec) const
{
	const double offset = Offset();
	if (std::isnan(offset)) {
		return offset;
	}
	return videoSec + offset;
}

double OffsetEstimator::ToVideo(double qpcSec) const
{
	const double offset = Offset();
	if (std::isnan(offset)) {
		return offset;
	}
	return qpcSec - offset;
}

double OffsetEstimator::RateRatio() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::size_t count = 0;
	auto snap = SnapshotOrdered(count);
	return ComputeRateRatio(snap.data(), count);
}

} // namespace grt
