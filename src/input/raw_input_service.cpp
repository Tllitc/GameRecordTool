/**
 * @file raw_input_service.cpp
 * @brief 基于 Raw Input（WM_INPUT）的键鼠采集服务实现。
 *
 * 设计要点：
 *   - 采集线程独立，线程内创建仅消息窗口并注册 RIDEV_INPUTSINK 设备；
 *   - 窗口具有线程亲和性，注销设备 / 销毁窗口均在采集线程上完成；
 *   - 同一条 RAWINPUT 中的鼠标多属性拆成独立事件，共享一个时间戳；
 *   - 事件队列用互斥锁保护，消费侧 Drain 时整体交换，持锁时间最短。
 *
 * 本任务只实现类本身，不在 plugin-main 中接线；RIDEV_INPUTSINK 为只读
 * 注册，不影响系统输入，后续任务可在插件加载后直接 Start。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "input/raw_input_service.h"

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include <windows.h>

#include "core/clock.h"
#include "core/log.h"

namespace grt {

namespace {

/// 仅消息窗口的窗口类名（全进程唯一）
constexpr wchar_t kWindowClassName[] = L"GRTRawInputMsgWindow";

/// 将 usButtonData 的低位 16 位按带符号解释（滚轮 delta）
int16_t SignedWheelDelta(USHORT buttonData)
{
	return static_cast<int16_t>(static_cast<uint16_t>(buttonData & 0xFFFFu));
}

} // 匿名命名空间

RawInputService::RawInputService()
{
	// 预留容量，减少高频鼠标移动下的反复扩容
	std::lock_guard<std::mutex> lock(queueMutex_);
	pendingEvents_.reserve(256);
}

RawInputService::~RawInputService()
{
	Stop();
}

bool RawInputService::IsRunning() const
{
	return running_.load();
}

bool RawInputService::Start()
{
	// 幂等：已经在运行直接返回成功
	if (running_.exchange(true)) {
		return true;
	}

	// 用于采集线程回传初始化结果的一次性事件
	HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (readyEvent == nullptr) {
		running_.store(false);
		GRT_LOG_ERROR("CreateEvent 失败：%s", Win32ErrorMessage().c_str());
		return false;
	}

	bool initOk = false;
	try {
		thread_ = std::thread([this, readyEvent, &initOk]() {
			// 先强制创建本线程消息队列，确保 Stop 可能在初始化失败
			// 提前返回的竞态下 PostThreadMessageW 也不会静默丢失
			MSG peek;
			PeekMessageW(&peek, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
			threadId_ = GetCurrentThreadId();

			initOk = InitializeOnThread();
			SetEvent(readyEvent);

			if (initOk) {
				// 消息泵：仅分发投递到窗口的消息（WM_INPUT），
				// WM_QUIT 由 PostThreadMessageW 投递到线程队列并在此退出
				MSG msg;
				while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
					if (msg.hwnd != nullptr) {
						DispatchMessageW(&msg);
					}
				}
			}

			// 窗口必须由创建它的线程销毁（Win32 线程亲和性），
			// 因此注销设备与销毁窗口统一放在采集线程收尾
			ShutdownOnThread();
		});
	} catch (const std::system_error& e) {
		running_.store(false);
		CloseHandle(readyEvent);
		GRT_LOG_ERROR("启动采集线程失败：%s", e.what());
		return false;
	}

	// 等待初始化（建窗 / 注册设备）完成后再向调用方返回结果
	WaitForSingleObject(readyEvent, INFINITE);
	CloseHandle(readyEvent);

	if (!initOk) {
		if (thread_.joinable()) {
			thread_.join();
		}
		threadId_ = 0;
		running_.store(false);
		return false;
	}

	GRT_LOG_INFO("RawInputService 已启动（RIDEV_INPUTSINK，线程 ID=%lu）",
	             static_cast<unsigned long>(threadId_));
	return true;
}

void RawInputService::Stop()
{
	// 幂等：只有负责把 running_ 从 true 改成 false 的这次调用执行收尾
	if (!running_.exchange(false)) {
		return;
	}

	const DWORD tid = threadId_;
	if (tid != 0) {
		// 向采集线程的消息队列投递 WM_QUIT，令 GetMessage 返回 0
		if (!PostThreadMessageW(tid, WM_QUIT, 0, 0)) {
			GRT_LOG_WARN("PostThreadMessageW(WM_QUIT) 失败：%s",
			             Win32ErrorMessage().c_str());
		}
	}

	if (thread_.joinable()) {
		// 禁止采集线程自行 join
		if (thread_.get_id() != std::this_thread::get_id()) {
			thread_.join();
		} else {
			thread_.detach();
		}
	}
	threadId_ = 0;
	GRT_LOG_INFO("RawInputService 已停止");
}

bool RawInputService::InitializeOnThread()
{
	HINSTANCE hInstance = GetModuleHandleW(nullptr);

	// 1. 注册窗口类（正常情况下类尚未注册；若同名类已存在则直接复用）
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = &RawInputService::WndProcThunk;
	wc.hInstance = hInstance;
	wc.lpszClassName = kWindowClassName;

	ATOM atom = RegisterClassExW(&wc);
	if (atom != 0) {
		windowClassAtom_ = atom;
		classOwned_ = true;
	} else if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		windowClassAtom_ = 0;
		classOwned_ = false;
		GRT_LOG_WARN("窗口类 %ls 已存在，复用现有注册", kWindowClassName);
	} else {
		GRT_LOG_ERROR("RegisterClassExW 失败：%s", Win32ErrorMessage().c_str());
		return false;
	}

	// 2. 创建仅消息窗口（HWND_MESSAGE 父窗口），CREATESTRUCT 里携带 this
	hwnd_ = CreateWindowExW(0, kWindowClassName, L"GameRecordTool Raw Input",
	                        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, this);
	if (hwnd_ == nullptr) {
		GRT_LOG_ERROR("CreateWindowExW(HWND_MESSAGE) 失败：%s",
		              Win32ErrorMessage().c_str());
		ShutdownOnThread();
		return false;
	}

	// 3. 注册键盘 / 鼠标 Raw Input，目标为消息窗，INPUTSINK 保证后台可收
	std::array<RAWINPUTDEVICE, 2> devices{};
	devices[0].usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
	devices[0].usUsage = 0x06;     // HID_USAGE_GENERIC_KEYBOARD
	devices[0].dwFlags = RIDEV_INPUTSINK;
	devices[0].hwndTarget = hwnd_;
	devices[1].usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
	devices[1].usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
	devices[1].dwFlags = RIDEV_INPUTSINK;
	devices[1].hwndTarget = hwnd_;

	// 说明：任务描述中提到注销时 cbSize=0，但 RAWINPUTDEVICE 结构体本身
	// 没有 cbSize 字段，注销的标准做法是置 RIDEV_REMOVE 且 hwndTarget=nullptr
	// （见 ShutdownOnThread）。
	if (!RegisterRawInputDevices(devices.data(),
	                             static_cast<UINT>(devices.size()),
	                             sizeof(RAWINPUTDEVICE))) {
		GRT_LOG_ERROR("RegisterRawInputDevices 失败：%s",
		              Win32ErrorMessage().c_str());
		ShutdownOnThread();
		return false;
	}
	registered_ = true;
	return true;
}

void RawInputService::ShutdownOnThread()
{
	// 撤销 Raw Input 注册：RIDEV_REMOVE + hwndTarget=nullptr
	if (registered_) {
		std::array<RAWINPUTDEVICE, 2> devices{};
		devices[0].usUsagePage = 0x01;
		devices[0].usUsage = 0x06;
		devices[0].dwFlags = RIDEV_REMOVE;
		devices[0].hwndTarget = nullptr;
		devices[1].usUsagePage = 0x01;
		devices[1].usUsage = 0x02;
		devices[1].dwFlags = RIDEV_REMOVE;
		devices[1].hwndTarget = nullptr;

		if (!RegisterRawInputDevices(devices.data(),
		                             static_cast<UINT>(devices.size()),
		                             sizeof(RAWINPUTDEVICE))) {
			// 进程退出场景下注销失败无实际影响，仅告警
			GRT_LOG_WARN("撤销 Raw Input 注册失败：%s",
			             Win32ErrorMessage().c_str());
		}
		registered_ = false;
	}

	if (hwnd_ != nullptr) {
		if (!DestroyWindow(hwnd_)) {
			GRT_LOG_WARN("DestroyWindow 失败：%s", Win32ErrorMessage().c_str());
		}
		hwnd_ = nullptr;
	}

	if (classOwned_ && windowClassAtom_ != 0) {
		if (!UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr))) {
			GRT_LOG_WARN("UnregisterClass 失败：%s", Win32ErrorMessage().c_str());
		}
		windowClassAtom_ = 0;
		classOwned_ = false;
	}
}

LRESULT CALLBACK RawInputService::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                               LPARAM lParam)
{
	RawInputService* self = nullptr;

	if (msg == WM_NCCREATE) {
		// 建窗的第一个消息，此时从 CREATESTRUCT 取回 this 并挂到 USERDATA
		const CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
		self = static_cast<RawInputService*>(cs->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	} else {
		self = reinterpret_cast<RawInputService*>(
		    GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	}

	if (self != nullptr) {
		return self->WndProc(hwnd, msg, wParam, lParam);
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT RawInputService::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_INPUT) {
		// RIM_INPUT(0)/RIM_INPUTSINK(1) 均正常处理
		HandleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RawInputService::HandleRawInput(HRAWINPUT hRawInput)
{
	// 先查询所需缓冲区大小
	UINT size = 0;
	const UINT headerResult =
	    GetRawInputData(hRawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
	if (headerResult != 0 || size == 0) {
		GRT_LOG_WARN("GetRawInputData 查询大小失败（result=%u）：%s",
		             static_cast<unsigned>(headerResult),
		             Win32ErrorMessage().c_str());
		return;
	}

	std::vector<BYTE> buffer(size);
	RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
	const UINT copied =
	    GetRawInputData(hRawInput, RID_INPUT, raw, &size, sizeof(RAWINPUTHEADER));
	if (copied != size) {
		GRT_LOG_WARN("GetRawInputData 读取数据失败（copied=%u, size=%u）：%s",
		             static_cast<unsigned>(copied), static_cast<unsigned>(size),
		             Win32ErrorMessage().c_str());
		return;
	}

	// 时间戳统一在拿到数据时采集，同一条 RAWINPUT 拆出的事件共用
	const double timestamp = Clock::NowSeconds();

	if (raw->header.dwType == RIM_TYPEKEYBOARD) {
		ParseKeyboard(*raw, timestamp);
	} else if (raw->header.dwType == RIM_TYPEMOUSE) {
		ParseMouse(*raw, timestamp);
	}
}

void RawInputService::ParseKeyboard(const RAWINPUT& raw, double timestamp)
{
	const RAWKEYBOARD& kb = raw.data.keyboard;

	// 键盘缓冲区溢出伪消息（VKEY 0xFF），直接忽略
	if (kb.VKey == 0xFF) {
		return;
	}

	InputEvent event{};
	event.type = InputType::Key;
	event.t = timestamp;
	event.vkey = static_cast<uint16_t>(kb.VKey);
	event.scan_code = static_cast<uint16_t>(kb.MakeCode);
	event.e0 = (kb.Flags & RI_KEY_E0) != 0;
	event.e1 = (kb.Flags & RI_KEY_E1) != 0;
	event.key_down = (kb.Flags & RI_KEY_BREAK) == 0; // 无 BREAK 位即 MAKE(按下)
	// Raw Input 无法可靠判定注入事件，保持 false（见 input_event.h 说明）
	event.injected = false;

	Enqueue(std::move(event));
}

void RawInputService::ParseMouse(const RAWINPUT& raw, double timestamp)
{
	const RAWMOUSE& mouse = raw.data.mouse;
	const USHORT flags = mouse.usButtonFlags;

	// 1) 移动事件：属性消息（usButtonFlags!=0）也可能同时携带 lLastX/lLastY。
	//    按任务要求，一条 RAWINPUT 固定拆出一个 MOVE 事件，dx/dy 允许为 0，
	//    是否丢弃零移动事件由上层决定。
	InputEvent move{};
	move.type = InputType::MouseMove;
	move.t = timestamp;
	move.dx = static_cast<int32_t>(mouse.lLastX);
	move.dy = static_cast<int32_t>(mouse.lLastY);
	Enqueue(std::move(move));

	// 2) 按键事件：逐位检测 DOWN/UP，各自拆成独立事件
	auto emitButton = [&](USHORT downBit, USHORT upBit, uint16_t button) {
		if ((flags & downBit) != 0) {
			InputEvent e{};
			e.type = InputType::MouseButton;
			e.t = timestamp;
			e.button = button;
			e.key_down = true;
			Enqueue(std::move(e));
		}
		if ((flags & upBit) != 0) {
			InputEvent e{};
			e.type = InputType::MouseButton;
			e.t = timestamp;
			e.button = button;
			e.key_down = false;
			Enqueue(std::move(e));
		}
	};

	emitButton(RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP,
	           mouse_buttons::kLeft);
	emitButton(RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP,
	           mouse_buttons::kRight);
	emitButton(RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP,
	           mouse_buttons::kMiddle);
	emitButton(RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP,
	           mouse_buttons::kXButton1);
	emitButton(RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP,
	           mouse_buttons::kXButton2);

	// 3) 滚轮事件：垂直 WHEEL / 水平 HWHEEL（正常不会同时出现，
	//    极端情况下优先按水平处理）
	const bool hWheel = (flags & RI_MOUSE_HWHEEL) != 0;
	const bool vWheel = (flags & RI_MOUSE_WHEEL) != 0;
	if (hWheel || vWheel) {
		InputEvent wheel{};
		wheel.type = InputType::MouseWheel;
		wheel.t = timestamp;
		wheel.horizontal_wheel = hWheel;
		wheel.wheel_delta = SignedWheelDelta(mouse.usButtonData);
		Enqueue(std::move(wheel));
	}
}

void RawInputService::Enqueue(InputEvent&& event)
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	if (subscriptionBuffers_.empty()) {
		// 无订阅者时退回单一共享缓冲（保持原 Drain/Poll 语义）
		pendingEvents_.push_back(std::move(event));
		return;
	}
	// 扇出：每个订阅者各得一份拷贝（InputEvent 为平凡可拷贝的 POD 结构）
	for (auto& entry : subscriptionBuffers_) {
		entry.second.push_back(event);
	}
}

size_t RawInputService::Drain(std::vector<InputEvent>& out)
{
	std::vector<InputEvent> taken;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		taken.swap(pendingEvents_); // 整体交换，最小化持锁时间
	}
	const size_t count = taken.size();
	if (count > 0) {
		out.insert(out.end(),
		           std::make_move_iterator(taken.begin()),
		           std::make_move_iterator(taken.end()));
	}
	return count;
}

std::vector<InputEvent> RawInputService::Poll()
{
	std::vector<InputEvent> taken;
	Drain(taken);
	return taken;
}

RawInputService& RawInputService::Shared()
{
	static RawInputService instance;
	return instance;
}

RawInputService::SubscriptionId RawInputService::Subscribe()
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	const SubscriptionId id = nextSubscriptionId_++;
	subscriptionBuffers_.emplace(id, std::vector<InputEvent>{});
	return id;
}

void RawInputService::Unsubscribe(SubscriptionId id)
{
	if (id == kInvalidSubscription) {
		return;
	}
	std::lock_guard<std::mutex> lock(queueMutex_);
	subscriptionBuffers_.erase(id);
}

size_t RawInputService::Drain(SubscriptionId id, std::vector<InputEvent>& out)
{
	if (id == kInvalidSubscription) {
		return 0;
	}
	std::vector<InputEvent> taken;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		const auto it = subscriptionBuffers_.find(id);
		if (it == subscriptionBuffers_.end()) {
			return 0;
		}
		taken.swap(it->second); // 整体交换，最小化持锁时间
	}
	const size_t count = taken.size();
	if (count > 0) {
		out.insert(out.end(),
		           std::make_move_iterator(taken.begin()),
		           std::make_move_iterator(taken.end()));
	}
	return count;
}

std::vector<InputEvent> RawInputService::Poll(SubscriptionId id)
{
	std::vector<InputEvent> taken;
	Drain(id, taken);
	return taken;
}

} // namespace grt
