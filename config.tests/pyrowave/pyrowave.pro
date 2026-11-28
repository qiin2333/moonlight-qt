QT -= core gui
CONFIG += console c++17
CONFIG -= app_bundle
include(dependency.pri)
macx:!disable-prebuilts: INCLUDEPATH += $$PWD/../../libs/mac/include
SOURCES = main.cpp
