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

    # A source checkout can be compiled directly.  The directory must contain
    # the platform-neutral C files from remoteusb/shared-core's source list.
    # An installed static library can be selected instead below.
    isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR): \
        MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR = $$(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR)
    isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
        MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR)
    isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY): \
        MOONLIGHT_REMOTE_USB_CORE_LIBRARY = $$(MOONLIGHT_REMOTE_USB_CORE_LIBRARY)

    # Normalize user/environment supplied paths before testing or handing them
    # to qmake.  A source checkout and an installed library are alternatives;
    # accepting both would make duplicate symbols and selection order depend on
    # the linker.
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR): \
        MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR = $$clean_path($$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR)
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
        MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$clean_path($$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR)
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY): \
        MOONLIGHT_REMOTE_USB_CORE_LIBRARY = $$clean_path($$MOONLIGHT_REMOTE_USB_CORE_LIBRARY)

    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR):!isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY) {
        error("Choose either MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR or " \
              "MOONLIGHT_REMOTE_USB_CORE_LIBRARY, not both")
    }
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR):!exists($$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR) {
        error("Remote USB shared core source directory is missing: " \
              "$$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR")
    }
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR):!exists($$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR) {
        error("Remote USB shared core include directory is missing: " \
              "$$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR")
    }
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY):!exists($$MOONLIGHT_REMOTE_USB_CORE_LIBRARY) {
        error("Remote USB shared core library is missing: " \
              "$$MOONLIGHT_REMOTE_USB_CORE_LIBRARY")
    }

    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR) {
        isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR): \
            MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR = $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR

        INCLUDEPATH += $$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR
        DEPENDPATH += $$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR
        MOONLIGHT_REMOTE_USB_CORE_SOURCES = \
            $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_wire.c \
            $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_broker.c \
            $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_pdu.c \
            $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_usbip.c \
            $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_executor.c \
            $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_transport.c \
            $$MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR/remote_usb_session.c
        for(core_source, MOONLIGHT_REMOTE_USB_CORE_SOURCES) {
            !exists($$core_source): error("Remote USB shared core source is missing: $$core_source")
        }
        SOURCES += $$MOONLIGHT_REMOTE_USB_CORE_SOURCES
        HEADERS += $$PWD/remote_usb_session_binding.h \
                   $$PWD/remote_usb_core_binding.h
        SOURCES += $$PWD/remote_usb_session_binding.cpp
        DEFINES += MOONLIGHT_REMOTE_USB_CORE_SOURCE

        # The shared core is C11.  Use the GNU dialect here because the Qt app
        # already has optional C units that rely on compiler extensions (for
        # example masterhook.c); keep the default qmake build untouched and
        # request this only when source mode is explicitly enabled.
        unix {
            QMAKE_CFLAGS += -std=gnu11
        }
        unix:!macx {
            QMAKE_CFLAGS += -pthread
            QMAKE_LFLAGS += -pthread
        }
    } else {
        !isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY) {
            HEADERS += $$PWD/remote_usb_session_binding.h \
                       $$PWD/remote_usb_core_binding.h
            SOURCES += $$PWD/remote_usb_session_binding.cpp
            !isEmpty(MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR) {
                INCLUDEPATH += $$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR
                DEPENDPATH += $$MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR
            }
            LIBS += $$MOONLIGHT_REMOTE_USB_CORE_LIBRARY
            DEFINES += MOONLIGHT_REMOTE_USB_CORE_LIBRARY

            # The static core uses pthread mutexes on POSIX.  CMake exports
            # this transitively, while a raw library path supplied to qmake
            # does not, so carry the link requirement here.
            unix:!macx: QMAKE_LFLAGS += -pthread
        } else {
            warning("CONFIG+=remote_usb requested without a shared core source or library; " \
                    "only the adapter interface is enabled")
        }
    }

    # The high-level lifecycle requires both the shared C session and a native
    # USB adapter. Keep it out of partial/stub builds so the default app can
    # still compile without either dependency.
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_SOURCE_DIR):equals(MOONLIGHT_REMOTE_USB_LIBUSB_ENABLED, 1) {
        HEADERS += $$PWD/remote_usb_session_coordinator.h
        SOURCES += $$PWD/remote_usb_session_coordinator.cpp
        DEFINES += MOONLIGHT_REMOTE_USB_SESSION_ENABLED
    }
    !isEmpty(MOONLIGHT_REMOTE_USB_CORE_LIBRARY):equals(MOONLIGHT_REMOTE_USB_LIBUSB_ENABLED, 1) {
        HEADERS += $$PWD/remote_usb_session_coordinator.h
        SOURCES += $$PWD/remote_usb_session_coordinator.cpp
        DEFINES += MOONLIGHT_REMOTE_USB_SESSION_ENABLED
    }
}
