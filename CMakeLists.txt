cmake_minimum_required(VERSION 3.10)
project(multi_cam_decoder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# MPP 头文件在系统路径
set(MPP_INCLUDE_DIRS "/usr/include")

# 查找 MPP 库 (RK3588 平台)
find_library(MPP_LIBRARY NAMES rockchip_mpp mpp PATHS /usr/lib/aarch64-linux-gnu /usr/lib /usr/local/lib)

# 查找 RGA 库 (RK3588 平台)
find_library(RGA_LIBRARY NAMES rga PATHS /usr/lib/aarch64-linux-gnu /usr/lib /usr/local/lib)

# 查找 RKNN 库 (RK3588 平台)
find_library(RKNN_LIBRARY NAMES rknnrt rknn_api PATHS /usr/lib /usr/lib/aarch64-linux-gnu /usr/local/lib)

# RKNN 头文件路径
set(RKNN_INCLUDE_DIRS "/home/topeet/rknpu2/runtime/RK3588/Linux/librknn_api/include")

# 检查是否在 RK3588 平台上
if(NOT MPP_LIBRARY OR NOT RGA_LIBRARY OR NOT RKNN_LIBRARY)
    message(WARNING "MPP/RGA/RKNN libraries not found. This project requires RK3588 platform.")
    message(WARNING "Please build on RK3588 board with proper SDK installed.")
    # 设置占位库路径以允许 CMake 配置成功（用于 IDE 语法检查）
    set(MPP_LIBRARY "mpp")
    set(RGA_LIBRARY "rga")
    set(RKNN_LIBRARY "rknn")
endif()

# 查找 Qt5 (可选)
find_package(Qt5 COMPONENTS Core Gui Widgets Network QUIET)

# 公共源文件（移植的模块）
set(COMMON_SOURCES
    src/decoder/mpp_decoder.cpp
    src/decoder/mpp_encoder.cpp
    src/inference/postprocess.cpp
    src/inference/preprocess.cpp
    src/utils/drawing.cpp
)

# 主程序
add_executable(multi_cam_decoder
    src/main.cpp
)

# 压力测试
add_executable(stress_test
    src/stress_test.cpp
)

# RKNN 优化测试
add_executable(rknn_optimized
    src/rknn_optimized.cpp
)

# H.264/H.265 解码测试
add_executable(h264_decode_test
    src/h264_decode_test_main.cpp
    src/decoder/mpp_decoder.cpp
)

# 包含目录
foreach(target multi_cam_decoder stress_test rknn_optimized h264_decode_test)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MPP_INCLUDE_DIRS}
        ${RKNN_INCLUDE_DIRS}
        /usr/include/rockchip
        /usr/include/rga
    )
    
    # 链接库
    target_link_libraries(${target} PRIVATE
        ${MPP_LIBRARY}
        ${RGA_LIBRARY}
        ${RKNN_LIBRARY}
        pthread
        dl
    )
endforeach()

# Qt 多路视频测试程序 (如果找到 Qt5)
if(Qt5_FOUND)
    message(STATUS "Qt5 found, building Qt UI targets")
    
    # Qt MOC 自动处理
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    
    set(QT_LIBS Qt5::Core Qt5::Gui Qt5::Widgets)
    set(COMMON_LIBS ${MPP_LIBRARY} ${RGA_LIBRARY} ${RKNN_LIBRARY} pthread dl)
    set(COMMON_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MPP_INCLUDE_DIRS}
        ${RKNN_INCLUDE_DIRS}
        /usr/include/rockchip
        /usr/include/rga
    )
    
    # 简化版测试程序（模拟数据）
    add_executable(simple_qt_test src/qt_ui/simple_test.cpp)
    target_include_directories(simple_qt_test PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(simple_qt_test PRIVATE ${QT_LIBS} ${COMMON_LIBS})
    
    # 单路真实摄像头测试程序
    add_executable(real_camera_test src/qt_ui/real_camera_test.cpp)
    target_include_directories(real_camera_test PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(real_camera_test PRIVATE ${QT_LIBS} ${COMMON_LIBS})
    
    # 多路摄像头测试程序
    add_executable(multi_camera_test src/qt_ui/multi_camera_test.cpp)
    target_include_directories(multi_camera_test PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(multi_camera_test PRIVATE ${QT_LIBS} ${COMMON_LIBS})
    
    # 视频文件测试程序（奶牛检测）
    add_executable(video_test src/qt_ui/video_test.cpp)
    target_include_directories(video_test PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(video_test PRIVATE ${QT_LIBS} ${COMMON_LIBS})
    
    # FFmpeg 视频解码测试程序（更稳定）
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG libavformat libavcodec libavutil libswscale)
        if(FFMPEG_FOUND)
            add_executable(ffmpeg_video_test src/qt_ui/ffmpeg_video_test.cpp)
            target_include_directories(ffmpeg_video_test PRIVATE ${COMMON_INCLUDES} ${FFMPEG_INCLUDE_DIRS})
            target_link_libraries(ffmpeg_video_test PRIVATE ${QT_LIBS} ${COMMON_LIBS} ${FFMPEG_LIBRARIES})
            
            add_executable(rtsp_test src/qt_ui/rtsp_test.cpp)
            target_include_directories(rtsp_test PRIVATE ${COMMON_INCLUDES} ${FFMPEG_INCLUDE_DIRS})
            target_link_libraries(rtsp_test PRIVATE ${QT_LIBS} ${COMMON_LIBS} ${FFMPEG_LIBRARIES})
            
            add_executable(rtsp_video_test src/qt_ui/rtsp_video_test.cpp)
            target_include_directories(rtsp_video_test PRIVATE ${COMMON_INCLUDES} ${FFMPEG_INCLUDE_DIRS})
            target_link_libraries(rtsp_video_test PRIVATE ${QT_LIBS} ${COMMON_LIBS} ${FFMPEG_LIBRARIES})
            
            add_executable(multi_rtsp_test src/qt_ui/multi_rtsp_test.cpp)
            target_include_directories(multi_rtsp_test PRIVATE ${COMMON_INCLUDES} ${FFMPEG_INCLUDE_DIRS})
            target_link_libraries(multi_rtsp_test PRIVATE ${QT_LIBS} ${COMMON_LIBS} ${FFMPEG_LIBRARIES})
            
            add_executable(multi_rtsp_mpp_test src/qt_ui/multi_rtsp_mpp_test.cpp)
            # ========== Qt UI Benchmark ==========
            add_executable(multi_rtsp_benchmark_ui src/qt_ui/multi_rtsp_benchmark_ui.cpp)
            target_include_directories(multi_rtsp_benchmark_ui PRIVATE ${COMMON_INCLUDES} ${FFMPEG_INCLUDE_DIRS})
            target_link_libraries(multi_rtsp_benchmark_ui PRIVATE ${QT_LIBS} ${COMMON_LIBS} ${FFMPEG_LIBRARIES})
            target_include_directories(multi_rtsp_mpp_test PRIVATE ${COMMON_INCLUDES} ${FFMPEG_INCLUDE_DIRS})
            target_link_libraries(multi_rtsp_mpp_test PRIVATE ${QT_LIBS} ${COMMON_LIBS} ${FFMPEG_LIBRARIES})

            # ========== Multi-RTSP Benchmark ==========
            add_executable(multi_rtsp_benchmark src/multi_rtsp_benchmark.cpp)
            target_include_directories(multi_rtsp_benchmark PRIVATE ${COMMON_INCLUDES} ${FFMPEG_INCLUDE_DIRS})
            target_link_libraries(multi_rtsp_benchmark PRIVATE ${COMMON_LIBS} ${FFMPEG_LIBRARIES})
            
            message(STATUS "FFmpeg found, building ffmpeg_video_test, rtsp_test, rtsp_video_test, multi_rtsp_test, multi_rtsp_mpp_test")
        else()
            message(STATUS "FFmpeg not found, skipping FFmpeg-based targets")
        endif()
    endif()
else()
    message(STATUS "Qt5 not found, skipping Qt UI target")
endif()

# 安装目标
install(TARGETS multi_cam_decoder stress_test DESTINATION bin)

# 纯命令行 MPP+RKNN 测试（无 Qt 依赖）
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(CLI_FFMPEG libavformat libavcodec libavutil libswscale)
    if(CLI_FFMPEG_FOUND)
        add_executable(mpp_rtsp_cli_test src/mpp_rtsp_cli_test.cpp)
        target_include_directories(mpp_rtsp_cli_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${MPP_INCLUDE_DIRS}
            ${RKNN_INCLUDE_DIRS}
            /usr/include/rockchip
            /usr/include/rga
            ${CLI_FFMPEG_INCLUDE_DIRS}
        )
        target_link_libraries(mpp_rtsp_cli_test PRIVATE
            ${MPP_LIBRARY}
            ${RGA_LIBRARY}
            ${RKNN_LIBRARY}
            ${CLI_FFMPEG_LIBRARIES}
            pthread
            dl
        )
        message(STATUS "Added mpp_rtsp_cli_test target")
    endif()
endif()

