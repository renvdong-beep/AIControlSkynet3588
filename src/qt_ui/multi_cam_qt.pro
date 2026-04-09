# AIControlSkynet3588 Qt 多路视频解码推理测试
QT += core gui widgets network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17
CONFIG += debug_and_release

# 目标名称
TARGET = multi_cam_qt
TEMPLATE = app

# RK3588 平台库路径
MPP_LIBS = -L/usr/lib/aarch64-linux-gnu -lrockchip_mpp
RGA_LIBS = -L/usr/lib/aarch64-linux-gnu -lrga
RKNN_LIBS = -L/usr/lib -lrknnrt

# RKNN 头文件路径
RKNN_INCLUDE = /home/topeet/rknpu2/runtime/RK3588/Linux/librknn_api/include

# 包含路径
INCLUDEPATH += .. \
               ../decoder \
               ../inference \
               ../utils \
               /usr/include/rockchip \
               /usr/include/rga \
               $$RKNN_INCLUDE

# 源文件
SOURCES += \
    main_qt.cpp \
    videocontrol.cpp \
    frmvideocontrol.cpp \
    frmwidget.cpp \
    mousemenu.cpp \
    mpprknnvideo.cpp \
    ../decoder/mpp_decoder.cpp \
    ../decoder/mpp_encoder.cpp \
    ../inference/postprocess.cpp \
    ../inference/preprocess.cpp \
    ../utils/drawing.cpp

# 头文件
HEADERS += \
    videocontrol.h \
    frmvideocontrol.h \
    frmwidget.h \
    mousemenu.h \
    mpprknnvideo.h \
    ../decoder/mpp_decoder.h \
    ../decoder/mpp_encoder.h \
    ../inference/postprocess.h \
    ../inference/preprocess.h \
    ../utils/drawing.h

# UI 文件
FORMS += \
    frmvideocontrol.ui

# 链接库
LIBS += $$MPP_LIBS $$RGA_LIBS $$RKNN_LIBS -lpthread -ldl

# 编译选项
QMAKE_CXXFLAGS += -Wall -Wextra -O2

# 安装
target.path = /usr/local/bin
INSTALLS += target
