/**
 * @file mouse_accel_service.h
 * @brief 鼠标加速（"增强指针精确度"/Enhance Pointer Precision）预检服务。
 *
 * 录制前必须确认系统鼠标加速处于关闭状态，否则游戏内鼠标位移与 Raw Input
 * 记录的相对量不一致，回放/分析结果会失真。本服务提供状态查询与"彻底关闭"
 * 能力，供 GUI 与开始录制热键做准入（CanStartRecording）。
 *
 * ============================================================
 * 重要约定（务必遵守）：
 *   【关闭后不自动恢复，用户需自行在 Windows 设置中开启】
 *   - 本服务【不保存】用户原来的鼠标参数；
 *   - 本服务【不提供】任何 Restore / 恢复接口；
 *   - 不会在析构、停止录制或插件卸载路径中做任何还原。
 * - 关闭操作会把 SPI_GETMOUSE 的三个值（两个阈值 + 加速开关）
 *   全部写 0，用户如需恢复请自行前往：
 *     设置 -> 蓝牙和其他设备 -> 鼠标 -> 其他鼠标设置 ->
 *     指针选项 -> 勾选"提高指针精确度"。
 * ============================================================
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace grt {

/// 鼠标加速预检服务（全部为静态方法，无需实例化）
class MouseAccelService {
public:
	MouseAccelService() = delete;

	/**
	 * @brief 查询系统鼠标加速（增强指针精确度）是否开启。
	 * @return 开启返回 true，关闭返回 false。
	 *
	 * SystemParametersInfoW(SPI_GETMOUSE) 返回三个 INT：
	 * 前两个为加速阈值，第三个为加速开关（非 0 即开启）。
	 * 查询失败时按保守策略返回 true（当作开启，阻止录制）。
	 */
	static bool IsEnhancePointerPrecisionOn();

	/**
	 * @brief 彻底关闭系统鼠标加速曲线。
	 *
	 * 通过 SPI_SETMOUSE 写入 {0, 0, 0}（两个阈值与加速开关全部归零），
	 * 并以 SPIF_SENDCHANGE 广播设置变更。
	 *
	 * 再次强调：【关闭后不自动恢复，用户需自行在 Windows 设置中开启】。
	 * @return 写入成功返回 true，失败返回 false。
	 */
	static bool DisableEnhancePointerPrecision();

	/**
	 * @brief 录制准入判断：鼠标加速关闭时才允许开始录制。
	 * @return 允许录制返回 true。
	 */
	static bool CanStartRecording();

	/**
	 * @brief 只读查询当前两个加速阈值（不做任何修改用途）。
	 * @param threshold1 输出第一个阈值（SPI_GETMOUSE 第 1 个值）。
	 * @param threshold2 输出第二个阈值（SPI_GETMOUSE 第 2 个值）。
	 * @return 查询成功返回 true。
	 */
	static bool GetThresholds(int& threshold1, int& threshold2);
};

} // namespace grt
