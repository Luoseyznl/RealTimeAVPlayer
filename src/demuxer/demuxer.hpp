#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>
#include <mutex>
#include <string>

#include "mediadefs.h"

// Custom deleter for AVFormatContext
struct AVFormatContextDeleter {
  void operator()(AVFormatContext* p) const {
    if (p) {
      avformat_close_input(&p);
    }
  }
};

/**
 * Demuxer: 根据媒体类型分离音视频流，提供读取数据包和跳转功能。
 * 读取到 EOF 后设置标志，可通过 isEOF() 查询。
 * 注意：如果多个 Demuxer 共享 AVFormatContext，需加锁保护 readNextPacket()
 * 以避免冲突。
 */
class Demuxer {
 public:
  Demuxer(Type type);
  ~Demuxer();  // 调用 close() 释放资源

  // 核心功能
  bool open(const std::string& filename);
  void close();
  std::unique_ptr<AVPacket> readNextPacket();
  bool seek(int64_t timestamp, int flags = 0);  // timestamp: 微秒(us)

  // 查询状态
  bool isEOF() const { return eof_; }
  bool isOpen() const { return format_ctx_ != nullptr; }

  // 获取属性
  AVFormatContext* getFormatContext() const { return format_ctx_.get(); }
  int getStreamIndex() const { return stream_index_; }
  AVStream* getAVStream() const { return stream_; }
  int64_t getDuration() const;

 private:
  Type type_;
  std::mutex mutex_;  // 保护 readNextPacket()，如果共享上下文则启用
  std::unique_ptr<AVFormatContext, AVFormatContextDeleter> format_ctx_;

  AVStream* stream_ = nullptr;
  int stream_index_ = -1;
  bool eof_ = false;
};