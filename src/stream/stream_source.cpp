#include "stream_source.hpp"

#include "utils/logger.hpp"

extern "C" {
#include <libavutil/frame.h>
}

using namespace utils;

// Helper 函数，按帧、包、顺序获取最佳时间戳
int64_t get_frame_pts(AVFrame* frame, AVPacket* packet) {
  if (!frame) return AV_NOPTS_VALUE;
  if (frame->pts != AV_NOPTS_VALUE) return frame->pts;
  if (packet && packet->pts != AV_NOPTS_VALUE) return packet->pts;
  return AV_NOPTS_VALUE;
}

StreamSource::StreamSource(Type type)
    : type_(type), fake_pts_(0), MAX_QUEUE_SIZE(type == Type::Video ? 30 : 50) {
  if (type == Type::Video) {
    width_ = 0;
    height_ = 0;
    frame_rate_ = 0.0;
    pixel_fmt_ = AV_PIX_FMT_NONE;
  } else {
    sample_rate_ = 0;
    channels_ = 0;
    sample_fmt_ = AV_SAMPLE_FMT_NONE;
    channel_layout_ = 0;
  }
}

StreamSource::~StreamSource() { close(); }

bool StreamSource::open(const std::string& filename) {
  LOG_INFO << "Opening " << (type_ == Type::Video ? "video" : "audio")
           << " stream from file: " << filename;

  // 1. 初始化 Demuxer 并打开文件
  demuxer_ = std::make_shared<Demuxer>(type_);
  if (!demuxer_->open(filename)) {
    LOG_ERROR << "Failed to open demuxer for file: " << filename;
    return false;
  }

  // 2. 获取流并初始化解码器
  AVStream* stream = demuxer_->getAVStream();
  if (!stream) {
    LOG_ERROR << "No valid stream found in file: " << filename;
    demuxer_->close();
    return false;
  }
  decoder_ = std::make_unique<Decoder>(type_);
  if (!initializeDecoder(stream)) {
    LOG_ERROR << "Failed to initialize decoder";
    demuxer_->close();
    decoder_->close();
    return false;
  }

  state_.store(State::Stopped);
  eof_.store(false);

  return true;
}

bool StreamSource::initializeDecoder(AVStream* stream) {
  LOG_INFO << "Initializing decoder for "
           << (type_ == Type::Video ? "video" : "audio") << " stream";
  if (!stream) {
    LOG_ERROR << "Invalid stream for decoder initialization";
    return false;
  }

  if (!decoder_->open(stream)) {
    LOG_ERROR << "Failed to open decoder";
    return false;
  }

  // Set stream properties
  const auto& config = decoder_->getConfig();
  if (type_ == Type::Video) {
    width_ = config.width;
    height_ = config.height;
    pixel_fmt_ = config.pixel_format;
    // 优先使用平均帧率（avg_frame_rate），如果不可用再使用实际帧率（r_frame_rate）
    if (stream->avg_frame_rate.den && stream->avg_frame_rate.num) {
      frame_rate_ = static_cast<double>(stream->avg_frame_rate.num) /
                    static_cast<double>(stream->avg_frame_rate.den);
    } else if (stream->r_frame_rate.den && stream->r_frame_rate.num) {
      frame_rate_ = static_cast<double>(stream->r_frame_rate.num) /
                    static_cast<double>(stream->r_frame_rate.den);
    } else {
      frame_rate_ = 0.0;
    }
    LOG_INFO << "Video stream opened: " << width_ << "x" << height_
             << ", frame rate: " << frame_rate_;
  } else {
    sample_rate_ = config.sample_rate;
    channels_ = config.channels;
    sample_fmt_ = config.sample_format;

    // Get channel layout from decoder context if available
    auto codec_ctx = decoder_->getCodecContext();
    if (codec_ctx) {
      if (codec_ctx->ch_layout.nb_channels > 0) {
        channel_layout_ = codec_ctx->ch_layout.u.mask;
      } else {
        // Fallback: set a simple default based on channels
        channel_layout_ =
            (channels_ == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
      }
    } else {
      // Fallback to codecpar if codec_ctx is not available
      if (stream->codecpar->ch_layout.nb_channels > 0) {
        channel_layout_ = stream->codecpar->ch_layout.u.mask;
      } else {
        channel_layout_ =
            (channels_ == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
      }
    }

    LOG_INFO << "Audio stream opened: " << sample_rate_
             << " Hz, channels: " << channels_
             << ", sample format: " << av_get_sample_fmt_name(sample_fmt_);
  }

  return true;
}

void StreamSource::startDecoding() {
  if (state_.load() == State::Running) {
    LOG_WARN << "StreamSource is already running";
    return;
  }
  // 从 Stopped 或 Paused 状态启动 decodingLoop 线程
  LOG_INFO << "Starting " << (type_ == Type::Video ? "video" : "audio")
           << " decoding thread";
  if (state_.load() == State::Stopped) {
    clearFrameQueue();
    eof_.store(false);
  }
  state_.store(State::Running);
  decoding_thread_ = std::thread(&StreamSource::decodingLoop, this);
}

void StreamSource::decodingLoop() {
  int packet_count = 0;  // 用于调试日志，记录处理的包数量

  while (state_.load() != State::Stopped) {
    if (state_.load() == State::Paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // Throttle if queue is full
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      if (frame_queue_.size() >= MAX_QUEUE_SIZE) {
        queue_cond_.wait(lock, [this]() {
          return frame_queue_.size() < MAX_QUEUE_SIZE ||
                 state_.load() != State::Running;
        });
        continue;  // Re-check state 避免在解码线程暂停时继续处理
      }
    }

    // 1. Read next packet from demuxer
    auto packet = demuxer_->readNextPacket();  // 智能指针管理
    if (!packet) {
      if (demuxer_->isEOF()) {
        eof_.store(true);
        LOG_INFO << (type_ == Type::Video ? "Video" : "Audio")
                 << " stream reached EOF";
        processPacket(nullptr);  // Flush decoder

        // Wait for all frames to be decoded before stopping
        {
          std::unique_lock<std::mutex> lock(queue_mutex_);
          queue_cond_.wait(lock, [this]() {
            return frame_queue_.empty() || state_.load() != State::Running;
          });

          if (frame_queue_.empty()) {
            state_.store(State::Stopped);
            LOG_INFO << (type_ == Type::Video ? "Video" : "Audio")
                     << " stream decoding completed";
            break;  // 读到数据包尽头也没有剩余帧了，退出解码循环
          } else {
            LOG_INFO << (type_ == Type::Video ? "Video" : "Audio")
                     << " stream has remaining frames in queue";
            // 仍可能有剩余帧，但状态切换了，继续处理
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    processPacket(packet.get());  // 传递裸指针，但 packet 自动释放

    if (++packet_count % 30 == 0) {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      LOG_DEBUG << "Processed " << packet_count << " packets for "
                << (type_ == Type::Video ? "video" : "audio") << " stream "
                << frame_queue_.size() << "/" << MAX_QUEUE_SIZE
                << " frames in queue";
    }
  }
}

void StreamSource::pauseDecoding() {
  State expected = State::Running;
  // 原子比较并交换
  if (state_.compare_exchange_strong(expected, State::Paused)) {
    LOG_INFO << (type_ == Type::Video ? "Video" : "Audio") << " reader paused";
  }
}

void StreamSource::resumeDecoding() {
  State expected = State::Paused;
  if (state_.compare_exchange_strong(expected, State::Running)) {
    LOG_INFO << (type_ == Type::Video ? "Video" : "Audio") << " reader resumed";
  }
}

void StreamSource::stopDecoding() {
  state_.store(State::Stopped);
  queue_cond_.notify_all();
  LOG_INFO << (type_ == Type::Video ? "Video" : "Audio") << " reader stopped";
}

void StreamSource::close() {
  stopDecoding();
  if (decoding_thread_.joinable()) {
    decoding_thread_.join();
  }
  clearFrameQueue();
  if (decoder_ && decoder_->isOpen()) {
    decoder_->close();
    decoder_.reset();  // 智能指针释放
  }
  if (demuxer_) {
    demuxer_->close();
    demuxer_.reset();  // 智能指针释放
  }

  state_.store(State::Stopped);
  eof_.store(false);
  LOG_INFO << (type_ == Type::Video ? "Video" : "Audio")
           << " StreamSource closed";
}

std::shared_ptr<StreamSource::Frame> StreamSource::getNextFrame() {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  if (frame_queue_.empty()) {
    return nullptr;  // No frame available
  }
  auto frame = frame_queue_.front();
  frame_queue_.pop();
  queue_cond_.notify_all();  // Notify decoding thread in case it was waiting
  return frame;
}

void StreamSource::processPacket(AVPacket* packet) {
  if (!decoder_ || !decoder_->isOpen()) {
    LOG_ERROR << "Decoder is not initialized";
    return;
  }

  // 1. Send packet to decoder (packet can be nullptr to flush)
  int ret = decoder_->decodePacket(packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR << "Error sending packet to decoder: " << errbuf;
    return;  // packet 由调用者释放
  }

  // 2. Receive all available frames from decoder
  while (true) {
    auto raw_frame = decoder_->receiveFrame();
    if (!raw_frame) {
      break;  // No more frames
    }

    int64_t pts_src = get_frame_pts(raw_frame.get(), packet);  // 原始时间戳
    int64_t pts = AV_NOPTS_VALUE;
    if (pts_src != AV_NOPTS_VALUE && demuxer_) {
      pts = av_rescale_q(pts_src, getTimeBase(), AV_TIME_BASE_Q);
    }

    int64_t duration = 0;
    if (type_ == Type::Video) {
      if (raw_frame->duration > 0) {
        duration =
            av_rescale_q(raw_frame->duration, getTimeBase(), AV_TIME_BASE_Q);
      } else if (frame_rate_ > 0.0) {
        duration = static_cast<int64_t>(AV_TIME_BASE / frame_rate_);
      }
    } else {
      if (raw_frame->nb_samples > 0 && sample_rate_ > 0) {
        duration = static_cast<int64_t>(
            static_cast<double>(raw_frame->nb_samples) /
            static_cast<double>(sample_rate_) * AV_TIME_BASE);
      }
    }

    // 伪时间戳 += 帧持续时间或默认帧间隔
    if (pts == AV_NOPTS_VALUE) {
      pts = av_rescale_q(fake_pts_, AV_TIME_BASE_Q, AV_TIME_BASE_Q);
      fake_pts_ += duration > 0
                       ? duration
                       : (type_ == Type::Video
                              ? static_cast<int64_t>(AV_TIME_BASE / 30)
                              : static_cast<int64_t>(AV_TIME_BASE / 50));
      LOG_WARN << "Frame has no valid PTS, assigning fake PTS: " << pts;
    }

    // clone frame and push to queue
    AVFrame* frame_clone = av_frame_clone(raw_frame.get());
    if (!frame_clone) {
      LOG_ERROR << "Could not clone frame";
      continue;
    }

    auto shared_frame = std::shared_ptr<AVFrame>(
        frame_clone, [](AVFrame* f) { av_frame_free(&f); });  // 自定义 deleter
    // 将 AVFrame 包装到 Frame 结构体中 (包含 pts 和 duration)
    auto wrapped_frame = std::make_shared<Frame>(shared_frame, pts, duration);
    pushFrameToQueue(wrapped_frame);
  }
}

void StreamSource::pushFrameToQueue(std::shared_ptr<Frame> frame) {
  if (!frame) return;

  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (frame_queue_.size() >= MAX_QUEUE_SIZE) {
    LOG_WARN << "Frame queue is full, dropping frame with PTS: " << frame->pts;
    frame.reset();  // Explicitly release to avoid memory leak
    return;
  }
  frame_queue_.push(frame);
  queue_cond_.notify_one();
}

void StreamSource::clearFrameQueue() {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  while (!frame_queue_.empty()) {
    frame_queue_.pop();
  }
  queue_cond_.notify_all();  // Notify decoding thread in case it was waiting
}

bool StreamSource::seek(int64_t timestamp) {
  LOG_INFO << "seek to " << timestamp;
  if (timestamp < 0 || timestamp > getDuration()) {
    LOG_ERROR << "Seek timestamp out of range: " << timestamp
              << ", duration: " << getDuration();
    return false;
  }

  if (!demuxer_) {
    LOG_ERROR << "No demuxer available";
    return false;
  }

  int seek_flags = AVSEEK_FLAG_BACKWARD;
  if (!demuxer_->seek(timestamp, seek_flags)) {
    LOG_ERROR << "Error seeking to position: " << timestamp;
    return false;
  }

  // Reset decoder and queues
  decoder_->flush();
  clearFrameQueue();
  eof_.store(false);
  fake_pts_ = 0;  // Reset fake PTS to avoid timestamp errors

  // Decode forward until we reach a frame at/after timestamp
  int packet_count = 0;
  int frames_queued_after_target = 0;
  while (true) {
    auto packet = demuxer_->readNextPacket();
    if (!packet) {
      LOG_WARN << "seek: no packet post-seek";
      break;
    }

    if (packet->stream_index != demuxer_->getStreamIndex()) {
      continue;  // packet 自动释放
    }

    if (decoder_->decodePacket(packet.get()) < 0) {
      LOG_ERROR << "Error sending packet to decoder during seek";
      return false;
    }

    packet_count++;
    if (packet_count % 30 == 0) {
      LOG_INFO << "Seek: processed " << packet_count << " packets";
    }

    while (true) {
      auto avframe = decoder_->receiveFrame();
      if (!avframe) {
        break;
      }

      int64_t pts_src = get_frame_pts(avframe.get(), nullptr);
      int64_t pts = (pts_src != AV_NOPTS_VALUE)
                        ? av_rescale_q(pts_src, getTimeBase(), AV_TIME_BASE_Q)
                        : AV_NOPTS_VALUE;
      if (pts == AV_NOPTS_VALUE) {
        continue;
      }

      int64_t duration = 0;
      if (type_ == Type::Video) {
        if (avframe->duration > 0)
          duration =
              av_rescale_q(avframe->duration, getTimeBase(), AV_TIME_BASE_Q);
        else if (frame_rate_ > 0.0)
          duration = static_cast<int64_t>(AV_TIME_BASE / frame_rate_);
      } else {
        int sr =
            sample_rate_ > 0 ? sample_rate_ : decoder_->getConfig().sample_rate;
        if (sr > 0)
          duration =
              (static_cast<int64_t>(avframe->nb_samples) * AV_TIME_BASE) / sr;
      }

      AVFrame* frame_clone = av_frame_clone(avframe.get());
      if (!frame_clone) {
        LOG_ERROR << "Could not clone frame during seek";
        continue;
      }

      auto shared_frame = std::shared_ptr<AVFrame>(
          frame_clone, [](AVFrame* f) { av_frame_free(&f); });
      auto frame_wrapper = std::make_shared<Frame>(shared_frame, pts, duration);

      LOG_DEBUG << "Seek decoded frame pts: " << pts
                << ", target: " << timestamp;

      if (pts >= timestamp) {
        LOG_DEBUG << "Seek: queuing frame with PTS: " << pts;

        pushFrameToQueue(frame_wrapper);
        frames_queued_after_target++;
        if (frames_queued_after_target >= 5) {
          LOG_DEBUG << "Seek completed, queued " << frames_queued_after_target
                    << " frames";
          return true;
        }
      }
    }
  }

  return true;
}

int64_t StreamSource::getCurrentTimestamp() const {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!frame_queue_.empty()) {
    return frame_queue_.front()->pts;
  }
  return 0;
}

AVRational StreamSource::getTimeBase() const {
  return demuxer_->getAVStream()->time_base;
}