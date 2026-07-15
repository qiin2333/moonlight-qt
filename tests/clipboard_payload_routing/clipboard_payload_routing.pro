QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = clipboard_payload_routing
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp
