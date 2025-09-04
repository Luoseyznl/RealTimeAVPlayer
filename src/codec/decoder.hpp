#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include <memory>
#include <mutex>
#include <string>

#include "mediadefs.h"

/**
 * Decoder: 封装 FFmpeg 解码器，支持音频/视频解码、重采样和转换。
 * 可通过继承重写 configureCodec() 自定义解码器配置。
 * 注意：多线程使用时需使用内部 mutex 同步
 */
class Decoder {
 public:
  struct Config {
    Type type;
    // 音频参数
    int sample_rate = 0;
    int channels = 0;
    AVSampleFormat sample_format = AV_SAMPLE_FMT_NONE;
    // 视频参数
    int width = 0;
    int height = 0;
    AVPixelFormat pixel_format = AV_PIX_FMT_NONE;
  };

  Decoder(Type type);
  virtual ~Decoder();

  // 用 Demuxer 获取 AVStream 指针初始化解码器上下文
  bool open(AVStream* stream);

  // 释放所有资源，可外部调用或析构时自动调用。
  void close();

  // 解码 Demuxer 读取的 AVPacket
  int decodePacket(AVPacket* packet);

  // 接收解码帧：从解码器获取解码后的帧。
  std::unique_ptr<AVFrame> receiveFrame();

  // 刷新解码器：处理缓冲区剩余数据，通常在 EOF 时调用。
  void flush();

  const Config& getConfig() const { return config_; }
  AVCodecContext* getCodecContext() const { return codec_ctx_.get(); }
  bool isOpen() const { return codec_ctx_ != nullptr; }

 protected:
  virtual bool configureCodec(AVStream* stream);

 private:
  // 自定义删除器
  struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
      if (ctx) {
        avcodec_free_context(&ctx);
      }
    }
  };

  struct SwrContextDeleter {
    void operator()(SwrContext* ctx) const {
      if (ctx) {
        swr_free(&ctx);
      }
    }
  };

  bool initializeCodec(AVStream* stream);  // 设置 AVCodecContext, SwrContext。
  void releaseCodec();                     // 释放 AVCodecContext, SwrContext。

  Type type_;
  Config config_;
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_ctx_;
  std::unique_ptr<SwrContext, SwrContextDeleter> swr_ctx_;
  std::mutex mutex_;
};