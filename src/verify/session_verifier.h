/**
 * @file session_verifier.h
 * @brief 录制结束后的三件套自动校验（Task 12）。
 *
 * 校验内容：
 *   1. 视频文件存在且为 ISO-BMFF（mp4/fMP4），可探测编码、分辨率、帧率、
 *      平均码率、时长，并按需求质量门槛判定：
 *        - 分辨率 ≥ 1920×1080（低于门槛判失败；恰好 1080p 起步）
 *        - 帧率 ≥ 24fps
 *        - 编码为 H.264 / H.265
 *        - 平均码率不低于该分辨率档下限（1080p 8Mbps、1440p 24Mbps、4K 35Mbps）
 *   2. 操作数据 JSON 可解析、含 game_metadata 对象与
 *      movement_events/looking_events/action_events 三个数组；各事件时间
 *      区间合法（start<end、非负、不超过视频时长）且按时间有序。
 *   3. 三件套齐全（mp4 + <base>.json + <base>_keymap.json），
 *      metadata.video_name 与实际视频文件名一致。
 *   4. keymap 覆盖 action_events 中出现的全部 keyboard/mouse 事件标识。
 *   5. metadata.duration_seconds 与视频时长偏差 ≤ 0.1s（对齐二期需求 4.4）。
 *
 * 视频参数探测优先调用系统中的 ffprobe（spec 允许随插件分发）；找不到或
 * 执行失败时回退到内置最小 ISO-BMFF 解析器（只读 moov 元数据，不解码），
 * 保证在未分发 ffprobe 的机器上也能离线完成校验。
 *
 * 本模块为纯同步、无 Qt/OBS 依赖（JSON 解析除外），可在后台线程调用，
 * 便于 Task 13 单元测试复用。
 */

#pragma once

#include <string>
#include <vector>

namespace grt {

/// 一条校验发现（错误导致整体不通过；警告不阻断）
struct VerifyIssue {
	bool isError = false;       ///< true=不合格原因；false=警告/提示
	std::string code;           ///< 稳定机器码（如 "video.bitrate_low"）
	std::string message;        ///< 面向用户的中文说明
};

/// 视频探测结果（字段取不到时保持 0/空）
struct VideoProbeInfo {
	bool available = false;     ///< 是否成功探测（false 时其余字段无意义）
	std::string codec;          ///< "h264" / "h265" / 其他原始 fourcc 小写
	std::string codecName;      ///< 展示名，如 "H.264 (AVC)" / "H.265 (HEVC)"
	int width = 0;
	int height = 0;
	double fps = 0.0;           ///< 平均帧率（样本总数 / 时长）
	double durationSeconds = 0.0;
	long long bitrateBps = 0;   ///< 平均码率（优先 mvhd 时长，回退文件大小）
	std::string probeBackend;   ///< "ffprobe" / "内置MP4解析" / 空
};

/// 一次会话校验的完整报告
struct VerifyReport {
	bool ok = false;                        ///< 无任何 error 即为通过
	VideoProbeInfo video;                   ///< 视频探测结果
	double dataDurationSeconds = 0.0;       ///< game_metadata.duration_seconds
	size_t movementCount = 0;
	size_t lookingCount = 0;
	size_t actionCount = 0;
	std::vector<VerifyIssue> issues;        ///< 错误 + 警告（按检查顺序）

	/// 状态短文案（用于历史"校验状态"列）：通过 / 失败 / 未校验由调用方定
	std::string StatusText() const;
	/// 多行完整报告（错误在前、警告在后，附视频参数摘要），用于 GUI 弹窗
	std::string ToDetailText() const;
};

/**
 * @brief 校验一次录制会话。
 * @param sessionDir   会话目录（三件套所在目录，UTF-8）
 * @param videoName    视频文件名（通常来自历史记录/前端最后录制路径）
 * @param ffprobePath  ffprobe 可执行文件路径；传空则仅用内置解析器。
 *                     可传 "ffprobe" 以走 PATH 查找。
 * @return 校验报告（永不抛异常；无法继续时以 error 形式给出原因）。
 */
VerifyReport VerifySessionArtifacts(const std::string& sessionDir,
                                    const std::string& videoName,
                                    const std::string& ffprobePath = {});

/**
 * @brief 探测视频容器的时长（秒）。
 *
 * 复用本模块的视频探测能力（优先级：ffprobe → 内置最小 ISO-BMFF 解析），
 * 供导出阶段在写入 duration_seconds 之前取得「实际视频时长」这一真值。
 *
 * @param videoPath    视频文件完整路径（UTF-8）
 * @param ffprobePath  ffprobe 路径；传空则仅用内置解析器
 * @param outSeconds   输出时长（秒）；成功时 > 0
 * @return true 探测成功且时长有限、为正；false 表示探测失败（含文件不存在）
 */
bool ProbeVideoDuration(const std::string& videoPath,
                        const std::string& ffprobePath,
                        double* outSeconds);

/**
 * @brief 解析运行时 ffprobe 路径（UTF-8）。
 *
 * 查找顺序：
 *   1. 插件 DLL 同目录下的 ffprobe.exe（部署形态，deploy.ps1 分发）；
 *   2. DLL 目录向上两级再进入 ffmpeg/bin/ffprobe.exe（开发态工程根目录）；
 *   3. 工程根目录 ffmpeg/bin/ffprobe.exe 的绝对路径兜底；
 *   4. 均不存在时返回 "ffprobe"，交由系统 PATH 解析（不存在则子进程
 *      启动失败，自动回退内置解析器）。
 */
std::string ResolveFfprobePath();

} // namespace grt
