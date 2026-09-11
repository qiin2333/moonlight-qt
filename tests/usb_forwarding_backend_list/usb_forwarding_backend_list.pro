QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = usb_forwarding_backend_list_test
INCLUDEPATH += ../../app
SOURCES += main.cpp \
    ../../app/backend/usbforwardingbackend.cpp \
    ../../app/backend/usbforwardingenvironment.cpp
HEADERS += \
    ../../app/backend/usbforwardingbackend.h \
    ../../app/backend/usbforwardingenvironment.h \
    ../../app/backend/usbforwardinglocalserver.h
win32:LIBS += -ladvapi32 -lshell32
