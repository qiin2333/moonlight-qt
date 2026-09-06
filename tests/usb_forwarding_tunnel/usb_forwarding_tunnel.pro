QT += core network
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = usb_forwarding_tunnel_probe
TEMPLATE = app
SOURCES += main.cpp ../../app/backend/usbforwardingtunnel.cpp
HEADERS += ../../app/backend/usbforwardingtunnel.h
