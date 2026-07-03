QT += core gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = file_mapping_mirror_e2e
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../file-mapping

SOURCES += \
    main.cpp \
    ../../file-mapping/vfs/remote_vfs.cpp \
    ../../file-mapping/mount/mount_provider.cpp \
    ../../file-mapping/mount/mount_coordinator.cpp \
    ../../file-mapping/mount/macos_finder_mirror_provider.cpp \
    ../../file-mapping/mount/windows_explorer_mirror_provider.cpp

HEADERS += \
    ../../file-mapping/protocol/file_mapping_errors.h \
    ../../file-mapping/protocol/file_mapping_messages.h \
    ../../file-mapping/vfs/remote_vfs.h \
    ../../file-mapping/vfs/vfs_handle.h \
    ../../file-mapping/vfs/vfs_item.h \
    ../../file-mapping/mount/mount_errors.h \
    ../../file-mapping/mount/mount_provider.h \
    ../../file-mapping/mount/mount_session.h \
    ../../file-mapping/mount/mount_coordinator.h \
    ../../file-mapping/mount/macos_finder_mirror_provider.h \
    ../../file-mapping/mount/windows_explorer_mirror_provider.h
