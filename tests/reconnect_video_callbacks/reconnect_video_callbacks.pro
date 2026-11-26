CONFIG += c++17 console
CONFIG -= app_bundle qt

TARGET = reconnect_video_callbacks
TEMPLATE = app

COMMON_C = $$PWD/../../moonlight-common-c/moonlight-common-c

INCLUDEPATH += \
    $$PWD/../../app \
    $$COMMON_C/src \
    $$COMMON_C/enet/include

DEFINES += HAS_SOCKLEN_T

# FakeCallbacks.c includes common-c's C11 atomic declarations.
contains(QMAKE_COMPILER, msvc) {
    QMAKE_CFLAGS += /std:c11 /experimental:c11atomics
}

SOURCES += \
    main.cpp \
    $$COMMON_C/src/FakeCallbacks.c

HEADERS += \
    ../../app/streaming/videocallbacks.h
