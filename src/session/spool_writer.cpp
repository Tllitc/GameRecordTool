/**
 * @file spool_writer.cpp
 * @brief 原始事件 spool（<base>_raw.jsonl）写入实现。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "session/spool_writer.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

#include "core/log.h"
#include "input/key_names.h"

namespace grt {

namespace {

/// UTF-8 路径 -> UTF-16，供 _wfopen 使用（fopen 受 ANSI 代码页限制）
std::wstring Utf8ToWide(const std::string& utf8)
{
	if (utf8.empty()) {
		return std::wstring();
	}
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
	                                    static_cast<int>(utf8.size()),
	                                    nullptr, 0);
	std::wstring wide;
	if (len > 0) {
		wide.resize(static_cast<size_t>(len));
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
		                    static_cast<int>(utf8.size()),
		                    wide.data(), len);
	}
	return wide;
}

/// 把秒数格式化为固定 6 位小数字符串（足够 1 微秒分辨率，且输出稳定）
void AppendSeconds(std::string& out, double seconds)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.6f", seconds);
	out += buf;
}

/// 追加一个 JSON 字符串字段（含转义与外层引号）
void AppendStringField(std::string& out, const char* name, const std::string& value)
{
	out += '"';
	out += name;
	out += "\":\"";
	out += SpoolWriter::EscapeJson(value);
	out += '"';
}

} // namespace

SpoolWriter::SpoolWriter() = default;

SpoolWriter::~SpoolWriter()
{
	Close();
}

bool SpoolWriter::Open(const std::string& utf8Path)
{
	Close();

	const std::wstring widePath = Utf8ToWide(utf8Path);
	// _SH_DENYNO：允许其它进程（离线分析工具）在录制中读取本文件
	std::FILE* file = nullptr;
	if (_wfopen_s(&file, widePath.c_str(), L"wb, ccs=UTF-8") != 0 ||
	    file == nullptr) {
		GRT_LOG_ERROR("无法创建原始事件 spool 文件：%s（err=%s）",
		              utf8Path.c_str(), Win32ErrorMessage().c_str());
		return false;
	}

	// 设置 4KB 全缓冲；不设置时 MSVC 默认缓冲仅 512 字节
	if (setvbuf(file, buffer_, _IOFBF, kBufferBytes) != 0) {
		GRT_LOG_WARN("设置 spool 文件缓冲失败，使用默认缓冲：%s",
		             utf8Path.c_str());
	}

	file_ = file;
	path_ = utf8Path;
	eventCount_ = 0;
	hasFlushClock_ = false;
	lastFlushQpc_ = 0.0;
	return true;
}

void SpoolWriter::Flush()
{
	if (file_ != nullptr) {
		std::fflush(file_);
	}
}

void SpoolWriter::MaybeTimedFlush(double nowQpc)
{
	if (file_ == nullptr) {
		return;
	}
	if (!hasFlushClock_) {
		hasFlushClock_ = true;
		lastFlushQpc_ = nowQpc;
		return;
	}
	if (nowQpc - lastFlushQpc_ >= kFlushIntervalSeconds) {
		std::fflush(file_);
		lastFlushQpc_ = nowQpc;
	}
}

void SpoolWriter::Close()
{
	if (file_ != nullptr) {
		std::fflush(file_);
		std::fclose(file_);
		file_ = nullptr;
	}
	path_.clear();
}

bool SpoolWriter::WriteSessionStart(double qpc0, const std::string& startedAtUtc)
{
	std::string line = "{\"kind\":\"session_start\",\"qpc0\":";
	AppendSeconds(line, qpc0);
	line += ',';
	AppendStringField(line, "started_at_utc", startedAtUtc);
	line += '}';
	return WriteLine(line);
}

bool SpoolWriter::WriteEvent(const InputEvent& event, double sessionTime)
{
	const char* typeName = "unknown";
	std::string name; // 键盘/鼠标按键/滚轮规范名（仅相关类型填充）

	switch (event.type) {
		case InputType::Key:
			typeName = "key";
			name = KeyVkeyToName(event.vkey);
			break;
		case InputType::MouseButton:
			typeName = "mouse_button";
			name = MouseButtonToName(event.button);
			break;
		case InputType::MouseMove:
			typeName = "mouse_move";
			break;
		case InputType::MouseWheel:
			typeName = "mouse_wheel";
			name = WheelEventName(event.horizontal_wheel, event.wheel_delta);
			break;
	}

	std::string line = "{\"kind\":\"raw\",\"t\":";
	AppendSeconds(line, sessionTime);
	line += ",\"qpc\":";
	AppendSeconds(line, event.t);
	line += ",\"type\":\"";
	line += typeName;
	line += '"';

	switch (event.type) {
		case InputType::Key:
			line += ",\"vkey\":";
			line += std::to_string(static_cast<unsigned>(event.vkey));
			line += ",\"scan\":";
			line += std::to_string(static_cast<unsigned>(event.scan_code));
			line += ",\"e0\":";
			line += event.e0 ? "true" : "false";
			line += ",\"e1\":";
			line += event.e1 ? "true" : "false";
			line += ",\"down\":";
			line += event.key_down ? "true" : "false";
			line += ',';
			AppendStringField(line, "name", name);
			break;

		case InputType::MouseButton:
			line += ",\"button\":";
			line += std::to_string(static_cast<unsigned>(event.button));
			line += ",\"down\":";
			line += event.key_down ? "true" : "false";
			line += ',';
			AppendStringField(line, "name", name);
			break;

		case InputType::MouseMove:
			line += ",\"dx\":";
			line += std::to_string(event.dx);
			line += ",\"dy\":";
			line += std::to_string(event.dy);
			break;

		case InputType::MouseWheel:
			line += ",\"delta\":";
			line += std::to_string(static_cast<int>(event.wheel_delta));
			line += ",\"horizontal\":";
			line += event.horizontal_wheel ? "true" : "false";
			line += ',';
			AppendStringField(line, "name", name);
			break;
	}

	line += ",\"injected\":";
	line += event.injected ? "true" : "false";
	line += '}';

	if (!WriteLine(line)) {
		return false;
	}
	++eventCount_;
	return true;
}

std::string SpoolWriter::EscapeJson(const std::string& raw)
{
	std::string out;
	out.reserve(raw.size() + 2);
	for (const unsigned char ch : raw) {
		switch (ch) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20) {
					// 其余控制字符统一用 \u00XX 转义
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x",
					              static_cast<unsigned>(ch));
					out += buf;
				} else {
					// 0x20 及以上（含 UTF-8 多字节序列原样字节）直接输出
					out += static_cast<char>(ch);
				}
				break;
		}
	}
	return out;
}

bool SpoolWriter::WriteLine(const std::string& line)
{
	if (file_ == nullptr) {
		return false;
	}
	if (std::fwrite(line.data(), 1, line.size(), file_) != line.size() ||
	    std::fputc('\n', file_) == EOF) {
		GRT_LOG_ERROR("写入 spool 失败：%s（err=%s）",
		              path_.c_str(), Win32ErrorMessage().c_str());
		return false;
	}
	return true;
}

} // namespace grt
