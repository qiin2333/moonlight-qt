QT += core gui gui-private
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle

INCLUDEPATH += ../../app

packagesExist(xcb) {
    DEFINES += HAVE_XCB_DISPLAY_MONITOR
    PKGCONFIG += xcb
}

SOURCES += \
    main.cpp \
    ../../app/streaming/video/overlayeventmonitor_linux.cpp

HEADERS += ../../app/streaming/video/overlayeventmonitor_linux.h
