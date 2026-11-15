/**
 * @file log.cpp
 * @brief 统一日志封装与 Win32 错误信息工具实现。
 */

#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <string>

#include "core/log_bus.h"

void grt_log(int level, const char *format, ...)
{
	// 先格式化为完整字符串：blog 需要一份 va_list，LogBus 推送也需要
	// 成品文本。先 vsnprintf 探测长度（va_list 被消费后不可复用，
	// 下面用 va_copy 为 blogva 单独保留一份）。
	va_list sizingArgs;
	va_start(sizingArgs, format);
	va_list blogArgs;
	va_copy(blogArgs, sizingArgs);

	const int needed = std::vsnprintf(nullptr, 0, format, sizingArgs);
	va_end(sizingArgs);

	std::string text;
	if (needed > 0) {
		text.resize(static_cast<size_t>(needed));
		// vsnprintf 会多写一个 '\0'，直接写入 string 的容量区，
		// 再把长度截回 needed（不依赖那个 '\0'）。
		std::vsnprintf(text.data(), static_cast<size_t>(needed) + 1,
		               format, blogArgs);
	}
	va_end(blogArgs);

	if (!text.empty()) {
		// 直接输出已格式化的完整文本，避免再次解析格式串
		blog(level, "%s", text.c_str());
		// 推送到进程内日志总线供 GUI 订阅（零订阅时内部为轻量追加）
		grt::LogBus::Instance().AddEntry(level, text);
	}
}

std::string Win32ErrorMessage(DWORD errorCode)
{
	// 优先使用系统消息表中的本地化描述；不强制自动换行
	PWSTR wideBuffer = nullptr;
	const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
	                    FORMAT_MESSAGE_IGNORE_INSERTS;

	DWORD length = FormatMessageW(flags, nullptr, errorCode,
	                              MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	                              reinterpret_cast<PWSTR>(&wideBuffer), 0, nullptr);

	std::wstring wide;
	if (length > 0 && wideBuffer != nullptr) {
		wide.assign(wideBuffer, length);
	}
	if (wideBuffer != nullptr) {
		LocalFree(wideBuffer);
	}

	// 去掉消息末尾的换行/空白
	while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' ||
	                         wide.back() == L' ' || wide.back() == L'\t')) {
		wide.pop_back();
	}

	if (wide.empty()) {
		char fallback[64];
		std::snprintf(fallback, sizeof(fallback), "未知错误 (code=%lu)",
		              static_cast<unsigned long>(errorCode));
		return std::string(fallback);
	}

	// UTF-16 -> UTF-8
	const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
	                                           static_cast<int>(wide.size()), nullptr, 0,
	                                           nullptr, nullptr);
	std::string result;
	if (utf8Length > 0) {
		result.resize(static_cast<size_t>(utf8Length));
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
		                    result.data(), utf8Length, nullptr, nullptr);
	}
	return result;
}
