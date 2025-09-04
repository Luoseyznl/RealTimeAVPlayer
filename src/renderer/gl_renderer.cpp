#define GL_SILENCE_DEPRECATION
#include "gl_renderer.hpp"

#include <chrono>

#include "utils/logger.hpp"

using namespace utils;

// GLSL 顶点着色器：接收顶点位置和纹理坐标，传递给片段着色器
const char* GLRenderer::VERTEX_SHADER_SOURCE = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 1.0);
    TexCoord = aTexCoord;
}
)";

// GLSL 片段着色器：接收 YUV 纹理，转换为 RGB 输出
const char* GLRenderer::FRAGMENT_SHADER_SOURCE = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;

void main() {
    float y = texture(texY, TexCoord).r;
    float u = texture(texU, TexCoord).r - 0.5;
    float v = texture(texV, TexCoord).r - 0.5;
    
    // YUV to RGB conversion
    float r = y + 1.402 * v;
    float g = y - 0.344 * u - 0.714 * v;
    float b = y + 1.772 * u;
    
    FragColor = vec4(r, g, b, 1.0);
}
)";

GLRenderer::GLRenderer(size_t max_queue_size)
    : max_queue_size_(max_queue_size) {}

GLRenderer::~GLRenderer() {
  stop();
  // 确保彻底清空残留帧
  clearFrames();
}

bool GLRenderer::start(int width, int height) {
  if (running_.load()) {
    LOG_WARN << "Renderer is already running";
    return false;
  }

  // 初始化 GLFW（OpenGL 只负责渲染，窗口管理交给 GLFW）
  if (!glfwInit()) {
    LOG_ERROR << "Failed to initialize GLFW";
    return false;
  }

  // 设置 OpenGL 窗口上下文（版本 3.3 Core Profile）
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);  // macOS 需要
#endif

  // 创建窗口
  window_ =
      glfwCreateWindow(width, height, "RealTimeAVPlayer", nullptr, nullptr);
  width_ = width;
  height_ = height;
  if (!window_) {
    LOG_ERROR << "Failed to create GLFW window";
    glfwTerminate();
    return false;
  }

  // 创建渲染线程
  running_.store(true);
  render_thread_ = std::thread(&GLRenderer::renderLoop, this);

  LOG_INFO << "Renderer started";
  return true;
}

void GLRenderer::stop() {
  if (!running_.load()) {
    return;
  }

  // 停止渲染线程
  running_.store(false);
  queue_cv_.notify_all();
  if (render_thread_.joinable()) {
    render_thread_.join();
  }

  // 销毁窗口和 GLFW
  if (window_) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
  glfwTerminate();

  LOG_INFO << "Renderer stopped";
}

bool GLRenderer::enqueueFrame(std::shared_ptr<AVFrame> frame) {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  if (frame_queue_.size() >= max_queue_size_) {
    LOG_WARN << "Frame queue is full, dropping frame with PTS: " << frame->pts;
    frame_queue_.pop_front();  // 移除最旧帧
  }
  frame_queue_.push_back(frame);
  queue_cv_.notify_one();  // 通知渲染线程
  return true;
}

void GLRenderer::clearFrames() {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  while (!frame_queue_.empty()) {
    frame_queue_.pop_front();
  }
  queue_cv_.notify_all();  // 通知渲染线程
}

void GLRenderer::requestResize(int width, int height) {
  std::lock_guard<std::mutex> lock(resize_mutex_);
  resize_width_ = width;
  resize_height_ = height;
  resize_pending_ = true;
  queue_cv_.notify_one();  // 通知渲染线程
}

void GLRenderer::renderLoop() {
  // 在渲染线程中初始化 OpenGL 上下文
  if (!initContext(width_, height_)) {  // 默认大小，稍后会根据窗口调整
    LOG_ERROR << "Failed to initialize OpenGL context";
    running_.store(false);
    return;
  }

  if (!initResources()) {
    LOG_ERROR << "Failed to initialize OpenGL resources";
    shutdownContext();
    running_.store(false);
    return;
  }

  LOG_INFO << "Entering render loop";

  while (running_.load()) {
    std::shared_ptr<AVFrame> frame;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      // 等待帧或停止信号
      queue_cv_.wait(
          lock, [this] { return !frame_queue_.empty() || !running_.load(); });

      if (!running_.load()) {
        break;  // 退出如果停止
      }

      if (!frame_queue_.empty()) {
        frame = frame_queue_.front();
        frame_queue_.pop_front();
      }
    }

    if (frame) {
      updateTexture(frame.get());
    }

    // 处理窗口大小调整请求
    {
      std::lock_guard<std::mutex> lock(resize_mutex_);
      if (resize_pending_) {
        width_ = resize_width_;
        height_ = resize_height_;
        glfwSetWindowSize(window_, width_, height_);
        glViewport(0, 0, width_, height_);
        resize_pending_ = false;
        LOG_INFO << "Resized to " << width_ << "x" << height_;
      }
    }

    // 渲染当前帧
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (frame) {
      // 根据渲染模式调整视口
      int win_w, win_h;
      glfwGetFramebufferSize(window_, &win_w, &win_h);
      float win_aspect = static_cast<float>(win_w) / win_h;
      float tex_aspect = static_cast<float>(tex_width_) / tex_height_;

      int view_w = win_w;
      int view_h = win_h;
      if (render_mode_ == RenderMode::KeepAspectRatio) {
        if (win_aspect > tex_aspect) {
          view_w = static_cast<int>(win_h * tex_aspect);
        } else {
          view_h = static_cast<int>(win_w / tex_aspect);
        }
      }
      int view_x = (win_w - view_w) / 2;
      int view_y = (win_h - view_h) / 2;
      glViewport(view_x, view_y, view_w, view_h);

      // 绘制纹理
      glUseProgram(shader_program_);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, tex_y_);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, tex_u_);
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, tex_v_);
      glBindVertexArray(vao_);
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
    glfwSwapBuffers(window_);
    glfwPollEvents();
  }

  // 清理 OpenGL 资源和上下文
  shutdownContext();
}

bool GLRenderer::initContext(int width, int height) {
  // 在此线程中使 OpenGL 上下文当前
  glfwMakeContextCurrent(window_);

  // 初始化 GLEW
  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK) {
    LOG_ERROR << "Failed to initialize GLEW";
    return false;
  }

  // 设置视口
  glViewport(0, 0, width, height);
  return true;
}

bool GLRenderer::initResources() {
  if (!initShaders()) {
    return false;
  }
  if (!initTexture()) {
    return false;
  }

  // 设置 VAO 和 VBO 用于全屏四边形
  float vertices[] = {
      // positions        // texture coords (y flipped)
      -1.0f, 1.0f,  0.0f, 0.0f, 0.0f,  // top-left
      -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,  // bottom-left
      1.0f,  -1.0f, 0.0f, 1.0f, 1.0f,  // bottom-right
      1.0f,  1.0f,  0.0f, 1.0f, 0.0f   // top-right
  };
  unsigned int indices[] = {0, 1, 2, 2, 3, 0};

  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  GLuint ebo;
  glGenBuffers(1, &ebo);

  glBindVertexArray(vao_);

  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
               GL_STATIC_DRAW);

  // Position attribute
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);
  // Texture coord attribute
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                        (void*)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glDeleteBuffers(1, &ebo);  // EBO 现在是 VAO 状态的一部分

  return true;
}

void GLRenderer::shutdownContext() {
  // 删除 OpenGL 资源
  if (tex_y_) {
    glDeleteTextures(1, &tex_y_);
    tex_y_ = 0;
  }
  if (tex_u_) {
    glDeleteTextures(1, &tex_u_);
    tex_u_ = 0;
  }
  if (tex_v_) {
    glDeleteTextures(1, &tex_v_);
    tex_v_ = 0;
  }
  if (shader_program_) {
    glDeleteProgram(shader_program_);
    shader_program_ = 0;
  }
  if (vbo_) {
    glDeleteBuffers(1, &vbo_);
    vbo_ = 0;
  }
  if (vao_) {
    glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }

  // 取消上下文
  glfwMakeContextCurrent(nullptr);
}

bool GLRenderer::initShaders() {
  // 编译顶点着色器
  GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vertex_shader, 1, &VERTEX_SHADER_SOURCE, nullptr);
  glCompileShader(vertex_shader);
  GLint success;
  glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
  if (!success) {
    char info_log[512];
    glGetShaderInfoLog(vertex_shader, 512, nullptr, info_log);
    LOG_ERROR << "Vertex shader compilation failed: " << info_log;
    return false;
  }

  // 编译片段着色器
  GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragment_shader, 1, &FRAGMENT_SHADER_SOURCE, nullptr);
  glCompileShader(fragment_shader);
  glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
  if (!success) {
    char info_log[512];
    glGetShaderInfoLog(fragment_shader, 512, nullptr, info_log);
    LOG_ERROR << "Fragment shader compilation failed: " << info_log;
    glDeleteShader(vertex_shader);
    return false;
  }

  // 链接着色器到程序
  shader_program_ = glCreateProgram();
  glAttachShader(shader_program_, vertex_shader);
  glAttachShader(shader_program_, fragment_shader);
  glLinkProgram(shader_program_);
  glGetProgramiv(shader_program_, GL_LINK_STATUS, &success);
  if (!success) {
    char info_log[512];
    glGetProgramInfoLog(shader_program_, 512, nullptr, info_log);
    LOG_ERROR << "Shader program linking failed: " << info_log;
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    return false;
  }

  // 清理着色器，因为它们现在链接到程序中
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);

  // 设置纹理采样器到对应纹理单元
  glUseProgram(shader_program_);
  glUniform1i(glGetUniformLocation(shader_program_, "texY"), 0);
  glUniform1i(glGetUniformLocation(shader_program_, "texU"), 1);
  glUniform1i(glGetUniformLocation(shader_program_, "texV"), 2);

  return true;
}

bool GLRenderer::initTexture() {
  // 生成 Y, U, V 平面的纹理
  glGenTextures(1, &tex_y_);
  glGenTextures(1, &tex_u_);
  glGenTextures(1, &tex_v_);

  if (!tex_y_ || !tex_u_ || !tex_v_) {
    LOG_ERROR << "Failed to generate textures";
    return false;
  }

  // 初始化 Y 纹理
  glBindTexture(GL_TEXTURE_2D, tex_y_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  // 初始化 U 纹理
  glBindTexture(GL_TEXTURE_2D, tex_u_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  // 初始化 V 纹理
  glBindTexture(GL_TEXTURE_2D, tex_v_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glBindTexture(GL_TEXTURE_2D, 0);  // 解绑
  return true;
}

void GLRenderer::updateTexture(AVFrame* frame) {
  if (!frame) return;
  if (frame->format != AV_PIX_FMT_YUV420P) {
    LOG_WARN << "Unsupported pixel format: " << frame->format;
    return;
  }

  // Ensure unpack alignment = 1 to avoid stride/padding issues
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  // Y 平面
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex_y_);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[0]);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frame->width, frame->height, 0, GL_RED,
               GL_UNSIGNED_BYTE, frame->data[0]);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  // U 平面 (半分辨率)
  int half_w = (frame->width + 1) / 2;
  int half_h = (frame->height + 1) / 2;
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, tex_u_);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[1]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, half_w, half_h, 0, GL_RED,
               GL_UNSIGNED_BYTE, frame->data[1]);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  // V 平面 (半分辨率)
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, tex_v_);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[2]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, half_w, half_h, 0, GL_RED,
               GL_UNSIGNED_BYTE, frame->data[2]);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  // Restore default unpack alignment
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}