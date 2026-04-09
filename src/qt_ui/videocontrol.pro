#-------------------------------------------------
#
# Project created by QtCreator 2017-01-05T22:11:54
#
#-------------------------------------------------

QT       += core gui network widgets


greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET      = videocontrol
TEMPLATE    = app
DESTDIR     = $$PWD/../bin
CONFIG      += warn_off

SOURCES     += main.cpp \
    datathread.cpp \
    #excelbash.cpp \
    controlconfig.cpp \
    frmwidget.cpp \
    clientto3399.cpp \
    mousemenu.cpp \
    mpprknnvideo.cpp \
    msgbox.cpp \
    nv12render.cpp \
    postprocess.cpp \
    preprocess.cpp \
    udpthread.cpp \
    utils/drawing.cpp \
    utils/mpp_decoder.cpp \
    utils/mpp_encoder.cpp \
    warningwidget.cpp \
    settingpage.cpp \
    warningwidgetlist.cpp \
    widget.cpp \
    workThread.cpp
SOURCES     += frmvideocontrol.cpp
SOURCES     += videocontrol.cpp

HEADERS     += frmvideocontrol.h \
    datathread.h \
    #excelbash.h \
    controlconfig.h \
    frmwidget.h \
    clientto3399.h \
    mousemenu.h \
    mpprknnvideo.h \
    msgbox.h \
    nv12render.h \
    postprocess.h \
    preprocess.h \
    rknn_api.h \
    udpthread.h \
    utils/drawing.h \
    utils/mpp_decoder.h \
    utils/mpp_encoder.h \
    warningwidget.h \
    settingpage.h \
    warningwidgetlist.h \
    widget.h \
    workThread.h
HEADERS     += videocontrol.h

FORMS       += frmvideocontrol.ui \
    frmwidget.ui \
    msgbox.ui \
    warningwidget.ui \
    settingpage.ui \
    warningwidgetlist.ui

INCLUDEPATH += $$PWD
INCLUDEPATH += $$PWD/api
INCLUDEPATH += $$PWD/rga/RK3588/include/
INCLUDEPATH += $$PWD/utils/
INCLUDEPATH += $$PWD/zlmediakit/include/
INCLUDEPATH += /usr/include/aarch64-linux-gnu
#INCLUDEPATH += $$PWD/form

include ($$PWD/api/api.pri)
#include ($$PWD/form/form.pri)

RESOURCES += \
    main.qrc

#CONFIG += console

#LIBS += -L$$PWD -lrknn_api -lrknnrt.so
LIBS += -L/usr/lib/ -lrknnrt
#LIBS += -L$$PWD/rga/RK3588/lib/Linux/aarch64/ -lrga
LIBS += -L$$PWD/lib/Linux/aarch64/ -lopencv_calib3d -lopencv_core -lopencv_dnn -lopencv_features2d -lopencv_imgcodecs -lopencv_imgproc -lopencv_video
#LIBS += -L$$PWD -lrockchip_mpp
LIBS += -L$$PWD/zlmediakit/aarch64/ -lmk_api
LIBS += -L/usr/lib/aarch64-linux-gnu/ -lavcodec -lavfilter -lavformat -lavresample -lavutil -lswscale \
     -lavfilter -lavformat -lavdevice -lavcodec -lswscale -lavutil -lswresample -lavdevice -lpthread -lm -lz -lrt -ldl  -lrga -lrockchip_mpp
CONFIG(release, debug|release){
RESOURCES += res.rc
}

RC_FILE= logo.rc

INCLUDEPATH += $$PWD/ffmpeg
include ($$PWD/ffmpeg/ffmpeg.pri)

DISTFILES +=


