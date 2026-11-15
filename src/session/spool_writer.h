/**
 * @file spool_writer.h
 * @brief 原始事件 spool（<base>_raw.jsonl）崩溃取证写入器。
 *
 * 录制过程中每一条原始 InputEvent 都以一行 JSON（JSON Lines）实时落盘，
 * 即使进程随后崩溃 / 断电，最近一次定时 flush 之前的事件仍然可追溯、
 * 可用于离线重处理（Task 12）。
 *
 * 可靠性策略：
 *   - 自建约 4KB 的 std::FILE 缓冲 + 每个处理循环（8ms）至少调用一次
 *     MaybeTimedFlush（50ms 周期）+ Stop 前强制 Flush：崩溃场景最多丢失
 *     约 50ms 的事件；
 *   - JSON 手写转义（不引入第三方 JSON 库），所有字符串按 JSON 字符串
 *     规则转义控制字符 / 引号 / 反斜杠，保证每行都是合法可解析 JSON；
 *   - 输出 UTF-8（Open 内部把 UTF-8 路径转 UTF-16 后 _wfopen）。
 *
 * 非线程安全：仅由 SessionManager 的处理线程使用。
 */

#pragma once

#include <cstdio>
#include <cstdint>
#include <string>

#include "input/input_event.h"

namespace grt {

class SpoolWriter {
public:
	/// 定时 flush 周期（秒）：崩溃时最多丢失此时长内的事件
	static constexpr double kFlushIntervalSeconds = 0.05;
	/// std::FILE 用户缓冲大小（字节）
	static constexpr std::size_t kBufferBytes = 4096;

	SpoolWriter();
	~SpoolWriter();

	SpoolWriter(const SpoolWriter&) = delete;
	SpoolWriter& operator=(const SpoolWriter&) = delete;

	/**
	 * @brief 创建/截断并打开 spool 文件。
	 * @param utf8Path 完整文件路径（UTF-8）。
	 * @return true 打开成功（且 4KB 缓冲设置成功）；false 文件无法创建。
	 */
	bool Open(const std::string& utf8Path);

	/// 立即 flush 用户缓冲到操作系统（fflush）；未打开为空操作
	void Flush();

	/**
	 * @brief 距离上次 flush 超过 kFlushIntervalSeconds 时 flush 一次。
	 * @param nowQpc 当前 QPC 秒（grt::Clock::NowSeconds()）。
	 */
	void MaybeTimedFlush(double nowQpc);

	/// 强制 flush 并关闭文件；未打开为空操作。析构会自动调用
	void Close();

	/// 文件是否处于打开状态
	bool IsOpen() const { return file_ != nullptr; }

	/// 已写入的原始事件行数（不含会话元信息行）
	std::uint64_t EventCount() const { return eventCount_; }

	/// 当前打开的文件路径（UTF-8）
	const std::string& Path() const { return path_; }

	/**
	 * @brief 写入一行会话元信息（录制开始标记）。
	 *
	 * 形如 {"kind":"session_start","qpc0":...,"started_at_utc":"..."}。
	 * @return 写入成功返回 true。
	 */
	bool WriteSessionStart(double qpc0, const std::string& startedAtUtc);

	/**
	 * @brief 写入一条原始输入事件（一行 JSONL）。
	 * @param event       原始事件（event.t 为采集 QPC 秒）。
	 * @param sessionTime 已换算的会话时间（秒，可为负：表示先于 qpc0）。
	 * @return 写入成功返回 true。
	 */
	bool WriteEvent(const InputEvent& event, double sessionTime);

	/**
	 * @brief JSON 字符串转义（纯函数，便于 Task 13 单测）。
	 * @return 可直接放入两个双引号之间的转义结果（不含外层引号）。
	 */
	static std::string EscapeJson(const std::string& raw);

private:
	/// 向文件写入一整行（末尾补 '\n'），失败时输出一次告警
	bool WriteLine(const std::string& line);

	std::FILE* file_ = nullptr;
	std::string path_;
	char buffer_[kBufferBytes]{};
	std::uint64_t eventCount_ = 0;
	double lastFlushQpc_ = 0.0;
	bool hasFlushClock_ = false;
};

} // namespace grt
