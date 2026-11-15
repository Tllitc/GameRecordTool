/**
 * @file raw_input_service.h
 * @brief 基于 Raw Input（WM_INPUT）的键鼠采集服务。
 *
 * 服务在内部专用线程上创建"仅消息窗口"（HWND_MESSAGE，
 * 窗口类名 L"GRTRawInputMsgWindow"），并以 RIDEV_INPUTSINK 方式注册
 * 键盘与鼠标，因此即使窗口 / 进程不在前台，也能在后台持续收到输入，
 * 且对系统输入完全只读（不拦截、不注入、不改变鼠标加速）。
 *
 * 解析出的 grt::InputEvent 追加到内部线程安全队列，由录制线程周期性
 * Drain/Poll 取走。时间戳统一在拿到 RAWINPUT 数据时用
 * grt::Clock::NowSeconds() 采集，同一条 RAWINPUT 拆出的多个事件
 * 共享同一个时间戳。
 *
 * 多消费方共享（重要）：
 *   进程内所有消费方（录制会话 / 全局热键 / 改键捕获）必须通过
 *   Shared() 复用同一个实例，并各自 Subscribe() 拿到独立缓冲。原因：
 *   Windows 对同一 usage 的 RIDEV_INPUTSINK 注册是"后来者覆盖"，同一
 *   进程内注册两次时只有最后一个注册的窗口会收到 WM_INPUT，其余实例
 *   永远取不到事件。共享实例后由 Enqueue 向各订阅缓冲扇出，互不抢占。
 *
 * 生命周期约束：
 *   - Start() / Stop() 均幂等，可从任意线程调用；
 *   - Start()/Stop() 只由 plugin-main 统一管理；其余消费方仅
 *     Subscribe()/Unsubscribe()，不得停止共享服务。
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "input/input_event.h"

namespace grt {

class RawInputService {
public:
	RawInputService();
	~RawInputService();

	RawInputService(const RawInputService&) = delete;
	RawInputService& operator=(const RawInputService&) = delete;

	/**
	 * @brief 创建消息窗口、注册 Raw Input 设备并启动内部消息泵线程。
	 * @return 成功返回 true；已经处于运行状态也视为成功。
	 *         任一步骤失败都会清理半初始化资源并返回 false。
	 */
	bool Start();

	/**
	 * @brief 停止消息泵、撤销设备注册、销毁窗口并等待线程结束。
	 *
	 * 幂等，可从任意线程调用；在内部线程之外的线程调用时会 join
	 * 采集线程。窗口销毁与设备注销在采集线程上执行（Win32 窗口
	 * 具有线程亲和性，DestroyWindow 必须由创建窗口的线程调用）。
	 */
	void Stop();

	/// 是否正在运行
	bool IsRunning() const;

	/// 订阅句柄：0 表示无效
	using SubscriptionId = std::uint64_t;
	static constexpr SubscriptionId kInvalidSubscription = 0;

	/**
	 * @brief 进程内共享实例。
	 *
	 * 同一进程内的多个消费方（录制会话 / 全局热键 / 改键捕获）必须复用
	 * 同一个 RawInputService：Windows 对同一 usage 的 RIDEV_INPUTSINK
	 * 注册是"后者覆盖"，多实例会导致只有最后注册的窗口收到 WM_INPUT，
	 * 其余实例永远取不到事件。
	 */
	static RawInputService& Shared();

	/**
	 * @brief 订阅事件流：返回独立缓冲的句柄，同一事件会扇出给所有订阅者。
	 *        调用方负责在不再需要时 Unsubscribe。
	 */
	SubscriptionId Subscribe();

	/// 取消订阅（幂等）。仅释放本订阅的缓冲，不影响服务运行状态与其他订阅。
	void Unsubscribe(SubscriptionId id);

	/// 取出并清空指定订阅的缓冲，追加到 out 末尾；无效/不存在句柄返回 0。
	size_t Drain(SubscriptionId id, std::vector<InputEvent>& out);

	/// 取出并清空指定订阅的缓冲（便捷重载）；无效/不存在句柄返回空。
	std::vector<InputEvent> Poll(SubscriptionId id);

	/**
	 * @brief 取出并清空内部缓冲，追加到 out 末尾。
	 * @return 本次取出的事件数量。内部通过交换缓冲区实现，持锁时间最短。
	 */
	size_t Drain(std::vector<InputEvent>& out);

	/**
	 * @brief 取出并清空内部缓冲（便捷重载）。
	 * @return 自上次取出后采集到的全部事件；无事件时返回空 vector。
	 */
	std::vector<InputEvent> Poll();

private:
	/// 内部线程入口：初始化窗口 / 设备，运行消息泵，退出时清理
	void ThreadMain();

	/// 在采集线程上完成窗口类注册、建窗与设备注册；成功返回 true
	bool InitializeOnThread();

	/// 在采集线程上撤销设备注册、销毁窗口并注销窗口类
	void ShutdownOnThread();

	/// 窗口过程静态 thunk（通过 GWLP_USERDATA 转发到实例）
	static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
	                                     LPARAM lParam);

	/// 实例窗口过程：只处理 WM_INPUT（其余交给 DefWindowProc）
	LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	/// 解析并分发一条 WM_INPUT 消息
	void HandleRawInput(HRAWINPUT hRawInput);

	/// 解析键盘 RAWINPUT
	void ParseKeyboard(const struct tagRAWINPUT& raw, double timestamp);

	/// 解析鼠标 RAWINPUT（同一条消息可能拆成 MOVE/BUTTON/WHEEL 多个事件）
	void ParseMouse(const struct tagRAWINPUT& raw, double timestamp);

	/// 生产者（采集线程）入队
	void Enqueue(InputEvent&& event);

	std::thread thread_;                ///< 采集线程
	DWORD threadId_ = 0;                ///< 采集线程 ID（供 PostThreadMessageW）
	HWND hwnd_ = nullptr;               ///< 仅消息窗口句柄
	ATOM windowClassAtom_ = 0;          ///< 已注册窗口类原子（注销时用）
	bool classOwned_ = false;           ///< 窗口类是否由本实例注册（决定是否负责注销）
	std::atomic<bool> running_{false};  ///< Start/Stop 状态标记（跨线程读取）
	bool registered_ = false;           ///< Raw Input 设备是否已成功注册

	std::mutex queueMutex_;             ///< 保护 pendingEvents_
	std::vector<InputEvent> pendingEvents_; ///< 待消费事件缓冲
	/// 各订阅者的独立事件缓冲（扇出目标），与 pendingEvents_ 共用 queueMutex_
	std::map<SubscriptionId, std::vector<InputEvent>> subscriptionBuffers_;
	/// 下一个订阅句柄（从 1 开始；0 保留为无效值）
	SubscriptionId nextSubscriptionId_ = 1;
};

} // namespace grt
