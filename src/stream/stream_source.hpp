#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "decoder.hpp"
#include "demuxer.hpp"

class StreamSource {
 public:
  // 音频 / 视频帧
  struct Frame {
    std::shared_ptr<AVFrame> frame;
    int64_t pts;
    int64_t duration;

    Frame(std::shared_ptr<AVFrame> f, int64_t p, int64_t d)
        : frame(f), pts(p), duration(d) {}
  };

  // 流状态
  enum class State { Stopped, Paused, Running };

  StreamSource(Type type);
  ~StreamSource();

  bool open(const std::string& filename);
  void close();  // 负责所有资源释放，可外部调用或析构使用

  void startDecoding();   // 启动解码线程 decodingLoop
  void pauseDecoding();   // 暂停解码线程
  void resumeDecoding();  // 恢复解码线程
  void stopDecoding();    // 停止解码线程

  bool seek(int64_t timestamp);           // 流级别跳转，单位微秒(us)
  std::shared_ptr<Frame> getNextFrame();  // 从队列中获取下一帧
  int64_t getCurrentTimestamp() const;    // 获取当前播放时间戳，单位微秒(us)

  bool isEOF() const { return eof_; }

  // Video properties
  int getWidth() const { return width_; }
  int getHeight() const { return height_; }
  double getFrameRate() const { return frame_rate_; }
  AVPixelFormat getPixelFormat() const { return pixel_fmt_; }

  // Audio properties
  int getSampleRate() const { return sample_rate_; }
  int getChannels() const { return channels_; }
  int64_t getChannelLayout() const { return channel_layout_; }
  AVSampleFormat getSampleFormat() const { return sample_fmt_; }

  // Duration in us
  int64_t getDuration() const { return demuxer_ ? demuxer_->getDuration() : 0; }
  AVRational getTimeBase() const;  // 直接获取流的时间基准

 private:
  bool initializeDecoder(AVStream* stream);  // 初始化解码器
  void processPacket(AVPacket* packet);      // 对原始数据包进行解码处理
  void decodingLoop();                       // 解码线程主循环
  void pushFrameToQueue(
      std::shared_ptr<Frame> frame);  // 将解码后的数据帧推入队列
  void clearFrameQueue();             // 清空帧队列

  int64_t calculateFrameDuration(AVFrame* frame);  // 计算帧持续时间

  Type type_;
  int64_t fake_pts_;  // 只在无效PTS时用，不影响其他逻辑

  // Video stream properties
  int width_ = 0;
  int height_ = 0;
  double frame_rate_ = 0.0;
  AVPixelFormat pixel_fmt_ = AV_PIX_FMT_NONE;

  // Audio stream properties
  int sample_rate_ = 0;
  int channels_ = 0;
  AVSampleFormat sample_fmt_ = AV_SAMPLE_FMT_NONE;
  int64_t channel_layout_ = 0;

  // Common components
  std::unique_ptr<Decoder> decoder_;  // Decoder 依赖独立的上下文、状态、缓冲区
  std::shared_ptr<Demuxer> demuxer_;  // Demuxer 仅负责解析媒体文件，可以共享

  // Thread management
  std::thread decoding_thread_;
  std::atomic<State> state_{State::Stopped};
  std::atomic<bool> eof_{false};

  // Frame queue
  std::queue<std::shared_ptr<Frame>> frame_queue_;  // 解码后的帧队列
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cond_;
  const size_t MAX_QUEUE_SIZE;
};