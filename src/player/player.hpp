#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class GLRenderer;
class AudioPlayer;
class StreamSource;
class GLFWwindow;

/**
 * Player: 支持播放控制、跳转、音量控制和回调机制。
 * 使用 GLRenderer 处理视频渲染，AudioPlayer 处理音频播放。
 */
class Player {
 public:
  enum class State { Stopped, Playing, Paused, Error };

  using TimestampCallback =
      std::function<void(int64_t timestamp, int64_t duration)>;
  using StateCallback = std::function<void(State state)>;

  Player();
  ~Player();

  bool open(const std::string& filename);  // 打开媒体文件
  void close();                            // 关闭并释放资源

  bool play();    // 开始播放
  void resume();  // 恢复播放
  void pause();   // 暂停播放
  void stop();    // 停止播放

  bool seek(double timestamp_sec);  // 跳转到指定时间戳（秒）。

  bool isFinished() const noexcept;

  State getState() const noexcept;              // 获取当前状态
  double getDuration() const noexcept;          // 获取总时长（秒）。
  double getCurrentTimestamp() const noexcept;  // 获取当前时间戳（秒）。
  GLFWwindow* getWindow() const noexcept;

  void setVolume(double norm) noexcept;  // norm: 0.0 ~ 1.0
  double getVolume() const noexcept;

  void setTimestampCallback(TimestampCallback cb) {
    timestamp_cb_ = std::move(cb);
  }
  void setStateCallback(StateCallback cb) { state_cb_ = std::move(cb); }
  void setKeyCallback(GLFWkeyfun cb) { key_callback_ = cb; }

 private:
  void renderLoop();
  void updateState(State new_state);

  // 窗口和渲染相关
  std::unique_ptr<StreamSource> video_reader_;
  std::shared_ptr<StreamSource> audio_reader_;
  std::unique_ptr<GLRenderer> renderer_;
  std::unique_ptr<AudioPlayer> audio_player_;

  std::thread render_thread_;
  std::atomic<bool> is_running_{false};
  std::atomic<State> state_{State::Stopped};
  std::mutex state_mutex_;  // 状态锁

  TimestampCallback timestamp_cb_ = nullptr;
  StateCallback state_cb_ = nullptr;
  GLFWkeyfun key_callback_ = nullptr;

  int64_t last_timestamp_ = 0;  // 上一次回调的时间戳，单位微秒(us)
  double last_delay_ = 0.0;     // 上一次回调的延迟，单位秒
};