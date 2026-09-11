QT += core qml
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = usb_forwarding_backend_list_test
INCLUDEPATH += ../../app
SOURCES += main.cpp \
    ../../app/backend/usbforwardingbackend.cpp \
    ../../app/backend/usbforwardingenvironment.cpp \
    ../../app/backend/usbforwardinglocalserver.cpp \
    ../../app/settings/streamingpreferences.cpp
HEADERS += \
    ../../app/backend/usbforwardingbackend.h \
    ../../app/backend/usbforwardingenvironment.h \
    ../../app/backend/usbforwardinglocalserver.h \
    ../../app/settings/streamingpreferences.h
win32:LIBS += -ladvapi32 -lshell32
