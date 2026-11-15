/**
 * @file clock.h
 * @brief 高精度单调时钟与 UTC 墙钟时间工具。
 *
 * 仅依赖 <windows.h> 与 C++ 标准库，不引入任何 OBS 头文件，
 * 以便后续任务中的纯逻辑单元测试可直接编译本模块。
 */

#pragma once

#include <cstdint>
#include <string>

namespace grt {

/**
 * @brief 高精度时钟封装（基于 QueryPerformanceCounter）。
 *
 * 时间原点为本进程内首次调用 NowTicks()/NowSeconds() 的时刻，
 * 因此返回值仅适合计算时间差（如帧时间戳、时长统计），不具备跨进程含义。
 */
class Clock {
public:
	/**
	 * @brief 获取当前单调时间（秒，double）。
	 *
	 * 以本进程首次调用时刻为原点，单调递增，不会受系统时间调整影响。
	 * 热点路径函数，实现保持轻量。
	 */
	static double NowSeconds();

	/**
	 * @brief 获取当前单调时钟的原始计数值（QPC ticks）。
	 *
	 * 首次调用时会查询并缓存 QueryPerformanceFrequency；
	 * 若 QPC 不可用则回退到 GetSystemTimePreciseAsFileTime，
	 * 此时 ticks 以 100ns 为单位（频率 10MHz）。
	 */
	static int64_t NowTicks();

	/**
	 * @brief 将原始计数值换算为秒（相对时间原点）。
	 * @param ticks NowTicks() 返回的计数值。
	 */
	static double TicksToSeconds(int64_t ticks);

	/**
	 * @brief 获取当前 UTC 墙钟时间。
	 * @return 格式固定为 "YYYY-MM-DDTHH:MM:SSZ" 的字符串
	 *         （供 game_metadata.start_time 使用）。
	 */
	static std::string WallTimeUtcIso8601();

private:
	Clock() = delete;
};

} // namespace grt
