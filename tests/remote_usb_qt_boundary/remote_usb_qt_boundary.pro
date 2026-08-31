QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = remote_usb_qt_boundary
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app \
    $$PWD/../../moonlight-common-c/moonlight-common-c/src

REMOTE_USB_CORE_DIR = $$clean_path($$PWD/../../moonlight-remote-usb-core)
INCLUDEPATH += $$REMOTE_USB_CORE_DIR/include
macx: REMOTE_USB_CORE_LIBRARY = $$REMOTE_USB_CORE_DIR/target/debug/libmoonlight_remote_usb_core.a
unix:!macx: REMOTE_USB_CORE_LIBRARY = $$REMOTE_USB_CORE_DIR/target/debug/libmoonlight_remote_usb_core.a
win32: REMOTE_USB_CORE_LIBRARY = $$REMOTE_USB_CORE_DIR/target/debug/moonlight_remote_usb_core.lib
remote_usb_rust_core.target = remote_usb_rust_core_build
remote_usb_rust_core.commands = cargo build --locked --manifest-path $$REMOTE_USB_CORE_DIR/Cargo.toml
remote_usb_rust_core.CONFIG += phony
QMAKE_EXTRA_TARGETS += remote_usb_rust_core
PRE_TARGETDEPS += remote_usb_rust_core_build
LIBS += $$REMOTE_USB_CORE_LIBRARY
unix:!macx: LIBS += -ldl -lpthread -lm
win32: LIBS += -ladvapi32 -lbcrypt -lntdll -luserenv -lws2_32

SOURCES += \
    main.cpp \
    ../../app/remoteusb/remote_usb_broker_client.cpp \
    ../../app/remoteusb/remote_usb_session_binding.cpp \
    ../../app/remoteusb/remote_usb_tls_channel.cpp

HEADERS += \
    ../../app/remoteusb/remote_usb_platform_adapter.h \
    ../../app/remoteusb/remote_usb_broker_client.h \
    ../../app/remoteusb/remote_usb_session_binding.h \
    ../../app/remoteusb/remote_usb_tls_channel.h
