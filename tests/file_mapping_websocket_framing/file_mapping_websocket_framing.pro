QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = file_mapping_websocket_framing
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/filemappingwebsocket.cpp

HEADERS += \
    ../../app/streaming/filemappingwebsocket.h
