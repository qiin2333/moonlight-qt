CONFIG += c++17 console
CONFIG -= app_bundle qt

TARGET = virtual_file_clipboard
TEMPLATE = app

INCLUDEPATH += $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/filetransfer/virtualfileclipboard_win.cpp

HEADERS += \
    ../../app/streaming/filetransfer/virtualfilesource.h \
    ../../app/streaming/filetransfer/virtualfileclipboard_win.h

LIBS += -lole32 -lshell32 -luser32
