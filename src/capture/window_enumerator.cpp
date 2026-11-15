/**
 * @file window_enumerator.cpp
 * @brief 顶层可采集窗口枚举与 win-capture 窗口规格串实现。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/window_enumerator.h"

#include <algorithm>

#include "core/log.h"

namespace grt {

namespace {

/// UTF-16 转 UTF-8（失败返回空串）
std::string WideToUtf8(const std::wstring& wide)
{
	if (wide.empty()) {
		return {};
	}

	const int len = WideCharToMultiByte(
		CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
		nullptr, 0, nullptr, nullptr);
	if (len <= 0) {
		return {};
	}

	std::string utf8(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
	                    static_cast<int>(wide.size()),
	                    utf8.data(), len, nullptr, nullptr);
	return utf8;
}

/// UTF-16 转 UTF-8（以 NUL 结尾的缓冲区版本）
std::string WideToUtf8(const wchar_t* wide)
{
	return wide ? WideToUtf8(std::wstring(wide)) : std::string{};
}

/**
 * @brief 查询进程的可执行文件完整路径。
 * @param pid  进程 ID。
 * @param path [out] 成功时写入 NT 风格路径（可能以 "\Device\..." 或盘符开头，
 *             QueryFullProcessImageNameW 在 Win10+ 通常返回 Win32 路径）。
 * @return 成功返回 true。
 */
bool QueryProcessImagePath(DWORD pid, std::wstring& path)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) {
		return false;
	}

	wchar_t buffer[MAX_PATH] = {0};
	DWORD size = MAX_PATH;
	const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer, &size);
	CloseHandle(process);

	if (!ok || size == 0) {
		return false;
	}

	path.assign(buffer, size);
	return true;
}

/// 取窗口类名（UTF-16）
std::wstring GetWindowClassName(HWND hwnd)
{
	wchar_t buffer[256] = {0};
	const int count = GetClassNameW(hwnd, buffer, 256);
	return count > 0 ? std::wstring(buffer, static_cast<size_t>(count))
	                 : std::wstring{};
}

/// EnumWindows 回调上下文
struct EnumContext {
	std::vector<WindowInfo>* windows;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
	auto* ctx = reinterpret_cast<EnumContext*>(lParam);

	// 过滤 1：窗口必须可见（含 IsWindowVisible 的祖先链可见性判断）
	if (!IsWindowVisible(hwnd)) {
		return TRUE;
	}

	// 过滤 2：子窗口、工具窗口不采集（与 win-capture check_window_valid 一致）
	const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if ((style & WS_CHILD) != 0 || (exStyle & WS_EX_TOOLWINDOW) != 0) {
		return TRUE;
	}

	// 过滤 3：标题非空
	const int titleLength = GetWindowTextLengthW(hwnd);
	if (titleLength <= 0) {
		return TRUE;
	}

	// 过滤 4：客户区宽高均大于 0
	RECT clientRect{};
	if (!GetClientRect(hwnd, &clientRect) ||
	    clientRect.right <= 0 || clientRect.bottom <= 0) {
		return TRUE;
	}

	WindowInfo info;
	info.hwnd = hwnd;
	info.clientWidth = clientRect.right;
	info.clientHeight = clientRect.bottom;

	std::wstring title(static_cast<size_t>(titleLength) + 1, L'\0');
	const int copied = GetWindowTextW(hwnd, title.data(),
	                                  static_cast<int>(title.size()));
	if (copied <= 0) {
		return TRUE; // 标题在枚举过程中消失
	}
	title.resize(static_cast<size_t>(copied));
	info.title = std::move(title);

	GetWindowThreadProcessId(hwnd, &info.pid);

	// 查询进程完整路径；取不到路径的受保护系统窗口直接跳过，
	// 普通应用一般都能通过 LIMITED_INFORMATION 权限查到。
	std::wstring imagePath;
	if (QueryProcessImagePath(info.pid, imagePath)) {
		info.exePath = imagePath;
		const size_t slash = imagePath.find_last_of(L"\\/");
		if (slash != std::wstring::npos) {
			info.exeName = imagePath.substr(slash + 1);
		}
	} else {
		// 排除 OBS 自身（win-capture 源码同样跳过当前进程），
		// 其余无路径窗口（系统壳/受保护进程）不纳入候选。
		if (info.pid == GetCurrentProcessId()) {
			return TRUE;
		}
		GRT_LOG_DEBUG("窗口进程路径查询失败，已跳过：pid=%lu 标题长度=%d",
		              static_cast<unsigned long>(info.pid), titleLength);
		return TRUE;
	}

	ctx->windows->push_back(std::move(info));
	return TRUE;
}

} // namespace

std::vector<WindowInfo> WindowEnumerator::Enumerate()
{
	std::vector<WindowInfo> result;
	EnumContext ctx{&result};

	// EnumWindows 只枚举顶层窗口；枚举期间窗口增删由系统保证快照稳定。
	EnumWindows(&EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));

	return result;
}

std::string WindowEnumerator::EncodeMatchSpec(const std::string& field)
{
	std::string encoded;
	encoded.reserve(field.size());

	for (char ch : field) {
		if (ch == '#') {
			encoded += "#22";
		} else if (ch == ':') {
			encoded += "#3A";
		} else {
			encoded.push_back(ch);
		}
	}

	return encoded;
}

bool WindowEnumerator::ParseMatchSpec(const std::string& spec,
                                      std::string& title,
                                      std::string& className,
                                      std::string& exe)
{
	// 未转义的字段中不应再出现 ':'，因此恰好切出 3 段才算合法。
	std::vector<size_t> separators;
	for (size_t i = 0; i < spec.size(); ++i) {
		if (spec[i] == ':') {
			separators.push_back(i);
		}
	}
	if (separators.size() != 2) {
		return false;
	}

	auto decode = [](std::string value) {
		// 解码顺序与编码相反：先还原冒号，再还原 '#'
		for (size_t pos = 0; (pos = value.find("#3A", pos)) != std::string::npos;) {
			value.replace(pos, 3, ":");
			pos += 1;
		}
		for (size_t pos = 0; (pos = value.find("#22", pos)) != std::string::npos;) {
			value.replace(pos, 3, "#");
			pos += 1;
		}
		return value;
	};

	title = decode(spec.substr(0, separators[0]));
	className = decode(spec.substr(separators[0] + 1,
	                               separators[1] - separators[0] - 1));
	exe = decode(spec.substr(separators[1] + 1));
	return true;
}

std::string WindowEnumerator::MakeWindowMatchSpec(const WindowInfo& info)
{
	const std::wstring classNameW = GetWindowClassName(info.hwnd);

	const std::string title = EncodeMatchSpec(WideToUtf8(info.title));
	const std::string className = EncodeMatchSpec(WideToUtf8(classNameW));
	const std::string exe = EncodeMatchSpec(WideToUtf8(info.exeName));

	std::string spec;
	spec.reserve(title.size() + className.size() + exe.size() + 2);
	spec += title;
	spec.push_back(':');
	spec += className;
	spec.push_back(':');
	spec += exe;
	return spec;
}

std::string WindowEnumerator::CurrentForegroundExe()
{
	HWND foreground = GetForegroundWindow();
	if (!foreground) {
		return {};
	}

	DWORD pid = 0;
	GetWindowThreadProcessId(foreground, &pid);
	if (pid == 0) {
		return {};
	}

	std::wstring path;
	if (!QueryProcessImagePath(pid, path)) {
		return {};
	}

	const size_t slash = path.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		path = path.substr(slash + 1);
	}
	return WideToUtf8(path);
}

} // namespace grt
