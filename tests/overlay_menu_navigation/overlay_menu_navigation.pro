QT += core gui testlib
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += ../../app
SOURCES += main.cpp ../../app/streaming/video/overlaymenupanel.cpp
HEADERS += ../../app/streaming/video/overlaymenupanel.h
RESOURCES += fonts.qrc
