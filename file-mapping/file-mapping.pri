INCLUDEPATH += $$PWD

SOURCES += \
    $$PWD/protocol/file_mapping_client.cpp \
    $$PWD/vfs/remote_vfs.cpp \
    $$PWD/vfs/protocol_remote_vfs.cpp \
    $$PWD/mount/macfuse_mount_provider.cpp \
    $$PWD/mount/mount_coordinator.cpp \
    $$PWD/mount/macos_finder_mirror_provider.cpp \
    $$PWD/mount/mount_provider.cpp \
    $$PWD/mount/mount_provider_factory.cpp \
    $$PWD/mount/mount_session.cpp \
    $$PWD/mount/unavailable_mount_provider.cpp \
    $$PWD/mount/windows_explorer_mirror_provider.cpp

HEADERS += \
    $$PWD/protocol/file_mapping_client.h \
    $$PWD/protocol/file_mapping_errors.h \
    $$PWD/protocol/file_mapping_messages.h \
    $$PWD/vfs/remote_vfs.h \
    $$PWD/vfs/protocol_remote_vfs.h \
    $$PWD/vfs/vfs_handle.h \
    $$PWD/vfs/vfs_item.h \
    $$PWD/mount/macfuse_mount_provider.h \
    $$PWD/mount/mount_coordinator.h \
    $$PWD/mount/macos_finder_mirror_provider.h \
    $$PWD/mount/mount_errors.h \
    $$PWD/mount/mount_provider.h \
    $$PWD/mount/mount_provider_factory.h \
    $$PWD/mount/mount_session.h \
    $$PWD/mount/unavailable_mount_provider.h \
    $$PWD/mount/windows_explorer_mirror_provider.h

macx {
    MACFUSE_INCLUDE =
    MACFUSE_LIB =

    isEmpty(MACFUSE_INCLUDE):exists(/usr/local/include/osxfuse/fuse/fuse.h): MACFUSE_INCLUDE = /usr/local/include/osxfuse/fuse
    isEmpty(MACFUSE_INCLUDE):exists(/opt/homebrew/include/osxfuse/fuse/fuse.h): MACFUSE_INCLUDE = /opt/homebrew/include/osxfuse/fuse
    isEmpty(MACFUSE_INCLUDE):exists(/usr/local/include/fuse.h): MACFUSE_INCLUDE = /usr/local/include
    isEmpty(MACFUSE_INCLUDE):exists(/opt/homebrew/include/fuse.h): MACFUSE_INCLUDE = /opt/homebrew/include

    isEmpty(MACFUSE_LIB):exists(/usr/local/lib/libfuse.dylib): MACFUSE_LIB = -L/usr/local/lib -lfuse
    isEmpty(MACFUSE_LIB):exists(/opt/homebrew/lib/libfuse.dylib): MACFUSE_LIB = -L/opt/homebrew/lib -lfuse
    isEmpty(MACFUSE_LIB):exists(/usr/local/lib/libosxfuse.dylib): MACFUSE_LIB = -L/usr/local/lib -losxfuse
    isEmpty(MACFUSE_LIB):exists(/opt/homebrew/lib/libosxfuse.dylib): MACFUSE_LIB = -L/opt/homebrew/lib -losxfuse

    !isEmpty(MACFUSE_INCLUDE):!isEmpty(MACFUSE_LIB) {
        DEFINES += MOONLIGHT_MACFUSE_ENABLED
        INCLUDEPATH += $$MACFUSE_INCLUDE
        LIBS += $$MACFUSE_LIB
    }
}
