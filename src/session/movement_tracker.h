/**
 * @file movement_tracker.h
 * @brief 移动事件（movement_events）生成器：把移动键按下/抬起边沿转为方向区间。
 *
 * 纯逻辑模块：只依赖标准库与 input/input_event.h（纯数据头）、
 * input/key_names.h（纯声明），不包含 <windows.h>/OBS/Qt 头，
 * 可在无 OBS 环境独立编译，供 Task 13 单元测试。
 *
 * 规则（与任务约定一一对应）：
 *   1. 纵码在前：组合方向为【纵向字母】+【横向字母】，取值仅
 *      W/B/L/R/WL/WR/BL/BR，例如 W+A（左）-> "WL"，S（后）+D（右）-> "BR"；
 *   2. 最近按下优先（later-press wins）：同一轴上前后/左右两个键同时按住时，
 *      以最后按下的键为准；先按下的键抬起后，当前方向自然回落到另一个键；
 *   3. 区间首尾相接、不重叠：方向在 t 时刻切换时，旧区间 end=t、新区间 start=t；
 *   4. 同方向相邻段合并：若新区间与上一条已闭合区间首尾相接且方向相同，
 *      直接延续为同一条；
 *   5. 不产生零长区间：时长 < kZeroEpsilon(1e-6) 的区间直接丢弃；
 *   6. 会话时间 t<0 的边沿不参与跟踪（录制起点之前的按下状态不追溯）。
 *
 * 非线程安全：仅由 SessionManager 的处理线程调用。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "input/input_event.h"
#include "session/session_types.h"

namespace grt {

class MovementTracker {
public:
	/// 小于此时长（秒）的区间视为零长区间，不输出
	static constexpr double kZeroEpsilon = 1e-6;

	/**
	 * @brief 构造移动跟踪器。
	 * @param forward 向前键规范名（配置 W/A/S/D 序列中的 [0]，通常为 "W"）
	 * @param back    向后键规范名（配置序列中的 [2]，通常为 "S"）
	 * @param left    向左键规范名（配置序列中的 [1]，通常为 "A"）
	 * @param right   向右键规范名（配置序列中的 [3]，通常为 "D"）
	 *
	 * 注意配置数组顺序为 W,A,S,D，本构造参数顺序为 forward,back,left,right，
	 * 调用方需做 [0],[2],[1],[3] 重排。
	 */
	MovementTracker(std::string forward, std::string back,
	                std::string left, std::string right);

	/**
	 * @brief 组合方向标签（纯函数）。
	 * @param f 向前键当前生效
	 * @param b 向后键当前生效
	 * @param l 向左键当前生效
	 * @param r 向右键当前生效
	 * @return "W"/"B"/"L"/"R"/"WL"/"WR"/"BL"/"BR"；四者皆 false 返回空串。
	 *
	 * 纵码在前：先纵向字母后横向字母。同一轴两个布尔同时为 true 时
	 * 纵向取 W、横向取 L（实际跟踪器按"最近按下优先"保证不会同时为 true，
	 * 这里仅为纯函数给出确定性兜底）。
	 */
	static std::string ComposeDirection(bool f, bool b, bool l, bool r);

	/**
	 * @brief 处理一个按键边沿（按规范名）。
	 * @param name        按键规范名（key_names.h 定义的空间）。
	 * @param down        true=按下边沿，false=抬起边沿。
	 * @param sessionTime 会话时间（秒，相对 qpc0）；t<0 的边沿忽略。
	 */
	void OnKey(const std::string& name, bool down, double sessionTime);

	/**
	 * @brief 处理一条原始输入事件（仅消费 Key 类型，其余类型忽略）。
	 *
	 * 内部经 KeyVkeyToName 转换后调用 OnKey。
	 */
	void ProcessEvent(const InputEvent& event, double sessionTime);

	/**
	 * @brief 结束跟踪并闭合当前尚未闭合的区间。
	 * @param endTime 会话结束时间（秒）。当前区间以 endTime 闭合；
	 *                零长区间同样丢弃。调用后通过 Events() 取结果。
	 *
	 * 不与上一条区间做合并：当前区间与上一条之间若方向相同本就会在
	 * 切换时完成合并，Finish 只负责封口。可重复调用（第二次为空操作）。
	 */
	void Finish(double endTime);

	/// 当前已生成的区间（Finish 前后都可读取；Finish 后为最终结果）
	const std::vector<MovementEvent>& Events() const { return events_; }

	/// 清空全部状态（按下栈与已生成区间），供同实例复用到下一次录制
	void Reset();

private:
	/// 依据按下栈当前内容重新计算方向；返回空串表示当前无移动
	std::string CurrentDirection() const;
	/// 在 time 时刻把方向从 oldDir 切换到 newDir（可能相同，可能为空）
	void HandleDirectionChange(const std::string& oldDir,
	                           const std::string& newDir, double time);
	/// 闭合当前开放区间；零长丢弃；同向且首尾相接则与上一条合并
	void CloseOpenInterval(double endTime);
	/// 判断 name 是否为四个移动键之一
	bool IsMovementKey(const std::string& name) const;

	std::string forward_;
	std::string back_;
	std::string left_;
	std::string right_;

	/// 当前按住的移动键按下顺序栈（仅保存移动键；back 为最近按下）
	std::vector<std::string> pressedStack_;

	/// 当前开放区间（startTime_/openDirection_ 有效）；hasOpen_ 为 true 时存在
	bool hasOpen_ = false;
	std::string openDirection_;
	double openStart_ = 0.0;

	std::vector<MovementEvent> events_;
	bool finished_ = false;
};

} // namespace grt
