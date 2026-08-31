QT += core gui
CONFIG += console c++17
CONFIG -= app_bundle

win32: LIBS += user32.lib

INCLUDEPATH += ../../app

SOURCES += \
    ../../app/settings/devicelocalsettings.cpp \
    ../../app/streaming/video/overlaybuttonposition.cpp \
    ../../app/streaming/video/overlaymenubutton.cpp

macx {
    INCLUDEPATH += ../../libs/mac/include ../../libs/mac/include/SDL2
    LIBS += -L../../libs/mac/lib -lSDL2
    SOURCES += \
        main_mac.mm \
        ../../app/streaming/video/macqteventpumpinputguard.mm \
        ../../app/streaming/video/overlayeventmonitor_mac.mm
} else:linux {
    QT += gui-private
    CONFIG += link_pkgconfig
    DEFINES += HAVE_LINUX_DISPLAY_EVENT_MONITOR HAVE_XCB_DISPLAY_MONITOR
    PKGCONFIG += x11 xtst xcb
    SOURCES += \
        main_linux.cpp \
        ../../app/streaming/video/overlayeventmonitor_linux.cpp
} else {
    SOURCES += main.cpp
}

HEADERS += \
    ../../app/settings/devicelocalsettings.h \
    ../../app/streaming/video/overlaybuttonposition.h \
    ../../app/streaming/video/overlayeventwakestate.h \
    ../../app/streaming/video/overlaymenubutton.h

macx: HEADERS += ../../app/streaming/video/overlayeventmonitor_mac.h
macx: HEADERS += ../../app/streaming/video/macqteventpumpinputguard.h
linux: HEADERS += ../../app/streaming/video/overlayeventmonitor_linux.h
