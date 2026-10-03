QT += core gui svg testlib
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += ../../app
SOURCES += main.cpp ../../app/streaming/video/overlaymenupanel.cpp ../../app/backend/transportpolicy.cpp
HEADERS += ../../app/streaming/video/overlaymenupanel.h
RESOURCES += fonts.qrc
