/**
 * @file looking_aggregator.h
 * @brief 视角事件（looking_events）时间桶聚合器：把相对鼠标移动聚合成定长桶。
 *
 * 纯逻辑模块：只依赖标准库与 input/input_event.h（纯数据头）、
 * session/session_types.h（纯数据头），不包含 <windows.h>/OBS/Qt，
 * 供 Task 13 单元测试。
 *
 * 聚合规则（与任务约定一一对应）：
 *   1. 桶长默认 1/60 秒，最大不超过 1/30 秒（构造时夹紧）；桶边界对齐
 *      会话时间 t=0，第 k 个桶为 [k*dt, (k+1)*dt)；
 *   2. 整条移动事件计入其【起点所在桶】（不按落点切分），桶内 dx/dy 用
 *      整数累加，生成事件时再转 double；
 *   3. 不补空桶：没有移动落入的桶不产生输出，因此相邻输出桶在时间轴上
 *      可以不连续；
 *   4. 会话时间 t<0 的移动整条丢弃并计入 NegativeDropped（不产生桶）；
 *   5. 粗标签以桶内合量判定，阈值 abs>0：垂直分量在前（U/D），
 *      水平分量在后（L/R），两轴均为 0 时为 "Neutral"，如右上 -> "UR"；
 *   6. Finish(tEnd) 只 flush 当前最后一个有数据的桶，其 end 取
 *      min((k+1)*dt, tEnd)，绝不凭空新增零增量桶。
 *
 * 非线程安全：仅由 SessionManager 的处理线程调用。
 */

#pragma once

#include <cstdint>
#include <vector>

#include "input/input_event.h"
#include "session/session_types.h"

namespace grt {

class LookingAggregator {
public:
	/// 默认桶长（秒）：60Hz
	static constexpr double kDefaultBucketSeconds = 1.0 / 60.0;
	/// 桶长上限（秒）：30Hz
	static constexpr double kMaxBucketSeconds = 1.0 / 30.0;

	/**
	 * @brief 构造聚合器。
	 * @param bucketSeconds 桶长（秒）；非正值或超过上限时夹紧到默认/上限。
	 */
	explicit LookingAggregator(double bucketSeconds = kDefaultBucketSeconds);

	/**
	 * @brief 加入一条已聚合好的整数位移（会话时间）。
	 * @param sessionTime 事件会话时间（秒，相对 qpc0）；t<0 整条丢弃。
	 * @param dx 桶内累加用的 X 相对位移（像素，屏幕坐标向右为正）。
	 * @param dy 桶内累加用的 Y 相对位移（像素，屏幕坐标向下为正）。
	 */
	void AddMove(double sessionTime, std::int64_t dx, std::int64_t dy);

	/**
	 * @brief 处理一条原始输入事件（只消费 MouseMove 类型）。
	 *
	 * 注意 RawInputService 对每条鼠标消息固定拆出一条 MOVE（dx/dy 可能为 0），
	 * 0 增量 MOVE 也按整事件计入其起点桶（桶因此可能产出 Neutral 标签）。
	 */
	void ProcessEvent(const InputEvent& event, double sessionTime);

	/**
	 * @brief flush 所有桶边界已完整（end <= 当前已见最大时间）的桶。
	 *
	 * 运行期由处理线程周期性调用以尽早产出事件；当前正在填充的桶
	 * （其右边界尚未越过）保留不输出。返回本次新 flush 的桶数量。
	 *
	 * 典型测试中可直接调用以观察中途结果。
	 */
	std::size_t FlushReady();

	/**
	 * @brief 结束聚合并 flush 最后一个有数据的桶。
	 * @param tEnd 会话结束时间（秒）；最后桶 end=min((k+1)*dt, tEnd)。
	 *
	 * 可重复调用（第二次为空操作）。不会为没有数据的桶生成事件。
	 */
	void Finish(double tEnd);

	/// 当前已 flush 的桶（Finish 后为最终结果）
	const std::vector<LookingEvent>& Events() const { return events_; }

	/// 因会话时间 <0 被丢弃的移动事件数
	std::uint64_t NegativeDropped() const { return negativeDropped_; }

	/// 实际生效的桶长（秒）
	double BucketSeconds() const { return dt_; }

	/// 清空全部状态，供同实例复用到下一次录制
	void Reset();

	/**
	 * @brief 依据桶内合量生成粗标签（纯函数）。
	 * @return "U"/"D"/"L"/"R"/"UL"/"UR"/"DL"/"DR"/"Neutral"，垂直分量在前。
	 */
	static std::string ComposeLabel(std::int64_t sumDx, std::int64_t sumDy);

private:
	/// 确保 open 桶对应 bucketIndex（不同则先 flush 旧桶再开新桶）
	void EnsureBucket(std::int64_t bucketIndex);
	/// 把当前 open 桶（有数据时）输出到 events_
	void FlushOpenBucket(std::int64_t bucketIndex, double endTime);
	/// 计算桶索引 k = floor(t / dt)
	std::int64_t BucketIndexOf(double sessionTime) const;

	double dt_;

	bool hasOpen_ = false;          ///< 是否存在正在填充的桶
	std::int64_t openIndex_ = 0;    ///< 当前桶索引 k
	std::int64_t sumDx_ = 0;        ///< 当前桶 X 合量（整数累加）
	std::int64_t sumDy_ = 0;
	bool openHasData_ = false;      ///< 当前桶是否收到过移动（含 0 增量）

	std::vector<LookingEvent> events_;
	std::uint64_t negativeDropped_ = 0;
	double maxTimeSeen_ = 0.0;      ///< 已见最大会话时间，FlushReady 用
	bool finished_ = false;
};

} // namespace grt
