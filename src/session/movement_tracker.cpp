/**
 * @file movement_tracker.cpp
 * @brief 移动事件跟踪器实现（纯逻辑，见 movement_tracker.h 的规则说明）。
 */

#include "session/movement_tracker.h"

#include <cmath>

#include "input/key_names.h"

namespace grt {

MovementTracker::MovementTracker(std::string forward, std::string back,
                                 std::string left, std::string right)
    : forward_(std::move(forward))
    , back_(std::move(back))
    , left_(std::move(left))
    , right_(std::move(right))
{
}

// static
std::string MovementTracker::ComposeDirection(bool f, bool b, bool l, bool r)
{
	// 纵码在前：纵向 W/B 优先，其次横向 L/R。
	// 正常调用不会出现同轴两个布尔同时为 true（按下栈已保证互斥），
	// 兜底策略为纵向取 W、横向取 L，保证纯函数输出确定。
	std::string vertical;
	if (f) {
		vertical = "W";
	} else if (b) {
		vertical = "B";
	}

	std::string horizontal;
	if (l) {
		horizontal = "L";
	} else if (r) {
		horizontal = "R";
	}

	return vertical + horizontal;
}

void MovementTracker::OnKey(const std::string& name, bool down, double sessionTime)
{
	if (finished_) {
		return;
	}
	// 录制起点之前的边沿不追溯：t<0 一律忽略
	if (sessionTime < 0.0) {
		return;
	}
	if (!IsMovementKey(name)) {
		return;
	}

	const std::string oldDirection = CurrentDirection();

	if (down) {
		// 键盘自动重复会产生额外 down 边沿：已在栈中则忽略，
		// 不改变"最近按下"顺序。
		bool alreadyPressed = false;
		for (const auto& pressed : pressedStack_) {
			if (pressed == name) {
				alreadyPressed = true;
				break;
			}
		}
		if (!alreadyPressed) {
			pressedStack_.push_back(name);
		}
	} else {
		// 抬起：从按下栈移除；录制前按下（down 边沿 t<0 被丢弃）
		// 导致栈中找不到时按未按下处理。
		for (auto it = pressedStack_.begin(); it != pressedStack_.end(); ++it) {
			if (*it == name) {
				pressedStack_.erase(it);
				break;
			}
		}
	}

	const std::string newDirection = CurrentDirection();
	HandleDirectionChange(oldDirection, newDirection, sessionTime);
}

void MovementTracker::ProcessEvent(const InputEvent& event, double sessionTime)
{
	if (event.type != InputType::Key) {
		return;
	}
	OnKey(KeyVkeyToName(event.vkey), event.key_down, sessionTime);
}

void MovementTracker::Finish(double endTime)
{
	if (finished_) {
		return;
	}
	finished_ = true;
	CloseOpenInterval(endTime);
}

void MovementTracker::Reset()
{
	pressedStack_.clear();
	events_.clear();
	hasOpen_ = false;
	openDirection_.clear();
	openStart_ = 0.0;
	finished_ = false;
}

std::string MovementTracker::CurrentDirection() const
{
	bool f = false;
	bool b = false;
	bool l = false;
	bool r = false;

	// 从栈顶（最近按下）向栈底扫描，每个轴只认第一个遇到的键，
	// 即"最近按下优先"。
	for (auto it = pressedStack_.rbegin(); it != pressedStack_.rend(); ++it) {
		const std::string& name = *it;
		if (!f && !b) {
			if (name == forward_) {
				f = true;
			} else if (name == back_) {
				b = true;
			}
		}
		if (!l && !r) {
			if (name == left_) {
				l = true;
			} else if (name == right_) {
				r = true;
			}
		}
	}

	return ComposeDirection(f, b, l, r);
}

void MovementTracker::HandleDirectionChange(const std::string& oldDir,
                                            const std::string& newDir, double time)
{
	if (oldDir == newDir) {
		return;
	}

	// 旧方向存在：在切换时刻闭合当前区间（end == 新区间 start，首尾相接）
	if (!oldDir.empty()) {
		CloseOpenInterval(time);
	}

	// 新方向存在：以切换时刻开启新区间。是否与上一条闭合区间同向合并，
	// 延迟到本区间闭合时统一判断（见 CloseOpenInterval），这样零长区间
	// 被丢弃时也不会破坏上一条区间。
	if (!newDir.empty()) {
		hasOpen_ = true;
		openDirection_ = newDir;
		openStart_ = time;
	}
}

void MovementTracker::CloseOpenInterval(double endTime)
{
	if (!hasOpen_) {
		return;
	}
	hasOpen_ = false;

	const double length = endTime - openStart_;
	if (!(length >= kZeroEpsilon)) {
		// 零长（或异常的负时长/NaN）区间直接丢弃
		openDirection_.clear();
		return;
	}

	if (!events_.empty()) {
		MovementEvent& last = events_.back();
		// 同方向且首尾相接（end_i == start_(i+1)）：合并为一条。
		// 典型场景：中间被一段零长/空档隔开的同方向区间。
		if (last.direction == openDirection_ &&
		    std::fabs(last.end_time - openStart_) < kZeroEpsilon) {
			last.end_time = endTime;
			openDirection_.clear();
			return;
		}
	}

	events_.push_back(MovementEvent{openDirection_, openStart_, endTime});
	openDirection_.clear();
}

bool MovementTracker::IsMovementKey(const std::string& name) const
{
	return name == forward_ || name == back_ ||
	       name == left_ || name == right_;
}

} // namespace grt
