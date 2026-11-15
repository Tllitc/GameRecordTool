/**
 * @file output_configurator.h
 * @brief SimpleOutput 录制输出（编码器/码率/容器/目录）自动配置。
 *
 * 全部配置均写入 obs_frontend_get_profile_config() 返回的 profile 配置：
 *   [Output]       Mode = "Simple"
 *   [SimpleOutput] RecQuality / RecEncoder / RecFormat2 / VBitrate / FilePath ...
 *   [Video]        AutoRemux
 *
 * 【从 OBS 32.2.2 源码确认的关键事实】
 *   - SimpleOutput 构造时（输出 handler 创建时）按 RecEncoder 创建一次录制
 *     视频编码器（get_simple_output_encoder 映射逻辑名 -> 真实 id）。
 *   - RecQuality="Stream" 时录制复用推流编码器（按 StreamEncoder 创建），
 *     StartRecording -> UpdateRecording 中推流不活动时调用 Update()，
 *     后者以 CBR + [SimpleOutput] VBitrate 更新码率——这是显式指定录制
 *     码率的途径（CRF/CQP 质量模式不读 VBitrate）。
 *   - FilePath / RecFormat2 / MuxerCustom / [Output] FilenameFormatting 等
 *     在每次录制启动时由 SimpleOutput::ConfigureRecording() 动态重读，
 *     因此这些键写入并 config_save 后，下一次录制立即生效。
 *   - RecFormat2 以 "fragmented" 开头即 fMP4/fMOV，ConfigureRecording 自动
 *     追加 "movflags=frag_keyframe+empty_moov+delay_moov"，扩展名仍为 .mp4。
 *   - AutoRemux 键位于 [Video]（bool）。普通 mp4 在 AutoRemux 打开时，
 *     GetRecordingFilename 内部会把容器改为 mkv，录制停止后自动 remux。
 *
 * 【已知限制（重要偏离项）】
 *   RecEncoder / RecQuality / Mode 的更改需要 SimpleOutput handler 重建才生效，
 *   前端内部由 OBSBasic::ResetOutputs() 完成，但该方法不是 slot/Q_INVOKABLE，
 *   obs-frontend-api 未导出；同名 profile 切换在 ChangeProfile 中直接短路，
 *   异名切换副作用不可接受（ResetVideo/InitService/重建热键/发事件/可能弹
 *   重启提示框）。因此本类只保证“写对配置并 config_save”：编码器/质量/模式
 *   相关键在 OBS 重启（或用户在设置页切换输出模式触发重建）后生效；
 *   路径、容器、文件名、AutoRemux、码率（RecQuality 已为 Stream 时）则在
 *   下次录制启动时立即生效。Apply() 会在编码器相关键发生变化时输出 WARN 日志。
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace grt {

/// 编码器可用性探测结果
struct ProbeResult {
	std::string h264EncoderId;  ///< 选定的 H.264 编码器真实 id（至少有 x264 兜底，非空）
	std::string hevcEncoderId;  ///< 选定的 HEVC 编码器真实 id（不可用时为空）
	bool h265Available = false; ///< HEVC 编码器是否可用
	std::string chosenId;       ///< 本次最终选用的编码器真实 id
	std::string chosenCodec;    ///< 本次最终编码格式："h264" 或 "hevc"
};

/// 一次录制方案的完整描述（供 SessionManager 等后续模块使用）
struct RecordPlan {
	int width = 0;          ///< 视频宽（像素）
	int height = 0;         ///< 视频高（像素）
	int fps = 60;           ///< 帧率
	int bitrateKbps = 0;    ///< 视频码率（kbps）
	std::string codec;      ///< 编码格式："h264" / "hevc"
	std::string encoderId;  ///< libobs 编码器真实 id（如 "ffmpeg_nvenc"）
	std::string container;  ///< 录制容器格式："fragmented_mp4" / "mp4" / "mkv"
	bool remux = false;     ///< 录制停止后是否依赖前端自动 remux 到 mp4
	bool below1080p = false; ///< 分辨率是否低于 1920x1080（码率已按 1080p 档位兜底）
};

class OutputConfigurator {
public:
	// —— 码率档位（kbps），档间向上取档；低于 1080p 用 1080p 档位 ——
	static constexpr int kBitrate1080p = 12000;
	static constexpr int kBitrate1440p = 30000;
	static constexpr int kBitrate4K = 45000;

	// —— SimpleOutput 配置常量（取值均经源码核实）——
	static constexpr const char* kRecFormatFmp4 = "fragmented_mp4";
	static constexpr const char* kRecFormatMkv = "mkv";
	static constexpr const char* kRecFormatMp4 = "mp4";
	/// 兜底的文件名格式：Apply 未指定文件名基名时使用（OBS 时间戳占位符，
	/// 前缀便于检索本插件录制文件）
	static constexpr const char* kFilenameFormatting = "GRT %CCYY-%MM-%DD %hh-%mm-%ss";

	/**
	 * @brief 探测可用的硬件/软件编码器。
	 *
	 * 用 obs_video_encoder_create 逐个试创建立即释放来验证（而非仅枚举
	 * 已注册 id，因为注册存在不等于硬件/运行时可用）。
	 * 自动探测顺序（H.264 与 HEVC 分别进行）：
	 *   H.264：obs_nvenc_h264_tex / ffmpeg_nvenc
	 *          -> h264_texture_amf -> obs_qsv11_v2 -> obs_x264
	 *   HEVC ：obs_nvenc_hevc_tex / ffmpeg_hevc_nvenc
	 *          -> h265_texture_amf -> obs_qsv11_hevc（无软件兜底）
	 *
	 * 必须在 OBS 主线程调用（编码器创建不是线程安全的）。
	 */
	static ProbeResult ProbeEncoders();

	/**
	 * @brief 依据分辨率与录制设置，计算码率档位。
	 * @param height       视频高度（像素）。
	 * @param width        视频宽度（像素）。
	 * @param overrideKbps 用户显式码率（kbps），非 0 时直接覆盖。
	 * @param below1080p   [out] 分辨率是否低于 1920x1080。
	 * @return 码率（kbps）。档间向上取档：>=2160 用 4K 档，>=1440 用
	 *         1440p 档，其余（含低于 1080p）用 1080p 档。
	 */
	static int ResolveBitrate(int width, int height, int overrideKbps,
	                          bool& below1080p);

	/**
	 * @brief 应用录制配置（写 profile config 并保存）。
	 *
	 * @param plan [in,out] 录制方案：调用前应填好 width/height/fps；
	 *             本函数填充 codec/encoderId/container/remux/bitrateKbps/
	 *             below1080p。
	 * @param outputDir    录制输出目录（UTF-8），不存在时递归创建。
	 * @param filenameBase 录制文件名基名（不含扩展名），录制文件将产出为
	 *                     <filenameBase>.<ext>，与同目录数据文件（json/
	 *                     keymap.json/raw.jsonl）基名对齐；为空时回退到
	 *                     kFilenameFormatting 的时间戳命名。
	 * @param codecPref    编码偏好："auto"/"h264"/"h265"。
	 * @param encoderPref  编码器偏好："auto"/"nvenc"/"amf"/"qsv"/"x264"。
	 * @param bitrateKbps  码率覆盖值，0 表示按分辨率档位自动。
	 * @param crashSafe    崩溃安全策略：true 优先 fMP4 直出，不可用时
	 *                     mkv + 停止后自动 remux；false 直接普通 mp4。
	 * @return 全部关键键写入成功返回 true；输出目录无法创建等致命失败 false。
	 *
	 * 必须在 OBS 主线程调用（访问 profile config 与编码器探测）。
	 */
	bool Apply(RecordPlan& plan, const std::string& outputDir,
	           const std::string& filenameBase, const std::string& codecPref,
	           const std::string& encoderPref, int bitrateKbps,
	           bool crashSafe);

	/**
	 * @brief 仅恢复本次 Apply 改动过的配置键（未改的键不动）。
	 * @return config_save 成功返回 true；未 Apply 过时为空操作并返回 true。
	 */
	bool Restore();

	/// 本次探测/选用的编码器详细结果
	const ProbeResult& ChosenInfo() const { return probe_; }

	/// 本次 Apply 改动过的键（"Section/Name" -> 原值），供诊断/单测
	const std::map<std::string, std::string>& SnapshotOfChangedKeys() const
	{
		return changed_;
	}

	/// 本次方案是否将在录制停止后自动 remux（mkv 中转）
	bool WillRemux() const { return willRemux_; }

	/**
	 * @brief 获取前端记录的最后一次录制文件完整路径（UTF-8）。
	 * @return 路径；无录制记录时为空串。内部已 bfree 前端返回的内存。
	 *
	 * obs_frontend_get_last_recording 返回 bstrdup 分配的 char*，调用者负责释放。
	 * 该接口仅读指针，可在任意线程调用，但建议主线程使用。
	 */
	static std::string LastRecordingPath();

private:
	/// 记录一个待改键的原值（仅第一次遇到时记录）
	void RememberOriginal(const char* section, const char* name,
	                      const std::string& originalValue);

	ProbeResult probe_;
	bool willRemux_ = false;
	bool applied_ = false;

	/// "Section/Name" -> 原值（含一个特殊标记区分“原本无用户值”）
	std::map<std::string, std::string> changed_;
	/// "Section/Name" -> 原值是否为用户显式值（false 表示原来不存在）
	std::map<std::string, bool> changedHadValue_;
};

} // namespace grt
