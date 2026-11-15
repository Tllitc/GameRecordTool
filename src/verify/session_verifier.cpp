/**
 * @file session_verifier.cpp
 * @brief 录制结束后三件套自动校验实现（Task 12）。
 *
 * 视频参数探测两级策略：
 *   1. 调用 ffprobe（需在 PATH 或传入路径），解析其 section-free 文本输出；
 *   2. ffprobe 不可用时使用内置最小 ISO-BMFF（mp4/fMP4）解析器，仅遍历
 *      moov/trak/mdia 元数据取 hdlr/mdhd/stsd/stts，不做任何媒体解码。
 *
 * 操作数据 JSON 使用本文件内的最小递归下降 JSON 解析器（只支持本工具
 * 导出的严格 JSON 子集），因此整个模块不依赖 Qt/OBS，可直接在后台线程或
 * 单元测试中调用。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "verify/session_verifier.h"

namespace grt {

namespace {

// ===========================================================================
// 通用辅助
// ===========================================================================

std::wstring Utf8ToWide(std::string_view utf8)
{
	if (utf8.empty()) {
		return {};
	}
	const int len = MultiByteToWideChar(
	    CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (len <= 0) {
		return {};
	}
	std::wstring wide(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
	                    static_cast<int>(utf8.size()), wide.data(), len);
	return wide;
}

std::string WideToUtf8(std::wstring_view wide)
{
	if (wide.empty()) {
		return {};
	}
	const int len = WideCharToMultiByte(
	    CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
	    nullptr, nullptr);
	if (len <= 0) {
		return {};
	}
	std::string utf8(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(),
	                    static_cast<int>(wide.size()), utf8.data(), len,
	                    nullptr, nullptr);
	return utf8;
}

/// 取路径所在目录（含末尾反斜杠）
std::wstring ParentDir(std::wstring path)
{
	if (path.empty()) {
		return {};
	}
	if (path.back() == L'\\' || path.back() == L'/') {
		path.pop_back();
	}
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring()
	                                   : path.substr(0, slash + 1);
}

bool Exists(const std::wstring& path)
{
	const DWORD attrs = GetFileAttributesW(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES &&
	       (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool FileExists(const std::string& path)
{
	if (path.empty()) {
		return false;
	}
	const DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
	return attrs != INVALID_FILE_ATTRIBUTES &&
	       (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ReadWholeFile(const std::string& path, std::string* out)
{
	// 路径在此模块内统一为 UTF-8；MSVC 的窄字符流重载按 ANSI 代码页
	// （中文系统为 CP936）解释文件名，含中文/非 ASCII 字符的会话目录会
	// 打不开文件——而 FileExists 走宽字符 API 判定为存在，于是表现为
	// “文件存在却无法读取”。故先转 UTF-16 再以宽字符原生路径打开。
	std::ifstream file(std::filesystem::path(Utf8ToWide(path)),
	                   std::ios::binary);
	if (!file) {
		return false;
	}
	std::ostringstream ss;
	ss << file.rdbuf();
	*out = ss.str();
	return file.good() || file.eof();
}

std::string BaseName(const std::string& path)
{
	const size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// 去掉首尾空白（ASCII）
std::string_view Trim(std::string_view text)
{
	while (!text.empty() &&
	       (text.front() == ' ' || text.front() == '\t' ||
	        text.front() == '\r' || text.front() == '\n')) {
		text.remove_prefix(1);
	}
	while (!text.empty() &&
	       (text.back() == ' ' || text.back() == '\t' ||
	        text.back() == '\r' || text.back() == '\n')) {
		text.remove_suffix(1);
	}
	return text;
}

bool StartsWith(std::string_view text, std::string_view prefix)
{
	return text.size() >= prefix.size() &&
	       text.compare(0, prefix.size(), prefix) == 0;
}

double ParseDouble(std::string_view text, double fallback = 0.0)
{
	double value = fallback;
	const std::string tmp(text);
	const char* first = tmp.c_str();
	char* end = nullptr;
	value = std::strtod(first, &end);
	return end == first ? fallback : value;
}

long long ParseLL(std::string_view text, long long fallback = 0)
{
	long long value = fallback;
	const std::string tmp(text);
	const char* first = tmp.c_str();
	char* end = nullptr;
	value = std::strtoll(first, &end, 10);
	return end == first ? fallback : value;
}

// ===========================================================================
// 最小 JSON 解析器（支持 object/array/string/number/bool/null）
// ===========================================================================

struct JsonValue {
	enum class Type { Null, Bool, Number, String, Array, Object } type =
	    Type::Null;
	bool boolean = false;
	double number = 0.0;
	std::string str;
	std::vector<JsonValue> array;
	std::map<std::string, JsonValue> object;

	const JsonValue* Find(std::string_view key) const
	{
		if (type != Type::Object) {
			return nullptr;
		}
		const auto it = object.find(std::string(key));
		return it == object.end() ? nullptr : &it->second;
	}

	std::string GetString(std::string_view key,
	                      const std::string& fallback = {}) const
	{
		const JsonValue* v = Find(key);
		return (v != nullptr && v->type == Type::String) ? v->str : fallback;
	}

	double GetNumber(std::string_view key, double fallback = 0.0) const
	{
		const JsonValue* v = Find(key);
		return (v != nullptr && v->type == Type::Number) ? v->number
		                                                 : fallback;
	}
};

class JsonParser {
public:
	explicit JsonParser(std::string_view text) : s_(text) {}

	bool Parse(JsonValue* out)
	{
		SkipWs();
		if (!ParseValue(out)) {
			return false;
		}
		SkipWs();
		return pos_ == s_.size(); // 不允许尾部垃圾
	}

private:
	bool Eof() const { return pos_ >= s_.size(); }
	char Peek() const { return Eof() ? '\0' : s_[pos_]; }

	void SkipWs()
	{
		while (!Eof()) {
			const char c = s_[pos_];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
				++pos_;
			} else {
				break;
			}
		}
	}

	bool Consume(char c)
	{
		SkipWs();
		if (Peek() == c) {
			++pos_;
			return true;
		}
		return false;
	}

	bool ParseValue(JsonValue* out)
	{
		SkipWs();
		if (Eof()) {
			return false;
		}
		switch (Peek()) {
		case '{':
			return ParseObject(out);
		case '[':
			return ParseArray(out);
		case '"': {
			out->type = JsonValue::Type::String;
			return ParseString(&out->str);
		}
		case 't':
		case 'f':
			return ParseBool(out);
		case 'n':
			return ParseNull(out);
		default:
			return ParseNumber(out);
		}
	}

	bool ParseObject(JsonValue* out)
	{
		out->type = JsonValue::Type::Object;
		++pos_; // '{'
		SkipWs();
		if (Consume('}')) {
			return true;
		}
		while (true) {
			SkipWs();
			std::string key;
			if (Peek() != '"' || !ParseString(&key)) {
				return false;
			}
			if (!Consume(':')) {
				return false;
			}
			JsonValue value;
			if (!ParseValue(&value)) {
				return false;
			}
			out->object[std::move(key)] = std::move(value);
			SkipWs();
			if (Consume(',')) {
				continue;
			}
			return Consume('}');
		}
	}

	bool ParseArray(JsonValue* out)
	{
		out->type = JsonValue::Type::Array;
		++pos_; // '['
		SkipWs();
		if (Consume(']')) {
			return true;
		}
		while (true) {
			JsonValue value;
			if (!ParseValue(&value)) {
				return false;
			}
			out->array.push_back(std::move(value));
			SkipWs();
			if (Consume(',')) {
				continue;
			}
			return Consume(']');
		}
	}

	bool ParseString(std::string* out)
	{
		if (Peek() != '"') {
			return false;
		}
		++pos_;
		while (!Eof()) {
			const char c = s_[pos_++];
			if (c == '"') {
				return true;
			}
			if (c == '\\') {
				if (Eof()) {
					return false;
				}
				const char esc = s_[pos_++];
				switch (esc) {
				case '"':
				case '\\':
				case '/':
					out->push_back(esc);
					break;
				case 'b':
					out->push_back('\b');
					break;
				case 'f':
					out->push_back('\f');
					break;
				case 'n':
					out->push_back('\n');
					break;
				case 'r':
					out->push_back('\r');
					break;
				case 't':
					out->push_back('\t');
					break;
				case 'u': {
					if (pos_ + 4 > s_.size()) {
						return false;
					}
					unsigned code = 0;
					for (int i = 0; i < 4; ++i) {
						const char h = s_[pos_++];
						code <<= 4;
						if (h >= '0' && h <= '9') {
							code |= static_cast<unsigned>(h - '0');
						} else if (h >= 'a' && h <= 'f') {
							code |= static_cast<unsigned>(h - 'a' + 10);
						} else if (h >= 'A' && h <= 'F') {
							code |= static_cast<unsigned>(h - 'A' + 10);
						} else {
							return false;
						}
					}
					// 仅 BMP 范围；keymap 键名/动作文案都在 BMP 内，
					// 代理对按 UTF-16 原样追加（不影响键存在性校验）
					if (code < 0x80) {
						out->push_back(static_cast<char>(code));
					} else if (code < 0x800) {
						out->push_back(static_cast<char>(0xC0 | (code >> 6)));
						out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
					} else {
						out->push_back(
						    static_cast<char>(0xE0 | (code >> 12)));
						out->push_back(
						    static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
						out->push_back(
						    static_cast<char>(0x80 | (code & 0x3F)));

					}
					break;
				}
				default:
					return false;
				}
			} else {
				out->push_back(c);
			}
		}
		return false;
	}

	bool ParseBool(JsonValue* out)
	{
		if (StartsWith(s_.substr(pos_), "true")) {
			pos_ += 4;
			out->type = JsonValue::Type::Bool;
			out->boolean = true;
			return true;
		}
		if (StartsWith(s_.substr(pos_), "false")) {
			pos_ += 5;
			out->type = JsonValue::Type::Bool;
			out->boolean = false;
			return true;
		}
		return false;
	}

	bool ParseNull(JsonValue* out)
	{
		if (StartsWith(s_.substr(pos_), "null")) {
			pos_ += 4;
			out->type = JsonValue::Type::Null;
			return true;
		}
		return false;
	}

	bool ParseNumber(JsonValue* out)
	{
		const size_t start = pos_;
		if (Peek() == '-') {
			++pos_;
		}
		while (!Eof()) {
			const char c = Peek();
			if ((c >= '0' && c <= '9') || c == '.' || c == 'e' ||
			    c == 'E' || c == '+' || c == '-') {
				++pos_;
			} else {
				break;
			}
		}
		if (pos_ == start) {
			return false;
		}
		out->type = JsonValue::Type::Number;
		out->number = ParseDouble(s_.substr(start, pos_ - start));
		return true;
	}

	std::string_view s_;
	size_t pos_ = 0;
};

bool ParseJsonFile(const std::string& path, JsonValue* root,
                   std::string* error)
{
	std::string content;
	if (!ReadWholeFile(path, &content)) {
		*error = "无法读取文件：" + BaseName(path);
		return false;
	}
	JsonParser parser(content);
	if (!parser.Parse(root)) {
		*error = "JSON 无法解析：" + BaseName(path);
		return false;
	}
	return true;
}

// ===========================================================================
// ffprobe 子进程
// ===========================================================================

/// 捕获子进程 stdout（UTF-8）；启动/退出失败返回 false。超时毫秒按
/// WaitForSingleObject 轮询实现，避免大文件时长时间卡住 UI 后台任务。
bool RunProcessCapture(const std::wstring& commandLine, int timeoutMs,
                       std::string* output)
{
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE readPipe = nullptr;
	HANDLE writePipe = nullptr;
	if (CreatePipe(&readPipe, &writePipe, &sa, 0) == 0) {
		return false;
	}
	// 读端不能继承，否则子进程退出后管道不会 EOF
	SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = writePipe;
	si.hStdError = writePipe;
	PROCESS_INFORMATION pi{};

	std::wstring mutableCmd = commandLine;
	const BOOL started = CreateProcessW(
	    nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
	    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(writePipe);
	if (!started) {
		CloseHandle(readPipe);
		return false;
	}

	std::string captured;
	std::array<char, 4096> buffer{};
	const DWORD startTick = GetTickCount();
	for (;;) {
		DWORD avail = 0;
		if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail,
		                  nullptr) == 0) {
			break;
		}
		if (avail > 0) {
			DWORD read = 0;
			const DWORD toRead =
			    std::min<DWORD>(avail,
			                    static_cast<DWORD>(buffer.size()));
			if (ReadFile(readPipe, buffer.data(), toRead, &read,
			             nullptr) == 0 ||
			    read == 0) {
				break;
			}
			captured.append(buffer.data(), read);
		} else {
			const DWORD exit = WaitForSingleObject(pi.hProcess, 0);
			if (exit == WAIT_OBJECT_0) {
				// 排空剩余输出
				for (;;) {
					DWORD read = 0;
					if (ReadFile(readPipe, buffer.data(),
					             static_cast<DWORD>(buffer.size()),
					             &read, nullptr) == 0 ||
					    read == 0) {
						break;
					}
					captured.append(buffer.data(), read);
				}
				break;
			}
			if (GetTickCount() - startTick >
			    static_cast<DWORD>(timeoutMs)) {
				TerminateProcess(pi.hProcess, 1);
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
				CloseHandle(readPipe);
				return false;
			}
			Sleep(10);
		}
	}

	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	CloseHandle(readPipe);
	*output = std::move(captured);
	return exitCode == 0;
}

/// 命令行参数加双引号（路径无内嵌双引号，简单转义即可）
std::wstring QuoteArg(const std::string& utf8)
{
	std::wstring wide = Utf8ToWide(utf8);
	return L"\"" + wide + L"\"";
}

/**
 * @brief 用 ffprobe 探测视频。
 *
 * 使用 section-free（-noflags noprint_wrappers / nokey 关闭）输出，每行
 * "key=value"，避免解析 INI 分段；取流与格式两组字段的最后一次出现值。
 */
bool ProbeWithFfprobe(const std::string& ffprobePath,
                      const std::string& videoPath, VideoProbeInfo* info)
{
	// codec_name/width/height/avg_frame_rate/duration/bit_rate
	const std::wstring cmd =
	    QuoteArg(ffprobePath) +
	    L" -v error -select_streams v:0 -show_entries"
	    L" stream=codec_name,width,height,avg_frame_rate,duration,bit_rate"
	    L" -show_entries format=duration,bit_rate"
	    L" -of default=noprint_wrappers=1:nokey=0 -- " +
	    QuoteArg(videoPath);

	std::string output;
	if (!RunProcessCapture(cmd, 10000, &output) || output.empty()) {
		return false;
	}

	std::map<std::string, std::string> kv;
	{
		std::istringstream ss(output);
		std::string line;
		while (std::getline(ss, line)) {
			const std::string_view t = Trim(line);
			const size_t eq = t.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			kv[std::string(Trim(t.substr(0, eq)))] =
			    std::string(Trim(t.substr(eq + 1)));
		}
	}

	auto get = [&](const char* key) -> std::string {
		const auto it = kv.find(key);
		return it == kv.end() ? std::string() : it->second;
	};

	const std::string codec = get("codec_name");
	info->codec = codec;
	info->width = static_cast<int>(ParseLL(get("width")));
	info->height = static_cast<int>(ParseLL(get("height")));

	// avg_frame_rate 形如 "60000/1001" 或 "60/1"
	const std::string frameRate = get("avg_frame_rate");
	if (!frameRate.empty()) {
		const size_t slash = frameRate.find('/');
		if (slash != std::string::npos) {
			const double num =
			    ParseDouble(frameRate.substr(0, slash), 0.0);
			const double den =
			    ParseDouble(frameRate.substr(slash + 1), 0.0);
			if (den > 0.0) {
				info->fps = num / den;
			}
		} else {
			info->fps = ParseDouble(frameRate);
		}
	}

	// default 输出中 stream 段在前、format 段在后，同名 key 经 map 保留
	// 最后一次出现值，因此 duration/bit_rate 取到的即 format 段值。
	info->durationSeconds = ParseDouble(get("duration"));
	long long bitrate = ParseLL(get("bit_rate"));
	if (bitrate <= 0) {
		// 文件大小兜底
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if (GetFileAttributesExW(Utf8ToWide(videoPath).c_str(),
		                         GetFileExInfoStandard, &fad) &&
		    info->durationSeconds > 0.0) {
			ULARGE_INTEGER size;
			size.LowPart = fad.nFileSizeLow;
			size.HighPart = fad.nFileSizeHigh;
			bitrate = static_cast<long long>(
			    8.0 * static_cast<double>(size.QuadPart) /
			    info->durationSeconds);
		}
	}
	info->bitrateBps = bitrate;
	info->available = info->width > 0 && info->height > 0 &&
	                  info->durationSeconds > 0.0;
	info->probeBackend = "ffprobe";
	return info->available;
}

// ===========================================================================
// 内置最小 ISO-BMFF 解析器
// ===========================================================================

class Mp4Reader {
public:
	explicit Mp4Reader(const std::string& path)
	{
		// 同 ReadWholeFile：宽字符原生路径，避免含中文的会话目录打不开
		file_.open(std::filesystem::path(Utf8ToWide(path)),
		           std::ios::binary);
		if (file_) {
			file_.seekg(0, std::ios::end);
			length_ = static_cast<uint64_t>(file_.tellg());
			file_.seekg(0, std::ios::beg);
		}
	}

	bool Good() const { return static_cast<bool>(file_); }
	uint64_t Length() const { return length_; }

	bool ReadAt(uint64_t offset, void* dst, size_t size)
	{
		if (offset > length_ || size > length_ - offset) {
			return false;
		}
		file_.clear();
		file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		file_.read(static_cast<char*>(dst),
		           static_cast<std::streamsize>(size));
		return static_cast<bool>(file_);
	}

private:
	std::ifstream file_;
	uint64_t length_ = 0;
};

uint32_t BeU32(const uint8_t* p)
{
	return (static_cast<uint32_t>(p[0]) << 24) |
	       (static_cast<uint32_t>(p[1]) << 16) |
	       (static_cast<uint32_t>(p[2]) << 8) |
	       static_cast<uint32_t>(p[3]);
}

uint16_t BeU16(const uint8_t* p)
{
	return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
	                             static_cast<uint16_t>(p[1]));
}

uint64_t BeU64(const uint8_t* p)
{
	return (static_cast<uint64_t>(BeU32(p)) << 32) |
	       static_cast<uint64_t>(BeU32(p + 4));
}

struct Box {
	char type[5] = {};
	uint64_t header = 0;   // 头长度
	uint64_t start = 0;    // 盒起点（含头）
	uint64_t size = 0;     // 整盒大小（含头）
	uint64_t DataStart() const { return start + header; }
	uint64_t DataSize() const { return size > header ? size - header : 0; }
};

/// 读取 offset 处的盒头；文件尾返回 false
bool ReadBoxHeader(Mp4Reader& reader, uint64_t offset, uint64_t parentEnd,
                   Box* box)
{
	if (offset + 8 > parentEnd) {
		return false;
	}
	uint8_t head[8];
	if (!reader.ReadAt(offset, head, 8)) {
		return false;
	}
	uint32_t size32 = BeU32(head);
	memcpy(box->type, head + 4, 4);
	box->type[4] = '\0';
	box->start = offset;
	box->header = 8;
	if (size32 == 1) {
		uint8_t ext[8];
		if (offset + 16 > parentEnd || !reader.ReadAt(offset + 8, ext, 8)) {
			return false;
		}
		box->size = BeU64(ext);
		box->header = 16;
	} else if (size32 == 0) {
		box->size = parentEnd - offset;
	} else {
		box->size = size32;
	}
	return box->size >= box->header &&
	       box->start + box->size <= parentEnd;
}

/**
 * @brief 在 [start,end) 内遍历直接子盒。
 * @return 返回 false 时 visitor 可提前终止遍历。
 */
template <typename Fn>
void ForEachChildBox(Mp4Reader& reader, uint64_t start, uint64_t end,
                     Fn&& visitor)
{
	uint64_t offset = start;
	while (offset + 8 <= end) {
		Box box;
		if (!ReadBoxHeader(reader, offset, end, &box)) {
			break;
		}
		if (box.size == 0) {
			break;
		}
		visitor(box);
		offset = box.start + box.size;
	}
}

/// 读整块（限制上限，防异常 size 爆内存）
bool ReadBoxData(Mp4Reader& reader, const Box& box, size_t maxSize,
                 std::vector<uint8_t>* out)
{
	const uint64_t dataSize = box.DataSize();
	if (dataSize > maxSize) {
		return false;
	}
	out->resize(static_cast<size_t>(dataSize));
	return dataSize == 0 ||
	       reader.ReadAt(box.DataStart(), out->data(),
	                     static_cast<size_t>(dataSize));
}

/// 在容器盒中按类型路径定位第一个子盒（路径如 {"mdia","minf","stbl"}）
bool FindBoxPath(Mp4Reader& reader, const Box& container,
                 std::initializer_list<const char*> path, Box* found)
{
	Box current = container;
	for (const char* wanted : path) {
		bool hit = false;
		ForEachChildBox(reader, current.DataStart(),
		                current.start + current.size,
		                [&](const Box& child) {
			                if (!hit && strcmp(child.type, wanted) == 0) {
				                current = child;
				                hit = true;
			                }
		                });
		if (!hit) {
			return false;
		}
	}
	*found = current;
	return true;
}

/// mdhd：version(1)+flags(3)，v1 为 64 位创建/修改时间与 timescale(32)/dur(64)
bool ParseMdhd(const std::vector<uint8_t>& d, uint32_t* timescale,
               uint64_t* duration)
{
	if (d.size() < 20) {
		return false;
	}
	const uint8_t version = d[0];
	size_t pos = 4;
	if (version == 1) {
		if (d.size() < 4 + 8 + 8 + 4 + 8) {
			return false;
		}
		pos += 8 + 8; // creation/modification
		*timescale = BeU32(d.data() + pos);
		pos += 4;
		*duration = BeU64(d.data() + pos);
	} else {
		if (d.size() < 4 + 4 + 4 + 4 + 4) {
			return false;
		}
		pos += 4 + 4;
		*timescale = BeU32(d.data() + pos);
		pos += 4;
		*duration = BeU32(d.data() + pos);
	}
	return *timescale > 0;
}

/**
 * @brief stsd 解析视觉样本条目：跳过 entry_count 后读取第一个 sample entry。
 *
 * sample entry 自身是带 8 字节盒头（size + fourcc）的 box；其 payload 为
 * VisualSampleEntry（78 字节：6 保留 + 2 data_reference_index + 16 预定义/
 * 保留 + width/height 在 payload 偏移 24/26 处）。因此相对盒起点，fourcc
 * 在 +4、width/height 在 +32/+34。
 */
bool ParseStsdVideo(const std::vector<uint8_t>& d, std::string* fourcc,
                    int* width, int* height)
{
	// 8（version/flags+entry_count）+ 8（sample entry 盒头）+ 78（payload）
	if (d.size() < 8 + 8 + 78) {
		return false;
	}
	// version/flags(4) + entry_count(4)，随后是 sample entry box 起点
	const uint8_t* e = d.data() + 8;
	char codec[5] = {};
	memcpy(codec, e + 4, 4);
	codec[4] = '\0';
	*fourcc = codec;
	*width = BeU16(e + 8 + 24);
	*height = BeU16(e + 8 + 26);
	return *width > 0 && *height > 0;
}

/**
 * @brief stts：(version/flags 4) + entry_count(4) + N*(sample_count(4),
 *        sample_delta(4))。返回样本总数与 delta 总和。
 */
bool ParseStts(const std::vector<uint8_t>& d, uint64_t* sampleCount,
               uint64_t* deltaSum)
{
	if (d.size() < 8) {
		return false;
	}
	const uint32_t entries = BeU32(d.data() + 4);
	if (d.size() < 8 + static_cast<size_t>(entries) * 8) {
		return false;
	}
	uint64_t count = 0;
	uint64_t deltas = 0;
	for (uint32_t i = 0; i < entries; ++i) {
		const uint8_t* p = d.data() + 8 + static_cast<size_t>(i) * 8;
		count += BeU32(p);
		deltas += static_cast<uint64_t>(BeU32(p)) *
		          static_cast<uint64_t>(BeU32(p + 4));
	}
	*sampleCount = count;
	*deltaSum = deltas;
	return true;
}

/// hdlr handler_type 位于 payload 偏移 8（version/flags 4 + pre_defined 4）
bool IsVideoHdlr(const std::vector<uint8_t>& d)
{
	return d.size() >= 12 && memcmp(d.data() + 8, "vide", 4) == 0;
}

/// 遍历单个 trak，判定视频轨并取参数
bool ParseVideoTrak(Mp4Reader& reader, const Box& trak,
                    VideoProbeInfo* info)
{
	Box mdia;
	if (!FindBoxPath(reader, trak, {"mdia"}, &mdia)) {
		return false;
	}

	// hdlr
	Box hdlr;
	if (!FindBoxPath(reader, mdia, {"hdlr"}, &hdlr)) {
		return false;
	}
	std::vector<uint8_t> hdlrData;
	if (!ReadBoxData(reader, hdlr, 4096, &hdlrData) ||
	    !IsVideoHdlr(hdlrData)) {
		return false;
	}

	// mdhd
	Box mdhd;
	uint32_t timescale = 0;
	uint64_t duration = 0;
	if (FindBoxPath(reader, mdia, {"mdhd"}, &mdhd)) {
		std::vector<uint8_t> mdhdData;
		if (ReadBoxData(reader, mdhd, 1 * 1024 * 1024, &mdhdData)) {
			ParseMdhd(mdhdData, &timescale, &duration);
		}
	}

	// minf/stbl/stsd + stts
	Box stsd;
	Box stts;
	if (!FindBoxPath(reader, mdia, {"minf", "stbl", "stsd"}, &stsd) ||
	    !FindBoxPath(reader, mdia, {"minf", "stbl", "stts"}, &stts)) {
		return false;
	}
	std::vector<uint8_t> stsdData;
	std::vector<uint8_t> sttsData;
	if (!ReadBoxData(reader, stsd, 16 * 1024 * 1024, &stsdData) ||
	    !ReadBoxData(reader, stts, 16 * 1024 * 1024, &sttsData)) {
		return false;
	}
	std::string fourcc;
	int width = 0;
	int height = 0;
	if (!ParseStsdVideo(stsdData, &fourcc, &width, &height)) {
		return false;
	}

	uint64_t sampleCount = 0;
	uint64_t deltaSum = 0;
	ParseStts(sttsData, &sampleCount, &deltaSum);

	info->width = width;
	info->height = height;

	std::string lower = fourcc;
	std::transform(lower.begin(), lower.end(), lower.begin(),
	               [](unsigned char c) {
		               return static_cast<char>(std::tolower(c));
	               });
	info->codec = lower;

	// 时长：优先 mdhd；退化用 stts delta 总和 / timescale
	double seconds = 0.0;
	if (timescale > 0) {
		if (duration > 0) {
			seconds = static_cast<double>(duration) /
			          static_cast<double>(timescale);
		} else if (sampleCount > 0) {
			seconds = static_cast<double>(deltaSum) /
			          static_cast<double>(timescale);
		}
	}
	info->durationSeconds = seconds;

	if (seconds > 0.0 && sampleCount > 0) {
		info->fps = static_cast<double>(sampleCount) / seconds;
	}

	// 平均码率：文件大小 × 8 / 时长（含容器开销，比裸流略高，对门槛
	// 判定是安全方向；OBS VBR 文件无 mdhd 码率字段）
	if (seconds > 0.0) {
		info->bitrateBps = static_cast<long long>(
		    8.0 * static_cast<double>(reader.Length()) / seconds);
	}

	info->available = width > 0 && height > 0 && seconds > 0.0;
	info->probeBackend = "内置MP4解析";
	return info->available;
}

/// 内置解析器入口：定位 moov，遍历 trak 找视频轨
bool ProbeBuiltIn(const std::string& videoPath, VideoProbeInfo* info)
{
	Mp4Reader reader(videoPath);
	if (!reader.Good()) {
		return false;
	}

	bool sawFtyp = false;
	Box moov{};
	bool haveMoov = false;
	ForEachChildBox(reader, 0, reader.Length(), [&](const Box& box) {
		if (strcmp(box.type, "ftyp") == 0) {
			sawFtyp = true;
		}
		if (strcmp(box.type, "moov") == 0 && !haveMoov) {
			moov = box;
			haveMoov = true;
		}
	});
	if (!haveMoov) {
		return false;
	}

	bool found = false;
	ForEachChildBox(reader, moov.DataStart(), moov.start + moov.size,
	                [&](const Box& trak) {
		                if (!found && strcmp(trak.type, "trak") == 0) {
			                VideoProbeInfo candidate;
			                if (ParseVideoTrak(reader, trak, &candidate)) {
				                *info = candidate;
				                found = true;
			                }
		                }
	                });
	(void)sawFtyp;
	return found;
}

// ===========================================================================
// 门槛与字段校验
// ===========================================================================

/// 按短边/长边归入分辨率档，返回该档码率下限 bps；低于 1080p 返回 0
long long MinBitrateForResolution(int width, int height)
{
	const int longSide = std::max(width, height);
	const int shortSide = std::min(width, height);
	// 16:9 参考：3840×2160=4K，2560×1440=1440p，1920×1080=1080p。
	// 允许少量裁切：以长边为主、短边兜底。
	if (longSide >= 3800 || shortSide >= 2100) {
		return 35'000'000;
	}
	if (longSide >= 2500 || shortSide >= 1400) {
		return 24'000'000;
	}
	if (longSide >= 1900 || shortSide >= 1050) {
		return 8'000'000;
	}
	return 0; // 低于 1080p
}

std::string CodecDisplayName(const std::string& rawCodec)
{
	if (rawCodec == "h264" || rawCodec == "avc1") {
		return "H.264 (AVC)";
	}
	if (rawCodec == "hevc" || rawCodec == "h265" || rawCodec == "hvc1" ||
	    rawCodec == "hev1") {
		return "H.265 (HEVC)";
	}
	return rawCodec.empty() ? "未知" : rawCodec;
}

bool IsH264OrH265(const std::string& codec)
{
	return codec == "h264" || codec == "avc1" || codec == "hevc" ||
	       codec == "h265" || codec == "hvc1" || codec == "hev1";
}

const std::array<const char*, 8> kValidDirections = {
    "W", "B", "L", "R", "WL", "WR", "BL", "BR"};

bool IsValidDirection(const std::string& dir)
{
	return std::any_of(kValidDirections.begin(), kValidDirections.end(),
	                   [&](const char* d) { return dir == d; });
}

/// 取数组（类型必须为数组）
const std::vector<JsonValue>* AsArray(const JsonValue& root,
                                      const char* key)
{
	const JsonValue* v = root.Find(key);
	if (v == nullptr || v->type != JsonValue::Type::Array) {
		return nullptr;
	}
	return &v->array;
}

void AddError(VerifyReport* report, const std::string& code,
              std::string message)
{
	report->issues.push_back(VerifyIssue{true, code, std::move(message)});
}

void AddWarning(VerifyReport* report, const std::string& code,
                std::string message)
{
	report->issues.push_back(VerifyIssue{false, code, std::move(message)});
}

/**
 * @brief duration_seconds 与视频时长的允许偏差（秒）。
 *        对齐二期需求 4.4 的 0.1s 口径；事件越界容差同值。
 */
constexpr double kDurationToleranceSeconds = 0.1;

/**
 * @brief 校验三个事件数组：字段齐全、数值有限、start<end、非负、按时间有序。
 * looking 额外允许 dx/dy 为任意有限值；区间超出视频时长仅警告。
 */
void ValidateEventArrays(const JsonValue& root, double videoDuration,
                         VerifyReport* report)
{
	const auto* movements = AsArray(root, "movement_events");
	const auto* looking = AsArray(root, "looking_events");
	const auto* actions = AsArray(root, "action_events");
	if (movements == nullptr) {
		AddError(report, "data.movement_type",
		         "movement_events 缺失或不是数组");
	}
	if (looking == nullptr) {
		AddError(report, "data.looking_type",
		         "looking_events 缺失或不是数组");
	}
	if (actions == nullptr) {
		AddError(report, "data.action_type",
		         "action_events 缺失或不是数组");
	}
	if (movements == nullptr || looking == nullptr ||
	    actions == nullptr) {
		return;
	}

	report->movementCount = movements->size();
	report->lookingCount = looking->size();
	report->actionCount = actions->size();

	// movement：direction/start_time/end_time
	double prevEnd = -1.0;
	for (size_t i = 0; i < movements->size(); ++i) {
		const JsonValue& e = (*movements)[i];
		const JsonValue* start = e.Find("start_time");
		const JsonValue* end = e.Find("end_time");
		const std::string dir = e.GetString("direction");
		const std::string where =
		    "movement_events[" + std::to_string(i) + "]";
		if (start == nullptr || start->type != JsonValue::Type::Number ||
		    end == nullptr || end->type != JsonValue::Type::Number) {
			AddError(report, "data.movement_fields",
			         where + " 缺少数值型 start_time/end_time");
			prevEnd = -1.0;
			continue;
		}
		if (!IsValidDirection(dir)) {
			AddError(report, "data.movement_direction",
			         where + " direction 非法：" +
			             (dir.empty() ? "(空)" : dir));
		}
		if (!std::isfinite(start->number) ||
		    !std::isfinite(end->number)) {
			AddError(report, "data.movement_finite",
			         where + " 时间含 NaN/Inf");
		} else if (!(start->number < end->number)) {
			AddError(report, "data.movement_interval",
			         where + " start_time 必须 < end_time");
		} else if (start->number < -1e-6) {
			AddError(report, "data.movement_negative",
			         where + " start_time 为负值");
		} else if (prevEnd >= 0.0 &&
		           start->number + 1e-6 < prevEnd) {
			AddError(report, "data.movement_order",
			         where + " 区间未按时间有序（早于上一区间 end）");
		}
		if (end != nullptr && end->type == JsonValue::Type::Number) {
			prevEnd = end->number;
		}
	}

	// looking：start_time/end_time/dx/dy（direction 可选）
	prevEnd = -1.0;
	for (size_t i = 0; i < looking->size(); ++i) {
		const JsonValue& e = (*looking)[i];
		const JsonValue* start = e.Find("start_time");
		const JsonValue* end = e.Find("end_time");
		const JsonValue* dx = e.Find("dx");
		const JsonValue* dy = e.Find("dy");
		const std::string where =
		    "looking_events[" + std::to_string(i) + "]";
		if (start == nullptr || start->type != JsonValue::Type::Number ||
		    end == nullptr || end->type != JsonValue::Type::Number ||
		    dx == nullptr || dx->type != JsonValue::Type::Number ||
		    dy == nullptr || dy->type != JsonValue::Type::Number) {
			AddError(report, "data.looking_fields",
			         where + " 缺少数值型 start_time/end_time/dx/dy");
			prevEnd = -1.0;
			continue;
		}
		if (!std::isfinite(start->number) ||
		    !std::isfinite(end->number) ||
		    !std::isfinite(dx->number) ||
		    !std::isfinite(dy->number)) {
			AddError(report, "data.looking_finite",
			         where + " 字段含 NaN/Inf");
		} else if (!(start->number < end->number)) {
			AddError(report, "data.looking_interval",
			         where + " start_time 必须 < end_time");
		} else if (start->number < -1e-6) {
			AddError(report, "data.looking_negative",
			         where + " start_time 为负值");
		} else if (prevEnd >= 0.0 &&
		           start->number + 1e-6 < prevEnd) {
			AddError(report, "data.looking_order",
			         where + " 桶未按时间有序");
		}
		if (end != nullptr && end->type == JsonValue::Type::Number) {
			prevEnd = end->number;
		}
	}

	// action：timestamp，按时间有序
	double prevTs = -1.0;
	for (size_t i = 0; i < actions->size(); ++i) {
		const JsonValue& e = (*actions)[i];
		const JsonValue* ts = e.Find("timestamp");
		const std::string where =
		    "action_events[" + std::to_string(i) + "]";
		if (ts == nullptr || ts->type != JsonValue::Type::Number) {
			AddError(report, "data.action_fields",
			         where + " 缺少数值型 timestamp");
			prevTs = -1.0;
			continue;
		}
		if (!std::isfinite(ts->number)) {
			AddError(report, "data.action_finite",
			         where + " timestamp 含 NaN/Inf");
		} else if (ts->number < -1e-6) {
			AddError(report, "data.action_negative",
			         where + " timestamp 为负值");
		} else if (prevTs >= 0.0 && ts->number + 1e-6 < prevTs) {
			AddError(report, "data.action_order",
			         where + " timestamp 未按时间有序");
		}
		prevTs = ts->number;
	}

	// 事件时间超过视频时长（只警告，允许导出端轻微越界）
	if (videoDuration > 0.0) {
		auto overVideo = [&](double t) {
			return t > videoDuration + kDurationToleranceSeconds;
		};
		for (const JsonValue& e : *movements) {
			const JsonValue* end = e.Find("end_time");
			if (end != nullptr && end->type == JsonValue::Type::Number &&
			    overVideo(end->number)) {
				AddWarning(report, "data.event_beyond_video",
				           "存在移动事件 end_time 超出视频时长 0.1s 以上");
				break;
			}
		}
		for (const JsonValue& e : *looking) {
			const JsonValue* end = e.Find("end_time");
			if (end != nullptr && end->type == JsonValue::Type::Number &&
			    overVideo(end->number)) {
				AddWarning(report, "data.event_beyond_video",
				           "存在视角事件 end_time 超出视频时长 0.1s 以上");
				break;
			}
		}
	}
}

/// 视频参数门槛
void ValidateVideo(const VideoProbeInfo& info, VerifyReport* report)
{
	if (!info.available) {
		AddError(report, "video.probe",
		         "无法从视频文件读取编码/分辨率/时长（文件损坏或不是有效 mp4）");
		return;
	}
	if (!IsH264OrH265(info.codec)) {
		AddError(report, "video.codec",
		         "视频编码不是 H.264/H.265（实测：" +
		             CodecDisplayName(info.codec) + "）");
	}
	if (info.width < 1920 || info.height < 1080) {
		AddError(report, "video.resolution",
		         "分辨率低于 1920×1080 门槛（实测：" +
		             std::to_string(info.width) + "×" +
		             std::to_string(info.height) + "）");
	}
	if (info.fps > 0.0 && info.fps < 24.0 - 0.01) {
		AddError(report, "video.fps",
		         "帧率低于 24fps 门槛（实测：" +
		             std::to_string(static_cast<int>(std::round(info.fps))) +
		             "fps）");
	}
	const long long minBitrate =
	    MinBitrateForResolution(info.width, info.height);
	if (minBitrate == 0) {
		AddWarning(report, "video.resolution_tier",
		           "分辨率低于 1080p 档，无对应码率下限，请确认是否符合预期");
	} else if (info.bitrateBps > 0 &&
	           info.bitrateBps <
	               static_cast<long long>(minBitrate * 0.97)) {
		// 3% 容差，吸收文件级码率统计噪声
		char need[32], got[32];
		std::snprintf(need, sizeof(need), "%.1f",
		              static_cast<double>(minBitrate) / 1e6);
		std::snprintf(got, sizeof(got), "%.2f",
		              static_cast<double>(info.bitrateBps) / 1e6);
		AddError(report, "video.bitrate",
		         std::string("平均码率低于该分辨率下限（要求 ≥") + need +
		             " Mbps，实测 " + got + " Mbps）");
	}
}

/// keymap 覆盖性：所有 action 中的 keyboard/mouse 标识必须在 keymap 中
void ValidateKeymapCoverage(
    const JsonValue& dataRoot, const std::string& keymapPath,
    const std::vector<JsonValue>& actions, VerifyReport* report)
{
	JsonValue keymapRoot;
	std::string error;
	if (!ParseJsonFile(keymapPath, &keymapRoot, &error)) {
		AddError(report, "keymap.parse", error);
		return;
	}
	if (keymapRoot.type != JsonValue::Type::Object) {
		AddError(report, "keymap.type",
		         "keymap JSON 顶层不是对象：" + BaseName(keymapPath));
		return;
	}
	std::vector<std::string> missing;
	for (const JsonValue& e : actions) {
		for (const char* field : {"mouse_event", "keyboard_event"}) {
			const std::string name = e.GetString(field);
			if (name.empty()) {
				continue;
			}
			const JsonValue* mapped = keymapRoot.Find(name);
			if (mapped == nullptr) {
				if (std::find(missing.begin(), missing.end(), name) ==
				    missing.end()) {
					missing.push_back(name);
				}
				continue;
			}
			const std::string type = mapped->GetString("type");
			const std::string action = mapped->GetString("action");
			if (type != "keyboard" && type != "mouse") {
				AddError(report, "keymap.entry_type",
				         "keymap 条目 \"" + name +
				             "\" 的 type 不是 keyboard/mouse");
			}
			if (action.empty()) {
				AddWarning(report, "keymap.empty_action",
				           "keymap 条目 \"" + name +
				               "\" 的 action 语义为空");
			}
		}
	}
	for (const std::string& name : missing) {
		AddError(report, "keymap.coverage",
		         "操作事件标识 \"" + name +
		             "\" 未在 keymap 说明书中定义");
	}
	(void)dataRoot;
}

} // namespace

// ===========================================================================
// 报告文本
// ===========================================================================

std::string VerifyReport::StatusText() const
{
	for (const VerifyIssue& issue : issues) {
		if (issue.isError) {
			return "失败";
		}
	}
	return "通过";
}

std::string VerifyReport::ToDetailText() const
{
	std::string text;
	if (video.available) {
		char line[512];
		std::snprintf(
		    line, sizeof(line),
		    "视频：%dx%d ｜ %s ｜ %.2f fps ｜ 时长 %.3fs ｜ "
		    "平均码率 %.2f Mbps（%s）\n",
		    video.width, video.height,
		    CodecDisplayName(video.codec).c_str(), video.fps,
		    video.durationSeconds,
		    static_cast<double>(video.bitrateBps) / 1e6,
		    video.probeBackend.empty() ? "未知来源"
		                               : video.probeBackend.c_str());
		text += line;
	} else {
		text += "视频：参数探测失败\n";
	}
	{
		char line[256];
		std::snprintf(line, sizeof(line),
		              "操作数据：duration=%.3fs ｜ movement=%zu ｜ "
		              "looking=%zu ｜ action=%zu\n",
		              dataDurationSeconds, movementCount, lookingCount,
		              actionCount);
		text += line;
	}
	text += "----------------------------------------\n";

	int errorIndex = 0;
	int warningIndex = 0;
	for (const VerifyIssue& issue : issues) {
		if (issue.isError) {
			text += "[失败] " + issue.message + "\n";
			++errorIndex;
		}
	}
	for (const VerifyIssue& issue : issues) {
		if (!issue.isError) {
			text += "[警告] " + issue.message + "\n";
			++warningIndex;
		}
	}
	if (errorIndex == 0) {
		text = "校验结果：通过" +
		       (warningIndex > 0
		            ? "（" + std::to_string(warningIndex) + " 条警告）"
		            : std::string()) +
		       "\n" + text;
	} else {
		text = "校验结果：失败（" + std::to_string(errorIndex) +
		       " 项不合格" +
		       (warningIndex > 0
		            ? "，" + std::to_string(warningIndex) + " 条警告"
		            : "") +
		       "）\n" + text;
	}
	return text;
}

// ===========================================================================
// 主入口
// ===========================================================================

VerifyReport VerifySessionArtifacts(const std::string& sessionDir,
                                    const std::string& videoName,
                                    const std::string& ffprobePath)
{
	VerifyReport report;

	const std::string trimmedDir =
	    sessionDir.empty() ? std::string()
	                       : std::string(Trim(sessionDir));
	const std::string trimmedVideo =
	    videoName.empty() ? std::string() : std::string(Trim(videoName));

	if (trimmedDir.empty() || trimmedVideo.empty()) {
		AddError(&report, "session.path",
		         "历史记录缺少会话目录或视频文件名，无法定位三件套");
		return report;
	}

	const std::string join =
	    trimmedDir.back() == '\\' || trimmedDir.back() == '/' ? "" : "\\";
	const std::string videoPath = trimmedDir + join + trimmedVideo;
	// 三件套基名=会话目录名（视频与操作数据/keymap 现已同名对齐）：
	// 视频由 OBS 以会话基名命名（见 output_configurator.cpp），操作数据/
	// keymap 由导出管线以会话基名命名（见 export_pipeline.cpp:490-514）。
	std::string dirNoSep = trimmedDir;
	while (!dirNoSep.empty() &&
	       (dirNoSep.back() == '\\' || dirNoSep.back() == '/')) {
		dirNoSep.pop_back();
	}
	const std::string base = BaseName(dirNoSep);
	const std::string dataPath =
	    trimmedDir + join + base + ".json";
	const std::string keymapPath =
	    trimmedDir + join + base + "_keymap.json";

	// —— 1. 三件套存在性 ——
	if (!FileExists(videoPath)) {
		AddError(&report, "files.video", "视频文件不存在：" + trimmedVideo);
	}
	if (!FileExists(dataPath)) {
		AddError(&report, "files.data",
		         "操作数据 JSON 不存在：" + base + ".json");
	}
	if (!FileExists(keymapPath)) {
		AddError(&report, "files.keymap",
		         "keymap 说明书不存在：" + base + "_keymap.json");
	}

	// —— 2. 视频探测 + 门槛 ——
	if (FileExists(videoPath)) {
		VideoProbeInfo info;
		bool probed = false;
		if (!ffprobePath.empty()) {
			probed = ProbeWithFfprobe(ffprobePath, videoPath, &info);
		}
		if (!probed) {
			probed = ProbeBuiltIn(videoPath, &info);
		}
		report.video = info;
		ValidateVideo(info, &report);
	}

	// —— 3. 操作数据 JSON ——
	if (FileExists(dataPath)) {
		JsonValue root;
		std::string error;
		if (!ParseJsonFile(dataPath, &root, &error)) {
			AddError(&report, "data.parse", error);
		} else if (root.type != JsonValue::Type::Object) {
			AddError(&report, "data.type",
			         "操作数据 JSON 顶层不是对象");
		} else {
			const JsonValue* meta = root.Find("game_metadata");
			if (meta == nullptr ||
			    meta->type != JsonValue::Type::Object) {
				AddError(&report, "data.metadata",
				         "缺少 game_metadata 对象");
			} else {
				report.dataDurationSeconds =
				    meta->GetNumber("duration_seconds", -1.0);
				if (report.dataDurationSeconds < 0.0) {
					AddError(&report, "data.duration",
					         "game_metadata.duration_seconds 缺失或非法");
				}
				const std::string metaVideo =
				    meta->GetString("video_name");
				if (!metaVideo.empty() &&
				    metaVideo != trimmedVideo) {
					AddError(&report, "data.video_name",
					         "metadata.video_name 与实际视频不一致（记录：" +
					             metaVideo + "）");
				}
			}
			ValidateEventArrays(root, report.video.durationSeconds,
			                     &report);

			// duration 与视频时长偏差 ≤ 0.1s（二期需求 4.4 口径）
			if (report.dataDurationSeconds >= 0.0 &&
			    report.video.durationSeconds > 0.0) {
				const double diff =
				    std::fabs(report.dataDurationSeconds -
				              report.video.durationSeconds);
				if (diff > kDurationToleranceSeconds) {
					char msg[256];
					std::snprintf(
					    msg, sizeof(msg),
					    "JSON duration(%.3fs) 与视频时长(%.3fs)"
					    "偏差 %.3fs，超过 0.1s",
					    report.dataDurationSeconds,
					    report.video.durationSeconds, diff);
					AddError(&report, "data.duration_mismatch", msg);
				}
			}

			// —— 4. keymap 覆盖 ——
			if (FileExists(keymapPath)) {
				const auto* actions =
				    AsArray(root, "action_events");
				if (actions != nullptr) {
					ValidateKeymapCoverage(root, keymapPath, *actions,
					                       &report);
				}
			}
		}
	}

	report.ok = std::none_of(
	    report.issues.begin(), report.issues.end(),
	    [](const VerifyIssue& issue) { return issue.isError; });
	return report;
}

std::string ResolveFfprobePath()
{
	std::wstring modulePath(MAX_PATH, L'\0');
	HMODULE self = nullptr;
	GetModuleHandleExW(
	    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	    reinterpret_cast<LPCWSTR>(&ResolveFfprobePath), &self);
	DWORD len = 0;
	if (self != nullptr) {
		len = GetModuleFileNameW(self, modulePath.data(),
		                         static_cast<DWORD>(modulePath.size()));
	}
	if (len > 0) {
		modulePath.resize(len);
		const std::wstring dir = ParentDir(modulePath);
		// 1) DLL 同目录（部署到 OBS\obs-plugins\64bit 时的分发位置）
		std::wstring candidate = dir + L"ffprobe.exe";
		if (Exists(candidate)) {
			return WideToUtf8(candidate);
		}
		// 2) DLL 目录向上两级 + ffmpeg/bin（开发态：
		//    build\Release\game_record_tool.dll）
		std::wstring root = ParentDir(ParentDir(dir));
		if (!root.empty()) {
			candidate = root + L"ffmpeg\\bin\\ffprobe.exe";
			if (Exists(candidate)) {
				return WideToUtf8(candidate);
			}
		}
	}
	// 3) 交由 PATH 解析；启动失败时调用方自动回退内置解析器
	return "ffprobe";
}

bool ProbeVideoDuration(const std::string& videoPath,
                        const std::string& ffprobePath,
                        double* outSeconds)
{
	if (outSeconds == nullptr || videoPath.empty()) {
		return false;
	}
	VideoProbeInfo info;
	bool probed = false;
	if (!ffprobePath.empty()) {
		probed = ProbeWithFfprobe(ffprobePath, videoPath, &info);
	}
	if (!probed) {
		probed = ProbeBuiltIn(videoPath, &info);
	}
	if (!probed || !std::isfinite(info.durationSeconds) ||
	    info.durationSeconds <= 0.0) {
		return false;
	}
	*outSeconds = info.durationSeconds;
	return true;
}

} // namespace grt
