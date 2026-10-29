QT -= core gui
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = host_cursor_math

INCLUDEPATH += ../../app

SOURCES += main.cpp

HEADERS += ../../app/streaming/input/hostcursormath.h
