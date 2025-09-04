#include "decoder.hpp"

#include "logger.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

using namespace utils;

Decoder::Decoder(Type type)
    : type_(type), codec_ctx_(nullptr), swr_ctx_(nullptr) {
  LOG_INFO << "Initializing Decoder";
}

Decoder::~Decoder() {
  LOG_INFO << "Destroying Decoder";
  close();
}

// 打开并初始化解码器，及配置解码器参数
bool Decoder::open(AVStream* stream) {
  if (!stream) {
    LOG_ERROR << "Invalid stream";
    return false;
  }

  bool is_video = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
  bool is_audio = stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
  if ((type_ == Type::Video && !is_video) ||
      (type_ == Type::Audio && !is_audio)) {
    LOG_ERROR << "Stream type mismatch";
    return false;
  }

  return initializeCodec(stream);
}

bool Decoder::initializeCodec(AVStream* stream) {
  // 1. 根据流的编码参数找到合适的解码器
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    LOG_ERROR << "Codec not found";
    return false;
  }
  // 2. 分配并初始化解码器上下文
  AVCodecContext* temp_ctx = avcodec_alloc_context3(codec);
  if (!temp_ctx) {
    LOG_ERROR << "Codec context allocation failed";
    return false;
  }
  codec_ctx_.reset(temp_ctx);  // 使用智能指针管理

  // 3. 将流的参数复制到解码器上下文
  if (avcodec_parameters_to_context(codec_ctx_.get(), stream->codecpar) < 0) {
    LOG_ERROR << "Copy codec parameters to context failed";
    releaseCodec();
    return false;
  }

  // 4. 打开解码器
  if (avcodec_open2(codec_ctx_.get(), codec, nullptr) < 0) {
    LOG_ERROR << "Codec opening failed";
    releaseCodec();
    return false;
  }

  // 5. 配置解码器参数
  if (!configureCodec(stream)) {
    LOG_ERROR << "Configure codec failed";
    releaseCodec();
    return false;
  }

  if (type_ == Type::Video) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(
        static_cast<AVPixelFormat>(stream->codecpar->format));
    LOG_INFO << "Video codec opened: " << codec->name
             << ", resolution: " << codec_ctx_->width << "x"
             << codec_ctx_->height
             << ", pixel format: " << (desc ? desc->name : "unknown");
  } else {
    LOG_INFO << "Audio codec opened: " << codec->name
             << ", sample rate: " << codec_ctx_->sample_rate
             << ", channels: " << codec_ctx_->ch_layout.nb_channels
             << ", sample format: "
             << av_get_sample_fmt_name(codec_ctx_->sample_fmt);
  }
  return true;
}

bool Decoder::configureCodec(AVStream* stream) {
  config_.type = type_;
  if (type_ == Type::Audio) {
    config_.sample_rate = stream->codecpar->sample_rate;
    config_.channels = stream->codecpar->ch_layout.nb_channels;
    config_.sample_format =
        static_cast<AVSampleFormat>(stream->codecpar->format);
    if (!swr_ctx_) {
      SwrContext* temp_swr = swr_alloc();
      if (!temp_swr) {
        LOG_ERROR << "Could not allocate resampler context";
        return false;
      }
      swr_ctx_.reset(temp_swr);
      // 音频解码后可能需要重采样（格式转换、采样率转换、声道数转换）
    }
  } else {
    config_.width = stream->codecpar->width;
    config_.height = stream->codecpar->height;
    config_.pixel_format = static_cast<AVPixelFormat>(stream->codecpar->format);
  }
  return true;
}

void Decoder::close() {
  LOG_INFO << "Closing decoder";
  swr_ctx_.reset();
  releaseCodec();
}

void Decoder::releaseCodec() { codec_ctx_.reset(); }

// 发送压缩数据包到解码器的输入缓冲区（packet 可能为 nullptr，表示刷新解码器）
int Decoder::decodePacket(AVPacket* packet) {
  std::lock_guard<std::mutex> lock(mutex_);  // 锁保护，避免竞态
  if (!codec_ctx_) {
    LOG_ERROR << "Codec context is not initialized";
    return AVERROR(EINVAL);
  }

  if (!packet) {
    // 发送空包表示刷新解码器
    LOG_DEBUG << "Sending flush packet to decoder";
    int ret = avcodec_send_packet(codec_ctx_.get(), nullptr);
    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      LOG_ERROR << "Error sending flush packet to decoder: " << errbuf;
      return ret;
    }
    return 0;
  }

  if (packet->size <= 0) {
    LOG_WARN << "Warning: Sending empty packet to decoder";
    return AVERROR(EINVAL);  // 返回错误码，避免发送空包
  }

  int ret = avcodec_send_packet(codec_ctx_.get(), packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR << "Error sending packet to decoder: " << errbuf;
    return ret;
  }

  LOG_DEBUG << "Packet sent to decoder, size: " << packet->size;
  return 0;
}

// 接收解码后的帧，存到 frame 指向的 AVFrame 结构体中
std::unique_ptr<AVFrame> Decoder::receiveFrame() {
  std::lock_guard<std::mutex> lock(mutex_);  // 锁保护，避免竞态
  if (!codec_ctx_) {
    LOG_ERROR << "Codec context is not initialized";
    return nullptr;
  }

  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    LOG_ERROR << "Failed to allocate frame";
    return nullptr;
  }

  int ret = avcodec_receive_frame(codec_ctx_.get(), frame);
  if (ret == AVERROR(EAGAIN)) {
    // 需要更多数据包才能解码出帧
    av_frame_free(&frame);
    LOG_DEBUG << "Decoder needs more packets to produce a frame";
    return nullptr;
  } else if (ret == AVERROR_EOF) {
    // 解码器已经完全刷新，无法再输出帧
    av_frame_free(&frame);
    LOG_DEBUG << "Decoder has been fully flushed, no more frames";
    return nullptr;
  } else if (ret < 0) {
    av_frame_free(&frame);
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR << "Error receiving frame from decoder: " << errbuf;
    return nullptr;
  }

  LOG_DEBUG << "Frame received from decoder, pts: " << frame->pts;
  return std::unique_ptr<AVFrame>(frame);  // 返回智能指针，自动释放
}

void Decoder::flush() {
  std::lock_guard<std::mutex> lock(mutex_);  // 锁保护
  LOG_INFO << "Flushing decoder";
  if (!codec_ctx_) {
    LOG_WARN << "Codec context is not initialized, cannot flush";
    return;
  }
  // 发送 nullptr packet 来 flush
  avcodec_flush_buffers(codec_ctx_.get());
}
