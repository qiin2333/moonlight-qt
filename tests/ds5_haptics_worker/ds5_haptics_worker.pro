QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = ds5_haptics_worker_test
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
win32 {
    INCLUDEPATH += ../../libs/windows/include/x64/SDL2
    LIBS += -L$$PWD/../../libs/windows/lib/x64 -lSDL2
    QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$PWD/../../libs/windows/lib/x64/SDL2.dll) $$shell_path($$OUT_PWD/release/) $$escape_expand(\n\t)
    QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$PWD/../../libs/windows/lib/x64/SDL3.dll) $$shell_path($$OUT_PWD/release/)
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl2
}
macx {
    SOURCES += ../../app/streaming/audio/dualsensehapticsmac.mm
    LIBS += -framework Foundation -framework CoreHaptics -framework GameController
}
SOURCES += main.cpp
