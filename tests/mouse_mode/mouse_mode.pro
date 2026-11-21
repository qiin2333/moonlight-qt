QT += core qml
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = mouse_mode
INCLUDEPATH += ../../app
SOURCES += main.cpp ../../app/settings/mousemode.cpp
