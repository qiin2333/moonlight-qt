QT += core network
CONFIG += c++17 console
TEMPLATE = app
TARGET = remote_usb_agent_client_smoke

INCLUDEPATH += ../../app ../../usb-agent
SOURCES += \
    main.cpp \
    ../../app/remoteusb/remote_usb_agent_client.cpp
HEADERS += \
    ../../app/remoteusb/remote_usb_agent_client.h \
    ../../usb-agent/usb_agent_protocol.h
