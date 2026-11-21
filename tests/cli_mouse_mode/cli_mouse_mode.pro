QT += core qml
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = cli_mouse_mode

INCLUDEPATH += $$PWD/../../app
SOURCES += main.cpp \
    $$PWD/../../app/cli/commandlineparser.cpp \
    $$PWD/../../app/settings/streamingpreferences.cpp \
    $$PWD/../../app/settings/mousemode.cpp
HEADERS += $$PWD/../../app/cli/commandlineparser.h \
    $$PWD/../../app/settings/streamingpreferences.h
