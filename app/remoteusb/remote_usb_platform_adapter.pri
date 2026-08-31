# Optional Qt-side Remote USB adapter boundary.
#
# The app includes this file unconditionally, but no Remote USB headers,
# sources, or libraries are enabled unless CONFIG+=remote_usb is supplied.
# This keeps the default qmake binary independent of the experimental feature.

contains(CONFIG, remote_usb_agent) {
    CONFIG += remote_usb
}

contains(CONFIG, remote_usb) {
        HEADERS += \
        $$PWD/remote_usb_platform_adapter.h \
        $$PWD/remote_usb_broker_client.h \
        $$PWD/remote_usb_tls_channel.h
        SOURCES += \
        $$PWD/remote_usb_broker_client.cpp \
        $$PWD/remote_usb_tls_channel.cpp
        contains(CONFIG, remote_usb_agent) {
            HEADERS += $$PWD/remote_usb_agent_client.h
            SOURCES += $$PWD/remote_usb_agent_client.cpp
            DEFINES += MOONLIGHT_REMOTE_USB_AGENT_CLIENT_ENABLED
        }
    DEFINES += MOONLIGHT_REMOTE_USB_ADAPTER_ENABLED

    # The libusb backend is deliberately a second opt-in.  The source file
    # always has a no-libusb stub, so requesting CONFIG+=remote_usb alone (or
    # building on a host without a USB development package) remains
    # linkable.  Add CONFIG+=remote_usb_libusb when a real backend is wanted.
    contains(CONFIG, remote_usb_libusb) {
        MOONLIGHT_REMOTE_USB_LIBUSB_ENABLED = 0
        HEADERS += $$PWD/remote_usb_libusb_adapter.h
        SOURCES += $$PWD/remote_usb_libusb_adapter.cpp

        isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR): \
            MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR = $$(MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR)
        isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY): \
            MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY = $$(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY)
        isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_ROOT): \
            MOONLIGHT_REMOTE_USB_LIBUSB_ROOT = $$(MOONLIGHT_REMOTE_USB_LIBUSB_ROOT)

        !isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR): \
            MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR = $$clean_path($$MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR)
        !isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY): \
            !contains(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY, ^-): \
            MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY = $$clean_path($$MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY)
        !isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_ROOT): \
            MOONLIGHT_REMOTE_USB_LIBUSB_ROOT = $$clean_path($$MOONLIGHT_REMOTE_USB_LIBUSB_ROOT)

        # An explicit root is useful on Windows and on non-standard Unix
        # installations.  A root may contain either include/libusb-1.0 or
        # include and lib directories; leave library naming to the toolchain.
        !isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_ROOT) {
            isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR): \
                MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR = $$MOONLIGHT_REMOTE_USB_LIBUSB_ROOT/include
            isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY) {
                MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY = -L$$MOONLIGHT_REMOTE_USB_LIBUSB_ROOT/lib -lusb-1.0
            }
        }

        !isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR) {
            INCLUDEPATH += $$MOONLIGHT_REMOTE_USB_LIBUSB_INCLUDE_DIR
        }
        !isEmpty(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY) {
            LIBS += $$split(MOONLIGHT_REMOTE_USB_LIBUSB_LIBRARY, " ")
            DEFINES += MOONLIGHT_REMOTE_USB_LIBUSB
            MOONLIGHT_REMOTE_USB_LIBUSB_ENABLED = 1
        } else:unix {
            # Homebrew, system packages, and most Linux distributions expose
            # a libusb-1.0.pc file.  packagesExist() avoids defining the real
            # backend when pkg-config cannot resolve the dependency.
            packagesExist(libusb-1.0) {
                CONFIG += link_pkgconfig
                PKGCONFIG += libusb-1.0
                DEFINES += MOONLIGHT_REMOTE_USB_LIBUSB
                MOONLIGHT_REMOTE_USB_LIBUSB_ENABLED = 1
            }
        }

        !equals(MOONLIGHT_REMOTE_USB_LIBUSB_ENABLED, 1) {
            warning("CONFIG+=remote_usb_libusb requested but libusb-1.0 was not found; using the unavailable stub")
        } else {
            unix:!macx {
                QMAKE_CXXFLAGS += -pthread
                QMAKE_LFLAGS += -pthread
            }
        }
    }

    # Build the pinned Rust core by default. Packagers may provide a prebuilt
    # static library and matching public header through the two overrides.
    isEmpty(MOONLIGHT_REMOTE_USB_CORE_DIR): \
        MOONLIGHT_REMOTE_USB_CORE_DIR = $$clean_path($$PWD/../../moonlight-remote-usb-core)
    isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
        MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR)
    isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY): \
        MOONLIGHT_REMOTE_USB_CORE_LIBRARY = $$(MOONLIGHT_REMOTE_USB_CORE_LIBRARY)
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
        MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$clean_path($$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR)
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY): \
        MOONLIGHT_REMOTE_USB_CORE_LIBRARY = $$clean_path($$MOONLIGHT_REMOTE_USB_CORE_LIBRARY)

    isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
        MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$MOONLIGHT_REMOTE_USB_CORE_DIR/include
    !exists($$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR/remoteusb.h): \
        error("Remote USB Rust C header is missing: $$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR/remoteusb.h")

    isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY) {
        !exists($$MOONLIGHT_REMOTE_USB_CORE_DIR/Cargo.toml): \
            error("Initialize the moonlight-remote-usb-core submodule")
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
    DEPENDPATH += $$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR
    LIBS += $$MOONLIGHT_REMOTE_USB_CORE_LIBRARY
    HEADERS += $$PWD/remote_usb_session_binding.h \
               $$PWD/remote_usb_core_binding.h
    SOURCES += $$PWD/remote_usb_session_binding.cpp
    DEFINES += MOONLIGHT_REMOTE_USB_RUST_CORE
    unix:!macx: LIBS += -ldl -lpthread -lm
    win32: LIBS += -ladvapi32 -lbcrypt -lntdll -luserenv -lws2_32

    equals(MOONLIGHT_REMOTE_USB_LIBUSB_ENABLED, 1) {
        HEADERS += $$PWD/remote_usb_session_coordinator.h
        SOURCES += $$PWD/remote_usb_session_coordinator.cpp
        DEFINES += MOONLIGHT_REMOTE_USB_SESSION_ENABLED
    }
}
