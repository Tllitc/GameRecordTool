/**
 * @file mouse_accel_service.cpp
 * @brief 鼠标加速（增强指针精确度）预检服务实现。
 *
 * 重要约定（与头文件一致）：
 *   【关闭后不自动恢复，用户需自行在 Windows 设置中开启】
 *   本文件不保存原值、不提供恢复函数、不在任何退出路径还原系统设置。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "input/mouse_accel_service.h"

#include <array>

#include <windows.h>

#include "core/log.h"

namespace grt {

bool MouseAccelService::IsEnhancePointerPrecisionOn()
{
	// SPI_GETMOUSE 返回三个 INT：[阈值1, 阈值2, 加速开关]
	std::array<int, 3> mouseParams{0, 0, 0};
	if (!SystemParametersInfoW(SPI_GETMOUSE, 0, mouseParams.data(), 0)) {
		// 查询失败：保守处理，当作加速开启，阻止开始录制
		GRT_LOG_ERROR("SystemParametersInfoW(SPI_GETMOUSE) 失败：%s",
		              Win32ErrorMessage().c_str());
		return true;
	}
	return mouseParams[2] != 0;
}

bool MouseAccelService::DisableEnhancePointerPrecision()
{
	// 三个值全部归零：彻底关闭鼠标加速曲线（含两个阈值与加速开关）。
	// 【关闭后不自动恢复，用户需自行在 Windows 设置中开启】
	std::array<int, 3> disabled{0, 0, 0};
	if (!SystemParametersInfoW(SPI_SETMOUSE, 0, disabled.data(),
	                           SPIF_SENDCHANGE)) {
		GRT_LOG_ERROR("SystemParametersInfoW(SPI_SETMOUSE) 关闭鼠标加速失败：%s",
		              Win32ErrorMessage().c_str());
		return false;
	}

	GRT_LOG_INFO("已关闭系统鼠标加速（增强指针精确度）；"
	             "关闭后不自动恢复，用户需自行在 Windows 设置中开启");
	return true;
}

bool MouseAccelService::CanStartRecording()
{
	return !IsEnhancePointerPrecisionOn();
}

bool MouseAccelService::GetThresholds(int& threshold1, int& threshold2)
{
	std::array<int, 3> mouseParams{0, 0, 0};
	if (!SystemParametersInfoW(SPI_GETMOUSE, 0, mouseParams.data(), 0)) {
		GRT_LOG_ERROR("SystemParametersInfoW(SPI_GETMOUSE) 查询阈值失败：%s",
		              Win32ErrorMessage().c_str());
		return false;
	}
	threshold1 = mouseParams[0];
	threshold2 = mouseParams[1];
	return true;
}

} // namespace grt
