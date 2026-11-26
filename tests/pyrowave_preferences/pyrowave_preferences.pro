QT += core qml
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle

TEMPLATE = app
TARGET = pyrowave_preferences
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src

pyrowave-test-enabled: DEFINES += HAVE_PYROWAVE

SOURCES += main.cpp ../../app/settings/streamingpreferences.cpp ../../app/settings/mousemode.cpp
HEADERS += ../../app/settings/streamingpreferences.h
