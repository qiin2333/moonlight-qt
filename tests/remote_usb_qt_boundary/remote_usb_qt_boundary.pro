QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = remote_usb_qt_boundary
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app \
    $$PWD/../../moonlight-common-c/moonlight-common-c/src

isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR): \
    MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR = $$(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR)
isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR) {
    error("Set MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR to the shared-core remoteusb source directory")
}
MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR = \
    $$clean_path($$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR)
!exists($$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR) {
    error("Remote USB shared core source directory is missing: $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR")
}
REMOTE_USB_CORE_DIR = $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR
INCLUDEPATH += $$REMOTE_USB_CORE_DIR

for(core_source, \
    remote_usb_wire.c \
    remote_usb_broker.c \
    remote_usb_pdu.c \
    remote_usb_usbip.c \
    remote_usb_executor.c \
    remote_usb_transport.c \
    remote_usb_session.c) {
    !exists($$REMOTE_USB_CORE_DIR/$$core_source) {
        error("Remote USB shared core source is missing: $$REMOTE_USB_CORE_DIR/$$core_source")
    }
}

SOURCES += \
    main.cpp \
    ../../app/remoteusb/remote_usb_broker_client.cpp \
    ../../app/remoteusb/remote_usb_session_binding.cpp \
    ../../app/remoteusb/remote_usb_tls_channel.cpp \
    $$REMOTE_USB_CORE_DIR/remote_usb_wire.c \
    $$REMOTE_USB_CORE_DIR/remote_usb_broker.c \
    $$REMOTE_USB_CORE_DIR/remote_usb_pdu.c \
    $$REMOTE_USB_CORE_DIR/remote_usb_usbip.c \
    $$REMOTE_USB_CORE_DIR/remote_usb_executor.c \
    $$REMOTE_USB_CORE_DIR/remote_usb_transport.c \
    $$REMOTE_USB_CORE_DIR/remote_usb_session.c

HEADERS += \
    ../../app/remoteusb/remote_usb_platform_adapter.h \
    ../../app/remoteusb/remote_usb_broker_client.h \
    ../../app/remoteusb/remote_usb_session_binding.h \
    ../../app/remoteusb/remote_usb_tls_channel.h

QMAKE_CFLAGS += -std=gnu11
unix:!macx: QMAKE_CFLAGS += -pthread
unix:!macx: QMAKE_LFLAGS += -pthread
