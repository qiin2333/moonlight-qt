QT += core gui network
CONFIG += c++17
CONFIG -= app_bundle

TARGET = moonlight-clipboard-helper
TEMPLATE = app

include(../globaldefs.pri)

INCLUDEPATH += $$PWD/../app

SOURCES += \
    main.cpp \
    ../app/streaming/clipboardipc.cpp \
    ../app/streaming/clipboardsync.cpp

macx:SOURCES += macos.mm

HEADERS += \
    ../app/streaming/clipboardipc.h \
    ../app/streaming/clipboardlogging.h \
    ../app/streaming/clipboardsync.h

unix:!macx {
    isEmpty(PREFIX) {
        PREFIX = /usr/local
    }
    isEmpty(BINDIR) {
        BINDIR = bin
    }

    target.path = $$PREFIX/$$BINDIR/
    INSTALLS += target
}
