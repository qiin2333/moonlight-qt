QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = moonlight-usb-agent

# The top-level Moonlight project exposes debug/release targets. Match those
# targets when bundled, while keeping direct Agent builds single-config.
contains(CONFIG, remote_usb_agent) {
    CONFIG += debug_and_release
    CONFIG(debug, debug|release) {
        DESTDIR = $$OUT_PWD/debug
    } else {
        DESTDIR = $$OUT_PWD/release
    }
}

INCLUDEPATH += ../app

isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR): \
    MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR = $$(MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR)
isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY): \
    MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY = $$(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY)
!isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR) {
    INCLUDEPATH += $$MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR
}
!isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY) {
    LIBS += $$split(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY, " ")
    DEFINES += MOONLIGHT_REMOTE_USB_LIBUSB
} else:unix {
    packagesExist(libusb-1.0) {
        CONFIG += link_pkgconfig
        PKGCONFIG += libusb-1.0
        DEFINES += MOONLIGHT_REMOTE_USB_LIBUSB
    }
}

isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR): \
    MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR = $$(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR)
isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
    MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR)
isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY): \
    MOONLIGHT_REMOTE_USB_CORE_LIBRARY = $$(MOONLIGHT_REMOTE_USB_CORE_LIBRARY)

!isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR):!isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY) {
    error("Choose either MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR or MOONLIGHT_REMOTE_USB_CORE_LIBRARY")
}

!isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR) {
    MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR = $$clean_path($$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR)
    isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
        MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR
    INCLUDEPATH += $$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR
    REMOTE_USB_CORE_SOURCES = \
        $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_wire.c \
        $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_broker.c \
        $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_pdu.c \
        $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_usbip.c \
        $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_executor.c \
        $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_transport.c \
        $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_session.c
    for(core_source, REMOTE_USB_CORE_SOURCES) {
        !exists($$core_source): error("Remote USB shared core source is missing: $$core_source")
    }
    SOURCES += $$REMOTE_USB_CORE_SOURCES
    QMAKE_CFLAGS += -std=gnu11
    DEFINES += MOONLIGHT_USB_AGENT_RUNTIME
} else:!isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY) {
    isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR) {
        error("MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR is required with a core library")
    }
    INCLUDEPATH += $$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR
    LIBS += $$split(MOONLIGHT_REMOTE_USB_CORE_LIBRARY, " ")
    DEFINES += MOONLIGHT_USB_AGENT_RUNTIME
}

contains(DEFINES, MOONLIGHT_USB_AGENT_RUNTIME) {
    SOURCES += \
        ../app/remoteusb/remote_usb_broker_client.cpp \
        ../app/remoteusb/remote_usb_tls_channel.cpp \
        ../app/remoteusb/remote_usb_session_binding.cpp
    HEADERS += \
        ../app/remoteusb/remote_usb_broker_client.h \
        ../app/remoteusb/remote_usb_tls_channel.h \
        ../app/remoteusb/remote_usb_session_binding.h \
        ../app/remoteusb/remote_usb_core_binding.h
}

SOURCES += \
    main.cpp \
    usb_agent_server.cpp \
    usb_agent_backend.cpp \
    ../app/remoteusb/remote_usb_libusb_adapter.cpp

HEADERS += \
    usb_agent_protocol.h \
    usb_agent_server.h \
    usb_agent_backend.h \
    ../app/remoteusb/remote_usb_libusb_adapter.h \
    ../app/remoteusb/remote_usb_platform_adapter.h
