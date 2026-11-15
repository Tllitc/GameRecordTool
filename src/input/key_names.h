/**
 * @file key_names.h
 * @brief 虚拟键码 / 鼠标按键 -> keymap 规范名的统一转换。
 *
 * keymap（GameProfile.keymap）的键名空间由本模块唯一定义，Task 10 的
 * keymap 编辑器与 Task 6 的 ActionMapper 都必须使用这里的转换结果，
 * 保证两边对同一个物理输入生成完全相同的标识。
 *
 * 命名约定：
 *   - 键盘：字母数字为单字符大写（"A"、"5"），其余为 Pascal 命名
 *     （"Space"、"Enter"、"RShift"、"NumpadAdd"、"F1"），
 *     未列出的虚拟键使用 "VK_0xXX"；
 *   - 鼠标按键：leftClick / rightClick / middleClick / x1Click / x2Click；
 *   - 滚轮：wheelUp / wheelDown（垂直）、hWheelLeft / hWheelRight（水平）。
 *     注意滚轮名不对应 InputEvent.button，而由
 *     WheelEventName(horizontal, delta) 依据方向生成。
 *
 * 纯逻辑模块：仅依赖 <cstdint>/<string>，不包含 <windows.h>，
 * 输入为 input_event.h 中的规范常量，可在无 OBS 环境单测。
 */

#pragma once

#include <cstdint>
#include <string>

namespace grt {

/**
 * @brief 虚拟键码 -> 键盘规范名。
 * @param vkey Win32 虚拟键码（RAWKEYBOARD.VKey）。
 * @return 规范名；无对应命名的键返回 "VK_0xXX"（两位大写十六进制），永不返回空串。
 */
std::string KeyVkeyToName(uint16_t vkey);

/**
 * @brief 鼠标按键常量 -> 鼠标规范名。
 * @param button mouse_buttons 中的规范常量（kLeft/kRight/kMiddle/kXButton1/kXButton2）。
 * @return "leftClick" 等规范名；未知常量返回空串（调用方按未映射处理）。
 */
std::string MouseButtonToName(uint16_t button);

/**
 * @brief 滚轮事件 -> 滚轮规范名。
 * @param horizontal false=垂直滚轮（wheelUp/wheelDown），true=水平滚轮（hWheelLeft/hWheelRight）。
 * @param delta      带符号滚轮量（InputEvent.wheel_delta，一个 notch 通常为 ±120）。
 * @return 规范名；delta==0 返回空串。
 *
 * 命名方向按 Win32 符号约定：垂直 delta>0 向前滚（wheelUp）；
 * 水平 delta>0 向右（WM_MOUSEHWHEEL 消息已由系统按调用线程窗口做镜像翻转，
 * 原始 usButtonData 高位为负时为向左）。
 */
std::string WheelEventName(bool horizontal, int16_t delta);

/**
 * @brief 判断规范名是否为已知键盘名（含 "VK_0xXX" 形态）。
 *
 * 用于 keymap 编辑器/持久化校验：不能与鼠标名空间冲突即可，
 * 因此实现为"非空且不是鼠标名"。
 */
bool IsKeyboardName(const std::string& name);

/**
 * @brief 判断规范名是否为已知鼠标名（5 个按键名 + 4 个滚轮名）。
 */
bool IsMouseName(const std::string& name);

} // namespace grt
