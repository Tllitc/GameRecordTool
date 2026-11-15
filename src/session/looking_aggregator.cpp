/**
 * @file looking_aggregator.cpp
 * @brief 视角事件时间桶聚合器实现（纯逻辑，见 looking_aggregator.h 的规则说明）。
 */

#include "session/looking_aggregator.h"

#include <algorithm>
#include <cmath>

namespace grt {

LookingAggregator::LookingAggregator(double bucketSeconds)
{
	if (!(bucketSeconds > 0.0)) {
		dt_ = kDefaultBucketSeconds;
	} else {
		dt_ = std::min(bucketSeconds, kMaxBucketSeconds);
	}
}

// static
std::string LookingAggregator::ComposeLabel(std::int64_t sumDx,
                                            std::int64_t sumDy)
{
	// 垂直分量在前（屏幕坐标 dy 向下为正，故 dy<0 为 U，dy>0 为 D），
	// 水平分量在后（dx>0 为 R，dx<0 为 L）。阈值严格 abs>0。
	std::string label;
	if (sumDy < 0) {
		label += "U";
	} else if (sumDy > 0) {
		label += "D";
	}
	if (sumDx < 0) {
		label += "L";
	} else if (sumDx > 0) {
		label += "R";
	}
	if (label.empty()) {
		label = "Neutral";
	}
	return label;
}

void LookingAggregator::AddMove(double sessionTime, std::int64_t dx,
                                std::int64_t dy)
{
	if (finished_) {
		return;
	}
	if (sessionTime < 0.0) {
		// 先于录制起点的移动整条丢弃并计数
		++negativeDropped_;
		return;
	}

	const std::int64_t bucketIndex = BucketIndexOf(sessionTime);
	EnsureBucket(bucketIndex);

	// 整数累加，转 double 推迟到生成事件时
	sumDx_ += dx;
	sumDy_ += dy;
	openHasData_ = true;

	if (sessionTime > maxTimeSeen_) {
		maxTimeSeen_ = sessionTime;
	}
}

void LookingAggregator::ProcessEvent(const InputEvent& event,
                                     double sessionTime)
{
	if (event.type != InputType::MouseMove) {
		return;
	}
	AddMove(sessionTime, static_cast<std::int64_t>(event.dx),
	        static_cast<std::int64_t>(event.dy));
}

std::size_t LookingAggregator::FlushReady()
{
	std::size_t flushed = 0;
	if (hasOpen_) {
		// 当前桶右边界已经越过（存在事件落入更晚时间）才闭合，
		// 避免同一桶的后续事件被误分到已输出条目。
		const double openEnd = static_cast<double>(openIndex_ + 1) * dt_;
		if (maxTimeSeen_ >= openEnd) {
			FlushOpenBucket(openIndex_, openEnd);
			++flushed;
		}
	}
	return flushed;
}

void LookingAggregator::Finish(double tEnd)
{
	if (finished_) {
		return;
	}
	finished_ = true;

	if (!hasOpen_) {
		return;
	}

	const double nominalEnd = static_cast<double>(openIndex_ + 1) * dt_;
	// 最后一个有数据桶的 end=min((k+1)*dt, tEnd)
	double end = std::min(nominalEnd, tEnd);
	const double start = static_cast<double>(openIndex_) * dt_;
	if (!(end >= start)) {
		// tEnd 异常早于桶起点时，直接丢弃该桶，不产生零长/负长区间
		hasOpen_ = false;
		openHasData_ = false;
		sumDx_ = 0;
		sumDy_ = 0;
		return;
	}
	FlushOpenBucket(openIndex_, end);
}

void LookingAggregator::Reset()
{
	events_.clear();
	hasOpen_ = false;
	openIndex_ = 0;
	sumDx_ = 0;
	sumDy_ = 0;
	openHasData_ = false;
	negativeDropped_ = 0;
	maxTimeSeen_ = 0.0;
	finished_ = false;
}

void LookingAggregator::EnsureBucket(std::int64_t bucketIndex)
{
	if (hasOpen_ && openIndex_ == bucketIndex) {
		return;
	}

	if (hasOpen_) {
		// 进入更晚的桶意味着旧桶右边界必然已越过（输入按时间顺序处理），
		// 直接以其标称右边界闭合。
		const double openEnd = static_cast<double>(openIndex_ + 1) * dt_;
		FlushOpenBucket(openIndex_, openEnd);
	}

	hasOpen_ = true;
	openIndex_ = bucketIndex;
	sumDx_ = 0;
	sumDy_ = 0;
	openHasData_ = false;
}

void LookingAggregator::FlushOpenBucket(std::int64_t bucketIndex,
                                        double endTime)
{
	const bool hadData = openHasData_;
	const std::int64_t dx = sumDx_;
	const std::int64_t dy = sumDy_;

	hasOpen_ = false;
	openHasData_ = false;
	sumDx_ = 0;
	sumDy_ = 0;

	if (!hadData) {
		// 不输出空桶
		return;
	}

	LookingEvent event;
	event.start_time = static_cast<double>(bucketIndex) * dt_;
	event.end_time = endTime;
	event.dx = static_cast<double>(dx);
	event.dy = static_cast<double>(dy);
	event.direction = ComposeLabel(dx, dy);
	events_.push_back(std::move(event));
}

std::int64_t LookingAggregator::BucketIndexOf(double sessionTime) const
{
	return static_cast<std::int64_t>(std::floor(sessionTime / dt_));
}

} // namespace grt
