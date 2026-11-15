/**
 * @file log_bus.h
 * @brief 全局日志总线（header-only）。
 *
 * 每条经 GRT_LOG_* 输出的日志在 grt_log() 内格式化完成后都会调用
 * LogBus::AddEntry()，供悬浮窗 / Dock GUI 实时订阅显示。
 *
 * 设计要点：
 *   - 进程内单例（函数局部静态，C++11 起初始化线程安全）；
 *   - 环形保留最近 kRetainEntries（200）条，GUI 订阅时可先取一次快照
 *     再持续接收新日志；
 *   - 用原子订阅计数做快速路径：零订阅时 AddEntry 只做一次极短的
 *     环形缓冲追加，不构造回调副本、不虚调用；
 *   - 回调在锁外执行，避免订阅者在回调中（经 QueuedConnection 排队等）
 *     与总线互斥；订阅者只需保证自身生命周期内正确 Unsubscribe。
 *
 * 不依赖 OBS / Qt，level 取值与 OBS blog 级别一致（LOG_INFO 等）。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace grt {

/// 一条已格式化完成的日志（text 已含 "[GameRecordTool] " 前缀）
struct LogEntry {
	int level = 0;             ///< OBS blog 级别（LOG_INFO / LOG_WARNING / ...）
	std::string text;          ///< 完整日志文本
	double timeSeconds = 0.0;  ///< 总线启动后的单调秒数，仅用于排序/节流显示
};

class LogBus {
public:
	/// 订阅回调类型（GUI 侧需自行通过 Qt QueuedConnection 等方式切线程）
	using LogCallback = std::function<void(const LogEntry&)>;

	/// 环形保留的最近日志条数
	static constexpr std::size_t kRetainEntries = 200;

	/// 进程内单例
	static LogBus& Instance()
	{
		static LogBus bus;
		return bus;
	}

	/**
	 * @brief 订阅实时日志。
	 * @param callback 每条新日志的回调（在 AddEntry 调用线程上同步执行，
	 *                 通常为任意业务线程；回调内不得做阻塞操作）。
	 * @return 不透明订阅句柄（>=1）；callback 为空返回 0。
	 */
	std::uint64_t Subscribe(LogCallback callback)
	{
		if (!callback) {
			return 0;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		const std::uint64_t handle = nextHandle_++;
		subscribers_.push_back(Subscriber{handle, std::move(callback)});
		subscriberCount_.store(static_cast<int>(subscribers_.size()),
		                       std::memory_order_release);
		return handle;
	}

	/// 退订（句柄无效时为空操作）
	void Unsubscribe(std::uint64_t handle)
	{
		if (handle == 0) {
			return;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
			if (it->handle == handle) {
				subscribers_.erase(it);
				break;
			}
		}
		subscriberCount_.store(static_cast<int>(subscribers_.size()),
		                       std::memory_order_release);
	}

	/**
	 * @brief 追加一条日志（由 grt_log 在格式化后调用）。
	 *
	 * 无论是否有订阅者都会写入环形缓冲；订阅者回调在锁外派发。
	 */
	void AddEntry(int level, const std::string& text)
	{
		LogEntry entry;
		entry.level = level;
		entry.text = text;
		entry.timeSeconds = SecondsSinceStart();

		// 先在锁内追加环形缓冲，并拷贝一份当前订阅者快照
		std::vector<Subscriber> snapshot;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			recent_.push_back(entry);
			if (recent_.size() > kRetainEntries) {
				recent_.pop_front();
			}
			if (subscriberCount_.load(std::memory_order_acquire) > 0) {
				snapshot = subscribers_;
			}
		}

		// 零订阅时 snapshot 为空，直接走轻量路径返回
		for (const auto& sub : snapshot) {
			sub.callback(entry);
		}
	}

	/**
	 * @brief 获取最近最多 maxCount 条日志的快照（按时间从旧到新）。
	 *
	 * GUI 订阅前先调用一次，可立即填充日志面板的历史内容。
	 */
	std::vector<LogEntry> SnapshotRecent(std::size_t maxCount = kRetainEntries) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<LogEntry> out;
		if (recent_.empty() || maxCount == 0) {
			return out;
		}
		const std::size_t start = recent_.size() > maxCount
		                              ? recent_.size() - maxCount
		                              : 0;
		out.reserve(recent_.size() - start);
		for (auto it = recent_.begin() + static_cast<std::ptrdiff_t>(start);
		     it != recent_.end(); ++it) {
			out.push_back(*it);
		}
		return out;
	}

private:
	LogBus() : startSteady_(std::chrono::steady_clock::now()) {}

	double SecondsSinceStart() const
	{
		const auto now = std::chrono::steady_clock::now();
		return std::chrono::duration<double>(now - startSteady_).count();
	}

	struct Subscriber {
		std::uint64_t handle;
		LogCallback callback;
	};

	mutable std::mutex mutex_;
	std::deque<LogEntry> recent_;       ///< 环形保留（超出容量弹出最旧）
	std::vector<Subscriber> subscribers_;
	std::atomic<int> subscriberCount_{0}; ///< 零订阅快速路径判据
	std::uint64_t nextHandle_ = 1;
	std::chrono::steady_clock::time_point startSteady_;
};

} // namespace grt
