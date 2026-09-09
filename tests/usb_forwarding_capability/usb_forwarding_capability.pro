QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = usb_forwarding_capability_test
SOURCES += main.cpp
HEADERS += ../../app/backend/usbforwardingcapability.h
