/**
 * @file clock.cpp
 * @brief 高精度单调时钟与 UTC 墙钟时间工具实现。
 */

// 减少 <windows.h> 引入的无关声明，避免 min/max 宏污染
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "clock.h"

#include <cstdio>

#include <windows.h>

#include "log.h"

namespace grt {

namespace {

/**
 * @brief 单调时钟的内部状态。
 *
 * 采用函数内静态变量 + 显式初始化标记：C++11 起静态局部变量初始化本身
 * 线程安全，后续读取无需加锁；频率与时间原点在进程生命周期内不变。
 */
struct MonotonicClockState {
	LARGE_INTEGER frequency{}; ///< QPC 频率（Hz）；回退模式下为 10000000（100ns）
	LARGE_INTEGER origin{};    ///< 时间原点（首次取值时的计数值）
	bool useQpc = true;        ///< 是否使用 QPC；false 表示回退到精确系统时间
	bool initialized = false;  ///< 初始化是否已完成
};

MonotonicClockState& ClockState()
{
	static MonotonicClockState state;
	return state;
}

/// 首次调用时探测 QPC 可用性并缓存频率与时间原点；QPC 失败时仅告警一次。
void EnsureClockInitialized()
{
	MonotonicClockState& state = ClockState();
	if (state.initialized) {
		return;
	}

	state.initialized = true;

	LARGE_INTEGER frequency{};
	LARGE_INTEGER counter{};
	if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0 &&
	    QueryPerformanceCounter(&counter)) {
		state.useQpc = true;
		state.frequency = frequency;
		state.origin = counter;
	} else {
		// 理论上现代 Windows 不会走到这里；回退使用高精度系统时间，
		// 其 FILETIME 以 100ns 为单位递增（频率 10MHz）
		GRT_LOG_WARN("QueryPerformanceCounter 不可用，回退到 GetSystemTimePreciseAsFileTime");
		state.useQpc = false;
		state.frequency.QuadPart = 10000000LL;
		FILETIME ft{};
		GetSystemTimePreciseAsFileTime(&ft);
		state.origin.QuadPart = (static_cast<LONGLONG>(ft.dwHighDateTime) << 32) |
		                        static_cast<LONGLONG>(ft.dwLowDateTime);
	}
}

/// 读取一次单调时钟原始值（QPC 或回退路径）。
int64_t ReadRawTicks()
{
	MonotonicClockState& state = ClockState();
	if (state.useQpc) {
		LARGE_INTEGER counter{};
		QueryPerformanceCounter(&counter);
		return counter.QuadPart;
	}

	FILETIME ft{};
	GetSystemTimePreciseAsFileTime(&ft);
	return (static_cast<int64_t>(ft.dwHighDateTime) << 32) |
	       static_cast<int64_t>(ft.dwLowDateTime);
}

} // 匿名命名空间

int64_t Clock::NowTicks()
{
	EnsureClockInitialized();
	return ReadRawTicks() - ClockState().origin.QuadPart;
}

double Clock::TicksToSeconds(int64_t ticks)
{
	EnsureClockInitialized();
	return static_cast<double>(ticks) / static_cast<double>(ClockState().frequency.QuadPart);
}

double Clock::NowSeconds()
{
	return TicksToSeconds(NowTicks());
}

std::string Clock::WallTimeUtcIso8601()
{
	FILETIME ftUtc{};
	GetSystemTimePreciseAsFileTime(&ftUtc);

	// FILETIME -> SYSTEMTIME（UTC），便于格式化各时间字段
	SYSTEMTIME stUtc{};
	FileTimeToSystemTime(&ftUtc, &stUtc);

	char buffer[32];
	// 固定格式 YYYY-MM-DDTHH:MM:SSZ（丢弃毫秒，满足 game_metadata 需求）
	std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ", stUtc.wYear,
	              stUtc.wMonth, stUtc.wDay, stUtc.wHour, stUtc.wMinute, stUtc.wSecond);
	return std::string(buffer);
}

} // namespace grt
