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

isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
    MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR)
isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY): \
    MOONLIGHT_REMOTE_USB_CORE_LIBRARY = $$(MOONLIGHT_REMOTE_USB_CORE_LIBRARY)
MOONLIGHT_REMOTE_USB_CORE_DIR = $$clean_path($$PWD/../moonlight-remote-usb-core)
isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
    MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$MOONLIGHT_REMOTE_USB_CORE_DIR/include
!exists($$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR/remoteusb.h): \
    error("Initialize the moonlight-remote-usb-core submodule")

isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY) {
    CONFIG(debug, debug|release) {
        MOONLIGHT_REMOTE_USB_RUST_PROFILE = debug
        MOONLIGHT_REMOTE_USB_CARGO_FLAGS =
    } else {
        MOONLIGHT_REMOTE_USB_RUST_PROFILE = release
        MOONLIGHT_REMOTE_USB_CARGO_FLAGS = --release
    }
    win32: MOONLIGHT_REMOTE_USB_CORE_LIBRARY = $$MOONLIGHT_REMOTE_USB_CORE_DIR/target/$$MOONLIGHT_REMOTE_USB_RUST_PROFILE/moonlight_remote_usb_core.lib
    else: MOONLIGHT_REMOTE_USB_CORE_LIBRARY = $$MOONLIGHT_REMOTE_USB_CORE_DIR/target/$$MOONLIGHT_REMOTE_USB_RUST_PROFILE/libmoonlight_remote_usb_core.a
    remote_usb_rust_core.target = remote_usb_rust_core_build
    remote_usb_rust_core.commands = cargo build --locked $$MOONLIGHT_REMOTE_USB_CARGO_FLAGS --manifest-path $$MOONLIGHT_REMOTE_USB_CORE_DIR/Cargo.toml
    remote_usb_rust_core.CONFIG += phony
    QMAKE_EXTRA_TARGETS += remote_usb_rust_core
    PRE_TARGETDEPS += remote_usb_rust_core_build
}

INCLUDEPATH += $$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR
LIBS += $$MOONLIGHT_REMOTE_USB_CORE_LIBRARY
unix:!macx: LIBS += -ldl -lpthread -lm
win32: LIBS += -ladvapi32 -lbcrypt -lntdll -luserenv -lws2_32
DEFINES += MOONLIGHT_USB_AGENT_RUNTIME MOONLIGHT_REMOTE_USB_RUST_CORE

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
