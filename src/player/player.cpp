#include "player.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

#include "utils/logger.hpp"

extern "C" {
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include "audio_player.hpp"
#include "gl_renderer.hpp"
#include "stream_source.hpp"

using namespace utils;

// 同步阈值：最小阈值为40ms，最大阈值为100ms，超过200ms则进行帧重复
const int64_t AV_SYNC_THRESHOLD_MIN = static_cast<int64_t>(0.04 * AV_TIME_BASE);
const int64_t AV_SYNC_THRESHOLD_MAX = static_cast<int64_t>(0.1 * AV_TIME_BASE);
const int64_t AV_SYNC_FRAMEDUP_THRESHOLD =
    static_cast<int64_t>(0.2 * AV_TIME_BASE);  // 音视频时间差过大时，允许重复帧

Player::Player()
    : state_(State::Stopped),
      render_thread_(),
      timestamp_cb_(nullptr),
      state_cb_(nullptr),
      last_delay_(0.0) {
  LOG_INFO << "Initializing Player";
  video_reader_ = std::make_unique<StreamSource>(Type::Video);
  audio_reader_ = std::make_shared<StreamSource>(Type::Audio);
  renderer_ = std::make_unique<GLRenderer>();
  audio_player_ = std::make_unique<AudioPlayer>();
}

Player::~Player() {
  LOG_INFO << "Destroying Player";
  close();
}

bool Player::open(const std::string& filename) {
  if (getState() != State::Stopped) {
    LOG_WARN << "Player is not in Stopped state";
    return false;
  }

  if (!video_reader_->open(filename)) {
    LOG_ERROR << "Failed to open video stream";
    video_reader_->close();
    return false;
  }
  if (!audio_reader_->open(filename)) {
    LOG_ERROR << "Failed to open audio stream";
    video_reader_->close();
    return false;
  } else {
    // Initialize audio player if audio stream is available
    LOG_INFO << "Audio stream found, initializing audio player";
    if (!audio_player_ || !audio_player_->initialize(audio_reader_)) {
      LOG_ERROR << "Failed to initialize audio player";
      video_reader_->close();
      audio_reader_->close();
      return false;
    }
  }

  // Initialize renderer if video stream is available
  if (video_reader_) {
    LOG_INFO << "Video stream found, initializing renderer";
    if (!renderer_ || !renderer_->start(video_reader_->getWidth(),
                                        video_reader_->getHeight())) {
      LOG_ERROR << "Failed to start renderer";
      video_reader_->close();
      audio_reader_->close();
      updateState(State::Error);
      return false;
    }
  }

  is_running_.store(true);
  render_thread_ = std::thread(&Player::renderLoop, this);
  updateState(State::Stopped);  // 打开后默认停止
  return true;
}

void Player::close() {
  if (getState() == State::Stopped) {
    return;
  }
  LOG_INFO << "Closing Player";

  is_running_.store(false);
  if (render_thread_.joinable()) {
    render_thread_.join();
  }

  if (audio_player_) {
    audio_player_->stop();
    audio_player_->clear();
  }
  if (renderer_) {
    renderer_->stop();
    renderer_->clearFrames();
  }
  if (video_reader_) {
    video_reader_->stopDecoding();
    video_reader_->close();
  }
  if (audio_reader_) {
    audio_reader_->stopDecoding();
    audio_reader_->close();
  }

  updateState(State::Stopped);
}

bool Player::play() {
  if (getState() == State::Playing) {
    return true;  // 已经在播放
  }
  if (getState() == State::Paused) {
    resume();
    return true;
  }

  if (getState() == State::Error) {
    LOG_ERROR << "Cannot play, player is in Error state";
    return false;
  }
  if (!video_reader_ || !audio_reader_) {
    LOG_ERROR << "Cannot play, streams are not opened";
    updateState(State::Error);
    return false;
  }

  // Start audio playback first to ensure audio is ready
  LOG_INFO << "Starting playback";
  if (audio_reader_) {
    audio_reader_->startDecoding();
    if (audio_player_ && !audio_player_->isPaused()) {
      audio_player_->resume();
    }
  }
  if (video_reader_) {
    video_reader_->startDecoding();
  }
  updateState(State::Playing);
  return true;
}

void Player::pause() {
  if (getState() != State::Playing) {
    return;
  }
  LOG_INFO << "Pausing playback";
  if (video_reader_) video_reader_->pauseDecoding();
  if (audio_reader_) {
    audio_reader_->pauseDecoding();
    if (audio_player_) audio_player_->pause();
  }
  updateState(State::Paused);
}

void Player::resume() {
  if (getState() != State::Paused) {
    return;
  }
  LOG_INFO << "Resuming playback";
  if (audio_reader_) {
    audio_reader_->resumeDecoding();
  }
  if (video_reader_) {
    video_reader_->resumeDecoding();
  }
  if (audio_player_ && audio_player_->isPaused()) {
    audio_player_->resume();
  }
  updateState(State::Playing);
}

void Player::stop() {
  if (getState() == State::Stopped) {
    return;
  }
  LOG_INFO << "Stopping playback";
  if (audio_player_) {
    audio_player_->stop();
  }
  if (audio_reader_) {
    audio_reader_->stopDecoding();
  }
  if (video_reader_) {
    video_reader_->stopDecoding();
  }
  if (renderer_) {
    renderer_->clearFrames();
  }
  updateState(State::Stopped);
}

bool Player::seek(double timestamp_seconds) {
  // pause during seek to avoid races
  pause();
  timestamp_seconds = std::min(std::max(0.0, timestamp_seconds), getDuration());
  int64_t seek_target = static_cast<int64_t>(timestamp_seconds * AV_TIME_BASE);

  if (video_reader_) {
    if (!video_reader_->seek(seek_target)) {
      LOG_ERROR << "Failed to seek video to timestamp: " << seek_target;
      return false;
    }
  }

  if (audio_reader_) {
    if (audio_player_) audio_player_->resetClock(seek_target);
    if (!audio_reader_->seek(seek_target)) {
      LOG_ERROR << "Failed to seek audio to timestamp: " << seek_target;
      return false;
    }
  }

  LOG_INFO << "Seeked to timestamp: " << seek_target;
  return true;
}

bool Player::isFinished() const noexcept {
  return (getState() == State::Stopped) &&
         (!video_reader_ || video_reader_->isEOF()) &&
         (!audio_reader_ || audio_reader_->isEOF());
}

Player::State Player::getState() const noexcept { return state_.load(); }

double Player::getDuration() const noexcept {
  if (video_reader_)
    return static_cast<double>(video_reader_->getDuration()) / AV_TIME_BASE;
  if (audio_reader_)
    return static_cast<double>(audio_reader_->getDuration()) / AV_TIME_BASE;
  return 0.0;
}

// 返回当前时间戳，优先使用音频时钟，其次是视频PTS（单位秒）
double Player::getCurrentTimestamp() const noexcept {
  if (audio_reader_ && audio_player_) {
    int64_t ac = audio_player_->getAudioClock();
    if (ac > 0) return static_cast<double>(ac) / AV_TIME_BASE;
  }
  if (video_reader_ && last_timestamp_ > 0) {
    return static_cast<double>(last_timestamp_) / AV_TIME_BASE;
  }
  if (!video_reader_ && audio_reader_ && audio_player_) {
    int64_t ac = audio_player_->getAudioClock();
    if (ac > 0) return static_cast<double>(ac) / AV_TIME_BASE;
  }
  return 0.0;
}

GLFWwindow* Player::getWindow() const noexcept {
  return renderer_ ? renderer_->window() : nullptr;
}

void Player::setVolume(double norm) noexcept {
  if (audio_player_) {
    audio_player_->setVolume(norm);
  }
}

double Player::getVolume() const noexcept {
  return audio_player_ ? audio_player_->getVolume() : 0.0;
}

void Player::renderLoop() {
  LOG_INFO << "Render thread started";

  if (!renderer_ || !video_reader_) {
    LOG_ERROR << "Renderer or video reader not initialized";
    is_running_.store(false);
    return;
  }

  // Wait until the renderer window is ready
  int wait_count = 0;
  while (wait_count < 50 && !renderer_->isRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    wait_count++;
  }

  // Install key callback if provided
  if (key_callback_) {
    GLFWwindow* window = renderer_->window();
    if (window) {
      glfwSetKeyCallback(window, key_callback_);
    }
  }

  while (is_running_.load()) {
    if (getState() == State::Paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // Handle window close event
    if (renderer_->window() && glfwWindowShouldClose(renderer_->window())) {
      LOG_INFO << "Window close requested";
      is_running_.store(false);
      break;
    }

    // Get next video frame
    auto video_frame = video_reader_->getNextFrame();
    if (!video_frame) {
      if (video_reader_->isEOF()) {
        LOG_INFO << "Video stream EOF reached";
        if (audio_reader_ && !audio_reader_->isEOF()) {
          // Wait for audio to finish
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        } else {
          // Both streams finished
          LOG_INFO << "Both video and audio streams finished";
          stop();
          break;
        }
      } else {
        // No frame available yet, wait a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
    }

    int64_t video_pts = video_frame->pts;
    int64_t audio_clock = audio_player_ ? audio_player_->getAudioClock() : 0;

    // Calculate the time difference between video PTS and audio clock
    double diff = static_cast<double>(video_pts - audio_clock) / AV_TIME_BASE;

    // Determine delay based on frame rate
    int64_t frame_delay =
        (video_frame->duration > 0)
            ? video_frame->duration
            : static_cast<int64_t>(AV_TIME_BASE /
                                   video_reader_->getFrameRate());
    double delay = static_cast<double>(frame_delay) / AV_TIME_BASE;

    // Adjust delay based on AV sync logic
    if (fabs(diff) < AV_SYNC_THRESHOLD_MIN) {
      // 时间差很小，不做处理，正常渲染
    } else if (fabs(diff) > AV_SYNC_THRESHOLD_MAX) {
      // 时间差太大，强制同步（丢帧或补帧）
      delay = std::max(0.0, delay + diff);
    } else if (diff > AV_SYNC_FRAMEDUP_THRESHOLD) {
      // 视频滞后太多，重复上一帧
      delay = 0.0;
    }

    // Smooth delay to avoid jitter (简单低通滤波)
    if (last_delay_ > 0) {
      delay = last_delay_ * 0.9 + delay * 0.1;
    }
    last_delay_ = delay;

    if (renderer_) {
      renderer_->enqueueFrame(video_frame->frame);
    }

    last_timestamp_ = video_pts;
    if (timestamp_cb_) {
      int64_t duration_us = static_cast<int64_t>(getDuration() * AV_TIME_BASE);
      timestamp_cb_(last_timestamp_, duration_us);
    }

    if (delay > 0) {
      int delay_ms = static_cast<int>(delay * 1000);
      if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      }
    }
  }
  LOG_INFO << "Render thread exiting";
  updateState(State::Stopped);
}

void Player::updateState(State new_state) {
  state_.store(new_state);
  if (state_cb_) {
    state_cb_(new_state);
  }
}