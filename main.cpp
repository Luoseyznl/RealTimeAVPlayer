#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "player.hpp"

extern "C" {
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <SDL2/SDL.h>
}

#include "utils/logger.hpp"

#ifdef __linux__
#include <X11/Xlib.h>
#endif

using namespace utils;

static std::atomic<bool> quit(false);
static Player* g_player = nullptr;

// 统一清理函数
void cleanup() {
  if (g_player) {
    g_player->stop();
    g_player->close();
    GLFWwindow* window = g_player->getWindow();
    if (window) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);  // 设置窗口关闭标志
    }
    g_player = nullptr;
  }
  glfwTerminate();  // 终止 GLFW
}

static void signal_handler(int signum) {
  if (signum == SIGINT) {
    quit.store(true);
  }
}

void printUsage(const char* prog_name) {
  std::cout << "Usage: " << prog_name << " <video_file>\n";
  std::cout << "Example: " << prog_name << " sample.mp4\n";
}

void handleKeyPress(Player& player, int key) {
  Player::State currentState = player.getState();
  if (currentState == Player::State::Error) {
    return;  // 跳过无效操作
  }

  switch (key) {
    case GLFW_KEY_SPACE:
    case GLFW_KEY_P:
      if (currentState == Player::State::Playing) {
        player.pause();
      } else if (currentState == Player::State::Paused) {
        player.resume();
      }
      break;
    case GLFW_KEY_Q:
    case GLFW_KEY_ESCAPE:
      quit.store(true);
      break;
    case GLFW_KEY_R:
      player.seek(0.0);  // 跳转到开头
      player.play();
      break;
    case GLFW_KEY_LEFT:
      player.seek(player.getCurrentTimestamp() - 5.0);  // 后退5秒
      player.play();
      break;
    case GLFW_KEY_RIGHT:
      player.seek(player.getCurrentTimestamp() + 5.0);  // 前进5秒
      player.play();
      break;
    case GLFW_KEY_UP:
      player.setVolume(std::min(player.getVolume() + 0.0625, 1.0));
      break;
    case GLFW_KEY_DOWN:
      player.setVolume(std::max(player.getVolume() - 0.0625, 0.0));
      break;
    case GLFW_KEY_S:  // 步进 step play
      if (currentState == Player::State::Paused) {
        player.seek(player.getCurrentTimestamp() + 0.04);  // 默认步进40ms
      }
      break;
    case GLFW_KEY_M:  // 静音切换
      if (player.getVolume() > 0.0) {
        player.setVolume(0.0);
      } else {
        player.setVolume(1.0);
      }
      break;
    default:
      break;
  }
}

void keyCallback(GLFWwindow* window, int key, int scancode, int action,
                 int mods) {
  if (action == GLFW_PRESS) {
    if (g_player) {
      handleKeyPress(*g_player, key);
    }
  }
}

void updateWindowTitle(int64_t currentTimeUs, int64_t durationUs) {
  int64_t cur_s = currentTimeUs / 1000000;
  int hours = static_cast<int>(cur_s / 3600);
  int minutes = static_cast<int>((cur_s / 60) % 60);
  int seconds = static_cast<int>(cur_s % 60);

  int64_t dur_s = durationUs / 1000000;
  int totalHours = static_cast<int>(dur_s / 3600);
  int totalMinutes = static_cast<int>((dur_s / 60) % 60);
  int totalSeconds = static_cast<int>(dur_s % 60);

  char buf[128];
  snprintf(buf, sizeof(buf), "Video Player - %02d:%02d:%02d / %02d:%02d:%02d",
           hours, minutes, seconds, totalHours, totalMinutes, totalSeconds);

  // Prefer printing to stdout to avoid GUI thread work here
  printf("\r%s", buf);
  fflush(stdout);  // Force immediate output
}

void onPlayerStateChanged(Player::State state) {
  if (state == Player::State::Error) {
    LOG_ERROR << "Player entered Error state, exiting...";
    quit.store(true);
  }
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    printUsage(argv[0]);
    return 1;
  }

#ifdef __linux__
  // 解决Xlib线程安全问题
  if (!XInitThreads()) {
    std::cerr << "Failed to initialize X11 threads" << std::endl;
    return 1;
  }
#endif

  // 注册信号处理函数
  signal(SIGINT, signal_handler);

  // 1. 在主线程中初始化 GLFW
  if (!glfwInit()) {
    std::cerr << "Failed to initialize GLFW" << std::endl;
    return -1;
  }

  Player player;
  g_player = &player;

  if (!player.open(argv[1])) {
    LOG_ERROR << "Failed to open media file: " << argv[1];
    cleanup();
    return -1;
  }

  // Handle callbacks
  player.setKeyCallback(keyCallback);
  player.setTimestampCallback(updateWindowTitle);
  player.setStateCallback(onPlayerStateChanged);

  if (!player.play()) {
    LOG_ERROR << "Failed to start playback";
    cleanup();
    return -1;
  }

  while (!quit.load()) {
    if (player.isFinished()) {
      LOG_INFO << "Playback finished";
      break;
    }

    glfwPollEvents();  // 处理窗口事件

    GLFWwindow* window = player.getWindow();
    if (window && glfwWindowShouldClose(window)) {
      quit.store(true);
      LOG_INFO << "Window close requested";
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  LOG_INFO << "Exiting program";
  cleanup();
  return 0;
}