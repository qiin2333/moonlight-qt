INCLUDEPATH += $$PWD

SOURCES += \
    $$PWD/protocol/file_mapping_client.cpp \
    $$PWD/vfs/remote_vfs.cpp \
    $$PWD/mount/mount_provider.cpp \
    $$PWD/mount/mount_session.cpp

HEADERS += \
    $$PWD/protocol/file_mapping_client.h \
    $$PWD/protocol/file_mapping_errors.h \
    $$PWD/protocol/file_mapping_messages.h \
    $$PWD/vfs/remote_vfs.h \
    $$PWD/vfs/vfs_handle.h \
    $$PWD/vfs/vfs_item.h \
    $$PWD/mount/mount_errors.h \
    $$PWD/mount/mount_provider.h \
    $$PWD/mount/mount_session.h
