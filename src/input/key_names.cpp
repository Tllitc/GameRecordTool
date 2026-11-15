/**
 * @file key_names.cpp
 * @brief 虚拟键码 / 鼠标按键 -> keymap 规范名转换实现。
 */

#include "input/key_names.h"

#include <array>
#include <cstdio>
#include <unordered_set>

#include <windows.h> // Win32 虚拟键码 VK_* 常量

#include "input/input_event.h"

namespace grt {

std::string KeyVkeyToName(uint16_t vkey)
{
	// 字母 A-Z / 数字 0-9：单字符大写
	if (vkey >= 'A' && vkey <= 'Z') {
		return std::string(1, static_cast<char>(vkey));
	}
	if (vkey >= '0' && vkey <= '9') {
		return std::string(1, static_cast<char>(vkey));
	}

	// 小键盘数字与运算符
	if (vkey >= VK_NUMPAD0 && vkey <= VK_NUMPAD9) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "Numpad%u",
		              static_cast<unsigned>(vkey - VK_NUMPAD0));
		return std::string(buf);
	}

	// 功能键 F1-F24
	if (vkey >= VK_F1 && vkey <= VK_F24) {
		char buf[8];
		std::snprintf(buf, sizeof(buf), "F%u",
		              static_cast<unsigned>(vkey - VK_F1 + 1));
		return std::string(buf);
	}

	switch (vkey) {
	// —— 控制键（通用侧）——
	case VK_BACK:       return "Backspace";
	case VK_TAB:        return "Tab";
	case VK_CLEAR:      return "Clear";
	case VK_RETURN:     return "Enter";
	case VK_SHIFT:      return "Shift";
	case VK_CONTROL:    return "Control";
	case VK_MENU:       return "Alt";
	case VK_PAUSE:      return "Pause";
	case VK_CAPITAL:    return "CapsLock";
	case VK_ESCAPE:     return "Escape";
	case VK_SPACE:      return "Space";

	// —— 导航 / 编辑区 ——
	case VK_PRIOR:      return "PageUp";
	case VK_NEXT:       return "PageDown";
	case VK_END:        return "End";
	case VK_HOME:       return "Home";
	case VK_LEFT:       return "Left";
	case VK_UP:         return "Up";
	case VK_RIGHT:      return "Right";
	case VK_DOWN:       return "Down";
	case VK_INSERT:     return "Insert";
	case VK_DELETE:     return "Delete";

	// —— 元键 / 系统键 ——
	case VK_LWIN:       return "LWin";
	case VK_RWIN:       return "RWin";
	case VK_APPS:       return "Apps";
	case VK_SLEEP:      return "Sleep";

	// —— 小键盘运算符 ——
	case VK_MULTIPLY:   return "NumpadMultiply";
	case VK_ADD:        return "NumpadAdd";
	case VK_SEPARATOR:  return "NumpadSeparator";
	case VK_SUBTRACT:   return "NumpadSubtract";
	case VK_DECIMAL:    return "NumpadDecimal";
	case VK_DIVIDE:     return "NumpadDivide";

	// —— 状态灯键 ——
	case VK_NUMLOCK:    return "NumLock";
	case VK_SCROLL:     return "ScrollLock";

	// —— 左右侧修饰键 ——
	case VK_LSHIFT:     return "LShift";
	case VK_RSHIFT:     return "RShift";
	case VK_LCONTROL:   return "LControl";
	case VK_RCONTROL:   return "RControl";
	case VK_LMENU:      return "LAlt";
	case VK_RMENU:      return "RAlt";

	default:
		break;
	}

	// 未列入命名表的虚拟键（含 OEM 标点、IME、多媒体键等）使用统一形态
	char fallback[16];
	std::snprintf(fallback, sizeof(fallback), "VK_0x%02X",
	              static_cast<unsigned>(vkey));
	return std::string(fallback);
}

std::string MouseButtonToName(uint16_t button)
{
	switch (button) {
		case mouse_buttons::kLeft:    return "leftClick";
		case mouse_buttons::kRight:   return "rightClick";
		case mouse_buttons::kMiddle:  return "middleClick";
		case mouse_buttons::kXButton1: return "x1Click";
		case mouse_buttons::kXButton2: return "x2Click";
		default: return std::string();
	}
}

std::string WheelEventName(bool horizontal, int16_t delta)
{
	if (delta == 0) {
		return std::string();
	}
	if (horizontal) {
		// Win32：水平滚轮向右 delta>0，向左 delta<0
		return delta > 0 ? "hWheelRight" : "hWheelLeft";
	}
	// 垂直滚轮：向前（远离用户）delta>0
	return delta > 0 ? "wheelUp" : "wheelDown";
}

bool IsMouseName(const std::string& name)
{
	// 5 个按键名 + 4 个滚轮名，共 9 个规范标识
	static const std::unordered_set<std::string> kMouseNames = {
		"leftClick", "rightClick", "middleClick", "x1Click", "x2Click",
		"wheelUp", "wheelDown", "hWheelLeft", "hWheelRight"
	};
	return kMouseNames.find(name) != kMouseNames.end();
}

bool IsKeyboardName(const std::string& name)
{
	// 键盘名空间开放（未知名以 VK_0xXX 兜底），唯一约束是不能与鼠标名冲突
	return !name.empty() && !IsMouseName(name);
}

} // namespace grt
