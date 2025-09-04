#pragma once

#include <SDL2/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include "stream_source.hpp"

// Custom deleter for SwrContext
struct SwrContextDeleter {
  void operator()(SwrContext* p) const {
    if (p) {
      swr_free(&p);
    }
  }
};

/**
 * AudioPlayer: 封装 SDL 音频播放器，用于实时音频帧播放。
 * 支持 PCM 数据缓冲、音量控制、时钟同步和线程安全操作。
 * 使用 SDL 处理音频输出，SwrContext 处理重采样。
 */
class AudioPlayer {
 public:
  AudioPlayer();
  ~AudioPlayer();

  // 初始化音频播放器：设置音频源和 SDL 设备。
  bool initialize(std::shared_ptr<StreamSource> audio_reader);
  void pause();   // 暂停播放
  void resume();  // 恢复播放
  void stop();    // 停止播放
  void clear();   // 清空缓冲区

  int64_t getAudioClock() const { return audio_clock_.load(); }
  void resetClock(int64_t pts) noexcept;

  bool isPaused() const { return paused_.load(); }

  void setVolume(double norm);  // norm: 0.0 ~ 1.0
  double getVolume() const;

 private:
  // SDL 音频回调函数：填充音频数据。
  static void audioCallback(void* userdata, uint8_t* stream, int len);
  void fillAudioData(uint8_t* stream, int len);  // 填充音频数据到 SDL 缓冲区。
  void producerThreadLoop();  // 生产者线程循环：从音频源拉取帧并转换。

  void convertPlanarToInterleaved(  // 将平面音频帧转换为交织格式。
      const AVFrame* frame, std::vector<uint8_t>& out);

  size_t pushPCMData(const uint8_t* data, size_t bytes);
  size_t popPCMData(uint8_t* out, size_t bytes);

  // 音频源和上下文
  std::shared_ptr<StreamSource> audio_reader_;  // 音频流源
  std::unique_ptr<SwrContext, SwrContextDeleter> swr_ctx_{nullptr,
                                                          SwrContextDeleter{}};

  SDL_AudioDeviceID audio_dev_{0};  // SDL 音频设备 ID

  // PCM 环形缓冲区
  mutable std::mutex pcm_mutex_;       // PCM 缓冲区锁
  std::vector<uint8_t> pcm_ring_buf_;  // 环形缓冲区
  size_t pcm_ring_cap_ = 0;            // 环形缓冲区容量
  uint64_t pcm_ring_read_ = 0;         // 读位置
  uint64_t pcm_ring_write_ = 0;        // 写位置

#ifndef NDEBUG
  std::ofstream pcm_out_;  // 调试：保存 PCM 数据（仅调试模式）
#endif

  // 线程和状态
  std::thread producer_thread_;                 // 生产者线程
  std::atomic<bool> pulling_{false};            // 是否正在拉取音频数据
  std::atomic<bool> paused_{false};             // 播放是否暂停
  std::atomic<bool> stop_{false};               // 是否停止播放
  std::atomic<bool> playback_finished_{false};  // 播放是否结束

  // 音频参数
  int sample_rate_ = 44100;                         // 音频采样率
  int channels_ = 2;                                // 音频通道数
  AVSampleFormat sample_fmt_ = AV_SAMPLE_FMT_NONE;  // 音频采样格式
  int64_t channel_layout_ = AV_CH_LAYOUT_STEREO;    // 通道布局
  std::atomic<int> volume_{
      SDL_MIX_MAXVOLUME};  // 音量，范围 0~SDL_MIX_MAXVOLUME

  // 时钟同步
  std::atomic<int64_t> audio_clock_;          // 音频时钟，单位微秒 (us)
  std::atomic<int64_t> base_pts_{0};          // 缓冲区基准时间戳，单位微秒 (us)
  std::atomic<int64_t> consumed_samples_{0};  // 已消耗音频样本数
};