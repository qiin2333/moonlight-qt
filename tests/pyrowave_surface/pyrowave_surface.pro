QT -= core gui
CONFIG += console c++17
CONFIG -= app_bundle
include(../../config.tests/pyrowave/dependency.pri)
win32 {
    INCLUDEPATH += $$PYROWAVE_DEPS/include/$$PYROWAVE_ARCH
    LIBS += -llibplacebo -lavutil -lavcodec -lwinmm
} else {
    PKGCONFIG += libplacebo libavutil libavcodec libavformat
}
INCLUDEPATH += ../../moonlight-common-c/moonlight-common-c/src
SOURCES += main.cpp ../../app/streaming/video/pyrowavesurfaces.cpp \
           ../../app/streaming/video/ffmpeg-renderers/plvk_c.c
