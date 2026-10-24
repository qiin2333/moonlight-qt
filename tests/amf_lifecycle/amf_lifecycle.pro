QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = amf_lifecycle

INCLUDEPATH += ../../app ../../third-party/AMF/amf
SOURCES += main.cpp ../../app/streaming/video/ffmpeg-renderers/amfsession.cpp
HEADERS += ../../app/streaming/video/ffmpeg-renderers/amfsession.h
win32 {
    SOURCES += hardware.cpp
    LIBS += -ld3d11 -ldxgi -lole32

    CONFIG(release, debug|release): FIXTURE_DIR = $$OUT_PWD/release
    else: FIXTURE_DIR = $$OUT_PWD/debug
    FIXTURE_DLL = $$FIXTURE_DIR/amf_runtime_fixture.dll
    fixture.target = $$FIXTURE_DLL
    fixture.depends = $$PWD/runtime_fixture.c
    fixture.commands = $$QMAKE_CXX /nologo /LD /std:c11 $(INCPATH) \
        /Fo$$shell_path($$FIXTURE_DIR/amf_runtime_fixture.obj) \
        $$shell_path($$PWD/runtime_fixture.c) /link /OUT:$$shell_path($$FIXTURE_DLL) \
        /IMPLIB:$$shell_path($$FIXTURE_DIR/amf_runtime_fixture.lib)
    QMAKE_EXTRA_TARGETS += fixture
    PRE_TARGETDEPS += $$FIXTURE_DLL
    QMAKE_CLEAN += $$FIXTURE_DLL $$FIXTURE_DIR/amf_runtime_fixture.obj \
                   $$FIXTURE_DIR/amf_runtime_fixture.lib $$FIXTURE_DIR/amf_runtime_fixture.exp
}
