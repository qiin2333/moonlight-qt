QT += core gui network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = clipboard_payload_routing
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/clipboardsync.cpp

macx:SOURCES += ../../clipboard-helper/macos.mm

HEADERS += \
    ../../app/streaming/clipboardlogging.h \
    ../../app/streaming/clipboardsync.h

SOURCES += regression.cpp file_offer.cpp ../../app/streaming/clipboardipc.cpp
HEADERS += ../../app/streaming/clipboardipc.h
RESOURCES += fixtures.qrc

# Files the host copied (clipboard kind 4)
SOURCES += ../../app/streaming/filetransfer/fileoffer.cpp
HEADERS += ../../app/streaming/filetransfer/fileoffer.h ../../app/streaming/filetransfer/virtualfilesource.h
win32 {
    SOURCES += ../../app/streaming/filetransfer/virtualfileclipboard_win.cpp
    HEADERS += ../../app/streaming/filetransfer/virtualfileclipboard_win.h
    LIBS += -lole32 -lshell32 -luser32
}
