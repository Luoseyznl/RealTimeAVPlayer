#include "demuxer.hpp"

#include "logger.hpp"

using namespace utils;

Demuxer::Demuxer(Type type) : type_(type) {
  LOG_INFO << "Initializing Demuxer";
}

Demuxer::~Demuxer() {
  LOG_INFO << "Destroying Demuxer";
  close();
}

bool Demuxer::open(const std::string& filename) {
  if (filename.empty()) {
    LOG_ERROR << "Invalid filename";
    return false;
  }

  // 打开输入文件并读取头信息
  AVFormatContext* temp_ctx = nullptr;
  if (avformat_open_input(&temp_ctx, filename.c_str(), nullptr, nullptr) < 0) {
    LOG_ERROR << "Could not open input file: " << filename;
    return false;
  }
  format_ctx_.reset(temp_ctx);

  // 获取流信息
  if (avformat_find_stream_info(format_ctx_.get(), nullptr) < 0) {
    LOG_ERROR << "Could not find stream information";
    close();
    return false;
  }

  // 查找目标流（根据 type_）
  for (unsigned int i = 0; i < format_ctx_->nb_streams; ++i) {
    AVStream* stream = format_ctx_->streams[i];
    if ((type_ == Type::Video &&
         stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ||
        (type_ == Type::Audio &&
         stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)) {
      stream_index_ = i;
      stream_ = stream;
      break;  // 找到第一个匹配的流
    }
  }

  if (stream_index_ < 0) {
    LOG_ERROR << "No " << (type_ == Type::Video ? "video" : "audio")
              << " stream found";
    close();
    return false;
  }

  LOG_INFO << "Opened file: " << filename << ", format: "
           << (format_ctx_->iformat ? format_ctx_->iformat->name : "unknown")
           << ", duration: " << getDuration() / 1000000.0 << " sec, "
           << (type_ == Type::Video ? "video" : "audio")
           << " stream index: " << stream_index_;

  eof_ = false;
  return true;
}

void Demuxer::close() {
  LOG_INFO << "Closing demuxer";
  format_ctx_.reset();
  stream_ = nullptr;
  stream_index_ = -1;
  eof_ = false;  // 重置 EOF 状态
}

std::unique_ptr<AVPacket> Demuxer::readNextPacket() {
  std::lock_guard<std::mutex> lock(mutex_);  // 锁保护，避免竞态
  if (!format_ctx_) {
    LOG_ERROR << "Format context is not initialized";
    return nullptr;
  }
  if (stream_index_ < 0) {
    LOG_ERROR << "No valid target stream";
    return nullptr;
  }
  // 循环读取，直到找到目标流的数据包
  while (true) {
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
      LOG_ERROR << "Could not allocate packet";
      return nullptr;
    }

    int ret = av_read_frame(format_ctx_.get(), packet);
    if (ret < 0) {
      av_packet_free(&packet);
      if (ret == AVERROR_EOF) {
        eof_ = true;
        LOG_INFO << "End of file reached";
      } else {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        LOG_ERROR << "Error reading frame: " << errbuf;
      }
      return nullptr;
    }

    if (packet->stream_index == stream_index_) {
      // 只返回目标流的数据包
      return std::unique_ptr<AVPacket>(packet);
    } else {
      // 释放非目标流的数据包，继续读取
      av_packet_free(&packet);
    }
  }
}

bool Demuxer::seek(int64_t timestamp, int flags) {
  if (!format_ctx_) {
    LOG_ERROR << "Demuxer not initialized";
    return false;
  }

  if (stream_index_ < 0) {
    LOG_ERROR << "No valid stream for seeking";
    return false;
  }

  AVStream* stream = format_ctx_->streams[stream_index_];
  if (!stream) {
    LOG_ERROR << "Invalid stream for seeking";
    return false;
  }

  // 将微秒转换为流的时间基准单位
  int64_t seek_target =
      av_rescale_q(timestamp, AV_TIME_BASE_Q, stream->time_base);

  LOG_DEBUG << "Seeking to " << timestamp
            << "us (stream timebase: " << stream->time_base.num << "/"
            << stream->time_base.den << ", target: " << seek_target << ")";

  int ret = av_seek_frame(format_ctx_.get(), stream_index_, seek_target, flags);

  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR << "Error seeking to position " << timestamp << "us: " << errbuf;
    return false;
  }

  eof_ = false;
  LOG_INFO << "Successfully seeked to " << timestamp << "us";
  return true;
}

int64_t Demuxer::getDuration() const {
  if (!format_ctx_) return 0;

  // 直接返回格式上下文的持续时间
  if (format_ctx_->duration != AV_NOPTS_VALUE) {
    return format_ctx_->duration;
  }

  // 如果格式上下文没有持续时间，尝试从流中获取
  if (stream_ && stream_->duration != AV_NOPTS_VALUE) {
    return av_rescale_q(stream_->duration, stream_->time_base, AV_TIME_BASE_Q);
  }

  return 0;
}