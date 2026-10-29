QT -= core gui
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = cursor_mode_policy

INCLUDEPATH += ../../app

SOURCES += main.cpp

HEADERS += ../../app/streaming/input/cursormodepolicy.h
