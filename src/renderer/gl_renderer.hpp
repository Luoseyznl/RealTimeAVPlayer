#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include "libavutil/frame.h"
}

/**
 * GLRenderer: 封装 OpenGL 渲染器，用于实时视频帧渲染。
 * 支持帧队列管理、线程安全渲染和窗口调整。
 * 使用 GLFW 和 GLEW 处理窗口和图形资源。
 */
class GLRenderer {
 public:
  enum class RenderMode { Normal, Stretch, KeepAspectRatio };

  explicit GLRenderer(size_t max_queue_size = 5);
  ~GLRenderer();

  bool start(int width, int height);  // 启动渲染器：创建窗口和渲染线程。
  void stop();

  bool enqueueFrame(std::shared_ptr<AVFrame> frame);
  void clearFrames();
  void requestResize(int width, int height);
  void setRenderMode(RenderMode mode) { render_mode_ = mode; }

  bool isRunning() const { return running_.load(); }
  GLFWwindow* window() const { return window_; }

 private:
  void renderLoop();  // 渲染循环：处理帧渲染和窗口事件。

  bool initContext(int width, int height);  // 创建 GLFW 窗口和 GLEW 上下文。
  void shutdownContext();                   // 销毁 GLFW 窗口和 OpenGL 资源。
  bool initResources();                     // 初始化着色器、纹理等。
  bool initShaders();
  bool initTexture();

  void updateTexture(AVFrame* frame);  // 更新纹理：从 AVFrame 更新 YUV 纹理。

  // 窗口与渲染参数
  GLFWwindow* window_{nullptr};                 // GLFW 窗口
  RenderMode render_mode_{RenderMode::Normal};  // 渲染模式
  int width_{0};                                // 窗口宽度
  int height_{0};                               // 窗口高度
  GLuint shader_program_{0};                    // 着色器程序
  GLuint vao_{0};                               // 顶点数组对象
  GLuint vbo_{0};                               // 顶点缓冲对象
  GLuint tex_y_{0};                             // Y 纹理
  GLuint tex_u_{0};                             // U 纹理
  GLuint tex_v_{0};                             // V 纹理

  // 渲染线程与队列
  std::thread render_thread_;                         // 渲染线程
  std::mutex queue_mutex_;                            // 队列锁
  std::condition_variable queue_cv_;                  // 队列条件变量
  std::deque<std::shared_ptr<AVFrame>> frame_queue_;  // 帧队列
  size_t max_queue_size_{5};                          // 最大队列大小
  std::atomic<bool> running_{false};                  // 运行状态

  // resize 请求
  int resize_width_{0};         // 新宽度
  int resize_height_{0};        // 新高度
  bool resize_pending_{false};  // 是否有未处理请求
  std::mutex resize_mutex_;     // resize 锁

  // 纹理和着色器
  int tex_width_{0};   // 纹理宽度
  int tex_height_{0};  // 纹理高度

  // 着色器源码
  static const char* VERTEX_SHADER_SOURCE;
  static const char* FRAGMENT_SHADER_SOURCE;
};