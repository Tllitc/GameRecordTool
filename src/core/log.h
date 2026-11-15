/**
 * @file log.h
 * @brief 统一日志封装与 Win32 错误信息工具。
 *
 * 所有日志经 GRT_LOG_* 宏输出，自动添加 "[GameRecordTool] " 前缀，
 * 最终转发到 OBS 的 blog()（printf 风格）。
 *
 * DEBUG 级别受编译期开关 GRT_VERBOSE_LOG 控制：
 *   - 默认（Release）关闭，GRT_LOG_DEBUG 不产生任何代码；
 *   - 由 CMake option(GRT_VERBOSE_LOG) 决定是否开启。
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

#include "util/base.h" // blog / LOG_INFO / LOG_WARNING / LOG_ERROR / LOG_DEBUG

/**
 * @brief 内部统一日志实现（供宏调用，业务代码不要直接使用）。
 * @param level  OBS 日志级别（LOG_INFO 等）。
 * @param format printf 风格格式串（已含 [GameRecordTool] 前缀）。
 */
void grt_log(int level, const char *format, ...);

/// 统一前缀，所有插件日志均以此开头，便于在 OBS 日志中检索
#define GRT_LOG_PREFIX "[GameRecordTool] "

#define GRT_LOG_INFO(format, ...) grt_log(LOG_INFO, GRT_LOG_PREFIX format, ##__VA_ARGS__)
#define GRT_LOG_WARN(format, ...) grt_log(LOG_WARNING, GRT_LOG_PREFIX format, ##__VA_ARGS__)
#define GRT_LOG_ERROR(format, ...) grt_log(LOG_ERROR, GRT_LOG_PREFIX format, ##__VA_ARGS__)

#ifdef GRT_VERBOSE_LOG
#define GRT_LOG_DEBUG(format, ...) grt_log(LOG_DEBUG, GRT_LOG_PREFIX format, ##__VA_ARGS__)
#else
// 详细日志关闭时编译为空，避免格式化参数被求值带来的开销
#define GRT_LOG_DEBUG(format, ...) ((void)0)
#endif

/**
 * @brief 将 Win32 错误码转换为可读的 UTF-8 文本。
 * @param errorCode Win32 错误码，默认取当前线程的 GetLastError()。
 * @return 系统错误消息的 UTF-8 字符串；无法获取时返回形如
 *         "未知错误 (code=123)" 的占位文本。
 */
std::string Win32ErrorMessage(DWORD errorCode = GetLastError());
