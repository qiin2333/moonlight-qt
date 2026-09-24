QT += core gui network
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = clipboard_helper_lifecycle
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += main.cpp ../../app/streaming/clipboardhelperclient.cpp ../../app/streaming/clipboardipc.cpp
HEADERS += ../../app/streaming/clipboardhelperclient.h ../../app/streaming/clipboardipc.h
win32 {
    INCLUDEPATH += ../../libs/windows/include/x64/SDL2
    LIBS += $$PWD/../../libs/windows/lib/x64/SDL2.lib
    QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$PWD/../../libs/windows/lib/x64/SDL2.dll) $$shell_path($$OUT_PWD/release/SDL2.dll)
    # SDL2 is the compatibility library and dynamically loads SDL3 at startup.
    QMAKE_POST_LINK += $$escape_expand(\\n\\t) $$QMAKE_COPY $$shell_path($$PWD/../../libs/windows/lib/x64/SDL3.dll) $$shell_path($$OUT_PWD/release/SDL3.dll)
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl2
}
SOURCES += ../../app/backend/nvaddress.cpp
