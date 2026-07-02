INCLUDEPATH += $$PWD

SOURCES += \
    $$PWD/protocol/file_mapping_client.cpp \
    $$PWD/vfs/remote_vfs.cpp \
    $$PWD/vfs/protocol_remote_vfs.cpp \
    $$PWD/mount/mac_file_provider_bridge_stub.cpp \
    $$PWD/mount/mac_file_provider_mount_provider.cpp \
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
    $$PWD/mount/mac_file_provider_bridge.h \
    $$PWD/mount/mac_file_provider_mount_provider.h \
    $$PWD/mount/macfuse_mount_provider.h \
    $$PWD/mount/mount_coordinator.h \
    $$PWD/mount/macos_finder_mirror_provider.h \
    $$PWD/mount/macfuse_runtime_abi.h \
    $$PWD/mount/mount_errors.h \
    $$PWD/mount/mount_provider.h \
    $$PWD/mount/mount_provider_factory.h \
    $$PWD/mount/mount_session.h \
    $$PWD/mount/unavailable_mount_provider.h \
    $$PWD/mount/windows_explorer_mirror_provider.h

macx {
    SOURCES -= $$PWD/mount/mac_file_provider_bridge_stub.cpp
    SOURCES += $$PWD/mount/mac_file_provider_bridge.mm
    LIBS += -framework FileProvider
    message(macFUSE host file mounting enabled with runtime loading)
}
