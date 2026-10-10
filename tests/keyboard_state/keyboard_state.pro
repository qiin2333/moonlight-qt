QT += core
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = keyboard_state
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += main.cpp
HEADERS += ../../app/streaming/input/keyboardstate.h
