set(SOURCES
    player/player.cpp
    stream/stream_source.cpp
    demuxer/demuxer.cpp
    codec/decoder.cpp
    renderer/gl_renderer.cpp
    player/audio_player.cpp
    utils/logger.cpp
)

add_library(RealTimeAVPlayerLib SHARED ${SOURCES})

set_target_properties(RealTimeAVPlayerLib PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin
    INSTALL_RPATH "$ORIGIN/../lib"
)

target_include_directories(RealTimeAVPlayerLib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/codec
    ${CMAKE_CURRENT_SOURCE_DIR}/demuxer
    ${CMAKE_CURRENT_SOURCE_DIR}/player
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/stream
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

# === 链接依赖 ===
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(SDL2 REQUIRED)

target_link_libraries(RealTimeAVPlayerLib PRIVATE
    OpenGL::GL
    GLEW::GLEW
    SDL2::SDL2
    FFMPEG # 统一走外层接口库
)

install(TARGETS RealTimeAVPlayerLib
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
