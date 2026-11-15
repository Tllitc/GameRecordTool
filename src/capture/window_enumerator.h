/**
 * @file window_enumerator.h
 * @brief 顶层可采集窗口枚举与 win-capture 窗口规格串工具。
 *
 * 窗口规格格式（从 OBS 32.2.2 源码确认，见
 * libobs/util/windows/window-helpers.c 中 ms_build_window_strings /
 * add_window）：
 *
 *     "<title>:<class>:<exe>"
 *
 *   - 三段顺序为 标题:类名:可执行文件名（仅文件名，不含路径）；
 *   - 段内出现的 '#' 先转义为 "#22"，':' 再转义为 "#3A"
 *     （解码顺序相反：先 "#3A"->':'，再 "#22"->'#'）；
 *   - 以 ':' 切分三段，三段均非空才视为有效规格。
 *
 * game_capture / window_capture 源的 "window" 设置值均使用该格式。
 *
 * 本类只有纯 Win32 调用（EnumWindows / 进程查询），不涉及 libobs，
 * 因此没有 OBS 主线程要求；可在任意线程调用。
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>
#include <vector>

namespace grt {

/// 单个可采集顶层窗口的描述信息
struct WindowInfo {
	HWND hwnd = nullptr;       ///< 窗口句柄
	DWORD pid = 0;             ///< 所属进程 ID
	std::wstring title;        ///< 窗口标题（UTF-16）
	std::wstring exeName;      ///< 可执行文件名（如 "game.exe"，不含目录）
	std::wstring exePath;      ///< 可执行文件完整路径；取不到时为空
	int clientWidth = 0;       ///< 客户区宽度（像素）
	int clientHeight = 0;      ///< 客户区高度（像素）
};

class WindowEnumerator {
public:
	/**
	 * @brief 枚举当前所有可见、有标题、客户区非空的顶层窗口。
	 *
	 * 过滤规则（与 win-capture 的 check_window_valid 基本一致）：
	 *   - IsWindowVisible 为真；
	 *   - 标题非空；
	 *   - GetClientRect 宽、高均大于 0；
	 *   - 非 WS_CHILD 子窗口、非 WS_EX_TOOLWINDOW 工具窗。
	 *
	 * 进程信息通过 OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) +
	 * QueryFullProcessImageNameW 获取完整路径与文件名。取不到路径的
	 * 系统窗口（如部分受保护进程）直接跳过；普通应用即使路径查询失败
	 * 也保留（exeName 可能为空）。
	 */
	static std::vector<WindowInfo> Enumerate();

	/**
	 * @brief 依据窗口信息生成 win-capture 的窗口匹配规格串。
	 * @return UTF-8 编码的 "<title>:<class>:<exeName>" 串；
	 *         类名在本函数内通过 GetClassNameW 实时获取。
	 *
	 * 输出为 UTF-8 是因为 obs_data_set_string 的 "window" 键要求
	 * 窄字符串（libobs 内部按 UTF-8 解释）。
	 */
	static std::string MakeWindowMatchSpec(const WindowInfo& info);

	/**
	 * @brief 对规格串的单个字段执行 win-capture 转义。
	 *
	 * 纯函数（无 Win32/libobs 依赖），可直接单元测试。
	 * 转义顺序固定：先 '#' -> "#22"，再 ':' -> "#3A"。
	 */
	static std::string EncodeMatchSpec(const std::string& field);

	/**
	 * @brief 将 win-capture 规格串解析回 标题/类名/exe 三段。
	 *
	 * 纯函数（无 Win32/libobs 依赖），可直接单元测试。
	 * 解码顺序固定：先 "#3A" -> ':'，再 "#22" -> '#'。
	 *
	 * @param spec     规格串（UTF-8）。
	 * @param title    [out] 解码后的标题。
	 * @param className [out] 解码后的窗口类名。
	 * @param exe      [out] 解码后的可执行文件名。
	 * @return 三段齐全且切分成功返回 true；否则返回 false（出参内容不保证）。
	 */
	static bool ParseMatchSpec(const std::string& spec,
	                           std::string& title,
	                           std::string& className,
	                           std::string& exe);

	/**
	 * @brief 获取当前前台窗口所属进程的可执行文件名（如 "game.exe"）。
	 * @return 成功返回 UTF-8 文件名（不含路径）；失败返回空串。
	 *
	 * 供后续任务（SessionManager 等）判断用户当前聚焦的游戏，本任务暂不使用。
	 */
	static std::string CurrentForegroundExe();
};

} // namespace grt
