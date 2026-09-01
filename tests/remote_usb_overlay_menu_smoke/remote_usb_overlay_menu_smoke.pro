QT += core gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = remote_usb_overlay_menu_smoke

INCLUDEPATH += ../../app
SOURCES += \
    main.cpp \
    ../../app/streaming/video/overlaymenupanel.cpp
HEADERS += \
    ../../app/streaming/video/overlaymenupanel.h
