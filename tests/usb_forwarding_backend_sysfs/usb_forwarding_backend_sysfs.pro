QT += core network
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = usb_forwarding_backend_sysfs_test
INCLUDEPATH += ../../app
SOURCES += main.cpp \
    ../../app/backend/usbforwardingbackend.cpp \
    ../../app/backend/usbforwardingenvironment.cpp \
    ../../app/backend/usbforwardinglocalserver.cpp
HEADERS += \
    ../../app/backend/usbforwardingbackend.h \
    ../../app/backend/usbforwardingenvironment.h \
    ../../app/backend/usbforwardinglocalserver.h
win32:LIBS += -ladvapi32 -lshell32
