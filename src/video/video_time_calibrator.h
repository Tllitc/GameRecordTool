/**
 * @file video_time_calibrator.h
 * @brief 视频帧时间戳（libobs 视频时钟）与 QPC 单调时钟之间的校准。
 *
 * 设计上分为两层：
 *   1. grt::OffsetEstimator —— 纯 C++ 数学层（定长环形缓冲 + 中位数偏移 +
 *      最小二乘速率诊断），不包含任何 OBS / Windows 头文件，可在无 OBS
 *      环境直接编译，供 Task 13 单元测试使用；
 *   2. grt::VideoTimeCalibrator —— OBS 接线层，持有 raw-video 回调、
 *      OffsetEstimator 以及录制起止标记。本头文件只对 OBS 类型做前置
 *      声明（video_data），OBS 头文件仅在 .cpp 中包含，因此本头文件
 *      同样可被无 OBS 的测试安全包含。
 *
 * 时间基事实（已对 third_party/obs-dev/include/libobs 头文件核实）：
 *   - struct video_data { uint8_t *data[MAX_AV_PLANES]; uint32_t linesize[];
 *     uint64_t timestamp; }（media-io/video-io.h），raw-video 回调拿到的
 *     timestamp 即视频输出时间戳；
 *   - libobs 视频输出时间戳的时间基为纳秒（os_gettime_ns 时钟域）。
 *     注意：本 SDK 版本（OBS 32.x）的 struct obs_video_info 中【没有】
 *     timebase_num / timebase_den 字段（obs.h），编码器包
 *     encoder_packet 才有独立 timebase。因此这里不存在“从
 *     obs_get_video_info 读取 timebase”的可能，统一按 1e9 纳秒换算
 *     （VIDEO_TIMESTAMP_NS_PER_SEC）。
 */

#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

// 前置声明 OBS C 头文件中的类型，避免本头文件依赖 libobs（.cpp 中再包含 obs.h）
struct video_data;

namespace grt {

/// 视频时间戳单位为纳秒：1 秒 = 1e9 ns
inline constexpr double kVideoTimestampNsPerSec = 1000000000.0;

/// 环形缓冲保留的最近样本数上限（120 个样本 ≈ 60fps 下 2 秒），防止无限增长
inline constexpr std::size_t kCalibratorRingSize = 120;

/**
 * @brief 一组成对的时钟样本：同一帧的视频时间与采集当时的 QPC 时间（均为秒）。
 */
struct TimeSample {
	double videoSec; ///< 由帧 pts（纳秒）按视频时间基换算得到的秒
	double qpcSec;   ///< 回调采集瞬间 grt::Clock::NowSeconds() 的值
};

/**
 * @brief 纯逻辑时钟偏移估计器（不依赖 libobs，可直接单元测试）。
 *
 * 模型：qpcSec = videoSec + offset。
 * offset 取最近样本 (qpcSec - videoSec) 的中位数——中位数对调度毛刺、
 * 个别异常帧稳健。QPC 与视频时钟理论速率一致（均为硬件单调时钟），
 * 因此【不做】斜率校正；另用最小二乘计算速率比仅作日志诊断。
 *
 * 线程安全：内部 mutex 保护，采样写入（视频线程）与换算读取（其它线程）
 * 可并发。临界区只操作固定小数组，保持极短。
 */
class OffsetEstimator {
public:
	/// 加入一个样本（环形覆盖最旧样本）。
	void AddSample(double videoSec, double qpcSec);

	/// 清空全部样本（重新开始校准）。
	void Reset();

	/// 至少 2 个样本才认为可做中位数偏移估计。
	bool Ready() const;

	/// 当前样本数。
	std::size_t SampleCount() const;

	/**
	 * @brief 中位数偏移 offset = median(qpcSec - videoSec)。
	 * @return 偏移秒；未就绪时返回 NaN（调用方必须先查 Ready()）。
	 */
	double Offset() const;

	/**
	 * @brief 视频时间 -> QPC 时间。
	 * @return videoSec + offset；未就绪返回 NaN（调用方必须先查 Ready()）。
	 */
	double ToQpc(double videoSec) const;

	/**
	 * @brief QPC 时间 -> 视频时间。
	 * @return qpcSec - offset；未就绪返回 NaN（调用方必须先查 Ready()）。
	 */
	double ToVideo(double qpcSec) const;

	/**
	 * @brief 速率诊断：对最近样本做最小二乘得到 qpc 相对 video 的速率比。
	 *
	 * 理想值 1.0；显著偏离 1.0 说明两个时钟速率不一致（正常不应发生，
	 * 仅用于日志报告，不参与任何校正）。
	 * @return 速率比；样本不足（<2）或视频方向无方差时返回 NaN。
	 */
	double RateRatio() const;

	/**
	 * @brief 给定一批样本求 (qpc - video) 的中位数偏移（纯函数，便于测试）。
	 * @param samples 样本数组（至少 2 个；不足返回 NaN）。
	 */
	static double MedianOffset(const TimeSample *samples, std::size_t count);

	/**
	 * @brief 给定一批样本求最小二乘速率比（纯函数，便于测试）。
	 */
	static double ComputeRateRatio(const TimeSample *samples, std::size_t count);

private:
	/// 取环形缓冲中按时间顺序排列的有效样本快照（调用方无需持锁）。
	std::array<TimeSample, kCalibratorRingSize> SnapshotOrdered(std::size_t &outCount) const;

	mutable std::mutex mutex_;
	std::array<TimeSample, kCalibratorRingSize> ring_{};
	std::size_t head_ = 0;  ///< 下一个写入位置
	std::size_t count_ = 0; ///< 已写入且未被覆盖的样本数（上限为环形容量）
};

/**
 * @brief 视频时间戳 ↔ QPC 校准器（OBS raw-video 输出回调方案）。
 *
 * raw-video 回调挂在 libobs 的【全局合成视频输出】上（obs_add_raw_video
 * 回调的是最终输出帧，不依赖任何具体 source）。回调运行在 libobs 视频
 * 线程，只读取帧 pts 并采样，不触碰像素数据。
 *
 * t=0 与录制时长：真实录制起点以后续 SessionManager 用 frontend 录制
 * 事件调用 MarkRecordingStart/End 为准。OBS 编码器缓冲造成的恒定首帧
 * 偏移已被 offset（qpc - video）中的常数项吸收，无需单独处理。
 *
 * 线程安全：内部 mutex 保护估计器与录制标记，视频线程写、查询线程读。
 */
class VideoTimeCalibrator {
public:
	VideoTimeCalibrator();
	~VideoTimeCalibrator();

	VideoTimeCalibrator(const VideoTimeCalibrator &) = delete;
	VideoTimeCalibrator &operator=(const VideoTimeCalibrator &) = delete;

	/**
	 * @brief 注册 raw-video 回调开始采样。
	 *
	 * 构造 video_scale_info（NV12、宽高 0 表示沿用视频输出尺寸、
	 * colorspace/range 用 DEFAULT），调用 obs_add_raw_video_callback。
	 * @return true 已注册（或本就在采样）；false OBS 视频信息不可用或异常。
	 */
	bool Start();

	/// 移除 raw-video 回调，停止采样（重复调用安全）。
	void Stop();

	/// 临时移除 raw-video 回调（为 obs_frontend_reset_video 让路），
	/// 保持 running_ 与已采集样本不变；可重复调用。
	void Suspend();

	/// 恢复 Suspend 移除的 raw-video 回调；未 Start 或未 Suspend 时为空操作。
	/// @return 当前回调是否处于已注册状态。
	bool Resume();

	/// 是否正在接收 raw-video 回调。
	bool IsRunning() const;

	/// 样本数 ≥ 2，可进行时钟换算。
	bool Ready() const;

	/// 当前已采集样本数。
	std::size_t SampleCount() const;

	/**
	 * @brief 视频时间（秒）-> QPC 时间（秒）。
	 * @return videoSec + offset；未就绪返回 NaN，调用方必须先查 Ready()。
	 */
	double VideoTimeToQpc(double videoTimeSec) const;

	/**
	 * @brief QPC 时间（秒）-> 视频时间（秒）。
	 * @return qpcSec - offset；未就绪返回 NaN，调用方必须先查 Ready()。
	 */
	double QpcToVideoTime(double qpcSec) const;

	/**
	 * @brief 记录录制起点（QPC 秒）。由后续 SessionManager 在收到
	 *        frontend 录制开始事件时调用。
	 */
	void MarkRecordingStart(double qpcSec);

	/**
	 * @brief 记录录制终点（QPC 秒）。
	 */
	void MarkRecordingEnd(double qpcSec);

	/**
	 * @brief 录制起点对应的视频时间（秒），即视频时钟域下的 t=0。
	 * @return 未标记起点或未就绪时返回 NaN（调用方先查 Ready()）。
	 */
	double RecordingVideoZero() const;

	/**
	 * @brief 把任意 QPC 时刻换算为相对录制起点的会话时间（秒）。
	 * @return qpcSec - startQpc；未标记起点时返回 NaN（不要求 Ready，
	 *         因为会话时间只做 QPC 差值，与视频偏移无关）。
	 */
	double ToSessionTime(double qpcSec) const;

	/**
	 * @brief 最近一次录制的时长（秒）= endQpc - startQpc。
	 * @return 缺少起点或终点标记时返回 NaN。
	 */
	double LastDurationSeconds() const;

private:
	/// libobs 视频线程回调（静态转发到 OnRawVideo）。
	static void RawVideoCallback(void *param, video_data *frame);

	/// 实际处理一帧：纳秒 pts -> 秒，采样，并在样本积累后输出一次诊断日志。
	void OnRawVideo(const video_data &frame);

	OffsetEstimator estimator_;
	mutable std::mutex stateMutex_; ///< 保护 running_ 与录制起止标记（const 查询方法也要加锁）
	bool running_ = false;
	bool callbackAttached_ = false; ///< raw-video 回调当前是否已注册（Suspend/Resume 切换）

	/// 上次输出诊断日志时的样本数，用于避免环形缓冲饱和后每帧重复报告（Bug 3）
	std::atomic<std::size_t> lastDiagCount_{0};

	bool startMarked_ = false;
	bool endMarked_ = false;
	double startQpc_ = 0.0;
	double endQpc_ = 0.0;
};

} // namespace grt
