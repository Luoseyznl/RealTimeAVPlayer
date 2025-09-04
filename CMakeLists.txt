cmake_minimum_required(VERSION 3.12)
project(RealTimeAVPlayer VERSION 1.0)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # 导出 compile_commands.json，方便 clangd / clang-tidy / VSCode

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 强制使用 C++17 标准

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # 默认安装到 usr/local 下，改到构建目录的 install 子目录（build/install）
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install Directory" FORCE)
endif()

# === 构建 FFmpeg === 
option(USE_INTERNAL_FFMPEG "Build FFmpeg from third_party/ffmpeg (if ON). If OFF, prefer system FFmpeg" OFF)

if(NOT USE_INTERNAL_FFMPEG)
    # PkgConfig 是一个用于管理库的工具，可以帮助查找和配置库的编译和链接选项
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(AVFORMAT_PKG QUIET libavformat) # QUIET 表示如果找不到包不会报错
        pkg_check_modules(AVCODEC_PKG QUIET libavcodec)
        pkg_check_modules(AVUTIL_PKG QUIET libavutil)
        pkg_check_modules(SWRESAMPLE_PKG QUIET libswresample)
        pkg_check_modules(SWSCALE_PKG QUIET libswscale)
        pkg_check_modules(AVDEVICE_PKG QUIET libavdevice)
    endif()

    if(AVFORMAT_PKG_FOUND AND AVCODEC_PKG_FOUND AND AVUTIL_PKG_FOUND
        AND SWRESAMPLE_PKG_FOUND AND SWSCALE_PKG_FOUND AND AVDEVICE_PKG_FOUND)
        message(STATUS "Using system FFmpeg via pkg-config") # STATUS 级别的信息不会中断构建过程
        list(APPEND FFMPEG_INCLUDE_DIR
            ${AVFORMAT_PKG_INCLUDE_DIRS} ${AVCODEC_PKG_INCLUDE_DIRS} ${AVUTIL_PKG_INCLUDE_DIRS}
            ${SWRESAMPLE_PKG_INCLUDE_DIRS} ${SWSCALE_PKG_INCLUDE_DIRS} ${AVDEVICE_PKG_INCLUDE_DIRS})
        list(APPEND FFMPEG_LIBRARY_DIR
            ${AVFORMAT_PKG_LIBRARY_DIRS} ${AVCODEC_PKG_LIBRARY_DIRS} ${AVUTIL_PKG_LIBRARY_DIRS}
            ${SWRESAMPLE_PKG_LIBRARY_DIRS} ${SWSCALE_PKG_LIBRARY_DIRS} ${AVDEVICE_PKG_LIBRARY_DIRS})
        list(APPEND FFMPEG_LIBRARIES
            ${AVFORMAT_PKG_LIBRARIES} ${AVCODEC_PKG_LIBRARIES} ${AVUTIL_PKG_LIBRARIES}
            ${SWRESAMPLE_PKG_LIBRARIES} ${SWSCALE_PKG_LIBRARIES} ${AVDEVICE_PKG_LIBRARIES})

        add_library(FFMPEG INTERFACE) # 将此外部库作为 INTERFACE 库添加到 CMake 中
        target_include_directories(FFMPEG INTERFACE ${FFMPEG_INCLUDE_DIR})
        target_link_libraries(FFMPEG INTERFACE ${FFMPEG_LIBRARIES})
    else()
        message(STATUS "System FFmpeg not found or incomplete; falling back to internal build")
        set(USE_INTERNAL_FFMPEG ON)
    endif()
endif()

if(USE_INTERNAL_FFMPEG)
    message(STATUS "Using internal FFmpeg build")
    set(FFMPEG_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg) # FFmpeg 源码目录
    set(FFMPEG_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}) # FFmpeg 安装目录 build/install/lib

    set(FFMPEG_CONFIGURE_OPTIONS # use when invoking ./configure
        --prefix=${FFMPEG_INSTALL_DIR}
        --disable-doc
        --disable-programs
        --enable-shared
    )

    # ExternalProject 模块用于处理外部项目的构建（源码依赖的库需要先构建）
    include(ExternalProject)
    ExternalProject_Add(ffmpeg
        SOURCE_DIR ${FFMPEG_SOURCE_DIR}
        BINARY_DIR ${FFMPEG_SOURCE_DIR}/build # FFmpeg 的构建目录
        CONFIGURE_COMMAND ${FFMPEG_SOURCE_DIR}/configure ${FFMPEG_CONFIGURE_OPTIONS}
        BUILD_COMMAND make -j${CMAKE_BUILD_PARALLEL_LEVEL}
        INSTALL_COMMAND make install
        LOG_DOWNLOAD ON
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
        BUILD_IN_SOURCE 0 # build in BINARY_DIR
    )

    set(FFMPEG_INCLUDE_DIR ${FFMPEG_INSTALL_DIR}/include)
    set(FFMPEG_LIBRARY_DIR ${FFMPEG_INSTALL_DIR}/lib)

    if(WIN32)
        set(FFMPEG_LIB_SUFFIX .lib)
    elseif(APPLE)
        set(FFMPEG_LIB_SUFFIX .dylib)
    else()
        set(FFMPEG_LIB_SUFFIX .so)
    endif()

    set(FFMPEG_LIBRARIES
        ${FFMPEG_LIBRARY_DIR}/libavcodec${FFMPEG_LIB_SUFFIX}
        ${FFMPEG_LIBRARY_DIR}/libavformat${FFMPEG_LIB_SUFFIX}
        ${FFMPEG_LIBRARY_DIR}/libavutil${FFMPEG_LIB_SUFFIX}
        ${FFMPEG_LIBRARY_DIR}/libswscale${FFMPEG_LIB_SUFFIX}
        ${FFMPEG_LIBRARY_DIR}/libavdevice${FFMPEG_LIB_SUFFIX}
        ${FFMPEG_LIBRARY_DIR}/libswresample${FFMPEG_LIB_SUFFIX}
    )

    add_library(FFMPEG INTERFACE)
    add_dependencies(FFMPEG ffmpeg) # 确保 FFMPEG 依赖于 ffmpeg 项目
    target_include_directories(FFMPEG INTERFACE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(FFMPEG INTERFACE ${FFMPEG_LIBRARIES})
endif()

# Fail early if we still don't have libraries
if(NOT FFMPEG_LIBRARIES)
    message(FATAL_ERROR "FFmpeg libraries not found. Set -DUSE_INTERNAL_FFMPEG=ON or install FFmpeg development packages.")
endif()


# === 构建项目 ===
add_subdirectory(src)
add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE RealTimeAVPlayerLib)

# Linux 上使用 X11 作为窗口系统，Windows/macOS 则使用各自的原生窗口系统
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE X11::X11)
endif()

find_package(glfw3 CONFIG QUIET)
if(TARGET GLFW::GLFW)
    message(STATUS "Using GLFW::GLFW target")
    target_link_libraries(${PROJECT_NAME} PRIVATE GLFW::GLFW)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW_PKG REQUIRED glfw3)
        message(STATUS "Using glfw3 via pkg-config")
        target_include_directories(${PROJECT_NAME} PRIVATE ${GLFW_PKG_INCLUDE_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${GLFW_PKG_LIBRARIES})
    else()
        message(FATAL_ERROR "glfw not found. Install libglfw3-dev or provide glfw3Config.cmake.")
    endif()
endif()

# === 输出目录 & RPATH ===
set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin
)

string(JOIN ":" FFMPEG_RPATH ${FFMPEG_LIBRARY_DIR})
set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "$ORIGIN/../lib;${FFMPEG_RPATH}"
    INSTALL_RPATH "$ORIGIN/../lib;${FFMPEG_RPATH}"
)

# === 安装规则 ===
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/ DESTINATION assets PATTERN ".git" EXCLUDE)
