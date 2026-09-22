QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = usb_forwarding_linux_test
!linux|android: error(This test requires desktop Linux)
SOURCES += main.cpp ../../usb-helper/linux/nativeusb.cpp
HEADERS += ../../usb-helper/linux/nativeusb.h
LIBS += -pthread
