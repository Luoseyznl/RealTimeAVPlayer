#include "audio_player.hpp"

#include <algorithm>
#include <cstring>

#include "libavutil/avutil.h"
#include "logger.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

using namespace utils;

AudioPlayer::AudioPlayer()
    : swr_ctx_(nullptr, SwrContextDeleter{}),  // 修复：使用自定义删除器
      audio_dev_(0),
      pcm_ring_cap_(0),
      pcm_ring_read_(0),
      pcm_ring_write_(0),
      pulling_(false),
      paused_(false),
      stop_(false),
      playback_finished_(false),
      sample_rate_(44100),
      channels_(2),
      sample_fmt_(AV_SAMPLE_FMT_NONE),
      channel_layout_(AV_CH_LAYOUT_STEREO),
      volume_(SDL_MIX_MAXVOLUME),
      audio_clock_(0),
      base_pts_(0),
      consumed_samples_(0) {}

AudioPlayer::~AudioPlayer() {
  stop();
#ifndef NDEBUG
  if (pcm_out_.is_open()) {
    pcm_out_.close();
  }
#endif
}

bool AudioPlayer::initialize(std::shared_ptr<StreamSource> audio_reader) {
  if (!audio_reader) {
    LOG_ERROR << "AudioReader is null";
    return false;
  }
  audio_reader_ = std::move(audio_reader);

  // 获取音频参数
  sample_rate_ = audio_reader_->getSampleRate();
  channels_ = audio_reader_->getChannels();
  sample_fmt_ = audio_reader_->getSampleFormat();
  channel_layout_ = audio_reader_->getChannelLayout();
  if (sample_rate_ <= 0 || channels_ <= 0 ||
      sample_fmt_ == AV_SAMPLE_FMT_NONE) {
    LOG_ERROR << "Invalid audio parameters";
    return false;
  }

  // 初始化 SDL 音频子系统
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
      LOG_ERROR << "Failed to initialize SDL audio: " << SDL_GetError();
      return false;
    }
  }

  // 打开音频设备并校验参数
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = sample_rate_;
  want.format = AUDIO_S16SYS;
  want.channels = static_cast<Uint8>(channels_);
  want.samples = 1024;
  want.callback = audioCallback;
  want.userdata = this;

  audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (audio_dev_ == 0) {
    LOG_ERROR << "Failed to open audio device: " << SDL_GetError();
    return false;
  }
  if (have.freq != want.freq || have.format != want.format ||
      have.channels != want.channels) {
    LOG_ERROR << "Audio device returned different spec than requested";
    SDL_CloseAudioDevice(audio_dev_);
    audio_dev_ = 0;
    return false;
  }

  // 初始化重采样上下文
  SwrContext* new_ctx = swr_alloc();
  if (!new_ctx) {
    LOG_ERROR << "Failed to allocate SwrContext";
    SDL_CloseAudioDevice(audio_dev_);
    audio_dev_ = 0;
    return false;
  }

  av_opt_set_int(new_ctx, "in_channel_layout", channel_layout_, 0);
  av_opt_set_int(new_ctx, "in_sample_rate", sample_rate_, 0);
  av_opt_set_sample_fmt(new_ctx, "in_sample_fmt", sample_fmt_, 0);
  av_opt_set_int(new_ctx, "out_channel_layout", channel_layout_, 0);
  av_opt_set_int(new_ctx, "out_sample_rate", sample_rate_, 0);
  av_opt_set_sample_fmt(new_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
  if (swr_init(new_ctx) < 0) {
    LOG_ERROR << "Failed to initialize SwrContext";
    swr_free(&new_ctx);
    SDL_CloseAudioDevice(audio_dev_);
    audio_dev_ = 0;
    return false;
  }
  swr_ctx_.reset(new_ctx);  // 使用智能指针

  // 分配环形缓冲区（2秒音频）
  int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
  int buffer_bytes = sample_rate_ * channels_ * bytes_per_sample * 2;
  if (buffer_bytes < 4096) buffer_bytes = 4096;
  pcm_ring_cap_ = buffer_bytes;
  pcm_ring_buf_.resize(pcm_ring_cap_, 0);
  pcm_ring_read_ = 0;
  pcm_ring_write_ = 0;

  // 启动生产者线程
  pulling_ = true;
  producer_thread_ = std::thread(&AudioPlayer::producerThreadLoop, this);

  // 启动音频播放
  SDL_PauseAudioDevice(audio_dev_, 0);

  LOG_INFO << "AudioPlayer initialized: freq=" << sample_rate_
           << " channels=" << channels_ << " buffer=" << pcm_ring_cap_
           << " bytes";
  return true;
}

void AudioPlayer::audioCallback(void* userdata, uint8_t* stream, int len) {
  auto* player = static_cast<AudioPlayer*>(userdata);
  if (!player || !player->audio_dev_) {
    std::memset(stream, 0, len);  // 如果播放器未初始化，填充静音
    return;
  }
  player->fillAudioData(stream, len);
}

void AudioPlayer::fillAudioData(uint8_t* stream, int len) {
  std::memset(stream, 0, len);  // 先填充静音数据
  if (paused_ || stop_) return;

  int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
  int bytes_per_frame = bytes_per_sample * channels_;
  int total_bytes_needed = len;
  int total_bytes_filled = 0;

  while (total_bytes_filled < total_bytes_needed) {
    // 如果音频流已结束且缓冲区已空，直接静音
    if (playback_finished_ && pcm_ring_read_ == pcm_ring_write_) {
      break;
    }

    int bytes_to_fill = total_bytes_needed - total_bytes_filled;
    size_t bytes_available =
        popPCMData(stream + total_bytes_filled, bytes_to_fill);
    LOG_DEBUG << "popPCMData got " << bytes_available << " bytes";
    if (bytes_available == 0) {
      break;
    }

    total_bytes_filled += bytes_available;
    consumed_samples_ += bytes_available / bytes_per_frame;
    int64_t base = base_pts_.load(std::memory_order_acquire);  // 获取基准时间戳
    audio_clock_.store(base + (consumed_samples_ * AV_TIME_BASE) / sample_rate_,
                       std::memory_order_release);
  }

  // 用 SDL_MixAudioFormat 做音量缩放
  int vol = volume_.load(std::memory_order_acquire);
  if (vol < SDL_MIX_MAXVOLUME && total_bytes_filled > 0) {
    std::vector<uint8_t> tmp(stream, stream + total_bytes_filled);
    SDL_MixAudioFormat(stream, tmp.data(), AUDIO_S16SYS, total_bytes_filled,
                       vol);
  }
}

void AudioPlayer::producerThreadLoop() {
  using namespace std::chrono_literals;
  std::vector<uint8_t> pcm_buffer;

  while (!stop_ && !playback_finished_) {
    if (paused_) {
      std::this_thread::sleep_for(10ms);
      continue;
    }

    auto frame = audio_reader_->getNextFrame();
    if (!frame) {
      // 读取音频帧失败，可能是流结束或出错
      if (audio_reader_->isEOF()) {
        playback_finished_.store(true);
        std::this_thread::sleep_for(10ms);
        continue;
      }
      std::this_thread::sleep_for(5ms);
      continue;
    }

    // 将平面格式转换为交错格式 (S16)
    convertPlanarToInterleaved(frame->frame.get(), pcm_buffer);
    if (pcm_buffer.empty()) {
      continue;
    }
    // 设置基准时间戳为音频帧的PTS
    if (base_pts_.load() == AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE) {
      int64_t pts_us = av_rescale_q(frame->pts, audio_reader_->getTimeBase(),
                                    AV_TIME_BASE_Q);
      base_pts_.store(pts_us, std::memory_order_release);
    }

    size_t bytes_to_write = pcm_buffer.size();
    const uint8_t* data_ptr = pcm_buffer.data();

    auto wait_start = std::chrono::high_resolution_clock::now();
    const auto max_wait_duration = 200ms;  // 最大等待时间

    while (bytes_to_write > 0 && !stop_ && !paused_) {
      size_t bytes_written = pushPCMData(data_ptr, bytes_to_write);
      if (bytes_written > 0) {
        data_ptr += bytes_written;
        bytes_to_write -= bytes_written;
      } else {
        // 环形缓冲区满，等待一段时间再尝试
        std::this_thread::sleep_for(5ms);
        auto now = std::chrono::high_resolution_clock::now();
        if (now - wait_start > max_wait_duration) {
          LOG_WARN << "Producer thread wait timeout, dropping audio data";
          break;  // 超过最大等待时间，跳出循环
        }
      }
    }
  }
}

void AudioPlayer::convertPlanarToInterleaved(const AVFrame* frame,
                                             std::vector<uint8_t>& out) {
  if (!frame || frame->nb_samples <= 0) {
    LOG_ERROR << "Invalid frame for conversion";
    out.clear();
    return;
  }

  // 计算重采样后所需的缓冲区大小
  int64_t delay = swr_get_delay(swr_ctx_.get(), frame->sample_rate);
  int max_out_samples = av_rescale_rnd(delay + frame->nb_samples, sample_rate_,
                                       frame->sample_rate, AV_ROUND_UP);
  int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
  int total_bytes = max_out_samples * channels_ * bytes_per_sample;
  out.resize(total_bytes);

  uint8_t* out_data[1] = {out.data()};
  int converted_samples =
      swr_convert(swr_ctx_.get(), out_data, max_out_samples,
                  (const uint8_t**)frame->data, frame->nb_samples);
  if (converted_samples < 0) {
    LOG_ERROR << "Error during resampling";
    out.clear();
    return;
  }

  int expected_bytes = converted_samples * channels_ * bytes_per_sample;
  out.resize(expected_bytes);
}

size_t AudioPlayer::pushPCMData(const uint8_t* data, size_t bytes) {
  if (!data || bytes == 0 || pcm_ring_cap_ == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(pcm_mutex_);

  size_t free_space = pcm_ring_cap_ - (pcm_ring_write_ - pcm_ring_read_);
  if (free_space == 0) {
    return 0;  // 环形缓冲区满
  }

  size_t bytes_to_write = std::min(bytes, free_space);
  size_t first_chunk = std::min(
      bytes_to_write, pcm_ring_cap_ - (pcm_ring_write_ % pcm_ring_cap_));
  size_t second_chunk = bytes_to_write - first_chunk;

  // 写入第一块数据
  std::memcpy(&pcm_ring_buf_[pcm_ring_write_ % pcm_ring_cap_], data,
              first_chunk);
  // 写入第二块数据（如果有的话）
  if (second_chunk > 0) {
    std::memcpy(&pcm_ring_buf_[0], data + first_chunk, second_chunk);
  }

  pcm_ring_write_ += bytes_to_write;
  return bytes_to_write;
}

size_t AudioPlayer::popPCMData(uint8_t* out, size_t bytes) {
  if (!out || bytes == 0 || pcm_ring_cap_ == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(pcm_mutex_);

  size_t available_data = pcm_ring_write_ - pcm_ring_read_;
  if (available_data == 0) {
    return 0;  // 环形缓冲区空
  }

  size_t bytes_to_read = std::min(bytes, available_data);
  size_t first_chunk =
      std::min(bytes_to_read, pcm_ring_cap_ - (pcm_ring_read_ % pcm_ring_cap_));
  size_t second_chunk = bytes_to_read - first_chunk;

  // 读取第一块数据
  std::memcpy(out, &pcm_ring_buf_[pcm_ring_read_ % pcm_ring_cap_], first_chunk);
  // 读取第二块数据（如果有的话）
  if (second_chunk > 0) {
    std::memcpy(out + first_chunk, &pcm_ring_buf_[0], second_chunk);
  }

  pcm_ring_read_ += bytes_to_read;
  return bytes_to_read;
}

void AudioPlayer::pause() {
  paused_.store(true);
  if (audio_dev_ != 0) {
    SDL_PauseAudioDevice(audio_dev_, 1);  // 暂停音频播放
  }
}

void AudioPlayer::resume() {
  paused_.store(false);
  if (audio_dev_ != 0) {
    SDL_PauseAudioDevice(audio_dev_, 0);  // 恢复音频播放
  }
}

void AudioPlayer::stop() {
  stop_.store(true);
  paused_.store(false);

  if (producer_thread_.joinable() &&
      producer_thread_.get_id() != std::this_thread::get_id()) {
    producer_thread_.join();
  }

  if (audio_dev_ != 0) {
    SDL_CloseAudioDevice(audio_dev_);
    audio_dev_ = 0;
  }
  swr_ctx_.reset();  // 智能指针自动释放
  SDL_Quit();

  {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    pcm_ring_buf_.clear();
    pcm_ring_cap_ = 0;
    pcm_ring_read_ = 0;
    pcm_ring_write_ = 0;
  }

  audio_reader_.reset();
  base_pts_ = 0;
  consumed_samples_ = 0;
  audio_clock_ = 0;
  pulling_ = false;
  stop_.store(false);
  playback_finished_ = false;
}

void AudioPlayer::clear() {
  std::lock_guard<std::mutex> lock(pcm_mutex_);
  pcm_ring_buf_.clear();
  pcm_ring_cap_ = 0;
  pcm_ring_read_ = 0;
  pcm_ring_write_ = 0;
  base_pts_ = 0;
  consumed_samples_ = 0;
  audio_clock_ = 0;
}

void AudioPlayer::resetClock(int64_t pts) noexcept {
  // Pause SDL callback to stop further consumption while we reset
  if (audio_dev_ != 0) {
    SDL_PauseAudioDevice(audio_dev_, 1);
  }

  // Clear ring buffer and reset counters under pcm_mutex_
  {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    pcm_ring_read_ = 0;
    pcm_ring_write_ = 0;
    if (!pcm_ring_buf_.empty()) {
      std::fill(pcm_ring_buf_.begin(), pcm_ring_buf_.end(), 0);
    }
  }

  // Reset timing state
  consumed_samples_.store(0, std::memory_order_release);
  base_pts_.store(pts, std::memory_order_release);
  audio_clock_.store(pts, std::memory_order_release);

  // Ensure playback_finished_ cleared so producer will refill
  playback_finished_.store(false, std::memory_order_release);

  // Resume audio callback
  if (audio_dev_ != 0) {
    SDL_PauseAudioDevice(audio_dev_, 0);
  }
}

void AudioPlayer::setVolume(double norm) {
  // clamp to [0,1]
  if (std::isnan(norm)) norm = 1.0;
  if (norm < 0.0) norm = 0.0;
  if (norm > 1.0) norm = 1.0;
  int v = static_cast<int>(
      std::round(norm * static_cast<double>(SDL_MIX_MAXVOLUME)));
  volume_.store(v, std::memory_order_release);
  LOG_INFO << "AudioPlayer setVolume: norm=" << norm << " -> volume=" << v;
}

double AudioPlayer::getVolume() const {
  int v = volume_.load(std::memory_order_acquire);
  return static_cast<double>(v) / static_cast<double>(SDL_MIX_MAXVOLUME);
}