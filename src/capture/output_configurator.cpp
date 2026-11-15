/**
 * @file output_configurator.cpp
 * @brief SimpleOutput 录制输出自动配置实现。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/output_configurator.h"

#include <array>
#include <string>

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <util/config-file.h>
#include <util/platform.h>

#include "core/log.h"

namespace grt {

namespace {

/// 单个编码器候选项：逻辑偏好名 + 真实 id（按探测顺序排列）
struct EncoderCandidate {
	const char* logical; ///< 对应 SimpleOutput RecEncoder 逻辑名
	const char* id;      ///< libobs 编码器 id
};

// H.264 探测顺序：NVENC（新版纹理优先，旧版 ffmpeg 兜底）-> AMF -> QSV -> x264
constexpr std::array<EncoderCandidate, 5> kH264Candidates = {{
	{"nvenc", "obs_nvenc_h264_tex"},
	{"nvenc", "ffmpeg_nvenc"},
	{"amd", "h264_texture_amf"},
	{"qsv", "obs_qsv11_v2"},
	{"x264", "obs_x264"},
}};

// HEVC 探测顺序：NVENC -> AMF -> QSV（无软件兜底）
constexpr std::array<EncoderCandidate, 4> kHevcCandidates = {{
	{"nvenc_hevc", "obs_nvenc_hevc_tex"},
	{"nvenc_hevc", "ffmpeg_hevc_nvenc"},
	{"amd_hevc", "h265_texture_amf"},
	{"qsv_hevc", "obs_qsv11_hevc"},
}};

/// 试创建一个视频编码器验证可用性，成功立即释放（与前端 EncoderAvailable 同思路）
bool TryCreateEncoder(const char* id)
{
	obs_encoder_t* encoder = obs_video_encoder_create(id, "grt_probe",
	                                                   nullptr, nullptr);
	if (encoder) {
		obs_encoder_release(encoder);
		return true;
	}
	return false;
}

/// 在候选列表中按顺序找出第一个可用项；全部不可用返回 nullptr
template <size_t N>
const EncoderCandidate* ProbeList(const std::array<EncoderCandidate, N>& list)
{
	for (const auto& candidate : list) {
		if (TryCreateEncoder(candidate.id)) {
			return &candidate;
		}
	}
	return nullptr;
}

/// 真实编码器 id -> SimpleOutput RecEncoder 逻辑名（按 codec 分表）
template <size_t N>
const char* LogicalForIdInList(const std::array<EncoderCandidate, N>& list,
                               const char* id)
{
	for (const auto& candidate : list) {
		if (std::string(candidate.id) == id) {
			return candidate.logical;
		}
	}
	return nullptr;
}

/// 用户编码器偏好是否匹配候选逻辑名（HEVC 下短名 nvenc/amf/qsv 映射到 *_hevc）
bool PrefMatchesCandidate(bool wantHevc, const std::string& encoderPref,
                          const EncoderCandidate& candidate)
{
	const std::string logical = candidate.logical;
	if (wantHevc) {
		return (encoderPref == "nvenc" && logical == "nvenc_hevc") ||
		       (encoderPref == "amf" && logical == "amd_hevc") ||
		       (encoderPref == "qsv" && logical == "qsv_hevc");
	}
	return logical == encoderPref;
}

std::string MakeKey(const char* section, const char* name)
{
	return std::string(section) + "/" + name;
}

} // namespace

ProbeResult OutputConfigurator::ProbeEncoders()
{
	ProbeResult result;

	const EncoderCandidate* h264 = ProbeList(kH264Candidates);
	if (h264) {
		result.h264EncoderId = h264->id;
	} else {
		// 正常环境 obs_x264 必然可用，此处仅作防御
		GRT_LOG_ERROR("未探测到任何可用的 H.264 编码器（含 obs_x264）");
	}

	const EncoderCandidate* hevc = ProbeList(kHevcCandidates);
	if (hevc) {
		result.hevcEncoderId = hevc->id;
		result.h265Available = true;
	}

	return result;
}

int OutputConfigurator::ResolveBitrate(int width, int height, int overrideKbps,
                                       bool& below1080p)
{
	if (overrideKbps > 0) {
		below1080p = (height < 1080 || width < 1920);
		return overrideKbps;
	}

	below1080p = (height < 1080 || width < 1920);

	// 档间向上取档：达到更高档任一边界即升入该档
	if (height >= 2160 || width >= 3840) {
		return kBitrate4K;
	}
	if (height >= 1440 || width >= 2560) {
		return kBitrate1440p;
	}
	// 1080p 及以下（含低于 1080p）统一 1080p 档位
	return kBitrate1080p;
}

void OutputConfigurator::RememberOriginal(const char* section, const char* name,
                                          const std::string& originalValue)
{
	const std::string key = MakeKey(section, name);
	if (changed_.find(key) != changed_.end()) {
		return; // 已记录过原值，不覆盖
	}
	changed_[key] = originalValue;
	changedHadValue_[key] = !originalValue.empty() ||
		config_has_user_value(obs_frontend_get_profile_config(), section, name);
}

bool OutputConfigurator::Apply(RecordPlan& plan, const std::string& outputDir,
                               const std::string& filenameBase,
                               const std::string& codecPref,
                               const std::string& encoderPref,
                               int bitrateKbps, bool crashSafe)
{
	config_t* config = obs_frontend_get_profile_config();
	if (!config) {
		GRT_LOG_ERROR("无法获取 profile config，输出配置中止");
		return false;
	}

	// —— 1. 输出目录递归创建（FilePath 在录制启动时动态读取）——
	if (outputDir.empty()) {
		GRT_LOG_ERROR("输出目录为空");
		return false;
	}
	if (!os_file_exists(outputDir.c_str())) {
		if (os_mkdirs(outputDir.c_str()) != 0) {
			GRT_LOG_ERROR("创建输出目录失败：%s", outputDir.c_str());
			return false;
		}
		GRT_LOG_INFO("已创建输出目录：%s", outputDir.c_str());
	}

	// —— 2. 编码器探测与选择 ——
	probe_ = ProbeEncoders();

	bool wantHevc = (codecPref == "h265" || codecPref == "hevc");
	if (wantHevc && !probe_.h265Available) {
		GRT_LOG_WARN("请求 H.265 但未探测到可用 HEVC 编码器，回退 H.264");
		wantHevc = false;
	}

	// 在对应 codec 的候选列表中按用户偏好过滤；auto/不匹配则用探测默认值
	const char* chosenId = nullptr;

	if (encoderPref != "auto" && !encoderPref.empty()) {
		auto pickByPref = [&](const auto& list) -> const char* {
			for (const auto& candidate : list) {
				// 偏好匹配逻辑名（HEVC 下短名 nvenc/amf/qsv 映射到 *_hevc）
				if (PrefMatchesCandidate(wantHevc, encoderPref, candidate) &&
				    TryCreateEncoder(candidate.id)) {
					return candidate.id;
				}
			}
			return nullptr;
		};
		chosenId = wantHevc ? pickByPref(kHevcCandidates)
		                    : pickByPref(kH264Candidates);
		if (!chosenId) {
			GRT_LOG_WARN("指定编码器不可用（%s/%s），改用自动探测结果",
			             encoderPref.c_str(), wantHevc ? "hevc" : "h264");
		}
	}
	if (!chosenId) {
		chosenId = wantHevc ? probe_.hevcEncoderId.c_str()
		                    : probe_.h264EncoderId.c_str();
	}
	if (!chosenId || !*chosenId) {
		GRT_LOG_ERROR("没有可用的视频编码器");
		return false;
	}

	probe_.chosenId = chosenId;
	probe_.chosenCodec = wantHevc ? "hevc" : "h264";
	const char* logicalEncoder = wantHevc
		? LogicalForIdInList(kHevcCandidates, chosenId)
		: LogicalForIdInList(kH264Candidates, chosenId);
	if (!logicalEncoder) {
		// chosenId 必来自候选表或探测结果，理论上不会失败；防御性兜底
		logicalEncoder = "x264";
	}

	// —— 3. 码率档位 ——
	plan.bitrateKbps = ResolveBitrate(plan.width, plan.height, bitrateKbps,
	                                  plan.below1080p);
	if (plan.below1080p) {
		GRT_LOG_WARN("录制分辨率 %dx%d 低于 1920x1080，码率按 1080p 档位 %d kbps",
		             plan.width, plan.height, plan.bitrateKbps);
	}

	// —— 4. 容器与崩溃安全策略 ——
	// fMP4 由内置 ffmpeg_muxer 支持（OBS 24+），随 obs-ffmpeg 必然存在，
	// 这里仍以输出类型枚举做一次防御性确认。
	bool fmp4Supported = false;
	{
		size_t idx = 0;
		const char* outputId = nullptr;
		while (obs_enum_output_types(idx++, &outputId)) {
			if (std::string(outputId) == "ffmpeg_muxer") {
				fmp4Supported = true;
				break;
			}
		}
	}

	std::string recFormat;
	bool autoRemux = false;
	if (crashSafe) {
		if (fmp4Supported) {
			// 碎片化 MP4：直接产出可播放的 .mp4，崩溃不丢整个文件
			recFormat = kRecFormatFmp4;
			autoRemux = false;
		} else {
			// mkv 抗崩溃 + 录制停止后前端自动 remux 成 mp4
			recFormat = kRecFormatMkv;
			autoRemux = true;
			GRT_LOG_WARN("碎片化 MP4 不可用，改用 mkv 录制并在停止后自动 remux");
		}
	} else {
		// 非崩溃安全：直接普通 mp4（必须关闭 AutoRemux，否则会被改成 mkv）
		recFormat = kRecFormatMp4;
		autoRemux = false;
	}
	willRemux_ = autoRemux;
	plan.container = recFormat;
	plan.remux = autoRemux;
	plan.codec = probe_.chosenCodec;
	plan.encoderId = chosenId;

	// —— 5. 写配置（先记录原值，仅本次 Apply 首次改动时记录）——
	// 这些编码器/质量/模式相关键需要输出 handler 重建才完全生效（见头注释）。
	const char* prevMode = config_get_string(config, "Output", "Mode");
	const char* prevRecQuality = config_get_string(config, "SimpleOutput", "RecQuality");
	const char* prevStreamEncoder = config_get_string(config, "SimpleOutput", "StreamEncoder");
	const char* prevRecFormat = config_get_string(config, "SimpleOutput", "RecFormat2");
	const char* prevFilePath = config_get_string(config, "SimpleOutput", "FilePath");
	const char* prevFilename = config_get_string(config, "Output", "FilenameFormatting");
	const uint64_t prevVBitrate = config_get_uint(config, "SimpleOutput", "VBitrate");
	const bool prevAutoRemux = config_get_bool(config, "Video", "AutoRemux");
	const char* prevFpsType = config_get_string(config, "Video", "FPSType");
	const char* prevFpsCommon = config_get_string(config, "Video", "FPSCommon");
	const uint64_t prevFpsInt = config_get_uint(config, "Video", "FPSInt");

	RememberOriginal("Output", "Mode", prevMode ? prevMode : "");
	RememberOriginal("SimpleOutput", "RecQuality", prevRecQuality ? prevRecQuality : "");
	RememberOriginal("SimpleOutput", "StreamEncoder",
	                 prevStreamEncoder ? prevStreamEncoder : "");
	RememberOriginal("SimpleOutput", "RecFormat2", prevRecFormat ? prevRecFormat : "");
	RememberOriginal("SimpleOutput", "FilePath", prevFilePath ? prevFilePath : "");
	RememberOriginal("Output", "FilenameFormatting", prevFilename ? prevFilename : "");
	RememberOriginal("SimpleOutput", "VBitrate",
	                 std::to_string(prevVBitrate));
	RememberOriginal("Video", "AutoRemux", prevAutoRemux ? "true" : "false");
	RememberOriginal("Video", "FPSType", prevFpsType ? prevFpsType : "");
	RememberOriginal("Video", "FPSCommon", prevFpsCommon ? prevFpsCommon : "");
	RememberOriginal("Video", "FPSInt", std::to_string(prevFpsInt));

	// RecQuality="Stream"：录制复用推流编码器，启动时 Update() 读 VBitrate（CBR）
	config_set_string(config, "Output", "Mode", "Simple");
	config_set_string(config, "SimpleOutput", "RecQuality", "Stream");
	config_set_string(config, "SimpleOutput", "StreamEncoder", logicalEncoder);
	config_set_string(config, "SimpleOutput", "RecFormat2", recFormat.c_str());
	config_set_string(config, "SimpleOutput", "FilePath", outputDir.c_str());
	// 录制文件名与同目录数据文件同基名：FilenameFormatting 不含 % 占位符时
	// OBS 按字面串使用，故直接写入会话基名即可产出 <base>.<ext>。
	// 未提供基名（如独立调用）时回退到可预测的时间戳格式。
	const std::string filenameFormatting =
		filenameBase.empty() ? std::string(kFilenameFormatting) : filenameBase;
	config_set_string(config, "Output", "FilenameFormatting",
	                  filenameFormatting.c_str());
	config_set_uint(config, "SimpleOutput", "VBitrate",
	                static_cast<uint64_t>(plan.bitrateKbps));
	config_set_bool(config, "Video", "AutoRemux", autoRemux);

	// 帧率：简单输出固定走 [Video] 公共帧率
	const std::string fpsText = std::to_string(plan.fps > 0 ? plan.fps : 60);
	config_set_string(config, "Video", "FPSType", "Common");
	config_set_string(config, "Video", "FPSCommon", fpsText.c_str());
	config_set_uint(config, "Video", "FPSInt",
	                static_cast<uint64_t>(plan.fps > 0 ? plan.fps : 60));

	const int saveResult = config_save(config);
	if (saveResult != 0) {
		GRT_LOG_ERROR("保存 profile config 失败（code=%d）", saveResult);
		return false;
	}

	// 编码器相关键若相对原值有变化，提示“需重建输出才生效”的限制
	const bool encoderChanged =
		(prevStreamEncoder == nullptr) ||
		(std::string(prevStreamEncoder) != logicalEncoder);
	const bool qualityChanged =
		(prevRecQuality == nullptr) || (std::string(prevRecQuality) != "Stream");
	const bool modeChanged =
		(prevMode == nullptr) || (std::string(prevMode) != "Simple");
	if (encoderChanged || qualityChanged || modeChanged) {
		GRT_LOG_WARN("输出模式/质量/编码器已写入配置（%s, %s, 码率 %d kbps），"
		             "需重建输出（重启 OBS 或切换输出模式）后编码器设置才生效；"
		             "路径/容器/文件名下次录制即生效",
		             logicalEncoder, probe_.chosenCodec.c_str(), plan.bitrateKbps);
	}

	applied_ = true;
	GRT_LOG_INFO("录制输出配置完成：容器=%s，自动remux=%d，目录=%s",
	             recFormat.c_str(), autoRemux ? 1 : 0, outputDir.c_str());
	return true;
}

bool OutputConfigurator::Restore()
{
	if (!applied_) {
		return true;
	}

	config_t* config = obs_frontend_get_profile_config();
	if (!config) {
		GRT_LOG_ERROR("恢复输出配置时无法获取 profile config");
		return false;
	}

	// 仅恢复本次改动过的键；原来不存在的键直接移除
	for (const auto& [key, value] : changed_) {
		const size_t slash = key.find('/');
		if (slash == std::string::npos) {
			continue;
		}
		const std::string section = key.substr(0, slash);
		const std::string name = key.substr(slash + 1);
		const bool hadValue = changedHadValue_[key];

		if (!hadValue) {
			config_remove_value(config, section.c_str(), name.c_str());
			continue;
		}

		// 按已知键类型选择还原方式（其余按字符串还原即可，
		// 布尔/整型键的值本身也是合法数字串，故显式分流）
		if (key == "Video/AutoRemux") {
			config_set_bool(config, section.c_str(), name.c_str(),
			                value == "true");
		} else if (key == "SimpleOutput/VBitrate" || key == "Video/FPSInt") {
			config_set_uint(config, section.c_str(), name.c_str(),
			                std::stoull(value));
		} else {
			config_set_string(config, section.c_str(), name.c_str(),
			                  value.c_str());
		}
	}

	const int saveResult = config_save(config);
	if (saveResult != 0) {
		GRT_LOG_ERROR("恢复后保存 profile config 失败（code=%d）", saveResult);
		return false;
	}

	const size_t restoredCount = changed_.size();
	changed_.clear();
	changedHadValue_.clear();
	willRemux_ = false;
	applied_ = false;
	GRT_LOG_INFO("录制输出配置已恢复（共还原 %zu 个键）", restoredCount);
	return true;
}

std::string OutputConfigurator::LastRecordingPath()
{
	char* path = obs_frontend_get_last_recording();
	if (!path) {
		return {};
	}
	std::string result = path; // 前端返回的已是 UTF-8
	bfree(path);
	return result;
}

} // namespace grt
